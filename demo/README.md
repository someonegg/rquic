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
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DRQC_ENABLE_EVENT_LOG=1
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

To batch stream writes explicitly, enable manual send mode on both endpoints:

```sh
./build/demo/demo_server --port 8443 --www-root /tmp/rquic-www --manual-send
./build/demo/demo_client --host 127.0.0.1 --port 8443 \
    --path /payload.bin --output /tmp/rquic-download.bin --manual-send
```

## Options

Server:

```text
--host ADDR
--port PORT
--www-root DIR
--log-level error|warn|info|debug
--manual-send
```

Client:

```text
--host ADDR
--port PORT
--path /resource
--output FILE
--log-level error|warn|info|debug
--manual-send
```
