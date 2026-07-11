#ifndef DEMO_UDP_H
#define DEMO_UDP_H

#include <sys/socket.h>

#include "demo_event.h"

#include <rquic/rquic.h>

typedef struct demo_udp_socket_s {
    int fd;
    struct sockaddr_storage local_addr;
    socklen_t local_addrlen;
} demo_udp_socket_t;

typedef struct demo_udp_peer_s {
    struct sockaddr_storage addr;
    socklen_t addrlen;
} demo_udp_peer_t;

typedef struct demo_udp_write_ctx_s {
    int fd;
    int fd4;
    int fd6;
    demo_event_runtime_t *runtime;
    demo_event_watch_t write_watch;
    rqc_engine_t *engine;
    rqc_cid_t cid;
    int has_cid;
    void *user_data;
} demo_udp_write_ctx_t;

typedef struct demo_udp_read_ctx_s {
    demo_udp_socket_t *socket;
    rqc_engine_t *engine;
    void *packet_user_data;
} demo_udp_read_ctx_t;

void demo_udp_socket_init(demo_udp_socket_t *sock);
void demo_udp_socket_close(demo_udp_socket_t *sock);
void demo_udp_write_ctx_cleanup(demo_udp_write_ctx_t *ctx);

int demo_udp_bind(demo_udp_socket_t *sock, const char *host, unsigned short port);
int demo_udp_open_client(demo_udp_socket_t *sock, const demo_udp_peer_t *peer);
int demo_udp_resolve_peer(const char *host, unsigned short port,
    demo_udp_peer_t *peer);

ssize_t demo_udp_send_on_fd(int fd, const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen);
ssize_t demo_udp_write_socket(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *conn_user_data);

int demo_udp_process_read(demo_udp_socket_t *sock, rqc_engine_t *engine,
    void *packet_user_data);
void demo_udp_read_event_cb(void *arg);

#endif
