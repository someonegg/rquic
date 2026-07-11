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
#include "src/transport/rqc_transport_params.h"

#include <stdio.h>
#include <string.h>

#define TEST_ALPN "rqc-test"

typedef struct test_ctx_s {
    int handshake_finished;
    int conn_create;
} test_ctx_t;

typedef struct test_engine_s {
    rqc_engine_t *engine;
    test_ctx_t ctx;
} test_engine_t;

static rqc_connection_t *create_client_conn(test_engine_t *eng,
    const uint8_t *proto_ext, size_t proto_ext_len);

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
    (void)wake_after;
    (void)engine_user_data;
}

static ssize_t
test_socket_write(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *conn_user_data)
{
    (void)buf;
    (void)peer_addr;
    (void)peer_addrlen;
    (void)conn_user_data;
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
    (void)resp_proto_ext;
    if (ctx != NULL) {
        ctx->conn_create++;
    }
    return 0;
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

static rqc_app_proto_callbacks_t
test_app_cbs(void)
{
    rqc_app_proto_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.conn_cbs.conn_create_notify = test_conn_create_notify;
    cbs.conn_cbs.conn_close_notify = test_conn_close_notify;
    cbs.conn_cbs.conn_handshake_finished = test_handshake_finished;
    cbs.stream_cbs.stream_read_notify = test_stream_notify;
    cbs.stream_cbs.stream_write_notify = test_stream_notify;
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

static test_engine_t
create_test_engine(rqc_engine_type_t type)
{
    test_engine_t out;
    rqc_config_t config;
    rqc_engine_callback_t engine_cb;
    rqc_transport_callbacks_t transport_cbs;
    rqc_app_proto_callbacks_t app_cbs;

    memset(&out, 0, sizeof(out));
    memset(&config, 0, sizeof(config));
    memset(&engine_cb, 0, sizeof(engine_cb));
    memset(&transport_cbs, 0, sizeof(transport_cbs));

    if (rqc_engine_get_default_config(&config, type) != RQC_OK) {
        return out;
    }
    config.cfg_log_level = RQC_LOG_FATAL;
    config.sendmmsg_on = 0;

    engine_cb.set_event_timer = test_set_event_timer;
    engine_cb.log_callbacks.rqc_log_write_err = test_log_write;
    engine_cb.log_callbacks.rqc_log_write_stat = test_log_write;
    engine_cb.log_callbacks.rqc_qlog_event_write = test_qlog_write;

    transport_cbs.write_socket = test_socket_write;

    out.engine = rqc_engine_create(type, &config, &engine_cb, &transport_cbs, &out.ctx);
    if (out.engine == NULL) {
        return out;
    }

    app_cbs = test_app_cbs();
    if (rqc_engine_register_alpn(out.engine, TEST_ALPN, strlen(TEST_ALPN), &app_cbs, NULL) != RQC_OK) {
        rqc_engine_destroy(out.engine);
        out.engine = NULL;
    }

    return out;
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
    test_engine_t eng = create_test_engine(RQC_ENGINE_CLIENT);
    rqc_connection_t *conn = create_client_conn(&eng, NULL, 0);
    ssize_t written;

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
    test_engine_t eng = create_test_engine(RQC_ENGINE_CLIENT);
    rqc_connection_t *conn = create_client_conn(&eng, NULL, 0);
    ssize_t written;

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
    rqc_proto_ext_t ext;

    init_conn_settings(&settings);
    make_cid(&dcid, 0xa0);
    make_cid(&scid, 0xb0);
    ext.data = proto_ext;
    ext.len = proto_ext_len;

    return rqc_client_create_connection(eng->engine, dcid, scid, &settings,
                                        "localhost", TEST_ALPN,
                                        proto_ext_len > 0 ? &ext : NULL, &eng->ctx);
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
rqc_test_client_handshake_state(void)
{
    const uint8_t peer_proto_ext[] = {9, 8, 7};
    uint8_t tp[RQC_MAX_TRANSPORT_PARAM_BUF_LEN];
    size_t tp_len = 0;
    test_engine_t client_eng = create_test_engine(RQC_ENGINE_CLIENT);
    rqc_connection_t *client = create_client_conn(&client_eng, NULL, 0);

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
    test_engine_t server_eng = create_test_engine(RQC_ENGINE_SERVER);
    rqc_connection_t *server = create_server_conn(&server_eng);

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
rqc_test_abnormal_handshake_input(void)
{
    uint8_t bad_tp[] = {0xff, 0xff};
    uint8_t tp[RQC_MAX_TRANSPORT_PARAM_BUF_LEN];
    uint8_t too_large_ext[RQC_MAX_PROTO_EXT_LEN + 1];
    size_t tp_len = 0;
    test_engine_t client_eng = create_test_engine(RQC_ENGINE_CLIENT);
    test_engine_t client_eng2 = create_test_engine(RQC_ENGINE_CLIENT);
    rqc_connection_t *client = create_client_conn(&client_eng, NULL, 0);
    rqc_connection_t *client2 = create_client_conn(&client_eng2, NULL, 0);

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
