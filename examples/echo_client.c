// echo_client.c
//
// A client that sends a one-way message and then a request.
//
// After echo_server has been started, run it like so:
//  ./echo_client tcp
//  ./echo_client udp
//

#include "msgbox.h"

#include <libgen.h>
#include <stdio.h>
#include <string.h>

#define true 1
#define false 0

static int done = false;

static char *event_names[] = {
  "msg_message",
  "msg_request",
  "msg_reply",
  "msg_listening",
  "msg_listening_ended",
  "msg_connection_ready",
  "msg_connection_closed",
  "msg_connection_lost",
  "msg_error"
};

void update(msg_Conn *conn, msg_Event event, msg_Data data) {

  printf("Client: received event %s.\n", event_names[event]);

  if (event == msg_error) printf("Client: error: %s.\n", msg_as_str(data));

  if (event == msg_connection_ready) {
    msg_Data data = msg_new_data("one-way message");
    msg_send(conn, data);
    msg_delete_data(data);
  }

  if (event == msg_message) {
    printf("Client: message is '%s'.\n", msg_as_str(data));

    msg_Data data = msg_new_data("request-reply message");
    msg_get(conn, data, "reply context");
    msg_delete_data(data);
  }

  if (event == msg_reply) {
    printf("Client: message is '%s'.\n", msg_as_str(data));
    printf("Client: reply_context is '%s'.\n",
        conn->reply_context ? (char *)conn->reply_context : "<null>");
    msg_disconnect(conn);
    done = true;
  }
}

int main(int argc, char **argv) {

  // Ensure argv[1] is either udp or tcp.
  if (argc != 2 || (strcmp(argv[1], "udp") && strcmp(argv[1], "tcp"))) {
    char *name = basename(argv[0]);
    printf("\n  Usage: %s (tcp|udp)\n\nMeant to be run after echo_server is started.\n", name);
    return 2;
  }

  char *protocol = argv[1];
  int port = protocol[0] == 't' ? 2345 : 2468;

  char address[128];
  snprintf(address, 128, "%s://127.0.0.1:%d", protocol, port);
  printf("Client: connecting to address %s\n", address);
  msg_connect(address, msg_no_context, update);

  int timeout_in_ms = 10;
  while (!done) msg_runloop(timeout_in_ms);

  return 0;
}
