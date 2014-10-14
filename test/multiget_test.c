// multiget_test.c
//
// Home repo: https://github.com/tylerneylon/msgbox
//
// Unit tests involving multiple gets for msgbox.
//

// This is the basic protocol followed by this client/server setup:
//
// c: get "hi"
//    s: send "hello"
// c: send "do you know what a rhetorical question is?"
// s: send "do i know what a rhetorical question is?"
// c: get "bye"
//    s: send "byee"

#include "msgbox.h"

#include "ctest.h"

// TODO Drop any unneeded includes here.
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cstructs/memprofile.h"

#define array_size(x) (sizeof(x) / sizeof(x[0]))

#define true 1
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

static void server_reply(char *req_str, char *reply_str, msg_Conn *conn) {
  test_printf("Server: got '%s', replying with '%s'.\n", req_str, reply_str);
  msg_Data data = msg_new_data(reply_str);
  msg_send(conn, data);
  msg_delete_data(data);
}

void server_update(msg_Conn *conn, msg_Event event, msg_Data data) {

  // We expect to hear events in this order.
  int expected_events[] = {
    msg_listening, msg_connection_ready, msg_request,
    msg_message, msg_request, msg_connection_closed};

  Context *ctx = (Context *)conn->conn_context;

  test_printf("Server: Received event %s\n", event_names[event]);

  if (event == msg_error) {
    char *err_str = msg_as_str(data);
    test_printf("Server: Error: %s\n", err_str);
    if (strcmp(err_str, "bind: Address already in use") == 0) {
      if (ctx->num_tries < max_tries) {
        test_printf("Will wait briefly and try again at address %s.\n", ctx->address);
        sleep(5);
        ctx->num_tries++;
        msg_listen(ctx->address, ctx, server_update);
        return;  // Don't count this as a server event.
      } else {
        test_printf("Server: max_tries reached; giving up listening (at %s).\n", ctx->address);
      }
    }
  }

  test_that(server_event_num < array_size(expected_events));
  test_that(event == expected_events[server_event_num]);

  if (event == msg_request) {
    char *req_str = msg_as_str(data);
    if (strcmp(req_str, "hi") == 0) {
      server_reply(req_str, "hello", conn);
    } else if (strcmp(req_str, "bye") == 0) {
      server_reply(req_str, "byee", conn);
    } else {
      test_failed("Server: Unexpected request string: %s.\n", req_str);
    }
  }

  if (event == msg_message) {
    char *str = msg_as_str(data);
    if (strcmp(str, "do you know what a rhetorical question is?") == 0) {
      test_printf("Server: replying to a one-way rhetorical message.\n");
      msg_Data reply_data = msg_new_data("do i know what a rhetorical question is?");
      msg_send(conn, reply_data);
      msg_delete_data(reply_data);
    } else {
      test_failed("Server: Unexpected message string: %s.\n", str);
    }
  }

  if (event == msg_connection_closed) {
    test_printf("Server: Connection closed.\n");
    free(ctx->address);
    free(ctx);
    server_done = true;
  }

  server_event_num++;
}

int server(int protocol_type) {
  server_done = false;
  server_event_num = 0;

  char address[256];
  snprintf(address, 256, "%s://*:%d",
      protocol_type == msg_udp ? "udp" : "tcp",
      protocol_type == msg_udp ? udp_port : tcp_port);

  Context *ctx = malloc(sizeof(Context));
  ctx->address   = strdup(address);
  ctx->num_tries = 0;

  msg_listen(address,         // protocol
             ctx,             // context
             server_update);  // callback
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
    msg_connection_ready, msg_reply, msg_message, msg_reply, msg_connection_closed};

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
        msg_connect(ctx->address, ctx, client_update);
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
    msg_Data data = msg_new_data("hi");
    msg_get(conn, data, msg_no_context);
    msg_delete_data(data);
  }

  if (event == msg_message) {
    test_printf("Client: Message from %s:%d.\n", msg_ip_str(conn), conn->remote_port);
    test_printf("Client: The message is '%s'.\n", msg_as_str(data));
    test_str_eq(msg_as_str(data), "do i know what a rhetorical question is?");

    msg_Data req_data = msg_new_data("bye");
    msg_get(conn, req_data, msg_no_context);
    msg_delete_data(req_data);
  }

  if (event == msg_reply) {
    test_printf("Client: Message from %s:%d.\n", msg_ip_str(conn), conn->remote_port);
    test_printf("Client: The message is '%s'.\n", msg_as_str(data));

    char *str = msg_as_str(data);
    if (strcmp(str, "hello") == 0) {
      msg_Data next_data = msg_new_data("do you know what a rhetorical question is?");
      msg_send(conn, next_data);
      msg_delete_data(next_data);
    } else if (strcmp(str, "byee") == 0) {
      msg_disconnect(conn);
    } else {
      test_failed("Client: Unexpected reply string '%s'.\n", str);
    }
  }

  if (event == msg_connection_closed) {
    test_printf("Client: Connection closed.\n");
    free(ctx->address);
    free(ctx);
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

  msg_connect(address, ctx, client_update);
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
