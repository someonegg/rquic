/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "src/transport/rqc_packet_in.h"
#include "src/common/rqc_memory_pool.h"
#include "src/transport/rqc_conn.h"

void
rqc_packet_in_init(rqc_packet_in_t *packet_in,
    const unsigned char *packet_in_buf,
    size_t packet_in_size,
    rqc_usec_t recv_time)
{
    packet_in->buf = packet_in_buf;
    packet_in->buf_size = packet_in_size;
    packet_in->pos = (unsigned char *)packet_in_buf;
    packet_in->last = (unsigned char *)packet_in_buf + packet_in_size;
    packet_in->pkt_recv_time = recv_time;
}

void
rqc_packet_in_destroy(rqc_packet_in_t *packet_in, rqc_connection_t *conn)
{
    rqc_free((void *)packet_in->buf);
    rqc_free(packet_in);
}
