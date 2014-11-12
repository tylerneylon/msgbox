// multi_msg_per_loop_test.c
//
// https://github.com/tylerneylon/msgbox
//
// This tests that a single runloop cycle is capable of retrieving multiple
// messages available for reading from a single socket.
//
// This test works as follows:
//  * client and server both start
//  * server sleeps briefly so the client can send several messages
//  * as soon as the server receives a message, that is its last runloop cycle
//
// Virtually all the time, if msgbox is working correctly and can receive
// multiple messages from the same socket in a single cycle, all the messages
// should be received together in a single call to msg_runloop.
//
// Under pathological conditions, msgbox could be working correctly but this
// test would fail in that, theoretically, the messages could not all be ready
// for the server all at the same time.
//

#include "msgbox.h"

#include "ctest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cstructs/memprofile.h"

#define array_size(x) (sizeof(x) / sizeof(x[0]))

#define true  1
#define false 0

// This can be used in cases of emergency debugging.
#define prline printf("%s:%d(%s)\n", __FILE__, __LINE__, __FUNCTION__)

// Defined in msgbox.c.
int net_allocs_for_class(int class);


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
int tcp_port;

typedef struct {
  char *address;
  int   num_tries;
} Context;

int max_tries = 24;


///////////////////////////////////////////////////////////////////////////////
// basic server

int server_done;
int server_event_num;
int num_msg_recd;
Context server_ctx;

void server_update(msg_Conn *conn, msg_Event event, msg_Data data) {

  // We expect to hear events in this order.
  int expected_events[] = {
    msg_listening, msg_connection_ready, msg_message,
    msg_message, msg_message, msg_connection_closed};

  test_printf("Server: Received event %s\n", event_names[event]);

  if (event == msg_error) {
    char *err_str = msg_as_str(data);
    test_printf("Server: Error: %s\n", err_str);
    if (strcmp(err_str, "bind: Address already in use") == 0) {
      if (server_ctx.num_tries < max_tries) {
        test_printf("Will wait briefly and try again at address %s.\n", server_ctx.address);
        sleep(5);
        server_ctx.num_tries++;
        msg_listen(server_ctx.address, server_update);
        return;  // Don't count this as a server event.
      } else {
        test_printf("Server: max_tries reached; giving up listening (at %s).\n", server_ctx.address);
      }
    }
  }

  test_that(server_event_num < array_size(expected_events));
  test_that(event == expected_events[server_event_num]);

  if (event == msg_message) {
    char *str = msg_as_str(data);
    test_str_eq(str, "why hello");
    server_done = true;
    num_msg_recd++;
  }

  server_event_num++;
}

int server(int protocol_type) {
  server_done = false;
  server_event_num = 0;
  num_msg_recd = 0;

  char address[256];
  snprintf(address, 256, "%s://*:%d",
      protocol_type == msg_udp ? "udp" : "tcp",
      protocol_type == msg_udp ? udp_port : tcp_port);

  server_ctx.address   = strdup(address);
  server_ctx.num_tries = 0;

  msg_listen(address, server_update);
  int timeout_in_ms = 10;

  // Sleep for 1ms to give the client time to send all the messages.
  usleep(1000);

  while (!server_done) msg_runloop(timeout_in_ms);

  test_that(num_msg_recd == 3);

  free(server_ctx.address);

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
    msg_connection_ready, msg_connection_closed};

  Context *ctx = (Context *)conn->conn_context;

  test_printf("Client: Received event %s\n", event_names[event]);

  if (event == msg_error) {
    char *err_str = msg_as_str(data);
    test_printf("Client: Error: %s\n", err_str);
    if (strcmp(err_str, "connect: Connection refused") == 0) {
      if (ctx->num_tries < max_tries) {
        test_printf("Client: Will wait briefly and try again at address %s.\n", ctx->address);
        sleep(5);
        ctx->num_tries++;
        msg_connect(ctx->address, client_update, ctx);
        return;  // Don't count this as a server event.
      } else {
        test_printf("Client: max_tries reached; giving up connecting (at %s).\n", ctx->address);
      }
    }
  }

  if (event == msg_error) test_printf("Client: Error: %s\n", msg_as_str(data));

  test_that(client_event_num < array_size(expected_events));
  test_that(event == expected_events[client_event_num]);

  if (event == msg_connection_ready) {
    msg_Data data = msg_new_data("why hello");
    msg_send(conn, data);
    msg_send(conn, data);
    msg_send(conn, data);
    msg_delete_data(data);
    client_done = true;
  }

  client_event_num++;
}

int client(int protocol_type, pid_t server_pid) {
  client_done = false;
  client_event_num = 0;

  // Sleep for 1ms to give the server time to start.
  usleep(1000);

  char address[256];
  snprintf(address, 256, "%s://127.0.0.1:%d",
      protocol_type == msg_udp ? "udp" : "tcp",
      protocol_type == msg_udp ? udp_port : tcp_port);

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

  free(ctx->address);
  free(ctx);

  return test_success;
}

int basic_test(int protocol_type) {

  test_printf("Test: Starting %s test.\n", protocol_type == msg_udp ? "udp" : "tcp");

  int status;
  pid_t child_pid = fork();
  if (child_pid == -1) return test_failure;

  if (child_pid == 0) {
    // Child process.
    exit(server(protocol_type));
    // TODO Test memory cleanliness (use net_allocs_for_class).
  } else {
    // Parent process.
    test_printf("Client pid=%d  server pid=%d\n", getpid(), child_pid);
    test_printf("Client: starting up.\n"); // tmp
    int client_failed = client(protocol_type, child_pid);
    int server_status;
    wait(&server_status);
    int server_failed = WEXITSTATUS(server_status);

    // Help check for memory leaks.
    test_that(net_allocs_for_class(0) == 0);

    // TODO Add deeper memory-leak checks via memprofile.

    test_printf("Test: client_failed=%d server_failed=%d.\n", client_failed, server_failed);

    return client_failed || server_failed;
  }
}

int udp_test() { return basic_test(msg_udp); }

int tcp_test() { return basic_test(msg_tcp); }

int main(int argc, char **argv) {
  set_verbose(0);  // Turn this on to help debug tests.

  // Generate random port numbers to help debugging in the face of bind errors
  // caused by 'address already in use' (from the internal TIME_WAIT tcp state).
  srand(time(NULL));
  udp_port = rand() % 1024 + 1024;
  tcp_port = rand() % 1024 + 1024;

  start_all_tests(argv[0]);
  run_tests(udp_test, tcp_test);
  return end_all_tests();
}
