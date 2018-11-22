#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include "dbg_err.h"
#include "utlist.h"

/*------------------------------------------------------------------
 * client receive should be blocking, but have a timeout so we can
*  detect terminate flag 
*  client send should be blocking
* 
*  server receive should be blocking
*  server send should be non-bocking so we can do send all
---------------------------------------------------------------------*/

#define SOCK_SEND_TIMEOUT_SEC 2
#define MSG_HEADER_MARK 0xEE


struct options {
  bool set_timeout;
  bool wait_send_ready;
  bool send_random;
  bool print_send_msgs;
  unsigned int msg_filler;
} OPT;

size_t msg_buf_size = 128;

typedef struct connection {
  int sock;
  int rcv_state;
  unsigned rcv_count;
  bool rcv_selected;
  size_t rcv_msg_size;
  size_t rcv_end_pos;
  char *rcv_msg;
  struct connection * next;
} connection_t;

struct client_stuff {
  const char *port_str;
  struct sockaddr_in addr;
  int sock;
  bool terminated;
  unsigned int send_count;
  unsigned int rcv_count;
  const char *msg;
  char *rcv_msg;
  size_t rcv_msg_size;
} CLI;

struct server_stuff {
  const char *port_str;
  struct sockaddr_in addr;
  int listen_sock;
  bool terminated;
  pthread_mutex_t list_mutex;
  struct connection * connection_list;
} SRV;


typedef void (* process_message_t) (struct connection *conn);

void init_options (void)
{
  OPT.set_timeout = true;
  OPT.wait_send_ready = true;
  OPT.send_random = false;
  OPT.print_send_msgs = false;
  OPT.msg_filler = 0;
}

void init_connection (struct connection *conn)
{
  conn->sock = -1;
  conn->rcv_state = -1;
  conn->rcv_count = 0;
  conn->rcv_selected = false;
  conn->rcv_msg_size = 0;
  conn->rcv_end_pos = 0;
  conn->rcv_msg = NULL;
  conn->next = NULL;
}

void init_client_stuff (void)
{
  CLI.port_str = NULL;
  CLI.sock = -1;
  CLI.terminated = false;
  CLI.send_count = 0;
  CLI.rcv_count = 0;
  CLI.msg = NULL;
  CLI.rcv_msg = NULL;
  CLI.rcv_msg_size = 0;
}

void init_server_stuff (void)
{
  SRV.port_str = NULL;
  SRV.listen_sock = -1;
  SRV.terminated = false;
  pthread_mutex_init (&SRV.list_mutex, NULL);
  SRV.connection_list = NULL;
}

// waits from 0 to 0x1FFFF (131071) usecs
void wait_random (void)
{
  struct timeval timeout;
  timeout.tv_sec = 0;
  timeout.tv_usec = random() >> 15;
  select (0, NULL, NULL, NULL, &timeout);
}

void wait_msecs (unsigned msecs)
{
  struct timeval timeout;
  unsigned msecs_lo = msecs % 1000;
  timeout.tv_sec = msecs / 1000;
  timeout.tv_usec = 1000 * msecs_lo;
  select (0, NULL, NULL, NULL, &timeout);
}

int wait_server_ready (void)
{
  struct timeval timeout;
  struct connection *conn;
  int i, rtn, sock, highest_sock;
  int fd = SRV.listen_sock;
  fd_set fds;

  highest_sock = -1;

  while (1)
  {
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    FD_ZERO (&fds);
    if (SRV.listen_sock != -1) {
      FD_SET (SRV.listen_sock, &fds);
      highest_sock = SRV.listen_sock;
      // printf ("Waiting on listener %d\n", listen_sock);
    }
    LL_FOREACH (SRV.connection_list, conn) {
      conn->rcv_selected = false;
      if (conn->rcv_state >= 0) {
        sock = conn->sock;
        // printf ("Waiting on %d\n", sock);
        if (sock > highest_sock)
          highest_sock = sock;
        FD_SET (sock, &fds);
      }
    }
    FD_SET (STDIN_FILENO, &fds);
    rtn = select (highest_sock+1, &fds, NULL, NULL, &timeout);
    if (rtn < 0) {
      printf ("Error on select for receive\n");
      return -1;
    }
    if (rtn != 0)
      break;
    printf ("Waiting for receive. Press <Enter> to terminate.\n");
  }
  rtn = 0;
  if (SRV.listen_sock != -1)
    if (FD_ISSET (SRV.listen_sock, &fds))
      rtn = 1;
  LL_FOREACH (SRV.connection_list, conn) {
    if (conn->rcv_state >= 0)
      if (FD_ISSET (conn->sock, &fds)) {
        conn->rcv_selected = true;
        rtn |= 2;
      }
  }
  if (FD_ISSET (STDIN_FILENO, &fds))
    rtn |= 4;
  return rtn;
}

unsigned int parse_num_arg (const char *arg, const char *arg_name)
{
	unsigned int result = 0;
	int i;
	char c;
	
	if (arg[0] == '\0') {
		printf ("Empty %s argument\n", arg_name);
		return (unsigned int) -1;
	}
	for (i=0; '\0' != (c=arg[i]); i++)
	{
		if ((c<'0') || (c>'9')) {
			printf ("Non-numeric %s argument\n", arg_name);
			return (unsigned int) -1;
		}
		result = (result*10) + c - '0';
	}
	return result;
}

int make_sockaddr (struct sockaddr_in *addr, const char *id, bool rcv_any)
{
  int rtn;
  unsigned int port;
  port = parse_num_arg (id, "port");
  if (port == (unsigned) -1)
    return -1;
  
  addr->sin_family = AF_INET;
  addr->sin_port = htons (port);
  if (rcv_any)
    addr->sin_addr.s_addr = INADDR_ANY;
  else {
    rtn = inet_pton (AF_INET, "127.0.0.1", &addr->sin_addr);
    if (rtn != 1) {
      printf ("inet_pton error\n");
      return -1;
    }
  }
  return 0;
}

int connect_server (void)
{
	int sock, flags;
	int reuse_opt = 1;

	if (NULL == SRV.port_str) {
		SRV.listen_sock = -1;
		return -1;
	}

	if (make_sockaddr (&SRV.addr, SRV.port_str, false) != 0)
          return -1;
	sock = socket (AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		dbg_err (errno, "Unable to create rcv socket\n");
 		return -1;
	}
#if 0
	flags = fcntl (sock, F_GETFL);
	if (flags == -1) {
		dbg_err (errno, "Unable to get socket flags: \n");
		close (sock);
 		return -1;
	}
	flags |= O_NONBLOCK;
	if (fcntl (sock, F_SETFL, flags) == -1) {
		dbg_err (errno, "Unable to set socket flags: \n");
		close (sock);
 		return -1;
	}
#endif
	if (bind (sock, (struct sockaddr *) &SRV.addr, 
          sizeof (struct sockaddr_in)) < 0) {
		dbg_err (errno, "Unable to bind to receive socket %s\n");
		close (sock);
		return -1;
	}
	if (listen (sock, 50) == -1) {
	  dbg_err (errno, "Listen error on receive socket: %s\n");
	  close (sock);
	  return -1;
	}
	SRV.listen_sock = sock;
	return 0;
}

int server_accept (void)
{
  int i, sock, flags;
  struct connection *conn;

  sock = accept (SRV.listen_sock, NULL, NULL);
  if (sock < 0) {
    if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
      return 1;
    dbg_err (errno, "Accept error on receive socket: %s\n");
    close (SRV.listen_sock);
    return -1;
  }
  printf ("Accepted %d\n", sock);
#if 0
  flags = fcntl (sock, F_GETFL);
  if (flags == -1) {
	dbg_err (errno, "Unable to get socket flags: \n");
	close (sock);
	return -1;
  }
  flags |= O_NONBLOCK;
  if (fcntl (sock, F_SETFL, flags) == -1) {
	dbg_err (errno, "Unable to set socket flags: \n");
	close (sock);
	return -1;
  }
#endif
  conn = (struct connection *) malloc (sizeof (struct connection));
  if (NULL == conn) {
    printf ("Unable to malloc connection structure in receiver accept\n");
    return -1;
  }
  init_connection (conn);
  conn->rcv_state = 0;
  conn->sock = sock;
  pthread_mutex_lock (&SRV.list_mutex);
  LL_APPEND (SRV.connection_list, conn);
  pthread_mutex_unlock (&SRV.list_mutex);
  return 0;

}

void shutdown_sock (int sock)
{
    shutdown (sock, SHUT_RDWR);
    close (sock);
}

void shutdown_connection (struct connection *conn)
{
  if (conn->rcv_state != -1) {
    shutdown (conn->sock, SHUT_RDWR);
    close (conn->sock);
    conn->sock = -1;
    conn->rcv_state = -1;
  }
}
 
void shutdown_server (void)
{
  int i;
  struct connection *conn;
  struct connection *tmp;

  if (SRV.listen_sock != -1) {
    LL_FOREACH_SAFE (SRV.connection_list, conn, tmp) {
      LL_DELETE (SRV.connection_list, conn);
      shutdown_connection (conn);
      free (conn);
    }
    shutdown_sock (SRV.listen_sock);
  }
}

int connect_client (void)
{
	int sock;
	int reuse_opt = 1;
	struct timeval send_timeout;
	struct timeval rcv_timeout;

	if (NULL == CLI.port_str) {
		CLI.sock = -1;
		return -1;
	}
	if (make_sockaddr (&CLI.addr, CLI.port_str, false) != 0)
          return -1;
	sock = socket (AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		dbg_err (errno, "Unable to create send socket\n");
 		return -1;
	}
	send_timeout.tv_sec = SOCK_SEND_TIMEOUT_SEC;
	send_timeout.tv_usec = 0;
	if (OPT.set_timeout)
		if (setsockopt (sock, SOL_SOCKET, SO_SNDTIMEO, 
		  &send_timeout, sizeof (send_timeout)) < 0) {
			dbg_err (errno, "Unable to set socket send timeout: \n");
			close (sock);
	 		return -1;
		}
	rcv_timeout.tv_sec = 0;
	rcv_timeout.tv_usec = 500000;
	if (setsockopt (sock, SOL_SOCKET, SO_RCVTIMEO, 
		  &rcv_timeout, sizeof (rcv_timeout)) < 0) {
			dbg_err (errno, "Unable to set socket rcv timeout: \n");
			close (sock);
	 		return -1;
		}
	if (connect (sock, (struct sockaddr *) &CLI.addr, sizeof (CLI.addr)) < 0) {
		dbg_err (errno, "Unable to connect to client socket\n");
		shutdown (CLI.sock, SHUT_RDWR);
		close (CLI.sock);
		return -1;
	}
	CLI.sock = sock;
	return 0;
}

void shutdown_client (void)
{
	if (CLI.sock != -1) {
		shutdown_sock (CLI.sock);
	}
}


#if 0
int wait_send_ready (int sock)
{
  struct timeval timeout;
  int rtn;
  fd_set fds;

  if (!OPT.wait_send_ready)
    return 0;
  while (1)
  {
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    FD_ZERO (&fds);
    FD_SET (sock, &fds);
    rtn = select (sock+1, NULL, &fds, NULL, &timeout);
    if (rtn < 0) {
      printf ("Error on select for send\n");
      return -1;
    }
    if (rtn != 0)
      return 0;
    printf ("Waiting for send\n");
  }
}
#endif

ssize_t socket_receive (int sock, void *buf, size_t len, bool *terminated)
{
  ssize_t bytes;

  while (true) {
    bytes = recv (sock, buf, len, 0);
    if (bytes >= 0)
      return bytes;
    if (NULL != terminated) {
      if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
        if (*terminated)
          return -2;
        continue; 
      }
    }
    return -1;
  }
}

int receive_msg_header (struct connection *conn, bool *terminated)
{
  int sock = conn->sock;
  ssize_t bytes;
  size_t msg_size;
  unsigned char header[4];

  bytes = socket_receive (sock, header, 4, terminated);
  if (bytes < 0) { 
    if (bytes == -1)
      dbg_err (errno, "Error receiving msg header\n");
    return bytes;
  }
  if (bytes == 0) {
	printf ("Sender %d closed\n", sock);
	return -1;
  }
  if (bytes != 4) {
	printf ("Expecting 4 byte msg header. Got %d bytes\n", bytes);
	return -1;
  }
  if ((header[0] != MSG_HEADER_MARK) || (header[1] != MSG_HEADER_MARK)) {
	printf ("Invalid msg header mark\n");
	return -1;
  }
  msg_size = ((size_t) header[2] << 8) + (size_t) header[3]; 
  conn->rcv_msg = malloc (msg_size);
  if (NULL == conn->rcv_msg) {
    printf ("Unable to malloc msg buffer for socket %d\n", sock);
    return -1;
  }
  conn->rcv_msg_size = msg_size;
  conn->rcv_end_pos = 0;
  conn->rcv_state = 1;
  return 0;
}


// returned msg must be freed
int receive_msg_data (struct connection *conn, process_message_t handle_msg,
  bool *terminated)
{
  ssize_t bytes;
  size_t read_len = conn->rcv_msg_size - conn->rcv_end_pos;
  int sock = conn->sock;
  char *buf = conn->rcv_msg;

  bytes = socket_receive (sock, buf+conn->rcv_end_pos, read_len, terminated);

  if (bytes < 0) { 
    if (bytes == -1)
      dbg_err (errno, "Error receiving msg header\n");
    return bytes;
  }

  if (bytes > read_len) {
    printf ("bytes received %d not eq read_len in header %u\n",
	bytes, read_len);
    return -1;
  }
  conn->rcv_end_pos += bytes;
  if (bytes < read_len) {
    printf ("Not all bytes received, only %d of %d. Waiting for remainder\n",
      bytes, read_len);
    return 0;
  }
  if (NULL != handle_msg)
    handle_msg (conn);
  conn->rcv_count += 1;
  conn->rcv_state = 0;
  return 1;
}

ssize_t client_receive (int sock, char **msg, bool *terminated)
{
  int rtn;
  struct connection conn;

  init_connection (&conn);
  conn.sock = sock;
  conn.rcv_state = 0;

  rtn = receive_msg_header (&conn, terminated);
  if (rtn < 0)
    return rtn;
  while (true) {
    rtn = receive_msg_data (&conn, NULL, terminated);
    if (rtn == 1) {
      *msg = conn.rcv_msg;
      return (ssize_t) conn.rcv_msg_size;
    }
    if (rtn < 0)
      break;
  }
  return rtn;
}

int server_receive_msgs (process_message_t handle_msg)
{
  int i, rtn;
  int error_cnt = 0;
  struct connection *conn;
  struct connection *tmp;
  
  LL_FOREACH (SRV.connection_list, conn)
    if (conn->rcv_selected) {
      if (conn->rcv_state == 0)
        rtn = receive_msg_header (conn, NULL);
      else if (conn->rcv_state == 1)
        rtn = receive_msg_data (conn, handle_msg, NULL);
      else
        continue;
      if (rtn < 0)
        conn->rcv_state = -2;
    }

  pthread_mutex_lock (&SRV.list_mutex);
  LL_FOREACH_SAFE (SRV.connection_list, conn, tmp)
    if (conn->rcv_state == -2) {
        LL_DELETE (SRV.connection_list, conn);
        printf ("Closing connection for socket %d\n", conn->sock);
        shutdown_connection (conn);
        free (conn);
        error_cnt++;
    }
  pthread_mutex_unlock (&SRV.list_mutex);

   if (error_cnt == 0)
     return 0;
   if (NULL != SRV.connection_list)
     return 0;

   return -1;
}


void wait_for_msgs (process_message_t handle_msg)
{
  int rtn;
  char inbuf[10];

  while (1)
  {
	  rtn = wait_server_ready ();
	  if (rtn < 0)
	    break;
	  if (rtn & 1)
	    if (server_accept () < 0)
	      break;
	  if (rtn & 2) {
	    server_receive_msgs (handle_msg);
          }
	  if (rtn & 4) { // key pressed
	    fgets (inbuf, 10, stdin);
	    break;
	  }
  }
  SRV.terminated = true;
  printf ("Exiting wait_for_msgs\n");
}

int send_msg (int sock, const char *msg, size_t sz_msg, bool non_block)
{
  int flags = 0;
  ssize_t bytes;
  int i;
  char *msg_buf;

  msg_buf = malloc (sz_msg+4);
  if (NULL == msg_buf) {
    printf ("Unable to malloc msg buffer for socket %d\n", sock);
    return -1;
  }
  msg_buf[0] = MSG_HEADER_MARK;
  msg_buf[1] = MSG_HEADER_MARK;
  msg_buf[2] = sz_msg / 256;
  msg_buf[3] = sz_msg % 256;
  strcpy (msg_buf+4, msg);

#if 0
  if (wait_send_ready () < 0)
     return -1;
#endif
  sz_msg += 4;
  if (non_block)
    flags = MSG_DONTWAIT;
  bytes = send (sock, msg_buf, sz_msg, flags);
  free (msg_buf);
  if (bytes < 0) { 
	dbg_err (errno, "Error sending msg\n");
	return -1;
  }
  if (bytes != sz_msg) {
	printf ("Not all bytes sent, just %d\n", bytes);
	return -1;
  }
  return 0;
}

/*-------------------------------------------------------------------
 * Test Code
*-------------------------------------------------------------------*/

pthread_t client_rcv_thread_id;
pthread_t server_send_thread_id;
bool server_received_something = false;

typedef struct {
  int allocated, used;
  int *sockets;
} socket_list_t;

void init_socket_list (socket_list_t *slist)
{
  slist->allocated = 0;
  slist->used = 0;
  slist->sockets = NULL;
}

void free_socket_list (socket_list_t *slist)
{
  if (NULL != slist->sockets)
    free (slist->sockets);
  init_socket_list (slist);
}

int append_to_socket_list (socket_list_t *slist, int socket)
{
  if (slist->allocated == 0) {
    slist->sockets = (int *) malloc (20 * sizeof (int));
    if (NULL == slist->sockets) {
      printf ("Unable to allocate memory for sockets list\n");
      return -1;
    }
    slist->allocated = 20;
  }
  if (slist->used >= slist->allocated) {
    int new_alloc = slist->allocated + 20;
    int *new_sockets = (int *) realloc (slist->sockets, new_alloc * sizeof(int));
    if (NULL == new_sockets) {
      printf ("Unable to allocate memory to expand sockets list\n");
      return -1;
    }
    slist->sockets = new_sockets;
    slist->allocated = new_alloc;
  }
  slist->sockets[slist->used++] = socket;
  return 0;
}

int find_socket_in_list (socket_list_t *slist, int socket)
{
  int i;
  for (i=0; i<slist->used; i++)
    if (socket == slist->sockets[i])
      return i;
  return -1;
}

static int create_thread (pthread_t *tid, void *(*thread_func) (void*))
{
	int rtn = pthread_create (tid, NULL, thread_func, NULL);
	if (rtn != 0) {
	  printf ("Error creating thread\n");
	}
	return rtn; 
}

static void *client_receiver_thread (void *arg)
{
  ssize_t rtn;
  //printf ("Started client receiver thread for %d\n", getpid());
  while (true) {
    rtn = client_receive (CLI.sock, &CLI.rcv_msg, &CLI.terminated);
    if (rtn < 0)
      break;
    CLI.rcv_count++;
    printf ("Client %d received: %s\n", getpid(), CLI.rcv_msg);
    free (CLI.rcv_msg);
    CLI.rcv_msg = NULL;
  }
  //printf ("Ending client receiver thread for %d\n", getpid());
}

void server_send_pass (socket_list_t *done_list, socket_list_t *not_done_list,
  const char *msg)
{
  size_t sz_msg = strlen(msg) + 1;
  bool found = false;
  struct connection *conn;

  while (true) {
    found = false;
    pthread_mutex_lock (&SRV.list_mutex);
    LL_FOREACH (SRV.connection_list, conn) {
      if (find_socket_in_list (done_list, conn->sock) >= 0)
        continue;
      if (find_socket_in_list (not_done_list, conn->sock) >= 0)
        continue;
      found = true;
      if (conn->rcv_state == -2) { 
        append_to_socket_list (done_list, conn->sock);
        break;
      }
      if (conn->rcv_count == 0) {
        append_to_socket_list (not_done_list, conn->sock);
        break;
      }
      if (send_msg (conn->sock, msg, sz_msg, true) == 0)
        append_to_socket_list (done_list, conn->sock);
      else
        append_to_socket_list (not_done_list, conn->sock);
      break;
    }
    pthread_mutex_unlock (&SRV.list_mutex);
    if (!found)
      break;
  }
}

int server_send_to_all_clients (const char *msg, unsigned timeout_ms)
{
  int rtn;
  socket_list_t done_list;
  socket_list_t not_done_list;
  struct connection *conn;
  unsigned delay = 0, total_delay = 0;

  init_socket_list (&done_list);
  init_socket_list (&not_done_list);
  
  while (!server_received_something && !SRV.terminated)
    wait_msecs (250);

  server_send_pass (&done_list, &not_done_list, msg);

  while (!SRV.terminated) {
    if (delay == 0)
      delay = 10;
    else if (delay == 10)
      delay = 20;
    else if (delay == 20)
      delay = 50;
    else if (delay == 50)
      delay = 100;
    else if (delay == 100)
      delay = 200;
    else if (delay == 200)
      delay = 500;
    else if (delay == 500)
      delay = 1000;
    if (timeout_ms != 0)
      if ((total_delay+delay) > timeout_ms)
        delay = timeout_ms - total_delay;
    wait_msecs (delay);
    init_socket_list (&not_done_list);
    server_send_pass (&done_list, &not_done_list, msg);
    if (timeout_ms != 0) {
      total_delay += delay;
      if (total_delay >= timeout_ms) {
        init_socket_list (&done_list);
        delay = 0;
        total_delay = 0;
      }
    }
  } // end while
  rtn = not_done_list.used;
  free_socket_list (&done_list);
  free_socket_list (&not_done_list);
  return rtn;
}

static void *server_send_thread (void *arg)
{
  int i, rtn;

  printf ("Starting server send thread\n");
  rtn = server_send_to_all_clients ("Hello from the server!", 1000);
  if (0 != rtn)
    printf ("Messages not sent to %d clients\n", rtn);
  printf ("Ending server send thread\n");
  return NULL;
}

void make_filled_msg (const char *msg, unsigned msg_num, char *filled_msg)
{
  int i;
  for (i=0; i<OPT.msg_filler; i++)
     filled_msg[i] = '.';
  sprintf (filled_msg+OPT.msg_filler, "%s %d", msg, msg_num);
}

void client_send_multiple (void)
{
  unsigned long i;
  size_t sz_msg;
  char buf[msg_buf_size+OPT.msg_filler];

  if (CLI.send_count == 0)
    CLI.send_count = 1;

  printf ("Sending %lu messages from pid %d\n", CLI.send_count, getpid());

  for (i=0; i<CLI.send_count; i++) {
    //if (i==100) // allow other senders to catch up
    //  wait_msecs (2000);
	  if ((i>0) && (CLI.rcv_count == 0))
	    wait_msecs (250);
	  else if (OPT.send_random)
	    wait_random ();
	  make_filled_msg (CLI.msg, i, buf);
	  sz_msg = strlen(buf) + 1;
	  if (send_msg(CLI.sock, buf, sz_msg, false) != 0)
		break;
	  if (OPT.print_send_msgs)
	    printf ("Sent msg %lu\n", i);
  }
}


void show_msg (struct connection *conn)
{
  int i;
  unsigned count = conn->rcv_count;
  char *buf = conn->rcv_msg;
  size_t bytes = conn->rcv_msg_size;

  if ((count & 0xFF) == 0) {
    printf ("RECEIVED \"%s\"\n", buf);
    return;
  }

  for (i=0; i<bytes; i++) {
    if (buf[i] == '.')
      continue;
    printf ("RECEIVED \"%s\"\n", buf+i);
    return;
  }

}

void process_rcv_msg (struct connection *conn)
{
  show_msg (conn);
  free (conn->rcv_msg);
  conn->rcv_msg = NULL;
  server_received_something = true;
}

int get_args (const int argc, const char **argv)
{
	int i;
	int mode = 0;

	for (i=1; i<argc; i++)
	{
		const char *arg = argv[i];
		if ((strlen(arg) == 1) && (arg[0] == 'r')) {
			mode = 'r';
			continue;
		}
		if ((strlen(arg) == 1) && (arg[0] == 's')) {
			mode = 's';
			continue;
		}
		if ((strlen(arg) == 1) && (arg[0] == 'm')) {
			mode = 'm';
			continue;
		}
		if ((strlen(arg) == 1) && (arg[0] == 'n')) {
			mode = 'n';
			continue;
		}
		if ((strlen(arg) == 1) && (arg[0] == 'f')) {
			mode = 'f';
			continue;
		}
		if ((mode == 0) && (strcmp(arg, "not") == 0)) {
			OPT.set_timeout = false;
			continue;
		}
		if ((mode == 0) && (strcmp(arg, "pr") == 0)) {
			OPT.print_send_msgs = true;
			continue;
		}
		if ((mode == 0) && (strcmp(arg, "rnd") == 0)) {
			OPT.send_random = true;
			continue;
		}
		if (mode == 'r') {
			SRV.port_str = arg;
			mode = 0;
			continue;
		}
		if (mode == 's') {
			CLI.port_str = arg;
			mode = 0;
			continue;
		}
		if (mode == 'm') {
			CLI.msg = arg;
			mode = 0;
			continue;
		}
		if (mode == 'n') {
			CLI.send_count = parse_num_arg (arg, "send_count");
			if (CLI.send_count == (unsigned) -1)
			  return -1;
			mode = 0;
			continue;
		}
		if (mode == 'f') {
			OPT.msg_filler = parse_num_arg (arg, "long_msg_filler");
			if (OPT.msg_filler == (unsigned) -1)
			  return -1;
			mode = 0;
			continue;
		}
		printf ("arg not preceded by r/s/m/n/f specifier\n");
		return -1;
	} 
	return 0;
}

int main (const int argc, const char **argv)
{
	srandom (getpid());

	init_options ();
	init_client_stuff ();
	init_server_stuff ();

	if (get_args(argc, argv) != 0)
		exit (4);

	if ((NULL != CLI.msg) && (NULL == CLI.port_str)) {
		printf ("msg specified witlhout a sender ID\n");
		exit(4);
	}

	if ((NULL == SRV.port_str) && (NULL == CLI.port_str)) {
		printf ("Nothing to do\n");
		exit(0);
	}

	if ((NULL != SRV.port_str) && (NULL != CLI.port_str)) {
		printf ("Cannot be both client and server\n");
		exit(0);
	}

	if (NULL != SRV.port_str) {
	  if (connect_server () < 0)
		exit(4);
	  if (create_thread (&server_send_thread_id, server_send_thread) == 0)
	  {
	    wait_for_msgs (process_rcv_msg);
	    pthread_join (server_send_thread_id, NULL);
	  }
	  shutdown_server ();
	  pthread_mutex_destroy (&SRV.list_mutex);
	}

	if (NULL != CLI.port_str) {
	  if (NULL == CLI.msg) {
		printf ("Message not specified for client\n");
		exit(4);
	  }
	  if (connect_client () < 0)
		exit(4);
	  if (create_thread (&client_rcv_thread_id, client_receiver_thread) == 0)
	  {
 	    client_send_multiple ();
            CLI.terminated = true;
            pthread_join (client_rcv_thread_id, NULL);
	  }
          shutdown_client ();
	}


  printf ("%d Done!\n", getpid());
}
