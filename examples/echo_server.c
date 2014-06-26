// echo_server.c
//
// Home repo: https://github.com/tylerneylon/msgbox
//
// A server that repeats back requests and messages.
//
// Run it as in one of these two examples:
//  ./echo_server tcp
//  ./echo_server udp
//

#include "msgbox.h"

#include <libgen.h>
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

  if (event == msg_listening) listening_conn = conn;

  if (event == msg_message || event == msg_request) {
    printf("Server: message is '%s'.\n", msg_as_str(data));

    // Reply to <msg> with echo:<msg>.
    msg_Data out_data = msg_new_data_space(data.num_bytes + strlen("echo:"));
    sprintf(out_data.bytes, "echo:%s", msg_as_str(data));
    msg_send(conn, out_data);
    msg_delete_data(out_data);
  }

  if (event == msg_connection_closed) done = true;
}

int main(int argc, char **argv) {

  // Ensure argv[1] is either udp or tcp.
  if (argc != 2 || (strcmp(argv[1], "udp") && strcmp(argv[1], "tcp"))) {
    char *name = basename(argv[0]);
    printf("\n  Usage: %s (tcp|udp)\n\n", name);
    return 2;
  }

  char *protocol = argv[1];
  int port = protocol[0] == 't' ? 2345 : 2468;

  char address[128];
  snprintf(address, 128, "%s://*:%d", protocol, port);
  printf("Server: listening at address %s\n", address);
  msg_listen(address, msg_no_context, update);

  int timeout_in_ms = 10;
  while (!done) msg_runloop(timeout_in_ms);

  msg_unlisten(listening_conn);

  // Give the runloop a chance to see the msg_listening_ended event.
  msg_runloop(timeout_in_ms);

  return 0;
}
