#include <rquic/rquic.h>
#include <rquic/rqc_errno.h>

#include "src/transport/rqc_cid.h"
#include "src/transport/rqc_client.h"
#include "src/transport/rqc_conn.h"
#include "src/transport/rqc_engine.h"
#include "src/transport/rqc_frame_parser.h"
#include "src/transport/rqc_packet_in.h"
#include "src/transport/rqc_packet_out.h"
#include "src/transport/rqc_send_queue.h"
#include "src/transport/rqc_stream.h"
#include "src/transport/rqc_transport_params.h"

#include <stdio.h>
#include <string.h>

#define TEST_ALPN "rqc-test"

typedef enum test_resp_proto_ext_mode_e {
    TEST_RESP_PROTO_EXT_NONE = 0,
    TEST_RESP_PROTO_EXT_NORMAL,
    TEST_RESP_PROTO_EXT_MAXIMUM,
    TEST_RESP_PROTO_EXT_OVERSIZED,
} test_resp_proto_ext_mode_t;

typedef struct test_ctx_s {
    int handshake_finished;
    int conn_create;
    int timer_calls;
    int socket_writes;
    int stream_write_notifies;
    rqc_usec_t wake_after;
    unsigned char *mutate_on_socket_write;
    size_t mutate_on_socket_write_len;
    int conn_create_ret;
    test_resp_proto_ext_mode_t resp_proto_ext_mode;
    uint8_t *resp_proto_ext_data;
    size_t resp_proto_ext_len_on_entry;
} test_ctx_t;

typedef struct test_engine_s {
    rqc_engine_t *engine;
    test_ctx_t ctx;
} test_engine_t;

static rqc_connection_t *create_client_conn(test_engine_t *eng,
    const uint8_t *proto_ext, size_t proto_ext_len);
static rqc_connection_t *create_client_conn_with_cids(test_engine_t *eng,
    uint8_t dcid_base, uint8_t scid_base);

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return -1; \
    } \
} while (0)

#define CHECK_EQ(actual, expected) do { \
    long long _a = (long long)(actual); \
    long long _e = (long long)(expected); \
    if (_a != _e) { \
        fprintf(stderr, "CHECK_EQ failed at %s:%d: %s=%lld expected %lld\n", \
                __FILE__, __LINE__, #actual, _a, _e); \
        return -1; \
    } \
} while (0)

#define CHECK_NE(actual, expected) do { \
    long long _a = (long long)(actual); \
    long long _e = (long long)(expected); \
    if (_a == _e) { \
        fprintf(stderr, "CHECK_NE failed at %s:%d: %s=%lld unexpected %lld\n", \
                __FILE__, __LINE__, #actual, _a, _e); \
        return -1; \
    } \
} while (0)

#define CHECK_GT(actual, expected) do { \
    long long _a = (long long)(actual); \
    long long _e = (long long)(expected); \
    if (_a <= _e) { \
        fprintf(stderr, "CHECK_GT failed at %s:%d: %s=%lld expected > %lld\n", \
                __FILE__, __LINE__, #actual, _a, _e); \
        return -1; \
    } \
} while (0)

static void
test_log_write(rqc_log_level_t lvl, const void *buf, size_t size, void *engine_user_data)
{
    (void)lvl;
    (void)buf;
    (void)size;
    (void)engine_user_data;
}

static void
test_qlog_write(qlog_event_importance_t imp, const void *buf, size_t size, void *engine_user_data)
{
    (void)imp;
    (void)buf;
    (void)size;
    (void)engine_user_data;
}

static void
test_set_event_timer(rqc_usec_t wake_after, void *engine_user_data)
{
    test_ctx_t *ctx = engine_user_data;
    if (ctx != NULL) {
        ctx->timer_calls++;
        ctx->wake_after = wake_after;
    }
}

static ssize_t
test_socket_write(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *conn_user_data)
{
    test_ctx_t *ctx = conn_user_data;
    (void)buf;
    (void)peer_addr;
    (void)peer_addrlen;
    if (ctx != NULL) {
        ctx->socket_writes++;
        if (ctx->mutate_on_socket_write != NULL) {
            memset(ctx->mutate_on_socket_write, 'x', ctx->mutate_on_socket_write_len);
            ctx->mutate_on_socket_write = NULL;
            ctx->mutate_on_socket_write_len = 0;
        }
    }
    return (ssize_t)size;
}

static int
test_conn_create_notify(rqc_connection_t *conn, const rqc_cid_t *cid,
    void *conn_user_data, void *conn_proto_data,
    const rqc_proto_ext_t *proto_ext, rqc_proto_ext_t *resp_proto_ext)
{
    test_ctx_t *ctx = (test_ctx_t *)conn_user_data;
    (void)conn;
    (void)cid;
    (void)conn_proto_data;
    (void)proto_ext;
    if (ctx != NULL) {
        ctx->conn_create++;
        if (resp_proto_ext == NULL) {
            return 0;
        }

        ctx->resp_proto_ext_data = resp_proto_ext->data;
        ctx->resp_proto_ext_len_on_entry = resp_proto_ext->len;
        switch (ctx->resp_proto_ext_mode) {
        case TEST_RESP_PROTO_EXT_NONE:
            resp_proto_ext->len = 0;
            break;
        case TEST_RESP_PROTO_EXT_NORMAL:
            resp_proto_ext->data[0] = 0x31;
            resp_proto_ext->data[1] = 0x32;
            resp_proto_ext->len = 2;
            break;
        case TEST_RESP_PROTO_EXT_MAXIMUM:
            memset(resp_proto_ext->data, 0x5a, RQC_MAX_PROTO_EXT_LEN);
            resp_proto_ext->len = RQC_MAX_PROTO_EXT_LEN;
            break;
        case TEST_RESP_PROTO_EXT_OVERSIZED:
            resp_proto_ext->len = RQC_MAX_PROTO_EXT_LEN + 1;
            break;
        }
    }
    return ctx != NULL ? ctx->conn_create_ret : 0;
}

static int
test_conn_close_notify(rqc_connection_t *conn, const rqc_cid_t *cid,
    void *conn_user_data, void *conn_proto_data)
{
    (void)conn;
    (void)cid;
    (void)conn_user_data;
    (void)conn_proto_data;
    return 0;
}

static void
test_handshake_finished(rqc_connection_t *conn, void *conn_user_data, void *conn_proto_data)
{
    test_ctx_t *ctx = (test_ctx_t *)conn_user_data;
    (void)conn;
    (void)conn_proto_data;
    if (ctx != NULL) {
        ctx->handshake_finished++;
    }
}

static rqc_int_t
test_stream_notify(rqc_stream_t *stream, void *strm_user_data)
{
    (void)stream;
    (void)strm_user_data;
    return RQC_OK;
}

static rqc_int_t
test_stream_write_notify(rqc_stream_t *stream, void *strm_user_data)
{
    test_ctx_t *ctx = strm_user_data;
    (void)stream;
    if (ctx != NULL) {
        ctx->stream_write_notifies++;
    }
    return RQC_OK;
}

static rqc_app_proto_callbacks_t
test_app_cbs(void)
{
    rqc_app_proto_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.conn_cbs.conn_create_notify = test_conn_create_notify;
    cbs.conn_cbs.conn_close_notify = test_conn_close_notify;
    cbs.conn_cbs.conn_handshake_finished = test_handshake_finished;
    cbs.stream_cbs.stream_read_notify = test_stream_notify;
    cbs.stream_cbs.stream_write_notify = test_stream_write_notify;
    cbs.stream_cbs.stream_create_notify = test_stream_notify;
    cbs.stream_cbs.stream_close_notify = test_stream_notify;
    return cbs;
}

static void
init_conn_settings(rqc_conn_settings_t *settings)
{
    *settings = internal_default_conn_settings;
    settings->proto_version = RQC_VERSION_V1;
    settings->cong_ctrl_callback = rqc_bbr_cb;
    settings->max_pkt_out_size = RQC_MAX_PACKET_OUT_SIZE;
    settings->max_udp_payload_size = RQC_MAX_PACKET_OUT_SIZE;
    settings->idle_time_out = 10000;
    settings->init_idle_time_out = 10000;
}

static void
create_test_engine(test_engine_t *out, rqc_engine_type_t type)
{
    rqc_config_t config;
    rqc_engine_callback_t engine_cb;
    rqc_transport_callbacks_t transport_cbs;
    rqc_app_proto_callbacks_t app_cbs;

    memset(out, 0, sizeof(*out));
    memset(&config, 0, sizeof(config));
    memset(&engine_cb, 0, sizeof(engine_cb));
    memset(&transport_cbs, 0, sizeof(transport_cbs));

    if (rqc_engine_get_default_config(&config, type) != RQC_OK) {
        return;
    }
    config.cfg_log_level = RQC_LOG_FATAL;
    config.sendmmsg_on = 0;

    engine_cb.set_event_timer = test_set_event_timer;
    engine_cb.log_callbacks.rqc_log_write_err = test_log_write;
    engine_cb.log_callbacks.rqc_log_write_stat = test_log_write;
    engine_cb.log_callbacks.rqc_qlog_event_write = test_qlog_write;

    transport_cbs.write_socket = test_socket_write;

    out->engine = rqc_engine_create(type, &config, &engine_cb, &transport_cbs, &out->ctx);
    if (out->engine == NULL) {
        return;
    }

    app_cbs = test_app_cbs();
    if (rqc_engine_register_alpn(out->engine, TEST_ALPN, strlen(TEST_ALPN), &app_cbs, NULL) != RQC_OK) {
        rqc_engine_destroy(out->engine);
        out->engine = NULL;
    }
}

static void
make_cid(rqc_cid_t *cid, uint8_t base)
{
    uint8_t data[RQC_DEFAULT_CID_LEN];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(base + i);
    }
    rqc_cid_set(cid, data, sizeof(data));
    cid->cid_seq_num = 0;
    cid->path_id = RQC_INITIAL_PATH_ID;
}

static int
encode_test_tp(uint8_t *buf, size_t cap, size_t *len)
{
    rqc_transport_params_t params;
    rqc_init_transport_params(&params);
    params.max_idle_timeout = 10000;
    params.max_udp_payload_size = RQC_MAX_PACKET_OUT_SIZE;
    params.initial_max_data = 1024 * 1024;
    params.initial_max_stream_data_bidi_local = 65536;
    params.initial_max_stream_data_bidi_remote = 65536;
    params.initial_max_stream_data_uni = 65536;
    params.initial_max_streams_bidi = 16;
    params.initial_max_streams_uni = 8;
    return rqc_encode_transport_params(&params, buf, cap, len);
}

int
rqc_test_handshake_frame_round_trip(void)
{
    const unsigned char alpn[] = TEST_ALPN;
    const uint8_t proto_ext[] = {0x10, 0x20, 0x30, 0x40};
    uint8_t tp[RQC_MAX_TRANSPORT_PARAM_BUF_LEN];
    size_t tp_len = 0;
    unsigned char parsed_alpn[RQC_MAX_ALPN_BUF_LEN];
    unsigned char parsed_tp[RQC_MAX_TRANSPORT_PARAM_BUF_LEN];
    unsigned char parsed_proto_ext[RQC_MAX_PROTO_EXT_LEN];
    size_t parsed_alpn_len = 0, parsed_tp_len = 0, parsed_proto_ext_len = 0;
    rqc_packet_out_t *po;
    rqc_packet_in_t pi;
    test_engine_t eng;
    rqc_connection_t *conn;
    ssize_t written;

    create_test_engine(&eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)eng.engine, 0);
    conn = create_client_conn(&eng, NULL, 0);
    CHECK_NE((uintptr_t)conn, 0);
    CHECK_EQ(encode_test_tp(tp, sizeof(tp), &tp_len), RQC_OK);
    po = rqc_packet_out_create(RQC_PACKET_OUT_SIZE);
    CHECK_NE((uintptr_t)po, 0);

    written = rqc_gen_handshake_frame(po, alpn, strlen((const char *)alpn),
                                      tp, tp_len, proto_ext, sizeof(proto_ext));
    CHECK_GT(written, 0);
    po->po_used_size = (unsigned int)written;
    CHECK_NE(po->po_frame_types & RQC_FRAME_BIT_HANDSHAKE, 0);

    memset(&pi, 0, sizeof(pi));
    rqc_packet_in_init(&pi, po->po_buf, (size_t)written, 0);
    CHECK_EQ(rqc_parse_handshake_frame(&pi, conn, parsed_alpn, sizeof(parsed_alpn), &parsed_alpn_len,
                                       parsed_tp, sizeof(parsed_tp), &parsed_tp_len,
                                       parsed_proto_ext, sizeof(parsed_proto_ext), &parsed_proto_ext_len), RQC_OK);

    CHECK_EQ(parsed_alpn_len, strlen((const char *)alpn));
    CHECK_EQ(memcmp(parsed_alpn, alpn, parsed_alpn_len), 0);
    CHECK_EQ(parsed_tp_len, tp_len);
    CHECK_EQ(memcmp(parsed_tp, tp, tp_len), 0);
    CHECK_EQ(parsed_proto_ext_len, sizeof(proto_ext));
    CHECK_EQ(memcmp(parsed_proto_ext, proto_ext, sizeof(proto_ext)), 0);
    CHECK_EQ((uintptr_t)pi.pos, (uintptr_t)pi.last);

    rqc_packet_out_destroy(po);
    rqc_engine_destroy(eng.engine);
    return 0;
}

int
rqc_test_malformed_handshake_frame(void)
{
    const unsigned char alpn[] = TEST_ALPN;
    const uint8_t proto_ext[] = {1, 2, 3};
    uint8_t tp[RQC_MAX_TRANSPORT_PARAM_BUF_LEN];
    size_t tp_len = 0;
    unsigned char out[RQC_MAX_ALPN_BUF_LEN];
    size_t out_len = 0;
    rqc_packet_out_t *po;
    rqc_packet_in_t pi;
    test_engine_t eng;
    rqc_connection_t *conn;
    ssize_t written;

    create_test_engine(&eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)eng.engine, 0);
    conn = create_client_conn(&eng, NULL, 0);
    CHECK_NE((uintptr_t)conn, 0);
    CHECK_EQ(encode_test_tp(tp, sizeof(tp), &tp_len), RQC_OK);
    po = rqc_packet_out_create(RQC_PACKET_OUT_SIZE);
    CHECK_NE((uintptr_t)po, 0);

    written = rqc_gen_handshake_frame(po, alpn, strlen((const char *)alpn),
                                      tp, tp_len, proto_ext, sizeof(proto_ext));
    CHECK_GT(written, 0);
    po->po_used_size = (unsigned int)written;

    memset(&pi, 0, sizeof(pi));
    rqc_packet_in_init(&pi, po->po_buf, (size_t)written - 1, 0);
    CHECK_EQ(rqc_parse_handshake_frame(&pi, conn, out, sizeof(out), &out_len,
                                       tp, sizeof(tp), &out_len, out, sizeof(out), &out_len),
             -RQC_EILLFRAME);

    memset(&pi, 0, sizeof(pi));
    rqc_packet_in_init(&pi, po->po_buf, (size_t)written, 0);
    CHECK_EQ(rqc_parse_handshake_frame(&pi, conn, out, 1, &out_len,
                                       tp, sizeof(tp), &out_len, out, sizeof(out), &out_len),
             RQC_ENOBUF);

    memset(&pi, 0, sizeof(pi));
    rqc_packet_in_init(&pi, po->po_buf, (size_t)written, 0);
    CHECK_EQ(rqc_parse_handshake_frame(&pi, conn, out, sizeof(out), &out_len,
                                       tp, sizeof(tp), &out_len, out, 1, &out_len),
             RQC_ENOBUF);

    rqc_packet_out_destroy(po);
    rqc_engine_destroy(eng.engine);
    return 0;
}

static rqc_connection_t *
create_client_conn(test_engine_t *eng, const uint8_t *proto_ext, size_t proto_ext_len)
{
    rqc_conn_settings_t settings;
    rqc_cid_t dcid, scid;
    rqc_proto_ext_t ext = {0};

    init_conn_settings(&settings);
    make_cid(&dcid, 0xa0);
    make_cid(&scid, 0xb0);
    if (proto_ext_len > 0) {
        memcpy(ext.data, proto_ext, proto_ext_len);
    }
    ext.len = proto_ext_len;

    return rqc_client_create_connection(eng->engine, dcid, scid, &settings,
                                        "localhost", TEST_ALPN,
                                        proto_ext_len > 0 ? &ext : NULL, &eng->ctx);
}

static rqc_connection_t *
create_client_conn_with_cids(test_engine_t *eng, uint8_t dcid_base, uint8_t scid_base)
{
    rqc_conn_settings_t settings;
    rqc_cid_t dcid, scid;

    init_conn_settings(&settings);
    make_cid(&dcid, dcid_base);
    make_cid(&scid, scid_base);

    return rqc_client_create_connection(eng->engine, dcid, scid, &settings,
                                        "localhost", TEST_ALPN, NULL, &eng->ctx);
}

static rqc_connection_t *
create_server_conn(test_engine_t *eng)
{
    rqc_conn_settings_t settings;
    rqc_cid_t dcid, scid;

    init_conn_settings(&settings);
    make_cid(&dcid, 0xc0);
    make_cid(&scid, 0xd0);

    return rqc_conn_create(eng->engine, &dcid, &scid, &settings, &eng->ctx, RQC_CONN_TYPE_SERVER);
}

int
rqc_test_stream_send_flush_on_eagain(void)
{
    test_engine_t eng;
    test_engine_t zero_eng;
    test_engine_t fin_eng;
    test_engine_t multi_eng;
    rqc_connection_t *conn;
    rqc_connection_t *other_conn;
    rqc_stream_t *stream;
    rqc_stream_t *other_stream;
    unsigned char data = 'x';
    int writes_after_local_flush;

    create_test_engine(&eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)eng.engine, 0);
    conn = create_client_conn(&eng, NULL, 0);
    CHECK_NE((uintptr_t)conn, 0);
    stream = rqc_stream_create_with_direction(conn, RQC_STREAM_BIDI, NULL);
    CHECK_NE((uintptr_t)stream, 0);

    /* The connection is not established, so no stream bytes can be queued. */
    CHECK_EQ(rqc_stream_send(stream, &data, 1, 0, 0), -RQC_EAGAIN);
    CHECK_EQ(eng.ctx.socket_writes, 0);

    /* flush=1 still drives the connection and sends its queued handshake. */
    CHECK_EQ(rqc_stream_send(stream, &data, 1, 0, 1), -RQC_EAGAIN);
    CHECK_GT(eng.ctx.socket_writes, 0);

    rqc_engine_destroy(eng.engine);

    create_test_engine(&zero_eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)zero_eng.engine, 0);
    conn = create_client_conn(&zero_eng, NULL, 0);
    CHECK_NE((uintptr_t)conn, 0);
    stream = rqc_stream_create_with_direction(conn, RQC_STREAM_BIDI, NULL);
    CHECK_NE((uintptr_t)stream, 0);

    CHECK_EQ(rqc_stream_send(stream, NULL, 0, 0, 0), -RQC_EAGAIN);
    CHECK_EQ(zero_eng.ctx.socket_writes, 0);
    CHECK_EQ(rqc_stream_send(stream, NULL, 0, 0, 1), -RQC_EAGAIN);
    CHECK_GT(zero_eng.ctx.socket_writes, 0);

    rqc_engine_destroy(zero_eng.engine);

    create_test_engine(&fin_eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)fin_eng.engine, 0);
    conn = create_client_conn(&fin_eng, NULL, 0);
    CHECK_NE((uintptr_t)conn, 0);
    stream = rqc_stream_create_with_direction(conn, RQC_STREAM_BIDI, NULL);
    CHECK_NE((uintptr_t)stream, 0);
    stream->stream_flag |= RQC_STREAM_FLAG_FIN_WRITE;

    CHECK_EQ(rqc_stream_send(stream, NULL, 0, 0, 0), 0);
    CHECK_EQ(fin_eng.ctx.socket_writes, 0);
    CHECK_EQ(rqc_stream_send(stream, NULL, 0, 0, 1), 0);
    CHECK_GT(fin_eng.ctx.socket_writes, 0);

    rqc_engine_destroy(fin_eng.engine);

    create_test_engine(&multi_eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)multi_eng.engine, 0);
    conn = create_client_conn_with_cids(&multi_eng, 0x10, 0x20);
    other_conn = create_client_conn_with_cids(&multi_eng, 0x30, 0x40);
    CHECK_NE((uintptr_t)conn, 0);
    CHECK_NE((uintptr_t)other_conn, 0);
    stream = rqc_stream_create_with_direction(conn, RQC_STREAM_BIDI, NULL);
    other_stream = rqc_stream_create_with_direction(other_conn, RQC_STREAM_BIDI, NULL);
    CHECK_NE((uintptr_t)stream, 0);
    CHECK_NE((uintptr_t)other_stream, 0);

    CHECK_EQ(rqc_stream_send(stream, &data, 1, 0, 0), -RQC_EAGAIN);
    CHECK_EQ(rqc_stream_send(other_stream, &data, 1, 0, 0), -RQC_EAGAIN);
    CHECK_EQ(multi_eng.ctx.socket_writes, 0);

    CHECK_EQ(rqc_stream_send(stream, &data, 1, 0, 1), -RQC_EAGAIN);
    writes_after_local_flush = multi_eng.ctx.socket_writes;
    CHECK_GT(writes_after_local_flush, 0);

    rqc_engine_finish_send(multi_eng.engine);
    CHECK_GT(multi_eng.ctx.socket_writes, writes_after_local_flush);

    rqc_engine_destroy(multi_eng.engine);
    return 0;
}

static rqc_stream_t *
create_established_test_stream(test_engine_t *eng, rqc_connection_t **conn_out)
{
    rqc_connection_t *conn = create_client_conn(eng, NULL, 0);
    if (conn == NULL) {
        return NULL;
    }
    conn->conn_state = RQC_CONN_STATE_ESTABED;
    *conn_out = conn;
    return rqc_stream_create_with_direction(conn, RQC_STREAM_BIDI, &eng->ctx);
}

int
rqc_test_stream_sendv_validation_and_empty(void)
{
    test_engine_t eng;
    test_engine_t fin_eng;
    test_engine_t closing_eng;
    test_engine_t reset_eng;
    rqc_connection_t *conn;
    rqc_stream_t *stream;
    rquic_iovec_t bad[] = {{NULL, 1}};
    rquic_iovec_t empty[] = {{NULL, 0}, {(const uint8_t *)"", 0}};
    unsigned char byte = 'x';
    rquic_iovec_t one[] = {{&byte, 1}};

    create_test_engine(&eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)eng.engine, 0);
    conn = create_client_conn(&eng, NULL, 0);
    CHECK_NE((uintptr_t)conn, 0);
    stream = rqc_stream_create_with_direction(conn, RQC_STREAM_BIDI, &eng.ctx);
    CHECK_NE((uintptr_t)stream, 0);

    CHECK_EQ(rqc_stream_sendv_atomic(NULL, NULL, 0, 0, 0), -RQC_EPARAM);
    CHECK_EQ(rqc_stream_sendv_atomic(stream, NULL, 1, 0, 0), -RQC_EPARAM);
    CHECK_EQ(rqc_stream_sendv_atomic(stream, bad, 1, 0, 0), -RQC_EPARAM);
    CHECK_EQ((uintptr_t)stream->atomic_ctx, 0);
    CHECK_EQ(stream->stream_send_offset, 0);

    CHECK_EQ(rqc_stream_sendv_atomic(stream, empty, 2, 0, 0), 0);
    CHECK_EQ((uintptr_t)stream->atomic_ctx, 0);
    CHECK_EQ(rqc_stream_sendv_atomic(stream, empty, 2, 0, 1), 0);
    CHECK_GT(eng.ctx.socket_writes, 0);
    CHECK_EQ(rqc_stream_sendv_atomic(stream, one, 1, 1, 1), -RQC_EAGAIN);
    CHECK_NE((uintptr_t)stream->atomic_ctx, 0);
    CHECK_EQ(stream->atomic_ctx->pending_len, 0);
    CHECK_EQ(stream->stream_flag & RQC_STREAM_FLAG_FIN_WRITE, 0);
    CHECK_GT(eng.ctx.socket_writes, 0);
    rqc_engine_destroy(eng.engine);

    create_test_engine(&fin_eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)fin_eng.engine, 0);
    stream = create_established_test_stream(&fin_eng, &conn);
    CHECK_NE((uintptr_t)stream, 0);
    CHECK_EQ(rqc_stream_sendv_atomic(stream, NULL, 0, 1, 0), 0);
    CHECK_NE(stream->stream_flag & RQC_STREAM_FLAG_FIN_WRITE, 0);
    CHECK_EQ(stream->stream_send_offset, 0);
    rqc_engine_destroy(fin_eng.engine);

    create_test_engine(&closing_eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)closing_eng.engine, 0);
    stream = create_established_test_stream(&closing_eng, &conn);
    CHECK_NE((uintptr_t)stream, 0);
    conn->conn_state = RQC_CONN_STATE_CLOSING;
    CHECK_EQ(rqc_stream_sendv_atomic(stream, empty, 2, 0, 0), -RQC_CLOSING);
    rqc_engine_destroy(closing_eng.engine);

    create_test_engine(&reset_eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)reset_eng.engine, 0);
    stream = create_established_test_stream(&reset_eng, &conn);
    CHECK_NE((uintptr_t)stream, 0);
    stream->stream_state_send = RQC_SEND_STREAM_ST_RESET_SENT;
    CHECK_EQ(rqc_stream_sendv_atomic(stream, empty, 2, 0, 0), -RQC_ESTREAM_RESET);
    rqc_engine_destroy(reset_eng.engine);
    return 0;
}

int
rqc_test_stream_sendv_complete_and_fixed_pending(void)
{
    test_engine_t full_eng;
    test_engine_t pending_eng;
    rqc_connection_t *conn;
    rqc_stream_t *stream;
    const uint8_t a[] = "abc";
    uint8_t b[] = "def";
    rquic_iovec_t iov[] = {{a, 3}, {NULL, 0}, {b, 3}, {NULL, 0}};
    unsigned char extra = 'z';

    create_test_engine(&full_eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)full_eng.engine, 0);
    stream = create_established_test_stream(&full_eng, &conn);
    CHECK_NE((uintptr_t)stream, 0);
    CHECK_EQ(rqc_stream_sendv_atomic(stream, iov, 4, 1, 0), 6);
    CHECK_EQ(stream->stream_send_offset, 6);
    CHECK_NE(stream->stream_flag & RQC_STREAM_FLAG_FIN_WRITE, 0);
    CHECK_NE((uintptr_t)stream->atomic_ctx, 0);
    CHECK_EQ(stream->atomic_ctx->pending_len, 0);
    rqc_engine_destroy(full_eng.engine);

    create_test_engine(&pending_eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)pending_eng.engine, 0);
    stream = create_established_test_stream(&pending_eng, &conn);
    CHECK_NE((uintptr_t)stream, 0);
    conn->conn_send_queue->sndq_packets_used_max =
        conn->conn_send_queue->sndq_packets_used + 1;
    pending_eng.ctx.mutate_on_socket_write = b;
    pending_eng.ctx.mutate_on_socket_write_len = 3;

    CHECK_EQ(rqc_stream_sendv_atomic(stream, iov, 4, 1, 0), 6);
    CHECK_EQ(memcmp(b, "def", 3), 0);
    CHECK_EQ(pending_eng.ctx.socket_writes, 0);
    CHECK_EQ(stream->stream_send_offset, 3);
    CHECK_EQ(stream->stream_flag & RQC_STREAM_FLAG_FIN_WRITE, 0);
    CHECK_EQ(stream->atomic_ctx->pending_len, 3);
    CHECK_EQ(stream->atomic_ctx->pending_offset, 0);
    CHECK_EQ((uintptr_t)stream->atomic_ctx->dynamic_buf, 0);
    CHECK_EQ(memcmp(stream->atomic_ctx->fixed_buf, "def", 3), 0);
    CHECK_EQ(rqc_stream_send(stream, &extra, 1, 0, 0), -RQC_EAGAIN);
    CHECK_EQ(rqc_stream_send(stream, NULL, 1, 0, 0), -RQC_EAGAIN);
    CHECK_EQ(rqc_stream_sendv_atomic(stream, iov, 1, 0, 0), -RQC_EAGAIN);
    CHECK_EQ(rqc_stream_sendv_atomic(stream, NULL, 1, 0, 0), -RQC_EAGAIN);
    CHECK_EQ(rqc_stream_send(stream, &extra, 1, 0, 1), -RQC_EAGAIN);
    CHECK_EQ(memcmp(b, "xxx", 3), 0);
    CHECK_EQ(pending_eng.ctx.stream_write_notifies, 0);
    CHECK_GT(pending_eng.ctx.socket_writes, 0);

    conn->conn_send_queue->sndq_packets_used_max++;
    rqc_engine_conn_logic(pending_eng.engine, conn);
    CHECK_EQ(stream->stream_send_offset, 6);
    CHECK_EQ(stream->atomic_ctx->pending_len, 0);
    CHECK_NE(stream->stream_flag & RQC_STREAM_FLAG_FIN_WRITE, 0);
    CHECK_EQ(pending_eng.ctx.stream_write_notifies, 1);
    CHECK_GT(pending_eng.ctx.socket_writes, 0);
    rqc_engine_destroy(pending_eng.engine);
    return 0;
}

int
rqc_test_stream_sendv_dynamic_multidrain_and_close(void)
{
    test_engine_t eng;
    rqc_connection_t *conn;
    rqc_stream_t *stream;
    uint8_t data[9000];
    rquic_iovec_t iov = {data, sizeof(data)};
    size_t first_offset;
    size_t drain_offset;

    memset(data, 0x5a, sizeof(data));
    create_test_engine(&eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)eng.engine, 0);
    stream = create_established_test_stream(&eng, &conn);
    CHECK_NE((uintptr_t)stream, 0);
    conn->conn_send_queue->sndq_packets_used_max =
        conn->conn_send_queue->sndq_packets_used + 1;

    CHECK_EQ(rqc_stream_sendv_atomic(stream, &iov, 1, 1, 0), (ssize_t)sizeof(data));
    first_offset = stream->stream_send_offset;
    CHECK_GT(first_offset, 0);
    CHECK(first_offset < sizeof(data));
    CHECK_GT(stream->atomic_ctx->pending_len, RQC_STREAM_ATOMIC_FIXED_CAPACITY);
    CHECK_NE((uintptr_t)stream->atomic_ctx->dynamic_buf, 0);
    CHECK_EQ(stream->stream_flag & RQC_STREAM_FLAG_FIN_WRITE, 0);

    conn->conn_send_queue->sndq_packets_used_max++;
    rqc_process_write_streams(conn);
    drain_offset = stream->stream_send_offset;
    CHECK_GT(drain_offset, first_offset);
    CHECK(drain_offset < sizeof(data));
    CHECK_GT(stream->atomic_ctx->pending_offset, 0);
    CHECK_EQ(eng.ctx.stream_write_notifies, 0);
    CHECK_EQ(stream->stream_flag & RQC_STREAM_FLAG_FIN_WRITE, 0);

    conn->conn_send_queue->sndq_packets_used_max += 16;
    rqc_process_write_streams(conn);
    CHECK_EQ(stream->stream_send_offset, sizeof(data));
    CHECK_EQ(stream->atomic_ctx->pending_len, 0);
    CHECK_EQ((uintptr_t)stream->atomic_ctx->dynamic_buf, 0);
    CHECK_NE(stream->stream_flag & RQC_STREAM_FLAG_FIN_WRITE, 0);
    CHECK_EQ(eng.ctx.stream_write_notifies, 1);

    /* Reset/close discards retained data before close notification/destruction. */
    stream->stream_flag &= ~RQC_STREAM_FLAG_FIN_WRITE;
    conn->conn_send_queue->sndq_packets_used_max = conn->conn_send_queue->sndq_packets_used + 1;
    CHECK_EQ(rqc_stream_sendv_atomic(stream, &iov, 1, 0, 0), (ssize_t)sizeof(data));
    rqc_stream_atomic_ctx_t *ctx = stream->atomic_ctx;
    CHECK_GT(ctx->pending_len, 0);
    CHECK_EQ(rqc_stream_close(stream), RQC_OK);
    CHECK_EQ(ctx->pending_len, 0);
    CHECK_EQ((uintptr_t)ctx->dynamic_buf, 0);

    rqc_engine_destroy(eng.engine);
    return 0;
}

int
rqc_test_fast_wakeup_dedup(void)
{
    test_engine_t eng;
    rqc_connection_t *conn;

    create_test_engine(&eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)eng.engine, 0);
    eng.ctx.timer_calls = 0;

    rqc_engine_wakeup_once(eng.engine);
    CHECK_EQ(eng.ctx.timer_calls, 1);
    CHECK_EQ(eng.ctx.wake_after, 1);
    CHECK_EQ(eng.engine->last_wake_after, 1);

    rqc_engine_wakeup_once(eng.engine);
    CHECK_EQ(eng.ctx.timer_calls, 1);

    eng.engine->eng_flag |= RQC_ENG_FLAG_RUNNING;
    rqc_engine_main_logic(eng.engine);
    CHECK_EQ(eng.engine->last_wake_after, 1);
    eng.engine->eng_flag &= ~RQC_ENG_FLAG_RUNNING;

    rqc_engine_main_logic(eng.engine);
    CHECK_EQ(eng.engine->last_wake_after, 0);
    rqc_engine_wakeup_once(eng.engine);
    CHECK_EQ(eng.ctx.timer_calls, 2);

    eng.engine->last_wake_after = 25;
    rqc_engine_wakeup_once(eng.engine);
    CHECK_EQ(eng.ctx.timer_calls, 3);
    CHECK_EQ(eng.ctx.wake_after, 1);

    conn = create_client_conn(&eng, NULL, 0);
    CHECK_NE((uintptr_t)conn, 0);
    rqc_conn_continue_send_by_conn(conn);
    CHECK_EQ(eng.ctx.timer_calls, 3);
    CHECK_EQ(eng.ctx.wake_after, 1);
    CHECK_EQ(eng.engine->last_wake_after, 1);

    rqc_engine_destroy(eng.engine);
    return 0;
}

int
rqc_test_client_handshake_state(void)
{
    const uint8_t peer_proto_ext[] = {9, 8, 7};
    uint8_t tp[RQC_MAX_TRANSPORT_PARAM_BUF_LEN];
    size_t tp_len = 0;
    test_engine_t client_eng;
    rqc_connection_t *client;

    create_test_engine(&client_eng, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)client_eng.engine, 0);
    client = create_client_conn(&client_eng, NULL, 0);
    CHECK_NE((uintptr_t)client, 0);
    CHECK_EQ(client->conn_state, RQC_CONN_STATE_CLIENT_HANDSHAKE);
    CHECK_EQ(client_eng.ctx.handshake_finished, 0);
    CHECK_EQ(encode_test_tp(tp, sizeof(tp), &tp_len), RQC_OK);

    CHECK_EQ(rqc_conn_process_handshake(client, (const unsigned char *)TEST_ALPN, strlen(TEST_ALPN),
                                        tp, tp_len, peer_proto_ext, sizeof(peer_proto_ext)), RQC_OK);
    CHECK_EQ(client->conn_state, RQC_CONN_STATE_ESTABED);
    CHECK(rqc_conn_is_handshake_recvd(client));
    CHECK(rqc_conn_is_handshake_done(client));
    CHECK_EQ(client_eng.ctx.handshake_finished, 1);
    CHECK_EQ(client->peer_proto_ext.len, sizeof(peer_proto_ext));
    CHECK_EQ(memcmp(client->peer_proto_ext.data, peer_proto_ext, sizeof(peer_proto_ext)), 0);

    CHECK_EQ(rqc_conn_process_handshake(client, (const unsigned char *)TEST_ALPN, strlen(TEST_ALPN),
                                        tp, tp_len, peer_proto_ext, sizeof(peer_proto_ext)), RQC_OK);
    CHECK_EQ(client->conn_state, RQC_CONN_STATE_ESTABED);
    CHECK_EQ(client_eng.ctx.handshake_finished, 1);

    rqc_engine_destroy(client_eng.engine);
    return 0;
}

int
rqc_test_server_handshake_state(void)
{
    const uint8_t peer_proto_ext[] = {1, 3, 5, 7};
    uint8_t tp[RQC_MAX_TRANSPORT_PARAM_BUF_LEN];
    size_t tp_len = 0;
    test_engine_t server_eng;
    rqc_connection_t *server;

    create_test_engine(&server_eng, RQC_ENGINE_SERVER);
    CHECK_NE((uintptr_t)server_eng.engine, 0);
    server = create_server_conn(&server_eng);
    CHECK_NE((uintptr_t)server, 0);
    server->version = RQC_VERSION_V1;
    CHECK_EQ(server->conn_state, RQC_CONN_STATE_SERVER_INIT);
    CHECK_EQ(encode_test_tp(tp, sizeof(tp), &tp_len), RQC_OK);

    CHECK_EQ(rqc_conn_process_handshake(server, (const unsigned char *)TEST_ALPN, strlen(TEST_ALPN),
                                        tp, tp_len, peer_proto_ext, sizeof(peer_proto_ext)), RQC_OK);
    CHECK_EQ(server->conn_state, RQC_CONN_STATE_SERVER_HANDSHAKE);
    CHECK(rqc_conn_is_handshake_recvd(server));
    CHECK(!rqc_conn_is_handshake_done(server));
    CHECK_EQ(server_eng.ctx.handshake_finished, 0);
    CHECK((server->conn_send_queue->sndq_send_packets.prev != &server->conn_send_queue->sndq_send_packets) ||
          (server->conn_send_queue->sndq_buff_1rtt_packets.prev != &server->conn_send_queue->sndq_buff_1rtt_packets));

    CHECK_EQ(rqc_conn_process_handshake(server, (const unsigned char *)TEST_ALPN, strlen(TEST_ALPN),
                                        tp, tp_len, peer_proto_ext, sizeof(peer_proto_ext)), RQC_OK);
    CHECK_EQ(server->conn_state, RQC_CONN_STATE_SERVER_HANDSHAKE);
    CHECK_EQ(server_eng.ctx.handshake_finished, 0);

    rqc_conn_on_handshake_acked(server);
    CHECK_EQ(server->conn_state, RQC_CONN_STATE_ESTABED);
    CHECK(rqc_conn_is_handshake_done(server));
    CHECK_EQ(server_eng.ctx.handshake_finished, 1);

    rqc_conn_on_handshake_acked(server);
    CHECK_EQ(server_eng.ctx.handshake_finished, 1);

    rqc_engine_destroy(server_eng.engine);
    return 0;
}

int
rqc_test_server_response_proto_ext_uses_embedded_array(void)
{
    const test_resp_proto_ext_mode_t modes[] = {
        TEST_RESP_PROTO_EXT_NONE,
        TEST_RESP_PROTO_EXT_NORMAL,
        TEST_RESP_PROTO_EXT_MAXIMUM,
    };
    uint8_t tp[RQC_MAX_TRANSPORT_PARAM_BUF_LEN];
    size_t tp_len = 0;

    CHECK_EQ(encode_test_tp(tp, sizeof(tp), &tp_len), RQC_OK);
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        test_engine_t server_eng;
        rqc_connection_t *server;
        size_t expected_len;

        create_test_engine(&server_eng, RQC_ENGINE_SERVER);
        CHECK_NE((uintptr_t)server_eng.engine, 0);
        server_eng.ctx.resp_proto_ext_mode = modes[i];
        server = create_server_conn(&server_eng);
        CHECK_NE((uintptr_t)server, 0);
        server->version = RQC_VERSION_V1;

        CHECK_EQ(rqc_conn_process_handshake(server,
                                            (const unsigned char *)TEST_ALPN, strlen(TEST_ALPN),
                                            tp, tp_len, NULL, 0), RQC_OK);
        CHECK_EQ((uintptr_t)server_eng.ctx.resp_proto_ext_data,
                 (uintptr_t)server->self_proto_ext.data);
        CHECK_EQ(server_eng.ctx.resp_proto_ext_len_on_entry, 0);

        expected_len = modes[i] == TEST_RESP_PROTO_EXT_NONE ? 0
            : modes[i] == TEST_RESP_PROTO_EXT_NORMAL ? 2 : RQC_MAX_PROTO_EXT_LEN;
        CHECK_EQ(server->self_proto_ext.len, expected_len);
        if (modes[i] == TEST_RESP_PROTO_EXT_NORMAL) {
            CHECK_EQ(server->self_proto_ext.data[0], 0x31);
            CHECK_EQ(server->self_proto_ext.data[1], 0x32);
        } else if (modes[i] == TEST_RESP_PROTO_EXT_MAXIMUM) {
            CHECK_EQ(server->self_proto_ext.data[0], 0x5a);
            CHECK_EQ(server->self_proto_ext.data[RQC_MAX_PROTO_EXT_LEN - 1], 0x5a);
        }

        rqc_engine_destroy(server_eng.engine);
    }

    return 0;
}

int
rqc_test_server_rejects_invalid_response_proto_ext(void)
{
    uint8_t tp[RQC_MAX_TRANSPORT_PARAM_BUF_LEN];
    size_t tp_len = 0;

    CHECK_EQ(encode_test_tp(tp, sizeof(tp), &tp_len), RQC_OK);
    test_engine_t server_eng;
    rqc_connection_t *server;

    create_test_engine(&server_eng, RQC_ENGINE_SERVER);
    CHECK_NE((uintptr_t)server_eng.engine, 0);
    server_eng.ctx.resp_proto_ext_mode = TEST_RESP_PROTO_EXT_OVERSIZED;
    server = create_server_conn(&server_eng);
    CHECK_NE((uintptr_t)server, 0);
    server->version = RQC_VERSION_V1;

    CHECK_EQ(rqc_conn_process_handshake(server,
                                        (const unsigned char *)TEST_ALPN, strlen(TEST_ALPN),
                                        tp, tp_len, NULL, 0), -RQC_EPARAM);
    CHECK_EQ(server->conn_err, TRA_INTERNAL_ERROR);
    CHECK_EQ(server->conn_state, RQC_CONN_STATE_SERVER_INIT);
    CHECK_EQ(server->self_proto_ext.len, 0);
    CHECK_EQ((uintptr_t)rqc_conn_get_self_proto_ext(server), 0);

    rqc_engine_destroy(server_eng.engine);

    create_test_engine(&server_eng, RQC_ENGINE_SERVER);
    CHECK_NE((uintptr_t)server_eng.engine, 0);
    server_eng.ctx.resp_proto_ext_mode = TEST_RESP_PROTO_EXT_NORMAL;
    server_eng.ctx.conn_create_ret = -1;
    server = create_server_conn(&server_eng);
    CHECK_NE((uintptr_t)server, 0);
    server->version = RQC_VERSION_V1;

    CHECK_EQ(rqc_conn_process_handshake(server,
                                        (const unsigned char *)TEST_ALPN, strlen(TEST_ALPN),
                                        tp, tp_len, NULL, 0), -TRA_INTERNAL_ERROR);
    CHECK_EQ(server->conn_err, TRA_INTERNAL_ERROR);
    CHECK_EQ(server->conn_state, RQC_CONN_STATE_SERVER_INIT);
    CHECK_EQ(server->self_proto_ext.len, 0);
    CHECK_EQ((uintptr_t)rqc_conn_get_self_proto_ext(server), 0);

    rqc_engine_destroy(server_eng.engine);

    return 0;
}

int
rqc_test_abnormal_handshake_input(void)
{
    uint8_t bad_tp[] = {0xff, 0xff};
    uint8_t tp[RQC_MAX_TRANSPORT_PARAM_BUF_LEN];
    uint8_t too_large_ext[RQC_MAX_PROTO_EXT_LEN + 1];
    size_t tp_len = 0;
    test_engine_t client_eng;
    test_engine_t client_eng2;
    rqc_connection_t *client;
    rqc_connection_t *client2;

    create_test_engine(&client_eng, RQC_ENGINE_CLIENT);
    create_test_engine(&client_eng2, RQC_ENGINE_CLIENT);
    CHECK_NE((uintptr_t)client_eng.engine, 0);
    CHECK_NE((uintptr_t)client_eng2.engine, 0);
    client = create_client_conn(&client_eng, NULL, 0);
    client2 = create_client_conn(&client_eng2, NULL, 0);
    CHECK_NE((uintptr_t)client, 0);
    CHECK_NE((uintptr_t)client2, 0);
    memset(too_large_ext, 0xa5, sizeof(too_large_ext));
    CHECK_EQ(encode_test_tp(tp, sizeof(tp), &tp_len), RQC_OK);

    CHECK_EQ(rqc_conn_process_handshake(client, (const unsigned char *)TEST_ALPN, strlen(TEST_ALPN),
                                        bad_tp, sizeof(bad_tp), NULL, 0), -RQC_EILLTP);
    CHECK_NE(client->conn_flag & RQC_CONN_FLAG_ERROR, 0);
    CHECK_EQ(client_eng.ctx.handshake_finished, 0);

    CHECK_EQ(rqc_conn_process_handshake(client2, (const unsigned char *)TEST_ALPN, strlen(TEST_ALPN),
                                        tp, tp_len, too_large_ext, sizeof(too_large_ext)), -RQC_EILLFRAME);
    CHECK_NE(client2->conn_flag & RQC_CONN_FLAG_ERROR, 0);
    CHECK_EQ(client_eng2.ctx.handshake_finished, 0);

    rqc_engine_destroy(client_eng.engine);
    rqc_engine_destroy(client_eng2.engine);
    return 0;
}

static void
init_test_stream_frame(rqc_stream_frame_t *frame, unsigned char *data,
    unsigned data_length, uint64_t data_offset)
{
    memset(frame, 0, sizeof(*frame));
    rqc_init_list_head(&frame->sf_list);
    frame->data = data;
    frame->data_length = data_length;
    frame->data_offset = data_offset;
}

int
rqc_test_stream_peek(void)
{
    rqc_engine_t engine;
    rqc_connection_t conn;
    rqc_stream_t stream;
    rqc_stream_frame_t frame1, frame2;
    unsigned char data1[] = "abcdef";
    unsigned char data2[] = "efghij";
    unsigned char peek_buf[16];
    uint64_t readable = 0;
    uint8_t fin = 0;
    ssize_t n;

    memset(&engine, 0, sizeof(engine));
    memset(&conn, 0, sizeof(conn));
    memset(&stream, 0, sizeof(stream));
    rqc_init_list_head(&conn.conn_read_streams);
    rqc_init_list_head(&stream.read_stream_list);
    rqc_init_list_head(&stream.stream_data_in.frames_tailq);
    conn.engine = &engine;
    conn.conn_flag = RQC_CONN_FLAG_TICKING;
    stream.stream_conn = &conn;

    init_test_stream_frame(&frame1, data1, 6, 0);
    init_test_stream_frame(&frame2, data2, 6, 4);
    rqc_list_add_tail(&frame1.sf_list, &stream.stream_data_in.frames_tailq);
    rqc_list_add_tail(&frame2.sf_list, &stream.stream_data_in.frames_tailq);
    stream.stream_data_in.next_read_offset = 2;
    stream.stream_data_in.merged_offset_end = 10;
    stream.stream_data_in.stream_length = 10;
    stream.stream_data_in.stream_determined = RQC_TRUE;

    rqc_stream_ready_to_read(&stream);
    CHECK(stream.stream_flag & RQC_STREAM_FLAG_READY_TO_READ);
    memset(peek_buf, 0, sizeof(peek_buf));
    n = rqc_stream_peek(&stream, peek_buf, 5, &readable, &fin);
    CHECK_EQ(n, 5);
    CHECK_EQ(readable, 8);
    CHECK_EQ(fin, 1);
    CHECK_EQ(memcmp(peek_buf, "cdefg", 5), 0);
    CHECK_EQ(stream.stream_data_in.next_read_offset, 2);
    CHECK_EQ(frame1.next_read_offset, 0);
    CHECK_EQ(frame2.next_read_offset, 0);
    CHECK(!(stream.stream_flag & RQC_STREAM_FLAG_READY_TO_READ));
    CHECK(rqc_list_empty(&conn.conn_read_streams));

    /* A caller can continue draining buffered messages in the same callback. */
    memset(peek_buf, 0, sizeof(peek_buf));
    n = rqc_stream_peek(&stream, peek_buf, sizeof(peek_buf), &readable, &fin);
    CHECK_EQ(n, 8);
    CHECK_EQ(memcmp(peek_buf, "cdefghij", 8), 0);

    n = rqc_stream_peek(&stream, NULL, 0, &readable, &fin);
    CHECK_EQ(n, 0);
    CHECK_EQ(readable, 8);
    CHECK_EQ(fin, 1);

    /* Incomplete data is disarmed, and new data can arm notification again. */
    stream.stream_data_in.stream_length = 12;
    rqc_stream_ready_to_read(&stream);
    n = rqc_stream_peek(&stream, peek_buf, sizeof(peek_buf), &readable, &fin);
    CHECK_EQ(n, 8);
    CHECK_EQ(readable, 8);
    CHECK_EQ(fin, 0);
    CHECK(!(stream.stream_flag & RQC_STREAM_FLAG_READY_TO_READ));
    rqc_stream_ready_to_read(&stream);
    CHECK(stream.stream_flag & RQC_STREAM_FLAG_READY_TO_READ);
    CHECK(!rqc_list_empty(&conn.conn_read_streams));
    rqc_stream_shutdown_read(&stream);

    stream.stream_data_in.next_read_offset = 10;
    stream.stream_data_in.merged_offset_end = 10;
    stream.stream_data_in.stream_length = 10;
    n = rqc_stream_peek(&stream, NULL, 0, &readable, &fin);
    CHECK_EQ(n, 0);
    CHECK_EQ(readable, 0);
    CHECK_EQ(fin, 1);

    stream.stream_data_in.stream_determined = RQC_FALSE;
    n = rqc_stream_peek(&stream, NULL, 0, &readable, &fin);
    CHECK_EQ(n, -RQC_EAGAIN);
    CHECK_EQ(readable, 0);
    CHECK_EQ(fin, 0);

    rqc_list_del_init(&frame1.sf_list);
    rqc_list_del_init(&frame2.sf_list);
    return 0;
}
