/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#define _GNU_SOURCE

#include <rquic/rquic.h>
#include <rquic/rquic_typedef.h>
#include <stdio.h>
#include <memory.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>
#include <string.h>
#include <event2/event.h>
#include "common.h"
#include "rqc_hq.h"
#include "platform.h"

#ifdef RQC_SYS_WINDOWS
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"event.lib")
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "crypt32")
#include "getopt.h"
#else
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <unistd.h>
#include <netdb.h>
#endif

#define RQC_PACKET_TMP_BUF_LEN  1600
#define MAX_BUF_SIZE            (100*1024*1024)

typedef enum rqc_demo_cli_alpn_type_s {
    ALPN_HQ,
} rqc_demo_cli_alpn_type_t;

#define MAX_HEADER 100

typedef struct rqc_demo_cli_user_conn_s rqc_demo_cli_user_conn_t;

typedef struct rqc_demo_cli_user_stream_s {
    rqc_demo_cli_user_conn_t   *user_conn;

    /* save file */
    char                        file_name[RESOURCE_LEN];
    FILE                        *recv_body_fp;

    /* stat for IO */
    size_t                      send_body_len;
    size_t                      recv_body_len;
    int                         recv_fin;
    rqc_msec_t                  start_time;

    /* hq request content */
    rqc_hq_request_t           *hq_request;
    char                       *send_buf;
    size_t                      send_len;
    size_t                      send_offset;
} rqc_demo_cli_user_stream_t;

/**
 * ============================================================================
 * the network config definition section
 * network config is those arguments about socket connection
 * all configuration on network should be put under this section
 * ============================================================================
 */

typedef enum rqc_demo_cli_task_mode_s {
    /* send multi requests in single connection with multi streams */
    MODE_SCMR,

    /* serially send multi requests in multi connections, with one request each connection */
    MODE_SCSR_SERIAL,

    /* concurrently send multi requests in multi connections, with one request each connection */
    MODE_SCSR_CONCURRENT,
} rqc_demo_cli_task_mode_t;

/* network arguments */
typedef struct rqc_demo_cli_net_config_s {

    /* server addr info */
    struct sockaddr_in6 addr;
    int                 addr_len;
    char                server_addr[64];
    short               server_port;
    char                host[256];

    /* ipv4 or ipv6 */
    int                 ipv6;

    int                 pacing; /* is pacing on */

    /* idle persist timeout */
    int                 conn_timeout;

    rqc_demo_cli_task_mode_t mode;

    uint8_t rebind_path;

    uint8_t addr_specified;
    uint8_t port_specified;

} rqc_demo_cli_net_config_t;

/**
 * ============================================================================
 * the quic config definition section
 * quic config is those arguments about quic connection
 * all configuration on network should be put under this section
 * ============================================================================
 */

typedef struct rqc_demo_cli_quic_config_s {
    /* alpn protocol of client */
    rqc_demo_cli_alpn_type_t alpn_type;
    char alpn[16];
    int quic_version;

    uint64_t recv_rate;

    uint64_t idle_timeout;

    size_t max_pkt_sz;
} rqc_demo_cli_quic_config_t;

/**
 * ============================================================================
 * the environment config definition section
 * environment config is those arguments about IO inputs and outputs
 * all configuration on environment should be put under this section
 * ============================================================================
 */

#define LOG_PATH "clog.log"
#define OUT_DIR  "."

/* environment config */
typedef struct rqc_demo_cli_env_config_s {

    /* log path */
    char    log_path[256];
    int     log_level;

    /* out file */
    char    out_file_dir[256];

    /* life cycle */
    int     life;
} rqc_demo_cli_env_config_t;

/**
 * ============================================================================
 * the request config definition section
 * all configuration on request should be put under this section
 * ============================================================================
 */

#define MAX_REQUEST_CNT 20480    /* client might deal MAX_REQUEST_CNT requests once */
#define MAX_REQUEST_LEN 256     /* the max length of a request */
#define g_host ""

/* args of one single request */
typedef struct rqc_demo_cli_request_s {
    char            path[RESOURCE_LEN];         /* request path */
    char            scheme[8];                  /* request scheme, http/https */
    REQUEST_METHOD  method;
    char            auth[AUTHORITY_LEN];
    char            url[URL_LEN];               /* original url */
} rqc_demo_cli_request_t;

/* request bundle args */
typedef struct rqc_demo_cli_requests_s {
    /* requests */
    int                     request_cnt;    /* requests cnt in urls */
    rqc_demo_cli_request_t  reqs[MAX_REQUEST_CNT];

    /* do not save responses to files */
    int dummy_mode;

    /* delay X us to start reqs */
    uint64_t req_start_delay;

    uint64_t idle_gap;

    int ext_reqn;
    int batch_cnt;

} rqc_demo_cli_requests_t;

/**
 * ============================================================================
 * the client args definition section
 * client initial args
 * ============================================================================
 */
typedef struct rqc_demo_cli_client_args_s {
    /* network args */
    rqc_demo_cli_net_config_t   net_cfg;

    /* quic args */
    rqc_demo_cli_quic_config_t  quic_cfg;

    /* environment args */
    rqc_demo_cli_env_config_t   env_cfg;

    /* request args */
    rqc_demo_cli_requests_t     req_cfg;
} rqc_demo_cli_client_args_t;

typedef enum rqc_demo_cli_task_status_s {
    TASK_STATUS_WAITTING,
    TASK_STATUS_RUNNING,
    TASK_STATUS_FINISHED,
    TASK_STATUS_FAILED,
} rqc_demo_cli_task_status_t;

typedef struct rqc_demo_cli_task_schedule_info_s {

    rqc_demo_cli_task_status_t  status;         /* task status */
    int                         req_create_cnt; /* streams created */
    int                         req_sent_cnt;
    int                         req_fin_cnt;    /* the req cnt which have received FIN */
    uint8_t                     fin_flag;       /* all reqs finished, need close */
} rqc_demo_cli_task_schedule_info_t;

/*
 * the task schedule info, used to mark the operation
 * info of all requests, the client will exit when all
 * tasks are finished or closed
 */
typedef struct rqc_demo_cli_task_schedule_s {
    /* the cnt of tasks that been running or have been ran */
    int idx;

    /* the task status, 0: not executed; 1: suc; -1: failed */
    rqc_demo_cli_task_schedule_info_t   *schedule_info;
} rqc_demo_cli_task_schedule_t;

/*
 * task info structure.
 * a task is strongly correlate to a net connection
 */
typedef struct rqc_demo_cli_task_s {
    int         task_idx;
    int         req_cnt;
    rqc_demo_cli_request_t   *reqs;      /* a task could contain multipule requests, which wil be sent  */
    rqc_demo_cli_user_conn_t *user_conn; /* user_conn handle */
} rqc_demo_cli_task_t;

typedef struct rqc_demo_cli_task_ctx_s {
    /* task mode */
    rqc_demo_cli_task_mode_t        mode;

    /* total task cnt */
    int                             task_cnt;

    /* task list */
    rqc_demo_cli_task_t            *tasks;

    /* current task schedule info */
    rqc_demo_cli_task_schedule_t    schedule;        /* current task index */
} rqc_demo_cli_task_ctx_t;

typedef struct rqc_demo_cli_ctx_s {
    /* rquic engine context */
    rqc_engine_t    *engine;

    /* libevent context */
    struct event    *ev_engine;
    struct event    *ev_task;
    struct event    *ev_kill;
    struct event_base *eb;  /* handle of libevent */

    /* log context */
    int             log_fd;
    char            log_path[256];

    /* client context */
    rqc_demo_cli_client_args_t  *args;

    /* task schedule context */
    rqc_demo_cli_task_ctx_t     task_ctx;
} rqc_demo_cli_ctx_t;

typedef struct rqc_demo_cli_user_path_s {

    uint64_t                path_id;
    uint8_t                 is_active;

    int                     fd;
    int                     rebind_fd;

    struct sockaddr_in6     local_addr;
    socklen_t               local_addrlen;
    struct sockaddr_in6     peer_addr;
    socklen_t               peer_addrlen;

    struct event           *ev_socket;
    struct event           *ev_rebind_socket;
    struct event           *ev_timeout;

    uint64_t                last_sock_op_time;

    rqc_demo_cli_user_conn_t *user_conn;

} rqc_demo_cli_user_path_t;

typedef struct rqc_demo_cli_user_conn_s {

    rqc_cid_t                cid;
    rqc_hq_conn_t           *hqc_handle;

    rqc_demo_cli_user_path_t path;

    struct event            *ev_delay_req;
    struct event            *ev_idle_restart;
    struct event            *ev_rebind_path;

    rqc_demo_cli_ctx_t      *ctx;
    rqc_demo_cli_task_t     *task;
} rqc_demo_cli_user_conn_t;

static void
rqc_demo_cli_delayed_idle_restart(int fd, short what, void *arg);

void
rqc_demo_cli_continue_send_reqs(rqc_demo_cli_user_conn_t *user_conn);

void
rqc_demo_cli_send_requests(rqc_demo_cli_user_conn_t *user_conn,
    rqc_demo_cli_client_args_t *args,
    rqc_demo_cli_request_t *reqs, int req_cnt);

int rqc_demo_cli_init_user(rqc_demo_cli_user_conn_t *user_conn, uint64_t path_id);

int
rqc_demo_cli_close_task(rqc_demo_cli_task_t *task)
{
    rqc_demo_cli_user_conn_t *user_conn = task->user_conn;

    if (user_conn->path.is_active) {
        user_conn->path.is_active = 0;
        /* remove event handle */
        if (user_conn->path.ev_socket) {
            event_del(user_conn->path.ev_socket);
        }

        event_del(user_conn->path.ev_timeout);
        /* close socket */
        close(user_conn->path.fd);

        if (user_conn->path.ev_rebind_socket) {
            event_del(user_conn->path.ev_rebind_socket);
        }

        if (user_conn->path.rebind_fd != -1) {
            close(user_conn->path.rebind_fd);
        }
    }

    if (user_conn->ev_delay_req) {
        event_del(user_conn->ev_delay_req);
        user_conn->ev_delay_req = NULL;
    }

    if (user_conn->ev_idle_restart) {
        event_del(user_conn->ev_idle_restart);
        user_conn->ev_idle_restart = NULL;
    }

    if (user_conn->ev_rebind_path) {
        event_del(user_conn->ev_rebind_path);
        user_conn->ev_rebind_path =  NULL;
    }

    return 0;
}

/**
 * [return] 1: all req suc, task finished, 0: still got req underway
 */
void
rqc_demo_cli_on_stream_fin(rqc_demo_cli_user_stream_t *user_stream)
{
    rqc_demo_cli_task_ctx_t *ctx = &user_stream->user_conn->ctx->task_ctx;
    rqc_demo_cli_user_conn_t *user_conn = user_stream->user_conn;
    rqc_demo_cli_ctx_t *conn_ctx = user_conn->ctx;
    int task_idx = user_stream->user_conn->task->task_idx;

    /* all reqs are finished, close the connection */
    if (++ctx->schedule.schedule_info[task_idx].req_fin_cnt
        == ctx->tasks[task_idx].req_cnt)
    {
        ctx->schedule.schedule_info[task_idx].fin_flag = 1;
        /* close rquic conn */
        rqc_hq_conn_close(conn_ctx->engine, user_conn->hqc_handle, &user_conn->cid);
    }
    printf("task[%d], fin_cnt: %d, fin_flag: %d\n", task_idx,
        ctx->schedule.schedule_info[task_idx].req_fin_cnt,
        ctx->schedule.schedule_info[task_idx].fin_flag);
}

/* directly finish a task */
void
rqc_demo_cli_on_task_finish(rqc_demo_cli_ctx_t *ctx, rqc_demo_cli_task_t *task)
{
    rqc_demo_cli_close_task(task);
    ctx->task_ctx.schedule.schedule_info[task->task_idx].status = TASK_STATUS_FINISHED;

    printf("task finished, total task_req_cnt: %d, req_fin_cnt: %d, req_sent_cnt: %d, "
            "req_create_cnt: %d\n", task->req_cnt,
            ctx->task_ctx.schedule.schedule_info[task->task_idx].req_fin_cnt,
            ctx->task_ctx.schedule.schedule_info[task->task_idx].req_sent_cnt,
            ctx->task_ctx.schedule.schedule_info[task->task_idx].req_create_cnt);
}

/* directly fail a task */
void
rqc_demo_cli_on_task_fail(rqc_demo_cli_ctx_t *ctx, rqc_demo_cli_task_t *task)
{
    ctx->task_ctx.schedule.schedule_info[task->task_idx].status = TASK_STATUS_FAILED;

    printf("task failed, total task_req_cnt: %d, req_fin_cnt: %d, req_sent_cnt: %d, "
           "req_create_cnt: %d\n", task->req_cnt,
           ctx->task_ctx.schedule.schedule_info[task->task_idx].req_fin_cnt,
           ctx->task_ctx.schedule.schedule_info[task->task_idx].req_sent_cnt,
           ctx->task_ctx.schedule.schedule_info[task->task_idx].req_create_cnt);
}

/******************************************************************************
 *                   start of engine callback functions                       *
 ******************************************************************************/

void
rqc_demo_cli_set_event_timer(rqc_usec_t wake_after, void *eng_user_data)
{
    rqc_demo_cli_ctx_t *ctx = (rqc_demo_cli_ctx_t *) eng_user_data;
    //printf("rqc_engine_wakeup_after %llu us, now %llu\n", wake_after, rqc_now());

    struct timeval tv;
    tv.tv_sec = wake_after / 1000000;
    tv.tv_usec = wake_after % 1000000;
    event_add(ctx->ev_engine, &tv);

}

int
rqc_demo_cli_open_log_file(rqc_demo_cli_ctx_t *ctx)
{
    ctx->log_fd = open(ctx->log_path, (O_WRONLY | O_APPEND | O_CREAT), 0644);
    if (ctx->log_fd <= 0) {
        return -1;
    }
    return 0;
}

int
rqc_demo_cli_close_log_file(rqc_demo_cli_ctx_t *ctx)
{
    if (ctx->log_fd <= 0) {
        return -1;
    }
    close(ctx->log_fd);
    return 0;
}

void
rqc_demo_cli_write_log_file(rqc_log_level_t lvl, const void *buf, size_t size, void *engine_user_data)
{
    rqc_demo_cli_ctx_t *ctx = (rqc_demo_cli_ctx_t*)engine_user_data;
    if (ctx->log_fd <= 0) {
        return;
    }
    //printf("%s", (char *)buf);
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
rqc_demo_cli_write_qlog_file(qlog_event_importance_t imp, const void *buf, size_t size, void *engine_user_data)
{
    rqc_demo_cli_ctx_t *ctx = (rqc_demo_cli_ctx_t*)engine_user_data;
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
 *                   start of common callback functions                       *
 ******************************************************************************/

ssize_t
rqc_demo_cli_write_socket(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *conn_user_data)
{
    rqc_demo_cli_user_conn_t *user_conn = (rqc_demo_cli_user_conn_t *)conn_user_data;
    rqc_demo_cli_user_path_t *user_path = &user_conn->path;

    ssize_t res = 0;

    do {
        set_sys_errno(0);
        res = sendto(user_path->fd, buf, size, 0, peer_addr, peer_addrlen);
        if (res < 0) {
            printf("rqc_demo_cli_write_socket err %zd %s, fd: %d, buf: %p, size: %zu, "
                "server_addr: %s\n", res, strerror(get_sys_errno()), user_path->fd, buf, size,
                user_conn->ctx->args->net_cfg.server_addr);
            if (get_sys_errno() == EAGAIN) {
                res = RQC_SOCKET_EAGAIN;
            }
        }
        user_path->last_sock_op_time = rqc_now();
    } while ((res < 0) && (get_sys_errno() == EINTR));

    return res;
}

#if defined(RQC_SUPPORT_SENDMMSG) && !defined(RQC_SYS_WINDOWS)
ssize_t
rqc_demo_cli_write_mmsg(void *conn_user_data, struct iovec *msg_iov, unsigned int vlen,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen)
{
    const int MAX_SEG = 128;
    rqc_demo_cli_user_conn_t *user_conn = (rqc_demo_cli_user_conn_t *)conn_user_data;
    ssize_t res = 0;
    int fd = user_conn->path.fd;
    struct mmsghdr mmsg[MAX_SEG];
    memset(&mmsg, 0, sizeof(mmsg));
    for (int i = 0; i < vlen; i++) {
        mmsg[i].msg_hdr.msg_iov = &msg_iov[i];
        mmsg[i].msg_hdr.msg_iovlen = 1;
    }
    do {
        errno = 0;
        res = sendmmsg(fd, mmsg, vlen, 0);
        if (res < 0) {
            printf("sendmmsg err %zd %s\n", res, strerror(errno));
            if (errno == EAGAIN) {
                res = RQC_SOCKET_EAGAIN;
            }
        }

    } while ((res < 0) && (get_sys_errno() == EINTR));
    return res;
}
#endif

/******************************************************************************
 *                       start of hq callback functions                       *
 ******************************************************************************/

int
rqc_demo_cli_hq_conn_create_notify(rqc_hq_conn_t *hqc, const rqc_cid_t *cid, void *user_data)
{
    DEBUG;

    // printf("rqc_demo_cli_hq_conn_create_notify, conn: %p, user_conn: %p\n", conn, user_data);
    rqc_demo_cli_user_conn_t *user_conn = (rqc_demo_cli_user_conn_t *)user_data;
    user_conn->hqc_handle = hqc;
    memcpy(&user_conn->cid, cid, sizeof(rqc_cid_t));
    return 0;
}

int
rqc_demo_cli_hq_conn_close_notify(rqc_hq_conn_t *hqc, const rqc_cid_t *cid, void *user_data)
{
    DEBUG;
    // printf("rqc_demo_cli_hq_conn_close_notify, conn: %p, user_conn: %p\n", conn, user_data);

    rqc_demo_cli_user_conn_t *user_conn = (rqc_demo_cli_user_conn_t *) user_data;
    rqc_demo_cli_on_task_finish(user_conn->ctx, user_conn->task);
    free(user_conn);
    return 0;
}

void
rqc_demo_cli_hq_conn_handshake_finished(rqc_hq_conn_t *hqc, void *conn_user_data)
{
    DEBUG;
    // rqc_demo_cli_user_conn_t *user_conn = (rqc_demo_cli_user_conn_t *)conn_user_data;
    printf("hqc[%p] handshake finished\n", hqc);
}

int
rqc_demo_cli_hq_req_send(rqc_hq_request_t *hqr, rqc_demo_cli_user_stream_t *user_stream)
{
    ssize_t ret = 0;

    if (user_stream->start_time == 0) {
        user_stream->start_time = rqc_now();
    }

    ret = rqc_hq_request_send_req(hqr, user_stream->send_buf);
    if (ret < 0) {
        switch (-ret) {
        case RQC_EAGAIN:
            return 0;

        default:
            printf("send stream failed, ret: %zd\n", ret);
            return -1;
        }

    } else {
        user_stream->send_offset += ret;
        user_stream->send_body_len += ret;
    }

    return ret;
}

int
rqc_demo_cli_hq_req_write_notify(rqc_hq_request_t *hqr, void *req_user_data)
{
    DEBUG;
    ssize_t ret = 0;
    rqc_demo_cli_user_stream_t *user_stream = (rqc_demo_cli_user_stream_t *)req_user_data;
    if (user_stream->send_len > user_stream->send_offset)
    {
        ret = rqc_demo_cli_hq_req_send(hqr, user_stream);
        // printf("rqc_demo_cli_hq_req_write_notify, user_stream[%p] send_cnt: %d\n", user_stream, ret);
        if (ret == user_stream->send_len) {
            user_stream->user_conn->ctx->task_ctx.schedule.schedule_info->req_sent_cnt++;
        }
    }

    return 0;
}

int
rqc_demo_cli_hq_req_read_notify(rqc_hq_request_t *hqr, void *req_user_data)
{
    DEBUG;
    unsigned char fin = 0;
    rqc_demo_cli_user_stream_t *user_stream = (rqc_demo_cli_user_stream_t *)req_user_data;
    char buff[4096] = {0};
    size_t buff_size = 4096;

    ssize_t read = 0;
    do {
        read = rqc_hq_request_recv_rsp(hqr, buff, buff_size, &fin);
        if (read == -RQC_EAGAIN) {
            break;

        } else if (read < 0) {
            printf("rqc_stream_recv error %zd\n", read);
            return 0;
        }

        if (user_stream->recv_body_fp) {
            int nwrite = fwrite(buff, 1, read, user_stream->recv_body_fp);
            if (nwrite != read) {
                printf("fwrite error\n");
                return -1;
            }
            fflush(user_stream->recv_body_fp);
        }

        user_stream->recv_body_len += read;
    } while (read > 0 && !fin);

    if (fin) {
        user_stream->recv_fin = 1;
        rqc_msec_t now_us = rqc_now();
        printf("\033[33m>>>>>>>> request time cost:%"PRIu64" us, speed:%"PRIu64" K/s \n"
               ">>>>>>>> user_stream[%p], req: %s, send_body_size:%zu, recv_body_size:%zu \033[0m\n",
               now_us - user_stream->start_time,
               (user_stream->send_body_len + user_stream->recv_body_len)*1000/(now_us - user_stream->start_time),
               user_stream, user_stream->file_name, user_stream->send_body_len, user_stream->recv_body_len);

        /* close file */
        if (user_stream->recv_body_fp) {
            fclose(user_stream->recv_body_fp);
            user_stream->recv_body_fp = NULL;
        }

        // rqc_demo_cli_on_stream_fin(user_stream);
    }
    return 0;
}

int
rqc_demo_cli_hq_req_close_notify(rqc_hq_request_t *hqr, void *req_user_data)
{
    DEBUG;
    // printf("rqc_demo_cli_hq_req_close_notify, stream: %p, user_conn: %p\n", stream, user_data);

    rqc_demo_cli_user_stream_t *user_stream = (rqc_demo_cli_user_stream_t *)req_user_data;

    /* print stats */
    rqc_stream_stats_t stats = rqc_hq_request_get_stats(hqr);

    printf("\033[33m[HQ-req] type:%u, err:%u, send_bytes:%zu, recv_bytes:%zu\n\033[0m",
           (unsigned)stats.stream_type, (unsigned)stats.stream_err, (size_t)stats.send_bytes, (size_t)stats.recv_bytes);

    /* task schedule */
    rqc_demo_cli_on_stream_fin(user_stream);

    if (!user_stream->user_conn->ev_idle_restart) {
        rqc_demo_cli_continue_send_reqs(user_stream->user_conn);
    }

    free(user_stream->send_buf);
    free(user_stream);
    return 0;
}

/******************************************************************************
 *                     start of socket callback functions                     *
 ******************************************************************************/

void
rqc_demo_cli_socket_write_handler(rqc_demo_cli_user_conn_t *user_conn, int fd)
{
    DEBUG;
    // rqc_conn_continue_send(user_conn->ctx->engine, &user_conn->cid);
}

void
rqc_demo_cli_socket_read_handler(rqc_demo_cli_user_conn_t *user_conn, int fd)
{
    DEBUG;
    ssize_t recv_size = 0;
    struct sockaddr addr;
    socklen_t addr_len = 0;
    unsigned char packet_buf[RQC_PACKET_TMP_BUF_LEN];

    rqc_demo_cli_user_path_t *user_path = &user_conn->path;

    do {
        recv_size = recvfrom(user_path->fd, packet_buf, sizeof(packet_buf), 0,
                            (struct sockaddr *)&addr, &addr_len);
        if (recv_size < 0 && get_sys_errno() == EAGAIN) {
            break;
        }

        if (recv_size <= 0) {
            break;
        }

        user_path->local_addrlen = sizeof(struct sockaddr_in6);
        rqc_int_t ret = getsockname(user_path->fd, (struct sockaddr*)&user_path->local_addr,
                                    &user_path->local_addrlen);
        if (ret != 0) {
            printf("getsockname error, errno: %d\n", get_sys_errno());
        }

        uint64_t recv_time = rqc_now();
        user_path->last_sock_op_time = recv_time;
        if (rqc_engine_packet_process(user_conn->ctx->engine, packet_buf, recv_size,
                                      (struct sockaddr *)(&user_path->local_addr),
                                      user_path->local_addrlen, (struct sockaddr *)(&addr),
                                      addr_len, (rqc_msec_t)recv_time,
                                      user_conn) != RQC_OK)
        {
            return;
        }
    } while (recv_size > 0);

    rqc_engine_finish_recv(user_conn->ctx->engine);
}

static void
rqc_demo_cli_socket_event_callback(int fd, short what, void *arg)
{
    //DEBUG;
    rqc_demo_cli_user_conn_t *user_conn = (rqc_demo_cli_user_conn_t *) arg;

    if (what & EV_WRITE) {
        rqc_demo_cli_socket_write_handler(user_conn, fd);

    } else if (what & EV_READ) {
        rqc_demo_cli_socket_read_handler(user_conn, fd);

    } else {
        printf("event callback: what=%d\n", what);
        exit(1);
    }
}

/******************************************************************************
 *                     start of engine callback functions                     *
 ******************************************************************************/

static void
rqc_demo_cli_engine_callback(int fd, short what, void *arg)
{
    // printf("timer wakeup now:%"PRIu64"\n", rqc_now());
    rqc_demo_cli_ctx_t *ctx = (rqc_demo_cli_ctx_t *) arg;
    rqc_engine_main_logic(ctx->engine);
}

static void
rqc_demo_cli_idle_callback(int fd, short what, void *arg)
{
    rqc_demo_cli_user_path_t *user_path = (rqc_demo_cli_user_path_t*) arg;
    rqc_demo_cli_user_conn_t *user_conn = user_path->user_conn;

    if (rqc_now() - user_path->last_sock_op_time < (uint64_t)user_conn->ctx->args->net_cfg.conn_timeout * 1000000) {
        struct timeval tv;
        tv.tv_sec = user_conn->ctx->args->net_cfg.conn_timeout;
        tv.tv_usec = 0;
        event_add(user_path->ev_timeout, &tv);

    } else {
        int rc = rqc_hq_conn_close(user_conn->ctx->engine, user_conn->hqc_handle, &user_conn->cid);

        if (user_conn->ev_delay_req) {
            event_del(user_conn->ev_delay_req);
            user_conn->ev_delay_req = NULL;
        }

        if (user_conn->ev_idle_restart) {
            event_del(user_conn->ev_idle_restart);
            user_conn->ev_idle_restart = NULL;
        }

        if (user_conn->ev_rebind_path) {
            event_del(user_conn->ev_rebind_path);
            user_conn->ev_rebind_path =  NULL;
        }

        printf("socket idle timeout, task failed, total task_cnt: %d, req_fin_cnt: %d, req_sent_cnt: %d, req_create_cnt: %d\n",
               user_conn->ctx->task_ctx.tasks[user_conn->task->task_idx].req_cnt,
               user_conn->ctx->task_ctx.schedule.schedule_info[user_conn->task->task_idx].req_fin_cnt,
               user_conn->ctx->task_ctx.schedule.schedule_info[user_conn->task->task_idx].req_sent_cnt,
               user_conn->ctx->task_ctx.schedule.schedule_info[user_conn->task->task_idx].req_create_cnt);
        rqc_demo_cli_on_task_fail(user_conn->ctx, user_conn->task);

        if (rc) {
            printf("close path or conn error, path_id %"PRIu64"\n", user_path->path_id);
            return;
        }
    }
}

static void
rqc_demo_cli_delayed_req_start(int fd, short what, void *arg)
{
    rqc_demo_cli_user_conn_t *user_conn = (rqc_demo_cli_user_conn_t *) arg;
    int batch_cnt = user_conn->ctx->args->req_cfg.batch_cnt;
    int req_cnt = user_conn->task->req_cnt;
    if (batch_cnt) {
        req_cnt = req_cnt > batch_cnt ? batch_cnt : req_cnt;
    }
    rqc_demo_cli_send_requests(user_conn, user_conn->ctx->args,
                               user_conn->task->reqs, req_cnt);

    if (req_cnt > user_conn->ctx->task_ctx.schedule.schedule_info[user_conn->task->task_idx].req_create_cnt
        && user_conn->ctx->args->req_cfg.idle_gap
        && user_conn->ctx->args->req_cfg.batch_cnt)
    {

        user_conn->ev_idle_restart = event_new(user_conn->ctx->eb, -1, 0,
                                                rqc_demo_cli_delayed_idle_restart,
                                                user_conn);
        struct timeval tv = {
            .tv_sec = user_conn->ctx->args->req_cfg.idle_gap / 1000,
            .tv_usec = (user_conn->ctx->args->req_cfg.idle_gap % 1000) * 1000,
        };
        event_add(user_conn->ev_idle_restart, &tv);
    }
}

static void
rqc_demo_cli_delayed_idle_restart(int fd, short what, void *arg)
{
    rqc_demo_cli_user_conn_t *user_conn = (rqc_demo_cli_user_conn_t *)arg;
    rqc_demo_cli_task_ctx_t *ctx = &user_conn->ctx->task_ctx;
    int task_idx = user_conn->task->task_idx;
    rqc_demo_cli_continue_send_reqs(ctx->tasks[task_idx].user_conn);
}

static void
rqc_demo_cli_rebind_path(int fd, short what, void *arg)
{
    rqc_demo_cli_user_conn_t *user_conn = (rqc_demo_cli_user_conn_t *) arg;
    if (user_conn->path.is_active) {
        // change fd
        int temp = user_conn->path.fd;
        user_conn->path.fd = user_conn->path.rebind_fd;
        user_conn->path.rebind_fd = temp;

        //stop read from the old socket
        event_del(user_conn->path.ev_socket);
        user_conn->path.ev_socket = NULL;
    }
}

/******************************************************************************
 *                        start of client init functions                      *
 ******************************************************************************/

void
rqc_demo_cli_init_conneciton_settings(rqc_conn_settings_t* settings,
    rqc_demo_cli_client_args_t *args)
{
    memset(settings, 0, sizeof(rqc_conn_settings_t));
    settings->pacing_on = args->net_cfg.pacing;
    settings->cong_ctrl_callback = rqc_bbr_cb;
    settings->cc_params.customize_on = 1,
    settings->cc_params.init_cwnd = 96,
    settings->so_sndbuf = 1024*1024;
    settings->proto_version = args->quic_cfg.quic_version;
    settings->spurious_loss_detect_on = 1;
    settings->recv_rate_bytes_per_sec = args->quic_cfg.recv_rate;
    settings->max_pkt_out_size = args->quic_cfg.max_pkt_sz;
    settings->adaptive_ack_frequency = 1;
}

/* set client args to default values */
void
rqc_demo_cli_init_args(rqc_demo_cli_client_args_t *args)
{
    memset(args, 0, sizeof(rqc_demo_cli_client_args_t));

    /* net cfg */
    args->net_cfg.conn_timeout = 30;
    strncpy(args->net_cfg.server_addr, "127.0.0.1", sizeof(args->net_cfg.server_addr));
    args->net_cfg.server_port = 8443;
    args->net_cfg.addr_specified = 0;
    args->net_cfg.port_specified = 0;

    /* env cfg */
    args->env_cfg.log_level = RQC_LOG_DEBUG;
    strncpy(args->env_cfg.log_path, LOG_PATH, sizeof(args->env_cfg.log_path));
    strncpy(args->env_cfg.out_file_dir, OUT_DIR, sizeof(args->env_cfg.out_file_dir));

    /* quic cfg */
    args->quic_cfg.alpn_type = ALPN_HQ;
    strncpy(args->quic_cfg.alpn, "hq-interop", sizeof(args->quic_cfg.alpn));
    args->quic_cfg.max_pkt_sz = 1350;
    args->quic_cfg.quic_version = RQC_VERSION_V1;
}

void
rqc_demo_cli_parse_server_addr(char *url, rqc_demo_cli_net_config_t *cfg)
{
    /* get hostname and port */
    char s_port[16] = {0};
    char tmp[256] = {0};
    sscanf(url, "%*[^://]://%[^/]", tmp);

    if (strchr(tmp, ':') != NULL) {
        sscanf(url, "%*[^://]://%[^:]:%[^/]", cfg->host, s_port);
    } else {
        sscanf(url, "%*[^://]://%[^/]", cfg->host);
    }

    /* parse port */
    if (!cfg->port_specified) {
        cfg->server_port = atoi(s_port);
    }

    if (!cfg->addr_specified) {
        /* set hint for hostname resolve */
        struct addrinfo hints = {0};
        memset(&hints, 0, sizeof(struct addrinfo));
        if (cfg->ipv6) {
            hints.ai_family = AF_INET6;    /* Allow IPv4 or IPv6 */
        } else {
            hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
        }
        hints.ai_socktype = SOCK_DGRAM; /* Datagram socket */
        hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
        hints.ai_protocol = 0;          /* Any protocol */
        hints.ai_canonname = NULL;
        hints.ai_addr = NULL;
        hints.ai_next = NULL;

        /* resolve server's ip from hostname */
        struct addrinfo *result = NULL;
        int rv = getaddrinfo(cfg->host, s_port, &hints, &result);
        if (rv != 0) {
            printf("get addr info from hostname: %s\n", gai_strerror(rv));
        }
        memcpy(&cfg->addr, result->ai_addr, result->ai_addrlen);
        cfg->addr_len = result->ai_addrlen;

        /* convert to string. */
        if (result->ai_family == AF_INET6) {
            inet_ntop(result->ai_family, &(((struct sockaddr_in6*)result->ai_addr)->sin6_addr),
                cfg->server_addr, sizeof(cfg->server_addr));
        } else {
            inet_ntop(result->ai_family, &(((struct sockaddr_in*)result->ai_addr)->sin_addr),
                cfg->server_addr, sizeof(cfg->server_addr));
        }
        freeaddrinfo(result);
    } else {

        if (strchr(cfg->server_addr, ':') != NULL) {
            struct sockaddr_in6 *addr6 = &cfg->addr;
            addr6->sin6_family = AF_INET6;
            addr6->sin6_port = htons(cfg->server_port);
            inet_pton(AF_INET6, cfg->server_addr, &addr6->sin6_addr);
            cfg->ipv6 = 1;
            cfg->addr_len = sizeof(struct sockaddr_in6);

        } else {
            struct sockaddr_in *addr4 = (struct sockaddr_in*)&cfg->addr;
            addr4->sin_family = AF_INET;
            addr4->sin_port = htons(cfg->server_port);
            inet_pton(AF_INET, cfg->server_addr, &addr4->sin_addr);
            cfg->ipv6 = 0;
            cfg->addr_len = sizeof(struct sockaddr_in);
        }
    }

    printf("server[%s] addr: %s:%d.\n", cfg->host, cfg->server_addr, cfg->server_port);

}

void
rqc_demo_cli_parse_urls(char *urls, rqc_demo_cli_client_args_t *args)
{
    /* split urls */
    int cnt = 0;
    static char *separator = " ";
    char *token = strtok(urls, separator);
    while (token != NULL) {
        if (token) {
            strncpy(args->req_cfg.reqs[cnt].url, token, URL_LEN - 1);
            sscanf(token, "%[^://]://%[^/]%s", args->req_cfg.reqs[cnt].scheme,
                args->req_cfg.reqs[cnt].auth, args->req_cfg.reqs[cnt].path);
        }
        cnt++;
        token = strtok(NULL, separator);
    }
    args->req_cfg.request_cnt = cnt;

    /* parse the server addr */
    if (args->req_cfg.request_cnt > 0) {
        rqc_demo_cli_parse_server_addr(args->req_cfg.reqs[0].url, &args->net_cfg);
    }
}

void
rqc_demo_cli_usage(int argc, char *argv[])
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
        "   -a    Server addr.\n"
        "   -p    Server port.\n"
        "   -l    Log level. e:error d:debug.\n"
        "   -L    rquic log path.\n"
        "   -d    do not save responses to files\n"
        "   -D    response directory\n"
        "   -U    Url. \n"
        "   -x    Extend the number of requests to X\n"
        "   -r    Send X requests per batch\n"
        "   -K    Client's life circle time\n"
        "   -w    waiting N ms to start the first request.\n"
        "   -I    Idle interval between requests (ms)\n"
        "   -t    Connection timeout. Default 3 seconds.\n"
        "   -T    Throttle recving rate (Bps)\n"
        "   -A    alpn selection: hq\n"
        "   -F    MTU size (default: 1200)\n"
        "   -e    NAT rebinding after 2s\n"
        "   -C    Pacing on.\n"
        "   -6    IPv6\n"
        , prog);
}

void
rqc_demo_cli_parse_args(int argc, char *argv[], rqc_demo_cli_client_args_t *args)
{
    int ch = 0;
    while ((ch = getopt(argc, argv, "a:p:l:L:dD:U:x:r:K:w:I:t:T:A:F:eC6")) != -1) {
        switch (ch) {
        case 'a':
            printf("option server addr :%s\n", optarg);
            snprintf(args->net_cfg.server_addr, sizeof(args->net_cfg.server_addr), "%s", optarg);
            args->net_cfg.addr_specified = 1;
            break;

        case 'p':
            printf("option server port :%s\n", optarg);
            args->net_cfg.server_port = atoi(optarg);
            args->net_cfg.port_specified = 1;
            break;

        case 'l':
            printf("option log level :%s\n", optarg);
            args->env_cfg.log_level = optarg[0];
            break;

        case 'L':
            printf("option log path :%s\n", optarg);
            strncpy(args->env_cfg.log_path, optarg, sizeof(args->env_cfg.log_path) - 1);
            break;

        case 'd':
            printf("option dummy mode on\n");
            args->req_cfg.dummy_mode = 1;
            break;

        case 'D':
            printf("option response directory: %s\n", optarg);
            strncpy(args->env_cfg.out_file_dir, optarg, sizeof(args->env_cfg.out_file_dir) - 1);
            break;

        case 'U': // request URL, address is parsed from the request
            printf("option url only:%s\n", optarg);
            rqc_demo_cli_parse_urls(optarg, args);
            break;

        case 'x':
            printf("Extend request number: %s\n", optarg);
            args->req_cfg.ext_reqn = atoi(optarg);
            break;

        case 'r':
            printf("Request batch: %s\n", optarg);
            args->req_cfg.batch_cnt = atoi(optarg);
            break;

        case 'K':
            printf("client life circle time: %s\n", optarg);
            args->env_cfg.life = atoi(optarg);
            break;

        case 'w':
            printf("option first req delay: %s\n", optarg);
            args->req_cfg.req_start_delay = atoi(optarg);
            break;

        case 'I':
            printf("option idle gap: %s\n", optarg);
            args->req_cfg.idle_gap = atoi(optarg);
            break;

        case 't':
            printf("option connection timeout :%s\n", optarg);
            args->net_cfg.conn_timeout = atoi(optarg);
            break;

        case 'T':
            printf("option recv rate limit: %s\n", optarg);
            args->quic_cfg.recv_rate = atoi(optarg);
            break;

        case 'A':
            printf("option set ALPN[%s]\n", optarg);
            if (strcmp(optarg, "hq") == 0) {
                args->quic_cfg.alpn_type = ALPN_HQ;
                strncpy(args->quic_cfg.alpn, "hq-interop", 11);
            }
            break;

        /* multi connections */
        case 'm':
            printf("option multi connection: on\n");
            switch (atoi(optarg)) {
            case 0:
                args->net_cfg.mode = MODE_SCMR;
                break;
            case 1:
                args->net_cfg.mode = MODE_SCSR_SERIAL;
                break;
            case 2:
                args->net_cfg.mode = MODE_SCSR_CONCURRENT;
            default:
                break;
            }
            break;

        case 'F':
            printf("MTU size: %s\n", optarg);
            args->quic_cfg.max_pkt_sz = atoi(optarg);
            break;

        case 'e':
            printf("option rebinding path after 2s\n");
            args->net_cfg.rebind_path = 1;
            break;

        case 'C':
            printf("option pacing :%s\n", "on");
            args->net_cfg.pacing = 1;
            break;

        case '6':
            printf("option ipv6\n");
            args->net_cfg.ipv6 = 1;
            break;

        default:
            printf("other option :%c\n", ch);
            rqc_demo_cli_usage(argc, argv);
            exit(0);
        }
    }

    if (args->req_cfg.ext_reqn
        && args->req_cfg.request_cnt < args->req_cfg.ext_reqn)
    {
        for (ch = args->req_cfg.request_cnt;
             ch < args->req_cfg.ext_reqn; ch++)
        {
            memcpy(&args->req_cfg.reqs[ch],
                   &args->req_cfg.reqs[ch - 1],
                   sizeof(rqc_demo_cli_request_t));
        }
        args->req_cfg.request_cnt = args->req_cfg.ext_reqn;
    }
}

#define MAX_REQ_BUF_LEN 1500
int
rqc_demo_cli_format_hq_req(char *buf, int len, rqc_demo_cli_request_t* req)
{
    return snprintf(buf, len, "%s", req->path);
    // return snprintf(buf, len, "%s %s\r\n", method_s[req->method], req->path);
}

int
rqc_demo_cli_send_hq_req(rqc_demo_cli_user_conn_t *user_conn,
    rqc_demo_cli_user_stream_t *user_stream, rqc_demo_cli_request_t *req)
{
    /* create request */
    user_stream->hq_request = rqc_hq_request_create(user_conn->ctx->engine, user_conn->hqc_handle,
                                                    &user_conn->cid, user_stream);
    if (user_stream->hq_request == NULL) {
        // printf("user_conn: %p, create stream failed, will try later\n", user_stream);
        return -1;
    }

    /* prepare stream data, which will be sent on callback */
    user_stream->send_buf = calloc(1, MAX_REQ_BUF_LEN);
    user_stream->send_len = rqc_demo_cli_format_hq_req(user_stream->send_buf, MAX_REQ_BUF_LEN, req);
    rqc_demo_cli_hq_req_send(user_stream->hq_request, user_stream);
    return 0;
}

void
rqc_demo_cli_open_file(rqc_demo_cli_user_stream_t *user_stream, const char *save_path,
    const char *req_path)
{
    char file_path[512] = {0};
    snprintf(file_path, sizeof(file_path), "%s%s", save_path, req_path);

    if (user_stream->user_conn->ctx->args->req_cfg.dummy_mode) {
        printf("dummy mode: do not open file[%s]\n", file_path);
        user_stream->recv_body_fp = NULL;
        return;
    }

    user_stream->recv_body_fp = fopen(file_path, "wb");
    if (NULL == user_stream->recv_body_fp) {
        printf("open file[%s] error\n", file_path);
    } else {
        printf("open file[%s] suc\n", file_path);
    }
}

void
rqc_demo_cli_on_task_req_sent(rqc_demo_cli_ctx_t *ctx, int task_id)
{
    ctx->task_ctx.schedule.schedule_info[task_id].req_create_cnt++;
}

void
rqc_demo_cli_send_requests(rqc_demo_cli_user_conn_t *user_conn, rqc_demo_cli_client_args_t *args,
    rqc_demo_cli_request_t *reqs, int req_cnt)
{
    DEBUG;
    for (int i = 0; i < req_cnt; i++) {
        /* user handle of stream */
        rqc_demo_cli_user_stream_t *user_stream = calloc(1, sizeof(rqc_demo_cli_user_stream_t));
        user_stream->user_conn = user_conn;
        // printf("   .. user_stream: %p\n", user_stream);

        /* open save file */
        rqc_demo_cli_open_file(user_stream, args->env_cfg.out_file_dir, reqs[i].path);
        strncpy(user_stream->file_name, reqs[i].path, RESOURCE_LEN - 1);

        /* send request */
        if (args->quic_cfg.alpn_type == ALPN_HQ) {
            if (rqc_demo_cli_send_hq_req(user_conn, user_stream, reqs + i) < 0) {
                printf("send hq req blocked, will try later, total sent_cnt: %d\n",
                    user_conn->ctx->task_ctx.schedule.schedule_info[user_conn->task->task_idx].req_create_cnt);
                free(user_stream);
                return;
            }

        }

        rqc_demo_cli_on_task_req_sent(user_conn->ctx, user_conn->task->task_idx);
    }
}

void
rqc_demo_cli_continue_send_reqs(rqc_demo_cli_user_conn_t *user_conn)
{
    rqc_demo_cli_ctx_t *ctx = user_conn->ctx;
    int task_idx = user_conn->task->task_idx;
    int req_create_cnt = ctx->task_ctx.schedule.schedule_info[task_idx].req_create_cnt;
    int req_cnt = user_conn->task->req_cnt - req_create_cnt;
    if (ctx->args->req_cfg.batch_cnt) {
        req_cnt = req_cnt > ctx->args->req_cfg.batch_cnt ? ctx->args->req_cfg.batch_cnt : req_cnt;
    }
    if (req_cnt > 0) {
        rqc_demo_cli_request_t *reqs = user_conn->task->reqs + req_create_cnt;
        rqc_demo_cli_send_requests(user_conn, ctx->args, reqs, req_cnt);

        if (user_conn->task->req_cnt > req_create_cnt
            && ctx->args->req_cfg.idle_gap
            && ctx->args->req_cfg.batch_cnt)
        {
            struct timeval tv = {
                .tv_sec = ctx->args->req_cfg.idle_gap / 1000,
                .tv_usec = (ctx->args->req_cfg.idle_gap % 1000) * 1000,
            };
            event_add(user_conn->ev_idle_restart, &tv);
        }
    }
}

void
rqc_demo_cli_init_callback(rqc_engine_callback_t *cb, rqc_transport_callbacks_t *transport_cbs,
    rqc_demo_cli_client_args_t* args)
{
    static rqc_engine_callback_t callback = {
        .log_callbacks = {
            .rqc_log_write_err = rqc_demo_cli_write_log_file,
            .rqc_log_write_stat = rqc_demo_cli_write_log_file,
            .rqc_qlog_event_write = rqc_demo_cli_write_qlog_file,
        },
        .set_event_timer = rqc_demo_cli_set_event_timer,
    };

    static rqc_transport_callbacks_t tcb = {
        .write_socket = rqc_demo_cli_write_socket,
    };

    *cb = callback;
    *transport_cbs = tcb;
}

int
rqc_demo_cli_init_alpn_ctx(rqc_demo_cli_ctx_t *ctx)
{
    int ret = 0;

    rqc_hq_callbacks_t hq_cbs = {
        .hqc_cbs = {
            .conn_create_notify = rqc_demo_cli_hq_conn_create_notify,
            .conn_close_notify = rqc_demo_cli_hq_conn_close_notify,
        },
        .hqr_cbs = {
            .req_close_notify = rqc_demo_cli_hq_req_close_notify,
            .req_read_notify = rqc_demo_cli_hq_req_read_notify,
            .req_write_notify = rqc_demo_cli_hq_req_write_notify,
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

int
rqc_demo_cli_init_rquic_engine(rqc_demo_cli_ctx_t *ctx, rqc_demo_cli_client_args_t *args)
{
    rqc_transport_callbacks_t transport_cbs;

    /* init engine callbacks */
    rqc_engine_callback_t callback;
    rqc_demo_cli_init_callback(&callback, &transport_cbs, args);

    rqc_config_t config;
    if (rqc_engine_get_default_config(&config, RQC_ENGINE_CLIENT) < 0) {
        return RQC_ERROR;
    }

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

    ctx->engine = rqc_engine_create(RQC_ENGINE_CLIENT, &config,
                                    &callback, &transport_cbs, ctx);
    if (ctx->engine == NULL) {
        printf("rqc_engine_create error\n");
        return RQC_ERROR;
    }

    if (rqc_demo_cli_init_alpn_ctx(ctx) < 0) {
        printf("init alpn ctx error!");
        return -1;
    }

    return RQC_OK;
}

int
rqc_demo_cli_init_rquic_connection(rqc_demo_cli_user_conn_t *user_conn,
    rqc_demo_cli_client_args_t *args)
{
    /* init connection settings */
    rqc_conn_settings_t conn_settings;
    rqc_demo_cli_init_conneciton_settings(&conn_settings, args);

    if (1) {
        const rqc_cid_t *cid = rqc_hq_connect(user_conn->ctx->engine,
            &conn_settings, args->net_cfg.host,
            (struct sockaddr*)&args->net_cfg.addr, args->net_cfg.addr_len,
            user_conn);

        if (cid == NULL) {
            return -1;
        }

        memcpy(&user_conn->cid, cid, sizeof(rqc_cid_t));
    }

    return 0;
}

void
rqc_demo_cli_start(rqc_demo_cli_user_conn_t *user_conn, rqc_demo_cli_client_args_t *args,
    rqc_demo_cli_request_t *reqs, int req_cnt)
{
    if (RQC_OK != rqc_demo_cli_init_rquic_connection(user_conn, args)) {
        printf("|rqc_demo_cli_start FAILED|\n");
        return;
    }

    if (args->net_cfg.rebind_path) {
        user_conn->ev_rebind_path = event_new(user_conn->ctx->eb, -1, 0,
                                               rqc_demo_cli_rebind_path,
                                               user_conn);
        struct timeval tv = {
            .tv_sec = 2,
            .tv_usec = 0,
        };
        event_add(user_conn->ev_rebind_path, &tv);
    }

    if (args->req_cfg.req_start_delay) {
        user_conn->ev_delay_req = event_new(user_conn->ctx->eb, -1, 0,
                                            rqc_demo_cli_delayed_req_start,
                                            user_conn);
        struct timeval tv = {
            .tv_sec = args->req_cfg.req_start_delay / 1000,
            .tv_usec = (args->req_cfg.req_start_delay % 1000) * 1000,
        };
        event_add(user_conn->ev_delay_req, &tv);

    } else {
        /* TODO: fix MAX_STREAMS bug */
        if (args->req_cfg.batch_cnt) {
            rqc_demo_cli_send_requests(user_conn, args, reqs, req_cnt > args->req_cfg.batch_cnt ? args->req_cfg.batch_cnt : req_cnt);
        } else {
            rqc_demo_cli_send_requests(user_conn, args, reqs, req_cnt);
        }

        if (req_cnt > user_conn->ctx->task_ctx.schedule.schedule_info[user_conn->task->task_idx].req_create_cnt
            && args->req_cfg.idle_gap
            && args->req_cfg.batch_cnt)
        {

            user_conn->ev_idle_restart = event_new(user_conn->ctx->eb, -1, 0,
                                                    rqc_demo_cli_delayed_idle_restart,
                                                    user_conn);
            struct timeval tv = {
                .tv_sec = args->req_cfg.idle_gap / 1000,
                .tv_usec = (args->req_cfg.idle_gap % 1000) * 1000,
            };
            event_add(user_conn->ev_idle_restart, &tv);
        }
    }
}

void
rqc_demo_cli_init_ctx(rqc_demo_cli_ctx_t *pctx, rqc_demo_cli_client_args_t *args)
{
    strncpy(pctx->log_path, args->env_cfg.log_path, sizeof(pctx->log_path) - 1);
    pctx->log_path[sizeof(pctx->log_path) - 1] = '\0';
    pctx->args = args;
    rqc_demo_cli_open_log_file(pctx);
}

int
rqc_demo_cli_all_tasks_finished(rqc_demo_cli_ctx_t *ctx)
{
    for (size_t i = 0; i < ctx->task_ctx.task_cnt; i++) {
        if (ctx->task_ctx.schedule.schedule_info[i].status <= TASK_STATUS_RUNNING) {
            return 0;
        }
    }
    return 1;
}

/* get an waiting task while task scheduler is idle */
int
rqc_demo_cli_get_idle_waiting_task(rqc_demo_cli_ctx_t *ctx)
{
    int waiting_idx = -1;
    int idle_flag = 1;
    for (size_t i = 0; i < ctx->task_ctx.task_cnt; i++) {
        /* if any task is running, break loop, and return no task */
        if (ctx->task_ctx.schedule.schedule_info[i].status == TASK_STATUS_RUNNING) {
            idle_flag = 0;
            break;
        }

        if (waiting_idx < 0
            && ctx->task_ctx.schedule.schedule_info[i].status == TASK_STATUS_WAITTING)
        {
            /* mark the first idle task */
            waiting_idx = i;
        }

    }

    return idle_flag ? waiting_idx : -1;
}

static int
rqc_demo_cli_create_socket(rqc_demo_cli_user_path_t *user_path, rqc_demo_cli_net_config_t* cfg)
{
    int size;
    int fd = 0;
    struct sockaddr *addr = (struct sockaddr*)&cfg->addr;
    fd = socket(addr->sa_family, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("create socket failed, errno: %d\n", get_sys_errno());
        return -1;
    }
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
    size = 1 * 1024 * 1024;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(int)) < 0) {
        printf("setsockopt failed, errno: %d\n", get_sys_errno());
        goto err;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(int)) < 0) {
        printf("setsockopt failed, errno: %d\n", get_sys_errno());
        goto err;
    }

    user_path->last_sock_op_time = rqc_now();

    return fd;

err:
    close(fd);
    return -1;
}

int
rqc_demo_cli_init_user(rqc_demo_cli_user_conn_t *user_conn, uint64_t path_id)
{
    rqc_demo_cli_ctx_t *ctx = user_conn->ctx;
    rqc_demo_cli_user_path_t *user_path = &user_conn->path;

    /* create the initial path */
    user_path->fd = rqc_demo_cli_create_socket(user_path, &ctx->args->net_cfg);
    if (user_path->fd < 0) {
        printf("rqc_create_socket error\n");
        return -1;
    }

    user_path->rebind_fd = -1;
    user_path->ev_rebind_socket = NULL;

    if (ctx->args->net_cfg.rebind_path) {
        user_path->rebind_fd = rqc_demo_cli_create_socket(user_path, &ctx->args->net_cfg);
        if (user_path->rebind_fd < 0) {
            printf("rqc_create_rebind_socket error\n");
            return -1;
        }
    }

    /* socket event */
    user_path->ev_socket = event_new(ctx->eb, user_path->fd, EV_READ | EV_PERSIST,
                                     rqc_demo_cli_socket_event_callback, user_conn);
    event_add(user_path->ev_socket, NULL);

    if (user_path->rebind_fd != -1) {
        user_path->ev_rebind_socket = event_new(ctx->eb, user_path->rebind_fd, EV_READ | EV_PERSIST,
                                                rqc_demo_cli_socket_event_callback, user_conn);
        event_add(user_path->ev_rebind_socket, NULL);
    }

    /* rquic timer */
    user_path->ev_timeout = event_new(ctx->eb, -1, 0, rqc_demo_cli_idle_callback, user_path);
    struct timeval tv;
    tv.tv_sec = ctx->args->net_cfg.conn_timeout;
    tv.tv_usec = 0;
    event_add(user_path->ev_timeout, &tv);

    user_path->is_active = 1;
    user_path->user_conn = user_conn;
    user_path->path_id = path_id;

    printf("Path created id = %"PRIu64"\n", path_id);

    return 0;
}

/* create one connection, send multi reqs in multi streams */
int
rqc_demo_cli_handle_task(rqc_demo_cli_ctx_t *ctx, rqc_demo_cli_task_t *task)
{
    DEBUG;
    /* create socket and connection callback user data */
    rqc_demo_cli_user_conn_t *user_conn = calloc(1, sizeof(rqc_demo_cli_user_conn_t));
    user_conn->ctx = ctx;
    user_conn->task = task;

    /* init the first path */
    if (rqc_demo_cli_init_user(user_conn, 0)) {
        return -1;
    }

    /* start client */
    rqc_demo_cli_start(user_conn, ctx->args, task->reqs, task->req_cnt);

    task->user_conn = user_conn;
    return 0;
}

static struct timeval tv_task_schedule = {0, 100};

/*
 * the task schedule timer callback, will break the main event loop
 * when all tasks are responsed or closed
 * under multi-connction mode, if previous task has finished, will
 * start a new connection and task.
 */
static void
rqc_demo_cli_task_schedule_callback(int fd, short what, void *arg)
{
    rqc_demo_cli_ctx_t *ctx = (rqc_demo_cli_ctx_t*)arg;
    uint8_t all_task_fin_flag = 1;
    uint8_t idle_flag = 1;
    int idle_waiting_task_id = -1;

    for (size_t i = 0; i < ctx->task_ctx.task_cnt; i++) {

        /* check if all tasks are finished */
        if (ctx->task_ctx.schedule.schedule_info[i].status <= TASK_STATUS_RUNNING) {
            all_task_fin_flag = 0;
        }

        /* record the first waiting task */
        if (idle_waiting_task_id == -1
            && ctx->task_ctx.schedule.schedule_info[i].status == TASK_STATUS_WAITTING)
        {
            idle_waiting_task_id = i;
        }
    }

    if (all_task_fin_flag) {
        printf("all tasks are finished, will break loop and exit\n\n");
        event_base_loopbreak(ctx->eb);
        return;
    }

    /* if idle and got a waiting task, run the task */
    if (idle_flag && idle_waiting_task_id >= 0) {
        /* handle task and set status to RUNNING */
        int ret = rqc_demo_cli_handle_task(ctx, ctx->task_ctx.tasks + idle_waiting_task_id);
        if (0 == ret) {
            ctx->task_ctx.schedule.schedule_info[idle_waiting_task_id].status = TASK_STATUS_RUNNING;

        } else {
            ctx->task_ctx.schedule.schedule_info[idle_waiting_task_id].status = TASK_STATUS_FAILED;
        }
    }

    /* start next round */
    event_add(ctx->ev_task, &tv_task_schedule);
}

void
rqc_demo_cli_init_scmr(rqc_demo_cli_task_ctx_t *tctx, rqc_demo_cli_client_args_t *args)
{
    tctx->task_cnt = 1; /* one task, one connection, all requests */

    /* init task list */
    tctx->tasks = calloc(1, sizeof(rqc_demo_cli_task_t) * 1);
    tctx->tasks->req_cnt = args->req_cfg.request_cnt;
    tctx->tasks->reqs = args->req_cfg.reqs;

    /* init schedule */
    tctx->schedule.schedule_info = calloc(1, sizeof(rqc_demo_cli_task_schedule_info_t) * 1);
}

void
rqc_demo_cli_init_scsr(rqc_demo_cli_task_ctx_t *tctx, rqc_demo_cli_client_args_t *args)
{
    tctx->task_cnt = args->req_cfg.request_cnt;

    /* init task list */
    tctx->tasks = calloc(1, sizeof(rqc_demo_cli_task_t) * tctx->task_cnt);
    for (int i = 0; i < tctx->task_cnt; i++) {
        tctx->tasks[i].task_idx = i;
        tctx->tasks[i].req_cnt = 1;
        tctx->tasks[i].reqs = (rqc_demo_cli_request_t*)args->req_cfg.reqs + i;
    }

    /* init schedule */
    tctx->schedule.schedule_info = calloc(1, sizeof(rqc_demo_cli_task_schedule_info_t) * tctx->task_cnt);
}

/* create task info according to args */
void
rqc_demo_cli_init_tasks(rqc_demo_cli_ctx_t *ctx)
{
    ctx->task_ctx.mode = ctx->args->net_cfg.mode;
    switch (ctx->args->net_cfg.mode) {
    case MODE_SCMR:
        rqc_demo_cli_init_scmr(&ctx->task_ctx, ctx->args);
        break;

    case MODE_SCSR_SERIAL:
    case MODE_SCSR_CONCURRENT:
        rqc_demo_cli_init_scsr(&ctx->task_ctx, ctx->args);
        break;

    default:
        break;
    }
}

/* prevent from endless task, this could be used if execution time is limited */
static void
rqc_demo_cli_kill_it_any_way_callback(int fd, short what, void *arg)
{
    rqc_demo_cli_ctx_t *ctx = (rqc_demo_cli_ctx_t*)arg;
    event_base_loopbreak(ctx->eb);
    printf("[* tasks are running more than %d seconds, kill it anyway! *]\n",
        ctx->args->env_cfg.life);
}

void
rqc_demo_cli_start_task_manager(rqc_demo_cli_ctx_t *ctx)
{
    rqc_demo_cli_init_tasks(ctx);

    /* init and arm task timer */
    ctx->ev_task = event_new(ctx->eb, -1, 0, rqc_demo_cli_task_schedule_callback, ctx);

    /* immediate engage task */
    rqc_demo_cli_task_schedule_callback(-1, 0, ctx);

    /* kill it anyway, to protect from endless task */
    if (ctx->args->env_cfg.life > 0) {
        struct timeval tv_kill_it_anyway = {ctx->args->env_cfg.life, 0};
        ctx->ev_kill = event_new(ctx->eb, -1, 0, rqc_demo_cli_kill_it_any_way_callback, ctx);
        event_add(ctx->ev_kill, &tv_kill_it_anyway);
    }
}

void
rqc_demo_cli_free_ctx(rqc_demo_cli_ctx_t *ctx)
{
    rqc_demo_cli_close_log_file(ctx);

    if (ctx->args) {
        free(ctx->args);
        ctx->args = NULL;
    }

    free(ctx);
}

int
main(int argc, char *argv[])
{
    /* init env if necessary */
    rqc_platform_init_env();

    /* get input client args */
    rqc_demo_cli_client_args_t *args = calloc(1, sizeof(rqc_demo_cli_client_args_t));
    rqc_demo_cli_init_args(args);
    rqc_demo_cli_parse_args(argc, argv, args);

    /* init client ctx */
    rqc_demo_cli_ctx_t *ctx = calloc(1, sizeof(rqc_demo_cli_ctx_t));
    rqc_demo_cli_init_ctx(ctx, args);

    /* engine event */
    ctx->eb = event_base_new();
    ctx->ev_engine = event_new(ctx->eb, -1, 0, rqc_demo_cli_engine_callback, ctx);
    rqc_demo_cli_init_rquic_engine(ctx, args);

    /* start task scheduler */
    rqc_demo_cli_start_task_manager(ctx);

    event_base_dispatch(ctx->eb);

    rqc_engine_destroy(ctx->engine);
    rqc_demo_cli_free_ctx(ctx);
    return 0;
}
