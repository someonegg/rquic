/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_CONN_H_INCLUDED_
#define _RQC_CONN_H_INCLUDED_

#include <rquic/rquic.h>
#include <rquic/rquic_typedef.h>
#include "src/transport/rqc_cid.h"
#include "src/common/rqc_log.h"
#include "src/common/rqc_log_event_callback.h"
#include "src/common/rqc_common.h"
#include "src/transport/rqc_packet_in.h"
#include "src/transport/rqc_packet_out.h"
#include "src/transport/rqc_recv_record.h"
#include "src/transport/rqc_defs.h"
#include "src/transport/rqc_transport_params.h"
#include "src/transport/rqc_timer.h"
#include "src/transport/rqc_multipath.h"
#include "src/common/rqc_list.h"

#define RQC_FC_INIT_RTT 60000
#define RQC_MIN_RECV_WINDOW (63000) /* ~ 1MBps when RTT = 60ms */

/* maximum accumulated number of rqc_engine_packet_process */
#define RQC_MAX_PACKET_PROCESS_BATCH 100

#define RQC_MAX_RECV_WINDOW (16 * 1024 * 1024)

static const uint32_t MAX_RSP_CONN_CLOSE_CNT = 3;

#define RQC_CONN_CLOSE_MSG(conn, msg) do {          \
    if ((conn)->conn_close_msg == NULL) {           \
        (conn)->conn_close_msg = (msg);             \
    }                                               \
} while(0)                                          \

/* send CONNECTION_CLOSE with err */
#define RQC_CONN_ERR(conn, err) do {                \
    if ((conn)->conn_err == 0) {                    \
        (conn)->conn_err = (err);                   \
        RQC_CONN_CLOSE_MSG(conn, "local error");    \
        (conn)->conn_flag |= RQC_CONN_FLAG_ERROR;   \
        rqc_conn_closing(conn);                     \
        rqc_log((conn)->log, RQC_LOG_ERROR, "|conn:%p|err:0x%xi|%s|", (conn), (uint64_t)(err), rqc_conn_addr_str(conn)); \
    }                                               \
} while(0)                                          \

extern rqc_conn_settings_t internal_default_conn_settings;

/* !!WARNING: to add state, please update conn_state_2_str */
typedef enum {
    /* server */
    RQC_CONN_STATE_SERVER_INIT = 0,
    RQC_CONN_STATE_SERVER_HANDSHAKE = 1,
    /* client */
    RQC_CONN_STATE_CLIENT_INIT = 5,
    RQC_CONN_STATE_CLIENT_HANDSHAKE = 6,
    /* client & server */
    RQC_CONN_STATE_ESTABED = 10,
    RQC_CONN_STATE_CLOSING = 11,
    RQC_CONN_STATE_DRAINING = 12,
    RQC_CONN_STATE_CLOSED = 13,
    RQC_CONN_STATE_N = 14,
} rqc_conn_state_t;

#define RQC_CONN_IMMEDIATE_CLOSE_FLAGS (RQC_CONN_FLAG_ERROR)

/* !!WARNING: to add flag, please update conn_flag_2_str */
typedef enum {
    RQC_CONN_FLAG_WAIT_WAKEUP_SHIFT,
    RQC_CONN_FLAG_HANDSHAKE_SENT_SHIFT,
    RQC_CONN_FLAG_HANDSHAKE_RECVD_SHIFT,
    RQC_CONN_FLAG_HANDSHAKE_DONE_SHIFT,
    RQC_CONN_FLAG_TICKING_SHIFT,
    RQC_CONN_FLAG_ACK_HAS_GAP_SHIFT,
    RQC_CONN_FLAG_TIME_OUT_SHIFT,
    RQC_CONN_FLAG_ERROR_SHIFT,
    RQC_CONN_FLAG_DATA_BLOCKED_SHIFT,
    RQC_CONN_FLAG_DCID_DONE_SHIFT,
    RQC_CONN_FLAG_UPPER_CONN_EXIST_SHIFT,
    RQC_CONN_FLAG_NEED_RUN_SHIFT,
    RQC_CONN_FLAG_PING_SHIFT,
    RQC_CONN_FLAG_LINGER_CLOSING_SHIFT,
    RQC_CONN_FLAG_CONN_CLOSING_NOTIFY_SHIFT,
    RQC_CONN_FLAG_CONN_CLOSING_NOTIFIED_SHIFT,
    RQC_CONN_FLAG_VERSION_NEGOTIATION_SHIFT,
    RQC_CONN_FLAG_SHIFT_NUM,
} rqc_conn_flag_shift_t;

typedef enum {
    RQC_CONN_FLAG_WAIT_WAKEUP           = 1ULL << RQC_CONN_FLAG_WAIT_WAKEUP_SHIFT,
    RQC_CONN_FLAG_HANDSHAKE_SENT        = 1ULL << RQC_CONN_FLAG_HANDSHAKE_SENT_SHIFT,
    RQC_CONN_FLAG_HANDSHAKE_RECVD       = 1ULL << RQC_CONN_FLAG_HANDSHAKE_RECVD_SHIFT,
    RQC_CONN_FLAG_HANDSHAKE_DONE        = 1ULL << RQC_CONN_FLAG_HANDSHAKE_DONE_SHIFT,
    RQC_CONN_FLAG_TICKING               = 1ULL << RQC_CONN_FLAG_TICKING_SHIFT,
    RQC_CONN_FLAG_ACK_HAS_GAP           = 1ULL << RQC_CONN_FLAG_ACK_HAS_GAP_SHIFT,
    RQC_CONN_FLAG_TIME_OUT              = 1ULL << RQC_CONN_FLAG_TIME_OUT_SHIFT,
    RQC_CONN_FLAG_ERROR                 = 1ULL << RQC_CONN_FLAG_ERROR_SHIFT,
    RQC_CONN_FLAG_DATA_BLOCKED          = 1ULL << RQC_CONN_FLAG_DATA_BLOCKED_SHIFT,
    RQC_CONN_FLAG_DCID_DONE             = 1ULL << RQC_CONN_FLAG_DCID_DONE_SHIFT,
    RQC_CONN_FLAG_UPPER_CONN_EXIST      = 1ULL << RQC_CONN_FLAG_UPPER_CONN_EXIST_SHIFT,
    RQC_CONN_FLAG_NEED_RUN              = 1ULL << RQC_CONN_FLAG_NEED_RUN_SHIFT,
    RQC_CONN_FLAG_PING                  = 1ULL << RQC_CONN_FLAG_PING_SHIFT,
    RQC_CONN_FLAG_LINGER_CLOSING        = 1ULL << RQC_CONN_FLAG_LINGER_CLOSING_SHIFT,
    RQC_CONN_FLAG_CLOSING_NOTIFY        = 1ULL << RQC_CONN_FLAG_CONN_CLOSING_NOTIFY_SHIFT,
    RQC_CONN_FLAG_CLOSING_NOTIFIED      = 1ULL << RQC_CONN_FLAG_CONN_CLOSING_NOTIFIED_SHIFT,
    RQC_CONN_FLAG_VERSION_NEGOTIATION   = 1ULL << RQC_CONN_FLAG_VERSION_NEGOTIATION_SHIFT,
} rqc_conn_flag_t;

typedef struct {
    rqc_usec_t              max_idle_timeout;
    uint64_t                max_udp_payload_size;
    uint64_t                max_data;
    uint64_t                max_stream_data_bidi_local;
    uint64_t                max_stream_data_bidi_remote;
    uint64_t                max_stream_data_uni;
    uint64_t                max_streams_bidi;
    uint64_t                max_streams_uni;
    uint64_t                ack_delay_exponent;
    rqc_usec_t              max_ack_delay;
} rqc_trans_settings_t;

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
    rqc_usec_t              fc_last_window_update_time;
} rqc_conn_flow_ctl_t;

typedef struct rqc_ping_record_s {
    rqc_list_head_t list;
    uint8_t         notified;
    uint32_t        ref_cnt;
} rqc_ping_record_t;

struct rqc_connection_s {

    rqc_conn_settings_t             conn_settings;
    rqc_engine_t                   *engine;

    rqc_proto_version_t             version;
    /* set when client receives a non-VN package from server or receives a VN package and processes it */
    uint32_t                        discard_vn_flag;

    /* original destination connection id, RFC 9000, Section 7.3. */
    rqc_cid_t                       original_dcid;

    rqc_cid_set_t                   dcid_set;
    rqc_cid_set_t                   scid_set;

    unsigned char                   peer_addr[sizeof(struct sockaddr_in6)];
    socklen_t                       peer_addrlen;

    unsigned char                   local_addr[sizeof(struct sockaddr_in6)];
    socklen_t                       local_addrlen;

    char                            addr_str[2 * (RQC_MAX_CID_LEN + INET6_ADDRSTRLEN) + 10];
    size_t                          addr_str_len;

    uint32_t                        conn_close_count;
    uint32_t                        packet_need_process_count; /* rqc_engine_packet_process number */

    rqc_conn_state_t                conn_state;
    rqc_memory_pool_t              *conn_pool;

    rqc_id_hash_table_t            *streams_hash;
    rqc_id_hash_table_t            *passive_streams_hash;
    rqc_list_head_t                 conn_write_streams,
                                    conn_read_streams, /* rqc_stream_t */
                                    conn_closing_streams,
                                    conn_all_streams;
    uint64_t                        cur_stream_id_bidi_local;
    uint64_t                        cur_stream_id_uni_local;
    int64_t                         max_stream_id_bidi_remote;
    int64_t                         max_stream_id_uni_remote;

    rqc_trans_settings_t            local_settings;
    rqc_trans_settings_t            remote_settings;

    /* a bitmap to record if ACKs should be generated for path */
    uint64_t                        ack_flag;
    rqc_conn_flag_t                 conn_flag;
    rqc_conn_type_t                 conn_type;

    /* callback function and user_data to application layer */
    rqc_transport_callbacks_t       transport_cbs;
    void                           *user_data;      /* user_data for application layer */

    /* callback function and user_data to application-layer-protocol layer */
    char                           *alpn;
    size_t                          alpn_len;
    uint8_t                         self_proto_ext_buf[RQC_MAX_PROTO_EXT_LEN];
    rqc_proto_ext_t                 self_proto_ext;
    uint8_t                         peer_proto_ext_buf[RQC_MAX_PROTO_EXT_LEN];
    rqc_proto_ext_t                 peer_proto_ext;
    rqc_app_proto_callbacks_t       app_proto_cbs;
    void                           *proto_data;

    rqc_log_t                      *log;

    rqc_send_queue_t               *conn_send_queue;

    rqc_timer_manager_t             conn_timer_manager;

    rqc_usec_t                      last_ticked_time;
    rqc_usec_t                      next_tick_time;
    rqc_usec_t                      conn_create_time;
    rqc_usec_t                      handshake_recv_time;
    rqc_usec_t                      handshake_complete_time; /* record the time when the handshake ends */
    rqc_usec_t                      first_data_send_time;    /* record the time when the bidirectional stream first sent data */
    rqc_usec_t                      conn_close_recv_time;
    rqc_usec_t                      conn_close_send_time;
    rqc_usec_t                      conn_last_send_time;
    rqc_usec_t                      conn_last_recv_time;

    rqc_conn_flow_ctl_t             conn_flow_ctl;

    uint32_t                        wakeup_pq_index;

    uint64_t                        conn_err;
    const char                     *conn_close_msg;

    rqc_path_ctx_t                 *the_path;

    /* for qlog */
    uint32_t                        packet_dropped_count;

    /* for data callback mode, instead of write_socket/write_mmsg */
    rqc_conn_pkt_filter_callback_pt pkt_filter_cb;
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
    rqc_list_head_t                 ping_notification_list;

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
        rqc_pkt_type_t              pkt_types[3];
        rqc_frame_type_bit_t        pkt_frames[3];
        uint32_t                    pkt_size[3];
        uint32_t                    pkt_udp_size[3];
        int                         pkt_err[3];
        rqc_usec_t                  pkt_timestamp[3];
        rqc_packet_number_t         pkt_pn[3];
        uint8_t                     curr_index;
        uint32_t                    conn_rcvd_pkts;
        uint32_t                    conn_udp_pkts;
    } rcv_pkt_stats;

    struct {
        rqc_pkt_type_t              pkt_types[3];
        rqc_frame_type_bit_t        pkt_frames[3];
        uint32_t                    pkt_size[3];
        rqc_usec_t                  pkt_timestamp[3];
        rqc_packet_number_t         pkt_pn[3];
        uint8_t                     curr_index;
        uint32_t                    conn_sent_pkts;
    } snd_pkt_stats;
};

const char *rqc_conn_state_2_str(rqc_conn_state_t state);
const char *rqc_conn_flag_2_str(rqc_connection_t *conn, rqc_conn_flag_t conn_flag);

static inline rqc_int_t
rqc_conn_is_handshake_sent(rqc_connection_t *conn) {
    return ((conn->conn_flag & RQC_CONN_FLAG_HANDSHAKE_SENT) != 0);
}

static inline rqc_int_t
rqc_conn_is_handshake_recvd(rqc_connection_t *conn) {
    return ((conn->conn_flag & RQC_CONN_FLAG_HANDSHAKE_RECVD) != 0);
}

static inline rqc_int_t
rqc_conn_is_handshake_done(rqc_connection_t *conn) {
    return ((conn->conn_flag & RQC_CONN_FLAG_HANDSHAKE_DONE) != 0);
}

static inline rqc_int_t
rqc_conn_is_established(rqc_connection_t *conn) {
    return (conn->conn_state == RQC_CONN_STATE_ESTABED);
}

static inline rqc_int_t
rqc_conn_is_dcid_done(rqc_connection_t *conn) {
    return ((conn->conn_flag & RQC_CONN_FLAG_DCID_DONE) != 0);
}

static inline rqc_uint_t
rqc_conn_get_mss(rqc_connection_t *conn) {
    return conn->pkt_out_size + RQC_ACK_SPACE;
}

static inline void *
rqc_conn_get_user_data(rqc_connection_t *conn)
{
    if (NULL == conn) {
        return NULL;
    }
    return conn->user_data;
}

/* get idle timeout in milliseconds */
static inline rqc_msec_t
rqc_conn_get_idle_timeout(rqc_connection_t *conn)
{
    if (conn->conn_type == RQC_CONN_TYPE_SERVER && !rqc_conn_is_handshake_done(conn))
    {
        /* only server will limit idle timeout to init_idle_time_out before handshake done */
        return conn->conn_settings.init_idle_time_out == 0
            ? RQC_CONN_INITIAL_IDLE_TIMEOUT : conn->conn_settings.init_idle_time_out;

    } else {
        return conn->local_settings.max_idle_timeout == 0
            ? RQC_CONN_DEFAULT_IDLE_TIMEOUT : conn->local_settings.max_idle_timeout;
    }
}

static inline void
rqc_conn_closing(rqc_connection_t *conn)
{
    /* set closing notify flag, and do notify with CANNOT_DESTROY protection
       later during rqc_engine_process_conn */
    conn->conn_flag |= RQC_CONN_FLAG_CLOSING_NOTIFY;
}

static inline void
rqc_conn_closing_notify(rqc_connection_t *conn)
{
    if (conn->transport_cbs.conn_closing
        && (conn->conn_flag & RQC_CONN_FLAG_CLOSING_NOTIFY))
    {
        conn->conn_flag &= ~RQC_CONN_FLAG_CLOSING_NOTIFY;

        if (!(conn->conn_flag & RQC_CONN_FLAG_CLOSING_NOTIFIED)) {
            conn->conn_flag |= RQC_CONN_FLAG_CLOSING_NOTIFIED;
            conn->transport_cbs.conn_closing(conn, &conn->scid_set.user_scid, conn->conn_err, conn->user_data);
        }
    }
}

char *rqc_local_addr_str(rqc_engine_t *engine, const struct sockaddr *local_addr, socklen_t local_addrlen);
char *rqc_peer_addr_str(rqc_engine_t *engine, const struct sockaddr *peer_addr, socklen_t peer_addrlen);
char *rqc_conn_addr_str(rqc_connection_t *conn);
char *rqc_path_addr_str(rqc_path_ctx_t *path);

rqc_connection_t *rqc_conn_create(rqc_engine_t *engine, rqc_cid_t *dcid, rqc_cid_t *scid,
    const rqc_conn_settings_t *settings, void *user_data, rqc_conn_type_t type);

rqc_connection_t *rqc_conn_server_create(rqc_engine_t *engine, const struct sockaddr *local_addr,
    socklen_t local_addrlen, const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    rqc_cid_t *dcid, rqc_cid_t *scid, rqc_conn_settings_t *settings, void *user_data);

rqc_int_t rqc_conn_immediate_close(rqc_connection_t *conn);
void rqc_conn_destroy(rqc_connection_t *xc);

rqc_int_t rqc_conn_version_check(rqc_connection_t *conn, uint32_t version);
rqc_int_t rqc_conn_send_version_negotiation(rqc_connection_t *conn);

rqc_int_t rqc_conn_client_on_alpn(rqc_connection_t *conn, const unsigned char *alpn, size_t alpn_len);
rqc_int_t rqc_conn_server_on_alpn(rqc_connection_t *conn, const unsigned char *alpn, size_t alpn_len);

rqc_int_t rqc_conn_check_dcid(rqc_connection_t *conn, rqc_cid_t *dcid);

rqc_usec_t rqc_conn_next_wakeup_time(rqc_connection_t *conn);

void rqc_conn_timer_expire(rqc_connection_t *conn, rqc_usec_t now);

/* process an UDP datagram */
rqc_int_t rqc_conn_process_packet(rqc_connection_t *conn, const unsigned char *packet_in_buf,
    size_t packet_in_size, rqc_usec_t recv_time);

void rqc_conn_process_packet_recved_path(rqc_connection_t *conn, rqc_cid_t *scid,
    size_t packet_in_size, rqc_usec_t recv_time);

void rqc_conn_schedule_packets_to_paths(rqc_connection_t *conn);

void rqc_conn_transmit_pto_probe_packets(rqc_connection_t *conn);
void rqc_conn_retransmit_lost_packets(rqc_connection_t *conn);
void rqc_conn_send_packets(rqc_connection_t *conn);
void rqc_conn_retransmit_lost_packets_batch(rqc_connection_t *conn);
void rqc_conn_transmit_pto_probe_packets_batch(rqc_connection_t *conn);
void rqc_conn_send_packets_batch(rqc_connection_t *conn);

/* from send_ctl */
void rqc_conn_decrease_unacked_stream_ref(rqc_connection_t *conn, rqc_packet_out_t *packet_out);
void rqc_conn_increase_unacked_stream_ref(rqc_connection_t *conn, rqc_packet_out_t *packet_out);
void rqc_conn_update_stream_stats_on_sent(rqc_connection_t *conn, rqc_send_ctl_t *ctl,
    rqc_packet_out_t *packet_out, rqc_usec_t now);

/* PTO，用于连接级别的定时器触发:
 * - RQC_TIMER_LINGER_CLOSE
 * - RQC_TIMER_CONN_DRAINING
 * - RQC_TIMER_STREAM_CLOSE
 */
rqc_usec_t rqc_conn_get_max_pto(rqc_connection_t *conn);
uint32_t rqc_conn_get_max_pto_backoff(rqc_connection_t *conn, uint8_t available_only);

/* for cc */
rqc_usec_t rqc_conn_get_min_srtt(rqc_connection_t *conn, rqc_bool_t available_only);
rqc_usec_t rqc_conn_get_max_srtt(rqc_connection_t *conn);
void rqc_conn_check_app_limit(rqc_connection_t *conn);

rqc_int_t rqc_conn_send_path_challenge(rqc_connection_t *conn, rqc_path_ctx_t *path);

void rqc_conn_buff_1rtt_packet(rqc_connection_t *conn, rqc_packet_out_t *po);
void rqc_conn_buff_1rtt_packets(rqc_connection_t *conn);
void rqc_conn_write_buffed_1rtt_packets(rqc_connection_t *conn);

void rqc_path_send_one_or_two_ack_elicit_pkts(rqc_path_ctx_t *path);

rqc_int_t rqc_conn_send_ping_internal(rqc_connection_t *conn, void *ping_user_data, rqc_bool_t notify);
rqc_ping_record_t* rqc_conn_create_ping_record(rqc_connection_t *conn);
void rqc_conn_destroy_ping_record(rqc_ping_record_t *pr);

rqc_int_t rqc_conn_send_handshake(rqc_connection_t *conn);
rqc_int_t rqc_conn_process_handshake(rqc_connection_t *conn,
    const unsigned char *alpn, size_t alpn_len,
    const unsigned char *tp, size_t tp_len,
    const unsigned char *proto_ext, size_t proto_ext_len);
void rqc_conn_on_handshake_acked(rqc_connection_t *conn);

#endif /* _RQC_CONN_H_INCLUDED_ */
