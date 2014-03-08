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

///////////////////////////////////////////////////////////////////////////////
// udp server

int server_done = 0;

void udp_server_update(msg_Conn *conn, msg_Event event, msg_Data data) {
  static int event_num = 1;
  // We expect to hear events in this order:
  // msg_Listening, msg_Message
  switch(event) {
    case msg_Listening:
      test_printf("%s: Listening.\n", __func__);
      test_that(event_num == 1);
      break;
    case msg_Message:
      test_printf("Message: Echoing a message back to %s:%d.\n",
          conn->remote_address, conn->remote_port);
      test_printf("The message is '%s'.\n", msg_as_str(data));
      test_that(strcmp(msg_as_str(data), "hello msgbox!") == 0);
      test_that(event_num == 2);
      msg_send(conn, data);
      server_done = 1;
      break;
    default:
      test_failed();
  }
  event_num++;
}

int udp_server() {

  msg_listen("udp://*:1234", msg_no_context, udp_server_update);
  while (!server_done) msg_runloop();

  return test_success;
}

///////////////////////////////////////////////////////////////////////////////
// udp client

int client_done = 0;

// TODO test message values here and above.

void udp_client_update(msg_Conn *conn, msg_Event event, msg_Data data) {
  static int event_num = 1;
  // We expect to hear events in this order:
  // msg_ConnectionReady, msg_Message
  switch(event) {
    case msg_ConnectionReady:
      test_printf("Connection ready with %s.\n", conn->remote_address);
      test_that(event_num == 1);
      msg_send(conn, msg_new_data("hello msgbox!"));
      break;
    case msg_Message:
      test_printf("Message from %s:%d.\n", conn->remote_address, conn->remote_port);
      test_printf("The message is '%s'.\n", msg_as_str(data));
      test_that(event_num == 2);
      test_that(strcmp(msg_as_str(data), "hello msgbox!") == 0);
      client_done = 1;
      break;
    default:
      test_failed();
  }
  event_num++;
}

int udp_client() {
  // Sleep for 1ms to give the server time to start.
  usleep(1000);

  msg_connect("udp://127.0.0.1:1234", msg_no_context, udp_client_update);
  while (!client_done) msg_runloop();

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
    // Parent process continues.
    int client_failed = udp_client();
    int server_failed;
    wait(&server_failed);
    return client_failed || server_failed;
  }
}

int main(int argc, char **argv) {
  set_verbose(0);  // Turn this on to help debug tests.
  start_all_tests(argv[0]);
  run_tests(udp_test);
  return end_all_tests();
}
