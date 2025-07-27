#ifndef _RQC_SEND_QUEUE_H_INCLUDED_
#define _RQC_SEND_QUEUE_H_INCLUDED_

#include "src/transport/rqc_packet_out.h"
#include "src/transport/rqc_conn.h"

#define RQC_SNDQ_PACKETS_USED_MAX            18000
#define RQC_SNDQ_RELEASE_ENOUGH_SPACE_TH     10  /* 1 / 10*/
#define RQC_SNDQ_MAX_UNACK_PACKETS_LIMIT     (100 * 1000) /* limit unack packets to avoid ddos attack */

typedef struct rqc_send_queue_s {

    rqc_connection_t           *sndq_conn;

    /* send queue for packets, should be in connection level */
    rqc_list_head_t             sndq_send_packets;                  /* rqc_packet_out_t to send */
    rqc_list_head_t             sndq_send_packets_high_pri;         /* rqc_packet_out_t to send with high priority */
    rqc_list_head_t             sndq_unacked_packets;               /* rqc_packet_out_t */

    rqc_list_head_t             sndq_lost_packets;                  /* rqc_packet_out_t */
    rqc_list_head_t             sndq_free_packets;                  /* rqc_packet_out_t */
    rqc_list_head_t             sndq_buff_1rtt_packets;             /* rqc_packet_out_t buff 1RTT before handshake done */
    rqc_list_head_t             sndq_pto_probe_packets;             /* rqc_packet_out_t */

    uint64_t                    sndq_packets_in_unacked_list;       /* to estimate bytes in the lists except for unacked list */
    uint64_t                    sndq_packets_used;
    uint64_t                    sndq_packets_used_bytes;
    uint64_t                    sndq_packets_free;
    uint64_t                    sndq_packets_used_max;

    rqc_bool_t                  sndq_full;

} rqc_send_queue_t;

static inline int
rqc_send_queue_can_write(rqc_send_queue_t *send_queue)
{
    if (send_queue->sndq_packets_used < send_queue->sndq_packets_used_max) {
        return RQC_TRUE;
    }
    return RQC_FALSE;
}

static inline rqc_bool_t
rqc_send_queue_release_enough_space(rqc_send_queue_t *send_queue)
{
    return (send_queue->sndq_packets_used_max - send_queue->sndq_packets_used)
            >= (send_queue->sndq_packets_used_max / RQC_SNDQ_RELEASE_ENOUGH_SPACE_TH);
}
uint64_t rqc_send_queue_get_unsent_packets_num(rqc_send_queue_t *send_queue);

rqc_send_queue_t *rqc_send_queue_create(rqc_connection_t *conn);
void rqc_send_queue_destroy(rqc_send_queue_t *send_queue);

void rqc_send_queue_destroy_packets_list(rqc_list_head_t *head);
void rqc_send_queue_pre_destroy_packets_list(rqc_send_queue_t *send_queue, rqc_list_head_t *head);

rqc_packet_out_t *rqc_send_queue_get_packet_out(rqc_send_queue_t *send_queue, unsigned need, rqc_pkt_type_t pkt_type);
rqc_packet_out_t *rqc_send_queue_get_packet_out_for_stream(rqc_send_queue_t *send_queue, unsigned need, rqc_pkt_type_t pkt_type,
    rqc_stream_t *stream);
int rqc_send_queue_out_queue_empty(rqc_send_queue_t *send_queue);

void rqc_send_queue_insert_send(rqc_packet_out_t *po, rqc_list_head_t *head, rqc_send_queue_t *send_queue);
void rqc_send_queue_remove_send(rqc_list_head_t *pos);

void rqc_send_queue_insert_lost(rqc_list_head_t *pos, rqc_list_head_t *head);
void rqc_send_queue_remove_lost(rqc_list_head_t *pos);

void rqc_send_queue_insert_free(rqc_packet_out_t *po, rqc_list_head_t *head, rqc_send_queue_t *send_queue);
void rqc_send_queue_remove_free(rqc_list_head_t *pos, rqc_send_queue_t *send_queue);

void rqc_send_queue_insert_buff(rqc_list_head_t *pos, rqc_list_head_t *head);
void rqc_send_queue_remove_buff(rqc_list_head_t *pos, rqc_send_queue_t *send_queue);

void rqc_send_queue_insert_probe(rqc_list_head_t *pos, rqc_list_head_t *head);
void rqc_send_queue_remove_probe(rqc_list_head_t *pos);

void rqc_send_queue_insert_unacked(rqc_packet_out_t *packet_out, rqc_list_head_t *head, rqc_send_queue_t *send_queue);
void rqc_send_queue_remove_unacked(rqc_packet_out_t *packet_out, rqc_send_queue_t *send_queue);

void rqc_send_queue_move_to_head(rqc_list_head_t *pos, rqc_list_head_t *head);
void rqc_send_queue_move_to_tail(rqc_list_head_t *pos, rqc_list_head_t *head);
void rqc_send_queue_move_to_high_pri(rqc_list_head_t *pos, rqc_send_queue_t *send_queue);

void rqc_send_queue_copy_to_lost(rqc_packet_out_t *packet_out, rqc_send_queue_t *send_queue, rqc_bool_t mark_retrans);
void rqc_send_queue_copy_to_probe(rqc_packet_out_t *packet_out, rqc_send_queue_t *send_queue, rqc_path_ctx_t *path);

void rqc_send_queue_drop_packets(rqc_connection_t *conn);
void rqc_send_queue_drop_stream_frame_packets(rqc_connection_t *conn, rqc_stream_id_t stream_id);

#endif /* _RQC_SEND_QUEUE_H_INCLUDED_ */
