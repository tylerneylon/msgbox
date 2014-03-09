#include "msgbox.h"

#include "CArray.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

// Possible future features
//
// * By default, all data objects sent to callbacks are dynamic and owned by us.
//   We free them when the callback returns. Add a function to make it possible for
//   a callback to retain ownership of a data buffer so that buffer doesn't have to
//   be copied by the user. This doesn't have to be in v1.
//

// Make it possible to turn on and off the assert macro based
// on the DEBUG preprocessor definition.

// TODO TEMP
#define DEBUG

#ifdef DEBUG
#include <assert.h>
#else
#define assert(x)
#endif

#define true 1
#define false 0

// This is a possible return value for functions that return
// an error string when an error is encountered.
#define no_error NULL

#define recv_buffer_len 32768

#define sock_in_size sizeof(struct sockaddr_in)

// TODO remove; these are for temporary testing
#include <stdio.h>

typedef struct {
  msg_Conn *conn;
  msg_Event event;
  msg_Data data;
  void *free_after_call;
} PendingCallback;

// TODO after tcp is added and most functionality is done
// Solidify the rules for how we call free after a callback is called.
// Right now they're something like this:
// * free(free_after_call) if free_after_call != NULL
// * free data->bytes if it's a tcp message and !NULL; (? revisit)
// * free data->bytes - header_len if it's a udp message and !NULL.
//
// For connection closes (closed or lost), the msg_Conn object will have already
// been removed from conns (and the socket removed from poll_fds), and the conn
// object will be a copy of the original set as free_after_call.

static int init_done = false;
static CArray immediate_callbacks = NULL;

// These arrays have corresponding elements at the same index.
static CArray poll_fds = NULL;
static CArray conns = NULL;

// Possible values for message_type
enum {
  one_way,
  request,
  reply,
  heartbeat,
  close
};

typedef struct {
  uint16_t message_type;
  uint16_t num_packets;
  uint16_t packet_id;
  uint16_t reply_id;
} Header;

#define header_len 8

// Header values for the reply_id field.
static uint16_t next_reply_id = 1;
static const uint16_t one_way_msg = 1;
static const uint16_t is_reply_mask = 1 << 15;
static const uint16_t max_reply_id = (1 << 15) - 1;  // = bits:01..1 with 15 ones.

///////////////////////////////////////////////////////////////////////////////
//  Internal functions.

void CArrayRemoveLast(CArray array) {
  CArrayRemoveElement(array, CArrayElement(array, array->count - 1));
}

void remove_last_polling_conn() {
  CArrayRemoveLast(conns);
  CArrayRemoveLast(poll_fds);
}

static void init_if_needed() {
  if (init_done) return;

  immediate_callbacks = CArrayNew(16, sizeof(PendingCallback));
  poll_fds = CArrayNew(8, sizeof(struct pollfd));
  conns = CArrayNew(8, sizeof(msg_Conn *));

  init_done = true;
}

static void send_callback(msg_Conn *conn, msg_Event event, msg_Data data) {
  //printf("%s: event=%d.\n", __func__, event);
  PendingCallback pending_callback = {conn, event, data, NULL};
  CArrayAddElement(immediate_callbacks, pending_callback);
}

static void send_callback_error(msg_Conn *conn, const char *msg, void *to_free) {
  //printf("%s: %s.\n", __func__, msg);
  PendingCallback pending_callback = {conn, msg_error, msg_new_data(msg), to_free};
  CArrayAddElement(immediate_callbacks, pending_callback);
}

static void send_callback_os_error(msg_Conn *conn, const char *msg, void *to_free) {
  //printf("%s: %s.\n", __func__, msg);
  static char err_msg[1024];
  snprintf(err_msg, 1024, "%s: %s", msg, strerror(errno));
  send_callback_error(conn, err_msg, to_free);
}

// Returns no_error (NULL) on success; otherwise an error string.
static char *set_sockaddr_for_conn(struct sockaddr_in *sockaddr, msg_Conn *conn) {
  memset(sockaddr, 0, sock_in_size);
  sockaddr->sin_family = AF_INET;
  sockaddr->sin_port = htons(conn->remote_port);
  char *ip_str = conn->remote_ip;
  if (strcmp(ip_str, "*") == 0) {
    sockaddr->sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (inet_aton(ip_str, &sockaddr->sin_addr) == 0) {
    static char err_msg[1024];
    snprintf(err_msg, 1024, "Couldn't parse ip string '%s'.", ip_str);
    return err_msg;
  }
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

  // Parse the ip_str.
  // The syntax within ip_str is checked by inet_aton for us.
  const char *ip_start = address + prefix_len;
  char *colon = strchr(ip_start, ':');
  if (colon == NULL) {
    snprintf(err_msg, 1024, "Can't parse address '%s'; missing colon after ip", address);
    return err_msg;
  }
  size_t ip_len = colon - ip_start;
  if (ip_len > 15 || ip_len < 1) {
    snprintf(err_msg, 1024,
        "Failing because ip length=%zd; expected to be 1-15 (in addresss '%s')",
        ip_len, address);
    return err_msg;
  }
  char *ip_str_end = stpncpy(conn->remote_ip, ip_start, ip_len);
  *ip_str_end = '\0';

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

static void set_header(msg_Data data, uint16_t msg_type, uint16_t num_packets,
    uint16_t packet_id, uint16_t reply_id) {
  Header *header = (Header *)(data.bytes - header_len);
  *header = (Header) {
    .message_type = htons(msg_type), .num_packets = htons(num_packets),
    .packet_id = htons(packet_id), .reply_id = htons(reply_id)};
}

// Reads the header of a udp packet.
// Returns true on success; false on failure.
static int read_header(int sock, msg_Conn *conn, Header *header) {
  uint16_t *buffer = (uint16_t *)header;
  ssize_t bytes_recvd = recv(sock, buffer, header_len, MSG_PEEK);
  if (bytes_recvd == -1) {
    send_callback_os_error(conn, "recv", NULL);
    return false;
  }
  // Convert each field from network to host byte ordering.
  static const size_t num_shorts = header_len / sizeof(uint16_t);
  for (int i = 0; i < num_shorts; ++i) buffer[i] = ntohs(buffer[i]);
  conn->reply_id = header->reply_id;
  return true;
}

static void read_from_socket(int sock, msg_Conn *conn) {
  //printf("%s(%d, %p)\n", __func__, sock, conn);
  Header header;
  if (!read_header(sock, conn, &header)) return;
  //printf("reply_id(raw)=%d num_packets=%d packet_id=%d.\n", conn->reply_id, num_packets, packet_id);

  msg_Event event;
  switch (header.message_type) {
    case one_way:
      event = msg_message;
      break;
    case request:
      event = msg_request;
      break;
    case reply:
      event = msg_reply;
      break;
    default:
      assert(0);
  }
  
  if (header.num_packets == 1) {
    char *buffer = malloc(recv_buffer_len);
    int default_options = 0;
    struct sockaddr_in remote_sockaddr;
    socklen_t remote_sockaddr_size = sock_in_size;
    ssize_t bytes_recvd = recvfrom(sock, buffer, recv_buffer_len, default_options,
        (struct sockaddr *)&remote_sockaddr, &remote_sockaddr_size);

    if (bytes_recvd == -1) return send_callback_os_error(conn, "recvfrom", NULL);

    msg_Data data = {bytes_recvd - header_len, buffer + header_len};
    strcpy(conn->remote_ip, inet_ntoa(remote_sockaddr.sin_addr));
    conn->remote_port = ntohs(remote_sockaddr.sin_port);

    send_callback(conn, event, data);

  } else {
    // TODO Handle the multi-packet case.
    assert(0);
  }
}

static msg_Conn *new_connection(void *conn_context, msg_Callback callback) {
  msg_Conn *conn = (msg_Conn *)malloc(sizeof(msg_Conn));
  memset(conn, 0, sizeof(msg_Conn));
  conn->conn_context = conn_context;
  conn->callback = callback;
  return conn;
}

// Sets up sockaddr based on address. If an error occurs, the error callback
// is scheduled.  Returns true on success.
// conn and its polling fd are added the polling conns arrays on success.
static int setup_sockaddr(struct sockaddr_in *sockaddr, const char *address, msg_Conn *conn) {
  int use_default_protocol = 0;
  int sock = socket(AF_INET, SOCK_DGRAM, use_default_protocol);
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

  const char *err_msg = parse_address_str(address, conn);
  if (err_msg != no_error) {
    remove_last_polling_conn();
    send_callback_error(conn, err_msg, conn);
    return false;
  }

  // Initialize the sockaddr_in struct.
  memset(sockaddr, 0, sock_in_size);
  sockaddr->sin_family = AF_INET;
  sockaddr->sin_port = htons(conn->remote_port);
  if (strcmp(conn->remote_ip, "*") == 0) {
    sockaddr->sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (inet_aton(conn->remote_ip, &sockaddr->sin_addr) == 0) {
    remove_last_polling_conn();
    static char err_msg[1024];
    snprintf(err_msg, 1024, "Couldn't parse ip string '%s'.", conn->remote_ip);
    send_callback_error(conn, err_msg, conn);
    return false;
  }

  return true;
}

typedef int (*SocketOpener)(int, const struct sockaddr *, socklen_t);

static void open_socket(const char *address, void *conn_context,
    msg_Callback callback, int for_listening) {
  init_if_needed();

  msg_Conn *conn = new_connection(conn_context, callback);
  conn->for_listening = for_listening;
  struct sockaddr_in *sockaddr = alloca(sock_in_size);
  if (!setup_sockaddr(sockaddr, address, conn)) return;  // There was an error.

  SocketOpener sys_open_sock = for_listening ? bind : connect;
  int ret_val = sys_open_sock(conn->socket, (struct sockaddr *)sockaddr, sock_in_size);
  if (ret_val == -1) {
    send_callback_os_error(conn, "bind", conn);
    return remove_last_polling_conn();
  }

  msg_Event event = for_listening ? msg_listening : msg_connection_ready;
  send_callback(conn, event, msg_no_data);
}


///////////////////////////////////////////////////////////////////////////////
//  Public functions.

void msg_runloop(int timeout_in_ms) {
  nfds_t num_fds = poll_fds->count;

  // TEMP
  static int num_prints = 0;
  if (num_prints < 3) {
    //printf("About to call poll.\n");
    //printf("num_fds=%d\n", num_fds);
    if (num_fds > 0) {
      struct pollfd *pfd = (struct pollfd *)poll_fds->elements;
      //printf("first fd=%d\n", pfd->fd);
    }
    num_prints++;
  }

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
      if (!(poll_fd->revents & POLLIN)) continue;
      int index = CArrayIndexOf(poll_fds, poll_fd);
      msg_Conn *conn = CArrayElementOfType(conns, index, msg_Conn *);
      read_from_socket(poll_fd->fd, conn);
    }
  }

  // Save the state of pending callbacks so that users can add new callbacks
  // from within their callbacks.
  CArray saved_immediate_callbacks = immediate_callbacks;
  immediate_callbacks = CArrayNew(16, sizeof(PendingCallback));

  CArrayFor(PendingCallback *, call, saved_immediate_callbacks) {
    call->conn->callback(call->conn, call->event, call->data);
    if (call->free_after_call) free(call->free_after_call);
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

void msg_disconnect(msg_Conn *conn) {
}

void msg_send(msg_Conn *conn, msg_Data data) {
  //printf("%s: '%s'\n", __func__, msg_as_str(data));

  // Set up the header.
  int num_packets = 1, packet_id = 0, reply_id = 0;
  set_header(data, one_way, num_packets, packet_id, reply_id);
  // TODO Be able to handle multi-packet data.

  int default_options = 0;
  //printf("protocol_type=%d for_listening=%d.\n", conn->protocol_type, conn->for_listening);
  if (conn->protocol_type == msg_udp && conn->for_listening) {
    //printf("Using sendto.\n");
    struct sockaddr_in sockaddr;
    char *err_msg = set_sockaddr_for_conn(&sockaddr, conn);
    if (err_msg) return send_callback_error(conn, err_msg, NULL);
    sendto(conn->socket, data.bytes - header_len, data.num_bytes + header_len, default_options,
        (struct sockaddr *)&sockaddr, sock_in_size);
  } else {
    //printf("Using send.\n");
    send(conn->socket, data.bytes - header_len, data.num_bytes + header_len, default_options);
  }
}

void msg_get(msg_Conn *conn, msg_Data data, void *reply_context) {
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
  msg_Data data = {num_bytes, malloc(num_bytes + header_len)};
  data.bytes += header_len;
  return data;
}

void msg_delete_data(msg_Data data) {
  free(data.bytes - header_len);
}

char *msg_error_str(msg_Data data) {
  return msg_as_str(data);
}

void *msg_no_context = NULL;

msg_Data msg_no_data = {0, NULL};

const int msg_tcp = SOCK_STREAM;
const int msg_udp = SOCK_DGRAM;

