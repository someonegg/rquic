/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_SAMPLE_H_INCLUDED_
#define _RQC_SAMPLE_H_INCLUDED_

#include <rquic/rquic_typedef.h>

typedef struct rqc_sample_s {
    /* sampling time */
    rqc_usec_t       now;
    /* the number of packets that have been transferred when the packet currently in ack is being sent */
    uint64_t         prior_delivered;
    /* time interval between samples */
    rqc_usec_t       interval;
    /* the amount of data transferred (ack) between two samples */
    uint32_t         delivered;
    /* the amount of newly delivered data*/
    uint32_t         acked;
    /* the amount of data sent but not received ack */
    uint32_t         bytes_inflight;
    /* before processing this ack */
    uint32_t         prior_inflight;
    /* sampled rtt */
    rqc_usec_t       rtt;
    uint32_t         is_app_limited;
    /* whether packet loss */
    uint32_t         loss;
    uint64_t         total_acked;
    rqc_usec_t       srtt;
    /* used to determine if generate_sample needs to be called */
    rqc_usec_t       prior_time;
    rqc_usec_t       ack_elapse;
    rqc_usec_t       send_elapse;
    uint32_t         delivery_rate;
    rqc_usec_t       lagest_ack_time;
    rqc_send_ctl_t  *send_ctl;

    rqc_usec_t       po_sent_time;

    /* for BBRv2 */
    uint32_t         prior_lost;
    uint64_t         tx_in_flight;
    uint32_t         lost_pkts;

    uint32_t         total_lost_pkts;

} rqc_sample_t;

void rqc_init_sample_before_ack(rqc_sample_t *sampler);

/**
 * @brief
 * @return: 0, success; 1, the ACK acks nothing; 2, the interval is too small.
 */
typedef enum {
    RQC_RATE_SAMPLE_VALID = 0,
    RQC_RATE_SAMPLE_ACK_NOTHING = 1,
    RQC_RATE_SAMPLE_INTERVAL_TOO_SAMLL = 2,
} rqc_sample_type_t;

rqc_sample_type_t rqc_generate_sample(rqc_sample_t *sampler,
    rqc_send_ctl_t *send_ctl, rqc_usec_t now);
void rqc_update_sample(rqc_sample_t *sample, rqc_packet_out_t *packet,
    rqc_send_ctl_t *send_ctl, rqc_usec_t now);
rqc_bool_t rqc_sample_check_app_limited(rqc_sample_t *sampler,
    rqc_send_ctl_t *send_ctl, rqc_send_queue_t *send_queue);
void rqc_sample_on_sent(rqc_packet_out_t *packet_out, rqc_send_ctl_t *send_ctl,
    rqc_usec_t now);

#endif
