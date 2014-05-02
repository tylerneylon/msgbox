// msgbox_test.c
//
// Unit tests for msgbox.
//

// TODO
// * Make it possible to easily run a test with one
//   part running on a remote server, and another
//   running on a client, possibly behind a NAT.
//

#include "msgbox.h"

#include "ctest.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "memprofile.h"

#define array_size(x) (sizeof(x) / sizeof(x[0]))

#define true 1
#define false 0

// Defined in msgbox.c.
int net_allocs_for_class(int class);

///////////////////////////////////////////////////////////////////////////////
// useful globals and functions

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

///////////////////////////////////////////////////////////////////////////////
// long string test

char *long_string = NULL;

void init_long_string_if_needed() {
  if (long_string) return;

  const long len = 1 << 20;
  char *alpha   = "abcdefghijklmnopqrstuvwxyz";
  long_string   = malloc(len);
  char *end     = long_string + len;
  for (char *c = long_string; c < end; ++c) {
    long index = c - long_string;
    index      = (int)(sin(index) * 12.5 + 12.5);
    *c         = alpha[index];
  }
  long_string[len - 1] = '\0';
}

int long_string_server_done;

void long_string_server_update(msg_Conn *conn, msg_Event event, msg_Data data) {
  test_printf("Server: Received event %s\n", event_names[event]);
  if (event == msg_error) test_printf("Server: Error: %s\n", msg_as_str(data));

  if (event == msg_message) {
    // Don't use test_str_eq here as these strings are too long to print out.
    int str_are_equal = (strcmp(msg_as_str(data), long_string) == 0);
    if (!str_are_equal) {
      test_printf("Unequal strings; rec'd len=%d, real len=%d\n",
                  strlen(msg_as_str(data)),
                  strlen(long_string));
    }
    test_that(str_are_equal);

    // Send a message back to trigger the client to close.
    // We avoid a TIME_WAIT state on the server when the client closes.
    msg_Data data = msg_new_data("thanks!");
    msg_send(conn, data);
    msg_delete_data(data);
  }

  if (event == msg_connection_closed) {
    test_printf("Server: Connection closed.\n");
    long_string_server_done = true;
  }

  if (event == msg_connection_lost) {
    test_failed("Server: connection lost.\n");
  }
}

int long_string_server() {
  long_string_server_done = false;

  char address[256];
  snprintf(address, 256, "tcp://*:%d", tcp_port);

  msg_listen(address, msg_no_context, long_string_server_update);
  int timeout_in_ms = 10;
  while (!long_string_server_done) msg_runloop(timeout_in_ms);

  // Sleep for 1ms as the client expects to finish before the server.
  // (Early server termination could be an error, so we check for it.)
  usleep(1000);

  return test_success;
}

int long_string_client_done;

void long_string_client_update(msg_Conn *conn, msg_Event event, msg_Data data) {
  test_printf("Client: Received event %s\n", event_names[event]);
  if (event == msg_error) test_printf("Client: Error: %s\n", msg_as_str(data));

  if (event == msg_connection_ready) {
    test_printf("long_string has len=%d\n", strlen(long_string));
    msg_Data data = msg_new_data(long_string);
    msg_send(conn, data);
    msg_delete_data(data);
  }

  if (event == msg_message) {
    test_printf("Client: closing connection.\n");
    msg_disconnect(conn);
  }

  if (event == msg_connection_closed) {
    test_printf("Client: Connection closed.\n");
    long_string_client_done = true;
  }
}

int long_string_client(pid_t server_pid) {
  long_string_client_done = false;

  // Sleep for 1ms to give the server time to start.
  usleep(1000);

  char address[256];
  snprintf(address, 256, "tcp://127.0.0.1:%d", tcp_port);

  msg_connect(address, msg_no_context, long_string_client_update);
  int timeout_in_ms = 10;
  while (!long_string_client_done) {
    msg_runloop(timeout_in_ms);

    // Check to see if the server process ended before we expected it to.
    int status;
    if (!long_string_client_done && waitpid(server_pid, &status, WNOHANG)) {
      test_failed("Client: Server process ended before client expected.");
    }
  }

  return test_success;
}

int long_string_test() {
  init_long_string_if_needed();

  pid_t child_pid = fork();
  if (child_pid == -1) test_failed("fork: %s\n", strerror(errno));

  if (child_pid == 0) {
    // Child process.
    exit(long_string_server());
  } else {
    // Parent process.
    test_printf("Client pid=%d  server pid=%d\n", getpid(), child_pid);
    int client_failed = long_string_client(child_pid);
    int server_status;
    wait(&server_status);
    int server_failed = WEXITSTATUS(server_status);

    // Help check for memory leaks.
    test_that(net_allocs_for_class(0) == 0);

    test_printf("Test: client_failed=%d server_failed=%d.\n", client_failed, server_failed);

    return client_failed || server_failed;
  }
  return test_success;
}


///////////////////////////////////////////////////////////////////////////////
// basic server

int server_done;
int server_event_num;

void server_update(msg_Conn *conn, msg_Event event, msg_Data data) {

  // We expect to hear events in this order.
  int expected_events[] = {
    msg_listening, msg_connection_ready, msg_message,
    msg_request, msg_connection_closed};

  test_printf("Server: Received event %s\n", event_names[event]);
  if (event == msg_error) test_printf("Server: Error: %s\n", msg_as_str(data));
  test_that(server_event_num < array_size(expected_events));
  test_that(event == expected_events[server_event_num]);

  if (event == msg_message) {
    test_printf("Server: Message: Echoing a message back to %s:%d.\n",
        msg_ip_str(conn), conn->remote_port);
    test_printf("Server: The message is '%s'.\n", msg_as_str(data));
    test_that(strcmp(msg_as_str(data), "hello msgbox!") == 0);
    msg_send(conn, data);
  }

  if (event == msg_request) {
    test_printf("Server: Request: Sending a reply back to %s:%d.\n",
        msg_ip_str(conn), conn->remote_port);
    test_printf("Server: The message is '%s'.\n", msg_as_str(data));
    test_str_eq(msg_as_str(data), "request string");

    msg_Data data = msg_new_data("reply string");
    msg_send(conn, data);
    msg_delete_data(data);
  }

  if (event == msg_connection_closed) {
    test_printf("Server: Connection closed.\n");
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

  msg_listen(address, msg_no_context, server_update);
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
    msg_connection_ready, msg_message, msg_reply, msg_connection_closed};

  test_printf("Client: Received event %s\n", event_names[event]);
  if (event == msg_error) test_printf("Client: Error: %s\n", msg_as_str(data));
  test_that(client_event_num < array_size(expected_events));
  test_that(event == expected_events[client_event_num]);

  if (event == msg_connection_ready) {
    msg_Data data = msg_new_data("hello msgbox!");
    msg_send(conn, data);
    msg_delete_data(data);
  }

  if (event == msg_message) {
    test_printf("Client: Message from %s:%d.\n", msg_ip_str(conn), conn->remote_port);
    test_printf("Client: The message is '%s'.\n", msg_as_str(data));
    test_str_eq(msg_as_str(data), "hello msgbox!");

    msg_Data data = msg_new_data("request string");
    msg_get(conn, data, msg_no_context);
    msg_delete_data(data);
  }

  if (event == msg_reply) {
    test_printf("Client: Message from %s:%d.\n", msg_ip_str(conn), conn->remote_port);
    test_printf("Client: The message is '%s'.\n", msg_as_str(data));
    test_str_eq(msg_as_str(data), "reply string");

    msg_disconnect(conn);
  }

  if (event == msg_connection_closed) {
    test_printf("Client: Connection closed.\n");
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

  msg_connect(address, msg_no_context, client_update);
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
  run_tests(udp_test, tcp_test, long_string_test);
  return end_all_tests();
}
