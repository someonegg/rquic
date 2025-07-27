#include "src/transport/rqc_send_queue.h"
#include "src/transport/rqc_packet.h"
#include "src/transport/rqc_packet_out.h"
#include "src/transport/rqc_conn.h"
#include "src/common/rqc_memory_pool.h"
#include "src/transport/rqc_utils.h"
#include "src/transport/rqc_multipath.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/transport/rqc_stream.h"
#include "src/transport/rqc_conn.h"

rqc_send_queue_t *
rqc_send_queue_create(rqc_connection_t *conn)
{
    rqc_send_queue_t *send_queue = rqc_pcalloc(conn->conn_pool, sizeof(rqc_send_queue_t));
    if (send_queue == NULL) {
        return NULL;
    }

    rqc_init_list_head(&send_queue->sndq_send_packets);
    rqc_init_list_head(&send_queue->sndq_send_packets_high_pri);
    rqc_init_list_head(&send_queue->sndq_unacked_packets);

    rqc_init_list_head(&send_queue->sndq_lost_packets);
    rqc_init_list_head(&send_queue->sndq_free_packets);
    rqc_init_list_head(&send_queue->sndq_buff_1rtt_packets);
    rqc_init_list_head(&send_queue->sndq_pto_probe_packets);

    if (conn->conn_settings.sndq_packets_used_max > 0) {
        send_queue->sndq_packets_used_max = conn->conn_settings.sndq_packets_used_max;
    } else {
        send_queue->sndq_packets_used_max = RQC_SNDQ_PACKETS_USED_MAX;
    }

    send_queue->sndq_conn = conn;

    send_queue->sndq_packets_in_unacked_list = 0;

    return send_queue;
}

void
rqc_send_queue_destroy_packets_list(rqc_list_head_t *head)
{
    rqc_list_head_t *pos, *next;
    rqc_packet_out_t *packet_out;
    rqc_list_for_each_safe(pos, next, head) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
        rqc_list_del_init(pos);
        rqc_packet_out_destroy(packet_out);
    }
}

void
rqc_send_queue_destroy(rqc_send_queue_t *send_queue)
{
    rqc_send_queue_destroy_packets_list(&send_queue->sndq_send_packets);
    rqc_send_queue_destroy_packets_list(&send_queue->sndq_send_packets_high_pri);
    rqc_send_queue_destroy_packets_list(&send_queue->sndq_unacked_packets);

    rqc_send_queue_destroy_packets_list(&send_queue->sndq_lost_packets);
    rqc_send_queue_destroy_packets_list(&send_queue->sndq_free_packets);
    rqc_send_queue_destroy_packets_list(&send_queue->sndq_buff_1rtt_packets);
    rqc_send_queue_destroy_packets_list(&send_queue->sndq_pto_probe_packets);

    send_queue->sndq_packets_used = 0;
    send_queue->sndq_packets_used_bytes = 0;
    send_queue->sndq_packets_free = 0;
    send_queue->sndq_packets_in_unacked_list = 0;
}

void
rqc_send_queue_pre_destroy_packets_list(rqc_send_queue_t *send_queue, rqc_list_head_t *head)
{
    rqc_list_head_t *pos, *next;
    rqc_list_for_each_safe(pos, next, head) {
        rqc_list_del_init(pos);
        rqc_list_add_tail(pos, &send_queue->sndq_free_packets);
    }
}

void
rqc_send_queue_pre_destroy(rqc_send_queue_t *send_queue)
{
    rqc_send_queue_pre_destroy_packets_list(send_queue, &send_queue->sndq_send_packets);
    rqc_send_queue_pre_destroy_packets_list(send_queue, &send_queue->sndq_send_packets_high_pri);
    rqc_send_queue_pre_destroy_packets_list(send_queue, &send_queue->sndq_unacked_packets);

    rqc_send_queue_pre_destroy_packets_list(send_queue, &send_queue->sndq_lost_packets);
    rqc_send_queue_pre_destroy_packets_list(send_queue, &send_queue->sndq_buff_1rtt_packets);
    rqc_send_queue_pre_destroy_packets_list(send_queue, &send_queue->sndq_pto_probe_packets);

    send_queue->sndq_packets_used = 0;
    send_queue->sndq_packets_used_bytes = 0;
    send_queue->sndq_packets_free = 0;
    send_queue->sndq_packets_in_unacked_list = 0;
}

rqc_packet_out_t *
rqc_send_queue_get_packet_out(rqc_send_queue_t *send_queue, unsigned need, rqc_pkt_type_t pkt_type)
{
    rqc_packet_out_t *packet_out;
    rqc_list_head_t  *pos;

    rqc_list_for_each_reverse(pos, &send_queue->sndq_send_packets) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
        if (packet_out->po_pkt.pkt_type == pkt_type
            && rqc_get_po_remained_size(packet_out) >= need)
        {
            return packet_out;
        }
    }

    packet_out = rqc_packet_out_get_and_insert_send(send_queue, pkt_type);
    if (packet_out == NULL) {
        return NULL;
    }

    return packet_out;
}

rqc_packet_out_t *
rqc_send_queue_get_packet_out_for_stream(rqc_send_queue_t *send_queue, unsigned need, rqc_pkt_type_t pkt_type,
    rqc_stream_t *stream)
{
    rqc_packet_out_t *packet_out;
    rqc_list_head_t  *pos;
    rqc_list_head_t  *list = &send_queue->sndq_send_packets;
    if (stream->stream_priority == RQC_STREAM_PRI_HIGH) {
        list = &send_queue->sndq_send_packets_high_pri;
    }

    rqc_list_for_each_reverse(pos, list) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
        if (packet_out->po_pkt.pkt_type == pkt_type
            && rqc_get_po_remained_size(packet_out) >= need
            && packet_out->po_stream_frames_idx < RQC_MAX_STREAM_FRAME_IN_PO
            && packet_out->po_stream_frames_idx > 0
            /* Avoid Head-of-Line blocking. */
            && packet_out->po_stream_frames[packet_out->po_stream_frames_idx - 1].ps_stream_id == stream->stream_id)
        {
            return packet_out;
        }
        /* Only try to fill the last packet now */
        break;
    }

    packet_out = rqc_packet_out_get_and_insert_send(send_queue, pkt_type);
    if (packet_out == NULL) {
        return NULL;
    }

    if (stream->stream_priority == RQC_STREAM_PRI_HIGH) {
        rqc_send_queue_move_to_high_pri(&packet_out->po_list, send_queue);
    }

    return packet_out;
}

int rqc_send_queue_out_queue_empty(rqc_send_queue_t *send_queue)
{
    int empty;
    empty = rqc_list_empty(&send_queue->sndq_send_packets)
            && rqc_list_empty(&send_queue->sndq_send_packets_high_pri)
            && rqc_list_empty(&send_queue->sndq_lost_packets)
            && rqc_list_empty(&send_queue->sndq_pto_probe_packets)
            && rqc_list_empty(&send_queue->sndq_buff_1rtt_packets)
            && rqc_list_empty(&send_queue->sndq_unacked_packets);
    if (!empty) {
        return empty;
    }

    rqc_path_ctx_t *path = send_queue->sndq_conn->the_path;
    if (path) {
        for (rqc_send_type_t type = 0; type < RQC_SEND_TYPE_N; type++) {
            empty = empty && rqc_list_empty(&path->path_schedule_buf[type]);
        }
    }

    return empty;
}

void
rqc_send_queue_insert_send(rqc_packet_out_t *po, rqc_list_head_t *head, rqc_send_queue_t *send_queue)
{
    rqc_list_add_tail(&po->po_list, head);
    send_queue->sndq_packets_used++;
}

void
rqc_send_queue_remove_send(rqc_list_head_t *pos)
{
    rqc_list_del_init(pos);
}

void
rqc_send_queue_insert_lost(rqc_list_head_t *pos, rqc_list_head_t *head)
{
    rqc_list_add_tail(pos, head);
}

void
rqc_send_queue_remove_lost(rqc_list_head_t *pos)
{
    rqc_list_del_init(pos);
}

void
rqc_send_queue_insert_free(rqc_packet_out_t *po, rqc_list_head_t *head, rqc_send_queue_t *send_queue)
{
    if (po->po_pr) {
        if (po->po_pr->ref_cnt <= 1) {
            rqc_conn_destroy_ping_record(po->po_pr);

        } else {
            po->po_pr->ref_cnt--;
            po->po_pr = NULL;
        }
    }
    rqc_list_add_tail(&po->po_list, head);
    send_queue->sndq_packets_free++;
    send_queue->sndq_packets_used--;
}

void
rqc_send_queue_remove_free(rqc_list_head_t *pos, rqc_send_queue_t *send_queue)
{
    rqc_list_del_init(pos);
    send_queue->sndq_packets_free--;
}

void
rqc_send_queue_insert_buff(rqc_list_head_t *pos, rqc_list_head_t *head)
{
    rqc_list_add_tail(pos, head);
}

void
rqc_send_queue_remove_buff(rqc_list_head_t *pos, rqc_send_queue_t *send_queue)
{
    rqc_list_del_init(pos);
    send_queue->sndq_packets_used--;
}

void
rqc_send_queue_insert_probe(rqc_list_head_t *pos, rqc_list_head_t *head)
{
    rqc_list_add_tail(pos, head);
}

void
rqc_send_queue_remove_probe(rqc_list_head_t *pos)
{
    rqc_list_del_init(pos);
}

void
rqc_send_queue_insert_unacked(rqc_packet_out_t *packet_out, rqc_list_head_t *head, rqc_send_queue_t *send_queue)
{
    rqc_connection_t *conn = send_queue->sndq_conn;
    rqc_list_add_tail(&packet_out->po_list, head);
    if (!(packet_out->po_flag & RQC_POF_IN_UNACK_LIST)) {
        send_queue->sndq_packets_in_unacked_list++;
        packet_out->po_flag |= RQC_POF_IN_UNACK_LIST;
        if (send_queue->sndq_packets_in_unacked_list > RQC_SNDQ_MAX_UNACK_PACKETS_LIMIT) {
            if (conn) {
                RQC_CONN_ERR(conn, RQC_ELIMIT);
                rqc_log(conn->log, RQC_LOG_ERROR,
                        "|sndq unack packets exceed|sndq_packets_in_unacked_list:%ui|",
                        send_queue->sndq_packets_in_unacked_list);
            }
        }
    }
}

void
rqc_send_queue_remove_unacked(rqc_packet_out_t *packet_out, rqc_send_queue_t *send_queue)
{
    rqc_list_del_init(&packet_out->po_list);
    /* @FIXED:
     * It is possible that the packet_out is not in the unacked list (e.g. in path buffer).
     * So, sndq_packets_in_unacked_list is incorrect sometimes.
     * Now, we use it to estimate unsent bytes. so, it's not gonna make fatal errors.
     * But, we must find a way to fix it.
     */
    if (packet_out->po_flag & RQC_POF_IN_UNACK_LIST) {
        if (send_queue->sndq_packets_in_unacked_list == 0) {
            rqc_log(send_queue->sndq_conn->log, RQC_LOG_ERROR, "|the_number_of_unacked_packets_in_sndq_will_become_negative!|");
            return;
        }
        send_queue->sndq_packets_in_unacked_list--;
        packet_out->po_flag &= ~RQC_POF_IN_UNACK_LIST;
    }
}

uint64_t
rqc_send_queue_get_unsent_packets_num(rqc_send_queue_t *send_queue)
{
    if (send_queue->sndq_packets_in_unacked_list > send_queue->sndq_packets_used) {
        rqc_log(send_queue->sndq_conn->log, RQC_LOG_ERROR, "|more_unacked_packets_than_used_packets|");
        return 0;
    }
    return send_queue->sndq_packets_used - send_queue->sndq_packets_in_unacked_list;
}

void
rqc_send_queue_move_to_head(rqc_list_head_t *pos, rqc_list_head_t *head)
{
    rqc_list_del_init(pos);
    rqc_list_add(pos, head);
}

void
rqc_send_queue_move_to_tail(rqc_list_head_t *pos, rqc_list_head_t *head)
{
    rqc_list_del_init(pos);
    rqc_list_add_tail(pos, head);
}

void
rqc_send_queue_move_to_high_pri(rqc_list_head_t *pos, rqc_send_queue_t *send_queue)
{
    rqc_list_del_init(pos);
    rqc_list_add_tail(pos, &send_queue->sndq_send_packets_high_pri);
}

void
rqc_send_queue_copy_to_lost(rqc_packet_out_t *packet_out, rqc_send_queue_t *send_queue, rqc_bool_t mark_retrans)
{
    rqc_connection_t *conn = send_queue->sndq_conn;

    rqc_packet_out_t *new_po = rqc_packet_out_get(send_queue);
    if (!new_po) {
        RQC_CONN_ERR(conn, RQC_EMALLOC);
        return;
    }

    rqc_packet_out_copy(new_po, packet_out);
    rqc_packet_out_remove_ack_frame(new_po);

    rqc_send_queue_insert_lost(&new_po->po_list, &send_queue->sndq_lost_packets);
    send_queue->sndq_packets_used++;
    if (mark_retrans) {
        packet_out->po_flag |= RQC_POF_RETRANSED;
    }
    new_po->po_flag &= ~RQC_POF_RETRANSED;
    new_po->po_flag &= ~RQC_POF_SPURIOUS_LOSS;
}

void
rqc_send_queue_copy_to_probe(rqc_packet_out_t *packet_out, rqc_send_queue_t *send_queue, rqc_path_ctx_t *path)
{
    rqc_connection_t *conn = send_queue->sndq_conn;

    rqc_packet_out_t *new_po = rqc_packet_out_get(send_queue);
    if (!new_po) {
        RQC_CONN_ERR(conn, RQC_EMALLOC);
        return;
    }

    rqc_packet_out_copy(new_po, packet_out);
    rqc_packet_out_remove_ack_frame(new_po);

    rqc_send_queue_insert_probe(&new_po->po_list, &send_queue->sndq_pto_probe_packets);
    send_queue->sndq_packets_used++;
    packet_out->po_flag |= RQC_POF_RETRANSED;
    new_po->po_flag &= ~RQC_POF_RETRANSED;
    new_po->po_flag &= ~RQC_POF_SPURIOUS_LOSS;
}

/* Called when conn is ready to close */
void
rqc_send_queue_drop_packets(rqc_connection_t *conn)
{
    rqc_send_queue_t *send_queue = conn->conn_send_queue;
    rqc_send_queue_pre_destroy(send_queue);

    rqc_path_ctx_t *path = conn->the_path;
    if (path) {
        path->path_send_ctl->ctl_bytes_in_flight = 0;
        path->path_send_ctl->ctl_bytes_ack_eliciting_inflight = 0;
        rqc_path_schedule_buf_pre_destroy(send_queue, path);
    }
}

int
rqc_send_ctl_stream_frame_can_drop(rqc_packet_out_t *packet_out, rqc_stream_id_t stream_id)
{
    int drop = 0;
    /*
     * Attached ACK could lead to a situation
     * where an original packet (w/o ACK) can be removed but the corresponding
     * replicated packet (w/ ACK) cannot be removed. This
     * ultimately causes that the po_origin of the replicated packet (R) points to a new
     * packet (N) to which the buffer of the original packet is reallocated. This is
     * very rare but may lead to a infinite loop or crash when the unacked list
     * in rqc_send_ctl_detect_lost is traversed. For example, when N is next to R in the unacked list,
     * removing R may also free N via rqc_send_ctl_indirectly_ack_or_drop_po. If that
     * happens, an infinite loop that traversing the free_packets list is triggered.
     */
    uint64_t mask = ~(RQC_FRAME_BIT_STREAM | RQC_FRAME_BIT_ACK);
    if ((packet_out->po_frame_types & mask) == 0) {
        drop = 0;
        for (int i = 0; i < RQC_MAX_STREAM_FRAME_IN_PO; i++) {
            if (packet_out->po_stream_frames[i].ps_is_used == 0) {
                break;
            }
            if (packet_out->po_stream_frames[i].ps_stream_id == stream_id) {
                drop = 1;

            } else {
                drop = 0;
                break;
            }
        }
    }
    return drop;
}

void
rqc_send_queue_drop_stream_frame_packets(rqc_connection_t *conn, rqc_stream_id_t stream_id)
{
    rqc_send_queue_t *send_queue = conn->conn_send_queue;
    rqc_list_head_t *pos, *next;
    rqc_packet_out_t *packet_out;
    int drop;
    int count = 0;
    int to_drop = 0;

    /*
     * The previous code was too complicated. Now, we want to keep it as simple
     * as possible. To do so, we just drop all pkts belonging to the closed stream
     * and decrease inflight on corresponding paths carefully. This could have minor
     * impacts on congestion controllers. But, it is ok.
     */

    rqc_list_for_each_safe(pos, next, &send_queue->sndq_unacked_packets) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
        drop = rqc_send_ctl_stream_frame_can_drop(packet_out, stream_id);
        if (drop) {
            count++;
            rqc_send_ctl_decrease_inflight(conn, packet_out);
            rqc_send_queue_remove_unacked(packet_out, send_queue);
            rqc_send_queue_insert_free(packet_out, &send_queue->sndq_free_packets, send_queue);
        }
    }

    rqc_list_for_each_safe(pos, next, &send_queue->sndq_send_packets) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
        drop = rqc_send_ctl_stream_frame_can_drop(packet_out, stream_id);
        if (drop) {
            count++;
            rqc_send_queue_remove_send(pos);
            rqc_send_queue_insert_free(packet_out, &send_queue->sndq_free_packets, send_queue);
        }
    }

    rqc_list_for_each_safe(pos, next, &send_queue->sndq_lost_packets) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
        drop = rqc_send_ctl_stream_frame_can_drop(packet_out, stream_id);
        if (drop) {
            count++;
            rqc_send_queue_remove_lost(pos);
            rqc_send_queue_insert_free(packet_out, &send_queue->sndq_free_packets, send_queue);
        }
    }

    rqc_list_for_each_safe(pos, next, &send_queue->sndq_pto_probe_packets) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
        drop = rqc_send_ctl_stream_frame_can_drop(packet_out, stream_id);
        if (drop) {
            count++;
            rqc_send_queue_remove_probe(pos);
            rqc_send_queue_insert_free(packet_out, &send_queue->sndq_free_packets, send_queue);
        }
    }

    rqc_path_ctx_t *path = conn->the_path;
    if (path) {
        rqc_list_for_each_safe(pos, next, &path->path_schedule_buf[RQC_SEND_TYPE_NORMAL]) {
            packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
            drop = rqc_send_ctl_stream_frame_can_drop(packet_out, stream_id);
            if (drop) {
                count++;
                rqc_path_send_buffer_remove(path, packet_out);
                rqc_send_queue_insert_free(packet_out, &send_queue->sndq_free_packets, send_queue);
            }
        }

        rqc_list_for_each_safe(pos, next, &path->path_schedule_buf[RQC_SEND_TYPE_RETRANS]) {
            packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
            drop = rqc_send_ctl_stream_frame_can_drop(packet_out, stream_id);
            if (drop) {
                count++;
                rqc_path_send_buffer_remove(path, packet_out);
                rqc_send_queue_insert_free(packet_out, &send_queue->sndq_free_packets, send_queue);
            }
        }

        rqc_list_for_each_safe(pos, next, &path->path_schedule_buf[RQC_SEND_TYPE_PTO_PROBE]) {
            packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
            drop = rqc_send_ctl_stream_frame_can_drop(packet_out, stream_id);
            if (drop) {
                count++;
                rqc_path_send_buffer_remove(path, packet_out);
                rqc_send_queue_insert_free(packet_out, &send_queue->sndq_free_packets, send_queue);
            }
        }
    }

    if (count > 0) {
        rqc_log(conn->log, RQC_LOG_INFO, "|stream_id:%ui|to_drop: %d|count:%d|", stream_id, to_drop, count);
    }
}
