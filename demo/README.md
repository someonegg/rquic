# demo

`demo` is the simplified RQUIC client/server example.

The demo shows the minimum application integration path:

- create a RQUIC engine,
- drive engine timers with the demo POSIX event loop,
- process UDP packets through RQUIC,
- register a tiny HQ-style application protocol,
- send one request stream,
- receive one deterministic response or download a real file.

## Build

`demo` is currently built only on POSIX platforms. The top-level Windows build
skips this directory.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DRQC_ENABLE_DEMO=ON -DRQC_ENABLE_EVENT_LOG=1
cmake --build build --target demo_server demo_client
```

## Run

Start the server:

```sh
./build/demo/demo_server --port 8443
```

Run the client from another terminal:

```sh
./build/demo/demo_client --host 127.0.0.1 --port 8443 --path /hello
```

Expected client output:

```text
rquic demo response
resource: /hello
OK bytes=37
```

## DATAGRAM echo

Start the same server, then send one text message without creating a stream:

```sh
./build/demo/demo_server --port 8443
./build/demo/demo_client --host 127.0.0.1 --port 8443 --datagram 'hello datagram'
```

Expected client output:

```text
OK datagram bytes=14
```

An empty message (`--datagram ''`) prints `OK datagram bytes=0`.
`--datagram` cannot be repeated or combined with explicit `--path` or `--output`.
The client checks both the length and content of the echo and waits at most five
seconds from connection startup. Timeout, early connection closure, zero MSS
after negotiation, send errors, and mismatched echoes produce a nonzero exit status.

Both peers advertise `max_datagram_frame_size = 65535` as their receive capability.
This is not the send payload limit: the demo checks `rqc_datagram_get_mss()` after
negotiation and rejects oversized messages without fragmentation. Packet settings
remain `max_pkt_out_size = 1350` and `max_udp_payload_size = 1366`, suitable for a
path with MTU 1500. The demo does not discover path MTU or promise support for
arbitrary MTUs.

The demo client requires MSS > 0 after negotiation, including for empty messages.
A zero MSS produces `client datagram echo mode requires MSS > 0`. This is a demo
policy: a peer advertising `max_datagram_frame_size = 2` can support empty
DATAGRAMs with zero MSS at the library level, but this echo mode rejects it.
Before negotiation finishes, a zero MSS does not trigger this check; the client
waits for the negotiation result callback.

DATAGRAM delivery is unreliable. The client retries only when submission returns
`EAGAIN`; after successful submission it never automatically retransmits. The
server echoes immediately or keeps one copied message until writable, dropping
and logging additional messages while an echo is pending. There is no application
retransmission or message fragmentation/reassembly.

## Download a file

Create a file tree and start the server with a file root:

```sh
mkdir -p /tmp/rquic-www
dd if=/dev/urandom of=/tmp/rquic-www/payload.bin bs=1024 count=256
./build/demo/demo_server --port 8443 --www-root /tmp/rquic-www
```

Download the file from another terminal:

```sh
./build/demo/demo_client --host 127.0.0.1 --port 8443 \
    --path /payload.bin --output /tmp/rquic-download.bin
```

Check the downloaded content:

```sh
cmp /tmp/rquic-www/payload.bin /tmp/rquic-download.bin
```

The file response demonstrates batched sending: file chunks are queued with
`flush=0`, then each logical batch is completed with
`rqc_engine_finish_send()`. Small requests and responses use `flush=1` so they
drive their connection immediately. STREAM FIN and flushing are independent;
the final file chunk still uses `flush=0` and relies on the batch finish call.

## Options

Server:

```text
--host ADDR
--port PORT
--www-root DIR
--log-level error|warn|info|debug
```

Client:

```text
--host ADDR
--port PORT
--path /resource
--output FILE
--datagram TEXT
--log-level error|warn|info|debug
```
