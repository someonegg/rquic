/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include <string.h>
#include <sys/types.h>
#include "src/transport/xqc_frame_parser.h"
#include "src/common/utils/vint/xqc_variable_len_int.h"
#include "src/common/xqc_log.h"
#include "src/common/xqc_log_event_callback.h"
#include "src/common/xqc_str.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_stream.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_packet_parser.h"

ssize_t
xqc_gen_stream_frame(xqc_packet_out_t *packet_out,
    xqc_stream_id_t stream_id, uint64_t offset, uint8_t fin,
    const unsigned char *payload, size_t size, size_t *written_size)
{
    /*
     * 0b00001XXX
     *  0x4     OFF
     *  0x2     LEN
     *  0x1     FIN
     */

    /*
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                         Stream ID (i)                       ...
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                         [Offset (i)]                        ...
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                         [Length (i)]                        ...
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    |                        Stream Data (*)                      ...
    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
     */

    unsigned char *dst_buf = packet_out->po_buf + packet_out->po_used_size;
    size_t dst_buf_len = xqc_get_po_remained_size(packet_out);

    *written_size = 0;
    /*  variable length integer's most significant 2 bits */
    unsigned stream_id_bits, offset_bits, length_bits;
    /* variable length integer's size(byte) */
    unsigned stream_id_len, offset_len, length_len;
    /* 0b00001XXX point to second byte */
    unsigned char *p = dst_buf + 1;
    /* fin_only means there is no stream data */
    uint8_t fin_only = (fin && !size);

    unsigned int idx = packet_out->po_stream_frames_idx;
    unsigned int prev_idx = idx - 1;
    if (idx >= XQC_MAX_STREAM_FRAME_IN_PO) {
        return -XQC_ELIMIT;
    }

    /* Try to combine with previous stream frame */
    if (idx > 0 && packet_out->po_frame_types == XQC_FRAME_BIT_STREAM /* No other frames */
        && packet_out->po_stream_frames[prev_idx].ps_stream_id == stream_id
        && packet_out->po_stream_frames[prev_idx].ps_offset + packet_out->po_stream_frames[prev_idx].ps_length == offset
        && packet_out->po_stream_frames[prev_idx].ps_length_offset > 0 /* Length field is present */)
    {
        unsigned char *p_type = packet_out->po_buf + packet_out->po_stream_frames[prev_idx].ps_type_offset;
        unsigned char *p_length = packet_out->po_buf + packet_out->po_stream_frames[prev_idx].ps_length_offset;
        size_t append_size = 0;

        /* Length is 2 Bytes */
        if ((*p_length & 0xC0) != 0x40) {
            goto new_frame;
        }

        if (!fin_only) {
            append_size = xqc_min(size, dst_buf_len);
            memcpy(dst_buf, payload, append_size);
            xqc_vint_write(p_length, packet_out->po_stream_frames[prev_idx].ps_length + append_size, 1, 2);
            packet_out->po_stream_frames[prev_idx].ps_length += append_size;
            if (append_size != size) {
                fin = 0;
            }
        }

        if (fin) {
            *p_type |= 0x01;
            packet_out->po_stream_frames[prev_idx].ps_has_fin = fin;
        }

        *written_size = append_size;
        return append_size;
    }

new_frame:
    stream_id_bits = xqc_vint_get_2bit(stream_id);
    stream_id_len = xqc_vint_len(stream_id_bits);
    if (offset) {
        offset_bits = xqc_vint_get_2bit(offset);
        offset_len = xqc_vint_len(offset_bits);

    } else {
        offset_len = 0;
    }

    if (!fin_only) {
        ssize_t n_avail;

        n_avail = dst_buf_len - (p + stream_id_len + offset_len - dst_buf);

        /*
         * If we cannot fill remaining buffer, we need to include data
         * length.
         */
        if (size <= n_avail) {
            /* length_len set to 2 bytes, easy to combine with other stream frame */
            length_bits = 1;
            length_len = 2;
            n_avail -= length_len;
            if (size > n_avail) {
                size = n_avail;
                fin = 0;
            }

        } else {
            /* reserve ACK, must have length. */
            size = n_avail;
            length_bits = 1;
            length_len = 2;
            size -= length_len;
            fin = 0;
        }

        if (n_avail <= 0 || size > n_avail) {
            return -XQC_ENOBUF;
        }

        xqc_vint_write(p, stream_id, stream_id_bits, stream_id_len);
        p += stream_id_len;

        if (offset_len) {
            xqc_vint_write(p, offset, offset_bits, offset_len);
        }
        p += offset_len;

        memcpy(p + length_len, payload, size);
        *written_size = size;

        if (length_len) {
            xqc_vint_write(p, size, length_bits, length_len);
            packet_out->po_stream_frames[idx].ps_length_offset = (unsigned int)(p - packet_out->po_buf);
        }

        p += length_len + size;

    } else {
        /* check if there is enough space to put Length */
        length_len = 1 + stream_id_len + offset_len < dst_buf_len ? 1 : 0;
        if (1 + stream_id_len + offset_len + length_len > dst_buf_len) {
            return -XQC_ENOBUF;
        }
        xqc_vint_write(p, stream_id, stream_id_bits, stream_id_len);
        p += stream_id_len;

        if (offset_len) {
            xqc_vint_write(p, offset, offset_bits, offset_len);
        }
        p += offset_len;

        if (length_len) {
            *p++ = 0;
        } else {
            packet_out->po_flag |= XQC_POF_STREAM_NO_LEN;
        }
    }

    dst_buf[0] = 0x08
                 | (!!offset_len << 2)
                 | (!!length_len << 1)
                 | (!!fin << 0);

    packet_out->po_stream_frames[idx].ps_type_offset = (unsigned int)(dst_buf - packet_out->po_buf);
    packet_out->po_stream_frames[idx].ps_offset = offset;
    packet_out->po_stream_frames[idx].ps_length = (unsigned int)size;
    packet_out->po_stream_frames[idx].ps_is_used = 1;
    packet_out->po_stream_frames[idx].ps_stream_id = stream_id;
    packet_out->po_stream_frames[idx].ps_has_fin = fin;
    packet_out->po_stream_frames_idx++;

    packet_out->po_frame_types |= XQC_FRAME_BIT_STREAM;

    return p - dst_buf;
}

xqc_int_t
xqc_parse_stream_frame(xqc_packet_in_t *packet_in, xqc_connection_t *conn,
    xqc_stream_frame_t *frame, xqc_stream_id_t *stream_id)
{
    uint64_t offset;
    uint64_t length;
    int      vlen;

    const unsigned char *p = packet_in->pos;
    const unsigned char *end = packet_in->last;

    const unsigned char first_byte = *p++;

    vlen = xqc_vint_read(p, end, stream_id);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    if (first_byte & 0x04) {
        vlen = xqc_vint_read(p, end, &offset);
        if (vlen < 0) {
            return -XQC_EVINTREAD;
        }
        p += vlen;
        frame->data_offset = offset;

    } else {
        frame->data_offset = 0;
    }

    if (first_byte & 0x02) {
        vlen = xqc_vint_read(p, end, &length);
        if (vlen < 0) {
            return -XQC_EVINTREAD;
        }

        p += vlen;
        if (length > end - p) {
            xqc_log(conn->log, XQC_LOG_ERROR, "|parse stream frame error|stream length:%d|packet length:%d",length, end - p);
            return -XQC_EILLFRAME;
        }
        frame->data_length = length;

    } else {
        frame->data_length = end - p;
    }

    if (first_byte & 0x01) {
        frame->fin = 1;

    } else {
        frame->fin = 0;
    }

    if (frame->data_length > 0) {
        frame->data = xqc_malloc(frame->data_length);
        if (!frame->data) {
            return -XQC_EMALLOC;
        }
        memcpy(frame->data, p, frame->data_length);
    }
    p += frame->data_length;

    packet_in->pos = (unsigned char *)p;
    packet_in->pi_frame_types |= XQC_FRAME_BIT_STREAM;

    xqc_log_event(conn->log, TRA_FRAMES_PROCESSED, XQC_FRAME_STREAM, frame);
    return XQC_OK;
}

void
xqc_gen_padding_frame(xqc_connection_t *conn, xqc_packet_out_t *packet_out)
{
    size_t total_len = XQC_PACKET_INITIAL_MIN_LENGTH;

    if (packet_out->po_used_size < total_len) {
        packet_out->po_padding = packet_out->po_buf + packet_out->po_used_size;
        memset(packet_out->po_padding, 0, total_len - packet_out->po_used_size);
        packet_out->po_used_size = total_len;
        packet_out->po_frame_types |= XQC_FRAME_BIT_PADDING;
    }
}

xqc_int_t
xqc_parse_padding_frame(xqc_packet_in_t *packet_in, xqc_connection_t *conn)
{
    packet_in->pi_frame_types |= XQC_FRAME_BIT_PADDING;
    packet_in->pos++;   /* skip frame type 0x00 */
    uint32_t length = 1;

    /* skip all padding bytes(0x00) */
    while (packet_in->pos < packet_in->last && *packet_in->pos == 0x00) {
        packet_in->pos++;
        length++;
    }

    xqc_log_event(conn->log, TRA_FRAMES_PROCESSED, XQC_FRAME_PADDING, length);
    return XQC_OK;
}

ssize_t
xqc_gen_ping_frame(xqc_packet_out_t *packet_out)
{
    /* Client send ping, server respond ack */
    unsigned char *dst_buf = packet_out->po_buf + packet_out->po_used_size;
    const unsigned char *begin = dst_buf;
    unsigned need = 1;
    if (need > xqc_get_po_remained_size(packet_out)) {
        return -XQC_ENOBUF;
    }
    *dst_buf++ = 0x01;
    packet_out->po_frame_types |= XQC_FRAME_BIT_PING;

    return dst_buf - begin;
}

xqc_int_t
xqc_parse_ping_frame(xqc_packet_in_t *packet_in, xqc_connection_t *conn)
{
    ++packet_in->pos;
    packet_in->pi_frame_types |= XQC_FRAME_BIT_PING;

    xqc_log_event(conn->log, TRA_FRAMES_PROCESSED, XQC_FRAME_PING);
    return XQC_OK;
}

/*
 *
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                     Largest Acknowledged (i)                ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                          ACK Delay (i)                      ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                       ACK Range Count (i)                   ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                       First ACK Range (i)                   ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                          ACK Ranges (*)                     ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                          [ECN Counts]                       ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

                        Figure 17: ACK Frame Format

    FOR EXAMPLE:
    110 109 108 107 106 105 104 103 102 101 100   //pkt num
    1   1   1   0   1   1   0   0   1   1   1     //1 means received

    Largest Acknowledged 110
    ACK Range Count 2
    First ACK Range 2
    Gap 0 Ack Range 1
    Gap 1 Ack Range 2
 */
ssize_t
xqc_gen_ack_frame(xqc_connection_t *conn, xqc_packet_out_t *packet_out, xqc_usec_t now,
    int ack_delay_exponent, xqc_recv_record_t *recv_record, xqc_usec_t largest_pkt_recv_time,
    int *has_gap, xqc_packet_number_t *largest_ack)
{
    unsigned char *dst_buf = packet_out->po_buf + packet_out->po_used_size;
    size_t dst_buf_len = xqc_get_po_remained_size_with_ack_spc(packet_out);

    xqc_packet_number_t largest_recv, prev_low;
    xqc_usec_t ack_delay;

    const unsigned char *begin = dst_buf;
    const unsigned char *end = dst_buf + dst_buf_len;
    unsigned char *p_range_count;
    unsigned range_count = 0, first_ack_range, gap, acks, gap_bits, acks_bits, need;

    xqc_list_head_t *pos, *next;
    xqc_pktno_range_node_t *range_node;

    xqc_pktno_range_node_t *first_range = NULL;
    xqc_list_for_each_safe(pos, next, &recv_record->list_head) {
        first_range = xqc_list_entry(pos, xqc_pktno_range_node_t, list);
        break;
    }

    if (first_range == NULL) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|recv_record empty|");
        return -XQC_ENULLPTR;
    }

    ack_delay = (now - largest_pkt_recv_time);
    largest_recv = first_range->pktno_range.high;
    first_ack_range = largest_recv - first_range->pktno_range.low;
    prev_low = first_range->pktno_range.low;

    ack_delay = ack_delay >> ack_delay_exponent;

    unsigned largest_recv_bits = xqc_vint_get_2bit(largest_recv);
    unsigned ack_delay_bits = xqc_vint_get_2bit(ack_delay);
    unsigned first_ack_range_bits = xqc_vint_get_2bit(first_ack_range);

    need = 1    /* type */
            + xqc_vint_len(largest_recv_bits)
            + xqc_vint_len(ack_delay_bits)
            + 1 /* range_count */
            + xqc_vint_len(first_ack_range_bits);

    if (dst_buf + need > end) {
        return -XQC_ENOBUF;
    }

    *dst_buf++ = 0x02;

    xqc_vint_write(dst_buf, largest_recv, largest_recv_bits, xqc_vint_len(largest_recv_bits));
    dst_buf += xqc_vint_len(largest_recv_bits);

    *largest_ack = largest_recv;

    xqc_vint_write(dst_buf, ack_delay, ack_delay_bits, xqc_vint_len(ack_delay_bits));
    dst_buf += xqc_vint_len(ack_delay_bits);

    p_range_count = dst_buf;
    dst_buf += 1;   /* max range_count 63, 1 byte */

    xqc_vint_write(dst_buf, first_ack_range, first_ack_range_bits, xqc_vint_len(first_ack_range_bits));
    dst_buf += xqc_vint_len(first_ack_range_bits);

    int is_first = 1;
    xqc_list_for_each_safe(pos, next, &recv_record->list_head) {    /* from second node */
        range_node = xqc_list_entry(pos, xqc_pktno_range_node_t, list);

        if (is_first) {
            is_first = 0;
            continue;
        }

        gap = prev_low - range_node->pktno_range.high - 2;
        acks = range_node->pktno_range.high - range_node->pktno_range.low;

        gap_bits = xqc_vint_get_2bit(gap);
        acks_bits = xqc_vint_get_2bit(acks);

        need = xqc_vint_len(gap_bits) + xqc_vint_len(acks_bits);
        if (dst_buf + need > end) {
            return -XQC_ENOBUF;
        }

        xqc_vint_write(dst_buf, gap, gap_bits, xqc_vint_len(gap_bits));
        dst_buf += xqc_vint_len(gap_bits);

        xqc_vint_write(dst_buf, acks, acks_bits, xqc_vint_len(acks_bits));
        dst_buf += xqc_vint_len(acks_bits);

        prev_low = range_node->pktno_range.low;

        ++range_count;
        if (range_count >= XQC_MAX_ACK_RANGE_CNT - 1) {
            break;
        }
    }

    if (range_count > 0) {
        *has_gap = 1;

    } else {
        *has_gap = 0;
    }
    xqc_vint_write(p_range_count, range_count, 0, 1);

    packet_out->po_frame_types |= XQC_FRAME_BIT_ACK;
    return dst_buf - begin;
}

/**
 * parse ack frame to ack_info
 */
xqc_int_t
xqc_parse_ack_frame(xqc_packet_in_t *packet_in, xqc_connection_t *conn, xqc_ack_info_t *ack_info)
{
    unsigned char *p = packet_in->pos;
    const unsigned char *end = packet_in->last;

    int vlen;
    uint64_t frame_type;
    vlen = xqc_vint_read(p, end, &frame_type);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;
    uint64_t largest_acked;
    uint64_t ack_range_count;   /* the actual range cnt */
    uint64_t first_ack_range;
    uint64_t range, gap;

    unsigned n_ranges = 0;      /* the range cnt stored */

    /*
     * mpquic draft-04: If the multipath extension has been successfully
     * negotiated, ACK frames in 1-RTT packets acknowledge packets sent
     * with the Connection ID having sequence number 0.
     */
    ack_info->path_id = 0;
    ack_info->pns = packet_in->pi_pkt.pkt_pns;

    vlen = xqc_vint_read(p, end, &largest_acked);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;
    ack_info->largest_acked = largest_acked;

    vlen = xqc_vint_read(p, end, &ack_info->ack_delay);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    ack_info->ack_delay = ack_info->ack_delay << conn->remote_settings.ack_delay_exponent;

    vlen = xqc_vint_read(p, end, &ack_range_count);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    vlen = xqc_vint_read(p, end, &first_ack_range);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    ack_info->ranges[n_ranges].high = largest_acked;
    ack_info->ranges[n_ranges].low = largest_acked - first_ack_range;
    n_ranges++;

    for (int i = 0; i < ack_range_count; ++i) {
        vlen = xqc_vint_read(p, end, &gap);
        if (vlen < 0) {
            return -XQC_EVINTREAD;
        }
        p += vlen;

        vlen = xqc_vint_read(p, end, &range);
        if (vlen < 0) {
            return -XQC_EVINTREAD;
        }
        p += vlen;

        if (n_ranges < XQC_MAX_ACK_RANGE_CNT) {
            ack_info->ranges[n_ranges].high = ack_info->ranges[n_ranges - 1].low - gap - 2;
            ack_info->ranges[n_ranges].low = ack_info->ranges[n_ranges].high - range;
            n_ranges++;
        }
    }

    /*
     * if the actual ack_range_count plus first ack_range is larger than
     * the XQC_MAX_ACK_RANGE_CNT, ack_info don't have enough space to store
     *  all the ack_ranges
     */
    if (ack_range_count + 1 > XQC_MAX_ACK_RANGE_CNT) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|ACK range exceed XQC_MAX_ACK_RANGE_CNT|");
    }

    ack_info->n_ranges = n_ranges;
    packet_in->pos = p;
    packet_in->pi_frame_types |= XQC_FRAME_BIT_ACK;

    xqc_log_event(conn->log, TRA_FRAMES_PROCESSED, XQC_FRAME_ACK, ack_info);
    return XQC_OK;
}

/*
 *
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                         Error Code (i)                      ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                       [ Frame Type (i) ]                    ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                    Reason Phrase Length (i)                 ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                        Reason Phrase (*)                    ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
ssize_t
xqc_gen_conn_close_frame(xqc_packet_out_t *packet_out,
    uint64_t err_code, int is_app, int frame_type)
{
    unsigned char *dst_buf = packet_out->po_buf + packet_out->po_used_size;
    const unsigned char *begin = dst_buf;

    unsigned char *reason = NULL;
    int reason_len = 0;

    unsigned frame_type_bits = xqc_vint_get_2bit(frame_type);
    unsigned reason_len_bits = xqc_vint_get_2bit(reason_len);
    unsigned err_code_len_bits = xqc_vint_get_2bit(err_code);

    unsigned need = 1
                    + xqc_vint_len(err_code_len_bits)
                    + xqc_vint_len(frame_type_bits)
                    + xqc_vint_len(reason_len_bits)
                    + reason_len;
    if (need > xqc_get_po_remained_size(packet_out)) {
        return -XQC_ENOBUF;
    }

    if (is_app) {
        *dst_buf++ = 0x1d;

    } else {
        *dst_buf++ = 0x1c;
    }

    xqc_vint_write(dst_buf, err_code, err_code_len_bits, xqc_vint_len(err_code_len_bits));
    dst_buf += xqc_vint_len(err_code_len_bits);

    if (!is_app) {
        xqc_vint_write(dst_buf, frame_type, frame_type_bits, xqc_vint_len(frame_type_bits));
        dst_buf += xqc_vint_len(frame_type_bits);
    }

    xqc_vint_write(dst_buf, reason_len, reason_len_bits, xqc_vint_len(reason_len_bits));
    dst_buf += xqc_vint_len(reason_len_bits);

#if 0   /* TODO: reason not supported yet */
    if (reason_len > 0) {
        memcpy(dst_buf, reason, reason_len);
        dst_buf += reason_len;
    }
#endif

    packet_out->po_frame_types |= XQC_FRAME_BIT_CONNECTION_CLOSE;

    return dst_buf - begin;
}

xqc_int_t
xqc_parse_conn_close_frame(xqc_packet_in_t *packet_in, uint64_t *err_code, xqc_connection_t *conn)
{
    unsigned char *p = packet_in->pos;
    const unsigned char *end = packet_in->last;
    const unsigned char first_byte = *p++;

    int vlen;
    uint64_t reason_len;
    uint64_t frame_type;

    vlen = xqc_vint_read(p, end, err_code);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    if (first_byte == 0x1c) {
        vlen = xqc_vint_read(p, end, &frame_type);
        if (vlen < 0) {
            return -XQC_EVINTREAD;
        }
        p += vlen;
    }

    vlen = xqc_vint_read(p, end, &reason_len);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    /* TODO: get reason string */
    p += reason_len;

    packet_in->pos = p;

    packet_in->pi_frame_types |= XQC_FRAME_BIT_CONNECTION_CLOSE;

    xqc_log_event(conn->log, TRA_FRAMES_PROCESSED, XQC_FRAME_CONNECTION_CLOSE, *err_code);
    return XQC_OK;
}

/*
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                        Stream ID (i)                        ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                  Application Error Code (i)                 ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                        Final Size (i)                       ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

 */
ssize_t
xqc_gen_reset_stream_frame(xqc_packet_out_t *packet_out, xqc_stream_id_t stream_id,
    uint64_t err_code, uint64_t final_size)
{
    unsigned char *dst_buf = packet_out->po_buf + packet_out->po_used_size;
    const unsigned char *begin = dst_buf;

    unsigned final_size_bits = xqc_vint_get_2bit(final_size);
    unsigned stream_id_bits = xqc_vint_get_2bit(stream_id);
    unsigned err_code_bits = xqc_vint_get_2bit(err_code);

    unsigned need = 1
                    + xqc_vint_len(stream_id_bits)
                    + xqc_vint_len(err_code_bits)
                    + xqc_vint_len(final_size_bits)
                    ;
    if (need > xqc_get_po_remained_size(packet_out)) {
        return -XQC_ENOBUF;
    }

    *dst_buf++ = 0x04;

    xqc_vint_write(dst_buf, stream_id, stream_id_bits, xqc_vint_len(stream_id_bits));
    dst_buf += xqc_vint_len(stream_id_bits);

    xqc_vint_write(dst_buf, err_code, err_code_bits, xqc_vint_len(err_code_bits));
    dst_buf += xqc_vint_len(err_code_bits);

    xqc_vint_write(dst_buf, final_size, final_size_bits, xqc_vint_len(final_size_bits));
    dst_buf += xqc_vint_len(final_size_bits);

    packet_out->po_frame_types |= XQC_FRAME_BIT_RESET_STREAM;

    return dst_buf - begin;
}

xqc_int_t
xqc_parse_reset_stream_frame(xqc_packet_in_t *packet_in, xqc_stream_id_t *stream_id,
    uint64_t *err_code, uint64_t *final_size, xqc_connection_t *conn)
{
    unsigned char *p = packet_in->pos;
    const unsigned char *end = packet_in->last;
    const unsigned char first_byte = *p++;

    int vlen;

    vlen = xqc_vint_read(p, end, stream_id);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    vlen = xqc_vint_read(p, end, err_code);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    vlen = xqc_vint_read(p, end, final_size);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    packet_in->pos = p;

    packet_in->pi_frame_types |= XQC_FRAME_BIT_RESET_STREAM;

    xqc_log_event(conn->log, TRA_FRAMES_PROCESSED, XQC_FRAME_RESET_STREAM, *stream_id, *err_code, *final_size);
    return XQC_OK;
}

/*
 *
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                        Stream ID (i)                        ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                  Application Error Code (i)                 ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
ssize_t
xqc_gen_stop_sending_frame(xqc_packet_out_t *packet_out, xqc_stream_id_t stream_id,
    uint64_t err_code)
{
    unsigned char *dst_buf = packet_out->po_buf + packet_out->po_used_size;
    const unsigned char *begin = dst_buf;

    unsigned stream_id_bits = xqc_vint_get_2bit(stream_id);
    unsigned err_code_bits = xqc_vint_get_2bit(err_code);

    unsigned need = 1
                    + xqc_vint_len(stream_id_bits)
                    + xqc_vint_len(err_code_bits)
                    ;
    if (need > xqc_get_po_remained_size(packet_out)) {
        return -XQC_ENOBUF;
    }

    *dst_buf++ = 0x05;

    xqc_vint_write(dst_buf, stream_id, stream_id_bits, xqc_vint_len(stream_id_bits));
    dst_buf += xqc_vint_len(stream_id_bits);

    xqc_vint_write(dst_buf, err_code, err_code_bits, xqc_vint_len(err_code_bits));
    dst_buf += xqc_vint_len(err_code_bits);

    packet_out->po_frame_types |= XQC_FRAME_BIT_STOP_SENDING;

    return dst_buf - begin;
}

xqc_int_t
xqc_parse_stop_sending_frame(xqc_packet_in_t *packet_in, xqc_stream_id_t *stream_id,
    uint64_t *err_code, xqc_connection_t *conn)
{
    unsigned char *p = packet_in->pos;
    const unsigned char *end = packet_in->last;
    const unsigned char first_byte = *p++;

    int vlen;

    vlen = xqc_vint_read(p, end, stream_id);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    vlen = xqc_vint_read(p, end, err_code);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    packet_in->pos = p;

    packet_in->pi_frame_types |= XQC_FRAME_BIT_STOP_SENDING;

    xqc_log_event(conn->log, TRA_FRAMES_PROCESSED, XQC_FRAME_STOP_SENDING, *stream_id, *err_code);
    return XQC_OK;
}

/*
 *     0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                       Data Limit (i)                        ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
ssize_t
xqc_gen_data_blocked_frame(xqc_packet_out_t *packet_out, uint64_t data_limit)
{
    unsigned char *dst_buf = packet_out->po_buf + packet_out->po_used_size;
    const unsigned char *begin = dst_buf;

    *dst_buf++ = 0x14;

    unsigned data_limit_bits = xqc_vint_get_2bit(data_limit);
    xqc_vint_write(dst_buf, data_limit, data_limit_bits, xqc_vint_len(data_limit_bits));
    dst_buf += xqc_vint_len(data_limit_bits);

    packet_out->po_frame_types |= XQC_FRAME_BIT_DATA_BLOCKED;

    return dst_buf - begin;
}

xqc_int_t
xqc_parse_data_blocked_frame(xqc_packet_in_t *packet_in, uint64_t *data_limit, xqc_connection_t *conn)
{
    unsigned char *p = packet_in->pos;
    const unsigned char *end = packet_in->last;
    const unsigned char first_byte = *p++;

    int vlen;

    vlen = xqc_vint_read(p, end, data_limit);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    packet_in->pos = p;

    packet_in->pi_frame_types |= XQC_FRAME_BIT_DATA_BLOCKED;

    xqc_log_event(conn->log, TRA_FRAMES_PROCESSED, XQC_FRAME_DATA_BLOCKED, *data_limit);
    return XQC_OK;
}

/*
 *     0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                        Stream ID (i)                        ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                    Stream Data Limit (i)                    ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
ssize_t
xqc_gen_stream_data_blocked_frame(xqc_packet_out_t *packet_out, xqc_stream_id_t stream_id, uint64_t stream_data_limit)
{
    unsigned char *dst_buf = packet_out->po_buf + packet_out->po_used_size;
    const unsigned char *begin = dst_buf;

    *dst_buf++ = 0x15;

    unsigned stream_id_bits = xqc_vint_get_2bit(stream_id);
    unsigned data_limit_bits = xqc_vint_get_2bit(stream_data_limit);

    xqc_vint_write(dst_buf, stream_id, stream_id_bits, xqc_vint_len(stream_id_bits));
    dst_buf += xqc_vint_len(stream_id_bits);

    xqc_vint_write(dst_buf, stream_data_limit, data_limit_bits, xqc_vint_len(data_limit_bits));
    dst_buf += xqc_vint_len(data_limit_bits);

    packet_out->po_frame_types |= XQC_FRAME_BIT_STREAM_DATA_BLOCKED;

    return dst_buf - begin;
}

xqc_int_t
xqc_parse_stream_data_blocked_frame(xqc_packet_in_t *packet_in, xqc_stream_id_t *stream_id, uint64_t *stream_data_limit, xqc_connection_t *conn)
{
    unsigned char *p = packet_in->pos;
    const unsigned char *end = packet_in->last;
    const unsigned char first_byte = *p++;

    int vlen;

    vlen = xqc_vint_read(p, end, stream_id);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    vlen = xqc_vint_read(p, end, stream_data_limit);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    packet_in->pos = p;

    packet_in->pi_frame_types |= XQC_FRAME_BIT_STREAM_DATA_BLOCKED;

    xqc_log_event(conn->log, TRA_FRAMES_PROCESSED, XQC_FRAME_STREAM_DATA_BLOCKED, *stream_id, *stream_data_limit);
    return XQC_OK;
}

/*
 *  0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                        Stream Limit (i)                     ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
ssize_t
xqc_gen_streams_blocked_frame(xqc_packet_out_t *packet_out, uint64_t stream_limit, int bidirectional)
{
    unsigned char *dst_buf = packet_out->po_buf + packet_out->po_used_size;
    const unsigned char *begin = dst_buf;

    if (bidirectional) {
        *dst_buf++ = 0x16;

    } else {
        *dst_buf++ = 0x17;
    }

    unsigned stream_limit_bits = xqc_vint_get_2bit(stream_limit);
    xqc_vint_write(dst_buf, stream_limit, stream_limit_bits, xqc_vint_len(stream_limit_bits));
    dst_buf += xqc_vint_len(stream_limit_bits);

    packet_out->po_frame_types |= XQC_FRAME_BIT_STREAMS_BLOCKED;

    return dst_buf - begin;
}

xqc_int_t
xqc_parse_streams_blocked_frame(xqc_packet_in_t *packet_in, uint64_t *stream_limit, int *bidirectional, xqc_connection_t *conn)
{
    unsigned char *p = packet_in->pos;
    const unsigned char *end = packet_in->last;
    const unsigned char first_byte = *p++;

    int vlen;

    if (first_byte == 0x16) {
        *bidirectional = 1;

    } else {
        *bidirectional = 0;
    }

    vlen = xqc_vint_read(p, end, stream_limit);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    packet_in->pos = p;

    packet_in->pi_frame_types |= XQC_FRAME_BIT_STREAMS_BLOCKED;

    xqc_log_event(conn->log, TRA_FRAMES_PROCESSED, XQC_FRAME_STREAMS_BLOCKED, *bidirectional, *stream_limit);
    return XQC_OK;
}

/*
 *
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                        Maximum Data (i)                     ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
ssize_t
xqc_gen_max_data_frame(xqc_packet_out_t *packet_out, uint64_t max_data)
{
    unsigned char *dst_buf = packet_out->po_buf + packet_out->po_used_size;
    const unsigned char *begin = dst_buf;

    *dst_buf++ = 0x10;

    unsigned max_data_bits = xqc_vint_get_2bit(max_data);
    xqc_vint_write(dst_buf, max_data, max_data_bits, xqc_vint_len(max_data_bits));
    dst_buf += xqc_vint_len(max_data_bits);

    packet_out->po_frame_types |= XQC_FRAME_BIT_MAX_DATA;

    return dst_buf - begin;
}

xqc_int_t
xqc_parse_max_data_frame(xqc_packet_in_t *packet_in, uint64_t *max_data, xqc_connection_t *conn)
{
    unsigned char *p = packet_in->pos;
    const unsigned char *end = packet_in->last;
    const unsigned char first_byte = *p++;

    int vlen;

    vlen = xqc_vint_read(p, end, max_data);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    packet_in->pos = p;

    packet_in->pi_frame_types |= XQC_FRAME_BIT_MAX_DATA;

    xqc_log_event(conn->log, TRA_FRAMES_PROCESSED, XQC_FRAME_MAX_DATA, *max_data);
    return XQC_OK;
}

/*
 *     0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                        Stream ID (i)                        ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                    Maximum Stream Data (i)                  ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
ssize_t
xqc_gen_max_stream_data_frame(xqc_packet_out_t *packet_out, xqc_stream_id_t stream_id, uint64_t max_stream_data)
{
    unsigned char *dst_buf = packet_out->po_buf + packet_out->po_used_size;
    const unsigned char *begin = dst_buf;

    *dst_buf++ = 0x11;

    unsigned stream_id_bits = xqc_vint_get_2bit(stream_id);
    unsigned max_stream_data_bits = xqc_vint_get_2bit(max_stream_data);

    xqc_vint_write(dst_buf, stream_id, stream_id_bits, xqc_vint_len(stream_id_bits));
    dst_buf += xqc_vint_len(stream_id_bits);

    xqc_vint_write(dst_buf, max_stream_data, max_stream_data_bits, xqc_vint_len(max_stream_data_bits));
    dst_buf += xqc_vint_len(max_stream_data_bits);

    packet_out->po_frame_types |= XQC_FRAME_BIT_MAX_STREAM_DATA;

    return dst_buf - begin;
}

xqc_int_t
xqc_parse_max_stream_data_frame(xqc_packet_in_t *packet_in, xqc_stream_id_t *stream_id, uint64_t *max_stream_data, xqc_connection_t *conn)
{
    unsigned char *p = packet_in->pos;
    const unsigned char *end = packet_in->last;
    const unsigned char first_byte = *p++;

    int vlen;

    vlen = xqc_vint_read(p, end, stream_id);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    vlen = xqc_vint_read(p, end, max_stream_data);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    packet_in->pos = p;

    packet_in->pi_frame_types |= XQC_FRAME_BIT_MAX_STREAM_DATA;

    xqc_log_event(conn->log, TRA_FRAMES_PROCESSED, XQC_FRAME_MAX_STREAM_DATA, *stream_id, *max_stream_data);
    return XQC_OK;
}

/*
 *
    0                   1                   2                   3
    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                     Maximum Streams (i)                     ...
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
ssize_t
xqc_gen_max_streams_frame(xqc_packet_out_t *packet_out, uint64_t max_streams, int bidirectional)
{
    unsigned char *dst_buf = packet_out->po_buf + packet_out->po_used_size;
    const unsigned char *begin = dst_buf;

    if (bidirectional) {
        *dst_buf++ = 0x12;

    } else {
        *dst_buf++ = 0x13;
    }

    unsigned max_streams_bits = xqc_vint_get_2bit(max_streams);
    xqc_vint_write(dst_buf, max_streams, max_streams_bits, xqc_vint_len(max_streams_bits));
    dst_buf += xqc_vint_len(max_streams_bits);

    packet_out->po_frame_types |= XQC_FRAME_BIT_MAX_STREAMS;

    return dst_buf - begin;
}

xqc_int_t
xqc_parse_max_streams_frame(xqc_packet_in_t *packet_in, uint64_t *max_streams, int *bidirectional, xqc_connection_t *conn)
{
    unsigned char *p = packet_in->pos;
    const unsigned char *end = packet_in->last;
    const unsigned char first_byte = *p++;

    int vlen;

    if (first_byte == 0x12) {
        *bidirectional = 1;

    } else {
        *bidirectional = 0;
    }

    vlen = xqc_vint_read(p, end, max_streams);
    if (vlen < 0) {
        return -XQC_EVINTREAD;
    }
    p += vlen;

    packet_in->pos = p;

    packet_in->pi_frame_types |= XQC_FRAME_BIT_MAX_STREAMS;

    xqc_log_event(conn->log, TRA_FRAMES_PROCESSED, XQC_FRAME_MAX_STREAM_DATA, *bidirectional, *max_streams);
    return XQC_OK;
}

/*
 * https://datatracker.ietf.org/doc/html/rfc9000#section-19.17
 *
 * PATH_CHALLENGE Frame {
 *    Type (i) = 0x1a,
 *    Data (64),
 * }
 *
 *               Figure 41: PATH_CHALLENGE Frame Format
 */

ssize_t
xqc_gen_path_challenge_frame(xqc_packet_out_t *packet_out, unsigned char *data)
{
    unsigned char *dst_buf = packet_out->po_buf + packet_out->po_used_size;
    const unsigned char *begin = dst_buf;
    unsigned need = 0;

    uint64_t frame_type = 0x1a;
    unsigned frame_type_bits = xqc_vint_get_2bit(frame_type);
    need = xqc_vint_len(frame_type_bits) + XQC_PATH_CHALLENGE_DATA_LEN;

    /* check packout_out have enough buffer length */
    if (need > xqc_get_po_remained_size(packet_out)) {
        return -XQC_ENOBUF;
    }

    /* Type(i) */
    xqc_vint_write(dst_buf, frame_type, frame_type_bits, xqc_vint_len(frame_type_bits));
    dst_buf += xqc_vint_len(frame_type_bits);

    /* Data (64) */
    xqc_memcpy(dst_buf, data, XQC_PATH_CHALLENGE_DATA_LEN);
    dst_buf += XQC_PATH_CHALLENGE_DATA_LEN;

    packet_out->po_frame_types |= XQC_FRAME_BIT_PATH_CHALLENGE;

    return dst_buf - begin;
}

xqc_int_t
xqc_parse_path_challenge_frame(xqc_packet_in_t *packet_in, unsigned char *data)
{
    unsigned char *p = packet_in->pos;
    const unsigned char *end = packet_in->last;
    const unsigned char first_byte = *p++;

    if (p + XQC_PATH_CHALLENGE_DATA_LEN > end) {
        return -XQC_EVINTREAD;
    }
    xqc_memcpy(data, p, XQC_PATH_CHALLENGE_DATA_LEN);
    p += XQC_PATH_CHALLENGE_DATA_LEN;

    packet_in->pos = p;

    packet_in->pi_frame_types |= XQC_FRAME_BIT_PATH_CHALLENGE;

    return XQC_OK;
}

/*
 * https://datatracker.ietf.org/doc/html/rfc9000#section-19.18
 *
 * PATH_RESPONSE Frame {
 *    Type (i) = 0x1b,
 *    Data (64),
 * }
 *
 *               Figure 42: PATH_RESPONSE Frame Format
 */

ssize_t
xqc_gen_path_response_frame(xqc_packet_out_t *packet_out, unsigned char *data)
{
    unsigned char *dst_buf = packet_out->po_buf + packet_out->po_used_size;
    const unsigned char *begin = dst_buf;
    unsigned need = 0;

    uint64_t frame_type = 0x1b;
    unsigned frame_type_bits = xqc_vint_get_2bit(frame_type);
    need = xqc_vint_len(frame_type_bits) + XQC_PATH_CHALLENGE_DATA_LEN;

    /* check packout_out have enough buffer length */
    if (need > xqc_get_po_remained_size(packet_out)) {
        return -XQC_ENOBUF;
    }

    /* Type(i) */
    xqc_vint_write(dst_buf, frame_type, frame_type_bits, xqc_vint_len(frame_type_bits));
    dst_buf += xqc_vint_len(frame_type_bits);

    /* Data (64) */
    xqc_memcpy(dst_buf, data, XQC_PATH_CHALLENGE_DATA_LEN);
    dst_buf += XQC_PATH_CHALLENGE_DATA_LEN;

    packet_out->po_frame_types |= XQC_FRAME_BIT_PATH_RESPONSE;

    return dst_buf - begin;
}

xqc_int_t
xqc_parse_path_response_frame(xqc_packet_in_t *packet_in, unsigned char *data)
{
    unsigned char *p = packet_in->pos;
    const unsigned char *end = packet_in->last;
    const unsigned char first_byte = *p++;

    if (p + XQC_PATH_CHALLENGE_DATA_LEN > end) {
        return -XQC_EVINTREAD;
    }

    xqc_memcpy(data, p, XQC_PATH_CHALLENGE_DATA_LEN);
    p += XQC_PATH_CHALLENGE_DATA_LEN;

    packet_in->pos = p;

    packet_in->pi_frame_types |= XQC_FRAME_BIT_PATH_RESPONSE;

    return XQC_OK;
}
