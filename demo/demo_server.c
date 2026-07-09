/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <memory.h>
#include <stdlib.h>
#include <fcntl.h>
#include <event2/event.h>
#include <signal.h>
#include <inttypes.h>
#include <rquic/rquic_typedef.h>
#include <rquic/rquic.h>
#include <ctype.h>
#include "platform.h"

#ifndef RQC_SYS_WINDOWS
#include <unistd.h>
#include <sys/wait.h>
#else
#include "getopt.h"
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"event.lib")
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "crypt32")
#endif

#include "common.h"
#include "rqc_hq.h"

#define RQC_PACKET_TMP_BUF_LEN 1500
#define MAX_BUF_SIZE (100*1024*1024)

/**
 * ============================================================================
 * the network config definition section
 * network config is those arguments about socket connection
 * all configuration on network should be put under this section
 * ============================================================================
 */

#define DEFAULT_IP   "127.0.0.1"
#define DEFAULT_PORT 8443

typedef struct rqc_demo_svr_net_config_s {

    /* server addr info */
    struct sockaddr addr;
    int     addr_len;
    char    ip[64];
    short   port;

    /* ipv4 or ipv6 */
    int     ipv6;

    int     pacing; /* is pacing on */

    /* idle persist timeout */
    int     conn_timeout;
} rqc_demo_svr_net_config_t;

/**
 * ============================================================================
 * the quic config definition section
 * quic config is those arguments about quic connection
 * all configuration on network should be put under this section
 * ============================================================================
 */

typedef struct rqc_demo_svr_quic_config_s {
    /* dummy mode */
    int  dummy_mode;

    size_t max_pkt_sz;
} rqc_demo_svr_quic_config_t;

/**
 * ============================================================================
 * the environment config definition section
 * environment config is those arguments about IO inputs and outputs
 * all configuration on environment should be put under this section
 * ============================================================================
 */

#define LOG_PATH "slog.log"
#define SOURCE_DIR  "."

/* environment config */
typedef struct rqc_demo_svr_env_config_s {
    /* log path */
    char    log_path[PATH_LEN];
    int     log_level;

    /* source file dir */
    char    source_file_dir[RESOURCE_LEN];
} rqc_demo_svr_env_config_t;

typedef struct rqc_demo_svr_args_s {
    /* network args */
    rqc_demo_svr_net_config_t    net_cfg;

    /* quic args */
    rqc_demo_svr_quic_config_t   quic_cfg;

    /* environment args */
    rqc_demo_svr_env_config_t    env_cfg;
} rqc_demo_svr_args_t;

typedef struct rqc_demo_svr_ctx_s {
    struct event_base   *eb;

    rqc_engine_t        *engine;
    struct event        *ev_engine;

    /* ipv4 server */
    int                 fd;
    struct sockaddr_in  local_addr;
    socklen_t           local_addrlen;
    struct event        *ev_socket;

    /* ipv6 server */
    int                 fd6;
    struct sockaddr_in6 local_addr6;
    socklen_t           local_addrlen6;
    struct event        *ev_socket6;

    int                 current_fd;

    int                 log_fd;

    rqc_demo_svr_args_t *args;
} rqc_demo_svr_ctx_t;

typedef struct rqc_demo_svr_user_conn_s {
    struct event           *ev_timeout;
    struct sockaddr_in6     peer_addr;
    socklen_t               peer_addrlen;
    rqc_cid_t               cid;
    rqc_demo_svr_ctx_t     *ctx;
} rqc_demo_svr_user_conn_t;

typedef struct rqc_demo_svr_resource_s {
    FILE       *fp;
    off_t       total_len;      /* total len of file */
    off_t       total_offset;   /* total sent offset of file */
    char       *buf;            /* send buf */
    int         buf_size;       /* send buf size */
    int         buf_len;        /* send buf len */
    int         buf_offset;     /* send buf offset */
} rqc_demo_svr_resource_t;

#define REQ_BUF_SIZE        2048
typedef struct rqc_demo_svr_user_stream_s {
    rqc_hq_request_t           *hq_request;

    // uint64_t            send_offset;
    int                         header_sent;
    int                         header_recvd;
    size_t                      send_body_len;
    size_t                      recv_body_len;
    char                       *recv_buf;

    // rqc_demo_svr_user_conn_t         *conn;
    rqc_demo_svr_resource_t     res;  /* resource info */
} rqc_demo_svr_user_stream_t;

/* the global unique server context */
rqc_demo_svr_ctx_t svr_ctx;

/******************************************************************************
 *                   start of engine callback functions                       *
 ******************************************************************************/

void
rqc_demo_svr_set_event_timer(rqc_msec_t wake_after, void *eng_user_data)
{
    rqc_demo_svr_ctx_t *ctx = (rqc_demo_svr_ctx_t *)eng_user_data;

    struct timeval tv;
    tv.tv_sec = wake_after / 1000000;
    tv.tv_usec = wake_after % 1000000;
    event_add(ctx->ev_engine, &tv);
}

int
rqc_demo_svr_accept(rqc_engine_t *engine, rqc_connection_t *conn, const rqc_cid_t *cid,
    void *eng_user_data)
{
    DEBUG;

    return 0;
}

int
rqc_demo_svr_open_log_file(rqc_demo_svr_ctx_t *ctx)
{
    ctx->log_fd = open(ctx->args->env_cfg.log_path, (O_WRONLY | O_APPEND | O_CREAT), 0644);
    if (ctx->log_fd <= 0) {
        return -1;
    }
    return 0;
}

int
rqc_demo_svr_close_log_file(rqc_demo_svr_ctx_t *ctx)
{
    if (ctx->log_fd <= 0) {
        return -1;
    }
    close(ctx->log_fd);
    return 0;
}

void
rqc_demo_svr_write_log_file(rqc_log_level_t lvl, const void *buf, size_t size, void *eng_user_data)
{
    rqc_demo_svr_ctx_t *ctx = (rqc_demo_svr_ctx_t*)eng_user_data;
    if (ctx->log_fd <= 0) {
        return;
    }

    int write_len = write(ctx->log_fd, buf, size);
    if (write_len < 0) {
        printf("write log failed, errno: %d\n", get_sys_errno());
        return;
    }
    write_len = write(ctx->log_fd, line_break, 1);
    if (write_len < 0) {
        printf("write log failed, errno: %d\n", get_sys_errno());
    }
}

void
rqc_demo_svr_write_qlog_file(qlog_event_importance_t imp, const void *buf, size_t size, void *eng_user_data)
{
    rqc_demo_svr_ctx_t *ctx = (rqc_demo_svr_ctx_t*)eng_user_data;
    if (ctx->log_fd <= 0) {
        return;
    }

    int write_len = write(ctx->log_fd, buf, size);
    if (write_len < 0) {
        printf("write qlog failed, errno: %d\n", get_sys_errno());
        return;
    }
    write_len = write(ctx->log_fd, line_break, 1);
    if (write_len < 0) {
        printf("write qlog failed, errno: %d\n", get_sys_errno());
    }
}

/******************************************************************************
 *                              common functions                              *
 ******************************************************************************/

void
rqc_demo_svr_close_user_stream_resource(rqc_demo_svr_user_stream_t * user_stream)
{
    if (user_stream->res.buf) {
        free(user_stream->res.buf);
        user_stream->res.buf = NULL;
    }

    if (user_stream->res.fp)
    {
        fclose(user_stream->res.fp);
        user_stream->res.fp = NULL;
    }
}

/******************************************************************************
 *                       start of hq callback functions                       *
 ******************************************************************************/

int
rqc_demo_svr_hq_conn_create_notify(rqc_hq_conn_t *hqc, const rqc_cid_t *cid, void *conn_user_data)
{
    DEBUG;
    rqc_demo_svr_user_conn_t *user_conn = calloc(1, sizeof(rqc_demo_svr_user_conn_t));
    rqc_hq_conn_set_user_data(hqc, user_conn);

    /* set ctx */
    user_conn->ctx = &svr_ctx;
    memcpy(&user_conn->cid, cid, sizeof(*cid));

    /* set addr info */
    rqc_hq_conn_get_peer_addr(hqc, (struct sockaddr *)&user_conn->peer_addr,
                              sizeof(user_conn->peer_addr), &user_conn->peer_addrlen);

    return 0;
}

int
rqc_demo_svr_hq_conn_close_notify(rqc_hq_conn_t *conn, const rqc_cid_t *cid, void *conn_user_data)
{
    DEBUG;

    if (conn_user_data == &svr_ctx) {
        return 0;
    }

    rqc_demo_svr_user_conn_t *user_conn = (rqc_demo_svr_user_conn_t*)conn_user_data;
    rqc_conn_stats_t stats = rqc_conn_get_stats(user_conn->ctx->engine, cid);
    printf("send_count:%u, lost_count:%u, tlp_count:%u, recv_count:%u, srtt:%"PRIu64" "
            "conn_err:%d, ack_info:%s\n",
            stats.send_count, stats.lost_count, stats.tlp_count, stats.recv_count, stats.srtt,
            stats.conn_err, stats.ack_info);
    free(user_conn);
    user_conn = NULL;

    return 0;
}

void
rqc_demo_svr_hq_conn_handshake_finished(rqc_hq_conn_t *conn, void *conn_user_data)
{
    DEBUG;
    // printf("rqc_demo_svr_conn_handshake_finished, user_data: %p, conn: %p\n", conn_user_data, conn);
    // rqc_demo_svr_user_conn_t *user_conn = (rqc_demo_svr_user_conn_t *)conn_user_data;
}

int
rqc_demo_svr_send_rsp_resource(rqc_demo_svr_user_stream_t *user_stream, char *data, ssize_t len,
    int fin)
{
    ssize_t ret = rqc_hq_request_send_rsp(user_stream->hq_request, data, len, fin);
    if (ret == -RQC_EAGAIN) {
        ret = 0;
    }

    return ret;
}

int
rqc_demo_svr_hq_req_create_notify(rqc_hq_request_t *hqr, void *req_user_data)
{
    DEBUG;
    rqc_demo_svr_user_stream_t *user_stream = calloc(1, sizeof(rqc_demo_svr_user_stream_t));
    user_stream->hq_request = hqr;

    rqc_hq_request_set_user_data(hqr, user_stream);

    user_stream->recv_buf = calloc(1, REQ_BUF_SIZE);

    return 0;
}

int
rqc_demo_svr_hq_req_close_notify(rqc_hq_request_t *hqr, void *req_user_data)
{
    DEBUG;
    rqc_demo_svr_user_stream_t *user_stream = (rqc_demo_svr_user_stream_t*)req_user_data;
    free(user_stream);
    return 0;
}

/**
 * send buf utill EAGAIN
 * [return] > 0: finish send; 0: not finished
 */
int
rqc_demo_svr_hq_send_file(rqc_hq_request_t *hqr, rqc_demo_svr_user_stream_t *user_stream)
{
    int ret = 0;
    rqc_demo_svr_resource_t *res = &user_stream->res;
    while (res->total_offset < res->total_len) {   /* still have bytes to be sent */
        char *send_buf = NULL;  /* the buf need to be send */
        int send_len = 0;       /* len of the the buf gonna be sent */
        if (res->buf_offset < res->buf_len) {
            /* prev buf not sent completely, continue send from last offset */
            send_buf = res->buf + res->buf_offset;
            send_len = res->buf_len - res->buf_offset;

        } else {
            /* prev buf sent, read new buf and send */
            res->buf_offset = 0;

            if (!svr_ctx.args->quic_cfg.dummy_mode) {
                res->buf_len = fread(res->buf, 1, res->buf_size, res->fp);
                if (res->buf_len <= 0) {
                    return -1;
                }
            } else {
                res->buf_len = res->total_len - res->total_offset;
                res->buf_len = res->buf_len > res->buf_size ? res->buf_size : res->buf_len;
                memset(res->buf, 'D', res->buf_len);
            }

            send_buf = res->buf;
            send_len = res->buf_len;
        }

        /* send buf */
        int fin = send_len + res->total_offset == res->total_len ? 1 : 0;
        ret = rqc_demo_svr_send_rsp_resource(user_stream, send_buf, send_len, fin);
        if (ret > 0) {
            res->buf_offset += ret;
            res->total_offset += ret;

        } else if (ret == 0) {
            break;

        } else {
            printf("send file data failed!!: ret: %d\n", ret);
            return -1;
        }
    }

    return res->total_offset == res->total_len;
}

void
rqc_demo_svr_handle_hq_request(rqc_demo_svr_user_stream_t *user_stream, rqc_hq_request_t *hqr,
    char *resource, ssize_t len)
{
    int ret = 0;

    if (!svr_ctx.args->quic_cfg.dummy_mode) {
        /* format file path */
        char file_path[PATH_LEN] = {0};
        snprintf(file_path, sizeof(file_path), "%s%s", svr_ctx.args->env_cfg.source_file_dir, resource);
        user_stream->res.fp = fopen(file_path, "rb");
        if (NULL == user_stream->res.fp) {
            printf("error open file [%s]\n", file_path);
            goto handle_error;
        }
        /* get total len */
        fseek(user_stream->res.fp, 0, SEEK_END);
#ifdef RQC_SYS_WINDOWS
        user_stream->res.total_len = ftell(user_stream->res.fp);
#else
        user_stream->res.total_len = ftello(user_stream->res.fp);
#endif
        fseek(user_stream->res.fp, 0, SEEK_SET);

    } else {
        user_stream->res.total_len = atoi(resource + 1);
        if (user_stream->res.total_len == 0) {
            user_stream->res.total_len = 1;
        }
    }

    // printf("open file[%s] suc, user_conn: %p\n", file_path, user_stream->conn);

    /* create buf */
    user_stream->res.buf = (char *)malloc(READ_FILE_BUF_LEN);
    if (NULL == user_stream->res.buf) {
        printf("error create resource buf\n");
        goto handle_error;
    }
    user_stream->res.buf_size = READ_FILE_BUF_LEN;

    /* begin to send file */
    ret = rqc_demo_svr_hq_send_file(hqr, user_stream);
    if (ret == 0) {
        return;
    }

handle_error:
    rqc_demo_svr_close_user_stream_resource(user_stream);
}

int
rqc_demo_svr_hq_req_read_notify(rqc_hq_request_t *hqr, void *req_user_data)
{
    DEBUG;
    unsigned char fin = 0;
    rqc_demo_svr_user_stream_t *user_stream = (rqc_demo_svr_user_stream_t *)req_user_data;
    ssize_t read;
    do {
        char *buf = user_stream->recv_buf + user_stream->recv_body_len;
        size_t buf_size = REQ_BUF_SIZE - user_stream->recv_body_len;
        read = rqc_hq_request_recv_req(hqr, buf, buf_size, &fin);
        if (read == -RQC_EAGAIN) {
            break;

        } else if (read < 0) {
            printf("rqc_stream_recv error %zd\n", read);
            return 0;
        }

        user_stream->recv_body_len += read;
    } while (read > 0 && !fin);

    if (fin) {
        rqc_demo_svr_handle_hq_request(user_stream, hqr, user_stream->recv_buf,
            user_stream->recv_body_len);
    }

    return 0;
}

int
rqc_demo_svr_hq_req_write_notify(rqc_hq_request_t *hqr, void *req_user_data)
{
    DEBUG;
    //printf("rqc_demo_svr_hq_req_write_notify user_data: %p\n", user_data);
    rqc_demo_svr_user_stream_t *user_stream = (rqc_demo_svr_user_stream_t*)req_user_data;
    int ret = rqc_demo_svr_hq_send_file(hqr, user_stream);
    if (ret != 0) {
        /* error or finish, close user_stream */
        rqc_demo_svr_close_user_stream_resource(user_stream);
    }

    return 0;
}

/******************************************************************************
 *                     start of socket operation function                     *
 ******************************************************************************/

ssize_t
rqc_demo_svr_write_socket(const unsigned char *buf, size_t size, const struct sockaddr *peer_addr,
    socklen_t peer_addrlen, void *conn_user_data)
{
    ssize_t res;

    // rqc_demo_svr_user_conn_t *user_conn = (rqc_demo_svr_user_conn_t *)conn_user_data;

    int fd = svr_ctx.current_fd;

    do {
        set_sys_errno(0);
        res = sendto(fd, buf, size, 0, peer_addr, peer_addrlen);
        if (res < 0) {
            printf("rqc_demo_svr_write_socket err %zd %s, fd: %d\n",
                res, strerror(get_sys_errno()), fd);
            if (get_sys_errno() == EAGAIN) {
                res = RQC_SOCKET_EAGAIN;
            }
        }
    } while ((res < 0) && (get_sys_errno() == EINTR));

    return res;
}

void
rqc_demo_svr_socket_write_handler(rqc_demo_svr_ctx_t *ctx, int fd)
{
    DEBUG
}

void
rqc_demo_svr_socket_read_handler(rqc_demo_svr_ctx_t *ctx, int fd)
{
    DEBUG;
    ssize_t recv_sum = 0;
    struct sockaddr_in6 peer_addr;
    socklen_t peer_addrlen = sizeof(peer_addr);
    ssize_t recv_size = 0;
    unsigned char packet_buf[RQC_PACKET_TMP_BUF_LEN];

    ctx->current_fd = fd;

    do {
        recv_size = recvfrom(fd, packet_buf, sizeof(packet_buf), 0,
                             (struct sockaddr *) &peer_addr, &peer_addrlen);
        if (recv_size < 0 && get_sys_errno() == EAGAIN) {
            break;
        }

        if (recv_size < 0) {
            printf("!!!!!!!!!recvfrom: recvmsg = %zd err=%s\n", recv_size, strerror(get_sys_errno()));
            break;
        }
        recv_sum += recv_size;

        uint64_t recv_time = rqc_now();
        rqc_int_t ret = rqc_engine_packet_process(ctx->engine, packet_buf, recv_size,
                                      (struct sockaddr *)(&ctx->local_addr), ctx->local_addrlen,
                                      (struct sockaddr *)(&peer_addr), peer_addrlen,
                                      (rqc_usec_t)recv_time, ctx);
        if (ret != RQC_OK) {
            printf("server_read_handler: packet process err, ret: %d\n", ret);
            return;
        }
    } while (recv_size > 0);

    printf("recvfrom size:%zu\n", recv_sum);
    rqc_engine_finish_recv(ctx->engine);
}

static void
rqc_demo_svr_socket_event_callback(int fd, short what, void *arg)
{
    //DEBUG;
    rqc_demo_svr_ctx_t *ctx = (rqc_demo_svr_ctx_t *)arg;
    if (what & EV_WRITE) {
        rqc_demo_svr_socket_write_handler(ctx, fd);

    } else if (what & EV_READ) {
        rqc_demo_svr_socket_read_handler(ctx, fd);

    } else {
        printf("event callback: fd=%d, what=%d\n", fd, what);
        exit(1);
    }
}

/* create socket and bind port */
static int
rqc_demo_svr_init_socket(int family, uint16_t port,
        struct sockaddr *local_addr, socklen_t local_addrlen)
{
    int size;
    int opt_reuseaddr;
    int fd = socket(family, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("create socket failed, errno: %d\n", get_sys_errno());
        return -1;
    }

    /* non-block */
#ifdef RQC_SYS_WINDOWS
    int flags = 1;
    if (ioctlsocket(fd, FIONBIO, &flags) == SOCKET_ERROR) {
        goto err;
    }
#else
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == -1) {
        printf("set socket nonblock failed, errno: %d\n", get_sys_errno());
        goto err;
    }
#endif

    /* reuse port */
    opt_reuseaddr = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt_reuseaddr, sizeof(opt_reuseaddr)) < 0) {
        printf("setsockopt failed, errno: %d\n", get_sys_errno());
        goto err;
    }

    /* send/recv buffer size */
    size = 1 * 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(int)) < 0) {
        printf("setsockopt failed, errno: %d\n", get_sys_errno());
        goto err;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(int)) < 0) {
        printf("setsockopt failed, errno: %d\n", get_sys_errno());
        goto err;
    }

    /* bind port */
    if (bind(fd, local_addr, local_addrlen) < 0) {
        printf("bind socket failed, family: %d, errno: %d, %s\n", family,
            get_sys_errno(), strerror(get_sys_errno()));
        goto err;
    }

    return fd;

err:
    close(fd);
    return -1;
}

static int
rqc_demo_svr_create_socket(rqc_demo_svr_ctx_t *ctx, rqc_demo_svr_net_config_t* cfg)
{
    /* ipv4 socket */
    memset(&ctx->local_addr, 0, sizeof(ctx->local_addr));
    ctx->local_addr.sin_family = AF_INET;
    ctx->local_addr.sin_port = htons(cfg->port);
    ctx->local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    ctx->local_addrlen = sizeof(ctx->local_addr);
    ctx->fd = rqc_demo_svr_init_socket(AF_INET, cfg->port, (struct sockaddr*)&ctx->local_addr,
        ctx->local_addrlen);
    printf("create ipv4 socket fd: %d\n", ctx->fd);

    /* ipv6 socket */
    memset(&ctx->local_addr6, 0, sizeof(ctx->local_addr6));
    ctx->local_addr6.sin6_family = AF_INET6;
    ctx->local_addr6.sin6_port = htons(cfg->port);
    ctx->local_addr6.sin6_addr = in6addr_any;
    ctx->local_addrlen6 = sizeof(ctx->local_addr6);
    ctx->fd6 = rqc_demo_svr_init_socket(AF_INET6, cfg->port, (struct sockaddr*)&ctx->local_addr6,
        ctx->local_addrlen6);
    printf("create ipv6 socket fd: %d\n", ctx->fd6);

    if (!ctx->fd && !ctx->fd6) {
        return -1;
    }

    return 0;
}

static void
rqc_demo_svr_engine_callback(int fd, short what, void *arg)
{
    rqc_demo_svr_ctx_t *ctx = (rqc_demo_svr_ctx_t *) arg;

    rqc_engine_main_logic(ctx->engine);
}

void
rqc_demo_svr_init_args(rqc_demo_svr_args_t *args)
{
    memset(args, 0, sizeof(rqc_demo_svr_args_t));

    /* net cfg */
    strncpy(args->net_cfg.ip, DEFAULT_IP, sizeof(args->net_cfg.ip) - 1);
    args->net_cfg.port = DEFAULT_PORT;

    /* env cfg */
    args->env_cfg.log_level = RQC_LOG_DEBUG;
    strncpy(args->env_cfg.log_path, LOG_PATH, PATH_LEN - 1);
    strncpy(args->env_cfg.source_file_dir, SOURCE_DIR, RESOURCE_LEN - 1);

    args->quic_cfg.max_pkt_sz = 1350;
}

void
rqc_demo_svr_usage(int argc, char *argv[])
{
    char *prog = argv[0];
    char *const slash = strrchr(prog, '/');
    if (slash) {
        prog = slash + 1;
    }
    printf(
            "Usage: %s [Options]\n"
            "\n"
            "Options:\n"
            "   -p    Listen port.\n"
            "   -l    Log level. e:error d:debug.\n"
            "   -L    rquic log path.\n"
            "   -d    do not read responses from files\n"
            "   -D    resource directory\n"
            "   -F    MTU size (default: 1200)\n"
            "   -C    Pacing on.\n"
            "   -6    IPv6\n"
            , prog);
}

void
rqc_demo_svr_parse_args(int argc, char *argv[], rqc_demo_svr_args_t *args)
{
    int ch = 0;
    while ((ch = getopt(argc, argv, "p:l:L:dD:F:C6")) != -1) {
        switch (ch) {
        case 'p':
            printf("option listen port :%s\n", optarg);
            args->net_cfg.port = atoi(optarg);
            break;

        case 'l':
            printf("option log level :%s\n", optarg);
            args->env_cfg.log_level = optarg[0];
            break;

        case 'L':
            printf("option log path :%s\n", optarg);
            snprintf(args->env_cfg.log_path, sizeof(args->env_cfg.log_path), "%s", optarg);
            break;

        case 'd':
            printf("option dummpy mode on\n");
            args->quic_cfg.dummy_mode = 1;
            break;

        case 'D':
            printf("option resource directory :%s\n", optarg);
            strncpy(args->env_cfg.source_file_dir, optarg, RESOURCE_LEN - 1);
            break;

        case 'F':
            printf("MTU size: %s\n", optarg);
            args->quic_cfg.max_pkt_sz = atoi(optarg);
            break;

        case 'C':
            printf("option pacing :%s\n", "on");
            args->net_cfg.pacing = 1;
            break;

        case '6':
            printf("option IPv6 :%s\n", "on");
            args->net_cfg.ipv6 = 1;
            break;

        default:
            printf("other option :%c\n", ch);
            rqc_demo_svr_usage(argc, argv);
            exit(0);
        }
    }
}

void
rqc_demo_svr_init_callback(rqc_engine_callback_t *cb, rqc_transport_callbacks_t *transport_cbs,
    rqc_demo_svr_args_t* args)
{
    static rqc_engine_callback_t callback = {
        .set_event_timer = rqc_demo_svr_set_event_timer,
        .log_callbacks = {
            .rqc_log_write_err = rqc_demo_svr_write_log_file,
            .rqc_log_write_stat = rqc_demo_svr_write_log_file,
            .rqc_qlog_event_write = rqc_demo_svr_write_qlog_file
        },
    };

    static rqc_transport_callbacks_t tcb = {
        .server_accept = rqc_demo_svr_accept,
        .write_socket = rqc_demo_svr_write_socket,
    };

    *cb = callback;
    *transport_cbs = tcb;
}

/* init server ctx */
void
rqc_demo_svr_init_ctx(rqc_demo_svr_ctx_t *ctx, rqc_demo_svr_args_t *args)
{
    memset(ctx, 0, sizeof(rqc_demo_svr_ctx_t));
    ctx->current_fd = -1;
    ctx->args = args;
    rqc_demo_svr_open_log_file(ctx);
}

void
rqc_demo_svr_init_conn_settings(rqc_engine_t *engine, rqc_demo_svr_args_t *args)
{
    /* init connection settings */
    rqc_conn_settings_t conn_settings = {
        .pacing_on  =   args->net_cfg.pacing,
        .cong_ctrl_callback = rqc_bbr_cb,
        .cc_params = {
            .customize_on = 1,
            .init_cwnd = 32,
            .bbr_enable_lt_bw = 1,
        },
        .spurious_loss_detect_on = 1,
        .init_idle_time_out = 60000,
        .max_pkt_out_size = args->quic_cfg.max_pkt_sz,
        .adaptive_ack_frequency = 1,
    };

    rqc_server_set_conn_settings(engine, &conn_settings);
}

int
rqc_demo_svr_init_alpn_ctx(rqc_demo_svr_ctx_t *ctx)
{
    int ret = 0;

    rqc_hq_callbacks_t hq_cbs = {
        .hqc_cbs = {
            .conn_create_notify = rqc_demo_svr_hq_conn_create_notify,
            .conn_close_notify = rqc_demo_svr_hq_conn_close_notify,
        },
        .hqr_cbs = {
            .req_create_notify = rqc_demo_svr_hq_req_create_notify,
            .req_close_notify = rqc_demo_svr_hq_req_close_notify,
            .req_read_notify = rqc_demo_svr_hq_req_read_notify,
            .req_write_notify = rqc_demo_svr_hq_req_write_notify,
        }
    };

    /* init hq context */
    ret = rqc_hq_ctx_init(ctx->engine, &hq_cbs);
    if (ret != RQC_OK) {
        printf("init hq context error, ret: %d\n", ret);
        return ret;
    }

    return ret;
}

/* init rquic server engine */
int
rqc_demo_svr_init_rquic_engine(rqc_demo_svr_ctx_t *ctx, rqc_demo_svr_args_t *args)
{
    /* init engine callbacks */
    rqc_engine_callback_t callback;
    rqc_transport_callbacks_t transport_cbs;
    rqc_demo_svr_init_callback(&callback, &transport_cbs, args);

    /* init engine config */
    rqc_config_t config;
    if (rqc_engine_get_default_config(&config, RQC_ENGINE_SERVER) < 0) {
        return RQC_ERROR;
    }

    config.cid_len = 12;

    switch (args->env_cfg.log_level) {
    case 'd':
        config.cfg_log_level = RQC_LOG_DEBUG;
        break;
    case 'i':
        config.cfg_log_level = RQC_LOG_INFO;
        break;
    case 'w':
        config.cfg_log_level = RQC_LOG_WARN;
        break;
    case 'e':
        config.cfg_log_level = RQC_LOG_ERROR;
        break;
    default:
        config.cfg_log_level = RQC_LOG_DEBUG;
        break;
    }

    /* create server engine */
    ctx->engine = rqc_engine_create(RQC_ENGINE_SERVER, &config,
                                    &callback, &transport_cbs, ctx);
    if (ctx->engine == NULL) {
        printf("rqc_engine_create error\n");
        return -1;
    }

    /* init server connection settings */
    rqc_demo_svr_init_conn_settings(ctx->engine, args);

    if (rqc_demo_svr_init_alpn_ctx(ctx) < 0) {
        printf("init alpn ctx error!");
        return -1;
    }

    return 0;
}

#if 0
void stop(int signo)
{
    event_base_loopbreak(eb);
    rqc_engine_destroy(ctx.engine);
    fflush(stdout);
    exit(0);
}
#endif

void
rqc_demo_svr_free_ctx(rqc_demo_svr_ctx_t *ctx)
{
    rqc_demo_svr_close_log_file(ctx);

    if (ctx->args) {
        free(ctx->args);
        ctx->args = NULL;
    }

    free(ctx);
}

void
th3_demo_proxy_sig_hndlr(int signo)
{
    if (signo == SIGTERM) {
        rqc_demo_svr_ctx_t *ctx = &svr_ctx;
        event_base_loopbreak(ctx->eb);
    }
}

int
main(int argc, char *argv[])
{
    /* init env if necessary */
    rqc_platform_init_env();

    signal(SIGTERM, th3_demo_proxy_sig_hndlr);

    /* get input server args */
    rqc_demo_svr_args_t *args = calloc(1, sizeof(rqc_demo_svr_args_t));
    rqc_demo_svr_init_args(args);
    rqc_demo_svr_parse_args(argc, argv, args);

    /* init server ctx */
    rqc_demo_svr_ctx_t *ctx = &svr_ctx;
    rqc_demo_svr_init_ctx(ctx, args);

    /* engine event */
    struct event_base *eb = event_base_new();
    ctx->ev_engine = event_new(eb, -1, 0, rqc_demo_svr_engine_callback, ctx);
    ctx->eb = eb;

    if (rqc_demo_svr_init_rquic_engine(ctx, args) < 0) {
        return -1;
    }

    /* init socket */
    int ret = rqc_demo_svr_create_socket(ctx, &args->net_cfg);
    if (ret < 0) {
        printf("rqc_create_socket error\n");
        return 0;
    }

    /* socket event */
    ctx->ev_socket = event_new(eb, ctx->fd, EV_READ | EV_PERSIST,
        rqc_demo_svr_socket_event_callback, ctx);
    event_add(ctx->ev_socket, NULL);

    /* socket event */
    ctx->ev_socket6 = event_new(eb, ctx->fd6, EV_READ | EV_PERSIST,
        rqc_demo_svr_socket_event_callback, ctx);
    event_add(ctx->ev_socket6, NULL);

    event_base_dispatch(eb);

    rqc_engine_destroy(ctx->engine);
    // rqc_demo_svr_free_ctx(ctx);

    return 0;
}
