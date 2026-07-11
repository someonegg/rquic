#ifndef DEMO_HQ_H
#define DEMO_HQ_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include <rquic/rquic.h>

typedef struct demo_hq_conn_s demo_hq_conn_t;
typedef struct demo_hq_stream_s demo_hq_stream_t;
typedef struct demo_hq_adapter_s demo_hq_adapter_t;

typedef int (*demo_hq_conn_notify_pt)(demo_hq_conn_t *conn,
    const rqc_cid_t *cid, void *conn_user_data);
typedef int (*demo_hq_stream_notify_pt)(demo_hq_stream_t *stream,
    void *stream_user_data);

typedef struct demo_hq_callbacks_s {
    demo_hq_conn_notify_pt conn_create;
    demo_hq_conn_notify_pt conn_close;
    demo_hq_stream_notify_pt stream_create;
    demo_hq_stream_notify_pt stream_close;
    demo_hq_stream_notify_pt stream_read;
    demo_hq_stream_notify_pt stream_write;
} demo_hq_callbacks_t;

demo_hq_adapter_t *demo_hq_register(rqc_engine_t *engine,
    const demo_hq_callbacks_t *callbacks);
void demo_hq_adapter_unregister(demo_hq_adapter_t *adapter);
void demo_hq_adapter_destroy(demo_hq_adapter_t *adapter);

const rqc_cid_t *demo_hq_connect(rqc_engine_t *engine,
    const rqc_conn_settings_t *conn_settings, const char *server_host,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *user_data);

int demo_hq_conn_close(rqc_engine_t *engine, const rqc_cid_t *cid);
void demo_hq_conn_set_user_data(demo_hq_conn_t *conn, void *user_data);
void *demo_hq_conn_get_user_data(demo_hq_conn_t *conn);
int demo_hq_conn_get_peer_addr(demo_hq_conn_t *conn, struct sockaddr *addr,
    socklen_t addr_cap, socklen_t *peer_addr_len);

demo_hq_stream_t *demo_hq_stream_create(rqc_engine_t *engine,
    demo_hq_conn_t *conn, const rqc_cid_t *cid, void *user_data);
void demo_hq_stream_set_user_data(demo_hq_stream_t *stream, void *user_data);
void *demo_hq_stream_get_user_data(demo_hq_stream_t *stream);
void *demo_hq_stream_get_conn_user_data(demo_hq_stream_t *stream);

int demo_hq_stream_set_request(demo_hq_stream_t *stream, const char *resource);
ssize_t demo_hq_stream_send_pending_request(demo_hq_stream_t *stream);

int demo_hq_stream_set_response(demo_hq_stream_t *stream, const unsigned char *data,
    size_t data_len);
ssize_t demo_hq_stream_send_pending_response(demo_hq_stream_t *stream);
ssize_t demo_hq_stream_send_response_chunk(demo_hq_stream_t *stream,
    const unsigned char *data, size_t data_len, uint8_t fin);

ssize_t demo_hq_stream_recv_request(demo_hq_stream_t *stream, char *resource,
    size_t resource_cap, uint8_t *fin);
ssize_t demo_hq_stream_recv_response(demo_hq_stream_t *stream, unsigned char *buf,
    size_t buf_cap, uint8_t *fin);

rqc_stream_stats_t demo_hq_stream_get_stats(demo_hq_stream_t *stream);

#endif
