/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_PACKET_IN_H_INCLUDED_
#define _RQC_PACKET_IN_H_INCLUDED_

#include <rquic/rquic_typedef.h>
#include "src/transport/rqc_packet.h"
#include "src/transport/rqc_frame.h"

/* 1518 - ether_hdr - ip_hdr (20) - udp_hdr (8) = 1472 */
#define RQC_MAX_PACKET_IN_LEN 1500

struct rqc_packet_in_s {
    rqc_packet_t            pi_pkt;
    rqc_list_head_t         pi_list;
    const unsigned char    *buf;
    size_t                  buf_size;
    unsigned char          *pos;
    unsigned char          *last;
    rqc_usec_t              pkt_recv_time;  /* microsecond */
    rqc_frame_type_bit_t    pi_frame_types;

    rqc_stream_id_t         stream_id;
};

#define RQC_BUFF_LEFT_SIZE(pos, last) ((last) > (pos) ? (last) - (pos) : 0)

void rqc_packet_in_init(rqc_packet_in_t *packet_in,
    const unsigned char *packet_in_buf,
    size_t packet_in_size,
    rqc_usec_t recv_time);

void rqc_packet_in_destroy(rqc_packet_in_t *packet_in, rqc_connection_t *conn);

#endif /* _RQC_PACKET_IN_H_INCLUDED_ */
