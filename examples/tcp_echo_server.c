// tcp_echo_server.c
//
// A server that repeats back requests and messages.
//

#include "msgbox.h"

#include <stdio.h>
#include <string.h>

#define true 1
#define false 0

static int done = false;
static msg_Conn *listening_conn = NULL;

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
  printf("Server: received event %s.\n", event_names[event]);
  if (event == msg_error) printf("Server: error: %s.\n", msg_as_str(data));

  if (event == msg_listening) {
    listening_conn = conn;
  }

  if (event == msg_message || event == msg_request) {
    printf("Server: message is '%s'.\n", msg_as_str(data));

    // Reply to <msg> with echo:<msg>.
    msg_Data out_data = msg_new_data_space(data.num_bytes + strlen("echo:"));
    sprintf(out_data.bytes, "echo:%s", msg_as_str(data));
    msg_send(conn, out_data);
    msg_delete_data(out_data);
  }

  if (event == msg_connection_closed) {
    done = true;
  }
}

int main() {
  msg_listen("tcp://*:2468", msg_no_context, update);

  int timeout_in_ms = 10;
  while (!done) msg_runloop(timeout_in_ms);

  msg_unlisten(listening_conn);

  // Give the runloop a chance to see the msg_listening_ended event.
  msg_runloop(timeout_in_ms);

  return 0;
}
