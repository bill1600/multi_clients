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
#include "dbg_err.h"
#include "utlist.h"

#define SOCK_SEND_TIMEOUT_SEC 2
#define MSG_HEADER_MARK 0xEE

const char *rcv_id = NULL;
const char *send_id = NULL;
bool wait_random_opt = false;
bool print_send_msg_opt = false;
struct options {
  bool set_timeout;
  bool wait_send_ready;
  unsigned int msg_filler;
} OPT;
size_t msg_buf_size = 128;
int listen_sock = -1;
int client_sock = -1;
struct client_stuff {
  struct sockaddr_in addr;
  unsigned int send_count;
  const char *msg;
} CLI;
struct server_stuff {
  struct sockaddr_in addr;
} SRV;

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

struct connection * connection_list;

typedef void (* process_message_t) (struct connection *conn);

void init_options (void)
{
  OPT.set_timeout = true;
  OPT.wait_send_ready = true;
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
  CLI.send_count = 0;
  CLI.msg = NULL;
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
  timeout.tv_sec = 0;
  timeout.tv_usec = 1000 * msecs;
  select (0, NULL, NULL, NULL, &timeout);
}

int wait_server_ready (void)
{
  struct timeval timeout;
  struct connection *conn;
  int i, rtn, sock, highest_sock;
  int fd = listen_sock;
  fd_set fds;

  highest_sock = -1;

  while (1)
  {
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    FD_ZERO (&fds);
    if (listen_sock != -1) {
      FD_SET (listen_sock, &fds);
      highest_sock = listen_sock;
      // printf ("Waiting on listener %d\n", listen_sock);
    }
    LL_FOREACH (connection_list, conn) {
      conn->rcv_selected = false;
      if (conn->rcv_state >= 0) {
        sock = conn->sock;
        // printf ("Waiting on %d\n", sock);
        if (sock > highest_sock)
          highest_sock = sock;
        FD_SET (sock, &fds);
      }
    }
    rtn = select (highest_sock+1, &fds, NULL, NULL, &timeout);
    if (rtn < 0) {
      printf ("Error on select for receive\n");
      return -1;
    }
    if (rtn != 0)
      break;
    printf ("Waiting for receive\n");
  }
  rtn = 0;
  if (listen_sock != -1)
    if (FD_ISSET (listen_sock, &fds))
      rtn = 1;
  LL_FOREACH (connection_list, conn) {
    if (conn->rcv_state >= 0)
      if (FD_ISSET (conn->sock, &fds)) {
        conn->rcv_selected = true;
        rtn |= 2;
      }
  }
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

	if (NULL == rcv_id) {
		listen_sock = -1;
		return -1;
	}

	if (make_sockaddr (&SRV.addr, rcv_id, false) != 0)
          return -1;
	sock = socket (AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		dbg_err (errno, "Unable to create rcv socket\n");
 		return -1;
	}
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
	listen_sock = sock;
	return 0;
}

int server_accept (void)
{
  int i, sock, flags;
  struct connection *conn;

  sock = accept (listen_sock, NULL, NULL);
  if (sock < 0) {
    if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
      return 1;
    dbg_err (errno, "Accept error on receive socket: %s\n");
    close (listen_sock);
    return -1;
  }
  printf ("Accepted %d\n", sock);
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

  conn = (struct connection *) malloc (sizeof (struct connection));
  if (NULL == conn) {
    printf ("Unable to malloc connection structure in receiver accept\n");
    return -1;
  }
  init_connection (conn);
  conn->rcv_state = 0;
  conn->sock = sock;
  LL_APPEND (connection_list, conn);
  return 0;

}

void shutdown_sock (int sock)
{
    shutdown (sock, SHUT_RDWR);
    close (sock);
}

void shutdown_connection (struct connection *conn)
{
  if (conn->rcv_state >= 0) {
    shutdown (conn->sock, SHUT_RDWR);
    close (conn->sock);
    conn->sock = -1;
    conn->rcv_state = -1;
  }
}
 
void shutdown_receiver (void)
{
  int i;
  struct connection *conn;
  struct connection *tmp;

  if (listen_sock != -1) {
    LL_FOREACH_SAFE (connection_list, conn, tmp) {
      LL_DELETE (connection_list, conn);
      shutdown_connection (conn);
      free (conn);
    }
    shutdown_sock (listen_sock);
  }
}

int connect_sender (void)
{
	int sock;
	int reuse_opt = 1;
	struct timeval send_timeout;

	if (NULL == send_id) {
		client_sock = -1;
		return -1;
	}
	if (make_sockaddr (&CLI.addr, send_id, false) != 0)
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
			dbg_err (errno, "Unable to set socket timeout: \n");
			close (sock);
	 		return -1;
		}
	if (connect (sock, (struct sockaddr *) &CLI.addr, sizeof (CLI.addr)) < 0) {
		dbg_err (errno, "Unable to connect to send socket\n");
		shutdown (client_sock, SHUT_RDWR);
		close (client_sock);
		return -1;
	}
	client_sock = sock;
	return 0;
}

void shutdown_sender (void)
{
	if (client_sock != -1) {
		shutdown_sock (client_sock);
	}
}



int wait_send_ready (void)
{
  struct timeval timeout;
  int rtn;
  int fd = client_sock;
  fd_set fds;

  if (!OPT.wait_send_ready)
    return 0;
  while (1)
  {
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    FD_ZERO (&fds);
    FD_SET (fd, &fds);
    rtn = select (fd+1, NULL, &fds, NULL, &timeout);
    if (rtn < 0) {
      printf ("Error on select for send\n");
      return -1;
    }
    if (rtn != 0)
      return 0;
    printf ("Waiting for send\n");
  }
}

int receive_msg_header (struct connection *conn)
{
  int sock = conn->sock;
  ssize_t bytes;
  size_t msg_size;
  unsigned char header[4];

  bytes = recv (sock, header, 4, 0);
  if (bytes < 0) { 
    dbg_err (errno, "Error receiving msg header\n");
    return -1;
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


// returned msg must be freed with nn_freemsg
int receive_msg_data (struct connection *conn, process_message_t handle_msg)
{
  ssize_t bytes;
  size_t read_len = conn->rcv_msg_size - conn->rcv_end_pos;
  int sock = conn->sock;
  char *buf = conn->rcv_msg;

  bytes = recv (sock, buf+conn->rcv_end_pos, read_len, 0);

  if (bytes < 0) { 
	dbg_err (errno, "Error receiving msg\n");
	return -1;
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
  handle_msg (conn);
  conn->rcv_count += 1;
  conn->rcv_state = 0;
  return 0;
}

int receive_msgs (process_message_t handle_msg)
{
  int i, rtn;
  int error_cnt = 0;
  struct connection *conn;
  struct connection *tmp;
  
  LL_FOREACH_SAFE (connection_list, conn, tmp)
    if (conn->rcv_selected) {
      if (conn->rcv_state == 0)
        rtn = receive_msg_header (conn);
      else if (conn->rcv_state == 1)
        rtn = receive_msg_data (conn, handle_msg);
      else
        continue;
      if (rtn < 0) {
        LL_DELETE (connection_list, conn);
        printf ("Closing connection for socket %d\n", conn->sock);
        shutdown_connection (conn);
        free (conn);
        error_cnt++;
      }
    }

   if (error_cnt == 0)
     return 0;
   if (NULL != connection_list)
     return 0;

   return -1;
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
}

void wait_for_msgs (void)
{
	int rtn;

	while (1)
	{
	  rtn = wait_server_ready ();
	  if (rtn < 0)
	    break;
	  if (rtn & 1)
	    if (server_accept () < 0)
	      break;
	  if (rtn & 2) {
	    rtn = receive_msgs (process_rcv_msg);
	    if (rtn != 0)
	      break; 
          }
	}
	shutdown_sender ();
	shutdown_receiver ();
}

int send_msg (const char *msg)
{
  int sz_msg = strlen (msg) + 1; // '\0' too
  ssize_t bytes;
  int i;
  char msg_buf[msg_buf_size];
  char *send_buf = (char *) msg;

  sz_msg += OPT.msg_filler;
  if ((sz_msg + 4) > msg_buf_size) {
    printf ("msg too long. exceeds %u\n", msg_buf_size);
    return -1;
  }
  msg_buf[0] = MSG_HEADER_MARK;
  msg_buf[1] = MSG_HEADER_MARK;
  msg_buf[2] = sz_msg / 256;
  msg_buf[3] = sz_msg % 256;
  for (i=0; i<OPT.msg_filler; i++)
     msg_buf[4+i] = '.';
  strcpy (msg_buf+4+OPT.msg_filler, msg);
  sz_msg += 4;
  send_buf = msg_buf;

  if (wait_send_ready () < 0)
     return -1;
  bytes = send (client_sock, send_buf, sz_msg, 0);
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


void send_multiple (void)
{
  unsigned long i;
  char buf[128];

  if (CLI.send_count == 0)
    CLI.send_count = 1;

  printf ("Sending %lu messages from pid %d\n", CLI.send_count, getpid());

	for (i=0; i<CLI.send_count; i++) {
	  if (i==100) // allow other senders to catch up
            wait_msecs (2000);
	  else if (wait_random_opt)
	    wait_random ();
	  sprintf (buf, "%s %d", CLI.msg, i);
	  if (send_msg(buf) != 0)
		break;
	  if (print_send_msg_opt)
	    printf ("Sent msg %lu\n", i);
	}
	shutdown_sender ();
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
			print_send_msg_opt = true;
			continue;
		}
		if ((mode == 0) && (strcmp(arg, "rnd") == 0)) {
			wait_random_opt = true;
			continue;
		}
		if (mode == 'r') {
			rcv_id = arg;
			mode = 0;
			continue;
		}
		if (mode == 's') {
			send_id = arg;
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

	if (get_args(argc, argv) != 0)
		exit (4);

	msg_buf_size += OPT.msg_filler;

	if ((NULL != CLI.msg) && (NULL == send_id)) {
		printf ("msg specified witlhout a sender ID\n");
		exit(4);
	}

	if ((NULL == rcv_id) && (NULL == send_id)) {
		printf ("Nothing to do\n");
		exit(0);
	}

	if ((NULL != rcv_id) && (NULL != send_id)) {
		printf ("Not currently handling both send and receive\n");
		exit(0);
	}

	if (NULL != rcv_id) {
		if (connect_server () < 0)
			exit(4);
	}

	if (NULL != send_id)
		if (connect_sender () < 0)
			exit(4);

	if (NULL == CLI.msg)
		wait_for_msgs ();
	else 
		send_multiple ();

  printf ("Done!\n");
}
