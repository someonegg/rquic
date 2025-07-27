RQUIC, raw-QUIC, QUIC without TLS
==================================

RQUIC is derived from [XQUIC](https://github.com/alibaba/xquic) through trimming, the version used is "1.8.3".

## Requirements

To build RQUIC, you need
* CMake

To run demo, you need
* libevent

## Build

```bash
sudo apt-get install -y build-essential libevent-dev

mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Debug -DRQC_ENABLE_TESTING=1 -DRQC_ENABLE_EVENT_LOG=1 ..
make
```

## Demo

```bash
cd demo
./demo_server -d &
./demo_client -d -a 127.0.0.1 -p 8443 -U 'https://test.rquic.com/123'
```
