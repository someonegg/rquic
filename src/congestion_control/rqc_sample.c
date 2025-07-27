/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "src/congestion_control/rqc_sample.h"
#include "src/common/rqc_config.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/transport/rqc_packet_out.h"
#include "src/transport/rqc_packet.h"

void rqc_init_sample_before_ack(rqc_sample_t *sampler)
{
    rqc_send_ctl_t *ctl = sampler->send_ctl;
    memset(sampler, 0, sizeof(rqc_sample_t));
    sampler->send_ctl = ctl;
}

/**
 * see https://tools.ietf.org/html/draft-cheng-iccrg-delivery-rate-estimation-00#section-3.3
 */
/* Upon receiving ACK, fill in delivery rate sample rs. */
rqc_sample_type_t
rqc_generate_sample(rqc_sample_t *sampler, rqc_send_ctl_t *send_ctl,
    rqc_usec_t now)
{

    /* we do NOT have a valid sample yet. */
    /* the ACK acks nothing */
    if (sampler->prior_time == 0) {
        sampler->interval = 0;
        rqc_log(send_ctl->ctl_conn->log, RQC_LOG_WARN,
                "|sampler_prior_time_is_zero!|");
        return RQC_RATE_SAMPLE_ACK_NOTHING;
    }

    sampler->acked = send_ctl->ctl_delivered - send_ctl->ctl_prior_delivered;
    /* Use the longer of the send_elapsed and ack_elapsed */
    sampler->interval = rqc_max(sampler->ack_elapse, sampler->send_elapse);
    sampler->delivered = send_ctl->ctl_delivered - sampler->prior_delivered;
    /* This is for BBRv2 */
    sampler->lost_pkts = send_ctl->ctl_lost_pkts_number - sampler->prior_lost;

    /*
     * Even if the interval is too small,
     * we need to update these data for Copa.
     */
    sampler->now = now;
    sampler->rtt = send_ctl->ctl_latest_rtt;
    sampler->srtt = send_ctl->ctl_srtt;
    sampler->bytes_inflight = send_ctl->ctl_bytes_in_flight;
    sampler->prior_inflight = send_ctl->ctl_prior_bytes_in_flight;
    sampler->total_acked = send_ctl->ctl_delivered;
    sampler->total_lost_pkts = send_ctl->ctl_lost_pkts_number;

    /*
     * Normally we expect interval >= MinRTT.
     * Note that rate may still be over-estimated when a spuriously
     * retransmitted skb was first (s)acked because "interval"
     * is under-estimated (up to an RTT). However, continuously
     * measuring the delivery rate during loss recovery is crucial
     * for connections suffer heavy or prolonged losses.
     */
    if (sampler->interval < send_ctl->ctl_minrtt) {
        sampler->interval = 0;
        return RQC_RATE_SAMPLE_INTERVAL_TOO_SAMLL;
    }
    if (sampler->interval != 0) {
        /* unit of interval is us */
        sampler->delivery_rate = (uint64_t)(1e6 * sampler->delivered / sampler->interval);
    }

    return RQC_RATE_SAMPLE_VALID;
}

/* Update rs when packet is SACKed or ACKed. */
void
rqc_update_sample(rqc_sample_t *sampler, rqc_packet_out_t *packet,
    rqc_send_ctl_t *send_ctl, rqc_usec_t now)
{
    if (packet->po_delivered_time == 0) {
        return; /* P already SACKed */
    }

    send_ctl->ctl_delivered += packet->po_used_size;
    send_ctl->ctl_delivered_time = now;

    /* Update info using the newest packet: */
    /* if it's the ACKs from the first RTT round, we use the sample anyway */

    if ((sampler->prior_delivered == 0)
        || (packet->po_delivered > sampler->prior_delivered))
    {
        sampler->prior_lost = packet->po_lost;
        sampler->tx_in_flight = packet->po_tx_in_flight;
        sampler->prior_delivered = packet->po_delivered;
        sampler->prior_time = packet->po_delivered_time;

        if (rqc_conn_is_handshake_done(send_ctl->ctl_conn)) {
            sampler->is_app_limited = packet->po_is_app_limited;
        } else {
            sampler->is_app_limited = 1;
        }

        sampler->send_elapse = packet->po_sent_time -
                               packet->po_first_sent_time;
        sampler->ack_elapse = send_ctl->ctl_delivered_time -
                              packet->po_delivered_time;
        send_ctl->ctl_first_sent_time = packet->po_sent_time;
        sampler->lagest_ack_time = now;
    }

    /* always keep it updated with the largest acked packet */
    sampler->po_sent_time = packet->po_sent_time;
    /*
     * Mark the packet as delivered once it's SACKed to
     * avoid being used again when it's cumulatively acked.
     */
    packet->po_delivered_time = 0;
}

rqc_bool_t
rqc_sample_check_app_limited(rqc_sample_t *sampler, rqc_send_ctl_t *send_ctl, rqc_send_queue_t *send_queue)
{
    uint32_t cwnd_bytes = send_ctl->ctl_cong_callback->
                          rqc_cong_ctl_get_cwnd(send_ctl->ctl_cong);
    uint32_t actual_mss = rqc_conn_get_mss(send_ctl->ctl_conn);
    rqc_bool_t not_cwnd_limited = send_ctl->ctl_bytes_in_flight + actual_mss <=
                                  cwnd_bytes;
    /* @FIXME: We should find a better way to adapt it to multipath.
     * The current implemetation is problematic. As even if we have pkts
     * in snd/lost/pto list, they may not be scheduled on the path. Therefore,
     * if the path buffer is empty, there might be some "bubbles" in the pipe.
     * However, we have no better idea to handle this problem at this moment.
     */

    rqc_bool_t all_path_buffer_empty = RQC_TRUE;
    int i;
    for (i = RQC_SEND_TYPE_NORMAL; i < RQC_SEND_TYPE_N; i++) {
        if (!rqc_list_empty(&send_ctl->ctl_path->path_schedule_buf[i])) {
            all_path_buffer_empty = RQC_FALSE;
        }
    }

    if (not_cwnd_limited    /* We are not limited by CWND. */
        && rqc_list_empty(&send_queue->sndq_send_packets)  /* We have no packet to send. */
        && rqc_list_empty(&send_queue->sndq_lost_packets)  /* All lost packets have been retransmitted. */
        && rqc_list_empty(&send_queue->sndq_pto_probe_packets)
        && all_path_buffer_empty)
    {
        send_ctl->ctl_app_limited = (send_ctl->ctl_delivered +
                                    send_ctl->ctl_bytes_in_flight) ?
                                    (send_ctl->ctl_delivered +
                                    send_ctl->ctl_bytes_in_flight)
                                    : 1;
        if (send_ctl->ctl_app_limited > 0) {
            rqc_log_event(send_ctl->ctl_conn->log, REC_CONGESTION_STATE_UPDATED, "application_limit");
        }
        return RQC_TRUE;
    }

    return RQC_FALSE;
}

void
rqc_sample_on_sent(rqc_packet_out_t *packet_out, rqc_send_ctl_t *send_ctl,
    rqc_usec_t now)
{
    if (send_ctl->ctl_bytes_in_flight == 0) {
        send_ctl->ctl_delivered_time = send_ctl->ctl_first_sent_time = now;
    }
    packet_out->po_delivered_time = send_ctl->ctl_delivered_time;
    packet_out->po_first_sent_time = send_ctl->ctl_first_sent_time;
    packet_out->po_delivered = send_ctl->ctl_delivered;
    packet_out->po_is_app_limited = send_ctl->ctl_app_limited > 0 ? RQC_TRUE : RQC_FALSE;
    packet_out->po_lost = send_ctl->ctl_lost_pkts_number;
    packet_out->po_tx_in_flight = send_ctl->ctl_bytes_in_flight +
                                  packet_out->po_used_size;
}
