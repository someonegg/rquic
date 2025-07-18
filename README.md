RQUIC, raw-QUIC, QUIC without TLS
==================================

## Requirements

To build RQUIC, you need
* CMake

To run demo, you need
* libevent

## Build

```bash
sudo apt-get install -y build-essential libevent-dev

mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug -DXQC_ENABLE_TESTING=1 -DXQC_ENABLE_EVENT_LOG=1 ..
make
```

## Demo

```bash
cd demo
./demo_server -d &
./demo_client -d -a 127.0.0.1 -p 8443 -U 'https://test.xquic.com/123'
```
