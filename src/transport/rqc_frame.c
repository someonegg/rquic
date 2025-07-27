/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include <rquic/rquic_typedef.h>
#include "src/common/rqc_log.h"
#include "src/transport/rqc_frame.h"
#include "src/common/utils/vint/rqc_variable_len_int.h"
#include "src/transport/rqc_engine.h"
#include "src/common/rqc_log.h"
#include "src/transport/rqc_packet_in.h"
#include "src/transport/rqc_conn.h"
#include "src/transport/rqc_frame_parser.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/transport/rqc_stream.h"
#include "src/transport/rqc_multipath.h"
#include "src/transport/rqc_defs.h"
#include "src/transport/rqc_utils.h"

static const char * const frame_type_2_str[RQC_FRAME_NUM] = {
    [RQC_FRAME_PADDING]              = "PADDING",
    [RQC_FRAME_PING]                 = "PING",
    [RQC_FRAME_ACK]                  = "ACK",
    [RQC_FRAME_HANDSHAKE]            = "HANDSHAKE",
    [RQC_FRAME_RESET_STREAM]         = "RESET_STREAM",
    [RQC_FRAME_STOP_SENDING]         = "STOP_SENDING",
    [RQC_FRAME_STREAM]               = "STREAM",
    [RQC_FRAME_MAX_DATA]             = "MAX_DATA",
    [RQC_FRAME_MAX_STREAM_DATA]      = "MAX_STREAM_DATA",
    [RQC_FRAME_MAX_STREAMS]          = "MAX_STREAMS",
    [RQC_FRAME_DATA_BLOCKED]         = "DATA_BLOCKED",
    [RQC_FRAME_STREAM_DATA_BLOCKED]  = "STREAM_DATA_BLOCKED",
    [RQC_FRAME_STREAMS_BLOCKED]      = "STREAMS_BLOCKED",
    [RQC_FRAME_PATH_CHALLENGE]       = "PATH_CHALLENGE",
    [RQC_FRAME_PATH_RESPONSE]        = "PATH_RESPONSE",
    [RQC_FRAME_CONNECTION_CLOSE]     = "CONNECTION_CLOSE",
    [RQC_FRAME_Extension]            = "Extension",
};

const char *
rqc_frame_type_2_str(rqc_engine_t *engine, rqc_frame_type_bit_t type_bit)
{
    engine->frame_type_buf[0] = '\0';
    size_t pos = 0;
    int wsize;
    for (int i = 0; i < RQC_FRAME_NUM; i++) {
        if (type_bit & 1ULL << i) {
            wsize = snprintf(engine->frame_type_buf + pos, sizeof(engine->frame_type_buf) - pos, "%s ",
                             frame_type_2_str[i]);
            if (wsize < 0 || wsize >= sizeof(engine->frame_type_buf) - pos) {
                break;
            }
            pos += wsize;
        }
    }
    return engine->frame_type_buf;
}

unsigned int
rqc_stream_frame_header_size(rqc_stream_id_t stream_id, uint64_t offset, size_t length)
{
    return 1 + rqc_vint_len_by_val(stream_id) +
            offset ? rqc_vint_len_by_val(offset) : 0 +
            rqc_vint_len_by_val(length);
}

rqc_int_t
rqc_insert_stream_frame(rqc_connection_t *conn, rqc_stream_t *stream, rqc_stream_frame_t *new_frame)
{

    /* insert rqc_stream_frame_t into stream->stream_data_in.frames_tailq in order of offset */
    unsigned char inserted = 0;
    rqc_list_head_t *pos;
    rqc_stream_frame_t *frame;

    rqc_list_for_each_reverse(pos, &stream->stream_data_in.frames_tailq) {
        frame = rqc_list_entry(pos, rqc_stream_frame_t, sf_list);

        if (rqc_max(frame->data_offset, new_frame->data_offset) <
            rqc_min(frame->data_offset + frame->data_length, new_frame->data_offset + new_frame->data_length))
        {
            /*
             * overlap
             *      |-----------|   frame
             * |-----------|        new_frame
             *        |------------|new_frame
             *        |----|        new_frame  do not insert
             * |-------------------|new_frame
             */
            rqc_log(conn->log, RQC_LOG_INFO, "|is overlap|offset:%ui|new_offset:%ui|len:%ud|new_len:%ud|",
                    frame->data_offset, new_frame->data_offset, frame->data_length, new_frame->data_length);
        }

        if (new_frame->data_offset >= frame->data_offset && new_frame->data_length > 0
            && new_frame->data_offset + new_frame->data_length <= frame->data_offset + frame->data_length)
        {
            rqc_log(conn->log, RQC_LOG_INFO, "|already recvd|offset:%ui|new_offset:%ui|len:%ud|new_len:%ud|",
                    frame->data_offset, new_frame->data_offset, frame->data_length, new_frame->data_length);
            return -RQC_EDUP_FRAME;
        }

        if (new_frame->data_offset >= frame->data_offset) {
            rqc_list_add(&new_frame->sf_list, pos);
            inserted = 1;
            break;
        }
    }

    if (!inserted) {
        rqc_list_add(&new_frame->sf_list, &stream->stream_data_in.frames_tailq);
    }

    /*
     * can merge
     * |--------------|merged_offset_end
     *          |----------|
     *                |--------|
     */
    /* merge */
    if (stream->stream_data_in.merged_offset_end >= new_frame->data_offset
        && stream->stream_data_in.merged_offset_end < new_frame->data_offset + new_frame->data_length)
    {
        stream->stream_data_in.merged_offset_end = new_frame->data_offset + new_frame->data_length;
        pos = new_frame->sf_list.next;
        rqc_list_for_each_from(pos, &stream->stream_data_in.frames_tailq) {
            frame = rqc_list_entry(pos, rqc_stream_frame_t, sf_list);
            if (stream->stream_data_in.merged_offset_end >= frame->data_offset) {
                stream->stream_data_in.merged_offset_end = rqc_max(frame->data_offset + frame->data_length,
                                                                   stream->stream_data_in.merged_offset_end);
            } else {
                /* There is a hole, break */
                break;
            }
        }
    }

    return RQC_OK;
}

rqc_int_t
rqc_process_frames(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret;
    unsigned char *last_pos = NULL;

    while (packet_in->pos < packet_in->last) {
        last_pos = packet_in->pos;

        unsigned char *pos = packet_in->pos;
        unsigned char *end = packet_in->last;
        ssize_t frame_type_len;
        uint64_t frame_type = 0;
        frame_type_len = rqc_vint_read(pos, end, &frame_type);
        if (frame_type_len < 0) {
            return -RQC_EVINTREAD;
        }

        if (conn->conn_state == RQC_CONN_STATE_CLOSING) {
            /* respond connection close when recv any packet except conn_close and ack */
            if (frame_type != 0x1c && frame_type != 0x1d)
            {
                rqc_conn_immediate_close(conn);
                packet_in->pos = packet_in->last;
                return RQC_OK;
            }

        } else if (conn->conn_state >= RQC_CONN_STATE_DRAINING) {
            /* do not respond any packet */
            packet_in->pos = packet_in->last;
            return RQC_OK;
        }

        // TODOXXXX
        switch (frame_type) {

        case 0x00:
            ret = rqc_process_padding_frame(conn, packet_in);
            break;
        case 0x01:
            ret = rqc_process_ping_frame(conn, packet_in);
            break;
        case 0x02:
        case 0x03:
            ret = rqc_process_ack_frame(conn, packet_in);
            break;
        case 0x04:
            ret = rqc_process_reset_stream_frame(conn, packet_in);
            break;
        case 0x05:
            ret = rqc_process_stop_sending_frame(conn, packet_in);
            break;
        case 0x06:
            ret = rqc_process_handshake_frame(conn, packet_in);
            break;
        case 0x08:
        case 0x09:
        case 0x0a:
        case 0x0b:
        case 0x0c:
        case 0x0d:
        case 0x0e:
        case 0x0f:
            ret = rqc_process_stream_frame(conn, packet_in);
            break;
        case 0x10:
            ret = rqc_process_max_data_frame(conn, packet_in);
            break;
        case 0x11:
            ret = rqc_process_max_stream_data_frame(conn, packet_in);
            break;
        case 0x12:
        case 0x13:
            ret = rqc_process_max_streams_frame(conn, packet_in);
            break;
        case 0x14:
            ret = rqc_process_data_blocked_frame(conn, packet_in);
            break;
        case 0x15:
            ret = rqc_process_stream_data_blocked_frame(conn, packet_in);
            break;
        case 0x16:
        case 0x17:
            ret = rqc_process_streams_blocked_frame(conn, packet_in);
            break;
        case 0x1a:
            ret = rqc_process_path_challenge_frame(conn, packet_in);
            break;
        case 0x1b:
            ret = rqc_process_path_response_frame(conn, packet_in);
            break;
        case 0x1c:
        case 0x1d:
            ret = rqc_process_conn_close_frame(conn, packet_in);
            break;

        default:
            rqc_log(conn->log, RQC_LOG_ERROR, "|unknown frame type|");
            return -RQC_EIGNORE_PKT;
        }

        if (ret != RQC_OK) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|process frame error|%d|", ret);
            return ret;
        }

        if (last_pos == packet_in->pos) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|pos not update|");
            return -RQC_ESYS;
        }
    }

    /*
     * An endpoint MUST treat receipt of a packet containing no frames as a
     * connection error of type PROTOCOL_VIOLATION
     */
    if (packet_in->pi_frame_types == 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|receive packet with no frame, close"
                "with PROTOCOL_VIOLATION|");
        RQC_CONN_ERR(conn, TRA_PROTOCOL_VIOLATION);
    }

    return RQC_OK;
}

rqc_int_t
rqc_process_padding_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret;

    ret = rqc_parse_padding_frame(packet_in, conn);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_parse_padding_frame error|");
        return ret;
    }

    return RQC_OK;
}

rqc_int_t
rqc_process_stream_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t            ret;
    rqc_stream_id_t      stream_id;
    rqc_stream_type_t    stream_type;
    rqc_stream_t        *stream = NULL;
    rqc_stream_frame_t  *stream_frame;

    stream_frame = rqc_calloc(1, sizeof(rqc_stream_frame_t));
    if (stream_frame == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_calloc error|");
        return -RQC_EMALLOC;
    }

    ret = rqc_parse_stream_frame(packet_in, conn, stream_frame, &stream_id);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_parse_stream_frame error|ret:%d|stream_id:%ui|", ret, stream_id);
        goto error;
    }

    stream_type = rqc_get_stream_type(stream_id);

    stream = rqc_find_stream_by_id(stream_id, conn->streams_hash);
    if (!stream) {
        if ((conn->conn_type == RQC_CONN_TYPE_SERVER && (stream_type == RQC_CLI_BID || stream_type == RQC_CLI_UNI))
            || (conn->conn_type == RQC_CONN_TYPE_CLIENT && (stream_type == RQC_SVR_BID || stream_type == RQC_SVR_UNI)))
        {
            stream = rqc_passive_create_stream(conn, stream_id, NULL);
            if (!stream) {
                goto free;
            }

        } else {
            rqc_log(conn->log, RQC_LOG_WARN, "|cannot find stream|stream_id:%ui|", stream_id);
            ret = RQC_OK; /* STREAM frame retransmitted after stream is closed. Ignore it. */
            goto error;
        }
    }

    packet_in->stream_id = stream_id;

    if (!stream->stream_stats.first_rcv_time) {
        if (packet_in->pkt_recv_time) {
            stream->stream_stats.first_rcv_time = packet_in->pkt_recv_time;

        } else {
            stream->stream_stats.first_rcv_time = rqc_monotonic_timestamp();
        }
    }

    conn->stream_stats.recv_bytes += stream_frame->data_length;

    if (stream->stream_state_recv >= RQC_RECV_STREAM_ST_RESET_RECVD) {
        ret = RQC_OK;
        goto free;
    }

    stream->stream_stats.final_packet_time = rqc_monotonic_timestamp();
    if (stream_frame->data_offset + stream_frame->data_length <= stream->stream_data_in.merged_offset_end) {
        if (!(stream_frame->fin && stream_frame->data_length == 0 && stream->stream_data_in.stream_length == 0)) {
            goto free;
        }
    }

    if (stream_frame->fin) {
        if (stream->stream_data_in.stream_determined
            && stream->stream_data_in.stream_length != stream_frame->data_offset + stream_frame->data_length)
        {
            rqc_log(conn->log, RQC_LOG_ERROR, "|final size changed|stream_id:%ui|", stream_id);
            RQC_CONN_ERR(conn, TRA_FINAL_SIZE_ERROR);
            ret = -RQC_EPROTO;
            goto error;
        }

        if (!stream->stream_stats.peer_fin_rcv_time) {
            stream->stream_stats.peer_fin_rcv_time = rqc_monotonic_timestamp();
        }

        stream->stream_data_in.stream_length = stream_frame->data_offset + stream_frame->data_length;
        stream->stream_data_in.stream_determined = RQC_TRUE;

        if (stream->stream_state_recv == RQC_RECV_STREAM_ST_RECV) {
            rqc_stream_recv_state_update(stream, RQC_RECV_STREAM_ST_SIZE_KNOWN);
        }
    }

    if (stream->stream_data_in.stream_determined
        && stream_frame->data_offset + stream_frame->data_length > stream->stream_data_in.stream_length)
    {
        rqc_log(conn->log, RQC_LOG_ERROR, "|exceed final size|stream_id:%ui|", stream_id);
        RQC_CONN_ERR(conn, TRA_FINAL_SIZE_ERROR);
        ret = -RQC_EPROTO;
        goto error;
    }

    /* if stream is discarded, drop all data */
    if (stream->stream_flag & RQC_STREAM_FLAG_DISCARDED) {
        /* if all data is discarded, try to close the stream */
        if (stream_frame->fin) {
            rqc_stream_close_discarded_stream(stream);
        }

        goto free;
    }

    ret = rqc_insert_stream_frame(conn, stream, stream_frame);
    if (ret == -RQC_EDUP_FRAME) {
        ret = RQC_OK;
        goto free;

    } else if (ret) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_insert_stream_frame error|stream_id:%ui|", stream_id);
        goto error;
    }

    /* receiver flow control */
    if (stream->stream_max_recv_offset < stream_frame->data_offset + stream_frame->data_length) {
        conn->conn_flow_ctl.fc_data_recved += stream_frame->data_offset + stream_frame->data_length - stream->stream_max_recv_offset;
        stream->stream_max_recv_offset = stream_frame->data_offset + stream_frame->data_length;
    }

    if (conn->conn_flow_ctl.fc_data_recved > conn->conn_flow_ctl.fc_max_data_can_recv) {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|exceed conn flow control|fc_data_recved:%ui|fc_max_data_can_recv:%ui|",
                conn->conn_flow_ctl.fc_data_recved, conn->conn_flow_ctl.fc_max_data_can_recv);
        RQC_CONN_ERR(conn, TRA_FLOW_CONTROL_ERROR);
        return -RQC_EPROTO;
    }

    if (stream->stream_max_recv_offset > stream->stream_flow_ctl.fc_max_stream_data_can_recv) {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|exceed stream flow control|stream_max_recv_offset:%ui|fc_max_stream_data_can_recv:%ui|",
                stream->stream_max_recv_offset, stream->stream_flow_ctl.fc_max_stream_data_can_recv);
        RQC_CONN_ERR(conn, TRA_FLOW_CONTROL_ERROR);
        return -RQC_EPROTO;
    }

    if (stream->stream_data_in.stream_determined
        && stream->stream_data_in.stream_length == stream->stream_data_in.merged_offset_end)
    {
        if (stream->stream_state_recv == RQC_RECV_STREAM_ST_SIZE_KNOWN) {
            rqc_stream_recv_state_update(stream, RQC_RECV_STREAM_ST_DATA_RECVD);
        }
        stream->stream_stats.stream_recv_time = rqc_monotonic_timestamp();
        rqc_stream_ready_to_read(stream);
    }

    else if (stream->stream_data_in.next_read_offset < stream->stream_data_in.merged_offset_end) {
        rqc_stream_ready_to_read(stream);
    }

    return RQC_OK;

error:
free:
    rqc_free(stream_frame->data);
    rqc_free(stream_frame);
    return ret;
}

rqc_int_t
rqc_process_ack_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret;

    rqc_ack_info_t ack_info;
    ret = rqc_parse_ack_frame(packet_in, conn, &ack_info);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_parse_ack_frame error|");
        return ret;
    }

    for (int i = 0; i < ack_info.n_ranges; i++) {
        rqc_log_event(conn->log, TRA_PACKETS_ACKED, packet_in, ack_info.ranges[i].high,
            ack_info.ranges[i].low, RQC_INITIAL_PATH_ID);
    }

    /* 对端还不支持MP，或还未握手确认时，使用 initial path */
    rqc_path_ctx_t *path = conn->the_path;
    rqc_pn_ctl_t *pn_ctl = rqc_get_pn_ctl(conn, path);
    ret = rqc_send_ctl_on_ack_received(path->path_send_ctl, pn_ctl, conn->conn_send_queue,
                                       &ack_info, packet_in->pkt_recv_time);

    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_send_ctl_on_ack_received error|");
        return ret;
    }

    return RQC_OK;
}

rqc_int_t
rqc_process_ping_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret;

    if (!rqc_conn_is_handshake_recvd(conn))
    {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|rqc_process_ping_frame error: ping frame shoud not be the first frame|");
        return RQC_ERROR;
    }
    ret = rqc_parse_ping_frame(packet_in, conn);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|rqc_parse_ping_frame error|");
        return ret;
    }

    return RQC_OK;
}

rqc_int_t
rqc_process_conn_close_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret;
    uint64_t err_code;

    ret = rqc_parse_conn_close_frame(packet_in, &err_code, conn);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|rqc_parse_conn_close_frame error|");
        return ret;
    }

    if (conn->conn_close_recv_time == 0) {
        conn->conn_close_recv_time = rqc_monotonic_timestamp();
    }

    if (err_code) {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|with err:0x%xi|", err_code);
        RQC_CONN_CLOSE_MSG(conn, "remote error");
        RQC_CONN_ERR(conn, err_code);
    } else {
        RQC_CONN_CLOSE_MSG(conn, "remote close");
    }

    if (conn->conn_state < RQC_CONN_STATE_CLOSING) {
        ret = rqc_conn_immediate_close(conn);
        if (ret != RQC_OK) {
            rqc_log(conn->log, RQC_LOG_ERROR,
                    "|rqc_conn_immediate_close error|");
        }
    }
    conn->conn_state = RQC_CONN_STATE_DRAINING;
    rqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);
    rqc_conn_closing(conn);

    return RQC_OK;
}

rqc_int_t
rqc_process_reset_stream_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret;
    uint64_t err_code;
    rqc_stream_id_t stream_id;
    uint64_t final_size;
    rqc_stream_t *stream;

    ret = rqc_parse_reset_stream_frame(packet_in, &stream_id, &err_code, &final_size, conn);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|rqc_parse_reset_stream_frame error|");
        return ret;
    }
    rqc_stream_type_t stream_type = rqc_get_stream_type(stream_id);

    stream = rqc_find_stream_by_id(stream_id, conn->streams_hash);
    if (!stream) {
        if ((conn->conn_type == RQC_CONN_TYPE_SERVER && (stream_type == RQC_CLI_BID || stream_type == RQC_CLI_UNI))
            || (conn->conn_type == RQC_CONN_TYPE_CLIENT && (stream_type == RQC_SVR_BID || stream_type == RQC_SVR_UNI)))
        {
            stream = rqc_passive_create_stream(conn, stream_id, NULL);
            if (!stream) {
                return RQC_OK;
            }

        } else {
            rqc_log(conn->log, RQC_LOG_WARN, "|cannot find stream|stream_id:%ui|", stream_id);
            /* Packet retransmitted after stream is closed */
            return RQC_OK;
        }
    }
    stream->stream_err = err_code;

    RQC_STREAM_CLOSE_MSG(stream, "remote reset");

    rqc_stream_closing(stream, err_code);

    if (stream->stream_state_send < RQC_SEND_STREAM_ST_RESET_SENT) {
        rqc_send_queue_drop_stream_frame_packets(conn, stream_id);
        rqc_write_reset_stream_to_packet(conn, stream, err_code, stream->stream_send_offset);
    }

    if (stream->stream_state_recv < RQC_RECV_STREAM_ST_RESET_RECVD) {
        rqc_stream_recv_state_update(stream, RQC_RECV_STREAM_ST_RESET_RECVD);
        if (stream->stream_stats.peer_reset_time == 0) {
            stream->stream_stats.peer_reset_time = rqc_monotonic_timestamp();
        }
        conn->conn_flow_ctl.fc_data_recved += (int64_t)final_size - (int64_t)stream->stream_max_recv_offset;
        conn->conn_flow_ctl.fc_data_read += (int64_t)final_size - (int64_t)stream->stream_data_in.next_read_offset;
        rqc_destroy_frame_list(&stream->stream_data_in.frames_tailq);
        rqc_stream_ready_to_read(stream);
    }
    return RQC_OK;
}

rqc_int_t
rqc_process_stop_sending_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret;
    uint64_t err_code;
    rqc_stream_id_t stream_id;
    rqc_stream_t *stream;

    ret = rqc_parse_stop_sending_frame(packet_in, &stream_id, &err_code, conn);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|rqc_parse_stop_sending_frame error|");
        return ret;
    }

    rqc_stream_type_t stream_type = rqc_get_stream_type(stream_id);

    stream = rqc_find_stream_by_id(stream_id, conn->streams_hash);
    if (!stream) {
        if ((conn->conn_type == RQC_CONN_TYPE_SERVER && (stream_type == RQC_CLI_BID || stream_type == RQC_CLI_UNI))
            || (conn->conn_type == RQC_CONN_TYPE_CLIENT && (stream_type == RQC_SVR_BID || stream_type == RQC_SVR_UNI)))
        {
            stream = rqc_passive_create_stream(conn, stream_id, NULL);
            if (!stream) {
                return RQC_OK;
            }

        } else {
            rqc_log(conn->log, RQC_LOG_WARN, "|cannot find stream|stream_id:%ui|", stream_id);
            /* Packet retransmitted after stream is closed */
            return RQC_OK;
        }
    }

    /*
     * An endpoint that receives a STOP_SENDING frame
     * MUST send a RESET_STREAM frame if the stream is in the Ready or Send
     * state.
     */
    if (stream->stream_state_send < RQC_SEND_STREAM_ST_RESET_SENT) {
        rqc_write_reset_stream_to_packet(conn, stream, REQUEST_CANCELLED, stream->stream_send_offset);
    }

    return RQC_OK;
}

rqc_int_t
rqc_process_data_blocked_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret;
    uint64_t data_limit, new_limit;

    ret = rqc_parse_data_blocked_frame(packet_in, &data_limit, conn);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|rqc_parse_data_blocked_frame error|");
        return ret;
    }

    if (conn->conn_flow_ctl.fc_data_read + conn->conn_flow_ctl.fc_recv_windows_size <= data_limit) {
        rqc_log(conn->log, RQC_LOG_INFO, "|cannot increase data_limit now|fc_max_data_can_recv:%ui|data_limit:%ui|fc_data_read:%ui|",
                conn->conn_flow_ctl.fc_max_data_can_recv, data_limit, conn->conn_flow_ctl.fc_data_read);
        return RQC_OK;
    }

    new_limit = conn->conn_flow_ctl.fc_data_read + conn->conn_flow_ctl.fc_recv_windows_size;

    if (new_limit > conn->conn_flow_ctl.fc_max_data_can_recv) {
        conn->conn_flow_ctl.fc_max_data_can_recv = new_limit;
        ret = rqc_write_max_data_to_packet(conn, conn->conn_flow_ctl.fc_max_data_can_recv);
        if (ret != RQC_OK) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_max_data_to_packet error|");
            return ret;
        }
    }

    return RQC_OK;
}

rqc_int_t
rqc_process_stream_data_blocked_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret;
    uint64_t stream_data_limit, new_limit;
    rqc_stream_id_t stream_id;
    rqc_stream_t *stream;

    ret = rqc_parse_stream_data_blocked_frame(packet_in, &stream_id, &stream_data_limit, conn);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_parse_stream_data_blocked_frame error|");
        return ret;
    }

    rqc_stream_type_t stream_type = rqc_get_stream_type(stream_id);

    stream = rqc_find_stream_by_id(stream_id, conn->streams_hash);
    if (!stream) {
        if ((conn->conn_type == RQC_CONN_TYPE_SERVER && (stream_type == RQC_CLI_BID || stream_type == RQC_CLI_UNI))
            || (conn->conn_type == RQC_CONN_TYPE_CLIENT && (stream_type == RQC_SVR_BID || stream_type == RQC_SVR_UNI)))
        {
            stream = rqc_passive_create_stream(conn, stream_id, NULL);
            if (!stream) {
                return RQC_OK;
            }

        } else {
            rqc_log(conn->log, RQC_LOG_WARN, "|cannot find stream|stream_id:%ui|", stream_id);
            /* Packet retransmitted after stream is closed */
            return RQC_OK;
        }
    }

    if (stream->stream_data_in.next_read_offset + stream->stream_flow_ctl.fc_stream_recv_window_size <= stream_data_limit) {
        rqc_log(conn->log, RQC_LOG_INFO, "|cannot increase data_limit now|fc_max_stream_data_can_recv:%ui|stream_data_limit:%ui|next_read_offset:%ui|stream_max_recv_offset:%ui|",
                stream->stream_flow_ctl.fc_max_stream_data_can_recv, stream_data_limit, stream->stream_data_in.next_read_offset, stream->stream_max_recv_offset);
        return RQC_OK;
    }

    new_limit = stream->stream_data_in.next_read_offset + stream->stream_flow_ctl.fc_stream_recv_window_size;

    if (new_limit > stream->stream_flow_ctl.fc_max_stream_data_can_recv) {
        stream->stream_flow_ctl.fc_max_stream_data_can_recv = new_limit;

        ret = rqc_write_max_stream_data_to_packet(conn, stream_id, stream->stream_flow_ctl.fc_max_stream_data_can_recv, RQC_PTYPE_SHORT_HEADER);
        if (ret != RQC_OK) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_max_stream_data_to_packet error|");
            return ret;
        }
    }

    return RQC_OK;
}

rqc_int_t
rqc_process_streams_blocked_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret;
    uint64_t stream_limit;
    int bidirectional;

    ret = rqc_parse_streams_blocked_frame(packet_in, &stream_limit, &bidirectional, conn);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|rqc_parse_streams_blocked_frame error|");
        return ret;
    }

    uint64_t new_max_streams;
    if (bidirectional) {
        /* there is no need to increase MAX_STREAMS */
        if (stream_limit < conn->conn_flow_ctl.fc_max_streams_bidi_can_recv) {
            return RQC_OK;
        }

        new_max_streams = rqc_min(stream_limit + conn->local_settings.max_streams_bidi,
            conn->conn_flow_ctl.fc_max_streams_bidi_can_recv + conn->local_settings.max_streams_bidi);
        conn->conn_flow_ctl.fc_max_streams_bidi_can_recv = new_max_streams;

    } else {
        /* there is no need to increase MAX_STREAMS */
        if (stream_limit < conn->conn_flow_ctl.fc_max_streams_uni_can_recv) {
            return RQC_OK;
        }

        new_max_streams = rqc_min(stream_limit + conn->local_settings.max_streams_uni,
            conn->conn_flow_ctl.fc_max_streams_uni_can_recv + conn->local_settings.max_streams_uni);
        conn->conn_flow_ctl.fc_max_streams_uni_can_recv = new_max_streams;
    }

    if (stream_limit < RQC_MAX_STREAMS && (new_max_streams > RQC_MAX_STREAMS)) {
        new_max_streams = RQC_MAX_STREAMS;
    }

    ret = rqc_write_max_streams_to_packet(conn, new_max_streams, bidirectional);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|rqc_write_max_streams_to_packet error|");
        return ret;
    }

    return RQC_OK;
}

rqc_int_t
rqc_process_max_data_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret;
    uint64_t max_data;

    ret = rqc_parse_max_data_frame(packet_in, &max_data, conn);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|rqc_parse_max_data_frame error|");
        return ret;
    }

    if (max_data > conn->conn_flow_ctl.fc_max_data_can_send) {
        conn->conn_flow_ctl.fc_max_data_can_send = max_data;
        conn->conn_flag &= ~RQC_CONN_FLAG_DATA_BLOCKED;

    } else {
        rqc_log(conn->log, RQC_LOG_INFO, "|max_data too small|max_data:%ui|max_data_old:%ui|",
                max_data, conn->conn_flow_ctl.fc_max_data_can_send);
    }

    return RQC_OK;
}

rqc_int_t
rqc_process_max_stream_data_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret = RQC_ERROR;
    uint64_t max_stream_data;
    rqc_stream_id_t stream_id;
    rqc_stream_t *stream;

    ret = rqc_parse_max_stream_data_frame(packet_in, &stream_id, &max_stream_data, conn);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_parse_max_stream_data_frame error|");
        return ret;
    }

    rqc_stream_type_t stream_type = rqc_get_stream_type(stream_id);

    stream = rqc_find_stream_by_id(stream_id, conn->streams_hash);
    if (!stream) {
        if ((conn->conn_type == RQC_CONN_TYPE_SERVER && (stream_type == RQC_CLI_BID || stream_type == RQC_CLI_UNI))
            || (conn->conn_type == RQC_CONN_TYPE_CLIENT && (stream_type == RQC_SVR_BID || stream_type == RQC_SVR_UNI)))
        {
            stream = rqc_passive_create_stream(conn, stream_id, NULL);
            if (!stream) {
                return RQC_OK;
            }

        } else {
            rqc_log(conn->log, RQC_LOG_WARN, "|cannot find stream|stream_id:%ui|", stream_id);
            /* Packet retransmitted after stream is closed */
            return RQC_OK;
        }
    }

    if (max_stream_data > stream->stream_flow_ctl.fc_max_stream_data_can_send) {
        stream->stream_flow_ctl.fc_max_stream_data_can_send = max_stream_data;
        stream->stream_flag &= ~RQC_STREAM_FLAG_DATA_BLOCKED;

    } else {
        rqc_log(conn->log, RQC_LOG_INFO, "|max_stream_data too small|max_stream_data=%ui|max_stream_data_old=%ui|",
                max_stream_data, stream->stream_flow_ctl.fc_max_stream_data_can_send);
    }
    return RQC_OK;
}

rqc_int_t
rqc_process_max_streams_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret = RQC_ERROR;
    uint64_t max_streams;
    int bidirectional;

    ret = rqc_parse_max_streams_frame(packet_in, &max_streams, &bidirectional, conn);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR,
                "|rqc_parse_max_streams_frame error|");
        return ret;
    }

    if (max_streams > RQC_MAX_STREAMS) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_process_max_streams_frame error|receive max_streams:%ui|", max_streams);
        return -RQC_EPROTO;
    }

    if (bidirectional) {
        if (max_streams > conn->conn_flow_ctl.fc_max_streams_bidi_can_send) {
            conn->conn_flow_ctl.fc_max_streams_bidi_can_send = max_streams;
        }

    } else {
        if (max_streams > conn->conn_flow_ctl.fc_max_streams_uni_can_send) {
            conn->conn_flow_ctl.fc_max_streams_uni_can_send = max_streams;
        }
    }

    return RQC_OK;
}

rqc_int_t
rqc_process_path_challenge_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret = RQC_ERROR;
    unsigned char path_challenge_data[RQC_PATH_CHALLENGE_DATA_LEN];

    ret = rqc_parse_path_challenge_frame(packet_in, path_challenge_data);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_parse_path_challenge_frame error|");
        return ret;
    }

    rqc_path_ctx_t *path = conn->the_path;
    if (path == NULL) {
        return RQC_ERROR;
    }

    ret = rqc_write_path_response_frame_to_packet(conn, path, path_challenge_data);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_path_response_frame_to_packet error|%d|", ret);
        return ret;
    }

    return RQC_OK;
}

rqc_int_t
rqc_process_path_response_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret = RQC_ERROR;
    unsigned char path_response_data[RQC_PATH_CHALLENGE_DATA_LEN];

    ret = rqc_parse_path_response_frame(packet_in, path_response_data);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_parse_path_response_frame error|");
        return ret;
    }

    rqc_path_ctx_t *path = conn->the_path;

    /*
     * If the content of a PATH_RESPONSE frame does not match the content of
     * a PATH_CHALLENGE frame previously sent by the endpoint, the endpoint
     * MAY generate a connection error of type PROTOCOL_VIOLATION.
     */

    if (memcmp(path->path_challenge_data, path_response_data, RQC_PATH_CHALLENGE_DATA_LEN) != 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|path:%ui|ignore|no match path challenge data|", path->path_id);
        return RQC_OK;
    }

    if (conn->conn_type == RQC_CONN_TYPE_SERVER
        && (path->rebinding_addrlen != 0)
        && (path->rebinding_check_response == 1))
    {
        /* successfully validate rebinding addr */
        rqc_memcpy(path->peer_addr, path->rebinding_addr, path->rebinding_addrlen);
        path->peer_addrlen = path->rebinding_addrlen;
        path->addr_str_len = 0;
        rqc_log(conn->log, RQC_LOG_INFO, "|path:%ui|REBINDING|validate NAT rebinding addr|path:%s|", path->path_id, rqc_path_addr_str(path));

        rqc_memcpy(conn->peer_addr, path->rebinding_addr, path->rebinding_addrlen);
        conn->peer_addrlen = path->rebinding_addrlen;
        conn->addr_str_len = 0;
        rqc_log(conn->log, RQC_LOG_INFO, "|path:%ui|REBINDING|validate NAT rebinding addr|conn:%s|", path->path_id, rqc_conn_addr_str(conn));

        if (conn->transport_cbs.conn_peer_addr_changed_notify) {
            conn->transport_cbs.conn_peer_addr_changed_notify(conn, rqc_conn_get_user_data(conn));
        }

        path->rebinding_valid++;
        path->rebinding_addrlen = 0;
        path->rebinding_check_response = 0;
        rqc_timer_unset(&path->path_send_ctl->path_timer_manager, RQC_TIMER_NAT_REBINDING);
    }

    return RQC_OK;
}

rqc_int_t
rqc_process_handshake_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in)
{
    rqc_int_t ret;
    unsigned char alpn[RQC_MAX_ALPN_BUF_LEN];
    size_t alpn_len;
    unsigned char tp[RQC_MAX_TRANSPORT_PARAM_BUF_LEN];
    size_t tp_len;

    ret = rqc_parse_handshake_frame(packet_in, conn, alpn, sizeof(alpn)-1, &alpn_len, tp, sizeof(tp), &tp_len);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_parse_handshake_frame error|%d|", ret);
        return ret;
    }

    ret = rqc_conn_process_handshake(conn, alpn, alpn_len, tp, tp_len);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_conn_process_handshake error|%d|state:%s|flag:%s|", ret,
            rqc_conn_state_2_str(conn->conn_state), rqc_conn_flag_2_str(conn, conn->conn_flag));
        return ret;
    }

    return RQC_OK;
}
