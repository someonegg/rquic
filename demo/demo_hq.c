#include "demo_hq.h"

#include "demo_common.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rquic/rqc_errno.h>

#define DEMO_HQ_ALPN_V1 "hq-interop"
#define DEMO_HQ_ALPN_V1_LEN 10
#define DEMO_HQ_ALPN_29 "hq-29"
#define DEMO_HQ_ALPN_29_LEN 5
#define DEMO_HQ_REQUEST_PREFIX "GET "
#define DEMO_HQ_REQUEST_SUFFIX "\r\n"
#define DEMO_HQ_REQUEST_MAX_LEN 1024

typedef struct demo_hq_ctx_s {
    demo_hq_callbacks_t callbacks;
} demo_hq_ctx_t;

struct demo_hq_adapter_s {
    rqc_engine_t *engine;
    demo_hq_ctx_t ctx;
};

static demo_hq_ctx_t *demo_hq_client_ctx;

struct demo_hq_conn_s {
    rqc_connection_t *conn;
    demo_hq_ctx_t *ctx;
    void *user_data;
};

struct demo_hq_stream_s {
    rqc_stream_t *stream;
    demo_hq_conn_t *conn;
    demo_hq_ctx_t *ctx;
    void *user_data;

    unsigned char *request_buf;
    size_t request_len;
    size_t request_off;

    unsigned char *response_buf;
    size_t response_len;
    size_t response_off;

    unsigned char *recv_req_buf;
    size_t recv_req_len;
    uint8_t recv_req_fin;
};

static int demo_hq_conn_create_notify(rqc_connection_t *conn, const rqc_cid_t *cid,
    void *conn_user_data, void *conn_proto_data,
    const rqc_proto_ext_t *proto_ext, rqc_proto_ext_t *resp_proto_ext);
static int demo_hq_conn_close_notify(rqc_connection_t *conn, const rqc_cid_t *cid,
    void *conn_user_data, void *conn_proto_data);
static void demo_hq_conn_handshake_finished(rqc_connection_t *conn,
    void *conn_user_data, void *conn_proto_data);
static int demo_hq_stream_create_notify(rqc_stream_t *stream, void *stream_user_data);
static int demo_hq_stream_close_notify(rqc_stream_t *stream, void *stream_user_data);
static int demo_hq_stream_read_notify(rqc_stream_t *stream, void *stream_user_data);
static int demo_hq_stream_write_notify(rqc_stream_t *stream, void *stream_user_data);

static rqc_app_proto_callbacks_t demo_hq_proto_callbacks = {
    .conn_cbs = {
        .conn_create_notify = demo_hq_conn_create_notify,
        .conn_close_notify = demo_hq_conn_close_notify,
        .conn_handshake_finished = demo_hq_conn_handshake_finished,
    },
    .stream_cbs = {
        .stream_create_notify = demo_hq_stream_create_notify,
        .stream_close_notify = demo_hq_stream_close_notify,
        .stream_read_notify = demo_hq_stream_read_notify,
        .stream_write_notify = demo_hq_stream_write_notify,
    },
};

static const char *
demo_hq_alpn_for_version(rqc_proto_version_t version)
{
    switch (version) {
    case RQC_IDRAFT_VER_29:
        return DEMO_HQ_ALPN_29;
    case RQC_VERSION_V1:
    default:
        return DEMO_HQ_ALPN_V1;
    }
}

static void
demo_hq_stream_destroy(demo_hq_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }

    free(stream->request_buf);
    free(stream->response_buf);
    free(stream->recv_req_buf);
    free(stream);
}

static int
demo_hq_register_one(rqc_engine_t *engine, const char *alpn, size_t alpn_len,
    demo_hq_ctx_t *ctx)
{
    if (rqc_engine_register_alpn(engine, alpn, alpn_len, &demo_hq_proto_callbacks, ctx)
        != RQC_OK)
    {
        return -RQC_EFATAL;
    }

    if (demo_hq_client_ctx == NULL) {
        demo_hq_client_ctx = ctx;
    }

    return RQC_OK;
}

static void
demo_hq_unregister_one(rqc_engine_t *engine, const char *alpn, size_t alpn_len,
    demo_hq_ctx_t *ctx)
{
    if (ctx != NULL && rqc_engine_get_alpn_ctx(engine, alpn, alpn_len) == ctx) {
        if (demo_hq_client_ctx == ctx) {
            demo_hq_client_ctx = NULL;
        }
    }
    (void)rqc_engine_unregister_alpn(engine, alpn, alpn_len);
}

demo_hq_adapter_t *
demo_hq_register(rqc_engine_t *engine, const demo_hq_callbacks_t *callbacks)
{
    demo_hq_adapter_t *adapter;
    int ret;

    if (engine == NULL || callbacks == NULL) {
        return NULL;
    }

    adapter = calloc(1, sizeof(*adapter));
    if (adapter == NULL) {
        return NULL;
    }
    adapter->engine = engine;
    adapter->ctx.callbacks = *callbacks;

    ret = demo_hq_register_one(engine, DEMO_HQ_ALPN_V1, DEMO_HQ_ALPN_V1_LEN,
        &adapter->ctx);
    if (ret != RQC_OK) {
        demo_hq_adapter_destroy(adapter);
        return NULL;
    }

    ret = demo_hq_register_one(engine, DEMO_HQ_ALPN_29, DEMO_HQ_ALPN_29_LEN,
        &adapter->ctx);
    if (ret != RQC_OK) {
        demo_hq_unregister_one(engine, DEMO_HQ_ALPN_V1, DEMO_HQ_ALPN_V1_LEN,
            &adapter->ctx);
        demo_hq_adapter_destroy(adapter);
        return NULL;
    }

    return adapter;
}

void
demo_hq_adapter_unregister(demo_hq_adapter_t *adapter)
{
    if (adapter == NULL) {
        return;
    }
    if (adapter->engine != NULL) {
        demo_hq_unregister_one(adapter->engine, DEMO_HQ_ALPN_V1,
            DEMO_HQ_ALPN_V1_LEN, &adapter->ctx);
        demo_hq_unregister_one(adapter->engine, DEMO_HQ_ALPN_29,
            DEMO_HQ_ALPN_29_LEN, &adapter->ctx);
        adapter->engine = NULL;
    }
    if (demo_hq_client_ctx == &adapter->ctx) {
        demo_hq_client_ctx = NULL;
    }
}

void
demo_hq_adapter_destroy(demo_hq_adapter_t *adapter)
{
    if (adapter == NULL) {
        return;
    }
    demo_hq_adapter_unregister(adapter);
    free(adapter);
}

const rqc_cid_t *
demo_hq_connect(rqc_engine_t *engine, const rqc_conn_settings_t *conn_settings,
    const char *server_host, const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *user_data)
{
    const char *alpn;

    if (conn_settings == NULL) {
        return NULL;
    }

    alpn = demo_hq_alpn_for_version(conn_settings->proto_version);
    return rqc_connect(engine, conn_settings, server_host, alpn, NULL,
        peer_addr, peer_addrlen, user_data);
}

int
demo_hq_conn_close(rqc_engine_t *engine, const rqc_cid_t *cid)
{
    return rqc_conn_close(engine, cid);
}

void
demo_hq_conn_set_user_data(demo_hq_conn_t *conn, void *user_data)
{
    if (conn != NULL) {
        conn->user_data = user_data;
    }
}

void *
demo_hq_conn_get_user_data(demo_hq_conn_t *conn)
{
    return conn != NULL ? conn->user_data : NULL;
}

int
demo_hq_conn_get_peer_addr(demo_hq_conn_t *conn, struct sockaddr *addr,
    socklen_t addr_cap, socklen_t *peer_addr_len)
{
    if (conn == NULL) {
        return -RQC_EPARAM;
    }
    return rqc_conn_get_peer_addr(conn->conn, addr, addr_cap, peer_addr_len);
}

demo_hq_stream_t *
demo_hq_stream_create(rqc_engine_t *engine, demo_hq_conn_t *conn,
    const rqc_cid_t *cid, void *user_data)
{
    demo_hq_stream_t *stream = calloc(1, sizeof(*stream));
    if (stream == NULL) {
        return NULL;
    }

    stream->conn = conn;
    stream->ctx = conn->ctx;
    stream->user_data = user_data;
    stream->stream = rqc_stream_create(engine, cid, NULL, stream);
    if (stream->stream == NULL) {
        demo_hq_stream_destroy(stream);
        return NULL;
    }

    return stream;
}

void
demo_hq_stream_set_user_data(demo_hq_stream_t *stream, void *user_data)
{
    if (stream != NULL) {
        stream->user_data = user_data;
    }
}

void *
demo_hq_stream_get_user_data(demo_hq_stream_t *stream)
{
    return stream != NULL ? stream->user_data : NULL;
}

void *
demo_hq_stream_get_conn_user_data(demo_hq_stream_t *stream)
{
    return stream != NULL && stream->conn != NULL ? stream->conn->user_data : NULL;
}

static int
demo_hq_replace_buffer(unsigned char **buf, size_t *len, size_t *off,
    const unsigned char *data, size_t data_len)
{
    unsigned char *new_buf = NULL;

    if (data_len > 0) {
        new_buf = malloc(data_len);
        if (new_buf == NULL) {
            return -RQC_EMALLOC;
        }
        memcpy(new_buf, data, data_len);
    }

    free(*buf);
    *buf = new_buf;
    *len = data_len;
    *off = 0;
    return RQC_OK;
}

int
demo_hq_stream_set_request(demo_hq_stream_t *stream, const char *resource)
{
    char errbuf[128];
    size_t resource_len;
    size_t request_len;
    unsigned char *buf;
    int ret;

    if (stream == NULL || resource == NULL) {
        return -RQC_EPARAM;
    }

    if (demo_validate_resource_path(resource, errbuf, sizeof(errbuf)) != 0) {
        (void)fprintf(stderr, "invalid request path: %s\n", errbuf);
        return -RQC_EPARAM;
    }

    resource_len = strlen(resource);
    request_len = strlen(DEMO_HQ_REQUEST_PREFIX) + resource_len
        + strlen(DEMO_HQ_REQUEST_SUFFIX);
    buf = malloc(request_len + 1);
    if (buf == NULL) {
        return -RQC_EMALLOC;
    }

    (void)snprintf((char *)buf, request_len + 1, "%s%s%s",
        DEMO_HQ_REQUEST_PREFIX, resource, DEMO_HQ_REQUEST_SUFFIX);
    ret = demo_hq_replace_buffer(&stream->request_buf, &stream->request_len,
        &stream->request_off, buf, request_len);
    free(buf);
    return ret;
}

static ssize_t
demo_hq_send_pending(rqc_stream_t *rqc_stream, unsigned char *buf, size_t len,
    size_t *off, uint8_t flush)
{
    ssize_t ret;
    size_t sent = 0;

    if (rqc_stream == NULL || off == NULL) {
        return -RQC_EPARAM;
    }

    while (*off < len) {
        uint8_t fin = (*off + (len - *off)) == len ? 1 : 0;
        ret = rqc_stream_send(rqc_stream, buf + *off, len - *off, fin, flush);
        if (ret == -RQC_EAGAIN) {
            return (ssize_t)sent;
        }
        if (ret < 0) {
            return ret;
        }
        if (ret == 0) {
            return (ssize_t)sent;
        }

        *off += (size_t)ret;
        sent += (size_t)ret;
    }

    return (ssize_t)sent;
}

ssize_t
demo_hq_stream_send_pending_request(demo_hq_stream_t *stream)
{
    if (stream == NULL || stream->request_buf == NULL) {
        return -RQC_EPARAM;
    }

    return demo_hq_send_pending(stream->stream, stream->request_buf,
        stream->request_len, &stream->request_off, 1);
}

int
demo_hq_stream_set_response(demo_hq_stream_t *stream, const unsigned char *data,
    size_t data_len)
{
    if (stream == NULL || (data == NULL && data_len != 0)) {
        return -RQC_EPARAM;
    }

    return demo_hq_replace_buffer(&stream->response_buf, &stream->response_len,
        &stream->response_off, data, data_len);
}

ssize_t
demo_hq_stream_send_pending_response(demo_hq_stream_t *stream)
{
    if (stream == NULL || (stream->response_buf == NULL && stream->response_len != 0)) {
        return -RQC_EPARAM;
    }

    if (stream->response_len == 0) {
        return rqc_stream_send(stream->stream, (unsigned char *)"", 0, 1, 1);
    }

    return demo_hq_send_pending(stream->stream, stream->response_buf,
        stream->response_len, &stream->response_off, 1);
}

ssize_t
demo_hq_stream_send_response_chunk(demo_hq_stream_t *stream,
    const unsigned char *data, size_t data_len, uint8_t fin)
{
    if (stream == NULL || (data == NULL && data_len != 0)) {
        return -RQC_EPARAM;
    }

    return rqc_stream_send(stream->stream, (unsigned char *)data, data_len, fin, 0);
}

static ssize_t
demo_hq_parse_request(demo_hq_stream_t *stream, char *resource,
    size_t resource_cap, uint8_t *fin)
{
    const char *start;
    const char *end;
    size_t path_len;

    if (stream->recv_req_len < strlen(DEMO_HQ_REQUEST_PREFIX)) {
        return RQC_OK;
    }

    if (memcmp(stream->recv_req_buf, DEMO_HQ_REQUEST_PREFIX,
            strlen(DEMO_HQ_REQUEST_PREFIX)) != 0)
    {
        return -RQC_EPROTO;
    }

    start = (const char *)stream->recv_req_buf + strlen(DEMO_HQ_REQUEST_PREFIX);
    end = strstr(start, DEMO_HQ_REQUEST_SUFFIX);
    if (end == NULL) {
        if (stream->recv_req_fin) {
            return -RQC_EPROTO;
        }
        return RQC_OK;
    }

    path_len = (size_t)(end - start);
    if (path_len + 1 > resource_cap) {
        return -RQC_ENOBUF;
    }

    memcpy(resource, start, path_len);
    resource[path_len] = '\0';
    if (demo_validate_resource_path(resource, NULL, 0) != 0) {
        return -RQC_EPARAM;
    }

    *fin = 1;
    return (ssize_t)path_len;
}

ssize_t
demo_hq_stream_recv_request(demo_hq_stream_t *stream, char *resource,
    size_t resource_cap, uint8_t *fin)
{
    uint8_t stream_fin = 0;
    ssize_t read = 0;

    if (stream == NULL || resource == NULL || resource_cap == 0 || fin == NULL) {
        return -RQC_EPARAM;
    }

    *fin = 0;
    if (stream->recv_req_buf == NULL) {
        stream->recv_req_buf = malloc(DEMO_HQ_REQUEST_MAX_LEN + 1);
        if (stream->recv_req_buf == NULL) {
            return -RQC_EMALLOC;
        }
    }

    do {
        size_t cap = DEMO_HQ_REQUEST_MAX_LEN - stream->recv_req_len;
        if (cap == 0) {
            return -RQC_ENOBUF;
        }

        read = rqc_stream_recv(stream->stream, stream->recv_req_buf + stream->recv_req_len,
            cap, &stream_fin);
        if (read == -RQC_EAGAIN) {
            break;
        }
        if (read < 0) {
            return read;
        }

        stream->recv_req_len += (size_t)read;
        stream->recv_req_buf[stream->recv_req_len] = '\0';
        if (stream_fin) {
            stream->recv_req_fin = 1;
        }
    } while (read > 0 && !stream_fin);

    return demo_hq_parse_request(stream, resource, resource_cap, fin);
}

ssize_t
demo_hq_stream_recv_response(demo_hq_stream_t *stream, unsigned char *buf,
    size_t buf_cap, uint8_t *fin)
{
    if (stream == NULL || buf == NULL || buf_cap == 0 || fin == NULL) {
        return -RQC_EPARAM;
    }

    return rqc_stream_recv(stream->stream, buf, buf_cap, fin);
}

rqc_stream_stats_t
demo_hq_stream_get_stats(demo_hq_stream_t *stream)
{
    if (stream == NULL) {
        rqc_stream_stats_t stats;
        memset(&stats, 0, sizeof(stats));
        return stats;
    }

    return rqc_stream_get_stats(stream->stream);
}

static int
demo_hq_conn_create_notify(rqc_connection_t *conn, const rqc_cid_t *cid,
    void *conn_user_data, void *conn_proto_data,
    const rqc_proto_ext_t *proto_ext, rqc_proto_ext_t *resp_proto_ext)
{
    demo_hq_ctx_t *ctx = (demo_hq_ctx_t *)conn_proto_data;
    demo_hq_conn_t *hq_conn;

    (void)proto_ext;
    (void)resp_proto_ext;

    if (ctx == NULL) {
        ctx = demo_hq_client_ctx;
    }

    if (ctx == NULL) {
        return -RQC_EFATAL;
    }

    hq_conn = calloc(1, sizeof(*hq_conn));
    if (hq_conn == NULL) {
        return -RQC_EMALLOC;
    }

    hq_conn->conn = conn;
    hq_conn->ctx = ctx;
    hq_conn->user_data = conn_user_data;
    rqc_conn_set_alp_user_data(conn, hq_conn);

    if (ctx->callbacks.conn_create != NULL) {
        int ret = ctx->callbacks.conn_create(hq_conn, cid, hq_conn->user_data);
        if (ret != RQC_OK) {
            rqc_conn_set_alp_user_data(conn, NULL);
            free(hq_conn);
            return ret;
        }
    }

    return RQC_OK;
}

static int
demo_hq_conn_close_notify(rqc_connection_t *conn, const rqc_cid_t *cid,
    void *conn_user_data, void *conn_proto_data)
{
    demo_hq_conn_t *hq_conn = (demo_hq_conn_t *)conn_proto_data;
    int ret = RQC_OK;

    (void)conn;
    (void)conn_user_data;

    if (hq_conn == NULL) {
        return RQC_OK;
    }

    if (hq_conn->ctx != NULL && hq_conn->ctx->callbacks.conn_close != NULL) {
        ret = hq_conn->ctx->callbacks.conn_close(hq_conn, cid, hq_conn->user_data);
    }

    free(hq_conn);
    return ret;
}

static void
demo_hq_conn_handshake_finished(rqc_connection_t *conn, void *conn_user_data,
    void *conn_proto_data)
{
    (void)conn;
    (void)conn_user_data;
    (void)conn_proto_data;
}

static int
demo_hq_stream_create_notify(rqc_stream_t *rqc_stream, void *stream_user_data)
{
    demo_hq_stream_t *stream = (demo_hq_stream_t *)stream_user_data;
    int created_here = 0;

    if (stream == NULL) {
        demo_hq_conn_t *conn = rqc_get_conn_alp_user_data_by_stream(rqc_stream);
        if (conn == NULL) {
            return -RQC_EFATAL;
        }

        stream = calloc(1, sizeof(*stream));
        if (stream == NULL) {
            return -RQC_EMALLOC;
        }

        stream->stream = rqc_stream;
        stream->conn = conn;
        stream->ctx = conn->ctx;
        rqc_stream_set_user_data(rqc_stream, stream);
        created_here = 1;
    }

    if (stream->ctx != NULL && stream->ctx->callbacks.stream_create != NULL) {
        int ret = stream->ctx->callbacks.stream_create(stream, stream->user_data);
        if (ret != RQC_OK && created_here) {
            rqc_stream_set_user_data(rqc_stream, NULL);
            demo_hq_stream_destroy(stream);
        }
        return ret;
    }

    return RQC_OK;
}

static int
demo_hq_stream_close_notify(rqc_stream_t *rqc_stream, void *stream_user_data)
{
    demo_hq_stream_t *stream = (demo_hq_stream_t *)stream_user_data;
    int ret = RQC_OK;

    (void)rqc_stream;

    if (stream == NULL) {
        return RQC_OK;
    }

    if (stream->ctx != NULL && stream->ctx->callbacks.stream_close != NULL) {
        ret = stream->ctx->callbacks.stream_close(stream, stream->user_data);
    }

    demo_hq_stream_destroy(stream);
    return ret;
}

static int
demo_hq_stream_read_notify(rqc_stream_t *rqc_stream, void *stream_user_data)
{
    demo_hq_stream_t *stream = (demo_hq_stream_t *)stream_user_data;

    (void)rqc_stream;

    if (stream != NULL && stream->ctx != NULL && stream->ctx->callbacks.stream_read != NULL) {
        return stream->ctx->callbacks.stream_read(stream, stream->user_data);
    }

    return RQC_OK;
}

static int
demo_hq_stream_write_notify(rqc_stream_t *rqc_stream, void *stream_user_data)
{
    demo_hq_stream_t *stream = (demo_hq_stream_t *)stream_user_data;

    (void)rqc_stream;

    if (stream != NULL && stream->ctx != NULL && stream->ctx->callbacks.stream_write != NULL) {
        return stream->ctx->callbacks.stream_write(stream, stream->user_data);
    }

    return RQC_OK;
}
