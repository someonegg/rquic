/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include <xquic/xquic_typedef.h>
#include "src/common/xqc_log.h"
#include "src/transport/xqc_frame.h"
#include "src/common/utils/vint/xqc_variable_len_int.h"
#include "src/transport/xqc_engine.h"
#include "src/common/xqc_log.h"
#include "src/transport/xqc_packet_in.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_frame_parser.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/transport/xqc_stream.h"
#include "src/transport/xqc_multipath.h"
#include "src/transport/xqc_defs.h"
#include "src/transport/xqc_utils.h"

static const char * const frame_type_2_str[XQC_FRAME_NUM] = {
    [XQC_FRAME_PADDING]              = "PADDING",
    [XQC_FRAME_PING]                 = "PING",
    [XQC_FRAME_ACK]                  = "ACK",
    [XQC_FRAME_RESET_STREAM]         = "RESET_STREAM",
    [XQC_FRAME_STOP_SENDING]         = "STOP_SENDING",
    [XQC_FRAME_STREAM]               = "STREAM",
    [XQC_FRAME_MAX_DATA]             = "MAX_DATA",
    [XQC_FRAME_MAX_STREAM_DATA]      = "MAX_STREAM_DATA",
    [XQC_FRAME_MAX_STREAMS]          = "MAX_STREAMS",
    [XQC_FRAME_DATA_BLOCKED]         = "DATA_BLOCKED",
    [XQC_FRAME_STREAM_DATA_BLOCKED]  = "STREAM_DATA_BLOCKED",
    [XQC_FRAME_STREAMS_BLOCKED]      = "STREAMS_BLOCKED",
    [XQC_FRAME_PATH_CHALLENGE]       = "PATH_CHALLENGE",
    [XQC_FRAME_PATH_RESPONSE]        = "PATH_RESPONSE",
    [XQC_FRAME_CONNECTION_CLOSE]     = "CONNECTION_CLOSE",
    [XQC_FRAME_Extension]            = "Extension",
};

const char *
xqc_frame_type_2_str(xqc_engine_t *engine, xqc_frame_type_bit_t type_bit)
{
    engine->frame_type_buf[0] = '\0';
    size_t pos = 0;
    int wsize;
    for (int i = 0; i < XQC_FRAME_NUM; i++) {
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
xqc_stream_frame_header_size(xqc_stream_id_t stream_id, uint64_t offset, size_t length)
{
    return 1 + xqc_vint_len_by_val(stream_id) +
            offset ? xqc_vint_len_by_val(offset) : 0 +
            xqc_vint_len_by_val(length);
}

xqc_int_t
xqc_insert_stream_frame(xqc_connection_t *conn, xqc_stream_t *stream, xqc_stream_frame_t *new_frame)
{

    /* insert xqc_stream_frame_t into stream->stream_data_in.frames_tailq in order of offset */
    unsigned char inserted = 0;
    xqc_list_head_t *pos;
    xqc_stream_frame_t *frame;

    xqc_list_for_each_reverse(pos, &stream->stream_data_in.frames_tailq) {
        frame = xqc_list_entry(pos, xqc_stream_frame_t, sf_list);

        if (xqc_max(frame->data_offset, new_frame->data_offset) <
            xqc_min(frame->data_offset + frame->data_length, new_frame->data_offset + new_frame->data_length))
        {
            /*
             * overlap
             *      |-----------|   frame
             * |-----------|        new_frame
             *        |------------|new_frame
             *        |----|        new_frame  do not insert
             * |-------------------|new_frame
             */
            xqc_log(conn->log, XQC_LOG_INFO, "|is overlap|offset:%ui|new_offset:%ui|len:%ud|new_len:%ud|",
                    frame->data_offset, new_frame->data_offset, frame->data_length, new_frame->data_length);
        }

        if (new_frame->data_offset >= frame->data_offset && new_frame->data_length > 0
            && new_frame->data_offset + new_frame->data_length <= frame->data_offset + frame->data_length)
        {
            xqc_log(conn->log, XQC_LOG_INFO, "|already recvd|offset:%ui|new_offset:%ui|len:%ud|new_len:%ud|",
                    frame->data_offset, new_frame->data_offset, frame->data_length, new_frame->data_length);
            return -XQC_EDUP_FRAME;
        }

        if (new_frame->data_offset >= frame->data_offset) {
            xqc_list_add(&new_frame->sf_list, pos);
            inserted = 1;
            break;
        }
    }

    if (!inserted) {
        xqc_list_add(&new_frame->sf_list, &stream->stream_data_in.frames_tailq);
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
        xqc_list_for_each_from(pos, &stream->stream_data_in.frames_tailq) {
            frame = xqc_list_entry(pos, xqc_stream_frame_t, sf_list);
            if (stream->stream_data_in.merged_offset_end >= frame->data_offset) {
                stream->stream_data_in.merged_offset_end = xqc_max(frame->data_offset + frame->data_length,
                                                                   stream->stream_data_in.merged_offset_end);
            } else {
                /* There is a hole, break */
                break;
            }
        }
    }

    return XQC_OK;
}

xqc_int_t
xqc_process_frames(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret;
    unsigned char *last_pos = NULL;

    while (packet_in->pos < packet_in->last) {
        last_pos = packet_in->pos;

        unsigned char *pos = packet_in->pos;
        unsigned char *end = packet_in->last;
        ssize_t frame_type_len;
        uint64_t frame_type = 0;
        frame_type_len = xqc_vint_read(pos, end, &frame_type);
        if (frame_type_len < 0) {
            return -XQC_EVINTREAD;
        }

        if (conn->conn_state == XQC_CONN_STATE_CLOSING) {
            /* respond connection close when recv any packet except conn_close and ack */
            if (frame_type != 0x1c && frame_type != 0x1d)
            {
                xqc_conn_immediate_close(conn);
                packet_in->pos = packet_in->last;
                return XQC_OK;
            }

        } else if (conn->conn_state >= XQC_CONN_STATE_DRAINING) {
            /* do not respond any packet */
            packet_in->pos = packet_in->last;
            return XQC_OK;
        }

        // TODOXXXX
        switch (frame_type) {

        case 0x00:
            ret = xqc_process_padding_frame(conn, packet_in);
            break;
        case 0x01:
            ret = xqc_process_ping_frame(conn, packet_in);
            break;
        case 0x02:
        case 0x03:
            ret = xqc_process_ack_frame(conn, packet_in);
            break;
        case 0x04:
            ret = xqc_process_reset_stream_frame(conn, packet_in);
            break;
        case 0x05:
            ret = xqc_process_stop_sending_frame(conn, packet_in);
            break;
        case 0x08:
        case 0x09:
        case 0x0a:
        case 0x0b:
        case 0x0c:
        case 0x0d:
        case 0x0e:
        case 0x0f:
            ret = xqc_process_stream_frame(conn, packet_in);
            break;
        case 0x10:
            ret = xqc_process_max_data_frame(conn, packet_in);
            break;
        case 0x11:
            ret = xqc_process_max_stream_data_frame(conn, packet_in);
            break;
        case 0x12:
        case 0x13:
            ret = xqc_process_max_streams_frame(conn, packet_in);
            break;
        case 0x14:
            ret = xqc_process_data_blocked_frame(conn, packet_in);
            break;
        case 0x15:
            ret = xqc_process_stream_data_blocked_frame(conn, packet_in);
            break;
        case 0x16:
        case 0x17:
            ret = xqc_process_streams_blocked_frame(conn, packet_in);
            break;
        case 0x1a:
            ret = xqc_process_path_challenge_frame(conn, packet_in);
            break;
        case 0x1b:
            ret = xqc_process_path_response_frame(conn, packet_in);
            break;
        case 0x1c:
        case 0x1d:
            ret = xqc_process_conn_close_frame(conn, packet_in);
            break;

        default:
            xqc_log(conn->log, XQC_LOG_ERROR, "|unknown frame type|");
            return -XQC_EIGNORE_PKT;
        }

        if (ret != XQC_OK) {
            xqc_log(conn->log, XQC_LOG_ERROR, "|process frame error|%d|", ret);
            return ret;
        }

        if (last_pos == packet_in->pos) {
            xqc_log(conn->log, XQC_LOG_ERROR, "|pos not update|");
            return -XQC_ESYS;
        }
    }

    /*
     * An endpoint MUST treat receipt of a packet containing no frames as a
     * connection error of type PROTOCOL_VIOLATION
     */
    if (packet_in->pi_frame_types == 0) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|receive packet with no frame, close"
                "with PROTOCOL_VIOLATION|");
        XQC_CONN_ERR(conn, TRA_PROTOCOL_VIOLATION);
    }

    return XQC_OK;
}

xqc_int_t
xqc_process_padding_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret;

    ret = xqc_parse_padding_frame(packet_in, conn);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_parse_padding_frame error|");
        return ret;
    }

    return XQC_OK;
}

xqc_int_t
xqc_process_stream_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t            ret;
    xqc_stream_id_t      stream_id;
    xqc_stream_type_t    stream_type;
    xqc_stream_t        *stream = NULL;
    xqc_stream_frame_t  *stream_frame;

    stream_frame = xqc_calloc(1, sizeof(xqc_stream_frame_t));
    if (stream_frame == NULL) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_calloc error|");
        return -XQC_EMALLOC;
    }

    ret = xqc_parse_stream_frame(packet_in, conn, stream_frame, &stream_id);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_parse_stream_frame error|ret:%d|stream_id:%ui|", ret, stream_id);
        goto error;
    }

    stream_type = xqc_get_stream_type(stream_id);

    stream = xqc_find_stream_by_id(stream_id, conn->streams_hash);
    if (!stream) {
        if ((conn->conn_type == XQC_CONN_TYPE_SERVER && (stream_type == XQC_CLI_BID || stream_type == XQC_CLI_UNI))
            || (conn->conn_type == XQC_CONN_TYPE_CLIENT && (stream_type == XQC_SVR_BID || stream_type == XQC_SVR_UNI)))
        {
            stream = xqc_passive_create_stream(conn, stream_id, NULL);
            if (!stream) {
                goto free;
            }

        } else {
            xqc_log(conn->log, XQC_LOG_WARN, "|cannot find stream|stream_id:%ui|", stream_id);
            ret = XQC_OK; /* STREAM frame retransmitted after stream is closed. Ignore it. */
            goto error;
        }
    }

    packet_in->stream_id = stream_id;

    if (!stream->stream_stats.first_rcv_time) {
        if (packet_in->pkt_recv_time) {
            stream->stream_stats.first_rcv_time = packet_in->pkt_recv_time;

        } else {
            stream->stream_stats.first_rcv_time = xqc_monotonic_timestamp();
        }
    }

    conn->stream_stats.recv_bytes += stream_frame->data_length;

    if (stream->stream_state_recv >= XQC_RECV_STREAM_ST_RESET_RECVD) {
        ret = XQC_OK;
        goto free;
    }

    stream->stream_stats.final_packet_time = xqc_monotonic_timestamp();
    if (stream_frame->data_offset + stream_frame->data_length <= stream->stream_data_in.merged_offset_end) {
        if (!(stream_frame->fin && stream_frame->data_length == 0 && stream->stream_data_in.stream_length == 0)) {
            goto free;
        }
    }

    if (stream_frame->fin) {
        if (stream->stream_data_in.stream_determined
            && stream->stream_data_in.stream_length != stream_frame->data_offset + stream_frame->data_length)
        {
            xqc_log(conn->log, XQC_LOG_ERROR, "|final size changed|stream_id:%ui|", stream_id);
            XQC_CONN_ERR(conn, TRA_FINAL_SIZE_ERROR);
            ret = -XQC_EPROTO;
            goto error;
        }

        if (!stream->stream_stats.peer_fin_rcv_time) {
            stream->stream_stats.peer_fin_rcv_time = xqc_monotonic_timestamp();
        }

        stream->stream_data_in.stream_length = stream_frame->data_offset + stream_frame->data_length;
        stream->stream_data_in.stream_determined = XQC_TRUE;

        if (stream->stream_state_recv == XQC_RECV_STREAM_ST_RECV) {
            xqc_stream_recv_state_update(stream, XQC_RECV_STREAM_ST_SIZE_KNOWN);
        }
    }

    if (stream->stream_data_in.stream_determined
        && stream_frame->data_offset + stream_frame->data_length > stream->stream_data_in.stream_length)
    {
        xqc_log(conn->log, XQC_LOG_ERROR, "|exceed final size|stream_id:%ui|", stream_id);
        XQC_CONN_ERR(conn, TRA_FINAL_SIZE_ERROR);
        ret = -XQC_EPROTO;
        goto error;
    }

    /* if stream is discarded, drop all data */
    if (stream->stream_flag & XQC_STREAM_FLAG_DISCARDED) {
        /* if all data is discarded, try to close the stream */
        if (stream_frame->fin) {
            xqc_stream_close_discarded_stream(stream);
        }

        goto free;
    }

    ret = xqc_insert_stream_frame(conn, stream, stream_frame);
    if (ret == -XQC_EDUP_FRAME) {
        ret = XQC_OK;
        goto free;

    } else if (ret) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_insert_stream_frame error|stream_id:%ui|", stream_id);
        goto error;
    }

    /* receiver flow control */
    if (stream->stream_max_recv_offset < stream_frame->data_offset + stream_frame->data_length) {
        conn->conn_flow_ctl.fc_data_recved += stream_frame->data_offset + stream_frame->data_length - stream->stream_max_recv_offset;
        stream->stream_max_recv_offset = stream_frame->data_offset + stream_frame->data_length;
    }

    if (conn->conn_flow_ctl.fc_data_recved > conn->conn_flow_ctl.fc_max_data_can_recv) {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|exceed conn flow control|fc_data_recved:%ui|fc_max_data_can_recv:%ui|",
                conn->conn_flow_ctl.fc_data_recved, conn->conn_flow_ctl.fc_max_data_can_recv);
        XQC_CONN_ERR(conn, TRA_FLOW_CONTROL_ERROR);
        return -XQC_EPROTO;
    }

    if (stream->stream_max_recv_offset > stream->stream_flow_ctl.fc_max_stream_data_can_recv) {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|exceed stream flow control|stream_max_recv_offset:%ui|fc_max_stream_data_can_recv:%ui|",
                stream->stream_max_recv_offset, stream->stream_flow_ctl.fc_max_stream_data_can_recv);
        XQC_CONN_ERR(conn, TRA_FLOW_CONTROL_ERROR);
        return -XQC_EPROTO;
    }

    if (stream->stream_data_in.stream_determined
        && stream->stream_data_in.stream_length == stream->stream_data_in.merged_offset_end)
    {
        if (stream->stream_state_recv == XQC_RECV_STREAM_ST_SIZE_KNOWN) {
            xqc_stream_recv_state_update(stream, XQC_RECV_STREAM_ST_DATA_RECVD);
        }
        stream->stream_stats.stream_recv_time = xqc_monotonic_timestamp();
        xqc_stream_ready_to_read(stream);
    }

    else if (stream->stream_data_in.next_read_offset < stream->stream_data_in.merged_offset_end) {
        xqc_stream_ready_to_read(stream);
    }

    return XQC_OK;

error:
free:
    xqc_free(stream_frame->data);
    xqc_free(stream_frame);
    return ret;
}

xqc_int_t
xqc_process_ack_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret;

    xqc_ack_info_t ack_info;
    ret = xqc_parse_ack_frame(packet_in, conn, &ack_info);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_parse_ack_frame error|");
        return ret;
    }

    for (int i = 0; i < ack_info.n_ranges; i++) {
        xqc_log_event(conn->log, TRA_PACKETS_ACKED, packet_in, ack_info.ranges[i].high,
            ack_info.ranges[i].low, XQC_INITIAL_PATH_ID);
    }

    /* 对端还不支持MP，或还未握手确认时，使用 initial path */
    xqc_path_ctx_t *path = conn->the_path;
    xqc_pn_ctl_t *pn_ctl = xqc_get_pn_ctl(conn, path);
    ret = xqc_send_ctl_on_ack_received(path->path_send_ctl, pn_ctl, conn->conn_send_queue,
                                       &ack_info, packet_in->pkt_recv_time);

    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_send_ctl_on_ack_received error|");
        return ret;
    }

    return XQC_OK;
}

xqc_int_t
xqc_process_ping_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret;

    /* ping frame should not be the first frame in the first initial packet */
    if (conn->conn_state == XQC_CONN_STATE_SERVER_INIT
        && !(conn->conn_flag & XQC_CONN_FLAG_HANDSHAKE_RECVD))
    {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|xqc_process_ping_frame error: ping frame shoud not be the first frame|");
        return XQC_ERROR;
    }
    ret = xqc_parse_ping_frame(packet_in, conn);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|xqc_parse_ping_frame error|");
        return ret;
    }

    return XQC_OK;
}

xqc_int_t
xqc_process_conn_close_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret;
    uint64_t err_code;

    ret = xqc_parse_conn_close_frame(packet_in, &err_code, conn);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|xqc_parse_conn_close_frame error|");
        return ret;
    }

    if (conn->conn_close_recv_time == 0) {
        conn->conn_close_recv_time = xqc_monotonic_timestamp();
    }

    if (err_code) {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|with err:0x%xi|", err_code);
        XQC_CONN_CLOSE_MSG(conn, "remote error");
        XQC_CONN_ERR(conn, err_code);
    } else {
        XQC_CONN_CLOSE_MSG(conn, "remote close");
    }

    if (conn->conn_state < XQC_CONN_STATE_CLOSING) {
        ret = xqc_conn_immediate_close(conn);
        if (ret != XQC_OK) {
            xqc_log(conn->log, XQC_LOG_ERROR,
                    "|xqc_conn_immediate_close error|");
        }
    }
    conn->conn_state = XQC_CONN_STATE_DRAINING;
    xqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);
    xqc_conn_closing(conn);

    return XQC_OK;
}

xqc_int_t
xqc_process_reset_stream_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret;
    uint64_t err_code;
    xqc_stream_id_t stream_id;
    uint64_t final_size;
    xqc_stream_t *stream;

    ret = xqc_parse_reset_stream_frame(packet_in, &stream_id, &err_code, &final_size, conn);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|xqc_parse_reset_stream_frame error|");
        return ret;
    }
    xqc_stream_type_t stream_type = xqc_get_stream_type(stream_id);

    stream = xqc_find_stream_by_id(stream_id, conn->streams_hash);
    if (!stream) {
        if ((conn->conn_type == XQC_CONN_TYPE_SERVER && (stream_type == XQC_CLI_BID || stream_type == XQC_CLI_UNI))
            || (conn->conn_type == XQC_CONN_TYPE_CLIENT && (stream_type == XQC_SVR_BID || stream_type == XQC_SVR_UNI)))
        {
            stream = xqc_passive_create_stream(conn, stream_id, NULL);
            if (!stream) {
                return XQC_OK;
            }

        } else {
            xqc_log(conn->log, XQC_LOG_WARN, "|cannot find stream|stream_id:%ui|", stream_id);
            /* Packet retransmitted after stream is closed */
            return XQC_OK;
        }
    }
    stream->stream_err = err_code;

    XQC_STREAM_CLOSE_MSG(stream, "remote reset");

    xqc_stream_closing(stream, err_code);

    if (stream->stream_state_send < XQC_SEND_STREAM_ST_RESET_SENT) {
        xqc_send_queue_drop_stream_frame_packets(conn, stream_id);
        xqc_write_reset_stream_to_packet(conn, stream, err_code, stream->stream_send_offset);
    }

    if (stream->stream_state_recv < XQC_RECV_STREAM_ST_RESET_RECVD) {
        xqc_stream_recv_state_update(stream, XQC_RECV_STREAM_ST_RESET_RECVD);
        if (stream->stream_stats.peer_reset_time == 0) {
            stream->stream_stats.peer_reset_time = xqc_monotonic_timestamp();
        }
        conn->conn_flow_ctl.fc_data_recved += (int64_t)final_size - (int64_t)stream->stream_max_recv_offset;
        conn->conn_flow_ctl.fc_data_read += (int64_t)final_size - (int64_t)stream->stream_data_in.next_read_offset;
        xqc_destroy_frame_list(&stream->stream_data_in.frames_tailq);
        xqc_stream_ready_to_read(stream);
    }
    return XQC_OK;
}

xqc_int_t
xqc_process_stop_sending_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret;
    uint64_t err_code;
    xqc_stream_id_t stream_id;
    xqc_stream_t *stream;

    ret = xqc_parse_stop_sending_frame(packet_in, &stream_id, &err_code, conn);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|xqc_parse_stop_sending_frame error|");
        return ret;
    }

    xqc_stream_type_t stream_type = xqc_get_stream_type(stream_id);

    stream = xqc_find_stream_by_id(stream_id, conn->streams_hash);
    if (!stream) {
        if ((conn->conn_type == XQC_CONN_TYPE_SERVER && (stream_type == XQC_CLI_BID || stream_type == XQC_CLI_UNI))
            || (conn->conn_type == XQC_CONN_TYPE_CLIENT && (stream_type == XQC_SVR_BID || stream_type == XQC_SVR_UNI)))
        {
            stream = xqc_passive_create_stream(conn, stream_id, NULL);
            if (!stream) {
                return XQC_OK;
            }

        } else {
            xqc_log(conn->log, XQC_LOG_WARN, "|cannot find stream|stream_id:%ui|", stream_id);
            /* Packet retransmitted after stream is closed */
            return XQC_OK;
        }
    }

    /*
     * An endpoint that receives a STOP_SENDING frame
     * MUST send a RESET_STREAM frame if the stream is in the Ready or Send
     * state.
     */
    if (stream->stream_state_send < XQC_SEND_STREAM_ST_RESET_SENT) {
        xqc_write_reset_stream_to_packet(conn, stream, REQUEST_CANCELLED, stream->stream_send_offset);
    }

    return XQC_OK;
}

xqc_int_t
xqc_process_data_blocked_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret;
    uint64_t data_limit, new_limit;

    ret = xqc_parse_data_blocked_frame(packet_in, &data_limit, conn);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|xqc_parse_data_blocked_frame error|");
        return ret;
    }

    if (conn->conn_flow_ctl.fc_data_read + conn->conn_flow_ctl.fc_recv_windows_size <= data_limit) {
        xqc_log(conn->log, XQC_LOG_INFO, "|cannot increase data_limit now|fc_max_data_can_recv:%ui|data_limit:%ui|fc_data_read:%ui|",
                conn->conn_flow_ctl.fc_max_data_can_recv, data_limit, conn->conn_flow_ctl.fc_data_read);
        return XQC_OK;
    }

    new_limit = conn->conn_flow_ctl.fc_data_read + conn->conn_flow_ctl.fc_recv_windows_size;

    if (new_limit > conn->conn_flow_ctl.fc_max_data_can_recv) {
        conn->conn_flow_ctl.fc_max_data_can_recv = new_limit;
        ret = xqc_write_max_data_to_packet(conn, conn->conn_flow_ctl.fc_max_data_can_recv);
        if (ret != XQC_OK) {
            xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_write_max_data_to_packet error|");
            return ret;
        }
    }

    return XQC_OK;
}

xqc_int_t
xqc_process_stream_data_blocked_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret;
    uint64_t stream_data_limit, new_limit;
    xqc_stream_id_t stream_id;
    xqc_stream_t *stream;

    ret = xqc_parse_stream_data_blocked_frame(packet_in, &stream_id, &stream_data_limit, conn);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_parse_stream_data_blocked_frame error|");
        return ret;
    }

    xqc_stream_type_t stream_type = xqc_get_stream_type(stream_id);

    stream = xqc_find_stream_by_id(stream_id, conn->streams_hash);
    if (!stream) {
        if ((conn->conn_type == XQC_CONN_TYPE_SERVER && (stream_type == XQC_CLI_BID || stream_type == XQC_CLI_UNI))
            || (conn->conn_type == XQC_CONN_TYPE_CLIENT && (stream_type == XQC_SVR_BID || stream_type == XQC_SVR_UNI)))
        {
            stream = xqc_passive_create_stream(conn, stream_id, NULL);
            if (!stream) {
                return XQC_OK;
            }

        } else {
            xqc_log(conn->log, XQC_LOG_WARN, "|cannot find stream|stream_id:%ui|", stream_id);
            /* Packet retransmitted after stream is closed */
            return XQC_OK;
        }
    }

    if (stream->stream_data_in.next_read_offset + stream->stream_flow_ctl.fc_stream_recv_window_size <= stream_data_limit) {
        xqc_log(conn->log, XQC_LOG_INFO, "|cannot increase data_limit now|fc_max_stream_data_can_recv:%ui|stream_data_limit:%ui|next_read_offset:%ui|stream_max_recv_offset:%ui|",
                stream->stream_flow_ctl.fc_max_stream_data_can_recv, stream_data_limit, stream->stream_data_in.next_read_offset, stream->stream_max_recv_offset);
        return XQC_OK;
    }

    new_limit = stream->stream_data_in.next_read_offset + stream->stream_flow_ctl.fc_stream_recv_window_size;

    if (new_limit > stream->stream_flow_ctl.fc_max_stream_data_can_recv) {
        stream->stream_flow_ctl.fc_max_stream_data_can_recv = new_limit;

        ret = xqc_write_max_stream_data_to_packet(conn, stream_id, stream->stream_flow_ctl.fc_max_stream_data_can_recv, XQC_PTYPE_SHORT_HEADER);
        if (ret != XQC_OK) {
            xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_write_max_stream_data_to_packet error|");
            return ret;
        }
    }

    return XQC_OK;
}

xqc_int_t
xqc_process_streams_blocked_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret;
    uint64_t stream_limit;
    int bidirectional;

    ret = xqc_parse_streams_blocked_frame(packet_in, &stream_limit, &bidirectional, conn);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|xqc_parse_streams_blocked_frame error|");
        return ret;
    }

    uint64_t new_max_streams;
    if (bidirectional) {
        /* there is no need to increase MAX_STREAMS */
        if (stream_limit < conn->conn_flow_ctl.fc_max_streams_bidi_can_recv) {
            return XQC_OK;
        }

        new_max_streams = xqc_min(stream_limit + conn->local_settings.max_streams_bidi,
            conn->conn_flow_ctl.fc_max_streams_bidi_can_recv + conn->local_settings.max_streams_bidi);
        conn->conn_flow_ctl.fc_max_streams_bidi_can_recv = new_max_streams;

    } else {
        /* there is no need to increase MAX_STREAMS */
        if (stream_limit < conn->conn_flow_ctl.fc_max_streams_uni_can_recv) {
            return XQC_OK;
        }

        new_max_streams = xqc_min(stream_limit + conn->local_settings.max_streams_uni,
            conn->conn_flow_ctl.fc_max_streams_uni_can_recv + conn->local_settings.max_streams_uni);
        conn->conn_flow_ctl.fc_max_streams_uni_can_recv = new_max_streams;
    }

    if (stream_limit < XQC_MAX_STREAMS && (new_max_streams > XQC_MAX_STREAMS)) {
        new_max_streams = XQC_MAX_STREAMS;
    }

    ret = xqc_write_max_streams_to_packet(conn, new_max_streams, bidirectional);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|xqc_write_max_streams_to_packet error|");
        return ret;
    }

    return XQC_OK;
}

xqc_int_t
xqc_process_max_data_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret;
    uint64_t max_data;

    ret = xqc_parse_max_data_frame(packet_in, &max_data, conn);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|xqc_parse_max_data_frame error|");
        return ret;
    }

    if (max_data > conn->conn_flow_ctl.fc_max_data_can_send) {
        conn->conn_flow_ctl.fc_max_data_can_send = max_data;
        conn->conn_flag &= ~XQC_CONN_FLAG_DATA_BLOCKED;

    } else {
        xqc_log(conn->log, XQC_LOG_INFO, "|max_data too small|max_data:%ui|max_data_old:%ui|",
                max_data, conn->conn_flow_ctl.fc_max_data_can_send);
    }

    return XQC_OK;
}

xqc_int_t
xqc_process_max_stream_data_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret = XQC_ERROR;
    uint64_t max_stream_data;
    xqc_stream_id_t stream_id;
    xqc_stream_t *stream;

    ret = xqc_parse_max_stream_data_frame(packet_in, &stream_id, &max_stream_data, conn);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_parse_max_stream_data_frame error|");
        return ret;
    }

    xqc_stream_type_t stream_type = xqc_get_stream_type(stream_id);

    stream = xqc_find_stream_by_id(stream_id, conn->streams_hash);
    if (!stream) {
        if ((conn->conn_type == XQC_CONN_TYPE_SERVER && (stream_type == XQC_CLI_BID || stream_type == XQC_CLI_UNI))
            || (conn->conn_type == XQC_CONN_TYPE_CLIENT && (stream_type == XQC_SVR_BID || stream_type == XQC_SVR_UNI)))
        {
            stream = xqc_passive_create_stream(conn, stream_id, NULL);
            if (!stream) {
                return XQC_OK;
            }

        } else {
            xqc_log(conn->log, XQC_LOG_WARN, "|cannot find stream|stream_id:%ui|", stream_id);
            /* Packet retransmitted after stream is closed */
            return XQC_OK;
        }
    }

    if (max_stream_data > stream->stream_flow_ctl.fc_max_stream_data_can_send) {
        stream->stream_flow_ctl.fc_max_stream_data_can_send = max_stream_data;
        stream->stream_flag &= ~XQC_STREAM_FLAG_DATA_BLOCKED;

    } else {
        xqc_log(conn->log, XQC_LOG_INFO, "|max_stream_data too small|max_stream_data=%ui|max_stream_data_old=%ui|",
                max_stream_data, stream->stream_flow_ctl.fc_max_stream_data_can_send);
    }
    return XQC_OK;
}

xqc_int_t
xqc_process_max_streams_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret = XQC_ERROR;
    uint64_t max_streams;
    int bidirectional;

    ret = xqc_parse_max_streams_frame(packet_in, &max_streams, &bidirectional, conn);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR,
                "|xqc_parse_max_streams_frame error|");
        return ret;
    }

    if (max_streams > XQC_MAX_STREAMS) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_process_max_streams_frame error|receive max_streams:%ui|", max_streams);
        return -XQC_EPROTO;
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

    return XQC_OK;
}

xqc_int_t
xqc_process_path_challenge_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret = XQC_ERROR;
    unsigned char path_challenge_data[XQC_PATH_CHALLENGE_DATA_LEN];

    ret = xqc_parse_path_challenge_frame(packet_in, path_challenge_data);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_parse_path_challenge_frame error|");
        return ret;
    }

    xqc_path_ctx_t *path = conn->the_path;
    if (path == NULL) {
        return XQC_ERROR;
    }

    ret = xqc_write_path_response_frame_to_packet(conn, path, path_challenge_data);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_write_path_response_frame_to_packet error|%d|", ret);
        return ret;
    }

    return XQC_OK;
}

xqc_int_t
xqc_process_path_response_frame(xqc_connection_t *conn, xqc_packet_in_t *packet_in)
{
    xqc_int_t ret = XQC_ERROR;
    unsigned char path_response_data[XQC_PATH_CHALLENGE_DATA_LEN];

    ret = xqc_parse_path_response_frame(packet_in, path_response_data);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_parse_path_response_frame error|");
        return ret;
    }

    xqc_path_ctx_t *path = conn->the_path;

    /*
     * If the content of a PATH_RESPONSE frame does not match the content of
     * a PATH_CHALLENGE frame previously sent by the endpoint, the endpoint
     * MAY generate a connection error of type PROTOCOL_VIOLATION.
     */

    if (memcmp(path->path_challenge_data, path_response_data, XQC_PATH_CHALLENGE_DATA_LEN) != 0) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|path:%ui|ignore|no match path challenge data|", path->path_id);
        return XQC_OK;
    }

    if (conn->conn_type == XQC_CONN_TYPE_SERVER
        && (path->rebinding_addrlen != 0)
        && (path->rebinding_check_response == 1))
    {
        /* successfully validate rebinding addr */
        xqc_memcpy(path->peer_addr, path->rebinding_addr, path->rebinding_addrlen);
        path->peer_addrlen = path->rebinding_addrlen;
        path->addr_str_len = 0;
        xqc_log(conn->log, XQC_LOG_INFO, "|path:%ui|REBINDING|validate NAT rebinding addr|path:%s|", path->path_id, xqc_path_addr_str(path));

        xqc_memcpy(conn->peer_addr, path->rebinding_addr, path->rebinding_addrlen);
        conn->peer_addrlen = path->rebinding_addrlen;
        conn->addr_str_len = 0;
        xqc_log(conn->log, XQC_LOG_INFO, "|path:%ui|REBINDING|validate NAT rebinding addr|conn:%s|", path->path_id, xqc_conn_addr_str(conn));

        if (conn->transport_cbs.conn_peer_addr_changed_notify) {
            conn->transport_cbs.conn_peer_addr_changed_notify(conn, xqc_conn_get_user_data(conn));
        }

        path->rebinding_valid++;
        path->rebinding_addrlen = 0;
        path->rebinding_check_response = 0;
        xqc_timer_unset(&path->path_send_ctl->path_timer_manager, XQC_TIMER_NAT_REBINDING);
    }

    return XQC_OK;
}
