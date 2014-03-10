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

#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// TODO TEMP remove
#include <stdio.h>

#include "memprofile.h"

#define array_size(x) (sizeof(x) / sizeof(x[0]))


///////////////////////////////////////////////////////////////////////////////
// useful globals and functions

static char *event_names[] = {
  "msg_message",
  "msg_request",
  "msg_reply",
  "msg_listening",
  "msg_connection_ready",
  "msg_connection_closed",
  "msg_connection_lost",
  "msg_error"
};

///////////////////////////////////////////////////////////////////////////////
// udp server

int server_done = 0;

void udp_server_update(msg_Conn *conn, msg_Event event, msg_Data data) {
  static int event_num = 0;

  // We expect to hear events in this order:
  int expected_events[] = {
    msg_listening, msg_connection_ready, msg_message, msg_connection_closed};
  test_printf("Server: Received event %s\n", event_names[event]);
  if (event == msg_error) test_printf("Server: Error: %s\n", msg_as_str(data));
  test_that(event_num < array_size(expected_events));
  test_that(event == expected_events[event_num]);

  if (event == msg_message) {
    test_printf("Server: Message: Echoing a message back to %s:%d.\n",
        msg_ip_str(conn), conn->remote_port);
    test_printf("Server: The message is '%s'.\n", msg_as_str(data));
    test_that(strcmp(msg_as_str(data), "hello msgbox!") == 0);
    msg_send(conn, data);
  }

  if (event == msg_connection_closed) {
    test_printf("Server: Connection closed.\n");
    server_done = 1;
  }

  event_num++;
}

int udp_server() {
  msg_listen("udp://*:1234", msg_no_context, udp_server_update);
  int timeout_in_ms = 10;
  while (!server_done) msg_runloop(timeout_in_ms);

  // Sleep for 1ms as the client expects to finish before the server.
  // (Early server termination could be an error, so we check for it.)
  usleep(1000);

  return test_success;
}

///////////////////////////////////////////////////////////////////////////////
// udp client

int client_done = 0;

// TODO test message values here and above.

void udp_client_update(msg_Conn *conn, msg_Event event, msg_Data data) {
  static int event_num = 0;

  // We expect to hear events in this order:
  int expected_events[] = {msg_connection_ready, msg_message, msg_connection_closed};
  test_printf("Client: Received event %s\n", event_names[event]);
  if (event == msg_error) test_printf("Client: Error: %s\n", msg_as_str(data));
  test_that(event_num < array_size(expected_events));
  test_that(event == expected_events[event_num]);

  if (event == msg_connection_ready) {
    msg_send(conn, msg_new_data("hello msgbox!"));
  }

  if (event == msg_message) {
    test_printf("Client: Message from %s:%d.\n", msg_ip_str(conn), conn->remote_port);
    test_printf("Client: The message is '%s'.\n", msg_as_str(data));
    test_that(strcmp(msg_as_str(data), "hello msgbox!") == 0);

    msg_disconnect(conn);
  }

  if (event == msg_connection_closed) {
    test_printf("Client: Connection closed.\n");
    client_done = 1;
  }

  event_num++;
}

int udp_client(pid_t server_pid) {
  // Sleep for 1ms to give the server time to start.
  usleep(1000);

  msg_connect("udp://127.0.0.1:1234", msg_no_context, udp_client_update);
  int timeout_in_ms = 10;
  while (!client_done) {
    msg_runloop(timeout_in_ms);

    // Check to see if the server process ended before we expected it to.
    int status;
    if (!client_done && waitpid(server_pid, &status, WNOHANG)) {
      test_failed("Server process ended before client expected.");
    }
  }

  return test_success;
}

int udp_test() {
  int status;
  pid_t child_pid = fork();
  if (child_pid == -1) return test_failure;

  if (child_pid == 0) {
    // Child process.
    exit(udp_server());
  } else {
    // Parent process.
    int client_failed = udp_client(child_pid);
    int server_status;
    wait(&server_status);
    int server_failed = WEXITSTATUS(server_status);

    // TODO Check for memory leaks.
    //printmeminfo();

    test_printf("Test: client_failed=%d server_failed=%d.\n", client_failed, server_failed);

    return client_failed || server_failed;
  }
}

int main(int argc, char **argv) {
  set_verbose(0);  // Turn this on to help debug tests.
  start_all_tests(argv[0]);
  run_tests(udp_test);
  return end_all_tests();
}
