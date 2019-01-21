/**
 * Copyright 2016 Comcast Cable Communications Management, LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#ifndef  _CIMPMSG_H
#define  _CIMPMSG_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

/*----------------------------------------------------------------------------*/
/*                               Data Structures                              */
/*----------------------------------------------------------------------------*/

typedef struct client_conn {
  struct sockaddr_in addr;
  int sock;
  int oserr;
  char *rcv_msg;
  size_t rcv_msg_size;
  unsigned int rcv_count;
  bool terminated;
  pthread_mutex_t send_mutex;
  pthread_mutex_t rcv_mutex;
} client_conn_t;

typedef struct server_opts {
  bool terminate_on_keypress;
  const char *waiting_msg;
} server_opts_t;

typedef struct server_rcv_msg_data {
  int sock;
  char *rcv_msg;
  size_t rcv_msg_size;
} server_rcv_msg_data_t;

#define CMSG_ACTION_MSG_RECEIVED	0
#define CMSG_ACTION_CONN_ADDED		1
#define CMSG_ACTION_CONN_DROPPED	2

typedef void (* process_message_t) 
    (int action_code, server_rcv_msg_data_t *rcv_msg_data);

/*----------------------------------------------------------------------------*/
/*                             Function Prototypes                            */
/*----------------------------------------------------------------------------*/

int cmsg_connect_server (const char *ip_addr, unsigned int port, 
  server_opts_t *options);
int cmsg_server_listen_for_msgs (process_message_t handle_msg, bool *terminated);
// Will exit and shutdown server if terminated flag is set,
// or if option terminate_on_keypress specified and a key is pressed
int cmsg_server_send (int sock, const char *msg, size_t sz_msg, bool non_block);

void init_client_conn (struct client_conn *conn);
int cmsg_connect_client (struct client_conn *conn, 
  const char *ip_addr, unsigned int port, unsigned int send_timeout_msecs);
void cmsg_shutdown_client (struct client_conn *conn);
// will set conn->terminated
ssize_t cmsg_client_receive (struct client_conn *conn);
// will return -1 if conn->terminated is set
int cmsg_client_send (struct client_conn *conn, const char *msg, size_t sz_msg, bool non_block);



#endif

