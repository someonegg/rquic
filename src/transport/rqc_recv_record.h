/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_RECV_RECORD_H_INCLUDED_
#define _RQC_RECV_RECORD_H_INCLUDED_

#include "src/common/utils/ringarray/rqc_ring_array.h"
#include "src/transport/rqc_packet.h"

typedef enum {
    RQC_PKTRANGE_OK,
    RQC_PKTRANGE_DUP,
    RQC_PKTRANGE_ERR,
} rqc_pkt_range_status;

typedef struct rqc_pktno_range_s {
    rqc_packet_number_t low, high;
} rqc_pktno_range_t;

typedef struct rqc_pktno_range_node_s {
    rqc_pktno_range_t   pktno_range;
    rqc_list_head_t     list;
} rqc_pktno_range_node_t;

typedef struct rqc_recv_record_s {
    rqc_list_head_t         list_head;  /* rqc_pktno_range_node_t */
    rqc_packet_number_t     rr_del_from;
    rqc_int_t               node_count;
} rqc_recv_record_t;

#define RQC_MAX_ACK_RANGE_CNT 64

typedef struct rqc_ack_info_s {
    uint64_t                path_id;
    unsigned                n_ranges;  /* must > 0 */
    rqc_pktno_range_t       ranges[RQC_MAX_ACK_RANGE_CNT];
    rqc_usec_t              ack_delay;
    rqc_packet_number_t     largest_acked;
} rqc_ack_info_t;

typedef struct rqc_ack_sent_entry_s {
    rqc_packet_number_t pkt_num;
    rqc_packet_number_t largest_ack;
} rqc_ack_sent_entry_t;

typedef struct rqc_ack_sent_record_s {
    rqc_rarray_t       *ack_sent;
    rqc_usec_t          last_add_time;
} rqc_ack_sent_record_t;

void rqc_recv_record_print(rqc_connection_t *conn, rqc_recv_record_t *recv_record, char *buff, unsigned buff_size);

void rqc_recv_record_del(rqc_recv_record_t *recv_record, rqc_packet_number_t del_from);

void rqc_recv_record_destroy(rqc_recv_record_t *recv_record);

rqc_pkt_range_status rqc_recv_record_add(rqc_recv_record_t *recv_record, rqc_packet_number_t packet_number);

rqc_packet_number_t rqc_recv_record_largest(rqc_recv_record_t *recv_record);

void rqc_maybe_should_ack(rqc_connection_t *conn, rqc_path_ctx_t *path, rqc_pn_ctl_t *pn_ctl, int out_of_order, rqc_usec_t now);

int rqc_ack_sent_record_init(rqc_ack_sent_record_t *record);

void rqc_ack_sent_record_destroy(rqc_ack_sent_record_t *record);

int rqc_ack_sent_record_add(rqc_ack_sent_record_t *record, rqc_packet_out_t *packet_out, rqc_usec_t srtt, rqc_usec_t now);

rqc_packet_number_t rqc_ack_sent_record_on_ack(rqc_ack_sent_record_t *record, rqc_ack_info_t *ack_info);

#endif /* _RQC_RECV_RECORD_H_INCLUDED_ */
