# msgbox

*An easier-to-use wrapper around C tcp and udp messaging.*

## Scratch notes (to be revised before release)

There are a few guarantees that help define the behavior of msgbox.

* `msg_connect` -> `msg_connection_setup` or `msg_connection_setup_error`
* `msg_listen` -> `msg_listening` or `msg_listening_error`
* `msg_connection_ready` -> `msg_connection_closed` or `msg_connection_lost`
* `msg_get` -> `msg_reply` or `msg_reply_missing`

A typical golden server event cycle looks like this:

* `msg_listening`
* Any number of the following cycles, possibly interleaving:
  * `msg_connection_ready` with a `msg_Conn` cloned from the listening connection
  * sequence of `msg_message` or `msg_request` events
  * `msg_connection_closed`

A typical golden client event cycle looks like this:

* `msg_connection_setup`
* sequence of `msg_reply` events, one per `msg_get` call
* `msg_connection_closed`

Once a connection is set up, the client and server are symmetric - either
can send a one-way message or a request to the other.

### Header protocol

We have 4 16-bit integers to work with - treat them all as unsigned.

1. `message_type`
   * `one_way`
   * `request`
   * `reply`
   * `heartbeat`
   * `close`
2. `num_packets`
3. `packet_id`
4. `reply_id`

