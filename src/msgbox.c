#include "msgbox.h"

#include "CArray.h"
#include "CList.h"
#include "CMap.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

// Possible future features
//
// * By default, all data objects sent to callbacks are dynamic and owned by us.
//   We free them when the callback returns. Add a function to make it possible for
//   a callback to retain ownership of a data buffer so that buffer doesn't have to
//   be copied by the user. This doesn't have to be in v1.
//

// Make it possible to turn on and off the assert macro based
// on the DEBUG preprocessor definition.

#ifdef DEBUG
#include "memprofile.h"
#include <assert.h>
#else
#define assert(x)
#endif

#define true 1
#define false 0

// This is a possible return value for functions that return
// an error string when an error is encountered.
#define no_error NULL

#define sock_in_size sizeof(struct sockaddr_in)

// TODO remove; these are for temporary testing
#include <stdio.h>

static msg_Data msg_no_data = { .num_bytes = 0, .bytes = NULL};

typedef struct {
  msg_Conn *conn;
  msg_Event event;
  msg_Data data;
  void *to_free;
} PendingCall;

// TODO
// **. after tcp is added and most functionality is done
// Solidify the rules for how we call free after a callback is called.
// Right now they're something like this:
// * free(to_free) if to_free != NULL
// * free data->bytes if it's a tcp message and !NULL; (? revisit)
// * free data->bytes - header_len if it's a udp message and !NULL.
//
// For connection closes (closed or lost), the msg_Conn object will have already
// been removed from conns (and the socket removed from poll_fds), and the conn
// object will be a copy of the original set as to_free.
//
// **. Try to eliminate unnamed function call parameter.
//
// **. Make the address info in msg_Conn an official Address object.
//
// **. Add timeouts for msg_get.
//
// **. When we receive a request, ensure that next_reply_id is above its reply_id.
//
// **. Avoid blocking in send_all; instead cache data that needs to be sent and
//     dole it out from the run loop. In practice, I can track how often this ends up
//     as a problem (maybe how often send returns 0) and use that to prioritize things.
//
// **. Check for error return values from send/sendto in all cases. That check is missing
//     at very least in msg_send.
//
// **. Encapsulate all references to header_len.
//
// **. Clean up use of num_bytes in the header for udp, as it is not used consistently now.
//
// **. Encapsulate out all the msg_Data handling. There's too much coupling with it now.
//

static CArray immediate_callbacks = NULL;

// These arrays have corresponding elements at the same index.
static CArray poll_fds = NULL;
static CArray conns = NULL;

// Possible values for message_type
enum {
  msg_type_one_way,
  msg_type_request,
  msg_type_reply,
  msg_type_heartbeat,
  msg_type_close
};

typedef struct {
  uint16_t message_type;
  uint16_t num_bytes;
  uint16_t packet_id;
  uint16_t reply_id;
} Header;

#define header_len 8
// TODO Drop recv_buffer_len if we don't use it.
#define recv_buffer_len (32768 - header_len)


// Header values for the reply_id field.
static uint16_t next_reply_id = 1;
static const uint16_t one_way_msg = 1;
static const uint16_t is_reply_mask = 1 << 15;
static const uint16_t max_reply_id = (1 << 15) - 1;  // = bits:01..1 with 15 ones.

///////////////////////////////////////////////////////////////////////////////
//  Connection status map.

typedef struct {
  uint32_t ip;  // Stored in network byte-order.
  uint16_t port;  // Stored in host byte-order.
  uint16_t protocol_type;
} Address;

Address *address_of_conn(msg_Conn *conn) {
  return (Address *)(&conn->remote_ip);
}

char *address_as_str(Address *address) {
  struct in_addr in;
  in.s_addr = address->ip;
  static char address_str[32];
  char *protocol = address->protocol_type == msg_udp ? "udp" : "tcp";
  snprintf(address_str, 32, "%s://%s:%d", protocol, inet_ntoa(in), address->port);
  return address_str;
}

int address_hash(void *address) {
  char *bytes = (char *)address;
  int hash = 0;
  for (int i = 0; i < sizeof(Address); ++i) {
    hash *= 234;
    hash += bytes[i];
  }
  return hash;
}

int address_eq(void *addr1, void *addr2) {
  return memcmp(addr1, addr2, sizeof(Address)) == 0;
}

int reply_id_hash(void *reply_id) {
  return (int)reply_id;
}

int reply_id_eq(void *reply_id1, void *reply_id2) {
  uint16_t id1 = (uint16_t)reply_id1;
  uint16_t id2 = (uint16_t)reply_id2;
  return id1 == id2;
}

typedef struct {
  double last_seen_at;
  CMap reply_contexts;  // Map reply_id -> reply_context.
  uint16_t next_reply_id;

  // These overlap; waiting_buffer is a suffix of total_buffer.
  msg_Data total_buffer;
  msg_Data waiting_buffer;
} ConnStatus;

ConnStatus *new_conn_status(double now) {
  ConnStatus *status = malloc(sizeof(ConnStatus));
  status->last_seen_at = now;
  status->reply_contexts = CMapNew(reply_id_hash, reply_id_eq);
  status->next_reply_id = 1;
  return status;
}

static void new_conn_status_buffer(ConnStatus *status, Header *header) {
  printf("*** Allocating %zd bytes for incoming tcp message.\n", header->num_bytes);  // DEBUG
  status->total_buffer = status->waiting_buffer = msg_new_data_space(header->num_bytes);
  memcpy(status->total_buffer.bytes - header_len, header, header_len);
}

static void delete_conn_status_buffer(ConnStatus *status) {
  printf("*** Freeing %zd bytes for incoming tcp message.\n", status->total_buffer.num_bytes);  // DEBUG
  msg_delete_data(status->total_buffer);
  status->total_buffer = status->waiting_buffer = (msg_Data) { .num_bytes = 0, .bytes = NULL };
}

static void delete_conn_status(void *status_v_ptr) {
  ConnStatus *status = (ConnStatus *)status_v_ptr;
  // This should be empty since we need to give the user a chance to free all contexts.
  assert(status->reply_contexts->count == 0);
  CMapDelete(status->reply_contexts);
}

// This maps Address -> ConnStatus.
// The actual keys & values are pointers to those types,
// and the releasers free them.
// TODO Once out_beats is added, let out_beats own the ConnStatus objects.
static CMap conn_status = NULL;

// Returns NULL if the given remote address has no associated status.
ConnStatus *status_of_conn(msg_Conn *conn) {
  Address *address = (Address *)(&conn->remote_ip);
  KeyValuePair *pair = CMapFind(conn_status, address);
  return pair ? (ConnStatus *)pair->value : NULL;
}

///////////////////////////////////////////////////////////////////////////////
//  Internal functions.

// DEBUG
static void print_bytes(char *bytes, size_t num_to_print) {
  printf("bytes (%zd) :", num_to_print);
  for (int i = 0; i < num_to_print; ++i) {
    printf(" 0x%02X", bytes[i]);
  }
  printf("\n");
}

// Returns -1 on error; 0 on success, similar to a system call.
static int send_all(int socket, msg_Data data) {
  data.bytes -= header_len;
  data.num_bytes += header_len;

  // DEBUG
  size_t orig_size = data.num_bytes;
  char *orig_bytes = data.bytes;

  while (data.num_bytes > 0) {
    int default_send_options = 0;
    int just_sent = send(socket, data.bytes, data.num_bytes, default_send_options);
    if (just_sent == -1) return -1;
    data.bytes += just_sent;
    data.num_bytes -= just_sent;
  }

  if (0) {
    printf("Just sent over the ");
    print_bytes(orig_bytes, orig_size);
  }

  return 0;
}

static void CArrayRemoveLast(CArray array) {
  CArrayRemoveElement(array, CArrayElement(array, array->count - 1));
}

static void remove_last_polling_conn() {
  CArrayRemoveLast(conns);
  CArrayRemoveLast(poll_fds);
}

static msg_Conn *new_connection(void *conn_context, msg_Callback callback) {
  msg_Conn *conn = malloc(sizeof(msg_Conn));
  memset(conn, 0, sizeof(msg_Conn));
  conn->conn_context = conn_context;
  conn->callback = callback;
  return conn;
}

static void init_if_needed() {
  static int init_done = false;
  if (init_done) return;

  immediate_callbacks = CArrayNew(16, sizeof(PendingCall));
  poll_fds = CArrayNew(8, sizeof(struct pollfd));
  conns = CArrayNew(8, sizeof(msg_Conn *));

  conn_status = CMapNew(address_hash, address_eq);
  conn_status->keyReleaser = free;
  conn_status->valueReleaser = delete_conn_status;

  init_done = true;
}

static void send_callback(msg_Conn *conn, msg_Event event, msg_Data data, void *to_free) {
  PendingCall pending_callback = {.conn = conn, .event = event, .data = data, .to_free = to_free};
  CArrayAddElement(immediate_callbacks, pending_callback);
}

static void send_callback_error(msg_Conn *conn, const char *msg, void *to_free) {
  send_callback(conn, msg_error, msg_new_data(msg), to_free);
}

static void send_callback_os_error(msg_Conn *conn, const char *msg, void *to_free) {
  static char err_msg[1024];
  snprintf(err_msg, 1024, "%s: %s", msg, strerror(errno));
  send_callback_error(conn, err_msg, to_free);
}

// Returns no_error (NULL) on success; otherwise an error string.
static char *set_sockaddr_for_conn(struct sockaddr_in *sockaddr, msg_Conn *conn) {
  memset(sockaddr, 0, sock_in_size);
  sockaddr->sin_family = AF_INET;
  sockaddr->sin_port = htons(conn->remote_port);
  sockaddr->sin_addr.s_addr = conn->remote_ip;
  return no_error;
}

// Returns no_error (NULL) on success, and sets the protocol_type,
// remote_ip, and remote_port of the given conn.
// Returns an error string if there was an error.
static const char *parse_address_str(const char *address, msg_Conn *conn) {
  assert(conn != NULL);
  static char err_msg[1024];

  // TODO once v1 functionality is done, see if I can
  // encapsulate the error pattern into a one-liner; eg with a macro.

  // Parse the protocol type; either tcp or udp.
  const char *tcp_prefix = "tcp://";
  const char *udp_prefix = "udp://";
  size_t prefix_len = 6;
  if (strncmp(address, tcp_prefix, prefix_len) == 0) {
    conn->protocol_type = SOCK_STREAM;
  } else if (strncmp(address, udp_prefix, prefix_len) == 0) {
    conn->protocol_type = SOCK_DGRAM;
  } else {
    // address has no recognizable prefix.
    snprintf(err_msg, 1024, "Failing due to unrecognized prefix: %s", address);
    return err_msg;
  }

  // Parse the ip substring in three steps.
  // 1. Find start and end of the ip substring.
  const char *ip_start = address + prefix_len;
  char *colon = strchr(ip_start, ':');
  if (colon == NULL) {
    snprintf(err_msg, 1024, "Can't parse address '%s'; missing colon after ip", address);
    return err_msg;
  }

  // 2. Check substring length and copy over so we can hand inet_aton a null-terminated version.
  size_t ip_len = colon - ip_start;
  if (ip_len > 15 || ip_len < 1) {
    snprintf(err_msg, 1024,
        "Failing because ip length=%zd; expected to be 1-15 (in addresss '%s')",
        ip_len, address);
    return err_msg;
  }
  char ip_str[16];
  char *ip_str_end = stpncpy(ip_str, ip_start, ip_len);
  *ip_str_end = '\0';

  // 3. Let inet_aton handle the actual parsing.
  if (strcmp(ip_str, "*") == 0) {
    conn->remote_ip = INADDR_ANY;
  } else {
    struct in_addr ip;
    if (inet_aton(ip_str, &ip) == 0) {
      snprintf(err_msg, 1024, "Couldn't parse ip string '%s'.", ip_str);
      return err_msg;
    }
    conn->remote_ip = ip.s_addr;
  }

  // Parse the port.
  int base_ten = 10;
  char *end_ptr = NULL;
  if (*(colon + 1) == '\0') {
    snprintf(err_msg, 1024, "Empty port string in address '%s'", address);
    return err_msg;
  }
  conn->remote_port = (int)strtol(colon + 1, &end_ptr, base_ten);
  if (*end_ptr != '\0') {
    snprintf(err_msg, 1024, "Invalid port string in address '%s'", address);
    return err_msg;
  }

  return no_error;
}

static void set_header(msg_Data data, uint16_t msg_type, uint16_t num_bytes,
    uint16_t packet_id, uint16_t reply_id) {
  Header *header = (Header *)(data.bytes - header_len);
  *header = (Header) {
    .message_type = htons(msg_type), .num_bytes = htons(num_bytes),
    .packet_id = htons(packet_id), .reply_id = htons(reply_id)};
}

// Reads the header of a udp packet.
// Returns true on success; false on failure.
static int read_header(int sock, msg_Conn *conn, Header *header) {
  uint16_t *buffer = (uint16_t *)header;
  int options = conn->protocol_type == msg_udp ? MSG_PEEK : 0;
  ssize_t bytes_recvd = recv(sock, buffer, header_len, options);
  if (bytes_recvd == -1) {
    send_callback_os_error(conn, "recv", NULL);
    return false;
  }

  // TODO Handle the case of receiving 0 bytes = remote connection closed.
  if (bytes_recvd == 0) {
    printf("\nRemote connection closed!\n");
    exit(22);  // random status for now
  }

  // Convert each field from network to host byte ordering.
  static const size_t num_shorts = header_len / sizeof(uint16_t);
  for (int i = 0; i < num_shorts; ++i) buffer[i] = ntohs(buffer[i]);
  conn->reply_id = header->reply_id;

  if (0) {
    printf("%s called; header has ", __func__);
    print_bytes((char *)header, sizeof(Header));
  }

  return true;
}

static ConnStatus *remote_address_seen(msg_Conn *conn) {
  Address *address = address_of_conn(conn);
  KeyValuePair *pair;
  ConnStatus *status;
  if ((pair = CMapFind(conn_status, address))) {
    // TODO Update the timing data for this remote address.
    status = (ConnStatus *)pair->value;
  } else {
    // It's a new remote address; conn_status takes ownership of address.
    status = new_conn_status(0.0 /* TODO set to now */);
    address = malloc(sizeof(Address));
    *address = *address_of_conn(conn);
    CMapSet(conn_status, address, status);
    send_callback(conn, msg_connection_ready, msg_no_data, NULL);
  }

  return status;
}

// Drops the conn from conn_status and sends msg_connection_closed.
static void local_disconnect(msg_Conn *conn) {
  Address *address = (Address *)(&conn->remote_ip);
  CMapUnset(conn_status, address);
  send_callback(conn, msg_connection_closed, msg_no_data, conn);

  if (!conn->for_listening) close(conn->socket);
}

// Returns true when the entire message is received;
// returns false when more data remains but no error occurred; and
// returns -1 when there was an error - the caller must respond to it.
static int continue_recv(int sock, ConnStatus *status) {
  msg_Data *buffer = &status->waiting_buffer;
  int default_options = 0;
  ssize_t bytes_in = recv(sock, buffer->bytes, buffer->num_bytes, default_options);
  if (bytes_in == -1) return -1;
  buffer->num_bytes -= bytes_in;
  return buffer->num_bytes == 0;
}

// TODO Make this function shorter or break it up.
static void read_from_socket(int sock, msg_Conn *conn) {
  ConnStatus *status = NULL;
  Header *header = NULL;
  msg_Data data;

  // Read in any tcp data.
  if (conn->protocol_type == msg_tcp) {

    if (conn->for_listening) {

      // Accept a new incoming connection.
      struct sockaddr_in remote_addr;
      socklen_t addr_len = sizeof(remote_addr);
      int new_sock = accept(conn->socket, (struct sockaddr *)&remote_addr, &addr_len);
      if (new_sock == -1) return send_callback_os_error(conn, "accept", NULL);

      msg_Conn *new_conn = new_connection(conn->conn_context, conn->callback);
      new_conn->socket = new_sock;
      new_conn->remote_ip = remote_addr.sin_addr.s_addr;
      new_conn->remote_port = ntohs(remote_addr.sin_port);
      new_conn->protocol_type = conn->protocol_type;
      CArrayAddElement(conns, new_conn);

      struct pollfd *new_poll_fd = (struct pollfd *)CArrayNewElement(poll_fds);
      new_poll_fd->fd = new_sock;
      new_poll_fd->events = POLLIN;

      remote_address_seen(new_conn);  // Sets up a ConnStatus and sends msg_connection_ready.
      return;
    }

    status = remote_address_seen(conn);
    if (status->waiting_buffer.num_bytes == 0) {

      // Begin a new recv.
      header = alloca(sizeof(Header));
      if (!read_header(sock, conn, header)) return;
      new_conn_status_buffer(status, header);
    } else {

      // Load header from the buffer we'll continue.
      header = (Header *)(status->total_buffer.bytes - header_len);
    }
    int ret_val = continue_recv(conn->socket, status);
    if (ret_val == -1) {
      send_callback_os_error(conn, "recv", NULL);
      return delete_conn_status_buffer(status);
    }
    if (ret_val == false) return;  // It will finish later.
    data = status->total_buffer;

    if (0) {
      printf("After continue_recv, data has ");
      print_bytes(data.bytes, data.num_bytes);
    }

    status->total_buffer = status->waiting_buffer = (msg_Data) { .num_bytes = 0, .bytes = NULL };

  } else {

    // New udp message: read the header.
    header = alloca(sizeof(Header));
    if (!read_header(sock, conn, header)) return;
  }

  if (0) {
    // TODO Remove. Debug code.
    char *msg_type_str[] = {
      "msg_type_one_way",
      "msg_type_request",
      "msg_type_reply",
      "msg_type_heartbeat",
      "msg_type_close"
    };
    if (header->message_type < (sizeof(msg_type_str) / sizeof(char *))) {
      printf("Received message of type '%s'.\n", msg_type_str[header->message_type]);
    } else {
      printf("Received message of unknown type %d.\n", header->message_type);
    }
  }

  // Set up the appropriate reaction event.
  msg_Event event;
  switch (header->message_type) {
    case msg_type_one_way:
      event = msg_message;
      break;
    case msg_type_request:
      event = msg_request;
      break;
    case msg_type_reply:
      event = msg_reply;
      break;
    case msg_type_heartbeat:
      assert(0);
      break;
    case msg_type_close:  // Only sent via udp.
      return local_disconnect(conn);
  }

  // Read in any udp data.
  if (conn->protocol_type == msg_udp) {
    data = msg_new_data_space(header->num_bytes);
    struct sockaddr_in remote_sockaddr;
    socklen_t remote_sockaddr_size = sock_in_size;
    int default_options = 0;
    ssize_t bytes_recvd = recvfrom(sock, data.bytes - header_len,
        data.num_bytes + header_len, default_options,
        (struct sockaddr *)&remote_sockaddr, &remote_sockaddr_size);

    if (bytes_recvd == -1) return send_callback_os_error(conn, "recvfrom", NULL);

    conn->remote_ip = remote_sockaddr.sin_addr.s_addr;
    conn->remote_port = ntohs(remote_sockaddr.sin_port);
    status = remote_address_seen(conn);
  }

  // Look up a reply_context if it's a reply.
  if (header->message_type == msg_type_reply) {
    KeyValuePair *pair = CMapFind(status->reply_contexts, (void *)(intptr_t)header->reply_id);
    if (pair == NULL) {
      return send_callback_error(conn, "Unrecognized reply_id", data.bytes - header_len);
    }
    conn->reply_context = pair->value;
  } else {
    conn->reply_context = NULL;
  }

  send_callback(conn, event, data, NULL);
}

// Sets up sockaddr based on address. If an error occurs, the error callback
// is scheduled.  Returns true on success.
// conn and its polling fd are added the polling conns arrays on success.
static int setup_sockaddr(struct sockaddr_in *sockaddr, const char *address, msg_Conn *conn) {
  const char *err_msg = parse_address_str(address, conn);
  if (err_msg) {
    send_callback_error(conn, err_msg, conn);
    return false;
  }

  int use_default_protocol = 0;
  int sock = socket(AF_INET, conn->protocol_type, use_default_protocol);
  if (sock == -1) {
    send_callback_os_error(conn, "socket", conn);
    return false;
  }

  // We have a real socket, so add entries to both poll_fds and conns.
  conn->socket = sock;
  CArrayAddElement(conns, conn);

  struct pollfd *poll_fd = (struct pollfd *)CArrayNewElement(poll_fds);
  poll_fd->fd = sock;
  poll_fd->events = POLLIN;

  // Initialize the sockaddr_in struct.
  memset(sockaddr, 0, sock_in_size);
  sockaddr->sin_family = AF_INET;
  sockaddr->sin_port = htons(conn->remote_port);
  sockaddr->sin_addr.s_addr = conn->remote_ip;

  return true;
}

typedef int (*SocketOpener)(int, const struct sockaddr *, socklen_t);

static void open_socket(const char *address, void *conn_context,
    msg_Callback callback, int for_listening) {
  init_if_needed();

  msg_Conn *conn = new_connection(conn_context, callback);
  conn->for_listening = for_listening;
  struct sockaddr_in *sockaddr = alloca(sock_in_size);
  if (!setup_sockaddr(sockaddr, address, conn)) return;  // Error; setup_sockaddr now owns conn.

  // Make the socket non-blocking so a connect call won't block.
  int flags = fcntl(conn->socket, F_GETFL, 0);
  if (flags == -1) {
    send_callback_os_error(conn, "fcntl", conn);
    return remove_last_polling_conn();
  }
  int ret_val = fcntl(conn->socket, F_SETFL, flags | O_NONBLOCK);
  if (ret_val == -1) {
    send_callback_os_error(conn, "fcntl", conn);
    return remove_last_polling_conn();
  }

  char *sys_call_name = for_listening ? "bind" : "connect";
  SocketOpener sys_open_sock = for_listening ? bind : connect;
  ret_val = sys_open_sock(conn->socket, (struct sockaddr *)sockaddr, sock_in_size);
  if (ret_val == -1) {
    if (!for_listening && conn->protocol_type == msg_tcp && errno == EINPROGRESS) {
      // The EINPROGRESS error is ok; in that case we'll send msg_connection_ready later.
      struct pollfd *poll_fd = CArrayElement(poll_fds, poll_fds->count - 1);
      poll_fd->events = POLLOUT;
      return;
    }
    send_callback_os_error(conn, sys_call_name, conn);
    return remove_last_polling_conn();
  }

  if (for_listening) {
    if (conn->protocol_type == msg_tcp) {
      ret_val = listen(conn->socket, SOMAXCONN);
      if (ret_val == -1) {
        send_callback_os_error(conn, "listen", conn);
        return remove_last_polling_conn();
      }
    }
    send_callback(conn, msg_listening, msg_no_data, NULL);
  } else {
    remote_address_seen(conn);  // Sends the msg_connection_ready event.
  }
}


///////////////////////////////////////////////////////////////////////////////
//  Public functions.

void msg_runloop(int timeout_in_ms) {
  nfds_t num_fds = poll_fds->count;

  int ret = poll((struct pollfd *)poll_fds->elements, num_fds, timeout_in_ms);

  if (ret == -1) {
    // It's difficult to send a standard error callback to the user here because
    // we don't know which connection (and therefore which callback pointer) to use;
    // also, critical errors should only happen here due to bugs in msgbox itself.
    if (errno == EFAULT || errno == EINVAL) {
      // These theoretically can only be my fault; still, let the user know.
      fprintf(stderr, "Internal msgbox error during 'poll' call: %s\n", strerror(errno));
    }
    // Otherwise errno is EAGAIN or EINTR, both non-critical.
  } else if (ret > 0) {
    CArrayFor(struct pollfd *, poll_fd, poll_fds) {
      if (poll_fd->revents == 0) continue;
      int index = CArrayIndexOf(poll_fds, poll_fd);
      msg_Conn *conn = CArrayElementOfType(conns, index, msg_Conn *);
      if (poll_fd->revents & POLLOUT) {
        // We only listen for this event when waiting for a tcp connect to complete.
        remote_address_seen(conn);  // Sends msg_connection_ready.
        poll_fd->events = POLLIN;
      }
      if (poll_fd->revents & POLLIN) read_from_socket(poll_fd->fd, conn);
    }
  }

  // Save the state of pending callbacks so that users can add new callbacks
  // from within their callbacks.
  CArray saved_immediate_callbacks = immediate_callbacks;
  immediate_callbacks = CArrayNew(16, sizeof(PendingCall));

  CArrayFor(PendingCall *, call, saved_immediate_callbacks) {
    call->conn->callback(call->conn, call->event, call->data);
    if (call->to_free) free(call->to_free);
  }

  // TODO free data from called callbacks

  // TODO handle any timed callbacks

  CArrayDelete(saved_immediate_callbacks);
}

void msg_listen(const char *address, void *conn_context, msg_Callback callback) {
  int for_listening = true;
  open_socket(address, conn_context, callback, for_listening);
}

void msg_connect(const char *address, void *conn_context, msg_Callback callback) {
  int for_listening = false;
  open_socket(address, conn_context, callback, for_listening);
}

void msg_unlisten(msg_Conn *conn) {
}

void msg_disconnect(msg_Conn *conn) {
  // TODO Think carefully about this and make sure it does what we want for either client or server.
  msg_Data data = msg_new_data_space(0);
  int num_bytes = 0, packet_id = 0, reply_id = 0;
  set_header(data, msg_type_close, num_bytes, packet_id, reply_id);

  int default_options = 0;
  send(conn->socket, data.bytes - header_len, data.num_bytes + header_len, default_options);
  msg_delete_data(data);

  local_disconnect(conn);
}

void msg_send(msg_Conn *conn, msg_Data data) {
  // Set up the header.
  int packet_id = 0;
  int msg_type = conn->reply_id ? msg_type_reply : msg_type_one_way;
  set_header(data, msg_type, data.num_bytes, packet_id, conn->reply_id);

  int default_options = 0;

  if (conn->protocol_type == msg_tcp) {
    int ret_val = send_all(conn->socket, data);
    if (ret_val == -1) send_callback_os_error(conn, "send", NULL);
    return;
  }

  if (conn->protocol_type == msg_udp && conn->for_listening) {
    struct sockaddr_in sockaddr;
    char *err_msg = set_sockaddr_for_conn(&sockaddr, conn);
    if (err_msg) return send_callback_error(conn, err_msg, NULL);
    ssize_t bytes_sent = sendto(conn->socket,
        data.bytes - header_len, data.num_bytes + header_len, default_options,
        (struct sockaddr *)&sockaddr, sock_in_size);
    if (bytes_sent == -1) send_callback_os_error(conn, "sendto", NULL);
  } else {
    ssize_t bytes_sent = send(conn->socket,
        data.bytes - header_len, data.num_bytes + header_len, default_options);
    if (bytes_sent == -1) send_callback_os_error(conn, "send", NULL);
  }
}

// TODO Refactor between msg_send and msg_get.
void msg_get(msg_Conn *conn, msg_Data data, void *reply_context) {
  // Look up the next reply id.
  ConnStatus *status = status_of_conn(conn);
  if (status == NULL) {
    static char err_msg[1024];
    snprintf(err_msg, 1024, "No known connection with %s", address_as_str(address_of_conn(conn)));
    return send_callback_error(conn, err_msg, NULL);
  }
  int reply_id = status->next_reply_id++;
  CMapSet(status->reply_contexts, (void *)(intptr_t)reply_id, reply_context);

  // Set up the header.
  int packet_id = 0;
  set_header(data, msg_type_request, data.num_bytes, packet_id, reply_id);

  int default_options = 0;
  if (conn->protocol_type == msg_udp && conn->for_listening) {
    struct sockaddr_in sockaddr;
    char *err_msg = set_sockaddr_for_conn(&sockaddr, conn);
    if (err_msg) return send_callback_error(conn, err_msg, NULL);
    sendto(conn->socket, data.bytes - header_len, data.num_bytes + header_len, default_options,
        (struct sockaddr *)&sockaddr, sock_in_size);
  } else {
    send(conn->socket, data.bytes - header_len, data.num_bytes + header_len, default_options);
  }
}

char *msg_as_str(msg_Data data) {
  return data.bytes;
}

msg_Data msg_new_data(const char *str) {
  // Allocate room for the string with +1 for the null terminator.
  msg_Data data = msg_new_data_space(strlen(str) + 1);
  strcpy(data.bytes, str);
  return data;
}

msg_Data msg_new_data_space(size_t num_bytes) {
  msg_Data data = {.num_bytes = num_bytes, .bytes = malloc(num_bytes + header_len)};
  data.bytes += header_len;
  return data;
}

void msg_delete_data(msg_Data data) {
  free(data.bytes - header_len);
}

char *msg_ip_str(msg_Conn *conn) {
  return inet_ntoa((struct in_addr) { .s_addr = conn->remote_ip});
}

char *msg_error_str(msg_Data data) {
  return msg_as_str(data);
}

void *msg_no_context = NULL;

const int msg_tcp = SOCK_STREAM;
const int msg_udp = SOCK_DGRAM;
