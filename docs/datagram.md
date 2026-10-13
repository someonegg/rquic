# Connection-level DATAGRAM API

The DATAGRAM API follows [RFC 9221](https://www.rfc-editor.org/rfc/rfc9221.html)
and uses the rquic handshake. It preserves message boundaries and allows
reordering and loss, without fragmentation or automatic retransmission.
DATAGRAMs share ACK processing, congestion control, bandwidth sampling, and
pacing with reliable streams, but do not consume stream or connection receive
flow-control credit.

## Configuration

Set `max_datagram_frame_size` in the connection settings to enable receiving.
The default value, `0`, disables DATAGRAM support. For example, `65535` accepts
any DATAGRAM frame that fits in a packet. The limit includes the frame type,
length field, and payload. Servers use `rqc_server_set_conn_settings`; clients
pass the settings to `rqc_connect`.

Sending only requires the peer to enable receiving, so unidirectional use is
supported. Applications must be recompiled after the public structure changes.

```c
settings.max_datagram_frame_size = 65535;
callbacks.dgram_cbs.datagram_read_notify = on_datagram;
callbacks.dgram_cbs.datagram_write_notify = on_datagram_writable;
/* Register callbacks with the ALPN before creating the connection. */
rqc_datagram_set_user_data(conn, app_context);
size_t mss = rqc_datagram_get_mss(conn);
uint64_t id;
rqc_int_t ret = rqc_datagram_send(conn, payload, payload_len, &id, 1);
```

## Payload size and sending

`rqc_datagram_get_mss` returns the current maximum payload size, accounting for
the peer's frame limit, both endpoints' UDP limits, the current packet size,
the actual rquic headers, and the existing ACK space reservation. It returns
`0` when negotiation is incomplete or the peer does not support DATAGRAMs.
Sending distinguishes these cases with `-RQC_EAGAIN` and
`-RQC_EDATAGRAM_NOT_SUPPORTED`, respectively. Oversized messages return
`-RQC_EDATAGRAM_TOO_LARGE`. Zero-length messages are allowed; nonzero lengths
require a valid data pointer.

A successful send means the entire message has been copied and queued; it does
not mean that it has been written to the UDP socket. `flush=0` activates the
connection for engine scheduling. `flush=1` drives the connection once after
submission, including when submission is blocked or a batch is only partially
accepted. Flush requests inside callbacks are handled by the current engine
processing flow to avoid recursive execution.

Each `rquic_iovec_t` passed to `rqc_datagram_send_multiple` represents a separate
message. Messages are accepted in order, stopping at the first failure. The
required `accepted_count` output is always written; callers retry starting at
that index. `datagram_id` and `datagram_ids` may be null. Only IDs for the
accepted prefix are valid. IDs are unique within the connection and are not
encoded in the wire payload.

## Callbacks

Optional callbacks are registered through `dgram_cbs`. Each receives the
connection and the DATAGRAM user data:

- `datagram_read_notify` immediately delivers a complete payload. The data
  pointer is valid only during the callback.
- `datagram_write_notify` indicates that a previously blocked submission can be
  retried, including when negotiation concludes that DATAGRAMs are unsupported.
- `datagram_mss_updated_notify` reports the initial negotiation result and
  changes to the maximum payload size.
- `datagram_acked_notify` and `datagram_lost_notify` receive the local send ID.
  Each message reports only its first ACK or suspected loss. A message suspected
  to be lost may still arrive. An ACK does not indicate application consumption,
  and a later ACK notification is not guaranteed.

## Application responsibilities

Closing a connection clears pending messages without reporting them as network
losses. Applications include business identifiers, FEC group identifiers, and
shard numbers in the payload, and size FEC shards according to the MSS.
Resending requires submitting a new DATAGRAM. Use reliable streams for control
messages and completion notifications.

This implementation does not provide a receive-credit protocol, fragmentation,
automatic retransmission, expiration policies, or application-flow lifecycle
management.
