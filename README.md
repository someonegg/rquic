RQUIC, raw-QUIC, QUIC without TLS
==================================

## Requirements

To build RQUIC, you need
* CMake
* BoringSSL

To run demo, you need
* libevent

## Build

```bash
sudo apt-get install -y build-essential libevent-dev

SSL_PATH_STR="path/to/boringssl"

mkdir -p build && cd build
cmake -DGCOV=on -DCMAKE_BUILD_TYPE=Debug -DXQC_ENABLE_TESTING=1 -DXQC_SUPPORT_SENDMMSG_BUILD=1 -DXQC_ENABLE_EVENT_LOG=1 -DXQC_ENABLE_BBR2=1 -DXQC_ENABLE_RENO=1 -DSSL_PATH=${SSL_PATH_STR} ..

make
```
