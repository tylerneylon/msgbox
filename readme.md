# msgbox
*A bite-sized tcp/udp library in pure C.*

This library is at an early stage and may be
a bit unstable.

## Motivation

`msgbox` is a small C library that sends messages to other applications built with `msgbox`.
It's useful for both client-server interaction or server-server communication within a cluster.

My personal motivation is my work
on a massively multiplayer online game.
I built `msgbox` to encapsulate concurrent TCP and UDP
connections, which are useful for such games.
"Dude," you might say, "dude, UDP has no connections."
Which is true. But `msgbox` gives UDP the notion of an
application-level connection, and offers a few other features:

* `msgbox` is small, efficient, and easy to learn and use.
* Always non-blocking; uses callbacks and plays well with your custom run loop.
* The interface and event cycle for `udp` and `tcp` is identical.
* Error checks are encapsulated in your callback instead of strewn
  throughout your code.
* Adds request-reply and connection semantics to UDP.
* Adds message-oriented semantics to TCP, which is stream-based.

## Server example

Here's a server that repeats back everything it hears:
```
#include "msgbox.h"

void msg_update(msg_Conn *conn, msg_Event event, msg_Data data) {
  if (event == msg_request) msg_send(conn, data);  // Repeat the same data back.
}

int main() {
  msg_listen("udp://*:2100", msg_no_context, msg_update);
  while (1) msg_runloop(10 /* timeout in ms */);
  return 0;
}
```

## Client example

Here's a client that sends a request and receives a reply:
```
#include "msgbox.h"
#include <stdio.h>

void msg_update(msg_Conn *conn, msg_Event event, msg_Data data) {
  if (event == msg_connection_ready) {
    msg_Data data = msg_new_data("hello!");
    msg_get(conn, data, msg_no_context);
    msg_delete_data(data);
  }
  if (event == msg_reply) printf("Got the reply: '%s'.\n", msg_as_str(data));
}

int main() {
  msg_connect("udp://127.0.0.1:2100", msg_no_context, msg_update);
  while (1) msg_runloop(10 /* timeout in ms */);
  return 0;
}
```

If you run the example server followed by the example client on the same
machine, the client will print out:

    Got the reply: 'hello!'.

Since you love these examples so much, I left them in infinite loops
so they will run forever.

## Documentation

The primary data flow of `msgbox` is based on callbacks.
`msgbox` provides data using a callback associated with either a
call to `msg_connect` for clients, or a call to `msg_listen` for servers.
These callbacks can happen from within `msgbox`'s runloop, which is
designed to be called repeatedly and often using the
`msg_runloop` function.

### Startup and shutdown functions

#### --- `msg_listen` ---

`void msg_listen(const char *address, void *conn_context, msg_Callback callback)`


This initiates a server at the given address, which must have the
syntax `(tcp|udp)://(ip|*):<port>`; an example is `"tcp://*:6070"`.
A `*` in the ip position tells `msg_listen` to listen on any interface, corresponding
to the system's `INADDR_ANY` value.

The `conn_context` pointer is treated as an opaque value by `msgbox`, and offers
you a way to know which listening address events occur on. This pointer will be
handed to your callback as `conn->conn_context` for every event associated with
this address.

The `callback` is a pointer to a function with the following return and parameter
types:

    void my_callback(struct msg_Conn *, msg_Event, msg_Data);

This callback receives all events associated with the address being listened to,
and is always called from within `msg_runloop`, described below.

A successful `msg_listen` call results in the `msg_listening` event being
sent to your callback.

#### --- `msg_unlisten` ---

`void msg_unlisten(msg_Conn *conn)`

This terminates a server, closing the underlying socket.

The `conn` object must be the same object that was previously sent with
any event associated with the address being closed. A good opportunity to
save `conn` is the `msg_listening` event, which occurs immediately after
a successful `msg_listen` call.

#### --- `msg_connect` ---

`void msg_connect(const char *address, void *conn_context, msg_Callback callback)`

This function is designed for client-side use. It initiates a connection with the listening
port specified in `address`. An example address is `"udp://1.2.3.4:8574"`.

The parameters are identical in meaning to those in `msg_listen`. The main differences are that:

* The server is expected to be listening before the client tries to connect; and
* the client never receives a `msg_listening` event.

The first event upon a successful `msg_connect` call is `msg_connection_ready`.

Note that UDP connections are entirely "in `msgbox`'s head;" more specifically,
connecting to a UDP port enacts no data transmission, but does specify the destination
and expected sender of all packets associated with the `conn` object sent to
your callback along with the `msg_connection_ready` event. One consequence is that
an unreachable UDP server will experience a successful `msg_connection_ready` event,
but any subsequent data transmission will fail.

#### --- `msg_disconnect` ---

`void msg_disconnect(msg_Conn *conn)`

This closes the given connection. As with `msg_unlisten`, the `conn` object
is expected to be the one sent to any event associated with
the connection being closed; `msg_connection_ready` is a good opportunity
to track this object.

Closing a connection on a server - include a udp server - does not
stop the server from listening on that address.

The `msg_connection_closed` event occurs on both client and server after
a successful disconnect.

### Sending messages

The `msg_send` and `msg_get` are similar enough that they're described together.

#### --- `msg_send` & `msg_get` ---

`void msg_send(msg_Conn *conn, msg_Data data)`

`void msg_get(msg_Conn *conn, msg_Data data, void *reply_context)`

These send aribitrary binary data on the given connection (`conn`). This
function can be used by either the client or the server once a connection is
ready - that is, after the `msg_connection_ready` event has been sent
with the given `conn` object.

The `msg_Data` object must be created by calling either `msg_new_data` (for strings)
or `msg_new_data_space` (for binary data), and later destroyed by calling
`msg_delete_data`.

A C string can be sent like this:
```
msg_Data string_data = msg_new_data(my_c_string);
msg_send(conn, string_data);
msg_delete_data(string_data);
```

Binary data can be sent like this:
```
msg_Data binary_data = msg_new_data_space(data_size);
populate_buffer(data.bytes /* type "char *" */, data.num_bytes /* type size_t */);
msg_send(conn, binary_data);
msg_delete_data(binary_data);
```

It's important to use `msg_new_data*` and `msg_delete_data` instead of
allocating your own buffer since room for headers is included in memory immediately
before the memory location of `data.bytes`.

The difference between `msg_send` and `msg_get` is that `msg_get` expects a reply
from the remote side. Either client or server may initiate a `msg_send` or `msg_get`.

When a reply is received after a `msg_get` call, it is given to the callback along
with the `msg_reply` event, and the value of `conn->reply_context` is the same as
the `reply_context` given to the initiating `msg_get` call. `msgbox` ensures that
`reply_context` is correct even if there are overlapping or out-of-order requests.
The purpose of `reply_context` is to make it easier for `msgbox` users to handle
incoming replies appropriately within their callback.

### Receiving messages

All messages are passed to the callback function registered with
`msg_listen` or `msg_connect`.

There are three message-receiving events that may be passed in to your
callback's `event` parameter:

* Event: `msg_message`

This indicates that `data` holds data sent from the remote side.
If it's string data, it can be converted to a string using the `msg_as_str` function like so:

    char *incoming_string = msg_as_str(data);  // But the data is owned by msgbox!

Incoming data is owned by `msgbox`, meaning that `msgbox` will free the memory
when your callback concludes. If you want to keep it, you need to copy it to memory
you allocate for it.

* Event: `msg_request`

This is similar to `msg_message`, except that the remote side is expecting a reply.
Call `msg_send` to send a reply. `msgbox` uses the value of `conn->reply_id` to
determine if a call to `msg_send` is a reply or not. The value 0 indicates that
it's not a reply. Since `msgbox` sets `conn->reply_id` to 0 before every event
except `msg_request`, you can usually call `msg_send` and expect it to do what
you want in the given context.

* Event: `msg_reply`

This is similar to `msg_message`, except that this is a reply to a previous
`msg_get` call. The value of `conn->reply-context` matches the `reply_context`
sent in to `msg_get`.

### The run loop

`msgbox` is designed with the expectation that you'll repeatedly
call `msg_runloop` as long as you want to work
with `msgbox`.

#### --- `msg_runloop` ---

`void msg_runloop(int timeout_in_ms)`

If you set `timeout_in_ms` to 0, then `msg_runloop` is nonblocking and will return
quickly. This is useful if you do your own work in your run loop; for example:
```
setup();
while (1) {
  do_some_work();
  msg_runloop(0 /* nonblocking */);
}
```

If your application has no other work to do outside of your `msgbox` callback, then
the timeout allows you to avoid a busy wait cycle. The following code is bad:
```
while (1) msg_runloop(0);  // This is a busy wait loop and it is a bad thing.
```

That's bad because, when no events are pending, you're keeping the cpu running
at full blast to do essentially nothing. The solution is a loop that looks more like this:
```
while (1) {
  do_occasional_work();
  msg_runloop(100);  // Calls us as soon as an event happens, or completes after 100ms.
}
```
When no events are incoming, this will only do work every 100ms, saving your cpu and
your electricity bill, and therefore your personal happiness and well-being.
When events *are* happening, they will be responded to immediately within the
run loop - `msg_runloop` skips the timeout as soon as an event occurs.

The special value `timeout_in_ms = -1` means to wait indefinitely for an event;
in that case `msg_runloop` will not return at all until an event occurs.

### Responding to errors

The `msg_error` event can occur in many cases. When this event is handed to your
callback, use `msg_as_str(data)` to get a human-friendly description of the problem.

## Building and using

Using the library in your code requires including `msgbox.h`.

There are two ways to link with the library.

One way to link with `msgbox` is to copy the `msgbox`
directory into your project and build/link its files as part
of your build process.

The other way to link with `msgbox` is to
run `make` in the root of the `msgbox` repo, which will produce
`out/libmsgbox.a`. This file can be linked with your code.

On windows, you must also link with `ws2_32.lib` or the
corresponding dll.

Example of building and using:

```
# Build libmsgbox.a.
$ make

# Write your app that includes msgbox.h.
$ vim my_app.c

# Compile and link with libmsgbox.a.
$ gcc my_app.c -o my_app out/libmsgbox.a
```

## Contributing

If you're interested in contributing to `msgbox`, please make sure the tests pass:

    $ make test

It would be great if you add a test for your bug fix or new functionality!

---

Thanks!
