/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _XQC_CONN_H_INCLUDED_
#define _XQC_CONN_H_INCLUDED_

#include <xquic/xquic.h>
#include <xquic/xquic_typedef.h>
#include "src/transport/xqc_cid.h"
#include "src/common/xqc_log.h"
#include "src/common/xqc_log_event_callback.h"
#include "src/common/xqc_common.h"
#include "src/transport/xqc_packet_in.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_recv_record.h"
#include "src/transport/xqc_defs.h"
#include "src/transport/xqc_transport_params.h"
#include "src/transport/xqc_timer.h"
#include "src/transport/xqc_multipath.h"
#include "src/common/xqc_list.h"

#define XQC_FC_INIT_RTT 60000
#define XQC_MIN_RECV_WINDOW (63000) /* ~ 1MBps when RTT = 60ms */

/* maximum accumulated number of xqc_engine_packet_process */
#define XQC_MAX_PACKET_PROCESS_BATCH 100

#define XQC_MAX_RECV_WINDOW (16 * 1024 * 1024)

static const uint32_t MAX_RSP_CONN_CLOSE_CNT = 3;

/* for debugging, will be deleted later */
#ifdef DEBUG_PRINT
#define XQC_DEBUG_PRINT printf("%s:%d (%s)\n", __FILE__, __LINE__, __FUNCTION__);
#else
#define XQC_DEBUG_PRINT
#endif

#define XQC_CONN_CLOSE_MSG(conn, msg) do {          \
    if ((conn)->conn_close_msg == NULL) {           \
        (conn)->conn_close_msg = (msg);             \
    }                                               \
} while(0)                                          \

/* send CONNECTION_CLOSE with err */
#define XQC_CONN_ERR(conn, err) do {                \
    if ((conn)->conn_err == 0) {                    \
        (conn)->conn_err = (err);                   \
        XQC_CONN_CLOSE_MSG(conn, "local error");    \
        (conn)->conn_flag |= XQC_CONN_FLAG_ERROR;   \
        xqc_conn_closing(conn);                     \
        xqc_log((conn)->log, XQC_LOG_ERROR, "|conn:%p|err:0x%xi|%s|", (conn), (uint64_t)(err), xqc_conn_addr_str(conn)); \
    }                                               \
} while(0)                                          \

extern xqc_conn_settings_t internal_default_conn_settings;

/* !!WARNING: to add state, please update conn_state_2_str */
typedef enum {
    /* server */
    XQC_CONN_STATE_SERVER_INIT = 0,
    XQC_CONN_STATE_SERVER_INITIAL_RECVD = 1,
    XQC_CONN_STATE_SERVER_INITIAL_SENT = 2,
    /* client */
    XQC_CONN_STATE_CLIENT_INIT = 5,
    XQC_CONN_STATE_CLIENT_INITIAL_SENT = 6,
    XQC_CONN_STATE_CLIENT_INITIAL_RECVD = 7,
    /* client & server */
    XQC_CONN_STATE_ESTABED = 10,
    XQC_CONN_STATE_CLOSING = 11,
    XQC_CONN_STATE_DRAINING = 12,
    XQC_CONN_STATE_CLOSED = 13,
    XQC_CONN_STATE_N = 14,
} xqc_conn_state_t;

#define XQC_CONN_IMMEDIATE_CLOSE_FLAGS (XQC_CONN_FLAG_ERROR)

/* !!WARNING: to add flag, please update conn_flag_2_str */
typedef enum {
    XQC_CONN_FLAG_WAIT_WAKEUP_SHIFT,
    XQC_CONN_FLAG_HANDSHAKE_RECVD_SHIFT,
    XQC_CONN_FLAG_HANDSHAKE_DONE_SHIFT,
    XQC_CONN_FLAG_TICKING_SHIFT,
    XQC_CONN_FLAG_ACK_HAS_GAP_SHIFT,
    XQC_CONN_FLAG_TIME_OUT_SHIFT,
    XQC_CONN_FLAG_ERROR_SHIFT,
    XQC_CONN_FLAG_DATA_BLOCKED_SHIFT,
    XQC_CONN_FLAG_DCID_DONE_SHIFT,
    XQC_CONN_FLAG_UPPER_CONN_EXIST_SHIFT,
    XQC_CONN_FLAG_NEED_RUN_SHIFT,
    XQC_CONN_FLAG_PING_SHIFT,
    XQC_CONN_FLAG_LINGER_CLOSING_SHIFT,
    XQC_CONN_FLAG_CONN_CLOSING_NOTIFY_SHIFT,
    XQC_CONN_FLAG_CONN_CLOSING_NOTIFIED_SHIFT,
    XQC_CONN_FLAG_VERSION_NEGOTIATION_SHIFT,
    XQC_CONN_FLAG_SHIFT_NUM,
} xqc_conn_flag_shift_t;

typedef enum {
    XQC_CONN_FLAG_WAIT_WAKEUP           = 1ULL << XQC_CONN_FLAG_WAIT_WAKEUP_SHIFT,
    XQC_CONN_FLAG_HANDSHAKE_RECVD       = 1ULL << XQC_CONN_FLAG_HANDSHAKE_RECVD_SHIFT,
    XQC_CONN_FLAG_HANDSHAKE_DONE        = 1ULL << XQC_CONN_FLAG_HANDSHAKE_DONE_SHIFT,
    XQC_CONN_FLAG_TICKING               = 1ULL << XQC_CONN_FLAG_TICKING_SHIFT,
    XQC_CONN_FLAG_ACK_HAS_GAP           = 1ULL << XQC_CONN_FLAG_ACK_HAS_GAP_SHIFT,
    XQC_CONN_FLAG_TIME_OUT              = 1ULL << XQC_CONN_FLAG_TIME_OUT_SHIFT,
    XQC_CONN_FLAG_ERROR                 = 1ULL << XQC_CONN_FLAG_ERROR_SHIFT,
    XQC_CONN_FLAG_DATA_BLOCKED          = 1ULL << XQC_CONN_FLAG_DATA_BLOCKED_SHIFT,
    XQC_CONN_FLAG_DCID_DONE             = 1ULL << XQC_CONN_FLAG_DCID_DONE_SHIFT,
    XQC_CONN_FLAG_UPPER_CONN_EXIST      = 1ULL << XQC_CONN_FLAG_UPPER_CONN_EXIST_SHIFT,
    XQC_CONN_FLAG_NEED_RUN              = 1ULL << XQC_CONN_FLAG_NEED_RUN_SHIFT,
    XQC_CONN_FLAG_PING                  = 1ULL << XQC_CONN_FLAG_PING_SHIFT,
    XQC_CONN_FLAG_LINGER_CLOSING        = 1ULL << XQC_CONN_FLAG_LINGER_CLOSING_SHIFT,
    XQC_CONN_FLAG_CLOSING_NOTIFY        = 1ULL << XQC_CONN_FLAG_CONN_CLOSING_NOTIFY_SHIFT,
    XQC_CONN_FLAG_CLOSING_NOTIFIED      = 1ULL << XQC_CONN_FLAG_CONN_CLOSING_NOTIFIED_SHIFT,
    XQC_CONN_FLAG_VERSION_NEGOTIATION   = 1ULL << XQC_CONN_FLAG_VERSION_NEGOTIATION_SHIFT,
} xqc_conn_flag_t;

typedef struct {
    xqc_usec_t              max_idle_timeout;
    uint64_t                max_udp_payload_size;
    uint64_t                max_data;
    uint64_t                max_stream_data_bidi_local;
    uint64_t                max_stream_data_bidi_remote;
    uint64_t                max_stream_data_uni;
    uint64_t                max_streams_bidi;
    uint64_t                max_streams_uni;
    uint64_t                ack_delay_exponent;
    xqc_usec_t              max_ack_delay;
} xqc_trans_settings_t;

typedef struct {
    /* flow control limit */
    uint64_t                fc_max_data_can_send;
    uint64_t                fc_data_sent;
    uint64_t                fc_max_data_can_recv;
    uint64_t                fc_data_recved;
    uint64_t                fc_data_read;

    uint64_t                fc_max_streams_bidi_can_send;
    uint64_t                fc_max_streams_bidi_can_recv;
    uint64_t                fc_max_streams_uni_can_send;
    uint64_t                fc_max_streams_uni_can_recv;

    uint64_t                fc_recv_windows_size;
    xqc_usec_t              fc_last_window_update_time;
} xqc_conn_flow_ctl_t;

typedef struct xqc_ping_record_s {
    xqc_list_head_t list;
    uint8_t         notified;
    uint32_t        ref_cnt;
} xqc_ping_record_t;

struct xqc_connection_s {

    xqc_conn_settings_t             conn_settings;
    xqc_engine_t                   *engine;

    xqc_proto_version_t             version;
    /* set when client receives a non-VN package from server or receives a VN package and processes it */
    uint32_t                        discard_vn_flag;

    /* original destination connection id, RFC 9000, Section 7.3. */
    xqc_cid_t                       original_dcid;

    xqc_cid_set_t                   dcid_set;
    xqc_cid_set_t                   scid_set;

    unsigned char                   peer_addr[sizeof(struct sockaddr_in6)];
    socklen_t                       peer_addrlen;

    unsigned char                   local_addr[sizeof(struct sockaddr_in6)];
    socklen_t                       local_addrlen;

    char                            addr_str[2 * (XQC_MAX_CID_LEN + INET6_ADDRSTRLEN) + 10];
    size_t                          addr_str_len;

    uint32_t                        conn_close_count;
    uint32_t                        packet_need_process_count; /* xqc_engine_packet_process number */

    xqc_conn_state_t                conn_state;
    xqc_memory_pool_t              *conn_pool;

    xqc_id_hash_table_t            *streams_hash;
    xqc_id_hash_table_t            *passive_streams_hash;
    xqc_list_head_t                 conn_write_streams,
                                    conn_read_streams, /* xqc_stream_t */
                                    conn_closing_streams,
                                    conn_all_streams;
    uint64_t                        cur_stream_id_bidi_local;
    uint64_t                        cur_stream_id_uni_local;
    int64_t                         max_stream_id_bidi_remote;
    int64_t                         max_stream_id_uni_remote;

    xqc_trans_settings_t            local_settings;
    xqc_trans_settings_t            remote_settings;

    /* a bitmap to record if ACKs should be generated for path */
    uint64_t                        ack_flag;
    xqc_conn_flag_t                 conn_flag;
    xqc_conn_type_t                 conn_type;

    /* callback function and user_data to application layer */
    xqc_transport_callbacks_t       transport_cbs;
    void                           *user_data;      /* user_data for application layer */

    /* callback function and user_data to application-layer-protocol layer */
    char                           *alpn;
    size_t                          alpn_len;
    xqc_app_proto_callbacks_t       app_proto_cbs;
    void                           *proto_data;

    xqc_log_t                      *log;

    xqc_send_queue_t               *conn_send_queue;

    xqc_timer_manager_t             conn_timer_manager;

    xqc_usec_t                      last_ticked_time;
    xqc_usec_t                      next_tick_time;
    xqc_usec_t                      conn_create_time;
    xqc_usec_t                      handshake_recv_time;
    xqc_usec_t                      handshake_complete_time; /* record the time when the handshake ends */
    xqc_usec_t                      first_data_send_time;    /* record the time when the bidirectional stream first sent data */
    xqc_usec_t                      conn_close_recv_time;
    xqc_usec_t                      conn_close_send_time;
    xqc_usec_t                      conn_last_send_time;
    xqc_usec_t                      conn_last_recv_time;

    xqc_conn_flow_ctl_t             conn_flow_ctl;

    uint32_t                        wakeup_pq_index;

    uint64_t                        conn_err;
    const char                     *conn_close_msg;

    xqc_path_ctx_t                 *the_path;

    /* for qlog */
    uint32_t                        packet_dropped_count;

    /* for data callback mode, instead of write_socket/write_mmsg */
    xqc_conn_pkt_filter_callback_pt pkt_filter_cb;
    void                           *pkt_filter_cb_user_data;

    struct {
        uint64_t                    send_bytes;
        uint64_t                    recv_bytes;
    } stream_stats;

    /* min pkt_out_size across all paths */
    size_t                          pkt_out_size;
    size_t                          max_pkt_out_size;
    size_t                          max_acked_po_size;

    /* pending ping notification */
    xqc_list_head_t                 ping_notification_list;

    /* cc blocking stats */
    uint32_t                        sched_cc_blocked;
    uint32_t                        send_cc_blocked;

    /* internal loss detection stats */
    uint32_t                        detected_loss_cnt;

    /* max consecutive PTO cnt among all paths */
    uint16_t                        max_pto_cnt;
    uint32_t                        finished_streams;
    uint32_t                        cli_bidi_streams;
    uint32_t                        svr_bidi_streams;

    /* receved pkts stats */
    struct {
        xqc_pkt_type_t              pkt_types[3];
        xqc_frame_type_bit_t        pkt_frames[3];
        uint32_t                    pkt_size[3];
        uint32_t                    pkt_udp_size[3];
        int                         pkt_err[3];
        xqc_usec_t                  pkt_timestamp[3];
        xqc_packet_number_t         pkt_pn[3];
        uint8_t                     curr_index;
        uint32_t                    conn_rcvd_pkts;
        uint32_t                    conn_udp_pkts;
    } rcv_pkt_stats;

    struct {
        xqc_pkt_type_t              pkt_types[3];
        xqc_frame_type_bit_t        pkt_frames[3];
        uint32_t                    pkt_size[3];
        xqc_usec_t                  pkt_timestamp[3];
        xqc_packet_number_t         pkt_pn[3];
        uint8_t                     curr_index;
        uint32_t                    conn_sent_pkts;
    } snd_pkt_stats;
};

const char *xqc_conn_state_2_str(xqc_conn_state_t state);
const char *xqc_conn_flag_2_str(xqc_connection_t *conn, xqc_conn_flag_t conn_flag);

static inline xqc_int_t
xqc_conn_is_handshake_done(xqc_connection_t *conn) {
    return ((conn->conn_flag & XQC_CONN_FLAG_HANDSHAKE_DONE) != 0);
}

static inline xqc_int_t
xqc_conn_is_established(xqc_connection_t *conn) {
    return (conn->conn_state == XQC_CONN_STATE_ESTABED);
}

static inline xqc_int_t
xqc_conn_is_dcid_done(xqc_connection_t *conn) {
    return ((conn->conn_flag & XQC_CONN_FLAG_DCID_DONE) != 0);
}

static inline xqc_uint_t
xqc_conn_get_mss(xqc_connection_t *conn) {
    return conn->pkt_out_size + XQC_ACK_SPACE;
}

static inline void *
xqc_conn_get_user_data(xqc_connection_t *c)
{
    if (NULL == c) {
        return NULL;
    }
    return c->user_data;
}

/* get idle timeout in milliseconds */
static inline xqc_msec_t
xqc_conn_get_idle_timeout(xqc_connection_t *conn)
{
    if (conn->conn_type == XQC_CONN_TYPE_SERVER && !xqc_conn_is_handshake_done(conn))
    {
        /* only server will limit idle timeout to init_idle_time_out before handshake done */
        return conn->conn_settings.init_idle_time_out == 0
            ? XQC_CONN_INITIAL_IDLE_TIMEOUT : conn->conn_settings.init_idle_time_out;

    } else {
        return conn->local_settings.max_idle_timeout == 0
            ? XQC_CONN_DEFAULT_IDLE_TIMEOUT : conn->local_settings.max_idle_timeout;
    }
}

static inline void
xqc_conn_closing(xqc_connection_t *conn)
{
    /* set closing notify flag, and do notify with CANNOT_DESTROY protection
       later during xqc_engine_process_conn */
    conn->conn_flag |= XQC_CONN_FLAG_CLOSING_NOTIFY;
}

static inline void
xqc_conn_closing_notify(xqc_connection_t *conn)
{
    if (conn->transport_cbs.conn_closing
        && (conn->conn_flag & XQC_CONN_FLAG_CLOSING_NOTIFY))
    {
        conn->conn_flag &= ~XQC_CONN_FLAG_CLOSING_NOTIFY;

        if (!(conn->conn_flag & XQC_CONN_FLAG_CLOSING_NOTIFIED)) {
            conn->conn_flag |= XQC_CONN_FLAG_CLOSING_NOTIFIED;
            conn->transport_cbs.conn_closing(conn, &conn->scid_set.user_scid, conn->conn_err, conn->user_data);
        }
    }
}

char *xqc_local_addr_str(xqc_engine_t *engine, const struct sockaddr *local_addr, socklen_t local_addrlen);
char *xqc_peer_addr_str(xqc_engine_t *engine, const struct sockaddr *peer_addr, socklen_t peer_addrlen);
char *xqc_conn_addr_str(xqc_connection_t *conn);
char *xqc_path_addr_str(xqc_path_ctx_t *path);

xqc_connection_t *xqc_conn_create(xqc_engine_t *engine, xqc_cid_t *dcid, xqc_cid_t *scid,
    const xqc_conn_settings_t *settings, void *user_data, xqc_conn_type_t type);

xqc_connection_t *xqc_conn_server_create(xqc_engine_t *engine, const struct sockaddr *local_addr,
    socklen_t local_addrlen, const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    xqc_cid_t *dcid, xqc_cid_t *scid, xqc_conn_settings_t *settings, void *user_data);

xqc_int_t xqc_conn_immediate_close(xqc_connection_t *conn);
void xqc_conn_destroy(xqc_connection_t *xc);

xqc_int_t xqc_conn_version_check(xqc_connection_t *c, uint32_t version);
xqc_int_t xqc_conn_send_version_negotiation(xqc_connection_t *c);

xqc_int_t xqc_conn_client_on_alpn(xqc_connection_t *conn, const unsigned char *alpn, size_t alpn_len);
xqc_int_t xqc_conn_server_on_alpn(xqc_connection_t *conn, const unsigned char *alpn, size_t alpn_len);

xqc_int_t xqc_conn_check_dcid(xqc_connection_t *conn, xqc_cid_t *dcid);

xqc_usec_t xqc_conn_next_wakeup_time(xqc_connection_t *conn);

void xqc_conn_timer_expire(xqc_connection_t *conn, xqc_usec_t now);

/* process an UDP datagram */
xqc_int_t xqc_conn_process_packet(xqc_connection_t *c, const unsigned char *packet_in_buf,
    size_t packet_in_size, xqc_usec_t recv_time);

void xqc_conn_process_packet_recved_path(xqc_connection_t *conn, xqc_cid_t *scid,
    size_t packet_in_size, xqc_usec_t recv_time);

void xqc_conn_schedule_packets_to_paths(xqc_connection_t *conn);

void xqc_conn_transmit_pto_probe_packets(xqc_connection_t *conn);
void xqc_conn_retransmit_lost_packets(xqc_connection_t *conn);
void xqc_conn_send_packets(xqc_connection_t *conn);
void xqc_conn_retransmit_lost_packets_batch(xqc_connection_t *conn);
void xqc_conn_transmit_pto_probe_packets_batch(xqc_connection_t *conn);
void xqc_conn_send_packets_batch(xqc_connection_t *conn);

/* from send_ctl */
void xqc_conn_decrease_unacked_stream_ref(xqc_connection_t *conn, xqc_packet_out_t *packet_out);
void xqc_conn_increase_unacked_stream_ref(xqc_connection_t *conn, xqc_packet_out_t *packet_out);
void xqc_conn_update_stream_stats_on_sent(xqc_connection_t *conn, xqc_send_ctl_t *ctl,
    xqc_packet_out_t *packet_out, xqc_usec_t now);

/* PTO，用于连接级别的定时器触发:
 * - XQC_TIMER_LINGER_CLOSE
 * - XQC_TIMER_CONN_DRAINING
 * - XQC_TIMER_STREAM_CLOSE
 */
xqc_usec_t xqc_conn_get_max_pto(xqc_connection_t *conn);
uint32_t xqc_conn_get_max_pto_backoff(xqc_connection_t *conn, uint8_t available_only);

/* for cc */
xqc_usec_t xqc_conn_get_min_srtt(xqc_connection_t *conn, xqc_bool_t available_only);
xqc_usec_t xqc_conn_get_max_srtt(xqc_connection_t *conn);
void xqc_conn_check_app_limit(xqc_connection_t *conn);

xqc_int_t xqc_conn_send_path_challenge(xqc_connection_t *conn, xqc_path_ctx_t *path);

void xqc_conn_buff_1rtt_packet(xqc_connection_t *conn, xqc_packet_out_t *po);
void xqc_conn_buff_1rtt_packets(xqc_connection_t *conn);
void xqc_conn_write_buffed_1rtt_packets(xqc_connection_t *conn);

void xqc_path_send_one_or_two_ack_elicit_pkts(xqc_path_ctx_t *path);

xqc_int_t xqc_conn_send_ping_internal(xqc_connection_t *conn, void *ping_user_data, xqc_bool_t notify);
xqc_ping_record_t* xqc_conn_create_ping_record(xqc_connection_t *conn);
void xqc_conn_destroy_ping_record(xqc_ping_record_t *pr);

#endif /* _XQC_CONN_H_INCLUDED_ */
