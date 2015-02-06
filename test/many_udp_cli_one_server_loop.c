// many_udp_cli_one_server_loop.c
//
// https://github.com/tylerneylon/msgbox
//
// This tests that a single udp runloop cycle is capable of retrieving multiple
// messages *from different clients* available for reading from a single socket.
//
// This is similar to multi_msg_per_loop_test, except that it's specific to
// udp and uses multiple clients simultaneously. This is an important test case
// as a listening udp socket does *not* create new per-client sockets on
// receiving a message, unlike the tcp behavior.
//
// This test works as follows:
//  * 1. startup - 2 clients (C), 1 server (S)
//  * 2. C: send one message each; S: sleep
//  * 3. S: expect one message from each client, setting conn_context,
//          checking remote addresses, changes remote address; C: sleep
//  * 4. C: send one message each; S: sleep
//  * 5. S: expect one message from each client,
//          checking conn_context; C: sleep or exit
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
// server

int server_done;
int server_event_num;
int num_msg_recd;
Context server_ctx;

int server_round_one_received = false;

void server_update(msg_Conn *conn, msg_Event event, msg_Data data) {

  test_printf("<pid %d> Server: Received event %s\n", getpid(), event_names[event]);

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

  if (event == msg_connection_ready) {
    // Prevent leak-through of a previously-set conn_context.
    // The purpose of this is to encourage the test to fail if the conn_context
    // is not being saved/restored as expected.
    conn->conn_context = NULL;
  }

  if (event == msg_message) {
    char *str = msg_as_str(data);
    test_str_eq(str, "why hello");

    static char address1[64] = "";
    static char address2[64] = "";

    static int addr1_seen = false;
    static int addr2_seen = false;

    static uint32_t other_ip;
    static uint16_t other_port;

    if (!server_round_one_received) {

      // Handle round one; one message from each client.

      if (strcmp(address1, "") == 0) {
        // First message.
        strcpy(address1, msg_address_str(conn));
        conn->conn_context = address1;

        other_ip   = conn->remote_ip;
        other_port = conn->remote_port;

      } else if (strcmp(address2, "") == 0) {
        // Second message.
        strcpy(address2, msg_address_str(conn));
        conn->conn_context = address2;

        conn->remote_ip   = other_ip;
        conn->remote_port = other_port;

        test_that(strcmp(address1, address2) != 0);
        server_round_one_received = true;
      }

    } else {

      // Handle round two: another message from each client.

      if (!addr1_seen && conn->conn_context == address1) {
        addr1_seen = true;
      } else if (!addr2_seen && conn->conn_context == address2) {
        addr2_seen = true;
      } else {
        test_failed("Saw unexpected conn_context (%p); "
            "expected either address1 (%p) or address2 (%p); each once.\n",
            conn->conn_context, address1, address2);
      }
    }

    num_msg_recd++;
    if (num_msg_recd == 4) {
      server_done = true;
    }
  }

  server_event_num++;
}

int server() {
  server_done = false;
  server_event_num = 0;
  num_msg_recd = 0;

  char address[256];
  snprintf(address, 256, "udp://*:%d", udp_port);

  server_ctx.address   = strdup(address);
  server_ctx.num_tries = 0;

  msg_listen(address, server_update);

  // Sleep for 1ms to give the client time to send all the messages.
  usleep(1000);

  int timeout_in_ms = 10;
  while (!server_done) msg_runloop(timeout_in_ms);

  // TODO Occasionally the next check fails. Understand why and update things accordingly.
  test_that(num_msg_recd == 4);

  free(server_ctx.address);

  // Sleep for 1ms as the client expects to finish before the server.
  // (Early server termination could be an error, so we check for it.)
  usleep(1000);

  return test_success;
}

///////////////////////////////////////////////////////////////////////////////
// client

int client_done;
int client_event_num;
int client_round_one_sent = false;
msg_Conn *client_to_server_conn = NULL;

void client_update(msg_Conn *conn, msg_Event event, msg_Data data) {

  // We expect to hear events in this order:
  int expected_events[] = {
    msg_connection_ready, msg_connection_closed};

  Context *ctx = (Context *)conn->conn_context;

  test_printf("<pid %d> Client: Received event %s\n", getpid(), event_names[event]);

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
    msg_delete_data(data);

    client_to_server_conn = conn;
    client_round_one_sent = true;
  }

  client_event_num++;
}

int client(pid_t server_pid) {
  client_done = false;
  client_event_num = 0;

  // Sleep for 1ms to give the server time to start.
  usleep(1000);

  char address[256];
  snprintf(address, 256, "udp://127.0.0.1:%d", udp_port);

  Context *ctx = malloc(sizeof(Context));
  ctx->address   = strdup(address);
  ctx->num_tries = 0;

  msg_connect(address, client_update, ctx);
  int timeout_in_ms = 10;
  while (!client_done) {
    msg_runloop(timeout_in_ms);

    if (client_round_one_sent) {

      // Make sure the server sees all round one messages
      // before we move on to phase two of our master plan.
      usleep(5000);

      msg_Data data = msg_new_data("why hello");
      msg_send(client_to_server_conn, data);
      msg_delete_data(data);

      client_done = true;
    }

    // Check to see if the server process ended before we expected it to.
    int status;
    if (!client_done && waitpid(server_pid, &status, WNOHANG)) {
      test_failed("<pid %d> Client: Server process ended before client expected.", getpid());
    }
  }

  free(ctx->address);
  free(ctx);

  return test_success;
}

pid_t fork_a_client(pid_t server_pid) {
  pid_t child_pid = fork();

  if (child_pid == -1) return child_pid;  // Indicate failure.

  if (child_pid == 0) {
    // Child process.
    test_printf("A client pid=%d\n", getpid());
    exit(client(server_pid));
    // TODO Test memory cleanliness.
  } else {
    return child_pid;
  }
}

int many_udp_cli_one_server_loop_test() {

  pid_t server_pid = getpid();
  test_printf("<pid %d> Server: pid=%d\n", server_pid, server_pid);

  pid_t cli1_pid = fork_a_client(server_pid);
  pid_t cli2_pid = fork_a_client(server_pid);

  if (cli1_pid == -1 || cli2_pid == -1) {
    test_failed("<pid %d> Server: one of the client fork calls failed.", server_pid);
  }

  // The clients will be briefly paused now so we can start the server.
  int server_failed = server();
  if (server_failed) return server_failed;

  // Check that the clients exited as we expected them to.
  for (int i = 0; i < 2; ++i) {
    int status;
    wait(&status);
    int a_cli_failed = WEXITSTATUS(status);
    if (a_cli_failed) return a_cli_failed;
  }

  return test_success;
}

int main(int argc, char **argv) {
  set_verbose(0);  // Turn this on to help debug tests.

  // Generate random port numbers to help debugging in the face of bind errors
  // caused by 'address already in use' (from the internal TIME_WAIT tcp state).
  srand(time(NULL));
  udp_port = rand() % 1024 + 1024;

  start_all_tests(argv[0]);
  run_tests(many_udp_cli_one_server_loop_test);
  return end_all_tests();
}
