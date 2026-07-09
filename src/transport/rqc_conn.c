/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include <rquic/rquic.h>
#include <errno.h>
#include "src/common/rqc_algorithm.h"
#include "src/common/rqc_common.h"
#include "src/common/rqc_malloc.h"
#include "src/common/rqc_str_hash.h"
#include "src/common/rqc_hash.h"
#include "src/common/rqc_priority_q.h"
#include "src/common/rqc_memory_pool.h"
#include "src/common/rqc_id_hash.h"
#include "src/transport/rqc_defs.h"
#include "src/transport/rqc_conn.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/transport/rqc_send_queue.h"
#include "src/transport/rqc_engine.h"
#include "src/transport/rqc_cid.h"
#include "src/transport/rqc_stream.h"
#include "src/transport/rqc_frame_parser.h"
#include "src/transport/rqc_packet_parser.h"
#include "src/transport/rqc_utils.h"
#include "src/transport/rqc_multipath.h"
#include "src/transport/rqc_packet.h"
#include <inttypes.h>

rqc_conn_settings_t internal_default_conn_settings = {
    .pacing_on                  = 0,
    .ping_on                    = 0,
    .so_sndbuf                  = 0,
    .sndq_packets_used_max      = 0,
    .linger                     = {.linger_on = 0, .linger_timeout = 0},
    .proto_version              = RQC_VERSION_V1,
    .init_idle_time_out         = RQC_CONN_INITIAL_IDLE_TIMEOUT,
    .idle_time_out              = RQC_CONN_DEFAULT_IDLE_TIMEOUT,
    .spurious_loss_detect_on    = 0,
    .max_pkt_out_size           = RQC_PACKET_OUT_SIZE,
    .max_ack_delay              = RQC_DEFAULT_MAX_ACK_DELAY,
    .ack_frequency              = 2,
    .loss_detection_pkt_thresh  = RQC_kPacketThreshold,
    .pto_backoff_factor         = 2.0,

    .recv_rate_bytes_per_sec    = 0,
    .enable_stream_rate_limit   = 0,

#ifdef RQC_PROTECT_POOL_MEM
    .protect_pool_mem           = 0,
#endif

    .disable_send_mmsg          = 0,
    .control_pto_value          = 0,
    .max_udp_payload_size       = RQC_CONN_MAX_UDP_PAYLOAD_SIZE,
};

rqc_conn_settings_t
rqc_conn_get_conn_settings_template(rqc_conn_settings_type_t settings_type)
{
    rqc_conn_settings_t conn_settings = internal_default_conn_settings;

    if (settings_type == RQC_CONN_SETTINGS_LOW_DELAY) {
        conn_settings.ack_frequency = 1;
        conn_settings.loss_detection_pkt_thresh = 2;
        conn_settings.pto_backoff_factor = 1.5;
    }

    return conn_settings;
}

void
rqc_server_set_conn_settings(rqc_engine_t *engine, const rqc_conn_settings_t *settings)
{
    engine->default_conn_settings.cong_ctrl_callback = settings->cong_ctrl_callback;
    engine->default_conn_settings.cc_params = settings->cc_params;
    engine->default_conn_settings.pacing_on = settings->pacing_on;
    engine->default_conn_settings.ping_on   = settings->ping_on;
    engine->default_conn_settings.so_sndbuf = settings->so_sndbuf;
    engine->default_conn_settings.sndq_packets_used_max = settings->sndq_packets_used_max;
    engine->default_conn_settings.linger    = settings->linger;
    engine->default_conn_settings.spurious_loss_detect_on = settings->spurious_loss_detect_on;
    engine->default_conn_settings.recv_rate_bytes_per_sec = settings->recv_rate_bytes_per_sec;
    engine->default_conn_settings.enable_stream_rate_limit = settings->enable_stream_rate_limit;
    engine->default_conn_settings.init_recv_window = settings->init_recv_window;
    engine->default_conn_settings.initial_rtt = settings->initial_rtt;
    engine->default_conn_settings.initial_pto_duration = settings->initial_pto_duration;
    engine->default_conn_settings.disable_send_mmsg = settings->disable_send_mmsg;
#ifdef RQC_PROTECT_POOL_MEM
    engine->default_conn_settings.protect_pool_mem = settings->protect_pool_mem;
#endif
    engine->default_conn_settings.adaptive_ack_frequency = settings->adaptive_ack_frequency;

    if (settings->max_udp_payload_size != 0) {
        engine->default_conn_settings.max_udp_payload_size = settings->max_udp_payload_size;
    }

    if (engine->default_conn_settings.init_recv_window) {
        engine->default_conn_settings.init_recv_window = rqc_max(engine->default_conn_settings.init_recv_window, RQC_QUIC_MAX_MSS);
    }

    if (settings->max_ack_delay) {
        engine->default_conn_settings.max_ack_delay = rqc_min(settings->max_ack_delay, RQC_DEFAULT_MAX_ACK_DELAY);
    }

    if (settings->init_idle_time_out > 0) {
        engine->default_conn_settings.init_idle_time_out = settings->init_idle_time_out;
    }

    if (settings->idle_time_out > 0) {
        engine->default_conn_settings.idle_time_out = settings->idle_time_out;
    }

    if (rqc_check_proto_version_valid(settings->proto_version)) {
        engine->default_conn_settings.proto_version = settings->proto_version;
    }

    if (settings->max_pkt_out_size != 0) {
        engine->default_conn_settings.max_pkt_out_size = settings->max_pkt_out_size;
    }

    if (engine->default_conn_settings.max_pkt_out_size > RQC_MAX_PACKET_OUT_SIZE) {
        engine->default_conn_settings.max_pkt_out_size = RQC_MAX_PACKET_OUT_SIZE;
    }

    if (settings->ack_frequency > 0) {
        engine->default_conn_settings.ack_frequency = settings->ack_frequency;
    }

    if (settings->pto_backoff_factor > 0) {
        engine->default_conn_settings.pto_backoff_factor = settings->pto_backoff_factor;
    }

    if (settings->loss_detection_pkt_thresh > 0) {
        engine->default_conn_settings.loss_detection_pkt_thresh = settings->loss_detection_pkt_thresh;
    }
}

rqc_conn_type_t
rqc_conn_get_type(rqc_connection_t *conn)
{
    return conn->conn_type;
}

rqc_int_t
rqc_conn_get_errno(rqc_connection_t *conn)
{
    return conn->conn_err;
}

rqc_usec_t
rqc_conn_get_lastest_rtt(rqc_engine_t *engine, const rqc_cid_t *cid)
{
    rqc_connection_t *conn;

    conn = rqc_engine_conns_hash_find(engine, cid, 's');
    if (!conn) {
        rqc_log(engine->log, RQC_LOG_ERROR, "|can not find connection|cid:%s",
                rqc_scid_str(engine, cid));
        return 0;
    }

    if (conn->the_path) {
        return conn->the_path->path_send_ctl->ctl_latest_rtt;
    }

    return 0;
}

void
rqc_conn_set_transport_user_data(rqc_connection_t *conn, void *user_data)
{
    conn->user_data = user_data;
}

void
rqc_conn_set_alp_user_data(rqc_connection_t *conn, void *user_data)
{
    conn->proto_data = user_data;
}

const rqc_proto_ext_t *
rqc_conn_get_peer_proto_ext(rqc_connection_t *conn)
{
    if (conn == NULL || conn->peer_proto_ext.len == 0) {
        return NULL;
    }

    return &conn->peer_proto_ext;
}

const rqc_proto_ext_t *
rqc_conn_get_self_proto_ext(rqc_connection_t *conn)
{
    if (conn == NULL || conn->self_proto_ext.len == 0) {
        return NULL;
    }

    return &conn->self_proto_ext;
}

rqc_int_t
rqc_conn_get_peer_addr(rqc_connection_t *conn, struct sockaddr *addr, socklen_t addr_cap,
    socklen_t *peer_addr_len)
{
    if (conn->peer_addrlen > addr_cap) {
        return -RQC_ENOBUF;
    }

    *peer_addr_len = conn->peer_addrlen;
    rqc_memcpy(addr, conn->peer_addr, conn->peer_addrlen);
    return RQC_OK;
}

rqc_int_t
rqc_conn_get_local_addr(rqc_connection_t *conn, struct sockaddr *addr, socklen_t addr_cap,
    socklen_t *local_addr_len)
{
    if (conn->local_addrlen > addr_cap) {
        return -RQC_ENOBUF;
    }

    *local_addr_len = conn->local_addrlen;
    rqc_memcpy(addr, conn->local_addr, conn->local_addrlen);
    return RQC_OK;
}

void
rqc_conn_set_pkt_filter_callback(rqc_connection_t *conn,
    rqc_conn_pkt_filter_callback_pt pkt_filter_cb,
    void *pkt_filter_cb_user_data)
{
    conn->pkt_filter_cb = pkt_filter_cb;
    conn->pkt_filter_cb_user_data = pkt_filter_cb_user_data;
}

void
rqc_conn_unset_pkt_filter_callback(rqc_connection_t *conn)
{
    if (conn) {
        conn->pkt_filter_cb = NULL;
        conn->pkt_filter_cb_user_data = NULL;
        rqc_log(conn->log, RQC_LOG_INFO, "|conn unset pkt filter callback, will"
                "use write_socket again");
    }
}

static void
rqc_conn_get_stats_internal(rqc_connection_t *conn, rqc_conn_stats_t *conn_stats)
{
    /* 1. 与路径无关的连接级别埋点 */
    rqc_memset(conn_stats->alpn, 0, RQC_MAX_ALPN_BUF_LEN);
    if (conn->alpn) {
        rqc_memcpy(conn_stats->alpn, conn->alpn, rqc_min(conn->alpn_len, RQC_MAX_ALPN_LEN));
    } else {
        conn_stats->alpn[0] = '-';
        conn_stats->alpn[1] = '1';
    }

    conn_stats->conn_err = (int)conn->conn_err;
    conn_stats->spurious_loss_detect_on = conn->conn_settings.spurious_loss_detect_on;
    conn_stats->max_acked_mtu = conn->max_acked_po_size;

    rqc_path_ctx_t *path = conn->the_path;

    /* 2. srtt 和 ack_info */
    if (path)
    {
        if (path->path_send_ctl->ctl_first_rtt_sample_time == 0) {
            conn_stats->srtt = 0;
            conn_stats->min_rtt = 0;
        } else {
            conn_stats->srtt = path->path_send_ctl->ctl_srtt;
            conn_stats->min_rtt = path->path_send_ctl->ctl_minrtt;
        }

        rqc_recv_record_print(conn, &(path->path_pn_ctl->ctl_recv_record),
                              conn_stats->ack_info, sizeof(conn_stats->ack_info));
    }

    /* 3. 路径count */
    if (path) {
        conn_stats->lost_count           += path->path_send_ctl->ctl_lost_count;
        conn_stats->send_count           += path->path_send_ctl->ctl_send_count;
        conn_stats->tlp_count            += path->path_send_ctl->ctl_tlp_count;
        conn_stats->spurious_loss_count  += path->path_send_ctl->ctl_spurious_loss_count;
        conn_stats->recv_count           += path->path_send_ctl->ctl_recv_count;
        conn_stats->inflight_bytes       += path->path_send_ctl->ctl_bytes_in_flight;
        conn_stats->total_rebind_count   += path->rebinding_count;
        conn_stats->total_rebind_valid   += path->rebinding_valid;
    }

    /* 路径信息 */
    rqc_conn_path_metrics_print(conn, conn_stats);
}

rqc_conn_stats_t
rqc_conn_get_stats(rqc_engine_t *engine, const rqc_cid_t *cid)
{
    rqc_connection_t *conn;
    rqc_conn_stats_t conn_stats;
    rqc_memzero(&conn_stats, sizeof(conn_stats));
    conn_stats.path_info.path_id = RQC_MAX_UINT64_VALUE;

    conn = rqc_engine_conns_hash_find(engine, cid, 's');
    if (!conn) {
        rqc_log(engine->log, RQC_LOG_ERROR, "|can not find connection|cid:%s",
                rqc_scid_str(engine, cid));
        return conn_stats;
    }

    rqc_conn_get_stats_internal(conn, &conn_stats);

    return conn_stats;
}

rqc_conn_qos_stats_t
rqc_conn_get_qos_stats(rqc_engine_t *engine, const rqc_cid_t *cid)
{
    rqc_connection_t *conn;
    rqc_conn_qos_stats_t qos_stats;
    rqc_memzero(&qos_stats, sizeof(qos_stats));

    conn = rqc_engine_conns_hash_find(engine, cid, 's');
    if (!conn) {
        rqc_log(engine->log, RQC_LOG_ERROR, "|can not find connection|cid:%s",
                rqc_scid_str(engine, cid));
        return qos_stats;
    }

    rqc_path_ctx_t *path = conn->the_path;
    if (path)
    {
        if (path->path_send_ctl->ctl_first_rtt_sample_time == 0) {
            qos_stats.srtt = 0;
            qos_stats.min_rtt = 0;

        } else {
            qos_stats.srtt = path->path_send_ctl->ctl_srtt;
            qos_stats.min_rtt = path->path_send_ctl->ctl_minrtt;
        }

        qos_stats.inflight_bytes += path->path_send_ctl->ctl_bytes_in_flight;
    }

    return qos_stats;
}

void
rqc_conn_continue_send_by_conn(rqc_connection_t *conn)
{
    if (!conn) {
        return;
    }

    rqc_engine_remove_wakeup_queue(conn->engine, conn);
    rqc_engine_add_active_queue(conn->engine, conn);

    rqc_engine_conn_logic(conn->engine, conn);
}

int
rqc_conn_continue_send(rqc_engine_t *engine, const rqc_cid_t *cid)
{
    rqc_connection_t *conn = rqc_engine_conns_hash_find(engine, cid, 's');
    if (!conn) {
        rqc_log(engine->log, RQC_LOG_ERROR, "|can not find connection|cid:%s",
                rqc_scid_str(engine, cid));
        return -RQC_ECONN_NFOUND;
    }

    rqc_conn_continue_send_by_conn(conn);
    return RQC_OK;
}

static const char * const rqc_conn_state_to_str[RQC_CONN_STATE_N] = {
    [RQC_CONN_STATE_SERVER_INIT]            = "S_INIT",
    [RQC_CONN_STATE_SERVER_HANDSHAKE]       = "S_HANDSHAKE",
    [RQC_CONN_STATE_CLIENT_INIT]            = "C_INIT",
    [RQC_CONN_STATE_CLIENT_HANDSHAKE]       = "C_HANDSHAKE",
    [RQC_CONN_STATE_ESTABED]                = "ESTABED",
    [RQC_CONN_STATE_CLOSING]                = "CLOSING",
    [RQC_CONN_STATE_DRAINING]               = "DRAINING",
    [RQC_CONN_STATE_CLOSED]                 = "CLOSED",
};

const char *
rqc_conn_state_2_str(rqc_conn_state_t state)
{
    return rqc_conn_state_to_str[state];
}

static const char * const rqc_conn_flag_to_str[RQC_CONN_FLAG_SHIFT_NUM] = {
    [RQC_CONN_FLAG_WAIT_WAKEUP_SHIFT]           = "WAIT_WAKEUP",
    [RQC_CONN_FLAG_HANDSHAKE_SENT_SHIFT]        = "HSK_SENT",
    [RQC_CONN_FLAG_HANDSHAKE_RECVD_SHIFT]       = "HSK_RECVD",
    [RQC_CONN_FLAG_HANDSHAKE_DONE_SHIFT]        = "HSK_DONE",
    [RQC_CONN_FLAG_TICKING_SHIFT]               = "TICKING",
    [RQC_CONN_FLAG_ACK_HAS_GAP_SHIFT]           = "HAS_GAP",
    [RQC_CONN_FLAG_TIME_OUT_SHIFT]              = "TIME_OUT",
    [RQC_CONN_FLAG_ERROR_SHIFT]                 = "ERROR",
    [RQC_CONN_FLAG_DATA_BLOCKED_SHIFT]          = "DATA_BLOCKED",
    [RQC_CONN_FLAG_DCID_DONE_SHIFT]             = "DCID_DONE",
    [RQC_CONN_FLAG_UPPER_CONN_EXIST_SHIFT]      = "UPPER_CONN_EXIST",
    [RQC_CONN_FLAG_NEED_RUN_SHIFT]              = "NEED_RUN",
    [RQC_CONN_FLAG_PING_SHIFT]                  = "PING",
    [RQC_CONN_FLAG_LINGER_CLOSING_SHIFT]        = "LINGER_CLOSING",
    [RQC_CONN_FLAG_CONN_CLOSING_NOTIFY_SHIFT]   = "CLOSING_NOTIFY",
    [RQC_CONN_FLAG_CONN_CLOSING_NOTIFIED_SHIFT] = "CLOSING_NOTIFIED",
    [RQC_CONN_FLAG_VERSION_NEGOTIATION_SHIFT]   = "VERSION_NEGOTIATION",
};

const char *
rqc_conn_flag_2_str(rqc_connection_t *conn, rqc_conn_flag_t conn_flag)
{
    rqc_engine_t *engine = conn->engine;
    engine->conn_flag_str_buf[0] = '\0';
    size_t pos = 0;
    int wsize;
    for (int i = 0; i < RQC_CONN_FLAG_SHIFT_NUM; i++) {
        if (conn_flag & 1ULL << i) {
            wsize = snprintf(engine->conn_flag_str_buf + pos, sizeof(engine->conn_flag_str_buf) - pos, "%s ",
                             rqc_conn_flag_to_str[i]);
            if (wsize < 0 || wsize >= sizeof(engine->conn_flag_str_buf) - pos) {
                break;
            }
            pos += wsize;
        }
    }

    return engine->conn_flag_str_buf;
}

char *
rqc_local_addr_str(rqc_engine_t *engine, const struct sockaddr *local_addr, socklen_t local_addrlen)
{
    if (local_addrlen == 0 || local_addr == NULL) {
        engine->local_addr_str[0] = '\0';
        return engine->local_addr_str;
    }

    struct sockaddr_in *sa_local = (struct sockaddr_in *)local_addr;
    if (sa_local->sin_family == AF_INET) {
        if (inet_ntop(sa_local->sin_family, &sa_local->sin_addr, engine->local_addr_str, local_addrlen) == NULL) {
            engine->local_addr_str[0] = '\0';
        }

    } else {
        if (inet_ntop(sa_local->sin_family, &((struct sockaddr_in6*)sa_local)->sin6_addr,
                      engine->local_addr_str, local_addrlen) == NULL)
        {
            engine->local_addr_str[0] = '\0';
        }
    }

    return engine->local_addr_str;
}

char *
rqc_peer_addr_str(rqc_engine_t *engine, const struct sockaddr *peer_addr, socklen_t peer_addrlen)
{
    if (peer_addrlen == 0 || peer_addr == NULL) {
        engine->peer_addr_str[0] = '\0';
        return engine->peer_addr_str;
    }

    struct sockaddr_in *sa_peer = (struct sockaddr_in *)peer_addr;
    if (sa_peer->sin_family == AF_INET) {
        if (inet_ntop(sa_peer->sin_family, &sa_peer->sin_addr, engine->peer_addr_str, peer_addrlen) == NULL) {
            engine->peer_addr_str[0] = '\0';
        }

    } else {
        if (inet_ntop(sa_peer->sin_family, &((struct sockaddr_in6*)sa_peer)->sin6_addr,
                      engine->peer_addr_str, peer_addrlen) == NULL)
        {
            engine->peer_addr_str[0] = '\0';
        }
    }

    return engine->peer_addr_str;
}

char *
rqc_conn_addr_str(rqc_connection_t *conn)
{
    if (conn->local_addrlen == 0 || conn->peer_addrlen == 0
        || conn->scid_set.user_scid.cid_len == 0 || conn->dcid_set.current_dcid.cid_len == 0)
    {
        return "addr or cid not avail";
    }

    if (conn->addr_str_len == 0) {
        struct sockaddr_in *sa_local = (struct sockaddr_in *)conn->local_addr;
        struct sockaddr_in *sa_peer = (struct sockaddr_in *)conn->peer_addr;

        conn->addr_str_len = snprintf(conn->addr_str, sizeof(conn->addr_str), "l-%s-%d-%s p-%s-%d-%s",
                                      rqc_local_addr_str(conn->engine, (struct sockaddr*)sa_local, conn->local_addrlen),
                                      ntohs(sa_local->sin_port), rqc_scid_str(conn->engine, &conn->scid_set.user_scid),
                                      rqc_peer_addr_str(conn->engine, (struct sockaddr*)sa_peer, conn->peer_addrlen),
                                      ntohs(sa_peer->sin_port), rqc_dcid_str(conn->engine, &conn->dcid_set.current_dcid));
    }

    return conn->addr_str;
}

char *
rqc_path_addr_str(rqc_path_ctx_t *path)
{
    if (path->local_addrlen == 0 || path->peer_addrlen == 0
        || path->path_scid.cid_len == 0 || path->path_dcid.cid_len == 0)
    {
        return "addr or cid not avail";
    }

    if (path->addr_str_len == 0) {
        struct sockaddr_in *sa_local = (struct sockaddr_in *)path->local_addr;
        struct sockaddr_in *sa_peer = (struct sockaddr_in *)path->peer_addr;

        path->addr_str_len = snprintf(path->addr_str, sizeof(path->addr_str), "l-%s-%d-%s p-%s-%d-%s",
                                      rqc_local_addr_str(path->parent_conn->engine, (struct sockaddr*)sa_local, path->local_addrlen),
                                      ntohs(sa_local->sin_port), rqc_scid_str(path->parent_conn->engine, &path->path_scid),
                                      rqc_peer_addr_str(path->parent_conn->engine, (struct sockaddr*)sa_peer, path->peer_addrlen),
                                      ntohs(sa_peer->sin_port), rqc_dcid_str(path->parent_conn->engine, &path->path_dcid));
    }

    return path->addr_str;
}

static inline void
rqc_conn_set_default_settings(rqc_trans_settings_t *settings)
{
    memset(settings, 0, sizeof(rqc_trans_settings_t));

    /* transport parameter related attributes */
    settings->max_ack_delay              = RQC_DEFAULT_MAX_ACK_DELAY;
    settings->ack_delay_exponent         = RQC_DEFAULT_ACK_DELAY_EXPONENT;
    settings->max_udp_payload_size       = RQC_DEFAULT_MAX_UDP_PAYLOAD_SIZE;
}

static inline void
rqc_conn_init_trans_settings(rqc_connection_t *conn)
{
    /* set local and remote settings to default */
    rqc_trans_settings_t *ls = &conn->local_settings;
    rqc_trans_settings_t *rs = &conn->remote_settings;

    rqc_conn_set_default_settings(ls);
    rqc_conn_set_default_settings(rs);

    /* set local default setting values */
    ls->max_streams_bidi = 1024;
    ls->max_streams_uni = 1024;
    ls->max_stream_data_bidi_remote = RQC_MAX_RECV_WINDOW;
    ls->max_stream_data_uni = RQC_MAX_RECV_WINDOW;

    if (conn->conn_settings.enable_stream_rate_limit) {
        ls->max_stream_data_bidi_local = conn->conn_settings.init_recv_window;

    } else {
        ls->max_stream_data_bidi_local = RQC_MAX_RECV_WINDOW;
    }

    if (conn->conn_settings.recv_rate_bytes_per_sec) {
        ls->max_data = conn->conn_settings.recv_rate_bytes_per_sec * RQC_FC_INIT_RTT / 1000000;
        ls->max_data = rqc_max(RQC_MIN_RECV_WINDOW, ls->max_data);
        ls->max_data = rqc_min(RQC_MAX_RECV_WINDOW, ls->max_data);
    } else {
        /* max_data is the sum of stream_data on all uni and bidi streams */
        ls->max_data = ls->max_streams_bidi * ls->max_stream_data_bidi_local
            + ls->max_streams_uni * ls->max_stream_data_uni;
    }

    ls->max_idle_timeout = conn->conn_settings.idle_time_out;

    ls->max_udp_payload_size = conn->conn_settings.max_udp_payload_size;

    ls->max_ack_delay = conn->conn_settings.max_ack_delay;
}

static inline void
rqc_conn_init_flow_ctl(rqc_connection_t *conn)
{
    rqc_conn_flow_ctl_t *flow_ctl = &conn->conn_flow_ctl;
    rqc_trans_settings_t * settings = & conn->local_settings;

    /* TODO: send params are inited to be zero, until handshake done */
    flow_ctl->fc_max_data_can_send = settings->max_data; /* replace with the value specified by peer after handshake */
    flow_ctl->fc_max_data_can_recv = settings->max_data;
    flow_ctl->fc_max_streams_bidi_can_send = settings->max_streams_bidi; /* replace with the value specified by peer after handshake */
    flow_ctl->fc_max_streams_bidi_can_recv = settings->max_streams_bidi;
    flow_ctl->fc_max_streams_uni_can_send = settings->max_streams_uni; /* replace with the value specified by peer after handshake */
    flow_ctl->fc_max_streams_uni_can_recv = settings->max_streams_uni;
    flow_ctl->fc_data_sent = 0;
    flow_ctl->fc_data_recved = 0;
    flow_ctl->fc_recv_windows_size = settings->max_data;
    flow_ctl->fc_last_window_update_time = 0;
}

static inline void
rqc_conn_init_timer_manager(rqc_connection_t *conn)
{
    rqc_timer_manager_t *timer_manager = &conn->conn_timer_manager;
    rqc_usec_t now = rqc_monotonic_timestamp();

    rqc_timer_init(timer_manager, conn->log, conn);

    rqc_timer_set(timer_manager, RQC_TIMER_CONN_IDLE, now, rqc_conn_get_idle_timeout(conn) * 1000);

    if (conn->conn_settings.ping_on
        && conn->conn_type == RQC_CONN_TYPE_CLIENT)
    {
        rqc_timer_set(timer_manager, RQC_TIMER_PING, now, RQC_PING_TIMEOUT * 1000);
    }
}

rqc_connection_t *
rqc_conn_create(rqc_engine_t *engine, rqc_cid_t *dcid, rqc_cid_t *scid,
    const rqc_conn_settings_t *settings, void *user_data, rqc_conn_type_t type)
{
    rqc_connection_t *xc = NULL;
#ifdef RQC_PROTECT_POOL_MEM
    rqc_memory_pool_t *pool = rqc_create_pool(engine->config->conn_pool_size, settings->protect_pool_mem);
#else
    rqc_memory_pool_t *pool = rqc_create_pool(engine->config->conn_pool_size);
#endif
    if (pool == NULL) {
        return NULL;
    }

#ifdef RQC_PROTECT_POOL_MEM
    rqc_log(engine->log, RQC_LOG_INFO, "|mempool|protect:%d|page_sz:%z|",
            pool->protect_block, pool->page_size);
#endif

    xc = rqc_pcalloc(pool, sizeof(rqc_connection_t));
    if (xc == NULL) {
        goto fail;
    }

    xc->conn_settings = *settings;

    if (xc->conn_settings.max_udp_payload_size == 0) {
        xc->conn_settings.max_udp_payload_size = engine->default_conn_settings.max_udp_payload_size;
    }

    if (xc->conn_settings.initial_rtt == 0) {
        xc->conn_settings.initial_rtt = RQC_kInitialRtt_us;
    }

    if (xc->conn_settings.max_ack_delay == 0) {
        xc->conn_settings.max_ack_delay = RQC_DEFAULT_MAX_ACK_DELAY;
    }
    xc->conn_settings.max_ack_delay = rqc_min(xc->conn_settings.max_ack_delay, RQC_DEFAULT_MAX_ACK_DELAY);

    if (xc->conn_settings.init_recv_window) {
        xc->conn_settings.init_recv_window = rqc_max(xc->conn_settings.init_recv_window, RQC_QUIC_MAX_MSS);

    } else {
        xc->conn_settings.init_recv_window = RQC_MIN_RECV_WINDOW;
    }

    if (xc->conn_settings.max_pkt_out_size == 0) {
        xc->conn_settings.max_pkt_out_size = engine->default_conn_settings.max_pkt_out_size;
    }

    if (xc->conn_settings.max_pkt_out_size > RQC_MAX_PACKET_OUT_SIZE) {
        xc->conn_settings.max_pkt_out_size = RQC_MAX_PACKET_OUT_SIZE;
    }

    if (xc->conn_settings.ack_frequency == 0) {
        xc->conn_settings.ack_frequency = engine->default_conn_settings.ack_frequency;
    }

    if (xc->conn_settings.pto_backoff_factor == 0) {
        xc->conn_settings.pto_backoff_factor = engine->default_conn_settings.pto_backoff_factor;
    }

    if (xc->conn_settings.loss_detection_pkt_thresh == 0) {
        xc->conn_settings.loss_detection_pkt_thresh = engine->default_conn_settings.loss_detection_pkt_thresh;
    }

    xc->version = (type == RQC_CONN_TYPE_CLIENT) ? settings->proto_version : RQC_IDRAFT_INIT_VER;

    if (type == RQC_CONN_TYPE_CLIENT
        && !rqc_check_proto_version_valid(settings->proto_version))
    {
        xc->conn_settings.proto_version = RQC_VERSION_V1;
        xc->version = RQC_VERSION_V1;
    }

    /* make sure a 0-value config will not result in immediate timeout */
    if (xc->conn_settings.init_idle_time_out == 0) {
        xc->conn_settings.init_idle_time_out = RQC_CONN_INITIAL_IDLE_TIMEOUT;
    }

    if (xc->conn_settings.idle_time_out == 0) {
        xc->conn_settings.idle_time_out = RQC_CONN_DEFAULT_IDLE_TIMEOUT;
    }

    rqc_conn_init_trans_settings(xc);
    rqc_conn_init_flow_ctl(xc);

    xc->conn_pool = pool;

    rqc_init_cid_set(&xc->dcid_set);
    rqc_init_cid_set(&xc->scid_set);

    if (rqc_cid_set_add_path(&xc->dcid_set, RQC_INITIAL_PATH_ID) != RQC_OK) {
        goto fail;
    }

    if (rqc_cid_set_add_path(&xc->scid_set, RQC_INITIAL_PATH_ID) != RQC_OK) {
        goto fail;
    }

    rqc_cid_set_update_state(&xc->dcid_set, RQC_INITIAL_PATH_ID, RQC_CID_SET_USED);
    rqc_cid_set_update_state(&xc->scid_set, RQC_INITIAL_PATH_ID, RQC_CID_SET_USED);

    if (rqc_cid_set_insert_cid(&xc->dcid_set, dcid, RQC_CID_USED,
                               RQC_CONN_ACTIVE_CID_LIMIT, RQC_INITIAL_PATH_ID))
    {
        goto fail;
    }
    rqc_cid_copy(&(xc->dcid_set.current_dcid), dcid);
    rqc_hex_dump(xc->dcid_set.current_dcid_str, dcid->cid_buf, dcid->cid_len);
    xc->dcid_set.current_dcid_str[dcid->cid_len * 2] = '\0';

    if (rqc_cid_set_insert_cid(&xc->scid_set, scid, RQC_CID_USED,
                               RQC_CONN_ACTIVE_CID_LIMIT, RQC_INITIAL_PATH_ID))
    {
        goto fail;
    }
    rqc_cid_copy(&(xc->scid_set.user_scid), scid);
    rqc_hex_dump(xc->scid_set.original_scid_str, scid->cid_buf, scid->cid_len);
    xc->scid_set.original_scid_str[scid->cid_len * 2] = '\0';
    rqc_cid_set_set_largest_seq_or_rpt(&xc->scid_set, RQC_INITIAL_PATH_ID, scid->cid_seq_num);

    xc->engine = engine;
    xc->log = rqc_log_init(engine->log->log_level, engine->log->log_event, engine->log->qlog_importance, engine->log->log_timestamp,
                           engine->log->log_level_name, engine, engine->log->log_callbacks, engine->log->user_data);
    xc->log->scid = xc->scid_set.original_scid_str;
    xc->transport_cbs = engine->transport_cbs;
    xc->user_data = user_data;
    xc->discard_vn_flag = 0;
    xc->conn_type = type;
    xc->conn_flag = 0;
    xc->conn_state = (type == RQC_CONN_TYPE_SERVER) ? RQC_CONN_STATE_SERVER_INIT : RQC_CONN_STATE_CLIENT_INIT;
    xc->self_proto_ext.data = xc->self_proto_ext_buf;
    xc->self_proto_ext.len = 0;
    xc->peer_proto_ext.data = xc->peer_proto_ext_buf;
    xc->peer_proto_ext.len = 0;
    rqc_log_event(xc->log, CON_CONNECTION_STATE_UPDATED, xc);
    xc->conn_create_time = rqc_monotonic_timestamp();
    xc->handshake_complete_time = 0;
    xc->first_data_send_time = 0;
    xc->max_stream_id_bidi_remote = -1;
    xc->max_stream_id_uni_remote = -1;
    xc->pkt_out_size = rqc_min(xc->conn_settings.max_pkt_out_size, xc->conn_settings.max_udp_payload_size - RQC_PACKET_OUT_EXT_SPACE);
    xc->max_pkt_out_size = xc->conn_settings.max_pkt_out_size;

    xc->conn_send_queue = rqc_send_queue_create(xc);
    if (xc->conn_send_queue == NULL) {
        goto fail;
    }

    rqc_conn_init_timer_manager(xc);

    rqc_init_list_head(&xc->conn_write_streams);
    rqc_init_list_head(&xc->conn_read_streams);
    rqc_init_list_head(&xc->conn_closing_streams);
    rqc_init_list_head(&xc->conn_all_streams);

    /* create streams_hash */
    xc->streams_hash = rqc_pcalloc(xc->conn_pool, sizeof(rqc_id_hash_table_t));
    if (xc->streams_hash == NULL) {
        goto fail;
    }

    if (rqc_id_hash_init(xc->streams_hash,
                         rqc_default_allocator,
                         engine->config->streams_hash_bucket_size) == RQC_ERROR) {
        goto fail;
    }

    xc->passive_streams_hash = rqc_pcalloc(xc->conn_pool, sizeof(rqc_id_hash_table_t));
    if (xc->passive_streams_hash == NULL) {
        goto fail;
    }

    if (rqc_id_hash_init(xc->passive_streams_hash, rqc_default_allocator,
                         engine->config->streams_hash_bucket_size) == RQC_ERROR) {
        goto fail;
    }

    /* insert into engine's conns_hash */
    if (rqc_insert_conns_hash(engine->conns_hash, xc,
                              xc->scid_set.user_scid.cid_buf,
                              xc->scid_set.user_scid.cid_len))
    {
        goto fail;
    }

    if (rqc_conn_init_paths_list(xc) != RQC_OK) {
        goto fail;
    }

    xc->pkt_filter_cb = NULL;

    rqc_init_list_head(&xc->ping_notification_list);

    rqc_log(xc->log, RQC_LOG_INFO, "|success|scid:%s|dcid:%s|conn:%p|",
            rqc_scid_str(engine, &xc->scid_set.user_scid), rqc_dcid_str(engine, &xc->dcid_set.current_dcid), xc);
    rqc_log_event(xc->log, TRA_PARAMETERS_SET, xc, RQC_LOG_LOCAL_EVENT);

    return xc;

fail:
    if (xc != NULL) {
        rqc_conn_destroy(xc);
    }
    return NULL;
}

rqc_connection_t *
rqc_conn_server_create(rqc_engine_t *engine, const struct sockaddr *local_addr,
    socklen_t local_addrlen, const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    rqc_cid_t *dcid, rqc_cid_t *scid, rqc_conn_settings_t *settings, void *user_data)
{
    rqc_int_t           ret;
    rqc_connection_t   *conn;
    rqc_cid_t           new_scid;

    rqc_cid_copy(&new_scid, scid);

    /*
     * Server enable cid negotiate, or client initial dcid length not equal to server config length.
     * If use the peer's dcid as scid directly, must make sure
     * its length equals to the config cid_len, otherwise might fail
     * decoding dcid from subsequent short header packets
     */
    if (engine->config->cid_negotiate
        || new_scid.cid_len != engine->config->cid_len)
    {
        /* server generates it's own cid */
        if (rqc_generate_cid(engine, scid, &new_scid, 0) != RQC_OK) {
            rqc_log(engine->log, RQC_LOG_ERROR, "|fail to generate_cid|");
            return NULL;
        }
    }

    conn = rqc_conn_create(engine, dcid, &new_scid, settings, user_data, RQC_CONN_TYPE_SERVER);
    if (conn == NULL) {
        rqc_log(engine->log, RQC_LOG_ERROR, "|fail to create connection|");
        return NULL;
    }

    rqc_cid_copy(&conn->original_dcid, scid);

    if (rqc_cid_in_cid_set(&conn->scid_set, &conn->original_dcid, RQC_INITIAL_PATH_ID) == NULL) {
        /*
         * if server choose it's own cid, then if server Initial is lost,
         * and if client Initial retransmit, server might use odcid to
         * find the created conn
         */
        if (rqc_insert_conns_hash(engine->conns_hash, conn,
                                  conn->original_dcid.cid_buf,
                                  conn->original_dcid.cid_len))
        {
            goto fail;
        }

        rqc_log(conn->log, RQC_LOG_INFO, "|hash odcid conn|odcid:%s|conn:%p|",
                rqc_dcid_str(engine, &conn->original_dcid), conn);
    }

    ret = rqc_memcpy_with_cap(conn->local_addr, sizeof(conn->local_addr),
                              local_addr, local_addrlen);
    if (ret == RQC_OK) {
        conn->local_addrlen = local_addrlen;

    } else {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|local addr too large|addr_len:%d|",
                (int)local_addrlen);
        goto fail;
    }

    ret = rqc_memcpy_with_cap(conn->peer_addr, sizeof(conn->peer_addr),
                              peer_addr, peer_addrlen);
    if (ret == RQC_OK) {
        conn->peer_addrlen = peer_addrlen;

    } else {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|peer addr too large|addr_len:%d|",
                (int)peer_addrlen);
        goto fail;
    }

    ret = rqc_conn_server_init_path_addr(conn, RQC_INITIAL_PATH_ID,
                                         local_addr, local_addrlen,
                                         peer_addr, peer_addrlen);
    if (ret != RQC_OK) {
        goto fail;
    }

    rqc_log_event(conn->log, CON_CONNECTION_STARTED, conn, RQC_LOG_REMOTE_EVENT);

    if (conn->transport_cbs.server_accept) {
        if (conn->transport_cbs.server_accept(engine, conn, &conn->scid_set.user_scid, user_data) < 0) {
            rqc_log(engine->log, RQC_LOG_ERROR, "|server_accept callback return error|");
            RQC_CONN_ERR(conn, TRA_CONNECTION_REFUSED_ERROR);
            goto fail;
        }
        conn->conn_flag |= RQC_CONN_FLAG_UPPER_CONN_EXIST;
    }

    return conn;

fail:
    rqc_conn_destroy(conn);
    return NULL;
}

rqc_int_t
rqc_conn_close(rqc_engine_t *engine, const rqc_cid_t *cid)
{
    rqc_int_t ret;
    rqc_connection_t *conn;

    conn = rqc_engine_conns_hash_find(engine, cid, 's');
    if (!conn) {
        rqc_log(engine->log, RQC_LOG_ERROR, "|can not find connection|cid:%s",
                rqc_scid_str(engine, cid));
        return -RQC_ECONN_NFOUND;
    }

    rqc_log(conn->log, RQC_LOG_INFO, "|conn:%p|state:%s|flag:%s|", conn,
            rqc_conn_state_2_str(conn->conn_state), rqc_conn_flag_2_str(conn, conn->conn_flag));

    RQC_CONN_CLOSE_MSG(conn, "local close");

    if (conn->conn_state >= RQC_CONN_STATE_DRAINING) {
        return RQC_OK;
    }

    /* close connection after all data sent and acked or RQC_TIMER_LINGER_CLOSE timeout */
    rqc_usec_t now = rqc_monotonic_timestamp();
    rqc_usec_t pto = rqc_conn_get_max_pto(conn);

    if (conn->conn_settings.linger.linger_on && !rqc_send_queue_out_queue_empty(conn->conn_send_queue)) {
        conn->conn_flag |= RQC_CONN_FLAG_LINGER_CLOSING;
        rqc_usec_t linger_timeout = conn->conn_settings.linger.linger_timeout;
        rqc_timer_set(&conn->conn_timer_manager, RQC_TIMER_LINGER_CLOSE, now,
                      (linger_timeout ? linger_timeout : 3 * pto));
        goto end;
    }

    ret = rqc_conn_immediate_close(conn);
    if (ret) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_conn_immediate_close error|ret:%d|", ret);
        return ret;
    }

end:
    rqc_engine_remove_wakeup_queue(engine, conn);
    rqc_engine_add_active_queue(engine, conn);

    rqc_engine_wakeup_once(engine);

    return RQC_OK;
}

rqc_int_t
rqc_conn_close_with_error(rqc_connection_t *conn, uint64_t err_code)
{
    RQC_CONN_ERR(conn, err_code);
    return RQC_OK;
}

/* cleanup connection and wait for draining */
static void
rqc_conn_shutdown(rqc_connection_t *conn)
{
    rqc_usec_t now = rqc_monotonic_timestamp();
    rqc_usec_t pto = rqc_conn_get_max_pto(conn);
    if (!rqc_timer_is_set(&conn->conn_timer_manager, RQC_TIMER_CONN_DRAINING)) {
        rqc_timer_set(&conn->conn_timer_manager, RQC_TIMER_CONN_DRAINING, now, 3 * pto);
    }

    rqc_send_queue_drop_packets(conn);

    if (conn->the_path) {
        rqc_timer_unset(&(conn->the_path->path_send_ctl->path_timer_manager), RQC_TIMER_ACK);
        rqc_timer_unset(&(conn->the_path->path_send_ctl->path_timer_manager), RQC_TIMER_LOSS_DETECTION);
    }
}

rqc_int_t
rqc_conn_immediate_close(rqc_connection_t *conn)
{
    int ret;

    if (conn->conn_state >= RQC_CONN_STATE_DRAINING) {
        return RQC_OK;
    }

    if (conn->conn_type == RQC_CONN_TYPE_CLIENT && !rqc_conn_is_handshake_sent(conn))
    {
        conn->conn_state = RQC_CONN_STATE_CLOSED;
        rqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);
        rqc_conn_log(conn, RQC_LOG_ERROR, "|client cannot send CONNECTION_CLOSE before hskpkt sent|");
        return RQC_OK;
    }

    if (conn->conn_type == RQC_CONN_TYPE_SERVER && !rqc_conn_is_handshake_recvd(conn))
    {
        conn->conn_state = RQC_CONN_STATE_CLOSED;
        rqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);
        rqc_conn_log(conn, RQC_LOG_ERROR, "|server cannot send CONNECTION_CLOSE before hskpkt received|");
        return RQC_OK;
    }

    if (conn->conn_state < RQC_CONN_STATE_CLOSING) {
        rqc_conn_shutdown(conn);

        /* convert state to CLOSING */
        rqc_log(conn->log, RQC_LOG_INFO, "|state to closing|state:%s|flags:%s|",
                rqc_conn_state_2_str(conn->conn_state),
                rqc_conn_flag_2_str(conn, conn->conn_flag));
        conn->conn_state = RQC_CONN_STATE_CLOSING;
        rqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);
    }

    /*
     * [Transport] 10.3.  Immediate Close, During the closing period, an endpoint that sends a CONNECTION_CLOSE
     * frame SHOULD respond to any incoming packet that can be decrypted with another packet containing a CONNECTION_CLOSE
     * frame.  Such an endpoint SHOULD limit the number of packets it generates containing a CONNECTION_CLOSE frame.
     */
    if (conn->conn_close_count < MAX_RSP_CONN_CLOSE_CNT) {
        ret = rqc_write_conn_close_to_packet(conn, conn->conn_err);
        if (ret) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_conn_close_to_packet error|ret:%d|", ret);
        }
        ++conn->conn_close_count;
        rqc_log(conn->log, RQC_LOG_INFO, "|gen_conn_close|state:%s|flag:%s|",
            rqc_conn_state_2_str(conn->conn_state), rqc_conn_flag_2_str(conn, conn->conn_flag));
    }

    return RQC_OK;
}

static void
rqc_conn_destroy_ping_notification_list(rqc_connection_t *conn)
{
    rqc_list_head_t *pos, *next;
    rqc_ping_record_t *pr;
    rqc_list_for_each_safe(pos, next, &conn->ping_notification_list) {
        pr = rqc_list_entry(pos, rqc_ping_record_t, list);
        rqc_conn_destroy_ping_record(pr);
    }
}

static void
rqc_conn_destroy_cids(rqc_connection_t *conn)
{
    rqc_cid_inner_t *cid = NULL;
    rqc_list_head_t *pos, *next;
    rqc_cid_set_inner_t *inner_set = NULL;
    rqc_list_head_t *pos_set, *next_set;

    if (conn->engine->conns_hash) {
        if (rqc_find_conns_hash(conn->engine->conns_hash, conn,
                                conn->original_dcid.cid_buf,
                                conn->original_dcid.cid_len))
        {
            rqc_remove_conns_hash(conn->engine->conns_hash, conn,
                                  conn->original_dcid.cid_buf,
                                  conn->original_dcid.cid_len);
        }
        rqc_list_for_each_safe(pos_set, next_set, &conn->scid_set.cid_set_list) {
            inner_set = rqc_list_entry(pos_set, rqc_cid_set_inner_t, next);

            rqc_list_for_each_safe(pos, next, &inner_set->cid_list) {
                cid = rqc_list_entry(pos, rqc_cid_inner_t, list);
                if (rqc_find_conns_hash(conn->engine->conns_hash, conn,
                                        cid->cid.cid_buf, cid->cid.cid_len))
                {
                    rqc_remove_conns_hash(conn->engine->conns_hash, conn,
                                        cid->cid.cid_buf, cid->cid.cid_len);
                }
            }
        }
    }

    rqc_destroy_cid_set(&conn->scid_set);
    rqc_destroy_cid_set(&conn->dcid_set);
}

void
rqc_conn_destroy(rqc_connection_t *xc)
{
    if (!xc) {
        return;
    }

    if (xc->conn_flag & RQC_CONN_FLAG_TICKING) {
        rqc_log(xc->log, RQC_LOG_ERROR, "|in RQC_CONN_FLAG_TICKING|%p|", xc);
        xc->conn_state = RQC_CONN_STATE_CLOSED;
        rqc_log_event(xc->log, CON_CONNECTION_STATE_UPDATED, xc);
        return;
    }

    if (xc->log->log_level >= RQC_LOG_STATS) {
        rqc_conn_stats_t conn_stats;
        rqc_memzero(&conn_stats, sizeof(rqc_conn_stats_t));
        rqc_conn_get_stats_internal(xc, &conn_stats);

        rqc_log(xc->log, RQC_LOG_STATS, "|%p|alpn:%s|"
            "handshake_time:%ui|"
            "first_send_delay:%ui|conn_persist:%ui|err:0x%xi|close_msg:%s|%s|"
            "hsk_recv:%ui|close_recv:%ui|close_send:%ui|last_recv:%ui|last_send:%ui|"
            "rebind_count:%d|rebind_valid:%d|rtx_pkt:%ud|tlp_pkt:%ud|"
            "snd_pkt:%ud|spurious_loss:%ud|detected_loss:%ud|"
            "max_pto:%ud|finished_streams:%ud|cli_bidi_s:%ud|svr_bidi_s:%ud|"
            "max_po_size:%uz|max_acked_po_size:%uz|"
            ,
            xc, conn_stats.alpn,
            rqc_calc_delay(xc->handshake_complete_time, xc->conn_create_time),
            rqc_calc_delay(xc->first_data_send_time, xc->conn_create_time),
            rqc_monotonic_timestamp() - xc->conn_create_time,
            xc->conn_err, xc->conn_close_msg ? xc->conn_close_msg : "", rqc_conn_addr_str(xc),
            rqc_calc_delay(xc->handshake_recv_time, xc->conn_create_time),
            rqc_calc_delay(xc->conn_close_recv_time, xc->conn_create_time),
            rqc_calc_delay(xc->conn_close_send_time, xc->conn_create_time),
            rqc_calc_delay(xc->conn_last_recv_time, xc->conn_create_time),
            rqc_calc_delay(xc->conn_last_send_time, xc->conn_create_time),
            conn_stats.total_rebind_count, conn_stats.total_rebind_valid,
            conn_stats.lost_count, conn_stats.tlp_count,
            conn_stats.send_count, conn_stats.spurious_loss_count, xc->detected_loss_cnt,
            xc->max_pto_cnt, xc->finished_streams, xc->cli_bidi_streams, xc->svr_bidi_streams,
            xc->pkt_out_size, xc->max_acked_po_size
            );
    }

    rqc_log_event(xc->log, CON_CONNECTION_CLOSED, xc);

    rqc_engine_remove_wakeup_queue(xc->engine, xc);

    rqc_list_head_t *pos, *next;
    rqc_stream_t    *stream;

    /* destroy streams, must before conn_close_notify */
    rqc_list_for_each_safe(pos, next, &xc->conn_all_streams) {
        stream = rqc_list_entry(pos, rqc_stream_t, all_stream_list);
        RQC_STREAM_CLOSE_MSG(stream, "conn closed");
        rqc_destroy_stream(stream);
    }

    /* notify destruction */
    if (xc->conn_flag & RQC_CONN_FLAG_UPPER_CONN_EXIST) {
        /* ALPN negotiated, notify close through application layer protocol callback function */
        if (xc->app_proto_cbs.conn_cbs.conn_close_notify) {
            xc->app_proto_cbs.conn_cbs.conn_close_notify(xc, &xc->scid_set.user_scid,
                                                         xc->user_data,
                                                         xc->proto_data);

        } else if (xc->transport_cbs.server_refuse) {
            /* ALPN context is not initialized, HSK has not been received */
            xc->transport_cbs.server_refuse(xc->engine, xc, &xc->scid_set.user_scid, xc->user_data);
            rqc_log(xc->log, RQC_LOG_REPORT,
                    "|conn close notified by refuse|%s", rqc_conn_addr_str(xc));

        } else {
            rqc_log(xc->log, RQC_LOG_REPORT,
                    "|conn close event not notified|%s", rqc_conn_addr_str(xc));
        }

        xc->conn_flag &= ~RQC_CONN_FLAG_UPPER_CONN_EXIST;
    }

    rqc_send_queue_destroy(xc->conn_send_queue);

    /* free streams hash */
    if (xc->streams_hash) {
        rqc_id_hash_release(xc->streams_hash);
        xc->streams_hash = NULL;
    }

    if (xc->passive_streams_hash) {
        rqc_id_hash_release(xc->passive_streams_hash);
        xc->passive_streams_hash = NULL;
    }

    rqc_conn_destroy_paths_list(xc);

    rqc_conn_destroy_ping_notification_list(xc);

    /* remove from engine's conns_hash and destroy cid_set*/
    rqc_conn_destroy_cids(xc);

    rqc_log_release(xc->log);

    if (xc->alpn) {
        rqc_free(xc->alpn);
    }

    /* free pool, must be the last thing to do */
    if (xc->conn_pool) {
        rqc_destroy_pool(xc->conn_pool);
    }
}

rqc_int_t
rqc_conn_version_check(rqc_connection_t *conn, uint32_t version)
{
    rqc_engine_t *engine = conn->engine;
    int i = 0;

    if (conn->conn_type == RQC_CONN_TYPE_SERVER && conn->version == RQC_IDRAFT_INIT_VER) {

        uint32_t *list = engine->config->support_version_list;
        uint32_t count = engine->config->support_version_count;

        if (rqc_uint32_list_find(list, count, version) == -1) {
            return -RQC_EPROTO;
        }

        for (i = RQC_IDRAFT_INIT_VER + 1; i < RQC_IDRAFT_VER_NEGOTIATION; i++) {
            if (rqc_proto_version_value[i] == version) {
                conn->version = i;
                return RQC_OK;
            }
        }

        return -RQC_EPROTO;
    }

    return RQC_OK;
}

rqc_int_t
rqc_conn_send_version_negotiation(rqc_connection_t *conn)
{
    rqc_packet_out_t *packet_out = rqc_packet_out_get_and_insert_send(conn->conn_send_queue, RQC_PTYPE_VERSION_NEGOTIATION);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|get RQC_PTYPE_VERSION_NEGOTIATION error|");
        return -RQC_EWRITE_PKT;
    }

    unsigned char *p = packet_out->po_buf;
    /* first byte of packet */
    *p++ = (1 << 7);

    /* version */
    *(uint32_t *)p = 0;
    p += sizeof(uint32_t);

    /* dcid len */
    *p = conn->dcid_set.current_dcid.cid_len;
    ++p;

    /* dcid */
    memcpy(p, conn->dcid_set.current_dcid.cid_buf, conn->dcid_set.current_dcid.cid_len);
    p += conn->dcid_set.current_dcid.cid_len;

    /* original destination ID len */
    *p = conn->original_dcid.cid_len;
    ++p;

    /* original destination ID */
    memcpy(p, conn->original_dcid.cid_buf, conn->original_dcid.cid_len);
    p += conn->original_dcid.cid_len;

    /* set supported version list */
    uint32_t *version_list = conn->engine->config->support_version_list;
    uint32_t version_count = conn->engine->config->support_version_count;
    unsigned char *end = packet_out->po_buf + packet_out->po_buf_size;
    for (size_t i = 0; i < version_count; ++i) {
        if (p + sizeof(uint32_t) <= end) {
            *(uint32_t*)p = htonl(version_list[i]);
            p += sizeof(uint32_t);

        } else {
            break;
        }
    }

    /* set used size of packet */
    packet_out->po_used_size = p - packet_out->po_buf;

    /* push to conns queue */
    rqc_engine_remove_wakeup_queue(conn->engine, conn);
    rqc_engine_add_active_queue(conn->engine, conn);

    conn->conn_flag &= ~RQC_CONN_FLAG_VERSION_NEGOTIATION;
    return RQC_OK;
}

rqc_int_t
rqc_conn_client_on_alpn(rqc_connection_t *conn, const unsigned char *alpn, size_t alpn_len)
{
    rqc_int_t ret;

    /* save alpn */
    conn->alpn = rqc_calloc(1, alpn_len + 1);
    if (conn->alpn == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|malloc alpn buffer error|");
        return -RQC_EMALLOC;
    }

    rqc_memcpy(conn->alpn, alpn, alpn_len);
    conn->alpn_len = alpn_len;

    /* set quic callbacks to quic connection */
    ret = rqc_engine_get_alpn_callbacks(conn->engine, alpn, alpn_len, &conn->app_proto_cbs);
    if (ret != RQC_OK) {
        rqc_free(conn->alpn);
        conn->alpn = NULL;
        conn->alpn_len = 0;
        rqc_log(conn->log, RQC_LOG_ERROR, "|can't get application layer callback|ret:%d", ret);
        return ret;
    }

    return RQC_OK;
}

rqc_int_t
rqc_conn_server_on_alpn(rqc_connection_t *conn, const unsigned char *alpn, size_t alpn_len)
{
    rqc_int_t ret;

    /* save alpn */
    conn->alpn = rqc_calloc(1, alpn_len + 1);
    if (conn->alpn == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|malloc alpn buffer error|");
        return -RQC_EMALLOC;
    }

    rqc_memcpy(conn->alpn, alpn, alpn_len);
    conn->alpn_len = alpn_len;

    /* set quic callbacks to quic connection */
    ret = rqc_engine_get_alpn_callbacks(conn->engine, alpn, alpn_len, &conn->app_proto_cbs);
    if (ret != RQC_OK) {
        rqc_free(conn->alpn);
        conn->alpn = NULL;
        conn->alpn_len = 0;
        rqc_log(conn->log, RQC_LOG_ERROR, "|can't get application layer callback|ret:%d", ret);
        return ret;
    }

    /* do callback */
    if (conn->app_proto_cbs.conn_cbs.conn_create_notify) {
        rqc_proto_ext_t proto_ext = {
            .data = conn->peer_proto_ext_buf,
            .len = conn->peer_proto_ext.len,
        };
        rqc_proto_ext_t resp_proto_ext = {0};
        if (conn->app_proto_cbs.conn_cbs.conn_create_notify(conn, &conn->scid_set.user_scid,
            conn->user_data, conn->proto_data, &proto_ext, &resp_proto_ext))
        {
            goto err;
        }

        if (resp_proto_ext.len > RQC_MAX_PROTO_EXT_LEN
            || (resp_proto_ext.len > 0 && resp_proto_ext.data == NULL))
        {
            rqc_log(conn->log, RQC_LOG_ERROR, "|invalid resp proto ext|len:%uz|", resp_proto_ext.len);
            RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
            return -RQC_EPARAM;
        }

        if (resp_proto_ext.len > 0) {
            rqc_memcpy(conn->self_proto_ext_buf, resp_proto_ext.data, resp_proto_ext.len);
        }
        conn->self_proto_ext.len = resp_proto_ext.len;

        conn->conn_flag |= RQC_CONN_FLAG_UPPER_CONN_EXIST;
    }

    return RQC_OK;

err:
    RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
    return -TRA_INTERNAL_ERROR;
}

/* check whether if the dcid is valid for the connection */
rqc_int_t
rqc_conn_check_dcid(rqc_connection_t *conn, rqc_cid_t *dcid)
{
    rqc_int_t ret;

    rqc_cid_inner_t *scid = rqc_cid_set_search_cid(&conn->scid_set, dcid);
    if (scid == NULL) {
        return -RQC_ECONN_CID_NOT_FOUND;
    }

    if (scid->state == RQC_CID_UNUSED) {
        ret = rqc_cid_switch_to_next_state(&conn->scid_set, scid, RQC_CID_USED, scid->cid.path_id);
        if (ret < 0) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_cid_switch_to_next_state error|scid:%s|",
                    rqc_scid_str(conn->engine, &scid->cid));
            return ret;
        }
    }

    return RQC_OK;
}

rqc_usec_t
rqc_conn_next_wakeup_time(rqc_connection_t *conn)
{
    rqc_usec_t min_time = RQC_MAX_UINT64_VALUE;
    rqc_usec_t wakeup_time;
    rqc_timer_t *timer;

    for (rqc_timer_type_t type = 0; type < RQC_TIMER_N; ++type) {
        timer = &conn->conn_timer_manager.timer[type];
        if (timer->timer_is_set) {
            min_time = rqc_min(min_time, timer->expire_time);
        }
    }

    rqc_path_ctx_t *path = conn->the_path;
    if (path && path->path_state == RQC_PATH_STATE_ACTIVE) {
        for (rqc_timer_type_t type = 0; type < RQC_TIMER_N; ++type) {
            timer = &(path->path_send_ctl->path_timer_manager.timer[type]);
            if (timer->timer_is_set) {
                min_time = rqc_min(min_time, timer->expire_time);
            }
        }
    }

    wakeup_time = min_time == RQC_MAX_UINT64_VALUE ? 0 : min_time;

    return wakeup_time;
}

void
rqc_conn_timer_expire(rqc_connection_t *conn, rqc_usec_t now)
{
    rqc_timer_expire(&conn->conn_timer_manager, now);

    rqc_path_ctx_t *path = conn->the_path;;
    if (path && path->path_state < RQC_PATH_STATE_CLOSED) {
        rqc_timer_expire(&path->path_send_ctl->path_timer_manager, now);
    }
}

static inline uint8_t
rqc_conn_tolerant_error(rqc_int_t ret)
{
    if (-RQC_EVERSION == ret || -RQC_EILLPKT == ret || -RQC_EWAITING == ret || -RQC_EIGNORE_PKT == ret)
    {
        return RQC_TRUE;
    }
    return RQC_FALSE;
}

static inline void
rqc_conn_log_recvd_packet(rqc_connection_t *c, rqc_packet_in_t *pi,
    size_t udp_size, rqc_int_t err, rqc_usec_t timestamp)
{
    int index = c->rcv_pkt_stats.curr_index;
    c->rcv_pkt_stats.pkt_frames[index] = pi->pi_frame_types;
    c->rcv_pkt_stats.pkt_err[index] = err;
    c->rcv_pkt_stats.pkt_size[index] = pi->pi_pkt.length;
    c->rcv_pkt_stats.pkt_timestamp[index] = rqc_calc_delay(timestamp,
                                                           c->conn_create_time);
    c->rcv_pkt_stats.pkt_timestamp[index] /= 1000; // ms
    c->rcv_pkt_stats.pkt_udp_size[index] = udp_size;
    c->rcv_pkt_stats.pkt_types[index] = pi->pi_pkt.pkt_type;
    c->rcv_pkt_stats.pkt_pn[index] = pi->pi_pkt.pkt_num;
    c->rcv_pkt_stats.conn_rcvd_pkts++;
    c->rcv_pkt_stats.curr_index = (index + 1) % 3;
}

static rqc_int_t
rqc_conn_confirm_cid(rqc_connection_t *c, rqc_packet_t *pkt)
{
    /*
     *  after a successful process of Initial packet, SCID from Initial
     *  is not equal to what remembered when connection was created, as
     *  server is not willing to use the client's DCID as SCID;
     */

    rqc_int_t ret;

    if (!(c->conn_flag & RQC_CONN_FLAG_DCID_DONE)) {

        if (rqc_cid_in_cid_set(&c->dcid_set, &pkt->pkt_scid, RQC_INITIAL_PATH_ID) == NULL) {
            ret = rqc_cid_set_insert_cid(&c->dcid_set, &pkt->pkt_scid, RQC_CID_USED,
                                         RQC_CONN_ACTIVE_CID_LIMIT, RQC_INITIAL_PATH_ID);
            if (ret != RQC_OK) {
                rqc_log(c->log, RQC_LOG_ERROR,
                        "|rqc_cid_set_insert_cid error|limit:%ui|unused:%i|used:%i|",
                        RQC_CONN_ACTIVE_CID_LIMIT,
                        rqc_cid_set_get_unused_cnt(&c->dcid_set, RQC_INITIAL_PATH_ID),
                        rqc_cid_set_get_used_cnt(&c->dcid_set, RQC_INITIAL_PATH_ID));
                return ret;
            }
        }

        if (RQC_OK != rqc_cid_is_equal(&c->dcid_set.current_dcid, &pkt->pkt_scid)) {
            rqc_log(c->log, RQC_LOG_INFO, "|dcid change|ori:%s|new:%s|",
                    rqc_dcid_str(c->engine, &c->dcid_set.current_dcid), rqc_scid_str(c->engine, &pkt->pkt_scid));
            rqc_cid_copy(&c->dcid_set.current_dcid, &pkt->pkt_scid);
            rqc_cid_copy(&c->the_path->path_dcid, &pkt->pkt_scid);
        }

        c->conn_flag |= RQC_CONN_FLAG_DCID_DONE;
    }

    return RQC_OK;
}

static void
rqc_conn_record_single(rqc_connection_t *c, rqc_packet_in_t *packet_in)
{
    if (!rqc_has_packet_number(&packet_in->pi_pkt)) {
        return;
    }

    rqc_path_ctx_t *path = c->the_path;
    if (path == NULL) {
        return;
    }

    /* update path stats */
    if (packet_in->pi_frame_types & RQC_FRAME_BIT_STREAM) {
        path->path_send_ctl->ctl_app_bytes_recv += packet_in->buf_size;
    }

    rqc_pn_ctl_t *pn_ctl = rqc_get_pn_ctl(c, path);
    rqc_send_ctl_t *send_ctl = path->path_send_ctl;

    rqc_pkt_range_status range_status;
    int out_of_order = 0;
    rqc_packet_number_t pkt_num = packet_in->pi_pkt.pkt_num;

    range_status = rqc_recv_record_add(&pn_ctl->ctl_recv_record, pkt_num);
    if (range_status == RQC_PKTRANGE_OK) {
        if (RQC_IS_ACK_ELICITING(packet_in->pi_frame_types)) {
            ++send_ctl->ctl_ack_eliciting_pkt;

            if (pkt_num > send_ctl->ctl_largest_received || send_ctl->ctl_largest_received == RQC_MAX_UINT64_VALUE) {
                send_ctl->ctl_largest_received = pkt_num;
                send_ctl->ctl_largest_recv_time = packet_in->pkt_recv_time;
            }
        }

        if (pkt_num != rqc_recv_record_largest(&pn_ctl->ctl_recv_record)) {
            out_of_order = 1;
        }

        rqc_maybe_should_ack(c, path, pn_ctl, out_of_order, packet_in->pkt_recv_time);
    }
}

static rqc_int_t
rqc_conn_on_pkt_processed(rqc_connection_t *c, rqc_packet_in_t *pi, rqc_usec_t now)
{
    rqc_int_t ret = RQC_OK;
    switch (pi->pi_pkt.pkt_type) {
    case RQC_PTYPE_INIT:
        ret = rqc_conn_confirm_cid(c, &pi->pi_pkt);
        break;

    case RQC_PTYPE_SHORT_HEADER:
        rqc_conn_on_handshake_acked(c);
        break;

    default:
        break;
    }

    /* record packet */
    rqc_conn_record_single(c, pi);
    if (pi->pi_frame_types & (~(RQC_FRAME_BIT_STREAM|RQC_FRAME_BIT_PADDING))) {
        c->conn_flag |= RQC_CONN_FLAG_NEED_RUN;
    }

    c->conn_last_recv_time = now;

    return ret;
}

rqc_int_t
rqc_conn_process_packet(rqc_connection_t *conn,
    const unsigned char *packet_in_buf, size_t packet_in_size,
    rqc_usec_t recv_time)
{
    rqc_int_t ret = RQC_OK;
    const unsigned char *last_pos = NULL;
    const unsigned char *pos = packet_in_buf;                   /* start of QUIC pkt */
    const unsigned char *end = packet_in_buf + packet_in_size;  /* end of udp datagram */
    rqc_packet_in_t packet;

    /* process all QUIC packets in UDP datagram */
    while (pos < end) {
        last_pos = pos;

        /* init packet in */
        rqc_packet_in_t *packet_in = &packet;
        memset(packet_in, 0, sizeof(*packet_in));
        rqc_packet_in_init(packet_in, pos, end - pos, recv_time);

        /* packet_in->pos will update inside */
        ret = rqc_packet_process_single(conn, packet_in);

        rqc_conn_log_recvd_packet(conn, packet_in, packet_in_size, ret, recv_time);

        if (ret == RQC_OK) {
            ret = rqc_conn_on_pkt_processed(conn, packet_in, recv_time);

        } else if (rqc_conn_tolerant_error(ret)) {
            /* ignore the remain bytes */
            packet_in->pos = packet_in->last;
            ret = RQC_OK;
            goto end;
        }

        /* error occurred or read state is error */
        if (ret != RQC_OK || last_pos == packet_in->pos) {
            /* if last_pos equals packet_in->pos, might trigger infinite loop, return to avoid it */
            rqc_log(conn->log, RQC_LOG_ERROR, "|process packets err|ret:%d|pos:%p|buf:%p|buf_size:%uz|",
                    ret, packet_in->pos, packet_in->buf, packet_in->buf_size);
            return ret != RQC_OK ? ret : -RQC_ESYS;
        }

        /* consume all the bytes and start parse next QUIC packet */
        pos = packet_in->last;
        rqc_log_event(conn->log, TRA_PACKET_RECEIVED, packet_in);
    }
end:
    return ret;
}

void
rqc_conn_process_packet_recved_path(rqc_connection_t *conn, rqc_cid_t *scid,
    size_t packet_in_size, rqc_usec_t recv_time)
{
    if (conn->the_path) {
        rqc_send_ctl_on_dgram_received(conn->the_path->path_send_ctl, packet_in_size);
    }
}

static void
rqc_conn_schedule_packets(rqc_connection_t *conn,  rqc_list_head_t *head,
    rqc_bool_t  packets_are_limited_by_cc, rqc_send_type_t send_type)
{
    rqc_usec_t now = rqc_monotonic_timestamp();
    rqc_path_ctx_t *path = conn->the_path;
    rqc_send_ctl_t *send_ctl = path->path_send_ctl;

    rqc_list_head_t *pos, *next;
    rqc_packet_out_t *packet_out;

    rqc_list_for_each_safe(pos, next, head) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);

        if (packets_are_limited_by_cc &&
            !rqc_send_packet_cwnd_allows(send_ctl, packet_out, path->path_schedule_bytes, 0))
        {
            conn->sched_cc_blocked++;
            if (packet_out->po_sched_cwnd_blk_ts == 0) {
                packet_out->po_sched_cwnd_blk_ts = now;
            }
            return;
        }

        rqc_path_send_buffer_append(path, packet_out, &path->path_schedule_buf[send_type]);
    }
}

void
rqc_conn_schedule_packets_to_paths(rqc_connection_t *conn)
{
    /* do neither CC nor Pacing */
    rqc_list_head_t *head = &conn->conn_send_queue->sndq_pto_probe_packets;

    rqc_conn_schedule_packets(conn, head, RQC_FALSE, RQC_SEND_TYPE_PTO_PROBE);

    head = &conn->conn_send_queue->sndq_lost_packets;

    rqc_conn_schedule_packets(conn, head, RQC_TRUE, RQC_SEND_TYPE_RETRANS);

    head = &conn->conn_send_queue->sndq_send_packets_high_pri;
    rqc_conn_schedule_packets(conn, head, RQC_FALSE, RQC_SEND_TYPE_NORMAL_HIGH_PRI);

    head = &conn->conn_send_queue->sndq_send_packets;
    rqc_conn_schedule_packets(conn, head, RQC_TRUE, RQC_SEND_TYPE_NORMAL);
}

static inline void
rqc_conn_log_sent_packet(rqc_connection_t *c, rqc_packet_out_t *po,
    rqc_usec_t timestamp)
{
    int index = c->snd_pkt_stats.curr_index;
    c->snd_pkt_stats.pkt_frames[index] = po->po_frame_types;
    c->snd_pkt_stats.pkt_size[index] = po->po_used_size;
    c->snd_pkt_stats.pkt_timestamp[index] = rqc_calc_delay(timestamp,
                                                           c->conn_create_time);
    c->snd_pkt_stats.pkt_timestamp[index] /= 1000;
    c->snd_pkt_stats.pkt_types[index] = po->po_pkt.pkt_type;
    c->snd_pkt_stats.pkt_pn[index] = po->po_pkt.pkt_num;
    c->snd_pkt_stats.conn_sent_pkts++;
    c->snd_pkt_stats.curr_index = (index + 1) % 3;
}

/* send data with callback, and process callback errors */
static ssize_t
rqc_send(rqc_connection_t *conn, rqc_path_ctx_t *path, unsigned char *data, unsigned int len)
{
    ssize_t sent = -RQC_ESOCKET;

    if (conn->pkt_filter_cb) {
        sent = conn->pkt_filter_cb(data, len, (struct sockaddr *)conn->peer_addr,
                                   conn->peer_addrlen, conn->pkt_filter_cb_user_data);
        if (sent < 0) {
            rqc_log(conn->log, RQC_LOG_ERROR,  "|pkt_filter_cb error|conn:%p|"
                    "size:%ud|sent:%z|", conn, len, sent);

            return sent == RQC_SOCKET_EAGAIN ? -RQC_EAGAIN : -RQC_EPACKET_FILETER_CALLBACK;
        }
        sent = len;

    } else {
        sent = conn->transport_cbs.write_socket(data, len,
                                                (struct sockaddr *)conn->peer_addr,
                                                conn->peer_addrlen,
                                                rqc_conn_get_user_data(conn));
        if (sent != len) {
            rqc_log(conn->log, RQC_LOG_ERROR,
                    "|write_socket error|conn:%p|size:%ud|sent:%z|", conn, len, sent);

            /* if callback return RQC_SOCKET_ERROR, close the connection */
            if (sent == RQC_SOCKET_ERROR) {
                rqc_log(conn->log, RQC_LOG_ERROR, "|conn:%p|socket exception, close connection|", conn);
                conn->conn_state = RQC_CONN_STATE_CLOSED;
                rqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);
            }

            return sent == RQC_SOCKET_EAGAIN ? -RQC_EAGAIN : -RQC_ESOCKET;
        }
    }

    rqc_log_event(conn->log, TRA_DATAGRAMS_SENT, sent, path->path_id);

    return sent;
}

/* send packets which have no packet number */
static ssize_t
rqc_process_packet_without_pn(rqc_connection_t *conn, rqc_path_ctx_t *path, rqc_packet_out_t *packet_out)
{
    /* directly send to peer */
    ssize_t sent = rqc_send(conn, path, packet_out->po_buf, packet_out->po_used_size);
    rqc_log_event(conn->log, TRA_PACKET_SENT, conn, packet_out, path, 0, sent, 0);
    if (sent > 0) {
        rqc_conn_log_sent_packet(conn, packet_out, rqc_monotonic_timestamp());
    }
    return sent;
}

/* send data in packet number space */
static ssize_t
rqc_send_packet_with_pn(rqc_connection_t *conn, rqc_path_ctx_t *path, rqc_packet_out_t *packet_out)
{
    /* record the send time of packet */
    rqc_usec_t now = rqc_monotonic_timestamp();
    packet_out->po_sent_time = now;

    /* send data */
    ssize_t sent = rqc_send(conn, path, packet_out->po_buf, packet_out->po_used_size);
    if (sent != packet_out->po_used_size) {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|write_socket error|conn:%p|path:%ui|pkt_num:%ui|size:%ud|sent:%z|pkt_type:%s|frame:%s|now:%ui|",
                conn, path->path_id, packet_out->po_pkt.pkt_num, packet_out->po_used_size, sent,
                rqc_pkt_type_2_str(packet_out->po_pkt.pkt_type),
                rqc_frame_type_2_str(conn->engine, packet_out->po_frame_types), now);
        return sent;

    } else {
        rqc_log_event(conn->log, TRA_PACKET_SENT, conn, packet_out, path, now, sent, 1);
    }

    /* deliver packet to send control */
    rqc_pn_ctl_t *pn_ctl = rqc_get_pn_ctl(conn, path);

    rqc_conn_log_sent_packet(conn, packet_out, now);
    rqc_send_ctl_on_packet_sent(path->path_send_ctl, pn_ctl, packet_out, now);
    return sent;
}

static void
rqc_encode_packet_with_pn(rqc_connection_t *conn, rqc_path_ctx_t *path, rqc_packet_out_t *packet_out)
{
    /* update dcid by send path */
    rqc_short_packet_update_dcid(packet_out, path->path_dcid);

    /* generate packet number */
    rqc_pn_ctl_t *pn_ctl = rqc_get_pn_ctl(conn, path);
    rqc_usec_t current_time = rqc_monotonic_timestamp();
    rqc_send_ctl_set_next_pn_for_packet(conn, pn_ctl, packet_out, current_time);

    rqc_write_packet_number(packet_out->po_ppktno, packet_out->po_pkt.pkt_num, RQC_PKTNO_CLASS);
    rqc_update_packet_length(packet_out);
}

/* process and send packet which has a packet number */
static ssize_t
rqc_process_packet_with_pn(rqc_connection_t *conn, rqc_path_ctx_t *path, rqc_packet_out_t *packet_out)
{
    rqc_encode_packet_with_pn(conn, path, packet_out);
    /* send packet in packet number space */
    return rqc_send_packet_with_pn(conn, path, packet_out);
}

static ssize_t
rqc_path_send_one_packet(rqc_connection_t *conn, rqc_path_ctx_t *path, rqc_packet_out_t *packet_out)
{
    if (rqc_has_packet_number(&packet_out->po_pkt)) {
        return rqc_process_packet_with_pn(conn, path, packet_out);

    } else {
        return rqc_process_packet_without_pn(conn, path, packet_out);
    }
}

static rqc_int_t
rqc_check_acked_or_dropped_pkt(rqc_connection_t *conn,
    rqc_packet_out_t *packet_out, rqc_send_type_t send_type)
{
    if (rqc_send_ctl_indirectly_ack_or_drop_po(conn, packet_out)) {
        return RQC_TRUE;
    }

    if (send_type == RQC_SEND_TYPE_RETRANS) {
        /* If not a TLP packet, mark it LOST */
        packet_out->po_flag |= RQC_POF_LOST;
    }

    return RQC_FALSE;
}

static void
rqc_path_send_packets(rqc_connection_t *conn, rqc_path_ctx_t *path,
    rqc_list_head_t *head, int congest, rqc_send_type_t send_type)
{
    ssize_t ret = 0;
    rqc_list_head_t  *pos, *next;
    rqc_packet_out_t *packet_out;

    rqc_send_ctl_t *send_ctl = path->path_send_ctl;
    rqc_send_queue_t *send_queue = conn->conn_send_queue;

    rqc_usec_t now = rqc_monotonic_timestamp();

    rqc_list_for_each_safe(pos, next, &path->path_schedule_buf[send_type]) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);

        if (rqc_check_acked_or_dropped_pkt(conn, packet_out, send_type)) {
            continue;
        }

        /* check cc limit */
        if (congest
            && !rqc_send_packet_check_cc(send_ctl, packet_out, 0, now))
        {
            send_ctl->ctl_conn->send_cc_blocked++;
            break;
        }

        ret = rqc_path_send_one_packet(conn, path, packet_out);
        if (ret < 0) {
            break;
        }

        if (RQC_CAN_IN_FLIGHT(packet_out->po_frame_types)
            && rqc_pacing_is_on(&send_ctl->ctl_pacing))
        {
            rqc_pacing_on_packet_sent(&send_ctl->ctl_pacing, packet_out->po_used_size);
        }

        /* move send list to unacked list */
        rqc_path_send_buffer_remove(path, packet_out);
        if (RQC_IS_ACK_ELICITING(packet_out->po_frame_types)) {
            rqc_send_queue_insert_unacked(packet_out,
                                          &send_queue->sndq_unacked_packets,
                                          send_queue);

        } else {
            rqc_send_queue_insert_free(packet_out, &send_queue->sndq_free_packets, send_queue);
        }
    }

    /* @FIXME: in the case of EAGAIN, we should not reschedule packets. */
    if (ret < 0 && ret != -RQC_EAGAIN) {
        rqc_path_send_buffer_clear(conn, path, head, send_type);
    }
}

void
rqc_conn_transmit_pto_probe_packets(rqc_connection_t *conn)
{
    /* do neither CC nor Pacing */
    int congest = 0;
    rqc_list_head_t *head = &conn->conn_send_queue->sndq_pto_probe_packets;
    rqc_path_send_packets(conn, conn->the_path, head, congest, RQC_SEND_TYPE_PTO_PROBE);
}

void
rqc_conn_retransmit_lost_packets(rqc_connection_t *conn)
{
    /* do congestion control */
    int congest = 1;
    rqc_list_head_t *head = &conn->conn_send_queue->sndq_lost_packets;
    rqc_path_send_packets(conn, conn->the_path, head, congest, RQC_SEND_TYPE_RETRANS);
}

void
rqc_conn_send_packets(rqc_connection_t *conn)
{
    /* high priority packets are not limited by CC */
    int congest = 0;
    rqc_list_head_t *head = &conn->conn_send_queue->sndq_send_packets_high_pri;
    rqc_path_send_packets(conn, conn->the_path, head, congest, RQC_SEND_TYPE_NORMAL_HIGH_PRI);

    congest = 1;
    head = &conn->conn_send_queue->sndq_send_packets;
    rqc_path_send_packets(conn, conn->the_path, head, congest, RQC_SEND_TYPE_NORMAL);
}

static void
rqc_on_packets_send_burst(rqc_connection_t *conn, rqc_path_ctx_t *path, ssize_t sent, rqc_usec_t now, rqc_send_type_t send_type)
{
    rqc_list_head_t  *pos, *next;
    rqc_packet_out_t *packet_out;
    int remove_count = 0; /* remove from send */

    rqc_send_ctl_t *send_ctl = path->path_send_ctl;
    rqc_pn_ctl_t *pn_ctl = rqc_get_pn_ctl(conn, path);
    rqc_send_queue_t *send_queue = conn->conn_send_queue;

    rqc_list_for_each_safe(pos, next, &path->path_schedule_buf[send_type]) {
        if (remove_count >= sent) {
            break;
        }

        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);

        rqc_conn_log_sent_packet(conn, packet_out, now);

        if (rqc_has_packet_number(&packet_out->po_pkt)) {
            /* count packets with pkt_num in the send control */
            if (RQC_CAN_IN_FLIGHT(packet_out->po_frame_types
                && rqc_pacing_is_on(&send_ctl->ctl_pacing)))
            {
                rqc_pacing_on_packet_sent(&send_ctl->ctl_pacing, packet_out->po_used_size);
            }

            rqc_send_ctl_on_packet_sent(send_ctl, pn_ctl, packet_out, now);
            rqc_path_send_buffer_remove(path, packet_out);
            if (RQC_IS_ACK_ELICITING(packet_out->po_frame_types)) {
                rqc_send_queue_insert_unacked(packet_out,
                                              &send_queue->sndq_unacked_packets,
                                              send_queue);
            } else {
                rqc_send_queue_insert_free(packet_out, &send_queue->sndq_free_packets, send_queue);
            }
        } else {
            /* packets with no packet number can't be acknowledged, hence they need no control */
            rqc_path_send_buffer_remove(path, packet_out);
            rqc_send_queue_insert_free(packet_out, &send_queue->sndq_free_packets, send_queue);
        }

        remove_count++;
    }
}

static ssize_t
rqc_send_burst(rqc_connection_t *conn, rqc_path_ctx_t *path, struct iovec *iov, int cnt)
{
    ssize_t sent_size = 0;
    int sent_cnt = 0;

    if (conn->pkt_filter_cb) {

        for (sent_cnt = 0; sent_cnt < cnt; sent_cnt++) {
            sent_size = conn->pkt_filter_cb(iov[sent_cnt].iov_base,
                                       iov[sent_cnt].iov_len,
                                       (struct sockaddr *)conn->peer_addr,
                                       conn->peer_addrlen,
                                       conn->pkt_filter_cb_user_data);
            if (sent_size < 0) {
                rqc_log(conn->log, RQC_LOG_ERROR,
                        "|pkt_filter_cb error|conn:%p|"
                        "size:%ud|sent:%z|sent_cnt:%d|",
                        conn, iov[sent_cnt].iov_len, sent_size,
                        sent_cnt);

                sent_size = sent_size == RQC_SOCKET_EAGAIN ? -RQC_EAGAIN : -RQC_ESOCKET;
                break;
            }
        }

        if (sent_size == -RQC_EAGAIN && sent_cnt == 0) {
            sent_cnt = -RQC_EAGAIN;

        } else if (sent_size == -RQC_ESOCKET) {
            sent_cnt = -RQC_EPACKET_FILETER_CALLBACK;
        }

    } else {
        sent_cnt = conn->transport_cbs.write_mmsg(iov, cnt,
                                              (struct sockaddr *)conn->peer_addr,
                                              conn->peer_addrlen,
                                              rqc_conn_get_user_data(conn));
        if (sent_cnt < 0) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|error send mmsg|");
            if (sent_cnt == RQC_SOCKET_ERROR) {
                rqc_log(conn->log, RQC_LOG_ERROR, "|socket exception, close connection|");
                conn->conn_state = RQC_CONN_STATE_CLOSED;
                rqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);
            }

            sent_cnt = sent_cnt == RQC_SOCKET_EAGAIN ? -RQC_EAGAIN : -RQC_ESOCKET;
        }
    }

    return sent_cnt;
}

static void
rqc_encode_packet_with_pn_ex(rqc_connection_t *conn, rqc_path_ctx_t *path, rqc_packet_out_t *packet_out, rqc_usec_t current_time)
{
    /* update dcid by send path */
    rqc_short_packet_update_dcid(packet_out, path->path_dcid);

    /* generate packet number */
    rqc_pn_ctl_t *pn_ctl = rqc_get_pn_ctl(conn, path);
    rqc_send_ctl_set_next_pn_for_packet(conn, pn_ctl, packet_out, current_time);

    rqc_write_packet_number(packet_out->po_ppktno, packet_out->po_pkt.pkt_num, RQC_PKTNO_CLASS);
    rqc_update_packet_length(packet_out);

    packet_out->po_sent_time = current_time;
}

static ssize_t
rqc_path_send_burst_packets(rqc_connection_t *conn, rqc_path_ctx_t *path,
    int congest, rqc_send_type_t send_type)
{
    ssize_t           ret;
    struct iovec      iov_array[RQC_MAX_SEND_MSG_ONCE];
    int               burst_cnt = 0;
    rqc_packet_out_t *packet_out;
    rqc_list_head_t  *pos, *next;
    rqc_send_ctl_t   *send_ctl = path->path_send_ctl;
    uint32_t          total_bytes_to_send = 0;

    /* process packets */
    rqc_usec_t now = rqc_monotonic_timestamp();
    rqc_list_for_each_safe(pos, next, &path->path_schedule_buf[send_type]) {
        /* process one packet */
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
        iov_array[burst_cnt].iov_base = packet_out->po_buf;
        iov_array[burst_cnt].iov_len = packet_out->po_used_size;

        if (rqc_has_packet_number(&packet_out->po_pkt)) {
            if (rqc_check_acked_or_dropped_pkt(conn, packet_out, send_type)) {
                continue;
            }

            /* check cc limit */
            if (congest
                && !rqc_send_packet_check_cc(send_ctl, packet_out, total_bytes_to_send, now))
            {
                send_ctl->ctl_conn->send_cc_blocked++;
                break;
            }

            rqc_encode_packet_with_pn_ex(conn, path, packet_out, now);
            total_bytes_to_send += packet_out->po_used_size;
        }

        /* reach send limit, break and send packets */
        burst_cnt++;
        if (burst_cnt >= RQC_MAX_SEND_MSG_ONCE) {
            burst_cnt = RQC_MAX_SEND_MSG_ONCE;
            break;
        }
    }

    /* nothing to send, return */
    if (burst_cnt == 0) {
        return burst_cnt;
    }

    /* burst send packets */
    ret = rqc_send_burst(conn, path, iov_array, burst_cnt);
    if (ret < 0) {
        return ret;

    } else if (ret != burst_cnt) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|error send msg|sent:%ui||cnt:%d|", ret, burst_cnt);
    }

    rqc_on_packets_send_burst(conn, path, ret, now, send_type);
    return ret;
}

static void
rqc_path_send_packets_batch(rqc_connection_t *conn, rqc_path_ctx_t *path,
    rqc_list_head_t *head, int congest, rqc_send_type_t send_type)
{
    ssize_t send_burst_count = 0;

    while (!(rqc_list_empty(&path->path_schedule_buf[send_type]))) {
        send_burst_count = rqc_path_send_burst_packets(conn, path, congest, send_type);
        if (send_burst_count != RQC_MAX_SEND_MSG_ONCE) {
            break;
        }
    }

    /* @FIXME: in the case of EAGAIN, we should not reschedule packets. */
    if (send_burst_count < 0 && send_burst_count != -RQC_EAGAIN) {
        rqc_path_send_buffer_clear(conn, path, head, send_type);
    }

}

void
rqc_conn_transmit_pto_probe_packets_batch(rqc_connection_t *conn)
{
    /* probe packets MUST NOT be blocked by the congestion controller */
    int congest = 0;
    rqc_list_head_t *head = &conn->conn_send_queue->sndq_pto_probe_packets;
    rqc_path_send_packets_batch(conn, conn->the_path, head, congest, RQC_SEND_TYPE_PTO_PROBE);
}

void
rqc_conn_retransmit_lost_packets_batch(rqc_connection_t *conn)
{
    /* do congestion control */
    int congest = 1;
    rqc_list_head_t *head = &conn->conn_send_queue->sndq_lost_packets;
    rqc_path_send_packets_batch(conn, conn->the_path, head, congest, RQC_SEND_TYPE_RETRANS);
}

void
rqc_conn_send_packets_batch(rqc_connection_t *conn)
{
    int congest = 0;
    rqc_list_head_t *head = &conn->conn_send_queue->sndq_send_packets_high_pri;
    rqc_path_send_packets_batch(conn, conn->the_path, head, congest, RQC_SEND_TYPE_NORMAL_HIGH_PRI);

    congest = 1;
    head = &conn->conn_send_queue->sndq_send_packets;
    rqc_path_send_packets_batch(conn, conn->the_path, head, congest, RQC_SEND_TYPE_NORMAL);

    return;
}

void
rqc_conn_decrease_unacked_stream_ref(rqc_connection_t *conn, rqc_packet_out_t *packet_out)
{
    int first_time_ack = 1;
    if (packet_out->po_flag & RQC_POF_STREAM_UNACK) {
        first_time_ack = first_time_ack && (!packet_out->po_acked);
        if (packet_out->po_origin) {
            first_time_ack = first_time_ack && (!packet_out->po_origin->po_acked);
        }
        if (first_time_ack) {
            rqc_stream_t *stream;
            for (int i = 0; i < RQC_MAX_STREAM_FRAME_IN_PO; i++) {
                if (packet_out->po_stream_frames[i].ps_is_used == 0) {
                    break;
                }
                stream = rqc_find_stream_by_id(packet_out->po_stream_frames[i].ps_stream_id, conn->streams_hash);
                if (stream != NULL) {
                    if (stream->stream_unacked_pkt == 0) {
                        rqc_log(conn->log, RQC_LOG_ERROR, "|stream_unacked_pkt too small|");

                    } else {
                        stream->stream_unacked_pkt--;
                    }

                    if (packet_out->po_stream_frames[i].ps_has_fin && stream->stream_stats.first_fin_ack_time == 0) {
                        stream->stream_stats.first_fin_ack_time = rqc_monotonic_timestamp();
                    }

                    /* Update stream state */
                    if (stream->stream_unacked_pkt == 0 && stream->stream_state_send == RQC_SEND_STREAM_ST_DATA_SENT) {
                        rqc_stream_send_state_update(stream, RQC_SEND_STREAM_ST_DATA_RECVD);
                        rqc_stream_maybe_need_close(stream);
                    }
                }
            }
        }
        packet_out->po_flag &= ~RQC_POF_STREAM_UNACK;
    }
}

void
rqc_conn_increase_unacked_stream_ref(rqc_connection_t *conn, rqc_packet_out_t *packet_out)
{
    if ((packet_out->po_frame_types & RQC_FRAME_BIT_STREAM)
        && !(packet_out->po_flag & RQC_POF_STREAM_UNACK))
    {
        if ((!packet_out->po_origin)) {
            rqc_stream_t *stream;
            for (int i = 0; i < RQC_MAX_STREAM_FRAME_IN_PO; i++) {
                if (packet_out->po_stream_frames[i].ps_is_used == 0) {
                    break;
                }
                stream = rqc_find_stream_by_id(packet_out->po_stream_frames[i].ps_stream_id, conn->streams_hash);
                if (stream != NULL) {
                    stream->stream_unacked_pkt++;
                    /* Update stream state */
                    if (stream->stream_state_send == RQC_SEND_STREAM_ST_READY) {
                        rqc_stream_send_state_update(stream, RQC_SEND_STREAM_ST_SEND);
                    }
                    if (packet_out->po_stream_frames[i].ps_has_fin
                        && stream->stream_state_send == RQC_SEND_STREAM_ST_SEND)
                    {
                        rqc_stream_send_state_update(stream, RQC_SEND_STREAM_ST_DATA_SENT);
                    }
                }
            }
        }
        packet_out->po_flag |= RQC_POF_STREAM_UNACK;
    }
}

void
rqc_conn_update_stream_stats_on_sent(rqc_connection_t *conn, rqc_send_ctl_t *ctl,
    rqc_packet_out_t *packet_out, rqc_usec_t now)
{
    rqc_stream_id_t stream_id;
    rqc_stream_t *stream[RQC_MAX_STREAM_FRAME_IN_PO] = {0};
    int stream_cnt = 0;
    int i, j;

    if (packet_out->po_frame_types & (RQC_FRAME_BIT_STREAM | RQC_FRAME_BIT_RESET_STREAM)) {
        for (i = 0; i < RQC_MAX_STREAM_FRAME_IN_PO; i++) {
            if (packet_out->po_stream_frames[i].ps_is_used == 0) {
                break;
            }
            stream_id = packet_out->po_stream_frames[i].ps_stream_id;
            stream[stream_cnt] = rqc_find_stream_by_id(stream_id, conn->streams_hash);
            for (j = 0; j < stream_cnt; j++) {
                if (stream[j] == stream[stream_cnt]) {
                    break;
                }
            }

            if (stream[stream_cnt]) {
                if (stream[stream_cnt]->stream_stats.first_snd_time == 0) {
                    stream[stream_cnt]->stream_stats.first_snd_time = now;
                }
                if (packet_out->po_stream_frames[i].ps_has_fin) {
                    stream[stream_cnt]->stream_stats.local_fin_snd_time = now;
                    if (stream[stream_cnt]->stream_stats.local_fst_fin_snd_time == 0) {
                        stream[stream_cnt]->stream_stats.local_fst_fin_snd_time = now;
                    }
                }
                if (packet_out->po_stream_frames[i].ps_is_reset) {
                    stream[stream_cnt]->stream_stats.local_reset_time = now;
                }
                // do not repeatedly count
                if (j == stream_cnt) {
                    if (packet_out->po_sched_cwnd_blk_ts) {
                        stream[stream_cnt]->stream_stats.sched_cwnd_blk_duration += now - packet_out->po_sched_cwnd_blk_ts;
                        stream[stream_cnt]->stream_stats.sched_cwnd_blk_cnt++;
                    }
                    if (packet_out->po_send_cwnd_blk_ts) {
                        stream[stream_cnt]->stream_stats.send_cwnd_blk_duration += now - packet_out->po_send_cwnd_blk_ts;
                        stream[stream_cnt]->stream_stats.send_cwnd_blk_cnt++;
                    }
                    if (packet_out->po_send_pacing_blk_ts) {
                        stream[stream_cnt]->stream_stats.send_pacing_blk_duration += now - packet_out->po_send_pacing_blk_ts;
                        stream[stream_cnt]->stream_stats.send_pacing_blk_cnt++;
                    }
                    if (packet_out->po_flag & (RQC_POF_TLP | RQC_POF_LOST)) {
                        stream[stream_cnt]->stream_stats.retrans_pkt_cnt++;
                    }
                    stream[stream_cnt]->stream_stats.sent_pkt_cnt++;
                    stream[stream_cnt]->stream_stats.max_pto_backoff = rqc_max(stream[stream_cnt]->stream_stats.max_pto_backoff, ctl->ctl_pto_count);
                    stream_cnt++;
                }
            }
        }
    }
}

rqc_usec_t
rqc_conn_get_max_pto(rqc_connection_t *conn)
{
    rqc_path_ctx_t *path = conn->the_path;
    if (path && path->path_state == RQC_PATH_STATE_ACTIVE) {
        return rqc_send_ctl_calc_pto(path->path_send_ctl);
    }
    return 0;
}

uint32_t
rqc_conn_get_max_pto_backoff(rqc_connection_t *conn, uint8_t available_only)
{
    rqc_path_ctx_t *path = conn->the_path;
    if (path && path->path_state == RQC_PATH_STATE_ACTIVE) {
        return path->path_send_ctl->ctl_pto_count;
    }
    return 0;
}

rqc_usec_t
rqc_conn_get_min_srtt(rqc_connection_t *conn, rqc_bool_t available_only)
{
    rqc_path_ctx_t *path = conn->the_path;
    if (path && path->path_state == RQC_PATH_STATE_ACTIVE) {
        return path->path_send_ctl->ctl_srtt;
    }
    return RQC_MAX_UINT64_VALUE;
}

rqc_usec_t
rqc_conn_get_max_srtt(rqc_connection_t *conn)
{
    rqc_path_ctx_t *path = conn->the_path;
    if (path && path->path_state == RQC_PATH_STATE_ACTIVE) {
        return path->path_send_ctl->ctl_srtt;
    }
    return 0;
}

void rqc_conn_check_app_limit(rqc_connection_t *conn)
{
    rqc_path_ctx_t *path = conn->the_path;
    if (path && path->path_state == RQC_PATH_STATE_ACTIVE) {
        if (rqc_sample_check_app_limited(&path->path_send_ctl->sampler,
                                         path->path_send_ctl, conn->conn_send_queue))
        {
            rqc_pacing_on_app_limit(&path->path_send_ctl->ctl_pacing);
        }
    }
}

rqc_int_t
rqc_conn_send_path_challenge(rqc_connection_t *conn, rqc_path_ctx_t *path)
{
    rqc_int_t           ret = RQC_OK;
    rqc_packet_out_t   *packet_out;
    rqc_usec_t          now;
    ssize_t             sent;

    /* generate random data for path challenge, store it to validate path_response */
    ret = rqc_generate_path_challenge_data(conn, path);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_generate_path_challenge_data error|%d|", ret);
        return ret;
    }

    /* write path challenge frame & send immediately */

    packet_out = rqc_write_new_packet(conn, RQC_PTYPE_SHORT_HEADER);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    ret = rqc_gen_path_challenge_frame(packet_out, path->path_challenge_data);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_path_challenge_frame error|%d|", ret);
        goto end;
    }
    packet_out->po_used_size += ret;

    rqc_encode_packet_with_pn(conn, path, packet_out);

    /* record the send time of packet */
    now = rqc_monotonic_timestamp();
    packet_out->po_sent_time = now;

    sent = conn->transport_cbs.write_socket(packet_out->po_buf, packet_out->po_used_size,
                                                (struct sockaddr *)path->rebinding_addr,
                                                path->rebinding_addrlen,
                                                rqc_conn_get_user_data(conn));

    if (sent != packet_out->po_used_size) {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|write_socket error|conn:%p|pkt_num:%ui|size:%ud|sent:%z|pkt_type:%s|frame:%s|now:%ui|",
                conn, packet_out->po_pkt.pkt_num, packet_out->po_used_size, sent,
                rqc_pkt_type_2_str(packet_out->po_pkt.pkt_type),
                rqc_frame_type_2_str(conn->engine, packet_out->po_frame_types), now);
        ret = -RQC_ESOCKET;
        goto end;

    } else {
        rqc_log(conn->log, RQC_LOG_INFO,
                "|<==|conn:%p|pkt_num:%ui|size:%ud|sent:%z|pkt_type:%s|frame:%s|inflight:%ud|now:%ui|",
                conn, packet_out->po_pkt.pkt_num, packet_out->po_used_size, sent,
                rqc_pkt_type_2_str(packet_out->po_pkt.pkt_type),
                rqc_frame_type_2_str(conn->engine, packet_out->po_frame_types), path->path_send_ctl->ctl_bytes_in_flight, now);
    }

end:
    rqc_send_queue_remove_send(&packet_out->po_list);
    rqc_send_queue_insert_free(packet_out, &conn->conn_send_queue->sndq_free_packets, conn->conn_send_queue);
    return ret;
}

void
rqc_conn_buff_1rtt_packet(rqc_connection_t *conn, rqc_packet_out_t *po)
{
    rqc_send_queue_remove_send(&po->po_list);
    rqc_send_queue_insert_buff(&po->po_list, &conn->conn_send_queue->sndq_buff_1rtt_packets);
    if (!rqc_conn_is_dcid_done(conn)) {
        po->po_flag |= RQC_POF_DCID_NOT_DONE;
    }
}

void
rqc_conn_buff_1rtt_packets(rqc_connection_t *conn)
{
    rqc_packet_out_t *packet_out;
    rqc_list_head_t *pos, *next;
    rqc_list_for_each_safe(pos, next, &conn->conn_send_queue->sndq_send_packets) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
        if (packet_out->po_pkt.pkt_type == RQC_PTYPE_SHORT_HEADER) {
            rqc_send_queue_remove_send(&packet_out->po_list);
            rqc_send_queue_insert_buff(&packet_out->po_list, &conn->conn_send_queue->sndq_buff_1rtt_packets);
            if (!rqc_conn_is_dcid_done(conn)) {
                packet_out->po_flag |= RQC_POF_DCID_NOT_DONE;
            }
        }
    }
}

void
rqc_conn_write_buffed_1rtt_packets(rqc_connection_t *conn)
{
    if (rqc_conn_is_established(conn)) {
        rqc_send_queue_t *send_queue = conn->conn_send_queue;
        rqc_list_head_t *pos, *next;
        rqc_packet_out_t *packet_out;
        rqc_list_for_each_safe(pos, next, &send_queue->sndq_buff_1rtt_packets) {
            packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
            rqc_send_queue_remove_buff(pos, send_queue);
            rqc_send_queue_insert_send(packet_out, &send_queue->sndq_send_packets, send_queue);
            if (packet_out->po_flag & RQC_POF_DCID_NOT_DONE) {
                rqc_short_packet_update_dcid(packet_out, conn->dcid_set.current_dcid);
            }
        }
    }
}

static rqc_packet_out_t *
rqc_conn_gen_ping(rqc_connection_t *conn)
{
    /* get pkt, which is inserted into sent list */
    rqc_packet_out_t *packet_out = rqc_write_new_packet(conn, RQC_PTYPE_NUM);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return NULL;
    }

    /* write PING to pkt */
    rqc_int_t ret = rqc_gen_ping_frame(packet_out);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_ping_frame error|");
        rqc_maybe_recycle_packet_out(packet_out, conn);
        return NULL;
    }

    packet_out->po_user_data = NULL;
    packet_out->po_used_size += ret;

    return packet_out;
}

static rqc_int_t
rqc_path_send_ping_to_probe(rqc_path_ctx_t *path)
{
    rqc_connection_t *conn = path->parent_conn;

    rqc_packet_out_t *packet_out = rqc_conn_gen_ping(conn);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    /* put PING into probe list, which is not limited by amplification or congestion-control */
    rqc_send_queue_remove_send(&packet_out->po_list);
    rqc_send_queue_insert_probe(&packet_out->po_list, &conn->conn_send_queue->sndq_pto_probe_packets);

    return RQC_OK;
}

int
rqc_conn_send_probe_pkt(rqc_connection_t *c, rqc_path_ctx_t *path,
    rqc_packet_out_t *packet_out)
{
    if (packet_out->po_flag & RQC_POF_IN_FLIGHT) {
        c->detected_loss_cnt++;
    }

    rqc_send_ctl_decrease_inflight(c, packet_out);
    rqc_send_queue_copy_to_probe(packet_out, c->conn_send_queue, path);

    packet_out->po_flag |= RQC_POF_TLP;

    return 0;
}

void
rqc_path_send_one_or_two_ack_elicit_pkts(rqc_path_ctx_t *path)
{
    rqc_connection_t       *c;
    rqc_packet_out_t       *packet_out;
    rqc_packet_out_t       *packet_out_last_sent;   /* for dup pto pkt */
    rqc_list_head_t        *pos, *next;
    rqc_list_head_t        *sndq;
    rqc_int_t               probe_num;

    c       = path->parent_conn;
    sndq    = &c->conn_send_queue->sndq_unacked_packets;

    /* on PTO rquic will try to send 2 ack-eliciting pkts at most. */
    probe_num        = RQC_CONN_PTO_PKT_CNT_MAX;

    packet_out_last_sent  = NULL;

    rqc_list_for_each_safe(pos, next, sndq) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);

        if (rqc_send_ctl_indirectly_ack_or_drop_po(c, packet_out)) {
            continue;
        }

        if (RQC_IS_ACK_ELICITING(packet_out->po_frame_types)
            && (RQC_NEED_REPAIR(packet_out->po_frame_types)
                || (packet_out->po_flag & RQC_POF_NOTIFY)))
        {
            rqc_conn_send_probe_pkt(c, path, packet_out);
            packet_out_last_sent = packet_out;

            if (--probe_num == 0) {
                break;
            }
        }
    }

    if (probe_num > 0) {
        if (packet_out_last_sent) {
            /* at least one pkt was sent, and there is still budget for send
               more ack-eliciting pkts, try to send the pkt again */
            while (probe_num > 0) {
                rqc_conn_send_probe_pkt(c, path, packet_out_last_sent);
                probe_num--;
            }

        } else {
            /* if no packet was sent, try to send PING frame */
            while (probe_num > 0) {
                rqc_path_send_ping_to_probe(path);
                probe_num--;
            }
        }
    }
}

rqc_int_t
rqc_conn_send_ping_internal(rqc_connection_t *conn, void *ping_user_data, rqc_bool_t notify)
{
    rqc_int_t ret;
    rqc_bool_t has_ping;
    rqc_ping_record_t *pr;

    ret = RQC_OK;

    if (conn->conn_state >= RQC_CONN_STATE_CLOSING) {
        return ret;
    }

    pr = rqc_conn_create_ping_record(conn);

    if (pr == NULL) {
        return -RQC_EMALLOC;
    }

    has_ping = RQC_FALSE;

    ret = rqc_write_ping_to_packet(conn, ping_user_data, notify, pr);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|write ping error|");
    } else {
        has_ping = RQC_TRUE;
    }

    if (!has_ping) {
        rqc_conn_destroy_ping_record(pr);
        return ret;
    }

    return RQC_OK;
}

/* used by upper level, shall never be invoked in rquic */
rqc_int_t
rqc_conn_send_ping(rqc_engine_t *engine, const rqc_cid_t *cid, void *ping_user_data)
{
    rqc_connection_t *conn;
    rqc_int_t ret;
    conn = rqc_engine_conns_hash_find(engine, cid, 's');
    if (!conn) {
        rqc_log(engine->log, RQC_LOG_ERROR, "|can not find connection|cid:%s",
                rqc_scid_str(engine, cid));
        return -RQC_ECONN_NFOUND;
    }

    ret = rqc_conn_send_ping_internal(conn, ping_user_data, RQC_TRUE);
    if (ret) {
        return ret;
    }

    rqc_engine_remove_wakeup_queue(engine, conn);
    rqc_engine_add_active_queue(engine, conn);

    rqc_engine_conn_logic(engine, conn);
    return RQC_OK;
}

rqc_ping_record_t*
rqc_conn_create_ping_record(rqc_connection_t *conn)
{
    rqc_ping_record_t *pr = rqc_calloc(1, sizeof(rqc_ping_record_t));
    rqc_init_list_head(&pr->list);
    rqc_list_add_tail(&pr->list, &conn->ping_notification_list);
    return pr;
}

void
rqc_conn_destroy_ping_record(rqc_ping_record_t *pr)
{
    rqc_list_del_init(&pr->list);
    rqc_free(pr);
}

static inline void
rqc_conn_get_local_transport_params(rqc_connection_t *conn, rqc_transport_params_t *params)
{
    rqc_trans_settings_t *settings = &conn->local_settings;
    params->max_idle_timeout = settings->max_idle_timeout;
    params->max_udp_payload_size = settings->max_udp_payload_size;
    params->initial_max_data = settings->max_data;
    params->initial_max_stream_data_bidi_local = settings->max_stream_data_bidi_local;
    params->initial_max_stream_data_bidi_remote = settings->max_stream_data_bidi_remote;
    params->initial_max_stream_data_uni = settings->max_stream_data_uni;
    params->initial_max_streams_bidi = settings->max_streams_bidi;
    params->initial_max_streams_uni = settings->max_streams_uni;
    params->ack_delay_exponent = settings->ack_delay_exponent;
    params->max_ack_delay = settings->max_ack_delay;
}

static rqc_int_t
rqc_conn_encode_local_tp(rqc_connection_t *conn, uint8_t *dst, size_t dst_cap, size_t *dst_len)
{
    rqc_int_t ret;
    rqc_transport_params_t params;
    memset(&params, 0, sizeof(rqc_transport_params_t));

    rqc_conn_get_local_transport_params(conn, &params);

    ret = rqc_encode_transport_params(&params, dst, dst_cap, dst_len);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|encode tls trans param error|ret:%d", ret);
        return ret;
    }

    return RQC_OK;
}

rqc_int_t
rqc_conn_send_handshake(rqc_connection_t *conn)
{
    rqc_int_t ret;
    unsigned char tp[RQC_MAX_TRANSPORT_PARAM_BUF_LEN];
    size_t tp_len;

    ret = rqc_conn_encode_local_tp(conn, tp, sizeof(tp), &tp_len);
    if (ret != RQC_OK) {
        return ret;
    }

    ret = rqc_write_handshake_frame_to_packet(conn, conn->alpn, conn->alpn_len, tp, tp_len,
                                              conn->self_proto_ext_buf, conn->self_proto_ext.len);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_conn_send_handshake error|%d|", ret);
        return ret;
    }

    if (conn->conn_type == RQC_CONN_TYPE_CLIENT) {
        conn->conn_state = RQC_CONN_STATE_CLIENT_HANDSHAKE;
        rqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);
    }

    return RQC_OK;
}

static inline rqc_int_t
rqc_conn_check_transport_params(rqc_connection_t *conn, const rqc_transport_params_t *params)
{
    /* parameters MUST NOT be larger than 2^60 */
    if (params->initial_max_stream_data_bidi_local > RQC_MAX_STREAMS
        || params->initial_max_stream_data_bidi_remote > RQC_MAX_STREAMS
        || params->initial_max_stream_data_uni > RQC_MAX_STREAMS
        || params->initial_max_streams_bidi > RQC_MAX_STREAMS
        || params->initial_max_streams_uni > RQC_MAX_STREAMS)
    {
        return -RQC_EILLTP;
    }
    return RQC_OK;
}

static inline void
rqc_conn_set_remote_transport_params(rqc_connection_t *conn, const rqc_transport_params_t *params)
{
    rqc_trans_settings_t *settings = &conn->remote_settings;
    settings->max_idle_timeout = params->max_idle_timeout;
    settings->max_udp_payload_size = params->max_udp_payload_size;
    settings->max_data = params->initial_max_data;
    settings->max_stream_data_bidi_local = params->initial_max_stream_data_bidi_local;
    settings->max_stream_data_bidi_remote = params->initial_max_stream_data_bidi_remote;
    settings->max_stream_data_uni = params->initial_max_stream_data_uni;
    settings->max_streams_bidi = params->initial_max_streams_bidi;
    settings->max_streams_uni = params->initial_max_streams_uni;
    settings->ack_delay_exponent = params->ack_delay_exponent;
    settings->max_ack_delay = params->max_ack_delay;
    if (conn->conn_type == RQC_CONN_TYPE_SERVER
        && settings->max_udp_payload_size >= RQC_PACKET_OUT_SIZE) {
        conn->pkt_out_size = rqc_min(conn->pkt_out_size, settings->max_udp_payload_size - RQC_PACKET_OUT_EXT_SPACE);
    }
}

static void
rqc_conn_update_flow_ctl_settings(rqc_connection_t *conn)
{
    rqc_conn_flow_ctl_t *flow_ctl = &conn->conn_flow_ctl;
    rqc_trans_settings_t *remote_settings = &conn->remote_settings;

    flow_ctl->fc_max_data_can_send = remote_settings->max_data;
    flow_ctl->fc_max_streams_bidi_can_send = remote_settings->max_streams_bidi;
    flow_ctl->fc_max_streams_uni_can_send = remote_settings->max_streams_uni;
}

rqc_int_t
rqc_conn_process_handshake(rqc_connection_t *conn,
    const unsigned char *alpn, size_t alpn_len,
    const unsigned char *tp, size_t tp_len,
    const unsigned char *proto_ext, size_t proto_ext_len)
{
    rqc_int_t ret;

    // ignore
    if (conn->conn_type == RQC_CONN_TYPE_CLIENT) {
        if (conn->conn_state != RQC_CONN_STATE_CLIENT_HANDSHAKE ||
            rqc_conn_is_handshake_recvd(conn)) {
            rqc_log(conn->log, RQC_LOG_INFO, "|rqc_conn_process_handshake ignore dups|");
            return RQC_OK;
        }
    } else if (conn->conn_type == RQC_CONN_TYPE_SERVER) {
        if (conn->conn_state != RQC_CONN_STATE_SERVER_INIT) {
            rqc_log(conn->log, RQC_LOG_INFO, "|rqc_conn_process_handshake ignore dups|");
            return RQC_OK;
        }
    }

    if (proto_ext_len > RQC_MAX_PROTO_EXT_LEN) {
        RQC_CONN_ERR(conn, TRA_PROTOCOL_VIOLATION);
        return -RQC_EILLFRAME;
    }
    if (proto_ext_len > 0) {
        rqc_memcpy(conn->peer_proto_ext_buf, proto_ext, proto_ext_len);
    }
    conn->peer_proto_ext.len = proto_ext_len;

    rqc_transport_params_t params;
    memset(&params, 0, sizeof(rqc_transport_params_t));
    ret = rqc_decode_transport_params(&params, tp, tp_len);
    if (ret != RQC_OK) {
        RQC_CONN_ERR(conn, TRA_TRANSPORT_PARAMETER_ERROR);
        return ret;
    }
    ret = rqc_conn_check_transport_params(conn, &params);
    if (ret != RQC_OK) {
        RQC_CONN_ERR(conn, TRA_TRANSPORT_PARAMETER_ERROR);
        return ret;
    }
    rqc_conn_set_remote_transport_params(conn, &params);

    rqc_list_head_t *pos, *next;
    rqc_stream_t    *stream;

    rqc_conn_update_flow_ctl_settings(conn);
    rqc_list_for_each_safe(pos, next, &conn->conn_all_streams) {
        stream = rqc_list_entry(pos, rqc_stream_t, all_stream_list);
        rqc_stream_update_flow_ctl(stream);
    }

    if (conn->conn_type == RQC_CONN_TYPE_CLIENT) {
        conn->conn_flag |= RQC_CONN_FLAG_HANDSHAKE_RECVD;
        conn->conn_flag |= RQC_CONN_FLAG_HANDSHAKE_DONE;

        conn->conn_state = RQC_CONN_STATE_ESTABED;
        rqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);

        if (conn->app_proto_cbs.conn_cbs.conn_handshake_finished) {
            conn->app_proto_cbs.conn_cbs.conn_handshake_finished(conn, conn->user_data, conn->proto_data);
        }
    } else if (conn->conn_type == RQC_CONN_TYPE_SERVER) {
        ret = rqc_conn_server_on_alpn(conn, alpn, alpn_len);
        if (ret != RQC_OK) {
            return ret;
        }

        ret = rqc_conn_send_handshake(conn);
        if (ret != RQC_OK) {
            return ret;
        }

        conn->conn_flag |= RQC_CONN_FLAG_HANDSHAKE_RECVD;
        conn->conn_state = RQC_CONN_STATE_SERVER_HANDSHAKE;
        rqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);
    }

    return RQC_OK;
}

void
rqc_conn_on_handshake_acked(rqc_connection_t *conn)
{
    if (conn->conn_type == RQC_CONN_TYPE_SERVER) {
        conn->conn_flag |= RQC_CONN_FLAG_HANDSHAKE_DONE;

        if (conn->conn_state == RQC_CONN_STATE_SERVER_HANDSHAKE) {
            conn->conn_state = RQC_CONN_STATE_ESTABED;
            rqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);

            if (conn->app_proto_cbs.conn_cbs.conn_handshake_finished) {
                conn->app_proto_cbs.conn_cbs.conn_handshake_finished(conn, conn->user_data, conn->proto_data);
            }
        }
    }
}
