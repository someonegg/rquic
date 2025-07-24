/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include <xquic/xquic.h>
#include <errno.h>
#include "src/common/xqc_algorithm.h"
#include "src/common/xqc_common.h"
#include "src/common/xqc_malloc.h"
#include "src/common/xqc_str_hash.h"
#include "src/common/xqc_hash.h"
#include "src/common/xqc_priority_q.h"
#include "src/common/xqc_memory_pool.h"
#include "src/common/xqc_id_hash.h"
#include "src/transport/xqc_defs.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/transport/xqc_send_queue.h"
#include "src/transport/xqc_engine.h"
#include "src/transport/xqc_cid.h"
#include "src/transport/xqc_stream.h"
#include "src/transport/xqc_frame_parser.h"
#include "src/transport/xqc_packet_parser.h"
#include "src/transport/xqc_utils.h"
#include "src/transport/xqc_multipath.h"
#include "src/transport/xqc_packet.h"
#include <inttypes.h>

xqc_conn_settings_t internal_default_conn_settings = {
    .pacing_on                  = 0,
    .ping_on                    = 0,
    .so_sndbuf                  = 0,
    .sndq_packets_used_max      = 0,
    .linger                     = {.linger_on = 0, .linger_timeout = 0},
    .proto_version              = XQC_VERSION_V1,
    .init_idle_time_out         = XQC_CONN_INITIAL_IDLE_TIMEOUT,
    .idle_time_out              = XQC_CONN_DEFAULT_IDLE_TIMEOUT,
    .spurious_loss_detect_on    = 0,
    .max_pkt_out_size           = XQC_PACKET_OUT_SIZE,
    .max_ack_delay              = XQC_DEFAULT_MAX_ACK_DELAY,
    .ack_frequency              = 2,
    .loss_detection_pkt_thresh  = XQC_kPacketThreshold,
    .pto_backoff_factor         = 2.0,

    .recv_rate_bytes_per_sec    = 0,
    .enable_stream_rate_limit   = 0,

#ifdef XQC_PROTECT_POOL_MEM
    .protect_pool_mem           = 0,
#endif

    .disable_send_mmsg          = 0,
    .control_pto_value          = 0,
    .max_udp_payload_size       = XQC_CONN_MAX_UDP_PAYLOAD_SIZE,
};

xqc_conn_settings_t
xqc_conn_get_conn_settings_template(xqc_conn_settings_type_t settings_type)
{
    xqc_conn_settings_t conn_settings = internal_default_conn_settings;

    if (settings_type == XQC_CONN_SETTINGS_LOW_DELAY) {
        conn_settings.ack_frequency = 1;
        conn_settings.loss_detection_pkt_thresh = 2;
        conn_settings.pto_backoff_factor = 1.5;
    }

    return conn_settings;
}

void
xqc_server_set_conn_settings(xqc_engine_t *engine, const xqc_conn_settings_t *settings)
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
#ifdef XQC_PROTECT_POOL_MEM
    engine->default_conn_settings.protect_pool_mem = settings->protect_pool_mem;
#endif
    engine->default_conn_settings.adaptive_ack_frequency = settings->adaptive_ack_frequency;

    if (settings->max_udp_payload_size != 0) {
        engine->default_conn_settings.max_udp_payload_size = settings->max_udp_payload_size;
    }

    if (engine->default_conn_settings.init_recv_window) {
        engine->default_conn_settings.init_recv_window = xqc_max(engine->default_conn_settings.init_recv_window, XQC_QUIC_MAX_MSS);
    }

    if (settings->max_ack_delay) {
        engine->default_conn_settings.max_ack_delay = xqc_min(settings->max_ack_delay, XQC_DEFAULT_MAX_ACK_DELAY);
    }

    if (settings->init_idle_time_out > 0) {
        engine->default_conn_settings.init_idle_time_out = settings->init_idle_time_out;
    }

    if (settings->idle_time_out > 0) {
        engine->default_conn_settings.idle_time_out = settings->idle_time_out;
    }

    if (xqc_check_proto_version_valid(settings->proto_version)) {
        engine->default_conn_settings.proto_version = settings->proto_version;
    }

    if (settings->max_pkt_out_size != 0) {
        engine->default_conn_settings.max_pkt_out_size = settings->max_pkt_out_size;
    }

    if (engine->default_conn_settings.max_pkt_out_size > XQC_MAX_PACKET_OUT_SIZE) {
        engine->default_conn_settings.max_pkt_out_size = XQC_MAX_PACKET_OUT_SIZE;
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

xqc_conn_type_t
xqc_conn_get_type(xqc_connection_t *conn)
{
    return conn->conn_type;
}

xqc_int_t
xqc_conn_get_errno(xqc_connection_t *conn)
{
    return conn->conn_err;
}

xqc_usec_t
xqc_conn_get_lastest_rtt(xqc_engine_t *engine, const xqc_cid_t *cid)
{
    xqc_connection_t *conn;
    xqc_path_ctx_t *path;

    conn = xqc_engine_conns_hash_find(engine, cid, 's');
    if (!conn) {
        xqc_log(engine->log, XQC_LOG_ERROR, "|can not find connection|cid:%s",
                xqc_scid_str(engine, cid));
        return 0;
    }

    if (conn->the_path) {
        return conn->the_path->path_send_ctl->ctl_latest_rtt;
    }

    return 0;
}

void
xqc_conn_set_transport_user_data(xqc_connection_t *conn, void *user_data)
{
    conn->user_data = user_data;
}

void
xqc_conn_set_alp_user_data(xqc_connection_t *conn, void *user_data)
{
    conn->proto_data = user_data;
}

xqc_int_t
xqc_conn_get_peer_addr(xqc_connection_t *conn, struct sockaddr *addr, socklen_t addr_cap,
    socklen_t *peer_addr_len)
{
    if (conn->peer_addrlen > addr_cap) {
        return -XQC_ENOBUF;
    }

    *peer_addr_len = conn->peer_addrlen;
    xqc_memcpy(addr, conn->peer_addr, conn->peer_addrlen);
    return XQC_OK;
}

xqc_int_t
xqc_conn_get_local_addr(xqc_connection_t *conn, struct sockaddr *addr, socklen_t addr_cap,
    socklen_t *local_addr_len)
{
    if (conn->local_addrlen > addr_cap) {
        return -XQC_ENOBUF;
    }

    *local_addr_len = conn->local_addrlen;
    xqc_memcpy(addr, conn->local_addr, conn->local_addrlen);
    return XQC_OK;
}

void
xqc_conn_set_pkt_filter_callback(xqc_connection_t *conn,
    xqc_conn_pkt_filter_callback_pt pkt_filter_cb,
    void *pkt_filter_cb_user_data)
{
    conn->pkt_filter_cb = pkt_filter_cb;
    conn->pkt_filter_cb_user_data = pkt_filter_cb_user_data;
}

void
xqc_conn_unset_pkt_filter_callback(xqc_connection_t *conn)
{
    if (conn) {
        conn->pkt_filter_cb = NULL;
        conn->pkt_filter_cb_user_data = NULL;
        xqc_log(conn->log, XQC_LOG_INFO, "|conn unset pkt filter callback, will"
                "use write_socket again");
    }
}

static void
xqc_conn_get_stats_internal(xqc_connection_t *conn, xqc_conn_stats_t *conn_stats)
{
    /* 1. 与路径无关的连接级别埋点 */
    xqc_memset(conn_stats->alpn, 0, XQC_MAX_ALPN_BUF_LEN);
    if (conn->alpn) {
        xqc_memcpy(conn_stats->alpn, conn->alpn, xqc_min(conn->alpn_len, XQC_MAX_ALPN_BUF_LEN-1));
    } else {
        conn_stats->alpn[0] = '-';
        conn_stats->alpn[1] = '1';
    }

    conn_stats->conn_err = (int)conn->conn_err;
    conn_stats->spurious_loss_detect_on = conn->conn_settings.spurious_loss_detect_on;
    conn_stats->max_acked_mtu = conn->max_acked_po_size;

    xqc_path_ctx_t *path = conn->the_path;

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

        xqc_recv_record_print(conn, &(path->path_pn_ctl->ctl_recv_record),
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
    xqc_conn_path_metrics_print(conn, conn_stats);
}

xqc_conn_stats_t
xqc_conn_get_stats(xqc_engine_t *engine, const xqc_cid_t *cid)
{
    xqc_connection_t *conn;
    xqc_conn_stats_t conn_stats;
    xqc_memzero(&conn_stats, sizeof(conn_stats));
    conn_stats.path_info.path_id = XQC_MAX_UINT64_VALUE;

    conn = xqc_engine_conns_hash_find(engine, cid, 's');
    if (!conn) {
        xqc_log(engine->log, XQC_LOG_ERROR, "|can not find connection|cid:%s",
                xqc_scid_str(engine, cid));
        return conn_stats;
    }

    xqc_conn_get_stats_internal(conn, &conn_stats);

    return conn_stats;
}

xqc_conn_qos_stats_t
xqc_conn_get_qos_stats(xqc_engine_t *engine, const xqc_cid_t *cid)
{
    xqc_connection_t *conn;
    xqc_conn_qos_stats_t qos_stats;
    xqc_memzero(&qos_stats, sizeof(qos_stats));

    conn = xqc_engine_conns_hash_find(engine, cid, 's');
    if (!conn) {
        xqc_log(engine->log, XQC_LOG_ERROR, "|can not find connection|cid:%s",
                xqc_scid_str(engine, cid));
        return qos_stats;
    }

    xqc_path_ctx_t *path = conn->the_path;
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
xqc_conn_continue_send_by_conn(xqc_connection_t *conn)
{
    if (!conn) {
        return;
    }

    xqc_engine_remove_wakeup_queue(conn->engine, conn);
    xqc_engine_add_active_queue(conn->engine, conn);

    xqc_engine_conn_logic(conn->engine, conn);
}

int
xqc_conn_continue_send(xqc_engine_t *engine, const xqc_cid_t *cid)
{
    xqc_connection_t *conn = xqc_engine_conns_hash_find(engine, cid, 's');
    if (!conn) {
        xqc_log(engine->log, XQC_LOG_ERROR, "|can not find connection|cid:%s",
                xqc_scid_str(engine, cid));
        return -XQC_ECONN_NFOUND;
    }

    xqc_conn_continue_send_by_conn(conn);
    return XQC_OK;
}

static const char * const xqc_conn_state_to_str[XQC_CONN_STATE_N] = {
    [XQC_CONN_STATE_SERVER_INIT]            = "S_INIT",
    [XQC_CONN_STATE_SERVER_INITIAL_RECVD]   = "S_INITIAL_RECVD",
    [XQC_CONN_STATE_SERVER_INITIAL_SENT]    = "S_INITIAL_SENT",
    [XQC_CONN_STATE_CLIENT_INIT]            = "C_INIT",
    [XQC_CONN_STATE_CLIENT_INITIAL_RECVD]   = "C_INITIAL_RECVD",
    [XQC_CONN_STATE_CLIENT_INITIAL_SENT]    = "C_INITIAL_SENT",
    [XQC_CONN_STATE_ESTABED]                = "ESTABED",
    [XQC_CONN_STATE_CLOSING]                = "CLOSING",
    [XQC_CONN_STATE_DRAINING]               = "DRAINING",
    [XQC_CONN_STATE_CLOSED]                 = "CLOSED",
};

const char *
xqc_conn_state_2_str(xqc_conn_state_t state)
{
    return xqc_conn_state_to_str[state];
}

static const char * const xqc_conn_flag_to_str[XQC_CONN_FLAG_SHIFT_NUM] = {
    [XQC_CONN_FLAG_WAIT_WAKEUP_SHIFT]           = "WAIT_WAKEUP",
    [XQC_CONN_FLAG_HANDSHAKE_RECVD_SHIFT]       = "HSK_RECVD",
    [XQC_CONN_FLAG_HANDSHAKE_DONE_SHIFT]        = "HSK_DONE",
    [XQC_CONN_FLAG_TICKING_SHIFT]               = "TICKING",
    [XQC_CONN_FLAG_ACK_HAS_GAP_SHIFT]           = "HAS_GAP",
    [XQC_CONN_FLAG_TIME_OUT_SHIFT]              = "TIME_OUT",
    [XQC_CONN_FLAG_ERROR_SHIFT]                 = "ERROR",
    [XQC_CONN_FLAG_DATA_BLOCKED_SHIFT]          = "DATA_BLOCKED",
    [XQC_CONN_FLAG_DCID_DONE_SHIFT]             = "DCID_DONE",
    [XQC_CONN_FLAG_UPPER_CONN_EXIST_SHIFT]      = "UPPER_CONN_EXIST",
    [XQC_CONN_FLAG_NEED_RUN_SHIFT]              = "NEED_RUN",
    [XQC_CONN_FLAG_PING_SHIFT]                  = "PING",
    [XQC_CONN_FLAG_LINGER_CLOSING_SHIFT]        = "LINGER_CLOSING",
    [XQC_CONN_FLAG_CONN_CLOSING_NOTIFY_SHIFT]   = "CLOSING_NOTIFY",
    [XQC_CONN_FLAG_CONN_CLOSING_NOTIFIED_SHIFT] = "CLOSING_NOTIFIED",
    [XQC_CONN_FLAG_VERSION_NEGOTIATION_SHIFT]   = "VERSION_NEGOTIATION",
};

const char *
xqc_conn_flag_2_str(xqc_connection_t *conn, xqc_conn_flag_t conn_flag)
{
    xqc_engine_t *engine = conn->engine;
    engine->conn_flag_str_buf[0] = '\0';
    size_t pos = 0;
    int wsize;
    for (int i = 0; i < XQC_CONN_FLAG_SHIFT_NUM; i++) {
        if (conn_flag & 1ULL << i) {
            wsize = snprintf(engine->conn_flag_str_buf + pos, sizeof(engine->conn_flag_str_buf) - pos, "%s ",
                             xqc_conn_flag_to_str[i]);
            if (wsize < 0 || wsize >= sizeof(engine->conn_flag_str_buf) - pos) {
                break;
            }
            pos += wsize;
        }
    }

    return engine->conn_flag_str_buf;
}

char *
xqc_local_addr_str(xqc_engine_t *engine, const struct sockaddr *local_addr, socklen_t local_addrlen)
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
xqc_peer_addr_str(xqc_engine_t *engine, const struct sockaddr *peer_addr, socklen_t peer_addrlen)
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
xqc_conn_addr_str(xqc_connection_t *conn)
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
                                      xqc_local_addr_str(conn->engine, (struct sockaddr*)sa_local, conn->local_addrlen),
                                      ntohs(sa_local->sin_port), xqc_scid_str(conn->engine, &conn->scid_set.user_scid),
                                      xqc_peer_addr_str(conn->engine, (struct sockaddr*)sa_peer, conn->peer_addrlen),
                                      ntohs(sa_peer->sin_port), xqc_dcid_str(conn->engine, &conn->dcid_set.current_dcid));
    }

    return conn->addr_str;
}

char *
xqc_path_addr_str(xqc_path_ctx_t *path)
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
                                      xqc_local_addr_str(path->parent_conn->engine, (struct sockaddr*)sa_local, path->local_addrlen),
                                      ntohs(sa_local->sin_port), xqc_scid_str(path->parent_conn->engine, &path->path_scid),
                                      xqc_peer_addr_str(path->parent_conn->engine, (struct sockaddr*)sa_peer, path->peer_addrlen),
                                      ntohs(sa_peer->sin_port), xqc_dcid_str(path->parent_conn->engine, &path->path_dcid));
    }

    return path->addr_str;
}

static inline void
xqc_conn_set_default_settings(xqc_trans_settings_t *settings)
{
    memset(settings, 0, sizeof(xqc_trans_settings_t));

    /* transport parameter related attributes */
    settings->max_ack_delay              = XQC_DEFAULT_MAX_ACK_DELAY;
    settings->ack_delay_exponent         = XQC_DEFAULT_ACK_DELAY_EXPONENT;
    settings->max_udp_payload_size       = XQC_DEFAULT_MAX_UDP_PAYLOAD_SIZE;
}

static inline void
xqc_conn_init_trans_settings(xqc_connection_t *conn)
{
    /* set local and remote settings to default */
    xqc_trans_settings_t *ls = &conn->local_settings;
    xqc_trans_settings_t *rs = &conn->remote_settings;

    xqc_conn_set_default_settings(ls);
    xqc_conn_set_default_settings(rs);

    /* set local default setting values */
    ls->max_streams_bidi = 1024;
    ls->max_streams_uni = 1024;
    ls->max_stream_data_bidi_remote = XQC_MAX_RECV_WINDOW;
    ls->max_stream_data_uni = XQC_MAX_RECV_WINDOW;

    if (conn->conn_settings.enable_stream_rate_limit) {
        ls->max_stream_data_bidi_local = conn->conn_settings.init_recv_window;

    } else {
        ls->max_stream_data_bidi_local = XQC_MAX_RECV_WINDOW;
    }

    if (conn->conn_settings.recv_rate_bytes_per_sec) {
        ls->max_data = conn->conn_settings.recv_rate_bytes_per_sec * XQC_FC_INIT_RTT / 1000000;
        ls->max_data = xqc_max(XQC_MIN_RECV_WINDOW, ls->max_data);
        ls->max_data = xqc_min(XQC_MAX_RECV_WINDOW, ls->max_data);
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
xqc_conn_init_flow_ctl(xqc_connection_t *conn)
{
    xqc_conn_flow_ctl_t *flow_ctl = &conn->conn_flow_ctl;
    xqc_trans_settings_t * settings = & conn->local_settings;

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
xqc_conn_init_timer_manager(xqc_connection_t *conn)
{
    xqc_timer_manager_t *timer_manager = &conn->conn_timer_manager;
    xqc_usec_t now = xqc_monotonic_timestamp();

    xqc_timer_init(timer_manager, conn->log, conn);

    xqc_timer_set(timer_manager, XQC_TIMER_CONN_IDLE, now, xqc_conn_get_idle_timeout(conn) * 1000);

    if (conn->conn_settings.ping_on
        && conn->conn_type == XQC_CONN_TYPE_CLIENT)
    {
        xqc_timer_set(timer_manager, XQC_TIMER_PING, now, XQC_PING_TIMEOUT * 1000);
    }
}

xqc_connection_t *
xqc_conn_create(xqc_engine_t *engine, xqc_cid_t *dcid, xqc_cid_t *scid,
    const xqc_conn_settings_t *settings, void *user_data, xqc_conn_type_t type)
{
    xqc_connection_t *xc = NULL;
#ifdef XQC_PROTECT_POOL_MEM
    xqc_memory_pool_t *pool = xqc_create_pool(engine->config->conn_pool_size, settings->protect_pool_mem);
#else
    xqc_memory_pool_t *pool = xqc_create_pool(engine->config->conn_pool_size);
#endif
    if (pool == NULL) {
        return NULL;
    }

#ifdef XQC_PROTECT_POOL_MEM
    xqc_log(engine->log, XQC_LOG_INFO, "|mempool|protect:%d|page_sz:%z|",
            pool->protect_block, pool->page_size);
#endif

    xc = xqc_pcalloc(pool, sizeof(xqc_connection_t));
    if (xc == NULL) {
        goto fail;
    }

    xc->conn_settings = *settings;

    if (xc->conn_settings.max_udp_payload_size == 0) {
        xc->conn_settings.max_udp_payload_size = engine->default_conn_settings.max_udp_payload_size;
    }

    if (xc->conn_settings.initial_rtt == 0) {
        xc->conn_settings.initial_rtt = XQC_kInitialRtt_us;
    }

    if (xc->conn_settings.max_ack_delay == 0) {
        xc->conn_settings.max_ack_delay = XQC_DEFAULT_MAX_ACK_DELAY;
    }
    xc->conn_settings.max_ack_delay = xqc_min(xc->conn_settings.max_ack_delay, XQC_DEFAULT_MAX_ACK_DELAY);

    if (xc->conn_settings.init_recv_window) {
        xc->conn_settings.init_recv_window = xqc_max(xc->conn_settings.init_recv_window, XQC_QUIC_MAX_MSS);

    } else {
        xc->conn_settings.init_recv_window = XQC_MIN_RECV_WINDOW;
    }

    if (xc->conn_settings.max_pkt_out_size == 0) {
        xc->conn_settings.max_pkt_out_size = engine->default_conn_settings.max_pkt_out_size;
    }

    if (xc->conn_settings.max_pkt_out_size > XQC_MAX_PACKET_OUT_SIZE) {
        xc->conn_settings.max_pkt_out_size = XQC_MAX_PACKET_OUT_SIZE;
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

    xc->version = (type == XQC_CONN_TYPE_CLIENT) ? settings->proto_version : XQC_IDRAFT_INIT_VER;

    if (type == XQC_CONN_TYPE_CLIENT
        && !xqc_check_proto_version_valid(settings->proto_version))
    {
        xc->conn_settings.proto_version = XQC_VERSION_V1;
        xc->version = XQC_VERSION_V1;
    }

    /* make sure a 0-value config will not result in immediate timeout */
    if (xc->conn_settings.init_idle_time_out == 0) {
        xc->conn_settings.init_idle_time_out = XQC_CONN_INITIAL_IDLE_TIMEOUT;
    }

    if (xc->conn_settings.idle_time_out == 0) {
        xc->conn_settings.idle_time_out = XQC_CONN_DEFAULT_IDLE_TIMEOUT;
    }

    xqc_conn_init_trans_settings(xc);
    xqc_conn_init_flow_ctl(xc);

    xc->conn_pool = pool;

    xqc_init_cid_set(&xc->dcid_set);
    xqc_init_cid_set(&xc->scid_set);

    if (xqc_cid_set_add_path(&xc->dcid_set, XQC_INITIAL_PATH_ID) != XQC_OK) {
        goto fail;
    }

    if (xqc_cid_set_add_path(&xc->scid_set, XQC_INITIAL_PATH_ID) != XQC_OK) {
        goto fail;
    }

    xqc_cid_set_update_state(&xc->dcid_set, XQC_INITIAL_PATH_ID, XQC_CID_SET_USED);
    xqc_cid_set_update_state(&xc->scid_set, XQC_INITIAL_PATH_ID, XQC_CID_SET_USED);

    if (xqc_cid_set_insert_cid(&xc->dcid_set, dcid, XQC_CID_USED,
                               XQC_CONN_ACTIVE_CID_LIMIT, XQC_INITIAL_PATH_ID))
    {
        goto fail;
    }
    xqc_cid_copy(&(xc->dcid_set.current_dcid), dcid);
    xqc_hex_dump(xc->dcid_set.current_dcid_str, dcid->cid_buf, dcid->cid_len);
    xc->dcid_set.current_dcid_str[dcid->cid_len * 2] = '\0';

    if (xqc_cid_set_insert_cid(&xc->scid_set, scid, XQC_CID_USED,
                               XQC_CONN_ACTIVE_CID_LIMIT, XQC_INITIAL_PATH_ID))
    {
        goto fail;
    }
    xqc_cid_copy(&(xc->scid_set.user_scid), scid);
    xqc_hex_dump(xc->scid_set.original_scid_str, scid->cid_buf, scid->cid_len);
    xc->scid_set.original_scid_str[scid->cid_len * 2] = '\0';
    xqc_cid_set_set_largest_seq_or_rpt(&xc->scid_set, XQC_INITIAL_PATH_ID, scid->cid_seq_num);

    xc->engine = engine;
    xc->log = xqc_log_init(engine->log->log_level, engine->log->log_event, engine->log->qlog_importance, engine->log->log_timestamp,
                           engine->log->log_level_name, engine, engine->log->log_callbacks, engine->log->user_data);
    xc->log->scid = xc->scid_set.original_scid_str;
    xc->transport_cbs = engine->transport_cbs;
    xc->user_data = user_data;
    xc->discard_vn_flag = 0;
    xc->conn_type = type;
    xc->conn_flag = 0;
    xc->conn_state = (type == XQC_CONN_TYPE_SERVER) ? XQC_CONN_STATE_SERVER_INIT : XQC_CONN_STATE_CLIENT_INIT;
    xqc_log_event(xc->log, CON_CONNECTION_STATE_UPDATED, xc);
    xc->conn_create_time = xqc_monotonic_timestamp();
    xc->handshake_complete_time = 0;
    xc->first_data_send_time = 0;
    xc->max_stream_id_bidi_remote = -1;
    xc->max_stream_id_uni_remote = -1;
    xc->pkt_out_size = xqc_min(xc->conn_settings.max_pkt_out_size, xc->conn_settings.max_udp_payload_size - XQC_PACKET_OUT_EXT_SPACE);
    xc->max_pkt_out_size = xc->conn_settings.max_pkt_out_size;

    xc->conn_send_queue = xqc_send_queue_create(xc);
    if (xc->conn_send_queue == NULL) {
        goto fail;
    }

    xqc_conn_init_timer_manager(xc);

    xqc_init_list_head(&xc->conn_write_streams);
    xqc_init_list_head(&xc->conn_read_streams);
    xqc_init_list_head(&xc->conn_closing_streams);
    xqc_init_list_head(&xc->conn_all_streams);

    /* create streams_hash */
    xc->streams_hash = xqc_pcalloc(xc->conn_pool, sizeof(xqc_id_hash_table_t));
    if (xc->streams_hash == NULL) {
        goto fail;
    }

    if (xqc_id_hash_init(xc->streams_hash,
                         xqc_default_allocator,
                         engine->config->streams_hash_bucket_size) == XQC_ERROR) {
        goto fail;
    }

    xc->passive_streams_hash = xqc_pcalloc(xc->conn_pool, sizeof(xqc_id_hash_table_t));
    if (xc->passive_streams_hash == NULL) {
        goto fail;
    }

    if (xqc_id_hash_init(xc->passive_streams_hash, xqc_default_allocator,
                         engine->config->streams_hash_bucket_size) == XQC_ERROR) {
        goto fail;
    }

    /* insert into engine's conns_hash */
    if (xqc_insert_conns_hash(engine->conns_hash, xc,
                              xc->scid_set.user_scid.cid_buf,
                              xc->scid_set.user_scid.cid_len))
    {
        goto fail;
    }

    if (xqc_conn_init_paths_list(xc) != XQC_OK) {
        goto fail;
    }

    xc->pkt_filter_cb = NULL;

    xqc_init_list_head(&xc->ping_notification_list);

    xqc_log(xc->log, XQC_LOG_INFO, "|success|scid:%s|dcid:%s|conn:%p|",
            xqc_scid_str(engine, &xc->scid_set.user_scid), xqc_dcid_str(engine, &xc->dcid_set.current_dcid), xc);
    xqc_log_event(xc->log, TRA_PARAMETERS_SET, xc, XQC_LOG_LOCAL_EVENT);

    return xc;

fail:
    if (xc != NULL) {
        xqc_conn_destroy(xc);
    }
    return NULL;
}

xqc_connection_t *
xqc_conn_server_create(xqc_engine_t *engine, const struct sockaddr *local_addr,
    socklen_t local_addrlen, const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    xqc_cid_t *dcid, xqc_cid_t *scid, xqc_conn_settings_t *settings, void *user_data)
{
    xqc_int_t           ret;
    xqc_connection_t   *conn;
    xqc_cid_t           new_scid;

    xqc_cid_copy(&new_scid, scid);

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
        if (xqc_generate_cid(engine, scid, &new_scid, 0) != XQC_OK) {
            xqc_log(engine->log, XQC_LOG_ERROR, "|fail to generate_cid|");
            return NULL;
        }
    }

    conn = xqc_conn_create(engine, dcid, &new_scid, settings, user_data, XQC_CONN_TYPE_SERVER);
    if (conn == NULL) {
        xqc_log(engine->log, XQC_LOG_ERROR, "|fail to create connection|");
        return NULL;
    }

    xqc_cid_copy(&conn->original_dcid, scid);

    if (xqc_cid_in_cid_set(&conn->scid_set, &conn->original_dcid, XQC_INITIAL_PATH_ID) == NULL) {
        /*
         * if server choose it's own cid, then if server Initial is lost,
         * and if client Initial retransmit, server might use odcid to
         * find the created conn
         */
        if (xqc_insert_conns_hash(engine->conns_hash, conn,
                                  conn->original_dcid.cid_buf,
                                  conn->original_dcid.cid_len))
        {
            goto fail;
        }

        xqc_log(conn->log, XQC_LOG_INFO, "|hash odcid conn|odcid:%s|conn:%p|",
                xqc_dcid_str(engine, &conn->original_dcid), conn);
    }

    ret = xqc_memcpy_with_cap(conn->local_addr, sizeof(conn->local_addr),
                              local_addr, local_addrlen);
    if (ret == XQC_OK) {
        conn->local_addrlen = local_addrlen;

    } else {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|local addr too large|addr_len:%d|",
                (int)local_addrlen);
        goto fail;
    }

    ret = xqc_memcpy_with_cap(conn->peer_addr, sizeof(conn->peer_addr),
                              peer_addr, peer_addrlen);
    if (ret == XQC_OK) {
        conn->peer_addrlen = peer_addrlen;

    } else {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|peer addr too large|addr_len:%d|",
                (int)peer_addrlen);
        goto fail;
    }

    ret = xqc_conn_server_init_path_addr(conn, XQC_INITIAL_PATH_ID,
                                         local_addr, local_addrlen,
                                         peer_addr, peer_addrlen);
    if (ret != XQC_OK) {
        goto fail;
    }

    xqc_log_event(conn->log, CON_CONNECTION_STARTED, conn, XQC_LOG_REMOTE_EVENT);

    if (conn->transport_cbs.server_accept) {
        if (conn->transport_cbs.server_accept(engine, conn, &conn->scid_set.user_scid, user_data) < 0) {
            xqc_log(engine->log, XQC_LOG_ERROR, "|server_accept callback return error|");
            XQC_CONN_ERR(conn, TRA_CONNECTION_REFUSED_ERROR);
            goto fail;
        }
        conn->conn_flag |= XQC_CONN_FLAG_UPPER_CONN_EXIST;
    }

    return conn;

fail:
    xqc_conn_destroy(conn);
    return NULL;
}

xqc_int_t
xqc_conn_close(xqc_engine_t *engine, const xqc_cid_t *cid)
{
    xqc_int_t ret;
    xqc_connection_t *conn;

    conn = xqc_engine_conns_hash_find(engine, cid, 's');
    if (!conn) {
        xqc_log(engine->log, XQC_LOG_ERROR, "|can not find connection|cid:%s",
                xqc_scid_str(engine, cid));
        return -XQC_ECONN_NFOUND;
    }

    xqc_log(conn->log, XQC_LOG_INFO, "|conn:%p|state:%s|flag:%s|", conn,
            xqc_conn_state_2_str(conn->conn_state), xqc_conn_flag_2_str(conn, conn->conn_flag));

    XQC_CONN_CLOSE_MSG(conn, "local close");

    if (conn->conn_state >= XQC_CONN_STATE_DRAINING) {
        return XQC_OK;
    }

    /* close connection after all data sent and acked or XQC_TIMER_LINGER_CLOSE timeout */
    xqc_usec_t now = xqc_monotonic_timestamp();
    xqc_usec_t pto = xqc_conn_get_max_pto(conn);

    if (conn->conn_settings.linger.linger_on && !xqc_send_queue_out_queue_empty(conn->conn_send_queue)) {
        conn->conn_flag |= XQC_CONN_FLAG_LINGER_CLOSING;
        xqc_usec_t linger_timeout = conn->conn_settings.linger.linger_timeout;
        xqc_timer_set(&conn->conn_timer_manager, XQC_TIMER_LINGER_CLOSE, now,
                      (linger_timeout ? linger_timeout : 3 * pto));
        goto end;
    }

    ret = xqc_conn_immediate_close(conn);
    if (ret) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_conn_immediate_close error|ret:%d|", ret);
        return ret;
    }

end:
    xqc_engine_remove_wakeup_queue(engine, conn);
    xqc_engine_add_active_queue(engine, conn);

    xqc_engine_wakeup_once(engine);

    return XQC_OK;
}

xqc_int_t
xqc_conn_close_with_error(xqc_connection_t *conn, uint64_t err_code)
{
    XQC_CONN_ERR(conn, err_code);
    return XQC_OK;
}

/* cleanup connection and wait for draining */
static void
xqc_conn_shutdown(xqc_connection_t *conn)
{
    xqc_path_ctx_t     *path;
    xqc_list_head_t    *pos, *next;
    xqc_send_ctl_t     *send_ctl;
    xqc_usec_t          now;

    now = xqc_monotonic_timestamp();
    xqc_usec_t pto = xqc_conn_get_max_pto(conn);
    if (!xqc_timer_is_set(&conn->conn_timer_manager, XQC_TIMER_CONN_DRAINING)) {
        xqc_timer_set(&conn->conn_timer_manager, XQC_TIMER_CONN_DRAINING, now, 3 * pto);
    }

    xqc_send_queue_drop_packets(conn);

    if (conn->the_path) {
        xqc_timer_unset(&(conn->the_path->path_send_ctl->path_timer_manager), XQC_TIMER_ACK);
        xqc_timer_unset(&(conn->the_path->path_send_ctl->path_timer_manager), XQC_TIMER_LOSS_DETECTION);
    }
}

xqc_int_t
xqc_conn_immediate_close(xqc_connection_t *conn)
{
    int ret;

    if (conn->conn_state >= XQC_CONN_STATE_DRAINING) {
        return XQC_OK;
    }

    if (conn->conn_type == XQC_CONN_TYPE_SERVER
       && !(conn->conn_flag & XQC_CONN_FLAG_HANDSHAKE_RECVD))
    {
        conn->conn_state = XQC_CONN_STATE_CLOSED;
        xqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);
        xqc_conn_log(conn, XQC_LOG_ERROR, "|server cannot send CONNECTION_CLOSE before initial pkt received|");
        return XQC_OK;
    }

    if (conn->conn_state < XQC_CONN_STATE_CLOSING) {
        xqc_conn_shutdown(conn);

        /* convert state to CLOSING */
        xqc_log(conn->log, XQC_LOG_INFO, "|state to closing|state:%s|flags:%s",
                xqc_conn_state_2_str(conn->conn_state),
                xqc_conn_flag_2_str(conn, conn->conn_flag));
        conn->conn_state = XQC_CONN_STATE_CLOSING;
        xqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);
    }

    /*
     * [Transport] 10.3.  Immediate Close, During the closing period, an endpoint that sends a CONNECTION_CLOSE
     * frame SHOULD respond to any incoming packet that can be decrypted with another packet containing a CONNECTION_CLOSE
     * frame.  Such an endpoint SHOULD limit the number of packets it generates containing a CONNECTION_CLOSE frame.
     */
    if (conn->conn_close_count < MAX_RSP_CONN_CLOSE_CNT) {
        ret = xqc_write_conn_close_to_packet(conn, conn->conn_err);
        if (ret) {
            xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_write_conn_close_to_packet error|ret:%d|", ret);
        }
        ++conn->conn_close_count;
        xqc_log(conn->log, XQC_LOG_INFO, "|gen_conn_close|state:%s|", xqc_conn_state_2_str(conn->conn_state));
    }

    return XQC_OK;
}

static void
xqc_conn_destroy_ping_notification_list(xqc_connection_t *conn)
{
    xqc_list_head_t *pos, *next;
    xqc_ping_record_t *pr;
    xqc_list_for_each_safe(pos, next, &conn->ping_notification_list) {
        pr = xqc_list_entry(pos, xqc_ping_record_t, list);
        xqc_conn_destroy_ping_record(pr);
    }
}

static void
xqc_conn_destroy_cids(xqc_connection_t *conn)
{
    xqc_cid_inner_t *cid = NULL;
    xqc_list_head_t *pos, *next;
    xqc_cid_set_inner_t *inner_set = NULL;
    xqc_list_head_t *pos_set, *next_set;

    if (conn->engine->conns_hash) {
        if (xqc_find_conns_hash(conn->engine->conns_hash, conn,
                                conn->original_dcid.cid_buf,
                                conn->original_dcid.cid_len))
        {
            xqc_remove_conns_hash(conn->engine->conns_hash, conn,
                                  conn->original_dcid.cid_buf,
                                  conn->original_dcid.cid_len);
        }
        xqc_list_for_each_safe(pos_set, next_set, &conn->scid_set.cid_set_list) {
            inner_set = xqc_list_entry(pos_set, xqc_cid_set_inner_t, next);

            xqc_list_for_each_safe(pos, next, &inner_set->cid_list) {
                cid = xqc_list_entry(pos, xqc_cid_inner_t, list);
                if (xqc_find_conns_hash(conn->engine->conns_hash, conn,
                                        cid->cid.cid_buf, cid->cid.cid_len))
                {
                    xqc_remove_conns_hash(conn->engine->conns_hash, conn,
                                        cid->cid.cid_buf, cid->cid.cid_len);
                }
            }
        }
    }

    xqc_destroy_cid_set(&conn->scid_set);
    xqc_destroy_cid_set(&conn->dcid_set);
}

void
xqc_conn_destroy(xqc_connection_t *xc)
{
    if (!xc) {
        return;
    }

    if (xc->conn_flag & XQC_CONN_FLAG_TICKING) {
        xqc_log(xc->log, XQC_LOG_ERROR, "|in XQC_CONN_FLAG_TICKING|%p|", xc);
        xc->conn_state = XQC_CONN_STATE_CLOSED;
        xqc_log_event(xc->log, CON_CONNECTION_STATE_UPDATED, xc);
        return;
    }

    if (xc->log->log_level >= XQC_LOG_STATS) {
        xqc_conn_stats_t conn_stats;
        xqc_memzero(&conn_stats, sizeof(xqc_conn_stats_t));
        xqc_conn_get_stats_internal(xc, &conn_stats);

        xqc_log(xc->log, XQC_LOG_STATS, "|%p|alpn:%s|"
            "handshake_time:%ui|"
            "first_send_delay:%ui|conn_persist:%ui|err:0x%xi|close_msg:%s|%s|"
            "hsk_recv:%ui|close_recv:%ui|close_send:%ui|last_recv:%ui|last_send:%ui|"
            "rebind_count:%d|rebind_valid:%d|rtx_pkt:%ud|tlp_pkt:%ud|"
            "snd_pkt:%ud|spurious_loss:%ud|detected_loss:%ud|"
            "max_pto:%ud|finished_streams:%ud|cli_bidi_s:%ud|svr_bidi_s:%ud|"
            "max_po_size:%uz|max_acked_po_size:%uz|"
            ,
            xc, conn_stats.alpn,
            xqc_calc_delay(xc->handshake_complete_time, xc->conn_create_time),
            xqc_calc_delay(xc->first_data_send_time, xc->conn_create_time),
            xqc_monotonic_timestamp() - xc->conn_create_time,
            xc->conn_err, xc->conn_close_msg ? xc->conn_close_msg : "", xqc_conn_addr_str(xc),
            xqc_calc_delay(xc->handshake_recv_time, xc->conn_create_time),
            xqc_calc_delay(xc->conn_close_recv_time, xc->conn_create_time),
            xqc_calc_delay(xc->conn_close_send_time, xc->conn_create_time),
            xqc_calc_delay(xc->conn_last_recv_time, xc->conn_create_time),
            xqc_calc_delay(xc->conn_last_send_time, xc->conn_create_time),
            conn_stats.total_rebind_count, conn_stats.total_rebind_valid,
            conn_stats.lost_count, conn_stats.tlp_count,
            conn_stats.send_count, conn_stats.spurious_loss_count, xc->detected_loss_cnt,
            xc->max_pto_cnt, xc->finished_streams, xc->cli_bidi_streams, xc->svr_bidi_streams,
            xc->pkt_out_size, xc->max_acked_po_size
            );
    }

    xqc_log_event(xc->log, CON_CONNECTION_CLOSED, xc);

    xqc_engine_remove_wakeup_queue(xc->engine, xc);

    xqc_list_head_t *pos, *next;
    xqc_stream_t    *stream;
    xqc_packet_in_t *packet_in;

    /* destroy streams, must before conn_close_notify */
    xqc_list_for_each_safe(pos, next, &xc->conn_all_streams) {
        stream = xqc_list_entry(pos, xqc_stream_t, all_stream_list);
        XQC_STREAM_CLOSE_MSG(stream, "conn closed");
        xqc_destroy_stream(stream);
    }

    /* notify destruction */
    if (xc->conn_flag & XQC_CONN_FLAG_UPPER_CONN_EXIST) {
        /* ALPN negotiated, notify close through application layer protocol callback function */
        if (xc->app_proto_cbs.conn_cbs.conn_close_notify) {
            xc->app_proto_cbs.conn_cbs.conn_close_notify(xc, &xc->scid_set.user_scid,
                                                         xc->user_data,
                                                         xc->proto_data);

        } else if (xc->transport_cbs.server_refuse) {
            /* ALPN context is not initialized, ClientHello has not been received */
            xc->transport_cbs.server_refuse(xc->engine, xc, &xc->scid_set.user_scid, xc->user_data);
            xqc_log(xc->log, XQC_LOG_REPORT,
                    "|conn close notified by refuse|%s", xqc_conn_addr_str(xc));

        } else {
            xqc_log(xc->log, XQC_LOG_REPORT,
                    "|conn close event not notified|%s", xqc_conn_addr_str(xc));
        }

        xc->conn_flag &= ~XQC_CONN_FLAG_UPPER_CONN_EXIST;
    }

    xqc_send_queue_destroy(xc->conn_send_queue);

    /* free streams hash */
    if (xc->streams_hash) {
        xqc_id_hash_release(xc->streams_hash);
        xc->streams_hash = NULL;
    }

    if (xc->passive_streams_hash) {
        xqc_id_hash_release(xc->passive_streams_hash);
        xc->passive_streams_hash = NULL;
    }

    xqc_conn_destroy_paths_list(xc);

    xqc_conn_destroy_ping_notification_list(xc);

    /* remove from engine's conns_hash and destroy cid_set*/
    xqc_conn_destroy_cids(xc);

    xqc_log_release(xc->log);

    if (xc->alpn) {
        xqc_free(xc->alpn);
    }

    /* free pool, must be the last thing to do */
    if (xc->conn_pool) {
        xqc_destroy_pool(xc->conn_pool);
    }
}

xqc_int_t
xqc_conn_version_check(xqc_connection_t *c, uint32_t version)
{
    xqc_engine_t *engine = c->engine;
    int i = 0;

    if (c->conn_type == XQC_CONN_TYPE_SERVER && c->version == XQC_IDRAFT_INIT_VER) {

        uint32_t *list = engine->config->support_version_list;
        uint32_t count = engine->config->support_version_count;

        if (xqc_uint32_list_find(list, count, version) == -1) {
            return -XQC_EPROTO;
        }

        for (i = XQC_IDRAFT_INIT_VER + 1; i < XQC_IDRAFT_VER_NEGOTIATION; i++) {
            if (xqc_proto_version_value[i] == version) {
                c->version = i;
                return XQC_OK;
            }
        }

        return -XQC_EPROTO;
    }

    return XQC_OK;
}

xqc_int_t
xqc_conn_send_version_negotiation(xqc_connection_t *c)
{
    xqc_packet_out_t *packet_out = xqc_packet_out_get_and_insert_send(c->conn_send_queue, XQC_PTYPE_VERSION_NEGOTIATION);
    if (packet_out == NULL) {
        xqc_log(c->log, XQC_LOG_ERROR, "|get XQC_PTYPE_VERSION_NEGOTIATION error|");
        return -XQC_EWRITE_PKT;
    }

    unsigned char *p = packet_out->po_buf;
    /* first byte of packet */
    *p++ = (1 << 7);

    /* version */
    *(uint32_t *)p = 0;
    p += sizeof(uint32_t);

    /* dcid len */
    *p = c->dcid_set.current_dcid.cid_len;
    ++p;

    /* dcid */
    memcpy(p, c->dcid_set.current_dcid.cid_buf, c->dcid_set.current_dcid.cid_len);
    p += c->dcid_set.current_dcid.cid_len;

    /* original destination ID len */
    *p = c->original_dcid.cid_len;
    ++p;

    /* original destination ID */
    memcpy(p, c->original_dcid.cid_buf, c->original_dcid.cid_len);
    p += c->original_dcid.cid_len;

    /* set supported version list */
    uint32_t *version_list = c->engine->config->support_version_list;
    uint32_t version_count = c->engine->config->support_version_count;
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
    xqc_engine_remove_wakeup_queue(c->engine, c);
    xqc_engine_add_active_queue(c->engine, c);

    c->conn_flag &= ~XQC_CONN_FLAG_VERSION_NEGOTIATION;
    return XQC_OK;
}

xqc_int_t
xqc_conn_client_on_alpn(xqc_connection_t *conn, const unsigned char *alpn, size_t alpn_len)
{
    xqc_int_t ret;

    /* save alpn */
    conn->alpn = xqc_calloc(1, alpn_len + 1);
    if (conn->alpn == NULL) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|malloc alpn buffer error|");
        return -XQC_EMALLOC;
    }

    xqc_memcpy(conn->alpn, alpn, alpn_len);
    conn->alpn_len = alpn_len;

    /* set quic callbacks to quic connection */
    ret = xqc_engine_get_alpn_callbacks(conn->engine, alpn, alpn_len, &conn->app_proto_cbs);
    if (ret != XQC_OK) {
        xqc_free(conn->alpn);
        conn->alpn = NULL;
        conn->alpn_len = 0;
        xqc_log(conn->log, XQC_LOG_ERROR, "|can't get application layer callback|ret:%d", ret);
        return ret;
    }

    return XQC_OK;
}

xqc_int_t
xqc_conn_server_on_alpn(xqc_connection_t *conn, const unsigned char *alpn, size_t alpn_len)
{
    xqc_int_t ret;

    /* save alpn */
    conn->alpn = xqc_calloc(1, alpn_len + 1);
    if (conn->alpn == NULL) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|malloc alpn buffer error|");
        return -XQC_EMALLOC;
    }

    xqc_memcpy(conn->alpn, alpn, alpn_len);
    conn->alpn_len = alpn_len;

    /* set quic callbacks to quic connection */
    ret = xqc_engine_get_alpn_callbacks(conn->engine, alpn, alpn_len, &conn->app_proto_cbs);
    if (ret != XQC_OK) {
        xqc_free(conn->alpn);
        conn->alpn = NULL;
        conn->alpn_len = 0;
        xqc_log(conn->log, XQC_LOG_ERROR, "|can't get application layer callback|ret:%d", ret);
        return ret;
    }

    uint8_t tp_buf[XQC_MAX_TRANSPORT_PARAM_BUF_LEN] = {0};
    size_t tp_len = 0;

    /* do callback */
    if (conn->app_proto_cbs.conn_cbs.conn_create_notify) {
        if (conn->app_proto_cbs.conn_cbs.conn_create_notify(conn, &conn->scid_set.user_scid,
            conn->user_data, conn->proto_data))
        {
            goto err;
        }
        conn->conn_flag |= XQC_CONN_FLAG_UPPER_CONN_EXIST;
    }

    return XQC_OK;

err:
    XQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
    return -TRA_INTERNAL_ERROR;
}

/* check whether if the dcid is valid for the connection */
xqc_int_t
xqc_conn_check_dcid(xqc_connection_t *conn, xqc_cid_t *dcid)
{
    xqc_int_t ret;

    xqc_cid_inner_t *scid = xqc_cid_set_search_cid(&conn->scid_set, dcid);
    if (scid == NULL) {
        return -XQC_ECONN_CID_NOT_FOUND;
    }

    if (scid->state == XQC_CID_UNUSED) {
        ret = xqc_cid_switch_to_next_state(&conn->scid_set, scid, XQC_CID_USED, scid->cid.path_id);
        if (ret < 0) {
            xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_cid_switch_to_next_state error|scid:%s|",
                    xqc_scid_str(conn->engine, &scid->cid));
            return ret;
        }
    }

    return XQC_OK;
}

xqc_usec_t
xqc_conn_next_wakeup_time(xqc_connection_t *conn)
{
    xqc_usec_t min_time = XQC_MAX_UINT64_VALUE;
    xqc_usec_t wakeup_time;
    xqc_timer_t *timer;

    for (xqc_timer_type_t type = 0; type < XQC_TIMER_N; ++type) {
        timer = &conn->conn_timer_manager.timer[type];
        if (timer->timer_is_set) {
            min_time = xqc_min(min_time, timer->expire_time);
        }
    }

    xqc_path_ctx_t *path = conn->the_path;
    if (path && path->path_state == XQC_PATH_STATE_ACTIVE) {
        for (xqc_timer_type_t type = 0; type < XQC_TIMER_N; ++type) {
            timer = &(path->path_send_ctl->path_timer_manager.timer[type]);
            if (timer->timer_is_set) {
                min_time = xqc_min(min_time, timer->expire_time);
            }
        }
    }

    wakeup_time = min_time == XQC_MAX_UINT64_VALUE ? 0 : min_time;

    return wakeup_time;
}

void
xqc_conn_timer_expire(xqc_connection_t *conn, xqc_usec_t now)
{
    xqc_timer_expire(&conn->conn_timer_manager, now);

    xqc_path_ctx_t *path = conn->the_path;;
    if (path && path->path_state < XQC_PATH_STATE_CLOSED) {
        xqc_timer_expire(&path->path_send_ctl->path_timer_manager, now);
    }
}

static inline uint8_t
xqc_conn_tolerant_error(xqc_int_t ret)
{
    if (-XQC_EVERSION == ret || -XQC_EILLPKT == ret || -XQC_EWAITING == ret || -XQC_EIGNORE_PKT == ret)
    {
        return XQC_TRUE;
    }
    return XQC_FALSE;
}

static inline void
xqc_conn_log_recvd_packet(xqc_connection_t *c, xqc_packet_in_t *pi,
    size_t udp_size, xqc_int_t err, xqc_usec_t timestamp)
{
    int index = c->rcv_pkt_stats.curr_index;
    c->rcv_pkt_stats.pkt_frames[index] = pi->pi_frame_types;
    c->rcv_pkt_stats.pkt_err[index] = err;
    c->rcv_pkt_stats.pkt_size[index] = pi->pi_pkt.length;
    c->rcv_pkt_stats.pkt_timestamp[index] = xqc_calc_delay(timestamp,
                                                           c->conn_create_time);
    c->rcv_pkt_stats.pkt_timestamp[index] /= 1000; // ms
    c->rcv_pkt_stats.pkt_udp_size[index] = udp_size;
    c->rcv_pkt_stats.pkt_types[index] = pi->pi_pkt.pkt_type;
    c->rcv_pkt_stats.pkt_pn[index] = pi->pi_pkt.pkt_num;
    c->rcv_pkt_stats.conn_rcvd_pkts++;
    c->rcv_pkt_stats.curr_index = (index + 1) % 3;
}

static xqc_int_t
xqc_conn_confirm_cid(xqc_connection_t *c, xqc_packet_t *pkt)
{
    /*
     *  after a successful process of Initial packet, SCID from Initial
     *  is not equal to what remembered when connection was created, as
     *  server is not willing to use the client's DCID as SCID;
     */

    xqc_int_t ret;

    if (!(c->conn_flag & XQC_CONN_FLAG_DCID_DONE)) {

        if (xqc_cid_in_cid_set(&c->dcid_set, &pkt->pkt_scid, XQC_INITIAL_PATH_ID) == NULL) {
            ret = xqc_cid_set_insert_cid(&c->dcid_set, &pkt->pkt_scid, XQC_CID_USED,
                                         XQC_CONN_ACTIVE_CID_LIMIT, XQC_INITIAL_PATH_ID);
            if (ret != XQC_OK) {
                xqc_log(c->log, XQC_LOG_ERROR,
                        "|xqc_cid_set_insert_cid error|limit:%ui|unused:%i|used:%i|",
                        XQC_CONN_ACTIVE_CID_LIMIT,
                        xqc_cid_set_get_unused_cnt(&c->dcid_set, XQC_INITIAL_PATH_ID),
                        xqc_cid_set_get_used_cnt(&c->dcid_set, XQC_INITIAL_PATH_ID));
                return ret;
            }
        }

        if (XQC_OK != xqc_cid_is_equal(&c->dcid_set.current_dcid, &pkt->pkt_scid)) {
            xqc_log(c->log, XQC_LOG_INFO, "|dcid change|ori:%s|new:%s|",
                    xqc_dcid_str(c->engine, &c->dcid_set.current_dcid), xqc_scid_str(c->engine, &pkt->pkt_scid));
            xqc_cid_copy(&c->dcid_set.current_dcid, &pkt->pkt_scid);
            xqc_cid_copy(&c->the_path->path_dcid, &pkt->pkt_scid);
        }

        c->conn_flag |= XQC_CONN_FLAG_DCID_DONE;
    }

    return XQC_OK;
}

static xqc_int_t
xqc_conn_on_initial_processed(xqc_connection_t *c, xqc_packet_in_t *pi, xqc_usec_t now)
{
    /* successful process of initial packet means that pkt's DCID/SCID is confirmed */
    return xqc_conn_confirm_cid(c, &pi->pi_pkt);
}

static void
xqc_conn_record_single(xqc_connection_t *c, xqc_packet_in_t *packet_in)
{
    if (!xqc_has_packet_number(&packet_in->pi_pkt)) {
        return;
    }

    xqc_path_ctx_t *path = c->the_path;
    if (path == NULL) {
        return;
    }

    /* update path stats */
    if (packet_in->pi_frame_types & XQC_FRAME_BIT_STREAM) {
        path->path_send_ctl->ctl_app_bytes_recv += packet_in->buf_size;
    }

    xqc_pn_ctl_t *pn_ctl = xqc_get_pn_ctl(c, path);
    xqc_send_ctl_t *send_ctl = path->path_send_ctl;

    xqc_pkt_range_status range_status;
    int out_of_order = 0;
    xqc_packet_number_t pkt_num = packet_in->pi_pkt.pkt_num;

    range_status = xqc_recv_record_add(&pn_ctl->ctl_recv_record, pkt_num);
    if (range_status == XQC_PKTRANGE_OK) {
        if (XQC_IS_ACK_ELICITING(packet_in->pi_frame_types)) {
            ++send_ctl->ctl_ack_eliciting_pkt;

            if (pkt_num > send_ctl->ctl_largest_received || send_ctl->ctl_largest_received == XQC_MAX_UINT64_VALUE) {
                send_ctl->ctl_largest_received = pkt_num;
                send_ctl->ctl_largest_recv_time = packet_in->pkt_recv_time;
            }
        }

        if (pkt_num != xqc_recv_record_largest(&pn_ctl->ctl_recv_record)) {
            out_of_order = 1;
        }

        xqc_maybe_should_ack(c, path, pn_ctl, out_of_order, packet_in->pkt_recv_time);
    }
}

static xqc_int_t
xqc_conn_on_pkt_processed(xqc_connection_t *c, xqc_packet_in_t *pi, xqc_usec_t now)
{
    xqc_int_t ret = XQC_OK;
    switch (pi->pi_pkt.pkt_type) {
    case XQC_PTYPE_INIT:
        ret = xqc_conn_on_initial_processed(c, pi, now);
        break;

    case XQC_PTYPE_SHORT_HEADER:
        // TODOXXXX
        break;

    default:
        break;
    }

    /* record packet */
    xqc_conn_record_single(c, pi);
    if (pi->pi_frame_types & (~(XQC_FRAME_BIT_STREAM|XQC_FRAME_BIT_PADDING))) {
        c->conn_flag |= XQC_CONN_FLAG_NEED_RUN;
    }

    c->conn_last_recv_time = now;

    return ret;
}

xqc_int_t
xqc_conn_process_packet(xqc_connection_t *c,
    const unsigned char *packet_in_buf, size_t packet_in_size,
    xqc_usec_t recv_time)
{
    xqc_int_t ret = XQC_OK;
    const unsigned char *last_pos = NULL;
    const unsigned char *pos = packet_in_buf;                   /* start of QUIC pkt */
    const unsigned char *end = packet_in_buf + packet_in_size;  /* end of udp datagram */
    xqc_packet_in_t packet;

    /* process all QUIC packets in UDP datagram */
    while (pos < end) {
        last_pos = pos;

        /* init packet in */
        xqc_packet_in_t *packet_in = &packet;
        memset(packet_in, 0, sizeof(*packet_in));
        xqc_packet_in_init(packet_in, pos, end - pos, recv_time);

        /* packet_in->pos will update inside */
        ret = xqc_packet_process_single(c, packet_in);

        xqc_conn_log_recvd_packet(c, packet_in, packet_in_size, ret, recv_time);

        if (ret == XQC_OK) {
            ret = xqc_conn_on_pkt_processed(c, packet_in, recv_time);

        } else if (xqc_conn_tolerant_error(ret)) {
            /* ignore the remain bytes */
            packet_in->pos = packet_in->last;
            ret = XQC_OK;
            goto end;
        }

        /* error occurred or read state is error */
        if (ret != XQC_OK || last_pos == packet_in->pos) {
            /* if last_pos equals packet_in->pos, might trigger infinite loop, return to avoid it */
            xqc_log(c->log, XQC_LOG_ERROR, "|process packets err|ret:%d|pos:%p|buf:%p|buf_size:%uz|",
                    ret, packet_in->pos, packet_in->buf, packet_in->buf_size);
            return ret != XQC_OK ? ret : -XQC_ESYS;
        }

        /* consume all the bytes and start parse next QUIC packet */
        pos = packet_in->last;
        xqc_log_event(c->log, TRA_PACKET_RECEIVED, packet_in);
    }
end:
    return ret;
}

void
xqc_conn_process_packet_recved_path(xqc_connection_t *conn, xqc_cid_t *scid,
    size_t packet_in_size, xqc_usec_t recv_time)
{
    if (conn->the_path) {
        xqc_send_ctl_on_dgram_received(conn->the_path->path_send_ctl, packet_in_size);
    }
}

static void
xqc_conn_schedule_packets(xqc_connection_t *conn,  xqc_list_head_t *head,
    xqc_bool_t  packets_are_limited_by_cc, xqc_send_type_t send_type)
{
    xqc_usec_t now = xqc_monotonic_timestamp();
    xqc_path_ctx_t *path = conn->the_path;
    xqc_send_ctl_t *send_ctl = path->path_send_ctl;

    xqc_list_head_t *pos, *next;
    xqc_packet_out_t *packet_out;

    xqc_list_for_each_safe(pos, next, head) {
        packet_out = xqc_list_entry(pos, xqc_packet_out_t, po_list);

        if (packets_are_limited_by_cc &&
            !xqc_send_packet_cwnd_allows(send_ctl, packet_out, path->path_schedule_bytes, 0))
        {
            conn->sched_cc_blocked++;
            if (packet_out->po_sched_cwnd_blk_ts == 0) {
                packet_out->po_sched_cwnd_blk_ts = now;
            }
            return;
        }

        xqc_path_send_buffer_append(path, packet_out, &path->path_schedule_buf[send_type]);
    }
}

void
xqc_conn_schedule_packets_to_paths(xqc_connection_t *conn)
{
    /* do neither CC nor Pacing */
    xqc_list_head_t *head = &conn->conn_send_queue->sndq_pto_probe_packets;

    xqc_conn_schedule_packets(conn, head, XQC_FALSE, XQC_SEND_TYPE_PTO_PROBE);

    head = &conn->conn_send_queue->sndq_lost_packets;

    xqc_conn_schedule_packets(conn, head, XQC_TRUE, XQC_SEND_TYPE_RETRANS);

    head = &conn->conn_send_queue->sndq_send_packets_high_pri;
    xqc_conn_schedule_packets(conn, head, XQC_FALSE, XQC_SEND_TYPE_NORMAL_HIGH_PRI);

    head = &conn->conn_send_queue->sndq_send_packets;
    xqc_conn_schedule_packets(conn, head, XQC_TRUE, XQC_SEND_TYPE_NORMAL);
}

static inline void
xqc_conn_log_sent_packet(xqc_connection_t *c, xqc_packet_out_t *po,
    xqc_usec_t timestamp)
{
    int index = c->snd_pkt_stats.curr_index;
    c->snd_pkt_stats.pkt_frames[index] = po->po_frame_types;
    c->snd_pkt_stats.pkt_size[index] = po->po_used_size;
    c->snd_pkt_stats.pkt_timestamp[index] = xqc_calc_delay(timestamp,
                                                           c->conn_create_time);
    c->snd_pkt_stats.pkt_timestamp[index] /= 1000;
    c->snd_pkt_stats.pkt_types[index] = po->po_pkt.pkt_type;
    c->snd_pkt_stats.pkt_pn[index] = po->po_pkt.pkt_num;
    c->snd_pkt_stats.conn_sent_pkts++;
    c->snd_pkt_stats.curr_index = (index + 1) % 3;
}

/* send data with callback, and process callback errors */
static ssize_t
xqc_send(xqc_connection_t *conn, xqc_path_ctx_t *path, unsigned char *data, unsigned int len)
{
    ssize_t sent;

    if (conn->pkt_filter_cb) {
        sent = conn->pkt_filter_cb(data, len, (struct sockaddr *)conn->peer_addr,
                                   conn->peer_addrlen, conn->pkt_filter_cb_user_data);
        if (sent < 0) {
            xqc_log(conn->log, XQC_LOG_ERROR,  "|pkt_filter_cb error|conn:%p|"
                    "size:%ud|sent:%z|", conn, len, sent);

            return sent == XQC_SOCKET_EAGAIN ? -XQC_EAGAIN : -XQC_EPACKET_FILETER_CALLBACK;
        }
        sent = len;

    } else {
        sent = conn->transport_cbs.write_socket(data, len,
                                                (struct sockaddr *)conn->peer_addr,
                                                conn->peer_addrlen,
                                                xqc_conn_get_user_data(conn));
        if (sent != len) {
            xqc_log(conn->log, XQC_LOG_ERROR,
                    "|write_socket error|conn:%p|size:%ud|sent:%z|", conn, len, sent);

            /* if callback return XQC_SOCKET_ERROR, close the connection */
            if (sent == XQC_SOCKET_ERROR) {
                xqc_log(conn->log, XQC_LOG_ERROR, "|conn:%p|socket exception, close connection|", conn);
                conn->conn_state = XQC_CONN_STATE_CLOSED;
                xqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);
            }

            return sent == XQC_SOCKET_EAGAIN ? -XQC_EAGAIN : -XQC_ESOCKET;
        }
    }

    xqc_log_event(conn->log, TRA_DATAGRAMS_SENT, sent, path->path_id);

    return sent;
}

/* send packets which have no packet number */
static ssize_t
xqc_process_packet_without_pn(xqc_connection_t *conn, xqc_path_ctx_t *path, xqc_packet_out_t *packet_out)
{
    /* directly send to peer */
    ssize_t sent = xqc_send(conn, path, packet_out->po_buf, packet_out->po_used_size);
    xqc_log_event(conn->log, TRA_PACKET_SENT, conn, packet_out, path, 0, sent, 0);
    if (sent > 0) {
        xqc_conn_log_sent_packet(conn, packet_out, xqc_monotonic_timestamp());
    }
    return sent;
}

/* send data in packet number space */
static ssize_t
xqc_send_packet_with_pn(xqc_connection_t *conn, xqc_path_ctx_t *path, xqc_packet_out_t *packet_out)
{
    /* record the send time of packet */
    xqc_usec_t now = xqc_monotonic_timestamp();
    packet_out->po_sent_time = now;

    /* send data */
    ssize_t sent = xqc_send(conn, path, packet_out->po_buf, packet_out->po_used_size);
    if (sent != packet_out->po_used_size) {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|write_socket error|conn:%p|path:%ui|pkt_num:%ui|size:%ud|sent:%z|pkt_type:%s|frame:%s|now:%ui|",
                conn, path->path_id, packet_out->po_pkt.pkt_num, packet_out->po_used_size, sent,
                xqc_pkt_type_2_str(packet_out->po_pkt.pkt_type),
                xqc_frame_type_2_str(conn->engine, packet_out->po_frame_types), now);
        return sent;

    } else {
        xqc_log_event(conn->log, TRA_PACKET_SENT, conn, packet_out, path, now, sent, 1);
    }

    /* deliver packet to send control */
    xqc_pn_ctl_t *pn_ctl = xqc_get_pn_ctl(conn, path);

    xqc_conn_log_sent_packet(conn, packet_out, now);
    xqc_send_ctl_on_packet_sent(path->path_send_ctl, pn_ctl, packet_out, now);
    return sent;
}

static void
xqc_encode_packet_with_pn(xqc_connection_t *conn, xqc_path_ctx_t *path, xqc_packet_out_t *packet_out)
{
    /* update dcid by send path */
    xqc_short_packet_update_dcid(packet_out, path->path_dcid);

    /* generate packet number */
    xqc_pn_ctl_t *pn_ctl = xqc_get_pn_ctl(conn, path);
    xqc_usec_t current_time = xqc_monotonic_timestamp();
    xqc_send_ctl_set_next_pn_for_packet(conn, pn_ctl, packet_out, current_time);

    xqc_write_packet_number(packet_out->po_ppktno, packet_out->po_pkt.pkt_num, XQC_PKTNO_CLASS);
    xqc_update_packet_length(packet_out);
}

/* process and send packet which has a packet number */
static ssize_t
xqc_process_packet_with_pn(xqc_connection_t *conn, xqc_path_ctx_t *path, xqc_packet_out_t *packet_out)
{
    xqc_encode_packet_with_pn(conn, path, packet_out);
    /* send packet in packet number space */
    return xqc_send_packet_with_pn(conn, path, packet_out);
}

static ssize_t
xqc_path_send_one_packet(xqc_connection_t *conn, xqc_path_ctx_t *path, xqc_packet_out_t *packet_out)
{
    if (xqc_has_packet_number(&packet_out->po_pkt)) {
        return xqc_process_packet_with_pn(conn, path, packet_out);

    } else {
        return xqc_process_packet_without_pn(conn, path, packet_out);
    }
}

static xqc_int_t
xqc_check_acked_or_dropped_pkt(xqc_connection_t *conn,
    xqc_packet_out_t *packet_out, xqc_send_type_t send_type)
{
    if (xqc_send_ctl_indirectly_ack_or_drop_po(conn, packet_out)) {
        return XQC_TRUE;
    }

    if (send_type == XQC_SEND_TYPE_RETRANS) {
        /* If not a TLP packet, mark it LOST */
        packet_out->po_flag |= XQC_POF_LOST;
    }

    return XQC_FALSE;
}

static void
xqc_path_send_packets(xqc_connection_t *conn, xqc_path_ctx_t *path,
    xqc_list_head_t *head, int congest, xqc_send_type_t send_type)
{
    ssize_t ret = 0;
    xqc_list_head_t  *pos, *next;
    xqc_packet_out_t *packet_out;

    xqc_send_ctl_t *send_ctl = path->path_send_ctl;
    xqc_send_queue_t *send_queue = conn->conn_send_queue;

    xqc_usec_t now = xqc_monotonic_timestamp();

    xqc_list_for_each_safe(pos, next, &path->path_schedule_buf[send_type]) {
        packet_out = xqc_list_entry(pos, xqc_packet_out_t, po_list);

        if (xqc_check_acked_or_dropped_pkt(conn, packet_out, send_type)) {
            continue;
        }

        /* check cc limit */
        if (congest
            && !xqc_send_packet_check_cc(send_ctl, packet_out, 0, now))
        {
            send_ctl->ctl_conn->send_cc_blocked++;
            break;
        }

        ret = xqc_path_send_one_packet(conn, path, packet_out);
        if (ret < 0) {
            break;
        }

        if (XQC_CAN_IN_FLIGHT(packet_out->po_frame_types)
            && xqc_pacing_is_on(&send_ctl->ctl_pacing))
        {
            xqc_pacing_on_packet_sent(&send_ctl->ctl_pacing, packet_out->po_used_size);
        }

        /* move send list to unacked list */
        xqc_path_send_buffer_remove(path, packet_out);
        if (XQC_IS_ACK_ELICITING(packet_out->po_frame_types)) {
            xqc_send_queue_insert_unacked(packet_out,
                                          &send_queue->sndq_unacked_packets,
                                          send_queue);

        } else {
            xqc_send_queue_insert_free(packet_out, &send_queue->sndq_free_packets, send_queue);
        }
    }

    /* @FIXME: in the case of EAGAIN, we should not reschedule packets. */
    if (ret < 0 && ret != -XQC_EAGAIN) {
        xqc_path_send_buffer_clear(conn, path, head, send_type);
    }
}

void
xqc_conn_transmit_pto_probe_packets(xqc_connection_t *conn)
{
    /* do neither CC nor Pacing */
    int congest = 0;
    xqc_list_head_t *head = &conn->conn_send_queue->sndq_pto_probe_packets;
    xqc_path_send_packets(conn, conn->the_path, head, congest, XQC_SEND_TYPE_PTO_PROBE);
}

void
xqc_conn_retransmit_lost_packets(xqc_connection_t *conn)
{
    /* do congestion control */
    int congest = 1;
    xqc_list_head_t *head = &conn->conn_send_queue->sndq_lost_packets;
    xqc_path_send_packets(conn, conn->the_path, head, congest, XQC_SEND_TYPE_RETRANS);
}

void
xqc_conn_send_packets(xqc_connection_t *conn)
{
    /* high priority packets are not limited by CC */
    int congest = 0;
    xqc_list_head_t *head = &conn->conn_send_queue->sndq_send_packets_high_pri;
    xqc_path_send_packets(conn, conn->the_path, head, congest, XQC_SEND_TYPE_NORMAL_HIGH_PRI);

    congest = 1;
    head = &conn->conn_send_queue->sndq_send_packets;
    xqc_path_send_packets(conn, conn->the_path, head, congest, XQC_SEND_TYPE_NORMAL);
}

static void
xqc_on_packets_send_burst(xqc_connection_t *conn, xqc_path_ctx_t *path, ssize_t sent, xqc_usec_t now, xqc_send_type_t send_type)
{
    xqc_list_head_t  *pos, *next;
    xqc_packet_out_t *packet_out;
    int remove_count = 0; /* remove from send */

    xqc_send_ctl_t *send_ctl = path->path_send_ctl;
    xqc_pn_ctl_t *pn_ctl = xqc_get_pn_ctl(conn, path);
    xqc_send_queue_t *send_queue = conn->conn_send_queue;

    xqc_list_for_each_safe(pos, next, &path->path_schedule_buf[send_type]) {
        if (remove_count >= sent) {
            break;
        }

        packet_out = xqc_list_entry(pos, xqc_packet_out_t, po_list);

        xqc_conn_log_sent_packet(conn, packet_out, now);

        if (xqc_has_packet_number(&packet_out->po_pkt)) {
            /* count packets with pkt_num in the send control */
            if (XQC_CAN_IN_FLIGHT(packet_out->po_frame_types
                && xqc_pacing_is_on(&send_ctl->ctl_pacing)))
            {
                xqc_pacing_on_packet_sent(&send_ctl->ctl_pacing, packet_out->po_used_size);
            }

            xqc_send_ctl_on_packet_sent(send_ctl, pn_ctl, packet_out, now);
            xqc_path_send_buffer_remove(path, packet_out);
            if (XQC_IS_ACK_ELICITING(packet_out->po_frame_types)) {
                xqc_send_queue_insert_unacked(packet_out,
                                              &send_queue->sndq_unacked_packets,
                                              send_queue);
            } else {
                xqc_send_queue_insert_free(packet_out, &send_queue->sndq_free_packets, send_queue);
            }
        } else {
            /* packets with no packet number can't be acknowledged, hence they need no control */
            xqc_path_send_buffer_remove(path, packet_out);
            xqc_send_queue_insert_free(packet_out, &send_queue->sndq_free_packets, send_queue);
        }

        remove_count++;
    }
}

static ssize_t
xqc_send_burst(xqc_connection_t *conn, xqc_path_ctx_t *path, struct iovec *iov, int cnt)
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
                xqc_log(conn->log, XQC_LOG_ERROR,
                        "|pkt_filter_cb error|conn:%p|"
                        "size:%ud|sent:%z|sent_cnt:%d|",
                        conn, iov[sent_cnt].iov_len, sent_size,
                        sent_cnt);

                sent_size = sent_size == XQC_SOCKET_EAGAIN ? -XQC_EAGAIN : -XQC_ESOCKET;
                break;
            }
        }

        if (sent_size == -XQC_EAGAIN && sent_cnt == 0) {
            sent_cnt = -XQC_EAGAIN;

        } else if (sent_size == -XQC_ESOCKET) {
            sent_cnt = -XQC_EPACKET_FILETER_CALLBACK;
        }

    } else {
        sent_cnt = conn->transport_cbs.write_mmsg(iov, cnt,
                                              (struct sockaddr *)conn->peer_addr,
                                              conn->peer_addrlen,
                                              xqc_conn_get_user_data(conn));
        if (sent_cnt < 0) {
            xqc_log(conn->log, XQC_LOG_ERROR, "|error send mmsg|");
            if (sent_cnt == XQC_SOCKET_ERROR) {
                xqc_log(conn->log, XQC_LOG_ERROR, "|socket exception, close connection|");
                conn->conn_state = XQC_CONN_STATE_CLOSED;
                xqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);
            }

            sent_cnt = sent_cnt == XQC_SOCKET_EAGAIN ? -XQC_EAGAIN : -XQC_ESOCKET;
        }
    }

    return sent_cnt;
}

static void
xqc_encode_packet_with_pn_ex(xqc_connection_t *conn, xqc_path_ctx_t *path, xqc_packet_out_t *packet_out, xqc_usec_t current_time)
{
    /* update dcid by send path */
    xqc_short_packet_update_dcid(packet_out, path->path_dcid);

    /* generate packet number */
    xqc_pn_ctl_t *pn_ctl = xqc_get_pn_ctl(conn, path);
    xqc_send_ctl_set_next_pn_for_packet(conn, pn_ctl, packet_out, current_time);

    xqc_write_packet_number(packet_out->po_ppktno, packet_out->po_pkt.pkt_num, XQC_PKTNO_CLASS);
    xqc_update_packet_length(packet_out);

    packet_out->po_sent_time = current_time;
}

static ssize_t
xqc_path_send_burst_packets(xqc_connection_t *conn, xqc_path_ctx_t *path,
    int congest, xqc_send_type_t send_type)
{
    ssize_t           ret;
    struct iovec      iov_array[XQC_MAX_SEND_MSG_ONCE];
    int               burst_cnt = 0;
    xqc_packet_out_t *packet_out;
    xqc_list_head_t  *pos, *next;
    xqc_send_ctl_t   *send_ctl = path->path_send_ctl;
    uint32_t          total_bytes_to_send = 0;

    /* process packets */
    xqc_usec_t now = xqc_monotonic_timestamp();
    xqc_list_for_each_safe(pos, next, &path->path_schedule_buf[send_type]) {
        /* process one packet */
        packet_out = xqc_list_entry(pos, xqc_packet_out_t, po_list);
        iov_array[burst_cnt].iov_base = packet_out->po_buf;
        iov_array[burst_cnt].iov_len = packet_out->po_used_size;

        if (xqc_has_packet_number(&packet_out->po_pkt)) {
            if (xqc_check_acked_or_dropped_pkt(conn, packet_out, send_type)) {
                continue;
            }

            /* check cc limit */
            if (congest
                && !xqc_send_packet_check_cc(send_ctl, packet_out, total_bytes_to_send, now))
            {
                send_ctl->ctl_conn->send_cc_blocked++;
                break;
            }

            xqc_encode_packet_with_pn_ex(conn, path, packet_out, now);
            total_bytes_to_send += packet_out->po_used_size;
        }

        /* reach send limit, break and send packets */
        burst_cnt++;
        if (burst_cnt >= XQC_MAX_SEND_MSG_ONCE) {
            burst_cnt = XQC_MAX_SEND_MSG_ONCE;
            break;
        }
    }

    /* nothing to send, return */
    if (burst_cnt == 0) {
        return burst_cnt;
    }

    /* burst send packets */
    ret = xqc_send_burst(conn, path, iov_array, burst_cnt);
    if (ret < 0) {
        return ret;

    } else if (ret != burst_cnt) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|error send msg|sent:%ui||cnt:%d|", ret, burst_cnt);
    }

    xqc_on_packets_send_burst(conn, path, ret, now, send_type);
    return ret;
}

static void
xqc_path_send_packets_batch(xqc_connection_t *conn, xqc_path_ctx_t *path,
    xqc_list_head_t *head, int congest, xqc_send_type_t send_type)
{
    ssize_t send_burst_count = 0;

    while (!(xqc_list_empty(&path->path_schedule_buf[send_type]))) {
        send_burst_count = xqc_path_send_burst_packets(conn, path, congest, send_type);
        if (send_burst_count != XQC_MAX_SEND_MSG_ONCE) {
            break;
        }
    }

    /* @FIXME: in the case of EAGAIN, we should not reschedule packets. */
    if (send_burst_count < 0 && send_burst_count != -XQC_EAGAIN) {
        xqc_path_send_buffer_clear(conn, path, head, send_type);
    }

}

void
xqc_conn_transmit_pto_probe_packets_batch(xqc_connection_t *conn)
{
    /* probe packets MUST NOT be blocked by the congestion controller */
    int congest = 0;
    xqc_list_head_t *head = &conn->conn_send_queue->sndq_pto_probe_packets;
    xqc_path_send_packets_batch(conn, conn->the_path, head, congest, XQC_SEND_TYPE_PTO_PROBE);
}

void
xqc_conn_retransmit_lost_packets_batch(xqc_connection_t *conn)
{
    /* do congestion control */
    int congest = 1;
    xqc_list_head_t *head = &conn->conn_send_queue->sndq_lost_packets;
    xqc_path_send_packets_batch(conn, conn->the_path, head, congest, XQC_SEND_TYPE_RETRANS);
}

void
xqc_conn_send_packets_batch(xqc_connection_t *conn)
{
    int congest = 0;
    xqc_list_head_t *head = &conn->conn_send_queue->sndq_send_packets_high_pri;
    xqc_path_send_packets_batch(conn, conn->the_path, head, congest, XQC_SEND_TYPE_NORMAL_HIGH_PRI);

    congest = 1;
    head = &conn->conn_send_queue->sndq_send_packets;
    xqc_path_send_packets_batch(conn, conn->the_path, head, congest, XQC_SEND_TYPE_NORMAL);

    return;
}

void
xqc_conn_decrease_unacked_stream_ref(xqc_connection_t *conn, xqc_packet_out_t *packet_out)
{
    int first_time_ack = 1;
    if (packet_out->po_flag & XQC_POF_STREAM_UNACK) {
        first_time_ack = first_time_ack && (!packet_out->po_acked);
        if (packet_out->po_origin) {
            first_time_ack = first_time_ack && (!packet_out->po_origin->po_acked);
        }
        if (first_time_ack) {
            xqc_stream_t *stream;
            for (int i = 0; i < XQC_MAX_STREAM_FRAME_IN_PO; i++) {
                if (packet_out->po_stream_frames[i].ps_is_used == 0) {
                    break;
                }
                stream = xqc_find_stream_by_id(packet_out->po_stream_frames[i].ps_stream_id, conn->streams_hash);
                if (stream != NULL) {
                    if (stream->stream_unacked_pkt == 0) {
                        xqc_log(conn->log, XQC_LOG_ERROR, "|stream_unacked_pkt too small|");

                    } else {
                        stream->stream_unacked_pkt--;
                    }

                    if (packet_out->po_stream_frames[i].ps_has_fin && stream->stream_stats.first_fin_ack_time == 0) {
                        stream->stream_stats.first_fin_ack_time = xqc_monotonic_timestamp();
                    }

                    /* Update stream state */
                    if (stream->stream_unacked_pkt == 0 && stream->stream_state_send == XQC_SEND_STREAM_ST_DATA_SENT) {
                        xqc_stream_send_state_update(stream, XQC_SEND_STREAM_ST_DATA_RECVD);
                        xqc_stream_maybe_need_close(stream);
                    }
                }
            }
        }
        packet_out->po_flag &= ~XQC_POF_STREAM_UNACK;
    }
}

void
xqc_conn_increase_unacked_stream_ref(xqc_connection_t *conn, xqc_packet_out_t *packet_out)
{
    if ((packet_out->po_frame_types & XQC_FRAME_BIT_STREAM)
        && !(packet_out->po_flag & XQC_POF_STREAM_UNACK))
    {
        if ((!packet_out->po_origin)) {
            xqc_stream_t *stream;
            for (int i = 0; i < XQC_MAX_STREAM_FRAME_IN_PO; i++) {
                if (packet_out->po_stream_frames[i].ps_is_used == 0) {
                    break;
                }
                stream = xqc_find_stream_by_id(packet_out->po_stream_frames[i].ps_stream_id, conn->streams_hash);
                if (stream != NULL) {
                    stream->stream_unacked_pkt++;
                    /* Update stream state */
                    if (stream->stream_state_send == XQC_SEND_STREAM_ST_READY) {
                        xqc_stream_send_state_update(stream, XQC_SEND_STREAM_ST_SEND);
                    }
                    if (packet_out->po_stream_frames[i].ps_has_fin
                        && stream->stream_state_send == XQC_SEND_STREAM_ST_SEND)
                    {
                        xqc_stream_send_state_update(stream, XQC_SEND_STREAM_ST_DATA_SENT);
                    }
                }
            }
        }
        packet_out->po_flag |= XQC_POF_STREAM_UNACK;
    }
}

void
xqc_conn_update_stream_stats_on_sent(xqc_connection_t *conn, xqc_send_ctl_t *ctl,
    xqc_packet_out_t *packet_out, xqc_usec_t now)
{
    xqc_stream_id_t stream_id;
    xqc_stream_t *stream[XQC_MAX_STREAM_FRAME_IN_PO] = {0};
    int stream_cnt = 0;
    int i, j;

    if (packet_out->po_frame_types & (XQC_FRAME_BIT_STREAM | XQC_FRAME_BIT_RESET_STREAM)) {
        for (i = 0; i < XQC_MAX_STREAM_FRAME_IN_PO; i++) {
            if (packet_out->po_stream_frames[i].ps_is_used == 0) {
                break;
            }
            stream_id = packet_out->po_stream_frames[i].ps_stream_id;
            stream[stream_cnt] = xqc_find_stream_by_id(stream_id, conn->streams_hash);
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
                    if (packet_out->po_flag & (XQC_POF_TLP | XQC_POF_LOST)) {
                        stream[stream_cnt]->stream_stats.retrans_pkt_cnt++;
                    }
                    stream[stream_cnt]->stream_stats.sent_pkt_cnt++;
                    stream[stream_cnt]->stream_stats.max_pto_backoff = xqc_max(stream[stream_cnt]->stream_stats.max_pto_backoff, ctl->ctl_pto_count);
                    stream_cnt++;
                }
            }
        }
    }
}

xqc_usec_t
xqc_conn_get_max_pto(xqc_connection_t *conn)
{
    xqc_path_ctx_t *path = conn->the_path;
    if (path && path->path_state == XQC_PATH_STATE_ACTIVE) {
        return xqc_send_ctl_calc_pto(path->path_send_ctl);
    }
    return 0;
}

uint32_t
xqc_conn_get_max_pto_backoff(xqc_connection_t *conn, uint8_t available_only)
{
    xqc_path_ctx_t *path = conn->the_path;
    if (path && path->path_state == XQC_PATH_STATE_ACTIVE) {
        return path->path_send_ctl->ctl_pto_count;
    }
    return 0;
}

xqc_usec_t
xqc_conn_get_min_srtt(xqc_connection_t *conn, xqc_bool_t available_only)
{
    xqc_path_ctx_t *path = conn->the_path;
    if (path && path->path_state == XQC_PATH_STATE_ACTIVE) {
        return path->path_send_ctl->ctl_srtt;
    }
    return XQC_MAX_UINT64_VALUE;
}

xqc_usec_t
xqc_conn_get_max_srtt(xqc_connection_t *conn)
{
    xqc_path_ctx_t *path = conn->the_path;
    if (path && path->path_state == XQC_PATH_STATE_ACTIVE) {
        return path->path_send_ctl->ctl_srtt;
    }
    return 0;
}

void xqc_conn_check_app_limit(xqc_connection_t *conn)
{
    xqc_path_ctx_t *path = conn->the_path;
    if (path && path->path_state == XQC_PATH_STATE_ACTIVE) {
        if (xqc_sample_check_app_limited(&path->path_send_ctl->sampler,
                                         path->path_send_ctl, conn->conn_send_queue))
        {
            xqc_pacing_on_app_limit(&path->path_send_ctl->ctl_pacing);
        }
    }
}

xqc_int_t
xqc_conn_send_path_challenge(xqc_connection_t *conn, xqc_path_ctx_t *path)
{
    xqc_int_t           ret = XQC_OK;
    xqc_packet_out_t   *packet_out;
    xqc_usec_t          now;
    ssize_t             sent;

    /* generate random data for path challenge, store it to validate path_response */
    ret = xqc_generate_path_challenge_data(conn, path);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_generate_path_challenge_data error|%d|", ret);
        return ret;
    }

    /* write path challenge frame & send immediately */

    packet_out = xqc_write_new_packet(conn, XQC_PTYPE_SHORT_HEADER);
    if (packet_out == NULL) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_write_new_packet error|");
        return -XQC_EWRITE_PKT;
    }

    ret = xqc_gen_path_challenge_frame(packet_out, path->path_challenge_data);
    if (ret < 0) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_gen_path_challenge_frame error|%d|", ret);
        goto end;
    }
    packet_out->po_used_size += ret;

    xqc_encode_packet_with_pn(conn, path, packet_out);

    /* record the send time of packet */
    now = xqc_monotonic_timestamp();
    packet_out->po_sent_time = now;

    sent = conn->transport_cbs.write_socket(packet_out->po_buf, packet_out->po_used_size,
                                                (struct sockaddr *)path->rebinding_addr,
                                                path->rebinding_addrlen,
                                                xqc_conn_get_user_data(conn));

    if (sent != packet_out->po_used_size) {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|write_socket error|conn:%p|pkt_num:%ui|size:%ud|sent:%z|pkt_type:%s|frame:%s|now:%ui|",
                conn, packet_out->po_pkt.pkt_num, packet_out->po_used_size, sent,
                xqc_pkt_type_2_str(packet_out->po_pkt.pkt_type),
                xqc_frame_type_2_str(conn->engine, packet_out->po_frame_types), now);
        ret = -XQC_ESOCKET;
        goto end;

    } else {
        xqc_log(conn->log, XQC_LOG_INFO,
                "|<==|conn:%p|pkt_num:%ui|size:%ud|sent:%z|pkt_type:%s|frame:%s|inflight:%ud|now:%ui|",
                conn, packet_out->po_pkt.pkt_num, packet_out->po_used_size, sent,
                xqc_pkt_type_2_str(packet_out->po_pkt.pkt_type),
                xqc_frame_type_2_str(conn->engine, packet_out->po_frame_types), path->path_send_ctl->ctl_bytes_in_flight, now);
    }

end:
    xqc_send_queue_remove_send(&packet_out->po_list);
    xqc_send_queue_insert_free(packet_out, &conn->conn_send_queue->sndq_free_packets, conn->conn_send_queue);
    return ret;
}

void
xqc_conn_buff_1rtt_packet(xqc_connection_t *conn, xqc_packet_out_t *po)
{
    xqc_send_queue_remove_send(&po->po_list);
    xqc_send_queue_insert_buff(&po->po_list, &conn->conn_send_queue->sndq_buff_1rtt_packets);
    if (!xqc_conn_is_dcid_done(conn)) {
        po->po_flag |= XQC_POF_DCID_NOT_DONE;
    }
}

void
xqc_conn_buff_1rtt_packets(xqc_connection_t *conn)
{
    xqc_packet_out_t *packet_out;
    xqc_list_head_t *pos, *next;
    xqc_list_for_each_safe(pos, next, &conn->conn_send_queue->sndq_send_packets) {
        packet_out = xqc_list_entry(pos, xqc_packet_out_t, po_list);
        if (packet_out->po_pkt.pkt_type == XQC_PTYPE_SHORT_HEADER) {
            xqc_send_queue_remove_send(&packet_out->po_list);
            xqc_send_queue_insert_buff(&packet_out->po_list, &conn->conn_send_queue->sndq_buff_1rtt_packets);
            if (!xqc_conn_is_dcid_done(conn)) {
                packet_out->po_flag |= XQC_POF_DCID_NOT_DONE;
            }
        }
    }
}

void
xqc_conn_write_buffed_1rtt_packets(xqc_connection_t *conn)
{
    if (xqc_conn_is_established(conn)) {
        xqc_send_queue_t *send_queue = conn->conn_send_queue;
        xqc_list_head_t *pos, *next;
        xqc_packet_out_t *packet_out;
        unsigned total = 0;
        xqc_list_for_each_safe(pos, next, &send_queue->sndq_buff_1rtt_packets) {
            packet_out = xqc_list_entry(pos, xqc_packet_out_t, po_list);
            xqc_send_queue_remove_buff(pos, send_queue);
            xqc_send_queue_insert_send(packet_out, &send_queue->sndq_send_packets, send_queue);
            if (packet_out->po_flag & XQC_POF_DCID_NOT_DONE) {
                xqc_short_packet_update_dcid(packet_out, conn->dcid_set.current_dcid);
            }
            ++total;
        }
    }
}

static xqc_packet_out_t *
xqc_conn_gen_ping(xqc_connection_t *conn)
{
    /* get pkt, which is inserted into sent list */
    xqc_packet_out_t *packet_out = xqc_write_new_packet(conn, XQC_PTYPE_NUM);
    if (packet_out == NULL) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_write_new_packet error|");
        return NULL;
    }

    /* write PING to pkt */
    xqc_int_t ret = xqc_gen_ping_frame(packet_out);
    if (ret < 0) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_gen_ping_frame error|");
        xqc_maybe_recycle_packet_out(packet_out, conn);
        return NULL;
    }

    packet_out->po_user_data = NULL;
    packet_out->po_used_size += ret;

    return packet_out;
}

static xqc_int_t
xqc_path_send_ping_to_probe(xqc_path_ctx_t *path)
{
    xqc_connection_t *conn = path->parent_conn;

    xqc_packet_out_t *packet_out = xqc_conn_gen_ping(conn);
    if (packet_out == NULL) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_write_new_packet error|");
        return -XQC_EWRITE_PKT;
    }

    /* put PING into probe list, which is not limited by amplification or congestion-control */
    xqc_send_queue_remove_send(&packet_out->po_list);
    xqc_send_queue_insert_probe(&packet_out->po_list, &conn->conn_send_queue->sndq_pto_probe_packets);

    return XQC_OK;
}

int
xqc_conn_send_probe_pkt(xqc_connection_t *c, xqc_path_ctx_t *path,
    xqc_packet_out_t *packet_out)
{
    if (packet_out->po_flag & XQC_POF_IN_FLIGHT) {
        c->detected_loss_cnt++;
    }

    xqc_send_ctl_decrease_inflight(c, packet_out);
    xqc_send_queue_copy_to_probe(packet_out, c->conn_send_queue, path);

    packet_out->po_flag |= XQC_POF_TLP;

    return 0;
}

void
xqc_path_send_one_or_two_ack_elicit_pkts(xqc_path_ctx_t *path)
{
    xqc_int_t               ret;
    xqc_connection_t       *c;
    xqc_packet_out_t       *packet_out;
    xqc_packet_out_t       *packet_out_last_sent;   /* for dup pto pkt */
    xqc_list_head_t        *pos, *next;
    xqc_list_head_t        *sndq;
    xqc_int_t               probe_num;

    c       = path->parent_conn;
    sndq    = &c->conn_send_queue->sndq_unacked_packets;

    /* on PTO xquic will try to send 2 ack-eliciting pkts at most. */
    probe_num        = XQC_CONN_PTO_PKT_CNT_MAX;

    packet_out_last_sent  = NULL;

    xqc_list_for_each_safe(pos, next, sndq) {
        packet_out = xqc_list_entry(pos, xqc_packet_out_t, po_list);

        if (xqc_send_ctl_indirectly_ack_or_drop_po(c, packet_out)) {
            continue;
        }

        if (XQC_IS_ACK_ELICITING(packet_out->po_frame_types)
            && (XQC_NEED_REPAIR(packet_out->po_frame_types)
                || (packet_out->po_flag & XQC_POF_NOTIFY)))
        {
            xqc_conn_send_probe_pkt(c, path, packet_out);
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
                xqc_conn_send_probe_pkt(c, path, packet_out_last_sent);
                probe_num--;
            }

        } else {
            /* if no packet was sent, try to send PING frame */
            while (probe_num > 0) {
                xqc_path_send_ping_to_probe(path);
                probe_num--;
            }
        }
    }
}

xqc_int_t
xqc_conn_send_ping_internal(xqc_connection_t *conn, void *ping_user_data, xqc_bool_t notify)
{
    xqc_int_t ret;
    xqc_path_ctx_t *path;
    xqc_list_head_t *pos, *next;
    xqc_bool_t has_ping;
    xqc_ping_record_t *pr;

    ret = XQC_OK;

    if (conn->conn_state >= XQC_CONN_STATE_CLOSING) {
        return ret;
    }

    pr = xqc_conn_create_ping_record(conn);

    if (pr == NULL) {
        return -XQC_EMALLOC;
    }

    has_ping = XQC_FALSE;

    ret = xqc_write_ping_to_packet(conn, NULL, ping_user_data, notify, pr);
    if (ret < 0) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|write ping error|");
    } else {
        has_ping = XQC_TRUE;
    }

    if (!has_ping) {
        xqc_conn_destroy_ping_record(pr);
        return ret;
    }

    return XQC_OK;
}

/* used by upper level, shall never be invoked in xquic */
xqc_int_t
xqc_conn_send_ping(xqc_engine_t *engine, const xqc_cid_t *cid, void *ping_user_data)
{
    xqc_connection_t *conn;
    xqc_int_t ret;
    conn = xqc_engine_conns_hash_find(engine, cid, 's');
    if (!conn) {
        xqc_log(engine->log, XQC_LOG_ERROR, "|can not find connection|cid:%s",
                xqc_scid_str(engine, cid));
        return -XQC_ECONN_NFOUND;
    }

    ret = xqc_conn_send_ping_internal(conn, ping_user_data, XQC_TRUE);
    if (ret) {
        return ret;
    }

    xqc_engine_remove_wakeup_queue(engine, conn);
    xqc_engine_add_active_queue(engine, conn);

    xqc_engine_conn_logic(engine, conn);
    return XQC_OK;
}

xqc_ping_record_t*
xqc_conn_create_ping_record(xqc_connection_t *conn)
{
    xqc_ping_record_t *pr = xqc_calloc(1, sizeof(xqc_ping_record_t));
    xqc_init_list_head(&pr->list);
    xqc_list_add_tail(&pr->list, &conn->ping_notification_list);
    return pr;
}

void
xqc_conn_destroy_ping_record(xqc_ping_record_t *pr)
{
    xqc_list_del_init(&pr->list);
    xqc_free(pr);
}
