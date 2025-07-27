/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "src/common/utils/vint/rqc_variable_len_int.h"
#include "src/transport/rqc_packet_out.h"
#include "src/transport/rqc_conn.h"
#include "src/common/rqc_memory_pool.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/transport/rqc_frame_parser.h"
#include "src/transport/rqc_packet_parser.h"
#include "src/transport/rqc_stream.h"
#include "src/transport/rqc_utils.h"
#include "src/transport/rqc_engine.h"
#include "src/transport/rqc_multipath.h"
#include "src/transport/rqc_packet_out.h"
#include "src/transport/rqc_cid.h"

rqc_packet_out_t *
rqc_packet_out_create(size_t po_buf_size)
{
    rqc_packet_out_t *packet_out;
    packet_out = rqc_calloc(1, sizeof(rqc_packet_out_t));
    if (!packet_out) {
        goto error;
    }

    packet_out->po_buf = rqc_malloc(RQC_PACKET_OUT_BUF_CAP);
    if (!packet_out->po_buf) {
        goto error;
    }

    packet_out->po_buf_cap = RQC_PACKET_OUT_BUF_CAP;
    packet_out->po_buf_size = po_buf_size;

    return packet_out;

error:
    if (packet_out) {
        rqc_free(packet_out->po_buf);
        rqc_free(packet_out);
    }
    return NULL;
}

static inline rqc_bool_t
rqc_packet_out_can_attach_ack(rqc_packet_out_t *po, rqc_pkt_type_t pkt_type)
{
    if (po->po_pkt.pkt_type != pkt_type) {
        return RQC_FALSE;
    }

    if (po->po_frame_types & (RQC_FRAME_BIT_ACK)) {
        return RQC_FALSE;
    }

    if (po->po_flag & RQC_POF_STREAM_NO_LEN) {
        return RQC_FALSE;
    }

    return RQC_TRUE;
}

void
rqc_packet_out_remove_ack_frame(rqc_packet_out_t *po)
{
    if (po->po_frame_types & RQC_FRAME_BIT_ACK)
    {
        po->po_used_size = po->po_ack_offset;
        po->po_frame_types &= ~(RQC_FRAME_BIT_ACK);
    }
}

void
rqc_packet_out_copy(rqc_packet_out_t *dst, rqc_packet_out_t *src)
{
    unsigned char *po_buf = dst->po_buf;
    // size_t cap = dst->po_buf_cap;
    // unsigned int size = dst->po_buf_size;
    rqc_memcpy(dst, src, sizeof(rqc_packet_out_t));
    dst->po_origin_ref_cnt = 0;

    rqc_packet_out_t *origin = src->po_origin == NULL ? src : src->po_origin;

    /* pointers should carefully assigned in rqc_packet_out_copy */
    dst->po_buf = po_buf;
    rqc_memcpy(dst->po_buf, src->po_buf, src->po_used_size);
    if (src->po_ppktno) {
        dst->po_ppktno = dst->po_buf + (src->po_ppktno - src->po_buf);
    }
    if (src->po_payload) {
        dst->po_payload = dst->po_buf + (src->po_payload - src->po_buf);
    }
    if (src->po_padding) {
        dst->po_padding = dst->po_buf + (src->po_padding - src->po_buf);
    }
    dst->po_origin = origin;
    origin->po_origin_ref_cnt++;
    dst->po_user_data = src->po_user_data;

    dst->po_flag &= ~RQC_POF_IN_UNACK_LIST;
    dst->po_flag &= ~RQC_POF_IN_PATH_BUF_LIST;

    dst->po_pr = src->po_pr;

    if (dst->po_pr) {
        dst->po_pr->ref_cnt++;
    }

    dst->po_send_cwnd_blk_ts = 0;
    dst->po_sched_cwnd_blk_ts = 0;
    dst->po_send_pacing_blk_ts = 0;
}

rqc_packet_out_t *
rqc_packet_out_get(rqc_send_queue_t *send_queue)
{
    rqc_packet_out_t *packet_out;
    unsigned int buf_size;
    size_t buf_cap;
    rqc_list_head_t *pos, *next;

    rqc_list_for_each_safe(pos, next, &send_queue->sndq_free_packets) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
        rqc_send_queue_remove_free(pos, send_queue);

        unsigned char *tmp = packet_out->po_buf;
        buf_size = send_queue->sndq_conn->pkt_out_size;
        buf_cap = packet_out->po_buf_cap;
        memset(packet_out, 0, sizeof(rqc_packet_out_t));
        packet_out->po_buf = tmp;
        packet_out->po_buf_size = buf_size;
        packet_out->po_buf_cap = buf_cap;
        goto return_po;
    }

    packet_out = rqc_packet_out_create(send_queue->sndq_conn->pkt_out_size);
    if (!packet_out) {
        return NULL;
    }

return_po:
    return packet_out;
}

rqc_packet_out_t *
rqc_packet_out_get_and_insert_send(rqc_send_queue_t *send_queue, enum rqc_pkt_type pkt_type)
{
    rqc_packet_out_t *packet_out;
    packet_out = rqc_packet_out_get(send_queue);
    if (!packet_out) {
        return NULL;
    }

    packet_out->po_pkt.pkt_type = pkt_type;

    /* generate packet number when send */
    packet_out->po_pkt.pkt_num = 0;

    rqc_send_queue_insert_send(packet_out, &send_queue->sndq_send_packets, send_queue);

    return packet_out;
}

void
rqc_packet_out_destroy(rqc_packet_out_t *packet_out)
{
    rqc_free(packet_out->po_buf);
    rqc_free(packet_out);
}

void
rqc_maybe_recycle_packet_out(rqc_packet_out_t *packet_out, rqc_connection_t *conn)
{
    /* recycle packetout if no frame in it */
    if (packet_out->po_frame_types == 0) {
        rqc_list_del_init(&packet_out->po_list);
        rqc_send_queue_insert_free(packet_out, &conn->conn_send_queue->sndq_free_packets, conn->conn_send_queue);
    }
}

int
rqc_write_packet_header(rqc_connection_t *conn, rqc_packet_out_t *packet_out)
{
    if (packet_out->po_used_size > 0) {
        return RQC_OK;
    }

    ssize_t ret = RQC_OK;

    rqc_pkt_type_t pkt_type = packet_out->po_pkt.pkt_type;

    if (pkt_type == RQC_PTYPE_SHORT_HEADER && packet_out->po_used_size == 0) {
        ret = rqc_gen_short_packet_header(packet_out,
                                          conn->dcid_set.current_dcid.cid_buf, conn->dcid_set.current_dcid.cid_len,
                                          RQC_PKTNO_CLASS, packet_out->po_pkt.pkt_num);

    } else if (pkt_type != RQC_PTYPE_SHORT_HEADER && packet_out->po_used_size == 0) {
        ret = rqc_gen_long_packet_header(packet_out,
                                         conn->dcid_set.current_dcid.cid_buf, conn->dcid_set.current_dcid.cid_len,
                                         conn->scid_set.user_scid.cid_buf, conn->scid_set.user_scid.cid_len,
                                         conn->version, RQC_PKTNO_CLASS);
    }

    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|gen header error|%z|", ret);
        return ret;
    }
    packet_out->po_used_size += ret;

    return RQC_OK;
}

rqc_packet_out_t *
rqc_write_new_packet(rqc_connection_t *conn, rqc_pkt_type_t pkt_type)
{
    int ret;
    rqc_packet_out_t *packet_out;

    if (pkt_type == RQC_PTYPE_NUM) {
        pkt_type = rqc_state_to_pkt_type(conn);
    }

    packet_out = rqc_packet_out_get_and_insert_send(conn->conn_send_queue, pkt_type);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_packet_out_get_and_insert_send error|");
        return NULL;
    }

    if (packet_out->po_used_size == 0) {
        ret = rqc_write_packet_header(conn, packet_out);
        if (ret) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_packet_header error|");
            goto error;
        }
    }

    return packet_out;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return NULL;
}

rqc_packet_out_t *
rqc_write_packet(rqc_connection_t *conn, rqc_pkt_type_t pkt_type, unsigned need)
{
    int ret;
    rqc_packet_out_t *packet_out;

    if (pkt_type == RQC_PTYPE_NUM) {
        pkt_type = rqc_state_to_pkt_type(conn);
    }

    packet_out = rqc_send_queue_get_packet_out(conn->conn_send_queue, need, pkt_type);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_send_queue_get_packet_out error|");
        return NULL;
    }

    if (packet_out->po_used_size == 0) {
        ret = rqc_write_packet_header(conn, packet_out);
        if (ret) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_packet_header error|");
            goto error;
        }
    }

    return packet_out;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return NULL;
}

rqc_packet_out_t *
rqc_write_packet_for_stream(rqc_connection_t *conn, rqc_pkt_type_t pkt_type, unsigned need, rqc_stream_t *stream)
{
    int ret;
    rqc_packet_out_t *packet_out;

    if (pkt_type == RQC_PTYPE_NUM) {
        pkt_type = rqc_state_to_pkt_type(conn);
    }

    packet_out = rqc_send_queue_get_packet_out_for_stream(conn->conn_send_queue, need, pkt_type, stream);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_send_queue_get_packet_out_for_stream error|");
        return NULL;
    }

    if (packet_out->po_used_size == 0) {
        ret = rqc_write_packet_header(conn, packet_out);
        if (ret) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_packet_header error|");
            goto error;
        }
    }

    return packet_out;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return NULL;
}

static int
rqc_write_ack_to_one_packet(rqc_connection_t *conn, rqc_packet_out_t *packet_out)
{
    ssize_t ret;
    int has_gap;
    rqc_packet_number_t largest_ack;
    rqc_usec_t now = rqc_monotonic_timestamp();

    rqc_path_ctx_t *path = conn->the_path;
    rqc_pn_ctl_t *pn_ctl = rqc_get_pn_ctl(conn, path);

    ret = rqc_gen_ack_frame(conn, packet_out, now, conn->local_settings.ack_delay_exponent,
                            &pn_ctl->ctl_recv_record, path->path_send_ctl->ctl_largest_recv_time,
                            &has_gap, &largest_ack);
    if (ret < 0) {
        goto error;
    }

    packet_out->po_ack_offset = packet_out->po_used_size;
    packet_out->po_used_size += ret;
    packet_out->po_largest_ack = largest_ack;

    path->path_send_ctl->ctl_ack_eliciting_pkt = 0;
    if (has_gap) {
        conn->conn_flag |= RQC_CONN_FLAG_ACK_HAS_GAP;
    } else {
        conn->conn_flag &= ~RQC_CONN_FLAG_ACK_HAS_GAP;
    }
    path->path_flag &= ~RQC_PATH_FLAG_SHOULD_ACK;
    conn->ack_flag &= ~(1 << path->path_id);

    path->path_send_ctl->ctl_ack_sent_cnt++;

    return RQC_OK;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return ret;
}

rqc_int_t
rqc_write_ack_to_packets(rqc_connection_t *conn)
{
    rqc_packet_out_t *packet_out;
    rqc_pkt_type_t pkt_type;
    rqc_list_head_t *pos, *next;
    int ret = RQC_OK;

    rqc_path_ctx_t *path = conn->the_path;

    if (path == NULL
        || path->path_state < RQC_PATH_STATE_VALIDATING
        || path->path_state >= RQC_PATH_STATE_CLOSED
        || !(path->path_flag & RQC_PATH_FLAG_SHOULD_ACK))
    {
        return RQC_OK;
    }

    /* Check if any ack should be sent in current path */
    rqc_pn_ctl_t *pn_ctl = rqc_get_pn_ctl(conn, path);
    rqc_pktno_range_node_t *first_range = NULL;
    rqc_list_head_t *tmp_pos, *tmp_next;
    rqc_list_for_each_safe(tmp_pos, tmp_next, &pn_ctl->ctl_recv_record.list_head) {
        first_range = rqc_list_entry(tmp_pos, rqc_pktno_range_node_t, list);
        break;
    }

    if (first_range == NULL) {
        return RQC_OK;
    }

    pkt_type = rqc_state_to_pkt_type(conn);

    /* Try to attach ack to packet_out in path_buffer */
    rqc_list_for_each_safe(pos, next, &path->path_schedule_buf[RQC_SEND_TYPE_NORMAL]) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);

        if (rqc_packet_out_can_attach_ack(packet_out, pkt_type)) {
            ret = rqc_write_ack_to_one_packet(conn, packet_out);
            if (ret == -RQC_ENOBUF) {
                goto write_new;
            } else if (ret == RQC_OK) {
                goto done;
            } else {
                return ret;
            }
        }
        goto write_new;
    }

    rqc_list_for_each_safe(pos, next, &conn->conn_send_queue->sndq_send_packets) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);

        if (rqc_packet_out_can_attach_ack(packet_out, pkt_type)) {
            ret = rqc_write_ack_to_one_packet(conn, packet_out);
            if (ret == -RQC_ENOBUF) {
                goto write_new;
            } else if (ret == RQC_OK) {
                goto done;
            } else {
                return ret;
            }
        }
        goto write_new;
    }

write_new:
    packet_out = rqc_write_new_packet(conn, pkt_type);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }
    ret = rqc_write_ack_to_one_packet(conn, packet_out);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_ack_to_one_packet write to new packet error|ret:%d|", ret);
        return ret;
    }
    /* send ack packet first */
    rqc_send_queue_move_to_high_pri(&packet_out->po_list, conn->conn_send_queue);

done:
    return RQC_OK;
}

int
rqc_write_ping_to_packet(rqc_connection_t *conn,
    void *po_user_data, rqc_bool_t notify, rqc_ping_record_t *pr)
{
    ssize_t ret;
    rqc_packet_out_t *packet_out;

    packet_out = rqc_write_new_packet(conn, RQC_PTYPE_NUM);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    ret = rqc_gen_ping_frame(packet_out);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_ping_frame error|");
        goto error;
    }

    packet_out->po_user_data = po_user_data;
    packet_out->po_used_size += ret;

    /*
     * rquic supports inner PING and user PING, user PING shall be notified
     * to upper level while inner PING shall not.  if RQC_POF_NOTIFY is not set,
     * it's an inner PING, do no callback
     */
    if (notify) {
        packet_out->po_flag |= RQC_POF_NOTIFY;
        if (pr) {
            packet_out->po_pr = pr;
            pr->ref_cnt++;
        }
    }

    conn->conn_flag &= ~RQC_CONN_FLAG_PING;

    rqc_send_queue_move_to_high_pri(&packet_out->po_list, conn->conn_send_queue);
    return RQC_OK;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return ret;
}

int
rqc_write_conn_close_to_packet(rqc_connection_t *conn, uint64_t err_code)
{
    ssize_t ret;
    rqc_packet_out_t *packet_out;

    packet_out = rqc_write_new_packet(conn, RQC_PTYPE_NUM);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    ret = rqc_gen_conn_close_frame(packet_out, err_code, err_code >= REQUEST_NO_ERROR ? 1:0, 0);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_conn_close_frame error|");
        goto error;
    }

    packet_out->po_used_size += ret;

    rqc_send_queue_move_to_high_pri(&packet_out->po_list, conn->conn_send_queue);

    return RQC_OK;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return ret;
}

int
rqc_write_reset_stream_to_packet(rqc_connection_t *conn, rqc_stream_t *stream,
    uint64_t err_code, uint64_t final_size)
{
    ssize_t ret;
    rqc_packet_out_t *packet_out;
    rqc_pkt_type_t pkt_type = RQC_PTYPE_SHORT_HEADER;
    rqc_bool_t buff_reset = RQC_FALSE;

    if (!rqc_conn_is_handshake_done(conn)) {
        buff_reset = RQC_TRUE;
    }

    packet_out = rqc_write_new_packet(conn, pkt_type);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    ret = rqc_gen_reset_stream_frame(packet_out, stream->stream_id, err_code, final_size);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_reset_stream_frame error|");
        goto error;
    }
    stream->stream_err = err_code;

    packet_out->po_used_size += ret;

    /* new packet with index 0 */
    packet_out->po_stream_frames[0].ps_stream_id = stream->stream_id;
    packet_out->po_stream_frames[0].ps_is_reset = 1;
    packet_out->po_stream_frames[0].ps_is_used = 1;
    packet_out->po_stream_frames_idx++;
    if (stream->stream_state_send < RQC_SEND_STREAM_ST_RESET_SENT) {
        rqc_stream_send_state_update(stream, RQC_SEND_STREAM_ST_RESET_SENT);
    }

    if (stream->stream_stats.app_reset_time == 0) {
        stream->stream_stats.app_reset_time = rqc_monotonic_timestamp();
    }

    if (buff_reset) {
        rqc_conn_buff_1rtt_packet(conn, packet_out);
    }

    return RQC_OK;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return ret;
}

int
rqc_write_stop_sending_to_packet(rqc_connection_t *conn, rqc_stream_t *stream,
    uint64_t err_code)
{
    ssize_t ret;
    rqc_packet_out_t *packet_out;

    /*
     * A STOP_SENDING frame can be sent for streams in the Recv or Size
        Known states
     */
    if (stream->stream_state_recv >= RQC_RECV_STREAM_ST_DATA_RECVD) {
        rqc_log(conn->log, RQC_LOG_WARN, "|beyond DATA_RECVD|stream_state_recv:%d|", stream->stream_state_recv);
        return RQC_OK;
    }

    rqc_pkt_type_t pkt_type = RQC_PTYPE_SHORT_HEADER;
    rqc_bool_t buff_pkt = RQC_FALSE;

    if (!rqc_conn_is_handshake_done(conn)) {
        buff_pkt = RQC_TRUE;
    }

    packet_out = rqc_write_new_packet(conn, pkt_type);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    ret = rqc_gen_stop_sending_frame(packet_out, stream->stream_id, err_code);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_stop_sending_frame error|");
        goto error;
    }

    packet_out->po_used_size += ret;

    if (buff_pkt) {
        rqc_conn_buff_1rtt_packet(conn, packet_out);
    }

    return RQC_OK;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return -RQC_EWRITE_PKT;
}

int
rqc_write_data_blocked_to_packet(rqc_connection_t *conn, uint64_t data_limit)
{
    ssize_t ret;
    rqc_packet_out_t *packet_out;

    packet_out = rqc_write_new_packet(conn, RQC_PTYPE_SHORT_HEADER);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    ret = rqc_gen_data_blocked_frame(packet_out, data_limit);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_data_blocked_frame error|");
        goto error;
    }

    packet_out->po_used_size += ret;

    /* we need to send this packet asap */
    rqc_send_queue_move_to_high_pri(&packet_out->po_list, conn->conn_send_queue);

    return RQC_OK;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return -RQC_EWRITE_PKT;
}

int
rqc_write_stream_data_blocked_to_packet(rqc_connection_t *conn, rqc_stream_id_t stream_id, uint64_t stream_data_limit)
{
    ssize_t ret;
    rqc_packet_out_t *packet_out;
    packet_out = rqc_write_new_packet(conn, RQC_PTYPE_SHORT_HEADER);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    ret = rqc_gen_stream_data_blocked_frame(packet_out, stream_id, stream_data_limit);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_stream_data_blocked_frame error|");
        goto error;
    }

    packet_out->po_used_size += ret;

    /* we need to send this packet asap */
    rqc_send_queue_move_to_high_pri(&packet_out->po_list, conn->conn_send_queue);

    return RQC_OK;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return -RQC_EWRITE_PKT;
}

int
rqc_write_streams_blocked_to_packet(rqc_connection_t *conn, uint64_t stream_limit, int bidirectional)
{
    ssize_t ret;
    rqc_packet_out_t *packet_out;

    packet_out = rqc_write_new_packet(conn, RQC_PTYPE_NUM);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    ret = rqc_gen_streams_blocked_frame(packet_out, stream_limit, bidirectional);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_streams_blocked_frame error|");
        goto error;
    }

    packet_out->po_used_size += ret;

    rqc_send_queue_move_to_high_pri(&packet_out->po_list, conn->conn_send_queue);

    return RQC_OK;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return -RQC_EWRITE_PKT;
}

int
rqc_write_max_data_to_packet(rqc_connection_t *conn, uint64_t max_data)
{
    ssize_t ret;
    rqc_packet_out_t *packet_out;

    packet_out = rqc_write_new_packet(conn, RQC_PTYPE_SHORT_HEADER);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    ret = rqc_gen_max_data_frame(packet_out, max_data);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_max_data_frame error|");
        goto error;
    }

    packet_out->po_used_size += ret;

    rqc_send_queue_move_to_high_pri(&packet_out->po_list, conn->conn_send_queue);

    return RQC_OK;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return -RQC_EWRITE_PKT;
}

int
rqc_write_max_stream_data_to_packet(rqc_connection_t *conn, rqc_stream_id_t stream_id, uint64_t max_stream_data, rqc_pkt_type_t pkt_type)
{
    ssize_t ret = RQC_OK;
    rqc_packet_out_t *packet_out;

    packet_out = rqc_write_new_packet(conn, pkt_type);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    ret = rqc_gen_max_stream_data_frame(packet_out, stream_id, max_stream_data);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_max_stream_data_frame error|");
        goto error;
    }

    packet_out->po_used_size += ret;

    rqc_send_queue_move_to_high_pri(&packet_out->po_list, conn->conn_send_queue);

    return RQC_OK;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return -RQC_EWRITE_PKT;
}

int
rqc_write_max_streams_to_packet(rqc_connection_t *conn, uint64_t max_stream, int bidirectional)
{
    ssize_t ret = RQC_ERROR;
    rqc_packet_out_t *packet_out;

    if (max_stream > RQC_MAX_STREAMS) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_max_streams_to_packet error|set max_stream:%ui", max_stream);
        return -RQC_EPARAM;
    }

    packet_out = rqc_write_new_packet(conn, RQC_PTYPE_NUM);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    ret = rqc_gen_max_streams_frame(packet_out, max_stream, bidirectional);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_max_streams_frame error|");
        goto error;
    }

    packet_out->po_used_size += ret;

    rqc_send_queue_move_to_high_pri(&packet_out->po_list, conn->conn_send_queue);

    return RQC_OK;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return -RQC_EWRITE_PKT;
}

int
rqc_write_stream_frame_to_packet(rqc_connection_t *conn,
    rqc_stream_t *stream, rqc_pkt_type_t pkt_type, uint8_t fin,
    const unsigned char *payload, size_t payload_size,
    size_t *send_data_written)
{
    rqc_packet_out_t *packet_out;
    int n_written;

    /* increase recv window */
    rqc_usec_t max_srtt = 0;
    uint64_t available_window;

    if (conn->conn_settings.enable_stream_rate_limit
        && stream->stream_send_offset == 0
        && stream->stream_type == RQC_CLI_BID)
    {
        available_window = stream->stream_flow_ctl.fc_max_stream_data_can_recv - stream->stream_data_in.next_read_offset;

        if (stream->recv_rate_bytes_per_sec) {
            /* set window according to the rate limit */
            max_srtt = rqc_conn_get_max_srtt(conn);
            stream->stream_flow_ctl.fc_stream_recv_window_size = stream->recv_rate_bytes_per_sec * max_srtt / 1000000;
            stream->stream_flow_ctl.fc_stream_recv_window_size = rqc_max(conn->conn_settings.init_recv_window, stream->stream_flow_ctl.fc_stream_recv_window_size);
            stream->stream_flow_ctl.fc_stream_recv_window_size = rqc_min(RQC_MAX_RECV_WINDOW, stream->stream_flow_ctl.fc_stream_recv_window_size);
        } else {
            /* set window to RQC_MAX_RECV_WINDOW */
            stream->stream_flow_ctl.fc_stream_recv_window_size = RQC_MAX_RECV_WINDOW;
        }

        if (stream->stream_flow_ctl.fc_stream_recv_window_size > available_window) {
            stream->stream_flow_ctl.fc_max_stream_data_can_recv += (stream->stream_flow_ctl.fc_stream_recv_window_size - available_window);
            rqc_write_max_stream_data_to_packet(conn, stream->stream_id, stream->stream_flow_ctl.fc_max_stream_data_can_recv, pkt_type);
        }
    }

    /* We need 25 bytes for stream frame header at most, and left bytes for stream data.
     * It's a trade-off value, bigger need bytes for higher payload rate. */
    const unsigned need = 50;
    packet_out = rqc_write_packet_for_stream(conn, pkt_type, need, stream);
    if (packet_out == NULL) {
        return -RQC_EWRITE_PKT;
    }

    n_written = rqc_gen_stream_frame(packet_out,
                                     stream->stream_id, stream->stream_send_offset, fin,
                                     payload,
                                     payload_size,
                                     send_data_written);
    if (n_written < 0) {
        rqc_maybe_recycle_packet_out(packet_out, conn);
        return n_written;
    }
    stream->stream_send_offset += *send_data_written;
    stream->stream_conn->conn_flow_ctl.fc_data_sent += *send_data_written;
    packet_out->po_used_size += n_written;
    packet_out->po_stream_id = stream->stream_id;
    packet_out->po_stream_offset = stream->stream_send_offset;

    if (fin && *send_data_written == payload_size) {
        stream->stream_flag |= RQC_STREAM_FLAG_FIN_WRITE;
        stream->stream_stats.local_fin_write_time = rqc_monotonic_timestamp();
    }

    if (!stream->stream_stats.first_write_time) {
        stream->stream_stats.first_write_time = rqc_monotonic_timestamp();
    }
    return RQC_OK;
}

rqc_int_t
rqc_write_path_challenge_frame_to_packet(rqc_connection_t *conn, rqc_path_ctx_t *path)
{
    rqc_int_t ret = RQC_ERROR;

    rqc_packet_out_t *packet_out = rqc_write_new_packet(conn, RQC_PTYPE_SHORT_HEADER);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    ret = rqc_gen_path_challenge_frame(packet_out, path->path_challenge_data);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_path_challenge_frame error|%d|", ret);
        goto error;
    }

    packet_out->po_used_size += ret;

    rqc_send_queue_move_to_high_pri(&packet_out->po_list, conn->conn_send_queue);

    return RQC_OK;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return ret;
}

rqc_int_t
rqc_write_path_response_frame_to_packet(rqc_connection_t *conn, rqc_path_ctx_t *path, unsigned char *path_response_data)
{
    rqc_int_t ret = RQC_ERROR;

    rqc_packet_out_t *packet_out = rqc_write_new_packet(conn, RQC_PTYPE_SHORT_HEADER);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    ret = rqc_gen_path_response_frame(packet_out, path_response_data);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_path_response_frame error|%d|", ret);
        goto error;
    }

    packet_out->po_used_size += ret;

    rqc_send_queue_move_to_high_pri(&packet_out->po_list, conn->conn_send_queue);

    return RQC_OK;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return ret;
}

rqc_int_t
rqc_write_handshake_frame_to_packet(rqc_connection_t *conn,
    const unsigned char *alpn, size_t alpn_len, uint8_t *tp, size_t tp_len)
{
    rqc_int_t ret = RQC_ERROR;

    rqc_packet_out_t *packet_out = rqc_write_new_packet(conn, RQC_PTYPE_INIT);
    if (packet_out == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_new_packet error|");
        return -RQC_EWRITE_PKT;
    }

    ret = rqc_gen_handshake_frame(packet_out, alpn, alpn_len, tp, tp_len);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_gen_handshake_frame error|%d|", ret);
        goto error;
    }

    packet_out->po_used_size += ret;

    rqc_send_queue_move_to_high_pri(&packet_out->po_list, conn->conn_send_queue);

    return RQC_OK;

error:
    rqc_maybe_recycle_packet_out(packet_out, conn);
    return ret;
}

size_t
rqc_get_po_remained_size(rqc_packet_out_t *po)
{
    size_t res;

    res = po->po_buf_size - po->po_used_size;

    return rqc_max(res, 0);
}

size_t
rqc_get_po_remained_size_with_ack_spc(rqc_packet_out_t *po)
{
    return rqc_get_po_remained_size(po) + RQC_ACK_SPACE;
}
