// timeout_test.c
//
// Home repo: https://github.com/tylerneylon/msgbox
//
// udp and tcp timeout tests for msgbox.
// This started as a copy of msgbox_test.
//

#include "msgbox.h"

#include "ctest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define array_size(x) (sizeof(x) / sizeof(x[0]))

#define true  1
#define false 0

// This can be used in cases of emergency debugging.
#define prline printf("%s:%d(%s)\n", __FILE__, __LINE__, __FUNCTION__)

///////////////////////////////////////////////////////////////////////////////
// useful globals, types, and functions

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

int udp_port;

typedef struct {
  char *address;
  int   num_tries;
} Context;

int max_tries = 24;


///////////////////////////////////////////////////////////////////////////////
// basic server

int server_done;
int server_event_num;

void server_update(msg_Conn *conn, msg_Event event, msg_Data data) {
  if (event == msg_connection_closed) {
    test_printf("Server: Connection closed.\n");
    server_done = true;
  }
}

int server(const char *protocol) {
  server_done = false;
  server_event_num = 0;

  char address[256];
  snprintf(address, 256, "%s://*:%d", protocol, udp_port);

  msg_listen(address, server_update);
  int timeout_in_ms = 10;

  while (!server_done) msg_runloop(timeout_in_ms);

  // Sleep for 1ms as the client expects to finish before the server.
  // (Early server termination could be an error, so we check for it.)
  usleep(1000);

  return test_success;
}

///////////////////////////////////////////////////////////////////////////////
// basic client

int client_done;
int client_event_num;

void client_update(msg_Conn *conn, msg_Event event, msg_Data data) {

  // We expect to hear events in this order:
  int expected_events[] = {
    msg_connection_ready, msg_error, msg_connection_closed};

  Context *ctx = (Context *)conn->conn_context;

  test_printf("Client: Received event %s\n", event_names[event]);

  if (event == msg_error) {
    const char *err = msg_as_str(data);
    test_printf("Client: Error: %s\n", err);
    const char *expected_err = (conn->protocol_type == msg_tcp ?
        "tcp get timed out" : "udp get timed out");
    test_str_eq(err, expected_err);
    msg_disconnect(conn);
  }

  test_that(client_event_num < array_size(expected_events));
  test_that(event == expected_events[client_event_num]);

  if (event == msg_connection_ready) {
    msg_Data data = msg_new_data("hello msgbox!");
    msg_get(conn, data, NULL);
    msg_delete_data(data);
  }

  if (event == msg_connection_closed) {
    test_printf("Client: Connection closed.\n");
    free(ctx->address);
    free(ctx);
    client_done = true;
  }

  client_event_num++;
}

int client(const char *protocol, pid_t server_pid) {
  client_done = false;
  client_event_num = 0;

  // Sleep for 1ms to give the server time to start.
  usleep(1000);

  char address[256];
  snprintf(address, 256, "%s://127.0.0.1:%d", protocol, udp_port);

  Context *ctx = malloc(sizeof(Context));
  ctx->address   = strdup(address);
  ctx->num_tries = 0;

  msg_connect(address, client_update, ctx);
  int timeout_in_ms = 10;
  while (!client_done) {
    msg_runloop(timeout_in_ms);

    // Check to see if the server process ended before we expected it to.
    int status;
    if (!client_done && waitpid(server_pid, &status, WNOHANG)) {
      test_failed("Client: Server process ended before client expected.");
    }
  }

  return test_success;
}

int timeout_test(const char *protocol) {

  test_printf("Test: Starting %s timeout test.\n", protocol);

  int status;
  pid_t child_pid = fork();
  if (child_pid == -1) return test_failure;

  if (child_pid == 0) {
    // Child process.
    exit(server(protocol));
  } else {
    // Parent process.
    test_printf("Client pid=%d  server pid=%d\n", getpid(), child_pid);
    test_printf("Client: starting up.\n"); // tmp
    int client_failed = client(protocol, child_pid);
    int server_status;
    wait(&server_status);
    int server_failed = WEXITSTATUS(server_status);

    test_printf("Test: client_failed=%d server_failed=%d.\n", client_failed, server_failed);

    return client_failed || server_failed;
  }
}

int udp_timeout_test() {
  return timeout_test("udp");
}

int tcp_timeout_test() {
  return timeout_test("tcp");
}

int main(int argc, char **argv) {
  set_verbose(0);  // Turn this on to help debug tests.

  // Generate random port numbers to help debugging in the face of bind errors
  // caused by 'address already in use' (from the internal TIME_WAIT tcp state).
  srand(time(NULL));
  udp_port = rand() % 1024 + 1024;

  start_all_tests(argv[0]);
  run_tests(udp_timeout_test, tcp_timeout_test);
  return end_all_tests();
}
