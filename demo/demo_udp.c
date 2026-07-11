#include "demo_udp.h"

#include "demo_common.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(RQC_SYS_WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>
#define DEMO_CLOSESOCKET closesocket
#else
#define DEMO_CLOSESOCKET close
#endif

static int
demo_is_again(int err)
{
    return err == EAGAIN
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        || err == EWOULDBLOCK
#endif
        ;
}

static int
demo_set_nonblocking(int fd)
{
#if defined(RQC_SYS_WINDOWS)
    u_long flags = 1;
    return ioctlsocket(fd, FIONBIO, &flags) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

static int
demo_set_socket_buffers(int fd)
{
    int size = DEMO_SOCKET_BUF_SIZE;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) < 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) < 0) {
        return -1;
    }
    return 0;
}

static int
demo_create_udp_socket(int family)
{
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) {
        return -1;
    }

    if (demo_set_nonblocking(fd) != 0 || demo_set_socket_buffers(fd) != 0) {
        DEMO_CLOSESOCKET(fd);
        return -1;
    }

    return fd;
}

static int
demo_service_name(unsigned short port, char *buf, size_t buf_size)
{
    if (snprintf(buf, buf_size, "%hu", port) >= (int)buf_size) {
        return -1;
    }
    return 0;
}

void
demo_udp_socket_init(demo_udp_socket_t *sock)
{
    if (sock == NULL) {
        return;
    }
    memset(sock, 0, sizeof(*sock));
    sock->fd = -1;
}

void
demo_udp_socket_close(demo_udp_socket_t *sock)
{
    if (sock == NULL) {
        return;
    }
    if (sock->fd >= 0) {
        DEMO_CLOSESOCKET(sock->fd);
    }
    demo_udp_socket_init(sock);
}

void
demo_udp_write_ctx_cleanup(demo_udp_write_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->runtime != NULL) {
        demo_event_runtime_disarm_write(ctx->runtime, &ctx->write_watch);
    }
    ctx->write_watch.fd = -1;
}

static void
demo_udp_write_event_cb(void *arg)
{
    demo_udp_write_ctx_t *ctx = (demo_udp_write_ctx_t *)arg;

    if (ctx == NULL || ctx->engine == NULL || !ctx->has_cid) {
        return;
    }

    (void)rqc_conn_continue_send(ctx->engine, &ctx->cid);
}

static void
demo_udp_arm_write_event(demo_udp_write_ctx_t *ctx, int fd)
{
    if (ctx == NULL || ctx->runtime == NULL || fd < 0 || ctx->engine == NULL
        || !ctx->has_cid)
    {
        return;
    }

    demo_event_runtime_arm_write(ctx->runtime, &ctx->write_watch, fd,
        demo_udp_write_event_cb, ctx);
}

int
demo_udp_bind(demo_udp_socket_t *sock, const char *host, unsigned short port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *ai;
    char service[16];
    int rc;

    if (sock == NULL || demo_service_name(port, service, sizeof(service)) != 0) {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    rc = getaddrinfo(host, service, &hints, &result);
    if (rc != 0) {
        return -1;
    }

    demo_udp_socket_init(sock);
    for (ai = result; ai != NULL; ai = ai->ai_next) {
        int reuseaddr = 1;
        int fd = demo_create_udp_socket(ai->ai_family);
        if (fd < 0) {
            continue;
        }

        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            sock->fd = fd;
            sock->local_addrlen = (socklen_t)sizeof(sock->local_addr);
            if (getsockname(fd, (struct sockaddr *)&sock->local_addr,
                    &sock->local_addrlen) != 0)
            {
                demo_udp_socket_close(sock);
                continue;
            }
            freeaddrinfo(result);
            return 0;
        }
        DEMO_CLOSESOCKET(fd);
    }

    freeaddrinfo(result);
    return -1;
}

int
demo_udp_resolve_peer(const char *host, unsigned short port, demo_udp_peer_t *peer)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *ai;
    char service[16];
    int rc;

    if (host == NULL || peer == NULL
        || demo_service_name(port, service, sizeof(service)) != 0)
    {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;

    rc = getaddrinfo(host, service, &hints, &result);
    if (rc != 0) {
        return -1;
    }

    for (ai = result; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_addrlen <= sizeof(peer->addr)) {
            memset(peer, 0, sizeof(*peer));
            memcpy(&peer->addr, ai->ai_addr, ai->ai_addrlen);
            peer->addrlen = (socklen_t)ai->ai_addrlen;
            freeaddrinfo(result);
            return 0;
        }
    }

    freeaddrinfo(result);
    return -1;
}

int
demo_udp_open_client(demo_udp_socket_t *sock, const demo_udp_peer_t *peer)
{
    if (sock == NULL || peer == NULL) {
        return -1;
    }

    demo_udp_socket_init(sock);
    sock->fd = demo_create_udp_socket(((const struct sockaddr *)&peer->addr)->sa_family);
    if (sock->fd < 0) {
        return -1;
    }

    sock->local_addrlen = (socklen_t)sizeof(sock->local_addr);
    if (getsockname(sock->fd, (struct sockaddr *)&sock->local_addr,
            &sock->local_addrlen) != 0)
    {
        demo_udp_socket_close(sock);
        return -1;
    }

    return 0;
}

ssize_t
demo_udp_send_on_fd(int fd, const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen)
{
    ssize_t sent;

    if (fd < 0 || buf == NULL || peer_addr == NULL) {
        return RQC_SOCKET_ERROR;
    }

    do {
        demo_set_sys_errno(0);
        sent = sendto(fd, buf, size, 0, peer_addr, peer_addrlen);
    } while (sent < 0 && demo_get_sys_errno() == EINTR);

    if (sent < 0) {
        if (demo_is_again(demo_get_sys_errno())) {
            return RQC_SOCKET_EAGAIN;
        }
        return RQC_SOCKET_ERROR;
    }

    return sent;
}

ssize_t
demo_udp_write_socket(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *conn_user_data)
{
    demo_udp_write_ctx_t *ctx = (demo_udp_write_ctx_t *)conn_user_data;
    int fd;

    if (ctx == NULL || peer_addr == NULL) {
        return RQC_SOCKET_ERROR;
    }

    fd = ctx->fd;
    if (fd < 0) {
        if (peer_addr->sa_family == AF_INET6) {
            fd = ctx->fd6;
        } else if (peer_addr->sa_family == AF_INET) {
            fd = ctx->fd4;
        }
    }

    ssize_t sent = demo_udp_send_on_fd(fd, buf, size, peer_addr, peer_addrlen);
    if (sent == RQC_SOCKET_EAGAIN) {
        demo_udp_arm_write_event(ctx, fd);
    }
    return sent;
}

int
demo_udp_process_read(demo_udp_socket_t *sock, rqc_engine_t *engine,
    void *packet_user_data)
{
    unsigned char packet_buf[DEMO_PACKET_BUF_SIZE];
    struct sockaddr_storage peer_addr;
    socklen_t peer_addrlen;
    int ret = 0;

    if (sock == NULL || sock->fd < 0 || engine == NULL) {
        return -1;
    }

    for (;;) {
        ssize_t recv_size;

        peer_addrlen = (socklen_t)sizeof(peer_addr);
        demo_set_sys_errno(0);
        recv_size = recvfrom(sock->fd, packet_buf, sizeof(packet_buf), 0,
            (struct sockaddr *)&peer_addr, &peer_addrlen);

        if (recv_size < 0) {
            int err = demo_get_sys_errno();
            if (demo_is_again(err)) {
                break;
            }
            if (err == EINTR) {
                continue;
            }
            ret = -err;
            break;
        }

        sock->local_addrlen = (socklen_t)sizeof(sock->local_addr);
        if (getsockname(sock->fd, (struct sockaddr *)&sock->local_addr,
                &sock->local_addrlen) != 0)
        {
            ret = -1;
            break;
        }

        if (rqc_engine_packet_process(engine, packet_buf, (size_t)recv_size,
                (const struct sockaddr *)&sock->local_addr, sock->local_addrlen,
                (const struct sockaddr *)&peer_addr, peer_addrlen,
                demo_now(), packet_user_data) != RQC_OK)
        {
            ret = -1;
            break;
        }
    }

    rqc_engine_finish_recv(engine);
    return ret;
}

void
demo_udp_read_event_cb(void *arg)
{
    demo_udp_read_ctx_t *ctx = (demo_udp_read_ctx_t *)arg;

    if (ctx != NULL) {
        (void)demo_udp_process_read(ctx->socket, ctx->engine, ctx->packet_user_data);
    }
}
