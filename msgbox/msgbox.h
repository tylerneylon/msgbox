// msgbox.h
//
// https://github.com/tylerneylon/msgbox
//
// A library to simplify working with tcp and udp messages.
//
// All calls are non-blocking.
//
// An address has the format (tcp|udp)://(<ip addr>|*):<port>.
// Examples:
//   You could listen on "tcp://*:8100".
//   You could connect to "udp://1.2.3.4:8200".
//
// See the examples directory for basic usage examples.
//

#pragma once

#include <inttypes.h>
#include <sys/types.h>

// Type definitions.

// Allocate and deallocate msg_Data using the msg_{new,delete}_data*
// functions below. That's needed since bytes points *into* a buffer with
// preamble space for headers.
typedef struct {
  size_t num_bytes;
  char *bytes;
} msg_Data;

typedef enum {
  msg_message,
  msg_request,
  msg_reply,
  msg_listening,
  msg_listening_ended,
  msg_connection_ready,
  msg_connection_closed,
  msg_connection_lost,
  msg_error
} msg_Event;

struct msg_Conn;

typedef void (*msg_Callback)(struct msg_Conn *, msg_Event, msg_Data);

typedef struct msg_Conn {
  void *conn_context;
  void *reply_context;
  msg_Callback callback;

  uint32_t remote_ip;      // Network byte-order.
  uint16_t remote_port;    // Host byte-order.
  uint16_t protocol_type;  // Valid values are msg_tcp or msg_udp.

  int socket;
  int for_listening;
  uint16_t reply_id;
  int index;
} msg_Conn;

// Event loop function; expects to be called frequently.

void msg_runloop(int timeout_in_ms);

// Calls to start or stop a client or server.

void msg_listen (const char *address, msg_Callback callback);
void msg_connect(const char *address, msg_Callback callback,
                 void *conn_context);

void msg_unlisten  (msg_Conn *conn);
void msg_disconnect(msg_Conn *conn);

// Calls to send a message.
// Call msg_get when you expect a reply; otherwise call msg_send.

void msg_send(msg_Conn *conn, msg_Data data);
void msg_get (msg_Conn *conn, msg_Data data, void *reply_context);

// Functions for working with msg_Data.

char *msg_as_str(msg_Data data);  // Assumes the underlying data is a C string.
msg_Data msg_new_data(const char *str);
msg_Data msg_new_data_space(size_t num_bytes);
void msg_delete_data(msg_Data data);

// Functions for working with msg_Conn.

char *msg_ip_str(msg_Conn *conn);
char *msg_address_str(msg_Conn *conn);

// Functions for working with errors.

char *msg_error_str(msg_Data data);

// Constants.

extern void *msg_no_context;

// Valid values for msg_Conn.protocol_type.
extern const int msg_udp;
extern const int msg_tcp;
