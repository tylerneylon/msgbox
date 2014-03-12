// udp_echo_client.c
//
// A client that sends a one-way message and then a request.
//

#include "msgbox.h"

#include <stdio.h>

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
        conn->reply_context ? conn->reply_context : "<null>");

    msg_disconnect(conn);
    done = true;
  }
}

int main() {
  msg_connect("udp://127.0.0.1:2345", msg_no_context, update);

  int timeout_in_ms = 10;
  while (!done) msg_runloop(timeout_in_ms);

  return 0;
}
