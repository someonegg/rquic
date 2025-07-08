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

## Demo

```bash
cd build/demo
keyfile=server.key
certfile=server.crt
openssl req -newkey rsa:2048 -x509 -nodes -keyout "$keyfile" -new -out "$certfile" -subj /CN=test.xquic.com
./demo_server -d &
./demo_client -d -a 127.0.0.1 -p 8443 -U 'https://test.xquic.com/123'
```
