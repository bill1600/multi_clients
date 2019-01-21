#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
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

#define SOCK_SEND_TIMEOUT_MSEC 2000
#define IP_ADDR "127.0.0.1"

struct options {
  bool set_timeout;
  bool wait_send_ready;
  bool send_random;
  bool print_send_msgs;
  unsigned int msg_filler;
} OPT;

size_t msg_buf_size = 128;

struct client_stuff {
  const char *port_str;
  unsigned int send_count;
  const char *send_msg;
  struct client_conn conn;
} CLI;

typedef struct connection {
  int sock;
  unsigned int rcv_count;
  struct connection * next;
} connection_t;

static struct server_stuff {
  server_opts_t opts;
  const char *port_str;
  bool send_process_terminated;
  pthread_mutex_t list_mutex;
  struct connection * connection_list;
} SRV
 = {
     .opts = {.terminate_on_keypress = true,
       .waiting_msg = "Waiting for receive. Press <Enter> to terminate.\n"},
     .port_str = NULL,
     .send_process_terminated = false,
     .list_mutex = PTHREAD_MUTEX_INITIALIZER,
     .connection_list = NULL
   };

void init_options (void)
{
  OPT.set_timeout = true;
  OPT.wait_send_ready = true;
  OPT.send_random = false;
  OPT.print_send_msgs = false;
  OPT.msg_filler = 0;
}


void init_client_stuff (void)
{
  CLI.port_str = NULL;
  CLI.send_count = 0;
  init_client_conn (&CLI.conn);
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

static int create_thread (pthread_t *tid, void *(*thread_func) (void*), void *arg)
{
	int rtn = pthread_create (tid, NULL, thread_func, arg);
	if (rtn != 0) {
	  printf ("Error creating thread\n");
	}
	return rtn; 
}

static void *client_receiver_thread (void *arg)
{
  client_conn_t *conn = (client_conn_t *) arg;
  ssize_t rtn;

  //printf ("Started client receiver thread for %d\n", getpid());
  while (true) {
    rtn = cmsg_client_receive (conn);
    if (rtn < 0)
      break;
    printf ("Client %d received: %s\n", getpid(), conn->rcv_msg);
    free (conn->rcv_msg);
    conn->rcv_msg = NULL;
  }
  //printf ("Ending client receiver thread for %d\n", getpid());
}

void server_send_pass (socket_list_t *done_list, socket_list_t *not_done_list,
  const char *msg)
{
  int rtn;
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
      if (conn->rcv_count == 0) {
        append_to_socket_list (not_done_list, conn->sock);
        break;
      }
      rtn = cmsg_server_send (conn->sock, msg, sz_msg, true);
      if ((rtn == 0) || (rtn == EBADF))
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

int server_send_to_all_clients (const char *msg, unsigned timeout_ms,
  bool *terminated)
{
  int rtn;
  socket_list_t done_list;
  socket_list_t not_done_list;
  struct connection *conn;
  unsigned delay = 0, total_delay = 0;

  init_socket_list (&done_list);
  init_socket_list (&not_done_list);
  
  while (!server_received_something && !*terminated)
    wait_msecs (250);

  server_send_pass (&done_list, &not_done_list, msg);

  while (!*terminated) {
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
  rtn = server_send_to_all_clients ("Hello from the server!", 1000,
    &SRV.send_process_terminated);
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
	  if ((i>0) && (CLI.conn.rcv_count == 0))
	    wait_msecs (250);
	  else if (OPT.send_random)
	    wait_random ();
	  make_filled_msg (CLI.send_msg, i, buf);
	  sz_msg = strlen(buf) + 1;
	  if (cmsg_client_send(&CLI.conn, buf, sz_msg, false) != 0)
		break;
	  if (OPT.print_send_msgs)
	    printf ("Sent msg %lu\n", i);
  }
}


void show_msg (server_rcv_msg_data_t *rcv_msg_data, connection_t *conn)
{
  int i;
  unsigned count;
  char *buf = rcv_msg_data->rcv_msg;
  size_t bytes = rcv_msg_data->rcv_msg_size;

  if (NULL == conn) {
    printf ("Message socket %d not found in list\n", rcv_msg_data->sock);
    count = 0;
  } else {
    count = conn->rcv_count;
  }
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

void process_rcv_msg (int action_code, server_rcv_msg_data_t *rcv_msg_data)
{
  connection_t *conn;
  connection_t *tmp;

  switch (action_code) {
    case CMSG_ACTION_CONN_ADDED:
      pthread_mutex_lock (&SRV.list_mutex);
      conn = (connection_t *) malloc (sizeof (connection_t));
      if (NULL != conn) {
        conn->sock = rcv_msg_data->sock;
        conn->rcv_count = 0;
        LL_APPEND (SRV.connection_list, conn);
      } else {
        printf ("Unable to alloc memory for new connection\n");
      }
      pthread_mutex_unlock (&SRV.list_mutex);
      break;
    case CMSG_ACTION_CONN_DROPPED:
      pthread_mutex_lock (&SRV.list_mutex);
      LL_FOREACH_SAFE (SRV.connection_list, conn, tmp)
        if (conn->sock == rcv_msg_data->sock) {
          LL_DELETE (SRV.connection_list, conn);
          free (conn);
          break;
        }
      pthread_mutex_unlock (&SRV.list_mutex);
      break;
    case CMSG_ACTION_MSG_RECEIVED:
      conn = NULL;
      pthread_mutex_lock (&SRV.list_mutex);
      LL_FOREACH (SRV.connection_list, conn)
        if (conn->sock == rcv_msg_data->sock) {
          conn->rcv_count += 1;
          break;
        }
      pthread_mutex_unlock (&SRV.list_mutex);
      show_msg (rcv_msg_data, conn);
      free (rcv_msg_data->rcv_msg);
      rcv_msg_data->rcv_msg = NULL;
      server_received_something = true;
      break;
    default:
      printf ("Invalid action code %d\n", action_code);
  }
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
			CLI.send_msg = arg;
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

	if ((NULL != CLI.send_msg) && (NULL == CLI.port_str)) {
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
	  unsigned int port = parse_num_arg (SRV.port_str, "port");
	  if (port == (unsigned int) (-1))
	    exit (4);
	  if (cmsg_connect_server (IP_ADDR, port, &SRV.opts) != 0)
		exit(4);
	  if (create_thread (&server_send_thread_id, server_send_thread, NULL) == 0)
	  {
	    cmsg_server_listen_for_msgs (process_rcv_msg, NULL);
	    SRV.send_process_terminated = true;
	    pthread_join (server_send_thread_id, NULL);
	  }
	  pthread_mutex_destroy (&SRV.list_mutex);
	}

	if (NULL != CLI.port_str) {
	  unsigned int port = parse_num_arg (CLI.port_str, "port");
	  if (port == (unsigned int) (-1))
	    exit (4);
	  if (NULL == CLI.send_msg) {
		printf ("Message not specified for client\n");
		exit(4);
	  }
	  if (cmsg_connect_client (&CLI.conn, IP_ADDR, port, 
		SOCK_SEND_TIMEOUT_MSEC) < 0)
	    exit(4);
	  if (create_thread (&client_rcv_thread_id, client_receiver_thread, &CLI.conn) == 0)
	  {
 	    client_send_multiple ();
            CLI.conn.terminated = true;
            pthread_join (client_rcv_thread_id, NULL);
	  }
          cmsg_shutdown_client (&CLI.conn);
	}


  printf ("%d Done!\n", getpid());
}
