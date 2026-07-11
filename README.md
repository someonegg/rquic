RQUIC
=====

RQUIC is a trimmed QUIC transport library derived from
[XQUIC](https://github.com/alibaba/xquic) 1.8.3.

It keeps the QUIC packet, stream, connection, pacing, timer, and congestion
control code, but removes TLS. Applications provide UDP I/O, timers, logging,
and protocol callbacks through the public C API in `include/rquic/`.

## Build

Requirements:

- CMake 3.10+
- C compiler with GNU11 support
- C++17 compiler when building tests

Build the library:

```sh
cmake -S . -B build
cmake --build build
```

This builds the static library:

```text
build/librquic.a
```

Build the demo programs:

```sh
cmake -S . -B build -DRQC_ENABLE_DEMO=ON
cmake --build build --target demo_server demo_client
```

The POSIX demo client and server are not built on Windows.

## Demo

Build the demo programs first, then start the server:

```sh
./build/demo/demo_server --port 8443
```

Run the client in another terminal:

```sh
./build/demo/demo_client --host 127.0.0.1 --port 8443 --path /hello
```

Expected client output:

```text
rquic demo response
resource: /hello
OK bytes=37
```

See [demo/README.md](demo/README.md) for file download mode and command options.

## Install

```sh
cmake --install build --prefix /usr/local
```

The install target exports `RQUIC::rquic` and installs public headers under
`include/rquic/`.

## Useful Options

- `RQC_ENABLE_DEMO=1`: build the POSIX demo client and server
- `RQC_ENABLE_TESTING=1`: enable testing; this also enables `RQC_ENABLE_DEMO`
- `RQC_ENABLE_EVENT_LOG=1`: enable qlog/event log support
- `RQC_ENABLE_BBR2=1`: include BBRv2 congestion control
