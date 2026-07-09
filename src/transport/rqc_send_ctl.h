/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_SEND_CTL_H_INCLUDED_
#define _RQC_SEND_CTL_H_INCLUDED_

#include "src/transport/rqc_packet_out.h"
#include "src/transport/rqc_conn.h"
#include "src/transport/rqc_pacing.h"
#include "src/congestion_control/rqc_sample.h"
#include "src/transport/rqc_send_queue.h"
#include "src/transport/rqc_timer.h"
#include "src/transport/rqc_multipath.h"
#include <math.h>

#define RQC_kPacketThreshold                3
#define RQC_kTimeThresholdShift             3
#define RQC_kPersistentCongestionThreshold  3

#define RQC_CONSECUTIVE_PTO_THRESH          2
/*
 * Timer granularity.  This is a system-dependent value.
 * However, implementations SHOULD use a value no smaller than 1ms.
 */
#define RQC_kGranularity                    2

#define RQC_kInitialRtt_us                  250000

/* 2^n */
#define rqc_send_ctl_pow(n)                 (1 << n)
#define rqc_send_ctl_pow_x(x, n)            (fabs(x - 2) < 1e-7) ? rqc_send_ctl_pow(n) : pow(x, n)

#define RQC_DEFAULT_RECORD_INTERVAL         (100000)    /* 100ms record interval */
#define RQC_DEFAULT_RTT_CHANGE_THRESHOLD    (50 * 1000) /* 50ms */
#define RQC_DEFAULT_BW_CHANGE_THRESHOLD     (50)        /* percentage of bandwidth change */
typedef struct {
    rqc_usec_t  last_record_time;     /* last periodic record time */
    rqc_usec_t  last_rtt_time;        /* last time the rtt was drastically changed */
    rqc_usec_t  last_lost_time;       /* last time a packet loss was recorded */
    rqc_usec_t  last_bw_time;         /* last time the bandwidth was drastically changed */
    uint64_t    record_interval;      /* all types of records are recorded only once in the interval */
    uint64_t    rtt_change_threshold; /* threshold of rtt change */
    uint64_t    bw_change_threshold;  /* threshold of bandwidth change */
    uint64_t    last_lost_count;      /* number of packets lost in the last record */
    uint64_t    last_send_count;      /* number of packets sent in the last record */
}rqc_send_ctl_info_t;

typedef struct rqc_pn_ctl_s {

    rqc_packet_number_t         ctl_packet_number;

    /* maximum value of Largest Acknowledged in the packet_out that sent ACK and was ACKed
     * ensures that the ACK has been received by the peer,
     * so that the sender can no longer generate ACKs smaller than that value */
    rqc_packet_number_t         ctl_largest_acked_ack;

    /* largest packet number of the packets sent */
    rqc_packet_number_t         ctl_largest_sent;

    /* record received pkt number range in a list */
    rqc_recv_record_t           ctl_recv_record;

    /* record ack sent */
    rqc_ack_sent_record_t       ack_sent_record;

    /* fields are used for detecting optimistic ack attacks */
    /* we skip pn in [ctl_skipped_pn_low, ctl_skipped_pn_high] */
    rqc_packet_number_t         ctl_skipped_pn_low;
    rqc_packet_number_t         ctl_skipped_pn_high;
    rqc_usec_t                  ctl_next_skip_chance;

} rqc_pn_ctl_t;

typedef struct rqc_send_ctl_s {
    rqc_connection_t            *ctl_conn;
    rqc_path_ctx_t              *ctl_path;

    /* largest packet number of the acked packets in packet_out */
    rqc_packet_number_t         ctl_largest_acked;

    /* sending time of largest packet */
    rqc_usec_t                  ctl_largest_acked_sent_time;

    /* largest packet number of the received packets in packet_in */
    rqc_packet_number_t         ctl_largest_received;

    /* received time of largest packet */
    rqc_usec_t                  ctl_largest_recv_time;

    /* Ack-eliciting Packets received since last ack sent */
    uint32_t                    ctl_ack_eliciting_pkt;

    rqc_usec_t                  ctl_loss_time;

    rqc_usec_t                  ctl_last_inflight_pkt_sent_time;
    rqc_usec_t                  ctl_time_of_last_sent_ack_eliciting_packet;
    rqc_packet_number_t         ctl_last_sent_ack_eliciting_packet_number;
    rqc_usec_t                  ctl_srtt,
                                ctl_rttvar,
                                ctl_minrtt,
                                ctl_latest_rtt;
    rqc_usec_t                  ctl_first_rtt_sample_time; /* The time when the conn gets the first RTT sample. */

    /* data record - latest rtt */
    uint32_t                    ctl_update_latest_rtt_count;
    rqc_msec_t                  ctl_latest_rtt_sum;
    rqc_msec_t                  ctl_latest_rtt_square_sum;

    rqc_timer_manager_t         path_timer_manager;

    unsigned                    ctl_pto_count;

    unsigned                    ctl_send_count;
    unsigned                    ctl_lost_count;
    unsigned                    ctl_tlp_count;
    unsigned                    ctl_spurious_loss_count;

    /* record time for last three cwnd limitation and rtt mutation*/
    rqc_msec_t                  ctl_recent_cwnd_limitation_time[3];
    uint8_t                     ctl_cwndlim_update_idx;

    unsigned                    ctl_recv_count;

    uint32_t                    ctl_max_bytes_in_flight;
    uint8_t                     ctl_is_cwnd_limited;

    unsigned                    ctl_bytes_in_flight;
    uint32_t                    ctl_bytes_ack_eliciting_inflight;
    unsigned                    ctl_prior_bytes_in_flight;

    uint64_t                    ctl_bytes_send;
    uint64_t                    ctl_bytes_recv;

    /* only accounts for stream packets */
    uint64_t                    ctl_app_bytes_send;
    uint64_t                    ctl_app_bytes_recv;

    const
    rqc_cong_ctrl_callback_t    *ctl_cong_callback;
    void                        *ctl_cong;

    rqc_pacing_t                ctl_pacing;

    uint64_t                    ctl_prior_delivered;    /* the amount of data delivered in the last call of on_ack_received*/
    uint64_t                    ctl_delivered;          /* the amount of data that has been marked as sent at the current ack moment */
    uint64_t                    ctl_app_limited;        /* The index of the last transmitted packet marked as application-limited,
                                                         * or 0 if the connection is not currently application-limited. */
    rqc_usec_t                  ctl_delivered_time;     /* time when the current packet was acked */
    rqc_usec_t                  ctl_first_sent_time;    /* Send time of the first packet in the current sampling period */
    uint32_t                    ctl_lost_pkts_number;   /* how many packets have been lost so far */

    rqc_packet_number_t         ctl_reordering_packet_threshold;
    int32_t                     ctl_reordering_time_threshold_shift;

    rqc_sample_t                sampler;

    rqc_send_ctl_info_t         ctl_info;

    unsigned                    ctl_recent_send_count[2];
    unsigned                    ctl_recent_lost_count[2];
    rqc_usec_t                  ctl_recent_stats_timestamp;

    uint64_t                    ctl_ack_sent_cnt;

} rqc_send_ctl_t;

static inline rqc_usec_t
rqc_send_ctl_calc_pto(rqc_send_ctl_t *send_ctl)
{
    /*
     * Per RFC 9002 6.2.1, PTO must use the peer-reported max_ack_delay.
     * PNS-specific Initial/Handshake handling is done in
     * rqc_send_ctl_get_pto_time.
     */
    return send_ctl->ctl_srtt + rqc_max(4 * send_ctl->ctl_rttvar, RQC_kGranularity * 1000)
        + send_ctl->ctl_conn->remote_settings.max_ack_delay * 1000;
}

int rqc_send_ctl_indirectly_ack_or_drop_po(rqc_connection_t *conn, rqc_packet_out_t *po);

rqc_send_ctl_t *rqc_send_ctl_create(rqc_path_ctx_t *path);

void rqc_send_ctl_destroy(rqc_send_ctl_t *send_ctl);

void rqc_send_ctl_reset(rqc_send_ctl_t *send_ctl);

rqc_pn_ctl_t *rqc_pn_ctl_create(rqc_connection_t *conn);

void rqc_pn_ctl_destroy(rqc_pn_ctl_t *pn_ctl);

rqc_pn_ctl_t *rqc_get_pn_ctl(rqc_connection_t *conn, rqc_path_ctx_t *path);

int rqc_send_ctl_can_send(rqc_send_ctl_t *send_ctl, rqc_packet_out_t *packet_out, uint32_t schedule_bytes);

rqc_bool_t rqc_send_packet_cwnd_allows(rqc_send_ctl_t *send_ctl,
    rqc_packet_out_t *packet_out, uint32_t schedule_bytes, rqc_usec_t now);

rqc_bool_t rqc_send_packet_pacer_allows(rqc_send_ctl_t *send_ctl,
    rqc_packet_out_t *packet_out, uint32_t schedule_bytes, rqc_usec_t now);

rqc_bool_t rqc_send_packet_check_cc(rqc_send_ctl_t *send_ctl, rqc_packet_out_t *packet_out, uint32_t schedule_bytes, rqc_usec_t now);

void rqc_send_ctl_increase_inflight(rqc_connection_t *conn, rqc_packet_out_t *packet_out);

void rqc_send_ctl_decrease_inflight(rqc_connection_t *conn, rqc_packet_out_t *packet_out);

void rqc_send_ctl_on_packet_sent(rqc_send_ctl_t *send_ctl, rqc_pn_ctl_t *pn_ctl, rqc_packet_out_t *packet_out, rqc_usec_t now);

int rqc_send_ctl_on_ack_received (rqc_send_ctl_t *send_ctl, rqc_pn_ctl_t *pn_ctl, rqc_send_queue_t *send_queue, rqc_ack_info_t *const ack_info, rqc_usec_t ack_recv_time);

void rqc_send_ctl_on_dgram_received(rqc_send_ctl_t *send_ctl, size_t dgram_size);

void rqc_send_ctl_update_rtt(rqc_send_ctl_t *send_ctl, rqc_usec_t *latest_rtt, rqc_usec_t ack_delay);

void rqc_send_ctl_on_spurious_loss_detected(rqc_send_ctl_t *send_ctl,
    rqc_usec_t ack_recv_time, rqc_packet_number_t largest_ack,
    rqc_packet_number_t spurious_loss_pktnum, rqc_usec_t spurious_loss_sent_time);

void rqc_send_ctl_detect_lost(rqc_send_ctl_t *send_ctl, rqc_send_queue_t *send_queue, rqc_usec_t now);

rqc_bool_t rqc_send_ctl_in_persistent_congestion(rqc_send_ctl_t *send_ctl, rqc_packet_out_t *largest_lost, rqc_usec_t now);

void rqc_send_ctl_congestion_event(rqc_send_ctl_t *send_ctl, rqc_usec_t sent_time);

int rqc_send_ctl_in_recovery(rqc_send_ctl_t *send_ctl, rqc_usec_t sent_time);

int rqc_send_ctl_is_app_limited(rqc_send_ctl_t *send_ctl);

int rqc_send_ctl_is_cwnd_limited(rqc_send_ctl_t *send_ctl);

void rqc_send_ctl_cc_on_ack(rqc_send_ctl_t *send_ctl, rqc_packet_out_t *acked_packet, rqc_usec_t now);

void rqc_send_ctl_on_packet_acked(rqc_send_ctl_t *send_ctl, rqc_packet_out_t *acked_packet, rqc_usec_t now, int do_cc);

void rqc_send_queue_maybe_remove_unacked(rqc_packet_out_t *packet_out, rqc_send_queue_t *send_queue, rqc_path_ctx_t *path);

rqc_usec_t rqc_send_ctl_get_pto_time(rqc_send_ctl_t *send_ctl, rqc_usec_t now);

void rqc_send_ctl_set_loss_detection_timer(rqc_send_ctl_t *send_ctl);

rqc_usec_t rqc_send_ctl_get_earliest_loss_time(rqc_send_ctl_t *send_ctl);

rqc_usec_t rqc_send_ctl_get_srtt(rqc_send_ctl_t *send_ctl);

float rqc_send_ctl_get_retrans_rate(rqc_send_ctl_t *send_ctl);

float rqc_send_ctl_get_spurious_loss_rate(rqc_send_ctl_t *send_ctl);

rqc_bool_t rqc_send_ctl_ack_received(rqc_send_ctl_t *send_ctl);

rqc_packet_number_t rqc_send_ctl_get_lost_sent_pn(rqc_send_ctl_t *send_ctl);

rqc_packet_number_t rqc_send_ctl_get_pkt_num_gap(rqc_send_ctl_t *send_ctl, rqc_packet_number_t front, rqc_packet_number_t back);

/* bytes per second */
uint64_t rqc_send_ctl_get_est_bw(rqc_send_ctl_t *send_ctl);
uint64_t rqc_send_ctl_get_pacing_rate(rqc_send_ctl_t *send_ctl);

void rqc_send_ctl_set_next_pn_for_packet(rqc_connection_t *conn, rqc_pn_ctl_t *pn_ctl,
    rqc_packet_out_t *packet_out, rqc_usec_t current_time);

#endif /* _RQC_SEND_CTL_H_INCLUDED_ */
