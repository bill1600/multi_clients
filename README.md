# multi_clients
Demo of socket server with multiple clients

# Building
. make_multi.sh

# Testing
. demo_server.sh

In another terminal window:

. demo_clients.sh

Demo shows a server receiving messages from 24 clients, and sending a hello message to each client.
Messages vary from 100 to 8000 bytes in length.

# Dependencies
utlist.h is a copywrited include file that handles linked lists

It can be obtained from https://github.com/troydhanson/uthash


