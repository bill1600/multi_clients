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
#include "cimpmsg.h"

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


typedef struct connection {
  int oserr;
  int rcv_state;
  bool rcv_selected;
  size_t rcv_end_pos;
  server_rcv_msg_data_t rcv_data;
  struct connection * next;
} connection_t;


struct server_stuff {
  unsigned int port;
  struct sockaddr_in addr;
  int listen_sock;
  bool send_process_terminated;
  bool terminate_on_keypress;
  const char *waiting_msg;
  pthread_mutex_t list_mutex;
  struct connection * connection_list;
} SRV
 = { .port = (unsigned int) -1, .listen_sock = -1,
     .send_process_terminated = false, .terminate_on_keypress = true,
     .waiting_msg = "Waiting for receive. Press <Enter> to terminate.\n",
     .list_mutex = PTHREAD_MUTEX_INITIALIZER,
     .connection_list = NULL
   };



void init_connection (struct connection *conn)
{
  conn->rcv_data.sock = -1;
  conn->oserr = 0;
  conn->rcv_state = -1;
  conn->rcv_data.rcv_count = 0;
  conn->rcv_selected = false;
  conn->rcv_data.rcv_msg_size = 0;
  conn->rcv_end_pos = 0;
  conn->rcv_data.rcv_msg = NULL;
  conn->next = NULL;
}

void init_client_conn (struct client_conn *conn)
{
  conn->sock = -1;
  conn->oserr = 0;
  conn->rcv_msg = NULL;
  conn->rcv_msg_size = 0;
  conn->rcv_count = 0;
  conn->terminated = false;
}


int wait_server_ready (bool *terminated)
{
  struct timeval timeout;
  struct connection *conn;
  int i, rtn, sock, highest_sock;
  int fd = SRV.listen_sock;
  int timeout_count = 0;
  fd_set fds;

  highest_sock = -1;

  while (1)
  {
    timeout.tv_sec = 0;
    timeout.tv_usec = 500000;
    FD_ZERO (&fds);
    if (SRV.listen_sock != -1) {
      FD_SET (SRV.listen_sock, &fds);
      highest_sock = SRV.listen_sock;
      // printf ("Waiting on listener %d\n", listen_sock);
    }
    LL_FOREACH (SRV.connection_list, conn) {
      conn->rcv_selected = false;
      if (conn->rcv_state >= 0) {
        sock = conn->rcv_data.sock;
        // printf ("Waiting on %d\n", sock);
        if (sock > highest_sock)
          highest_sock = sock;
        FD_SET (sock, &fds);
      }
    }
    if (SRV.terminate_on_keypress) {
      FD_SET (STDIN_FILENO, &fds);
    }
    rtn = select (highest_sock+1, &fds, NULL, NULL, &timeout);
    if (rtn < 0) {
      printf ("Error on select for receive\n");
      return -1;
    }
    if (rtn != 0)
      break;
    if (NULL != terminated)
      if (*terminated)
        break;
    if (NULL != SRV.waiting_msg) {
      ++timeout_count;
      if ((timeout_count & 3) == 0)
        printf (SRV.waiting_msg);
    }
  }
  rtn = 0;
  if (SRV.listen_sock != -1)
    if (FD_ISSET (SRV.listen_sock, &fds))
      rtn = 1;
  LL_FOREACH (SRV.connection_list, conn) {
    if (conn->rcv_state >= 0)
      if (FD_ISSET (conn->rcv_data.sock, &fds)) {
        conn->rcv_selected = true;
        rtn |= 2;
      }
  }
  if (SRV.terminate_on_keypress) {
    if (FD_ISSET (STDIN_FILENO, &fds))
      rtn |= 4;
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

int make_sockaddr (struct sockaddr_in *addr, 
  const char *ip_addr, unsigned int port, bool rcv_any)
{
  int rtn;
  if (port == (unsigned) -1)
    return -1;
  
  addr->sin_family = AF_INET;
  addr->sin_port = htons (port);
  if (rcv_any)
    addr->sin_addr.s_addr = INADDR_ANY;
  else {
    rtn = inet_pton (AF_INET, ip_addr, &addr->sin_addr);
    if (rtn != 1) {
      printf ("inet_pton error\n");
      return -1;
    }
  }
  return 0;
}

int cmsg_connect_server (const char *ip_addr, unsigned int port,
  server_opts_t *options)
{
	int sock, flags, rtn;
	int reuse_opt = 1;

	if (NULL != options) {
		SRV.terminate_on_keypress = options->terminate_on_keypress;
		SRV.waiting_msg = options->waiting_msg;
	}

	if ((NULL == ip_addr) || ((unsigned int) -1 == port)) {
		SRV.listen_sock = -1;
		return EINVAL;
	}

	if (make_sockaddr (&SRV.addr, ip_addr, port, false) != 0)
          return EINVAL;
	sock = socket (AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		dbg_err (errno, "Unable to create rcv socket\n");
 		return errno;
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
		rtn = errno;
		close (sock);
		return rtn;
	}
	if (listen (sock, 50) == -1) {
	  dbg_err (errno, "Listen error on receive socket: %s\n");
	  rtn = errno;
	  close (sock);
	  return rtn;
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
    return 2;
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
  conn->rcv_data.sock = sock;
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
    shutdown (conn->rcv_data.sock, SHUT_RDWR);
    close (conn->rcv_data.sock);
    conn->rcv_data.sock = -1;
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

int cmsg_connect_client (struct client_conn *conn, 
  const char *ip_addr, unsigned int port, unsigned int send_timeout_msecs)
{
	int sock;
	int reuse_opt = 1;
	struct timeval send_timeout;
	struct timeval rcv_timeout;

	init_client_conn (conn);

	if ((unsigned int) -1 == port) {
		conn->sock = -1;
		return -1;
	}
	if (make_sockaddr (&conn->addr, ip_addr, port, false) != 0)
          return -1;
	sock = socket (AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		conn->oserr = errno;
		dbg_err (errno, "Unable to create send socket\n");
 		return -1;
	}
	if (send_timeout_msecs != (unsigned int) -1) {
		send_timeout.tv_sec = send_timeout_msecs / 1000;
		send_timeout.tv_usec = (send_timeout_msecs % 1000) * 1000;
		if (setsockopt (sock, SOL_SOCKET, SO_SNDTIMEO, 
		  &send_timeout, sizeof (send_timeout)) < 0) {
			conn->oserr = errno;
			dbg_err (errno, "Unable to set socket send timeout: \n");
			close (sock);
	 		return -1;
		}
	}
	rcv_timeout.tv_sec = 0;
	rcv_timeout.tv_usec = 500000;
	if (setsockopt (sock, SOL_SOCKET, SO_RCVTIMEO, 
		  &rcv_timeout, sizeof (rcv_timeout)) < 0) {
			conn->oserr = errno;
			dbg_err (errno, "Unable to set socket rcv timeout: \n");
			close (sock);
	 		return -1;
		}
	if (connect (sock, (struct sockaddr *) &conn->addr, sizeof (conn->addr)) < 0) {
		conn->oserr = errno;
		dbg_err (errno, "Unable to connect to client socket\n");
		shutdown (sock, SHUT_RDWR);
		close (sock);
		return -1;
	}
	conn->sock = sock;
	return 0;
}

void cmsg_shutdown_client (struct client_conn *conn)
{
  if (conn->sock != -1) {
	shutdown_sock (conn->sock);
	conn->sock = -1;
  }
}


ssize_t socket_receive (struct connection *conn, void *buf, size_t len, bool *terminated)
{
  ssize_t bytes;

  while (true) {
    bytes = recv (conn->rcv_data.sock, buf, len, 0);
    if (bytes >= 0)
      return bytes;
    if (NULL != terminated) {
      if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
        if (*terminated)
          return -2;
        continue; 
      }
    }
    conn->oserr = errno;
    return -1;
  }
}

int receive_msg_header (struct connection *conn, bool *terminated)
{
  int sock = conn->rcv_data.sock;
  ssize_t bytes;
  size_t msg_size;
  unsigned char header[4];

  bytes = socket_receive (conn, header, 4, terminated);
  if (bytes < 0) { 
    if (bytes == -1)
      dbg_err (conn->oserr, "Error receiving msg header\n");
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
  conn->rcv_data.rcv_msg = malloc (msg_size);
  if (NULL == conn->rcv_data.rcv_msg) {
    printf ("Unable to malloc msg buffer for socket %d\n", sock);
    return -1;
  }
  conn->rcv_data.rcv_msg_size = msg_size;
  conn->rcv_end_pos = 0;
  conn->rcv_state = 1;
  return 0;
}


// returned msg must be freed
int receive_msg_data (struct connection *conn, process_message_t handle_msg,
  bool *terminated)
{
  ssize_t bytes;
  size_t read_len = conn->rcv_data.rcv_msg_size - conn->rcv_end_pos;
  int sock = conn->rcv_data.sock;
  char *buf = conn->rcv_data.rcv_msg;

  bytes = socket_receive (conn, buf+conn->rcv_end_pos, read_len, terminated);

  if (bytes < 0) { 
    if (bytes == -1)
      dbg_err (conn->oserr, "Error receiving msg header\n");
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
    handle_msg (&conn->rcv_data);
  conn->rcv_data.rcv_count += 1;
  conn->rcv_state = 0;
  return 1;
}

ssize_t cmsg_client_receive (struct client_conn *cconn)
{
  int rtn;
  struct connection rconn;

  init_connection (&rconn);
  rconn.rcv_data.sock = cconn->sock;
  rconn.rcv_state = 0;

  rtn = receive_msg_header (&rconn, &cconn->terminated);
  if (rtn < 0)
    return rtn;
  while (true) {
    rtn = receive_msg_data (&rconn, NULL, &cconn->terminated);
    if (rtn == 1) {
      cconn->rcv_msg = rconn.rcv_data.rcv_msg;
      cconn->rcv_msg_size = rconn.rcv_data.rcv_msg_size; 
      cconn->rcv_count++;
      return (ssize_t) rconn.rcv_data.rcv_msg_size;
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
        printf ("Closing connection for socket %d\n", conn->rcv_data.sock);
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


void cmsg_server_listen_for_msgs (process_message_t handle_msg, bool *terminated)
{
  int rtn;
  char inbuf[10];

  while (1)
  {
	  rtn = wait_server_ready (terminated);
	  if (rtn < 0)
	    break;
	  if (rtn & 1)
	    server_accept ();
	  if (rtn & 2)
	    server_receive_msgs (handle_msg);
	  if (SRV.terminate_on_keypress) {
	    if (rtn & 4) { // key pressed
	      fgets (inbuf, 10, stdin);
	      break;
	    }
	  }
	  if (NULL != terminated)
            if (*terminated)
	      break;
  }
  printf ("Exiting wait_for_msgs\n");
  shutdown_server ();
}

int cmsg_send_msg (int sock, const char *msg, size_t sz_msg, bool non_block)
{
  int flags = 0;
  ssize_t bytes;
  int i;
  char *msg_buf;

  msg_buf = malloc (sz_msg+4);
  if (NULL == msg_buf) {
    printf ("Unable to malloc msg buffer for socket %d\n", sock);
    return ENOMEM;
  }
  msg_buf[0] = MSG_HEADER_MARK;
  msg_buf[1] = MSG_HEADER_MARK;
  msg_buf[2] = sz_msg / 256;
  msg_buf[3] = sz_msg % 256;
  memcpy (msg_buf+4, msg, sz_msg);

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
	return errno;
  }
  if (bytes != sz_msg) {
	printf ("Not all bytes sent, just %d\n", bytes);
	return EIO;
  }
  return 0;
}

