/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "src/congestion_control/rqc_bbr.h"
#include "src/congestion_control/rqc_bbr2.h"
#include "src/congestion_control/rqc_bbr_common.h"
#include "src/transport/rqc_engine.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/transport/rqc_pacing.h"
#include "src/transport/rqc_packet.h"
#include "src/transport/rqc_packet_out.h"
#include "src/transport/rqc_frame.h"
#include "src/transport/rqc_conn.h"
#include "src/transport/rqc_stream.h"
#include "src/common/rqc_memory_pool.h"
#include "src/common/rqc_algorithm.h"
#include "src/congestion_control/rqc_sample.h"
#include "src/transport/rqc_pacing.h"
#include "src/transport/rqc_utils.h"

int
rqc_send_ctl_indirectly_ack_or_drop_po(rqc_connection_t *conn, rqc_packet_out_t *packet_out)
{
    rqc_path_ctx_t *path = conn->the_path;
    if (path == NULL) {
        return RQC_FALSE;
    }

    rqc_send_ctl_t *send_ctl = path->path_send_ctl;
    rqc_send_queue_t *send_queue = conn->conn_send_queue;

    if (packet_out->po_acked
        || (packet_out->po_origin && packet_out->po_origin->po_acked))
    {
        if (packet_out->po_origin && packet_out->po_origin->po_acked) {
            /* We should not do congestion control here. */
            rqc_send_ctl_on_packet_acked(send_ctl, packet_out, 0, 0);
        }
        rqc_send_queue_maybe_remove_unacked(packet_out, send_queue, path);
        return RQC_TRUE;
    }

    return RQC_FALSE;
}

rqc_send_ctl_t *
rqc_send_ctl_create(rqc_path_ctx_t *path)
{
    rqc_connection_t *conn = path->parent_conn;

    rqc_send_ctl_t *send_ctl;
    send_ctl = rqc_pcalloc(conn->conn_pool, sizeof(rqc_send_ctl_t));
    if (send_ctl == NULL) {
        return NULL;
    }

    send_ctl->ctl_path = path;
    send_ctl->ctl_conn = conn;

    send_ctl->ctl_pto_count = 0;
    send_ctl->ctl_minrtt = RQC_MAX_UINT32_VALUE;
    send_ctl->ctl_srtt = conn->conn_settings.initial_rtt;
    send_ctl->ctl_rttvar = send_ctl->ctl_srtt / 2;
    send_ctl->ctl_latest_rtt = 0;
    send_ctl->ctl_max_bytes_in_flight = 0;
    send_ctl->ctl_reordering_packet_threshold = conn->conn_settings.loss_detection_pkt_thresh;
    send_ctl->ctl_reordering_time_threshold_shift = RQC_kTimeThresholdShift;
    send_ctl->ctl_first_rtt_sample_time = 0;

    send_ctl->ctl_largest_acked = RQC_MAX_UINT64_VALUE;
    send_ctl->ctl_largest_received = RQC_MAX_UINT64_VALUE;
    send_ctl->ctl_time_of_last_sent_ack_eliciting_packet = 0;
    send_ctl->ctl_loss_time = 0;

    memset(&send_ctl->ctl_largest_acked_sent_time, 0,
           sizeof(send_ctl->ctl_largest_acked_sent_time));

    memset(&send_ctl->ctl_largest_recv_time, 0,
           sizeof(send_ctl->ctl_largest_recv_time));

    send_ctl->ctl_is_cwnd_limited = 0;
    send_ctl->ctl_delivered = 0;
    send_ctl->ctl_lost_pkts_number = 0;
    send_ctl->ctl_last_inflight_pkt_sent_time = 0;

    rqc_timer_init(&send_ctl->path_timer_manager, conn->log, send_ctl);

    if (conn->conn_settings.cong_ctrl_callback.rqc_cong_ctl_init_bbr) {
        send_ctl->ctl_cong_callback = &conn->conn_settings.cong_ctrl_callback;

    } else if (conn->conn_settings.cong_ctrl_callback.rqc_cong_ctl_init) {
        send_ctl->ctl_cong_callback = &conn->conn_settings.cong_ctrl_callback;

    } else {
        send_ctl->ctl_cong_callback = &rqc_bbr_cb;
    }
    send_ctl->ctl_cong = rqc_pcalloc(conn->conn_pool, send_ctl->ctl_cong_callback->rqc_cong_ctl_size());

    if (conn->conn_settings.cong_ctrl_callback.rqc_cong_ctl_init_bbr) {
        send_ctl->ctl_cong_callback->rqc_cong_ctl_init_bbr(send_ctl->ctl_cong,
                                                           &send_ctl->sampler, conn->conn_settings.cc_params);

    } else {
        send_ctl->ctl_cong_callback->rqc_cong_ctl_init(send_ctl->ctl_cong, send_ctl, conn->conn_settings.cc_params);
    }

    rqc_pacing_init(&send_ctl->ctl_pacing, conn->conn_settings.pacing_on, send_ctl);

    send_ctl->ctl_info.record_interval = RQC_DEFAULT_RECORD_INTERVAL;
    send_ctl->ctl_info.last_record_time = 0;
    send_ctl->ctl_info.last_rtt_time = 0;
    send_ctl->ctl_info.last_lost_time = 0;
    send_ctl->ctl_info.last_bw_time = 0;
    send_ctl->ctl_info.rtt_change_threshold = RQC_DEFAULT_RTT_CHANGE_THRESHOLD;
    send_ctl->ctl_info.bw_change_threshold = RQC_DEFAULT_BW_CHANGE_THRESHOLD;

    send_ctl->sampler.send_ctl = send_ctl;

    rqc_log_event(conn->log, REC_PARAMETERS_SET, send_ctl, RQC_kGranularity, conn->conn_settings.cc_params);
    return send_ctl;
}

void
rqc_send_ctl_destroy(rqc_send_ctl_t *send_ctl)
{
    send_ctl->ctl_bytes_ack_eliciting_inflight = 0;
    send_ctl->ctl_bytes_in_flight = 0;
}

void
rqc_send_ctl_reset(rqc_send_ctl_t *send_ctl)
{
    rqc_connection_t *conn = send_ctl->ctl_conn;

    send_ctl->ctl_pto_count = 0;
    send_ctl->ctl_minrtt = RQC_MAX_UINT32_VALUE;
    send_ctl->ctl_srtt = conn->conn_settings.initial_rtt;
    send_ctl->ctl_rttvar = send_ctl->ctl_srtt / 2;
    send_ctl->ctl_max_bytes_in_flight = 0;
    send_ctl->ctl_reordering_packet_threshold = RQC_kPacketThreshold;
    send_ctl->ctl_reordering_time_threshold_shift = RQC_kTimeThresholdShift;
    send_ctl->ctl_ack_sent_cnt = 0;
    send_ctl->ctl_first_rtt_sample_time = 0;

    send_ctl->ctl_largest_acked = RQC_MAX_UINT64_VALUE;
    send_ctl->ctl_largest_received = RQC_MAX_UINT64_VALUE;
    send_ctl->ctl_time_of_last_sent_ack_eliciting_packet = 0;
    send_ctl->ctl_loss_time = 0;

    memset(&send_ctl->ctl_largest_acked_sent_time, 0,
           sizeof(send_ctl->ctl_largest_acked_sent_time));

    memset(&send_ctl->ctl_largest_recv_time, 0,
           sizeof(send_ctl->ctl_largest_recv_time));

    send_ctl->ctl_is_cwnd_limited = 0;
    send_ctl->ctl_delivered = 0;
    send_ctl->ctl_lost_pkts_number = 0;
    send_ctl->ctl_last_inflight_pkt_sent_time = 0;

    rqc_timer_init(&send_ctl->path_timer_manager, conn->log, send_ctl);

    rqc_pacing_init(&send_ctl->ctl_pacing, conn->conn_settings.pacing_on, send_ctl);

    send_ctl->ctl_info.record_interval = RQC_DEFAULT_RECORD_INTERVAL;
    send_ctl->ctl_info.last_record_time = 0;
    send_ctl->ctl_info.last_rtt_time = 0;
    send_ctl->ctl_info.last_lost_time = 0;
    send_ctl->ctl_info.last_bw_time = 0;
    send_ctl->ctl_info.rtt_change_threshold = RQC_DEFAULT_RTT_CHANGE_THRESHOLD;
    send_ctl->ctl_info.bw_change_threshold = RQC_DEFAULT_BW_CHANGE_THRESHOLD;

    send_ctl->sampler.send_ctl = send_ctl;

    /*
     * Move all sent/unsent packets to the send queue for resending
     * Initial packets with new packet header.
     *
     * TODO: Refactoring packet generation: generate packet header before sent.
     * Then all we need to do is move the packets from the unack queue to the
     * send queue, and the rest of the queues will send normally.
     */

    rqc_list_head_t *pos, *next;
    rqc_packet_out_t *packet_out;
    rqc_send_queue_t *send_queue = conn->conn_send_queue;

    rqc_list_for_each_safe(pos, next, &send_queue->sndq_unacked_packets) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
        rqc_send_queue_remove_unacked(packet_out, send_queue);
        rqc_send_queue_move_to_tail(pos, &send_queue->sndq_send_packets);
        rqc_send_ctl_decrease_inflight(conn, packet_out);
    }

    rqc_list_for_each_safe(pos, next, &send_queue->sndq_send_packets_high_pri) {
        rqc_send_queue_move_to_tail(pos, &send_queue->sndq_send_packets);
    }

    rqc_list_for_each_safe(pos, next, &send_queue->sndq_lost_packets) {
        rqc_send_queue_move_to_tail(pos, &send_queue->sndq_send_packets);
    }

    rqc_list_for_each_safe(pos, next, &send_queue->sndq_pto_probe_packets) {
        rqc_send_queue_move_to_tail(pos, &send_queue->sndq_send_packets);
    }

    rqc_log_event(conn->log, REC_PARAMETERS_SET, send_ctl, RQC_kGranularity, conn->conn_settings.cc_params);
}

rqc_pn_ctl_t *
rqc_pn_ctl_create(rqc_connection_t *conn)
{
    rqc_pn_ctl_t *pn_ctl;
    pn_ctl = rqc_pcalloc(conn->conn_pool, sizeof(rqc_pn_ctl_t));
    if (pn_ctl == NULL) {
        return NULL;
    }

    rqc_memzero(&pn_ctl->ctl_recv_record, sizeof(rqc_recv_record_t));
    rqc_init_list_head(&pn_ctl->ctl_recv_record.list_head);

    if (rqc_ack_sent_record_init(&pn_ctl->ack_sent_record) == RQC_ERROR) {
        return NULL;
    }

    return pn_ctl;
}

void
rqc_pn_ctl_destroy(rqc_pn_ctl_t *pn_ctl)
{
    rqc_recv_record_destroy(&pn_ctl->ctl_recv_record);
    rqc_ack_sent_record_destroy(&pn_ctl->ack_sent_record);
}

rqc_pn_ctl_t *
rqc_get_pn_ctl(rqc_connection_t *conn, rqc_path_ctx_t *path)
{
    return path->path_pn_ctl;
}

/*
 * QUIC's congestion control is based on TCP NewReno [RFC6582].  NewReno
 * is a congestion window based congestion control.  QUIC specifies the
 * congestion window in bytes rather than packets due to finer control
 * and the ease of appropriate byte counting [RFC3465].
 *
 * QUIC hosts MUST NOT send packets if they would increase
 * bytes_in_flight (defined in Appendix B.2) beyond the available
 * congestion window, unless the packet is a probe packet sent after a
 * PTO timer expires, as described in Section 6.3.

 * Implementations MAY use other congestion control algorithms, such as
 * Cubic [RFC8312], and endpoints MAY use different algorithms from one
 * another.  The signals QUIC provides for congestion control are
 * generic and are designed to support different algorithms.
 */
int
rqc_send_ctl_can_send(rqc_send_ctl_t *send_ctl, rqc_packet_out_t *packet_out, uint32_t schedule_bytes)
{
    rqc_connection_t *conn = send_ctl->ctl_conn;

    int can = 1;
    unsigned congestion_window = send_ctl->ctl_cong_callback->rqc_cong_ctl_get_cwnd(send_ctl->ctl_cong);

    if (conn->conn_settings.so_sndbuf > 0) {
        congestion_window = rqc_min(congestion_window, conn->conn_settings.so_sndbuf);
    }

    if (send_ctl->ctl_bytes_in_flight + schedule_bytes + packet_out->po_used_size > congestion_window) {
        can = 0;
    }

    return can;
}

rqc_bool_t
rqc_send_packet_cwnd_allows(rqc_send_ctl_t *send_ctl,
    rqc_packet_out_t *packet_out, uint32_t schedule_bytes, rqc_usec_t now)
{
    if (RQC_CAN_IN_FLIGHT(packet_out->po_frame_types)) {
        /* packet with high priority first */
        if (!rqc_send_ctl_can_send(send_ctl, packet_out, schedule_bytes)) {
            if (packet_out->po_send_cwnd_blk_ts == 0) {
                packet_out->po_send_cwnd_blk_ts = now;
            }
            return RQC_FALSE;
        }
    }

    return RQC_TRUE;
}

rqc_bool_t
rqc_send_packet_pacer_allows(rqc_send_ctl_t *send_ctl,
    rqc_packet_out_t *packet_out, uint32_t schedule_bytes, rqc_usec_t now)
{
    if (RQC_CAN_IN_FLIGHT(packet_out->po_frame_types)) {

        if (rqc_pacing_is_on(&send_ctl->ctl_pacing)) {
            if (!rqc_pacing_can_write(&send_ctl->ctl_pacing,
                    schedule_bytes + packet_out->po_used_size))
            {
                if (packet_out->po_send_pacing_blk_ts == 0) {
                    packet_out->po_send_pacing_blk_ts = now;
                }
                return RQC_FALSE;
            }
        }
    }

    return RQC_TRUE;
}

rqc_bool_t
rqc_send_packet_check_cc(rqc_send_ctl_t *send_ctl,
    rqc_packet_out_t *po, uint32_t schedule_bytes, rqc_usec_t now)
{
    return rqc_send_packet_cwnd_allows(send_ctl, po, schedule_bytes, now)
           && rqc_send_packet_pacer_allows(send_ctl, po, schedule_bytes, now);
}

void
rqc_send_queue_maybe_remove_unacked(rqc_packet_out_t *packet_out, rqc_send_queue_t *send_queue, rqc_path_ctx_t *path)
{
    /* it is origin & some pkt ref to this packet */
    if (packet_out->po_origin == NULL && packet_out->po_origin_ref_cnt != 0) {
        return;
    }

    if (path && (packet_out->po_flag & RQC_POF_IN_PATH_BUF_LIST)) {
        rqc_path_send_buffer_remove(path, packet_out);

    } else {
        rqc_send_queue_remove_unacked(packet_out, send_queue);
    }

    if (packet_out->po_origin
        && (--packet_out->po_origin->po_origin_ref_cnt) == 0)
    {
        /* po_origin could be an inflight one, thus requiring decrease inflight. */
        rqc_send_ctl_decrease_inflight(send_queue->sndq_conn, packet_out->po_origin);
        rqc_send_queue_remove_unacked(packet_out->po_origin, send_queue);
        rqc_send_queue_insert_free(packet_out->po_origin, &send_queue->sndq_free_packets, send_queue);
    }

    rqc_send_queue_insert_free(packet_out, &send_queue->sndq_free_packets, send_queue);
}

void
rqc_send_ctl_on_reset_stream_acked(rqc_send_ctl_t *send_ctl, rqc_packet_out_t *packet_out)
{
    if (packet_out->po_frame_types & RQC_FRAME_BIT_RESET_STREAM) {
        rqc_stream_t *stream;
        for (int i = 0; i < RQC_MAX_STREAM_FRAME_IN_PO; i++) {
            if (packet_out->po_stream_frames[i].ps_is_used == 0) {
                break;
            }
            stream = rqc_find_stream_by_id(packet_out->po_stream_frames[i].ps_stream_id, send_ctl->ctl_conn->streams_hash);
            if (stream != NULL && packet_out->po_stream_frames[i].ps_is_reset) {
                if (stream->stream_state_send == RQC_SEND_STREAM_ST_RESET_SENT) {
                    rqc_stream_send_state_update(stream, RQC_SEND_STREAM_ST_RESET_RECVD);
                    rqc_stream_maybe_need_close(stream);
                }
            }
        }
    }
}

void
rqc_send_ctl_increase_inflight(rqc_connection_t *conn, rqc_packet_out_t *packet_out)
{
    rqc_path_ctx_t *path = conn->the_path;
    if (path == NULL) {
        return;
    }

    rqc_send_ctl_t *send_ctl = path->path_send_ctl;
    if (!(packet_out->po_flag & RQC_POF_IN_FLIGHT) && RQC_CAN_IN_FLIGHT(packet_out->po_frame_types)) {
        if (RQC_IS_ACK_ELICITING(packet_out->po_frame_types)) {
            send_ctl->ctl_bytes_in_flight += packet_out->po_used_size;
            send_ctl->ctl_bytes_ack_eliciting_inflight += packet_out->po_used_size;
            packet_out->po_flag |= RQC_POF_IN_FLIGHT;
        }
    }
}

void
rqc_send_ctl_decrease_inflight(rqc_connection_t *conn, rqc_packet_out_t *packet_out)
{
    rqc_path_ctx_t *path = conn->the_path;
    if (path == NULL) {
        return;
    }

    rqc_send_ctl_t *send_ctl = path->path_send_ctl;
    if (packet_out->po_flag & RQC_POF_IN_FLIGHT) {
        if (RQC_IS_ACK_ELICITING(packet_out->po_frame_types)) {
            send_ctl->ctl_bytes_ack_eliciting_inflight = rqc_uint32_bounded_subtract(send_ctl->ctl_bytes_ack_eliciting_inflight, packet_out->po_used_size);
            send_ctl->ctl_bytes_in_flight = rqc_uint32_bounded_subtract(send_ctl->ctl_bytes_in_flight, packet_out->po_used_size);
            packet_out->po_flag &= ~RQC_POF_IN_FLIGHT;
        }
    }
}

static void
rqc_send_ctl_update_cwnd_limited(rqc_send_ctl_t *send_ctl, rqc_usec_t now)
{
    if (send_ctl->ctl_bytes_in_flight > send_ctl->ctl_max_bytes_in_flight) {
        send_ctl->ctl_max_bytes_in_flight = send_ctl->ctl_bytes_in_flight;
    }
    uint32_t cwnd_bytes = send_ctl->ctl_cong_callback->rqc_cong_ctl_get_cwnd(send_ctl->ctl_cong);
    /* If we can not send the next full-size packet, we are CWND limited. */
    send_ctl->ctl_is_cwnd_limited = 0;
    uint32_t actual_mss = rqc_conn_get_mss(send_ctl->ctl_conn);
    if ((send_ctl->ctl_bytes_in_flight + actual_mss) > cwnd_bytes) {
        send_ctl->ctl_is_cwnd_limited = 1;
        /* record the time of cwnd limited */
        send_ctl->ctl_recent_cwnd_limitation_time[send_ctl->ctl_cwndlim_update_idx] = now;
        send_ctl->ctl_cwndlim_update_idx = (send_ctl->ctl_cwndlim_update_idx + 1) % 3;
    }
}

/**
 * OnPacketSent
 */
void
rqc_send_ctl_on_packet_sent(rqc_send_ctl_t *send_ctl, rqc_pn_ctl_t *pn_ctl, rqc_packet_out_t *packet_out, rqc_usec_t now)
{
    rqc_sample_on_sent(packet_out, send_ctl, now);

    if (packet_out->po_pkt.pkt_num > pn_ctl->ctl_largest_sent) {
        pn_ctl->ctl_largest_sent = packet_out->po_pkt.pkt_num;
    }

    send_ctl->ctl_bytes_send += packet_out->po_used_size;
    if (packet_out->po_frame_types & RQC_FRAME_BIT_STREAM) {
        send_ctl->ctl_app_bytes_send += packet_out->po_used_size;
    }

    if (packet_out->po_largest_ack > 0) {
        rqc_ack_sent_record_add(&pn_ctl->ack_sent_record, packet_out, send_ctl->ctl_srtt, now);
    }

    if (RQC_CAN_IN_FLIGHT(packet_out->po_frame_types)) {

        if (RQC_IS_ACK_ELICITING(packet_out->po_frame_types)) {
            send_ctl->ctl_time_of_last_sent_ack_eliciting_packet =
            packet_out->po_sent_time;
            send_ctl->ctl_last_sent_ack_eliciting_packet_number =
            packet_out->po_pkt.pkt_num;
        }

        rqc_conn_update_stream_stats_on_sent(send_ctl->ctl_conn, send_ctl, packet_out, now);

        if (send_ctl->ctl_bytes_in_flight == 0) {

            if (send_ctl->ctl_cong_callback->rqc_cong_ctl_init_bbr
                && send_ctl->ctl_app_limited > 0)
            {

                send_ctl->ctl_cong_callback->rqc_cong_ctl_restart_from_idle(send_ctl->ctl_cong, send_ctl->ctl_delivered);
                rqc_log_event(send_ctl->ctl_conn->log, REC_CONGESTION_STATE_UPDATED, "restart");
            }

            if (!send_ctl->ctl_cong_callback->rqc_cong_ctl_init_bbr) {
                send_ctl->ctl_cong_callback->rqc_cong_ctl_restart_from_idle(send_ctl->ctl_cong, send_ctl->ctl_last_inflight_pkt_sent_time);
                rqc_log_event(send_ctl->ctl_conn->log, REC_CONGESTION_STATE_UPDATED, "restart");
            }
        }

        if (!(packet_out->po_flag & RQC_POF_IN_FLIGHT)) {
            rqc_send_ctl_increase_inflight(send_ctl->ctl_conn, packet_out);
            rqc_conn_increase_unacked_stream_ref(send_ctl->ctl_conn, packet_out);
        }

        if (RQC_IS_ACK_ELICITING(packet_out->po_frame_types))
        {
            rqc_send_ctl_set_loss_detection_timer(send_ctl);
        }

        if (packet_out->po_flag & RQC_POF_LOST) {
            ++send_ctl->ctl_lost_count;
            send_ctl->ctl_recent_lost_count[0]++;
            packet_out->po_flag &= ~RQC_POF_LOST;

        }

        if (packet_out->po_flag & RQC_POF_TLP) {
            ++send_ctl->ctl_tlp_count;
            send_ctl->ctl_recent_lost_count[0]++;
            packet_out->po_flag &= ~RQC_POF_TLP;
        }

        ++send_ctl->ctl_send_count;
        send_ctl->ctl_recent_send_count[0]++;

        send_ctl->ctl_last_inflight_pkt_sent_time = now;
        rqc_send_ctl_update_cwnd_limited(send_ctl, now);
    }

    if (packet_out->po_frame_types & RQC_FRAME_BIT_CONNECTION_CLOSE) {
        if (send_ctl->ctl_conn->conn_close_send_time == 0) {
            send_ctl->ctl_conn->conn_close_send_time = now;
        }
    }

    if (packet_out->po_frame_types & RQC_FRAME_BIT_HANDSHAKE) {
        send_ctl->ctl_conn->conn_flag |= RQC_CONN_FLAG_HANDSHAKE_SENT;
    }

    send_ctl->ctl_conn->conn_last_send_time = now;

    if (!send_ctl->ctl_recent_stats_timestamp) {
        send_ctl->ctl_recent_stats_timestamp = now;
    }

    if (now >= send_ctl->ctl_recent_stats_timestamp + (5 * send_ctl->ctl_srtt)) {
        send_ctl->ctl_recent_stats_timestamp = now;
        send_ctl->ctl_recent_lost_count[1] = send_ctl->ctl_recent_lost_count[0];
        send_ctl->ctl_recent_send_count[1] = send_ctl->ctl_recent_send_count[0];
        send_ctl->ctl_recent_lost_count[0] = 0;
        send_ctl->ctl_recent_send_count[0] = 0;
    }
}

/**
 * OnAckReceived
 */
int
rqc_send_ctl_on_ack_received(rqc_send_ctl_t *send_ctl, rqc_pn_ctl_t *pn_ctl, rqc_send_queue_t *send_queue, rqc_ack_info_t *const ack_info, rqc_usec_t ack_recv_time)
{
    rqc_connection_t *conn = send_ctl->ctl_conn;

    rqc_packet_out_t *packet_out;
    rqc_list_head_t *pos, *next;
    rqc_pktno_range_t *range = &ack_info->ranges[ack_info->n_ranges - 1];

    /* 标记ack info里是否有这条路径发出的包 */
    unsigned char has_acked = 0, update_largest_ack = 0;
    unsigned char has_ack_eliciting = 0, spurious_loss_detected = 0;
    rqc_packet_number_t frame_largest_ack = ack_info->ranges[0].high;
    rqc_packet_number_t spurious_loss_pktnum = 0;
    rqc_usec_t spurious_loss_sent_time = 0;
    unsigned char need_del_record = 0;

    rqc_packet_number_t largest_acked_ack = rqc_ack_sent_record_on_ack(&pn_ctl->ack_sent_record, ack_info);
    if (largest_acked_ack > pn_ctl->ctl_largest_acked_ack) {
        pn_ctl->ctl_largest_acked_ack = largest_acked_ack;
        need_del_record = 1;
    }

    /* 记录ack info里这条路径发出的最大pn的包 */
    rqc_packet_number_t path_largest_pkt_num = 0;

    rqc_init_sample_before_ack(&send_ctl->sampler);

    /* detect and remove acked packets */
    rqc_list_for_each_safe(pos, next, &send_queue->sndq_unacked_packets) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);

        // 直到pn超过frame_largest_ack，结束遍历
        if (packet_out->po_pkt.pkt_num > frame_largest_ack) {
            break;
        }

        // 如果pn大于发的最大pn，报错
        if (packet_out->po_pkt.pkt_num > pn_ctl->ctl_largest_sent) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|pkt is not sent yet|%ui|", packet_out->po_pkt.pkt_num);
            return -RQC_EPROTO;
        }

        // range从后数，ack range递增
        while (packet_out->po_pkt.pkt_num > range->high && range != ack_info->ranges) {
            --range;
        }

        if (packet_out->po_pkt.pkt_num >= range->low) {
            // this packet is acked

            // 修改标志位
            if (has_acked == 0) {
                /* 初始化 */
                send_ctl->ctl_prior_delivered = send_ctl->ctl_delivered;
                send_ctl->ctl_prior_bytes_in_flight = send_ctl->ctl_bytes_in_flight;

                has_acked = 1;
            }

            path_largest_pkt_num = packet_out->po_pkt.pkt_num;

            // 更新ctl_largest_acked
            // 若ack info里此路径最大pn大于path largest acked，更新 largest acked
            if (packet_out->po_pkt.pkt_num > send_ctl->ctl_largest_acked ||
                send_ctl->ctl_largest_acked == RQC_MAX_UINT64_VALUE)
            {
                update_largest_ack = 1;
                send_ctl->ctl_largest_acked = packet_out->po_pkt.pkt_num;
                send_ctl->ctl_largest_acked_sent_time = packet_out->po_sent_time;
            }

            // 更新 largest_ack_both
            if (packet_out->po_largest_ack > pn_ctl->ctl_largest_acked_ack) {
                pn_ctl->ctl_largest_acked_ack = packet_out->po_largest_ack;
                need_del_record = 1;
            }

            // 更新sample
            rqc_update_sample(&send_ctl->sampler, packet_out, send_ctl, ack_recv_time);

            /* Packet previously declared lost gets acked */
            if (!(packet_out->po_flag & RQC_POF_SPURIOUS_LOSS) && (packet_out->po_flag & RQC_POF_RETRANSED)) {
                ++send_ctl->ctl_spurious_loss_count;
                if (!spurious_loss_detected) {
                    spurious_loss_detected = 1;
                    spurious_loss_pktnum = packet_out->po_pkt.pkt_num;
                    spurious_loss_sent_time = packet_out->po_sent_time;
                }
                packet_out->po_flag |= RQC_POF_SPURIOUS_LOSS;
            }

            rqc_send_ctl_on_packet_acked(send_ctl, packet_out, ack_recv_time, 1);

            if (packet_out->po_used_size > conn->max_acked_po_size) {
                conn->max_acked_po_size = packet_out->po_used_size;
            }

            rqc_send_queue_maybe_remove_unacked(packet_out, send_queue, NULL);

            if (RQC_IS_ACK_ELICITING(packet_out->po_frame_types)) {
                has_ack_eliciting = 1;
            }
        }
    }

    /* 此path没有ack */
    if (!has_acked) {
        return RQC_OK;
    }

    if (update_largest_ack && has_ack_eliciting) {
        /* 更新 ctl_latest_rtt */
        send_ctl->ctl_latest_rtt = ack_recv_time - send_ctl->ctl_largest_acked_sent_time;
        /* 更新rtt */
        rqc_send_ctl_update_rtt(send_ctl, &send_ctl->ctl_latest_rtt, ack_info->ack_delay);
    }

    /* TODO: ECN */

    /* spurious loss */
    if (spurious_loss_detected) {
        rqc_send_ctl_on_spurious_loss_detected(send_ctl, ack_recv_time, path_largest_pkt_num,
                                               spurious_loss_pktnum, spurious_loss_sent_time);
    }

    /* DetectAndRemoveLostPackets + OnPacketsLost */
    rqc_send_ctl_detect_lost(send_ctl, send_queue, ack_recv_time);

    // 更新recv record
    if (need_del_record) {
        rqc_recv_record_del(&pn_ctl->ctl_recv_record, pn_ctl->ctl_largest_acked_ack + 1);
    }

    send_ctl->ctl_pto_count = 0;
    rqc_send_ctl_set_loss_detection_timer(send_ctl);

    /* Clear app-limited field if the bubble is gone. */
    /* @NOTE: we need to clear it for Cubic/Reno as well. */
    if (send_ctl->ctl_app_limited
        && send_ctl->ctl_delivered > send_ctl->ctl_app_limited)
    {
        send_ctl->ctl_app_limited = 0;
    }

    /* BBR */
    if (send_ctl->ctl_cong_callback->rqc_cong_ctl_init_bbr /* && stream_frame_acked */) {

        uint64_t bw_before = 0, bw_after = 0;
        int bw_record_flag = 0;
        rqc_usec_t now = ack_recv_time;
        rqc_sample_type_t sample_type = rqc_generate_sample(&send_ctl->sampler, send_ctl, ack_recv_time);

        /* Make sure that we do not call BBR with a invalid sampler. */
        if (sample_type == RQC_RATE_SAMPLE_VALID) {
            if ((send_ctl->ctl_cong_callback->rqc_cong_ctl_get_bandwidth_estimate != NULL)
                && send_ctl->ctl_conn->log->log_level >= RQC_LOG_INFO
                && (send_ctl->ctl_info.last_bw_time + send_ctl->ctl_info.record_interval <= now))
            {
                bw_before = send_ctl->ctl_cong_callback->rqc_cong_ctl_get_bandwidth_estimate(send_ctl->ctl_cong);
                if (bw_before != 0) {
                    bw_record_flag = 1;
                }
            }

            send_ctl->ctl_cong_callback->rqc_cong_ctl_on_ack_multiple_pkts(send_ctl->ctl_cong, &send_ctl->sampler);
        }

        if (bw_record_flag) {
            bw_after = send_ctl->ctl_cong_callback->rqc_cong_ctl_get_bandwidth_estimate(send_ctl->ctl_cong);
            if (bw_after > 0) {
                if (rqc_sub_abs(bw_after, bw_before) * 100 > (bw_before * send_ctl->ctl_info.bw_change_threshold)) {

                    send_ctl->ctl_info.last_bw_time = now;
                    rqc_conn_log(conn, RQC_LOG_INFO,
                                 "|bandwidth change record|bw_before:%ui|bw_after:%ui|srtt:%ui|cwnd:%ui|",
                                 bw_before, bw_after, send_ctl->ctl_srtt, send_ctl->ctl_cong_callback->rqc_cong_ctl_get_cwnd(send_ctl->ctl_cong));
                }
            }
        }

    } else if (send_ctl->ctl_cong_callback->rqc_cong_ctl_on_ack_multiple_pkts) {
        rqc_sample_type_t sample_type = rqc_generate_sample(&send_ctl->sampler, send_ctl, ack_recv_time);
        /* Currently, this is only the case for Copa. */
        if (sample_type != RQC_RATE_SAMPLE_ACK_NOTHING) {
            send_ctl->ctl_cong_callback->rqc_cong_ctl_on_ack_multiple_pkts(send_ctl->ctl_cong, &send_ctl->sampler);
        }
    }

    rqc_log_event(conn->log, REC_METRICS_UPDATED, send_ctl);
    return RQC_OK;
}

/**
 * OnDatagramReceived
 */
void
rqc_send_ctl_on_dgram_received(rqc_send_ctl_t *send_ctl, size_t dgram_size)
{
    send_ctl->ctl_bytes_recv += dgram_size;
    send_ctl->ctl_recv_count++;
}

void
rqc_send_ctl_latest_rtt_tracking(rqc_send_ctl_t *send_ctl, rqc_usec_t *latest_rtt)
{
    /* if sum is closed to range */
    if (send_ctl->ctl_latest_rtt_square_sum > ((uint64_t)1 << 62)) {
        return;
    }

    ++send_ctl->ctl_update_latest_rtt_count;

    rqc_msec_t sample = (*latest_rtt)/1000;
    send_ctl->ctl_latest_rtt_sum += sample;
    send_ctl->ctl_latest_rtt_square_sum += sample * sample;
}

/**
 * UpdateRtt
 */
void
rqc_send_ctl_update_rtt(rqc_send_ctl_t *send_ctl, rqc_usec_t *latest_rtt, rqc_usec_t ack_delay)
{
    rqc_send_ctl_latest_rtt_tracking(send_ctl, latest_rtt);

    /* Based on {{RFC6298}}. */
    if (send_ctl->ctl_first_rtt_sample_time == 0) {
        send_ctl->ctl_minrtt = *latest_rtt;
        send_ctl->ctl_srtt = *latest_rtt;
        send_ctl->ctl_rttvar = *latest_rtt >> 1;
        send_ctl->ctl_first_rtt_sample_time = rqc_monotonic_timestamp();

    } else {
        send_ctl->ctl_minrtt = rqc_min(*latest_rtt, send_ctl->ctl_minrtt);

        if (rqc_conn_is_handshake_done(send_ctl->ctl_conn)) {
            ack_delay = rqc_min(ack_delay, send_ctl->ctl_conn->remote_settings.max_ack_delay * 1000);
        }

        /* Adjust for ack delay if it's plausible. */
        rqc_usec_t adjusted_rtt = *latest_rtt;
        if (adjusted_rtt > ack_delay
            && (adjusted_rtt + 1000) >= (send_ctl->ctl_minrtt + ack_delay))
        {
            adjusted_rtt -= ack_delay;
        }

        uint64_t srtt = send_ctl->ctl_srtt;
        uint64_t rttvar = send_ctl->ctl_rttvar;

        /* rttvar = 3/4 * rttvar + 1/4 * abs(smoothed_rtt - adjusted_rtt)  */
        send_ctl->ctl_rttvar -= send_ctl->ctl_rttvar >> 2;
        send_ctl->ctl_rttvar += (send_ctl->ctl_srtt > adjusted_rtt
                            ? send_ctl->ctl_srtt - adjusted_rtt : adjusted_rtt - send_ctl->ctl_srtt) >> 2;

        /* smoothed_rtt = 7/8 * smoothed_rtt + 1/8 * adjusted_rtt */
        send_ctl->ctl_srtt -= send_ctl->ctl_srtt >> 3;
        send_ctl->ctl_srtt += adjusted_rtt >> 3;

        if (rqc_sub_abs(send_ctl->ctl_srtt, srtt)  > send_ctl->ctl_info.rtt_change_threshold) {
            rqc_usec_t now = rqc_monotonic_timestamp();
            if (send_ctl->ctl_info.last_rtt_time + send_ctl->ctl_info.record_interval <= now) {
                send_ctl->ctl_info.last_rtt_time = now;
                rqc_conn_log(send_ctl->ctl_conn, RQC_LOG_INFO, "|before update rtt|srtt:%ui|rttvar:%ui|"
                            "after update rtt|srtt:%ui|rttvar:%ui|minrtt:%ui|latest_rtt:%ui|ack_delay:%ui|",
                             srtt, rttvar, send_ctl->ctl_srtt, send_ctl->ctl_rttvar, send_ctl->ctl_minrtt, *latest_rtt, ack_delay);
            }
        }
    }
}

void
rqc_send_ctl_on_spurious_loss_detected(rqc_send_ctl_t *send_ctl,
    rqc_usec_t ack_recv_time, rqc_packet_number_t largest_ack,
    rqc_packet_number_t spurious_loss_pktnum, rqc_usec_t spurious_loss_sent_time)
{
    if (!send_ctl->ctl_conn->conn_settings.spurious_loss_detect_on) {
        return;
    }

    /* Adjust Packet Threshold */
    if (largest_ack < spurious_loss_pktnum) {
        return;
    }
    send_ctl->ctl_reordering_packet_threshold = rqc_max(send_ctl->ctl_reordering_packet_threshold,
                                                        rqc_send_ctl_get_pkt_num_gap(send_ctl, spurious_loss_pktnum, largest_ack) + 1);

    /* Adjust Time Threshold */
    if (ack_recv_time < spurious_loss_sent_time) {
        return;
    }
    rqc_usec_t reorder_time_interval = ack_recv_time - spurious_loss_sent_time;
    rqc_usec_t max_rtt = rqc_max(send_ctl->ctl_latest_rtt, send_ctl->ctl_srtt);
    while (max_rtt + (max_rtt >> send_ctl->ctl_reordering_time_threshold_shift) < reorder_time_interval
           && send_ctl->ctl_reordering_time_threshold_shift > 0)
    {
        --send_ctl->ctl_reordering_time_threshold_shift;
    }
}

/**
 * DetectAndRemoveLostPackets + OnPacketsLost
 */
void
rqc_send_ctl_detect_lost(rqc_send_ctl_t *send_ctl, rqc_send_queue_t *send_queue, rqc_usec_t now)
{
    rqc_list_head_t *pos, *next;
    rqc_packet_out_t *po, *largest_lost = NULL;
    uint64_t lost_n = 0;

    send_ctl->ctl_loss_time = 0;
    send_ctl->sampler.loss = 0;

    rqc_connection_t *conn = send_ctl->ctl_conn;

    if (send_ctl->ctl_largest_acked == RQC_MAX_UINT64_VALUE) {
        rqc_log(conn->log, RQC_LOG_WARN, "|exception|largest acked is not recorded|");
        return;
    }

    /* loss_delay = 9/8 * max(latest_rtt, smoothed_rtt) */
    rqc_usec_t loss_delay = rqc_max(send_ctl->ctl_latest_rtt, send_ctl->ctl_srtt);
    loss_delay += loss_delay >> send_ctl->ctl_reordering_time_threshold_shift;

    /* Minimum time of kGranularity before packets are deemed lost. */
    loss_delay = rqc_max(loss_delay, RQC_kGranularity * 1000);

    /* Packets sent before this time are deemed lost. */
    rqc_usec_t lost_send_time = now - loss_delay;

    /* Packets with packet numbers before this are deemed lost. */
    /* 若 lost_pn == RQC_MAX_UINT64_VALUE, 无丢包 */
    rqc_packet_number_t lost_pn = rqc_send_ctl_get_lost_sent_pn(send_ctl);

    rqc_list_for_each_safe(pos, next, &send_queue->sndq_unacked_packets) {
        po = rqc_list_entry(pos, rqc_packet_out_t, po_list);

        if (rqc_send_ctl_indirectly_ack_or_drop_po(conn, po)) {
            continue;
        }

        /* If this packet is not lost, so is the next packet */
        if (po->po_pkt.pkt_num > send_ctl->ctl_largest_acked) {
            break;
        }

        /* Mark packet as lost, or set time when it should be marked. */
        if (po->po_sent_time <= lost_send_time
            || (lost_pn != RQC_MAX_UINT64_VALUE && po->po_pkt.pkt_num <= lost_pn))
        {
            if (po->po_flag & RQC_POF_IN_FLIGHT) {
                rqc_send_ctl_decrease_inflight(conn, po);

                if (RQC_NEED_REPAIR(po->po_frame_types)
                    || (po->po_flag & RQC_POF_NOTIFY))
                {
                    rqc_send_queue_copy_to_lost(po, send_queue, RQC_TRUE);

                } else {
                    rqc_send_queue_remove_unacked(po, send_queue);
                    rqc_send_queue_insert_free(po, &send_queue->sndq_free_packets, send_queue);
                }

                conn->detected_loss_cnt++;
                lost_n++;

                rqc_log_event(conn->log, REC_PACKET_LOST, po, lost_pn, lost_send_time, loss_delay);

            } else {
                continue;
            }

            /* remember largest_loss for OnPacketsLost */
            if (largest_lost == NULL
                || (po->po_pkt.pkt_num > largest_lost->po_pkt.pkt_num))
            {
                largest_lost = po;
            }

        } else {
            if (send_ctl->ctl_loss_time == 0) {
                send_ctl->ctl_loss_time = po->po_sent_time + loss_delay;

            } else {
                send_ctl->ctl_loss_time = rqc_min(send_ctl->ctl_loss_time, po->po_sent_time + loss_delay);
            }
        }
    }

    /* update statistic */
    send_ctl->ctl_lost_pkts_number += lost_n;
    send_ctl->sampler.loss = lost_n;

    /**
     * OnPacketsLost
     */
    if (largest_lost) {
        /*
         * Start a new congestion epoch if the last lost packet
         * has passed the end of the previous recovery epoch.
         * enter loss recovery here
         */
        rqc_send_ctl_congestion_event(send_ctl, largest_lost->po_sent_time);

        if (send_ctl->ctl_first_rtt_sample_time == 0) {
            return;
        }

        /* Collapse congestion window if persistent congestion */
        if (send_ctl->ctl_cong_callback->rqc_cong_ctl_reset_cwnd
            && rqc_send_ctl_in_persistent_congestion(send_ctl, largest_lost, now))
        {
            /* For loss-based CCs, it means we are gonna slow start again. */
            send_ctl->ctl_max_bytes_in_flight = 0;
            /* we reset BBR's cwnd here */
            send_ctl->ctl_cong_callback->rqc_cong_ctl_reset_cwnd(send_ctl->ctl_cong);
        }

        if (send_ctl->ctl_info.last_lost_time + send_ctl->ctl_info.record_interval <= now) {
            rqc_usec_t lost_interval = now - send_ctl->ctl_info.last_lost_time;
            send_ctl->ctl_info.last_lost_time = now;
            uint64_t lost_count = send_ctl->ctl_lost_count + lost_n - send_ctl->ctl_info.last_lost_count;
            uint64_t send_count = send_ctl->ctl_send_count - send_ctl->ctl_info.last_send_count;
            send_ctl->ctl_info.last_lost_count = send_ctl->ctl_lost_count + lost_n;
            send_ctl->ctl_info.last_send_count = send_ctl->ctl_send_count;
            uint64_t bw = 0;
            if (send_ctl->ctl_cong_callback->rqc_cong_ctl_get_bandwidth_estimate) {
                bw = send_ctl->ctl_cong_callback->rqc_cong_ctl_get_bandwidth_estimate(send_ctl->ctl_cong);
            }
            rqc_conn_log(conn, RQC_LOG_INFO, "|lost interval:%ui|lost_count:%ui|send_count:%ui|pkt_num:%ui"
                        "|po_send_time:%ui|srtt:%ui|cwnd:%ud|bw:%ui|conn_life:%ui|now:%ui|last_lost_time:%ui|",
                        lost_interval, lost_count, send_count, largest_lost->po_pkt.pkt_num, largest_lost->po_sent_time, send_ctl->ctl_srtt,
                        send_ctl->ctl_cong_callback->rqc_cong_ctl_get_cwnd(send_ctl->ctl_cong), bw, now - conn->conn_create_time);
        }
    }
}

/**
 * InPersistentCongestion
 */
rqc_bool_t
rqc_send_ctl_in_persistent_congestion(rqc_send_ctl_t *send_ctl, rqc_packet_out_t *largest_lost, rqc_usec_t now)
{
    if (send_ctl->ctl_pto_count >= RQC_CONSECUTIVE_PTO_THRESH) {
        rqc_usec_t duration = (send_ctl->ctl_srtt + rqc_max(send_ctl->ctl_rttvar << 2, RQC_kGranularity * 1000)
            + send_ctl->ctl_conn->remote_settings.max_ack_delay * 1000) * RQC_kPersistentCongestionThreshold;
        if (now - largest_lost->po_sent_time > duration) {
            return RQC_TRUE;
        }
    }

    return RQC_FALSE;
}

/**
 * CongestionEvent
 */
void
rqc_send_ctl_congestion_event(rqc_send_ctl_t *send_ctl, rqc_usec_t sent_time)
{
    if (send_ctl->ctl_cong_callback->rqc_cong_ctl_on_lost) {
        send_ctl->ctl_cong_callback->rqc_cong_ctl_on_lost(send_ctl->ctl_cong, sent_time);
    }
}

/**
 * IsAppLimited
 */
int
rqc_send_ctl_is_app_limited(rqc_send_ctl_t *send_ctl)
{
    return send_ctl->ctl_app_limited > 0;
}

/* This function is called inside cc's on_ack callbacks if needed */
int
rqc_send_ctl_is_cwnd_limited(rqc_send_ctl_t *send_ctl)
{
    if (send_ctl->ctl_cong_callback->rqc_cong_ctl_in_slow_start(send_ctl->ctl_cong)) {
        uint32_t double_cwnd = send_ctl->ctl_max_bytes_in_flight << 1;
        uint32_t cwnd = send_ctl->ctl_cong_callback->rqc_cong_ctl_get_cwnd(send_ctl->ctl_cong);
        return cwnd < double_cwnd;
    }
    return (send_ctl->ctl_is_cwnd_limited);
}

void
rqc_send_ctl_cc_on_ack(rqc_send_ctl_t *send_ctl, rqc_packet_out_t *acked_packet,
                       rqc_usec_t now)
{
    if (send_ctl->ctl_cong_callback->rqc_cong_ctl_on_ack) {
        send_ctl->ctl_cong_callback->rqc_cong_ctl_on_ack(send_ctl->ctl_cong, acked_packet, now);
    }
}

/**
 * OnPacketAcked
 */
void
rqc_send_ctl_on_packet_acked(rqc_send_ctl_t *send_ctl,
    rqc_packet_out_t *acked_packet, rqc_usec_t now, int do_cc)
{
    rqc_packet_out_t *packet_out = acked_packet;
    rqc_connection_t *conn = send_ctl->ctl_conn;
    rqc_bool_t notify_ping;

    if (acked_packet->po_frame_types & RQC_FRAME_BIT_HANDSHAKE) {
        rqc_conn_on_handshake_acked(conn);
    }

    rqc_conn_decrease_unacked_stream_ref(send_ctl->ctl_conn, packet_out);

    /* If a packet marked as STREAM_CLOSED, when it is acked, it comes here */
    if (packet_out->po_flag & RQC_POF_IN_FLIGHT) {
        rqc_send_ctl_decrease_inflight(send_ctl->ctl_conn, packet_out);

        if (packet_out->po_frame_types & RQC_FRAME_BIT_RESET_STREAM) {
            rqc_send_ctl_on_reset_stream_acked(send_ctl, packet_out);
        }

        if (packet_out->po_frame_types & RQC_FRAME_BIT_PING) {
            if (conn->app_proto_cbs.conn_cbs.conn_ping_acked
                && (packet_out->po_flag & RQC_POF_NOTIFY))
            {
                notify_ping = RQC_TRUE;
                if (packet_out->po_pr && packet_out->po_pr->notified) {
                    notify_ping = RQC_FALSE;
                }

                if (notify_ping) {
                    conn->app_proto_cbs.conn_cbs.conn_ping_acked(conn, &conn->scid_set.user_scid,
                                                        packet_out->po_user_data, conn->user_data, conn->proto_data);
                    if (packet_out->po_pr) {
                        packet_out->po_pr->notified = RQC_TRUE;
                    }
                }

            }
        }

        if (do_cc) {
            rqc_send_ctl_cc_on_ack(send_ctl, packet_out, now);
        }
    }

    packet_out->po_acked = 1;
    if (packet_out->po_origin) {
        packet_out->po_origin->po_acked = 1;
    }
}

rqc_usec_t
rqc_send_ctl_get_pto_time(rqc_send_ctl_t *send_ctl, rqc_usec_t now)
{
    rqc_usec_t duration;
    rqc_usec_t pto_timeout = RQC_MAX_UINT64_VALUE;
    rqc_connection_t *c = send_ctl->ctl_conn;
    double  backoff = rqc_send_ctl_pow_x(c->conn_settings.pto_backoff_factor, send_ctl->ctl_pto_count);

    /* set a cap to avoid PTO timeout overflow */
    backoff = rqc_min(backoff, 1 << 16);

    /* get pto duration */
    if (c->conn_settings.control_pto_value) {
        duration = send_ctl->ctl_srtt + send_ctl->ctl_srtt / 4;

    } else {
        duration = (send_ctl->ctl_srtt
                   + rqc_max(4 * send_ctl->ctl_rttvar, RQC_kGranularity * 1000));
    }

    /* RTT has not been measured yet*/
    if (send_ctl->ctl_first_rtt_sample_time == 0
        && c->conn_settings.initial_pto_duration != 0)
    {
        duration = c->conn_settings.initial_pto_duration;
    }

    duration *= backoff;

    if (send_ctl->ctl_bytes_in_flight == 0) {
        pto_timeout = rqc_monotonic_timestamp() + duration;
    } else if (send_ctl->ctl_bytes_ack_eliciting_inflight > 0) {
        if (rqc_conn_is_handshake_done(send_ctl->ctl_conn)) {
            duration += c->remote_settings.max_ack_delay * 1000 * backoff;
        }
        pto_timeout = send_ctl->ctl_time_of_last_sent_ack_eliciting_packet + duration;
    }

    return pto_timeout;
}

/**
 * SetLossDetectionTimer
 */
void
rqc_send_ctl_set_loss_detection_timer(rqc_send_ctl_t *send_ctl)
{
    rqc_usec_t now = rqc_monotonic_timestamp();
    rqc_usec_t interval = 0;

    rqc_usec_t loss_time = rqc_send_ctl_get_earliest_loss_time(send_ctl);
    interval = (loss_time > now) ? (loss_time - now) : 0;
    if (loss_time != 0) {
        /* Time threshold loss detection. */
        rqc_timer_set(&send_ctl->path_timer_manager, RQC_TIMER_LOSS_DETECTION, now, interval);
        return;
    }

    /* Don't arm timer if there are no ack-eliciting packets in flight. */
    if (0 == send_ctl->ctl_bytes_ack_eliciting_inflight)
    {
        rqc_timer_unset(&send_ctl->path_timer_manager, RQC_TIMER_LOSS_DETECTION);
        return;
    }

    /* get PTO timeout and update loss detection timer */
    rqc_usec_t timeout = rqc_send_ctl_get_pto_time(send_ctl, now);
    interval = (timeout > now) ? (timeout - now) : 0;
    rqc_timer_set(&send_ctl->path_timer_manager, RQC_TIMER_LOSS_DETECTION, now, interval);
}

/**
 * GetLossTimeAndSpace
 */
rqc_usec_t
rqc_send_ctl_get_earliest_loss_time(rqc_send_ctl_t *send_ctl)
{
    return send_ctl->ctl_loss_time;
}

rqc_usec_t
rqc_send_ctl_get_srtt(rqc_send_ctl_t *send_ctl)
{
    return send_ctl->ctl_srtt;
}

float
rqc_send_ctl_get_retrans_rate(rqc_send_ctl_t *send_ctl)
{
    if (send_ctl->ctl_send_count <= 0) {
        return 0.0f;

    } else {
        return (float)(send_ctl->ctl_lost_count + send_ctl->ctl_tlp_count) / send_ctl->ctl_send_count;
    }
}

float
rqc_send_ctl_get_spurious_loss_rate(rqc_send_ctl_t *send_ctl)
{
    if (send_ctl->ctl_send_count <= 0) {
        return 0.0f;

    } else {
        return (float)(send_ctl->ctl_spurious_loss_count) / send_ctl->ctl_send_count;
    }
}

rqc_bool_t
rqc_send_ctl_ack_received(rqc_send_ctl_t *send_ctl)
{
    return send_ctl->ctl_largest_acked_sent_time > 0;
}

rqc_packet_number_t
rqc_send_ctl_get_lost_sent_pn(rqc_send_ctl_t *send_ctl)
{
    rqc_packet_number_t largest_acked = send_ctl->ctl_largest_acked;
    rqc_packet_number_t threshold = send_ctl->ctl_reordering_packet_threshold;
    rqc_packet_number_t lost_pn = RQC_MAX_UINT64_VALUE;     /* pkt num从0开始 */

    if (largest_acked >= threshold) {
        lost_pn = largest_acked - threshold;
    }

    return lost_pn;
}

rqc_packet_number_t
rqc_send_ctl_get_pkt_num_gap(rqc_send_ctl_t *send_ctl, rqc_packet_number_t front, rqc_packet_number_t back)
{
    return back - front;
}

uint64_t
rqc_send_ctl_get_est_bw(rqc_send_ctl_t *send_ctl)
{
    if (send_ctl->ctl_cong && send_ctl->ctl_cong_callback) {
        if (send_ctl->ctl_cong_callback->rqc_cong_ctl_get_bandwidth_estimate) {
            return send_ctl->ctl_cong_callback->rqc_cong_ctl_get_bandwidth_estimate(send_ctl->ctl_cong);

        } else if (send_ctl->ctl_cong_callback->rqc_cong_ctl_get_cwnd) {
            uint64_t cwnd = send_ctl->ctl_cong_callback->rqc_cong_ctl_get_cwnd(send_ctl->ctl_cong);
            rqc_usec_t srtt = send_ctl->ctl_srtt ? send_ctl->ctl_srtt : send_ctl->ctl_conn->conn_settings.initial_rtt;
            return (cwnd * 1000000) / srtt;
        }
    }

    return 0;
}

uint64_t
rqc_send_ctl_get_pacing_rate(rqc_send_ctl_t *send_ctl) {
    return rqc_pacing_rate_calc(&send_ctl->ctl_pacing);
}

void
rqc_send_ctl_set_next_pn_for_packet(rqc_connection_t *conn, rqc_pn_ctl_t *pn_ctl,
    rqc_packet_out_t *packet_out, rqc_usec_t current_time)
{
    packet_out->po_pkt.pkt_num = pn_ctl->ctl_packet_number++;
}
