/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include <string.h>
#include "src/transport/xqc_packet_parser.h"
#include "src/transport/xqc_cid.h"
#include "src/common/utils/vint/xqc_variable_len_int.h"
#include "src/transport/xqc_packet_out.h"
#include "src/common/xqc_algorithm.h"
#include "src/common/xqc_log.h"
#include "src/transport/xqc_engine.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_packet.h"
#include "src/transport/xqc_stream.h"
#include "src/transport/xqc_utils.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/transport/xqc_defs.h"
#include "src/common/xqc_random.h"

#define xqc_pktno_class_bytes(c) ((c) + 1)

unsigned
xqc_short_packet_header_size(unsigned char dcid_len, unsigned char pktno_class)
{
    return 1                    /* first byte */
           + dcid_len
           + XQC_PKTLEN_BYTES   /* Length (i) */
           + xqc_pktno_class_bytes(pktno_class);
}

unsigned
xqc_long_packet_header_size (unsigned char dcid_len, unsigned char scid_len,
    unsigned char pktno_class, xqc_pkt_type_t type)
{
    return 1                    /* first byte */
           + XQC_PKTVER_BYTES   /* version */
           + 2                  /* DCID Len (8) SCID Len (8) */
           + dcid_len
           + scid_len
           + XQC_PKTLEN_BYTES   /* Length (i) */
           + xqc_pktno_class_bytes(pktno_class);
}

xqc_int_t
xqc_packet_parse_cid(xqc_cid_t *dcid, xqc_cid_t *scid, uint8_t cid_len, const unsigned char *buf, size_t size)
{
    const unsigned char *pos = NULL;
    const unsigned char *end = buf + size;

    if (size <= 0) {
        return -XQC_EPARAM;
    }

    if ((buf[0] & 0x40) == 0 && (buf[0] & 0x80) == 0) {
        return -XQC_EILLPKT;
    }

    /* short header */
    if (XQC_PACKET_IS_SHORT_HEADER(buf)) {
        if (size < 1 + cid_len) {
            return -XQC_EILLPKT;
        }

        xqc_cid_set(dcid, buf + 1, cid_len);
        return XQC_OK;
    }

    /* long header */
    if (size < 1 + XQC_PKTVER_BYTES + 2) {
        return -XQC_EILLPKT;
    }

    pos = buf + 1 + XQC_PKTVER_BYTES;
    dcid->cid_len = (uint8_t)(*pos);
    if (dcid->cid_len > XQC_MAX_CID_LEN) {
        return -XQC_EILLPKT;
    }
    pos += 1;

    if (XQC_BUFF_LEFT_SIZE(pos, end) < dcid->cid_len + 1) {
        return -XQC_EILLPKT;
    }
    xqc_memcpy(dcid->cid_buf, pos, dcid->cid_len);
    pos += dcid->cid_len;

    scid->cid_len = (uint8_t)(*pos);
    if (scid->cid_len > XQC_MAX_CID_LEN) {
        return -XQC_EILLPKT;
    }
    pos += 1;

    if (XQC_BUFF_LEFT_SIZE(pos, end) < scid->cid_len) {
        return -XQC_EILLPKT;
    }
    xqc_memcpy(scid->cid_buf, pos, scid->cid_len);
    pos += scid->cid_len;

    return XQC_OK;
}

static void
xqc_packet_parse_packet_number(uint8_t *pos, xqc_uint_t pktno_bytes, uint64_t *packet_num)
{
    *packet_num = 0;
    for (int i = 0; i < pktno_bytes; i++) {
        *packet_num = ((*packet_num) << 8u) + (*pos);
        pos++;
    }
}

/**
 * https://datatracker.ietf.org/doc/html/rfc9000#appendix-A.3
 * @param largest_pn Largest received packet number
 * @param truncated_pn Packet number parsed from header
 * @param pn_nbits Number of bits the truncated_pn has
 * @return
 */
static xqc_packet_number_t
xqc_packet_calc_packet_number(xqc_packet_number_t largest_pn, xqc_packet_number_t truncated_pn,
    unsigned pn_nbits)
{
    xqc_packet_number_t expected_pn, pn_win, pn_hwin, pn_mask, candidate_pn;
    expected_pn = largest_pn + 1;
    pn_win = (xqc_packet_number_t) 1 << pn_nbits;
    pn_hwin = pn_win >> (xqc_packet_number_t) 1;
    pn_mask = pn_win - 1;

    candidate_pn = (expected_pn & ~pn_mask) | truncated_pn;

    // To fully align with RFC9000
    if ((candidate_pn + pn_hwin <= expected_pn)
        && (candidate_pn < ((1ULL << 62) - pn_win)))
    {
        return candidate_pn + pn_win;
    }

    if (candidate_pn > expected_pn + pn_hwin && candidate_pn >= pn_win) {
        return candidate_pn - pn_win;
    }

    return candidate_pn;
}

static void
xqc_packet_decode_packet_number(xqc_connection_t *c, xqc_packet_in_t *packet_in)
{
    uint8_t *header = (uint8_t *)packet_in->buf;
    uint8_t *pktno = header + packet_in->pi_pkt.pkt_num_offset;
    unsigned pktno_bytes = XQC_PACKET_HEADER_PKTNO_BYTES(header);

    /* parse packet number from header */
    xqc_packet_number_t truncated_pn;
    xqc_packet_parse_packet_number(pktno, pktno_bytes, &truncated_pn);

    /* decode packet number */
    xqc_packet_number_t largest_pn = 0;
    xqc_path_ctx_t *path = c->the_path;
    if (path) {
        xqc_pn_ctl_t *pn_ctl = xqc_get_pn_ctl(c, path);
        largest_pn = xqc_recv_record_largest(&pn_ctl->ctl_recv_record);
    }

    packet_in->pi_pkt.pkt_num =
        xqc_packet_calc_packet_number(largest_pn, truncated_pn, pktno_bytes * 8);
}

int
xqc_write_packet_number(unsigned char *buf, xqc_packet_number_t packet_number, unsigned char pktno_class)
{
    unsigned char *p = buf;
    unsigned pktno_bytes = xqc_pktno_class_bytes(pktno_class);
    if (pktno_bytes == 4) {
        *buf++ = packet_number >> 24;
    }

    if (pktno_bytes >= 3) {
        *buf++ = packet_number >> 16;
    }

    if (pktno_bytes >= 2) {
        *buf++ = packet_number >> 8;
    }
    *buf++ = packet_number;

    return buf - p;
}

void
xqc_update_packet_length(xqc_packet_out_t *packet_out)
{
    unsigned char *plength = packet_out->po_ppktno - XQC_PKTLEN_BYTES;
    unsigned length = packet_out->po_buf + packet_out->po_used_size - packet_out->po_ppktno;
    xqc_vint_write(plength, length, 0x01, 2);
}

void
xqc_short_packet_update_dcid(xqc_packet_out_t *packet_out, xqc_cid_t dcid)
{
    if (packet_out->po_pkt.pkt_type == XQC_PTYPE_SHORT_HEADER) {
        unsigned char *dst = packet_out->po_buf + 1;
        /* dcid len can't be changed */
        xqc_memcpy(dst, dcid.cid_buf, dcid.cid_len);
    }
}

/*
0                   1                   2                   3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+
|0|1|S|R|R|R|P P|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                Destination Connection ID (0..144)           ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           Length (i)                        ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Packet Number (8/16/24/32)               ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          Payload (*)                        ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                   Short Header Packet Format
*/

int
xqc_gen_short_packet_header(xqc_packet_out_t *packet_out, unsigned char *dcid, unsigned char dcid_len,
    unsigned char pktno_class, xqc_packet_number_t packet_number)
{
    unsigned char spin_bit = 0x01;
    unsigned char reserved_bits = 0x00;

    unsigned need = xqc_short_packet_header_size(dcid_len, pktno_class);

    unsigned char *dst_buf = packet_out->po_buf;
    size_t dst_buf_size = xqc_get_po_remained_size(packet_out);

    packet_out->po_pkt.pkt_type = XQC_PTYPE_SHORT_HEADER;

    if (need > dst_buf_size) {
        return -XQC_ENOBUF;
    }

    dst_buf[0] = 0x40 | spin_bit << 5 | reserved_bits << 3 | pktno_class;
    dst_buf++;

    if (dcid_len) {
        memcpy(dst_buf, dcid, dcid_len);
    }
    dst_buf += dcid_len;

    dst_buf += XQC_PKTLEN_BYTES;

    packet_out->po_ppktno = dst_buf;
    dst_buf += xqc_write_packet_number(dst_buf, packet_number, pktno_class);
    packet_out->po_payload = dst_buf;

    return need;
}

xqc_int_t
xqc_packet_parse_short_header(xqc_connection_t *c, xqc_packet_in_t *packet_in)
{
    unsigned char *pos = packet_in->pos;
    unsigned char *end = packet_in->last;
    xqc_packet_t *packet = &packet_in->pi_pkt;
    uint8_t cid_len = c->scid_set.user_scid.cid_len;
    int size = 0;
    uint64_t length = 0;

    packet_in->pi_pkt.pkt_type = XQC_PTYPE_SHORT_HEADER;

    if (XQC_BUFF_LEFT_SIZE(pos, packet_in->last) < 1 + cid_len) {
        xqc_log(c->log, XQC_LOG_ERROR, "|cid len error|cid_len:%d|size:%d",
                1 + cid_len, XQC_BUFF_LEFT_SIZE(pos, packet_in->last));
        return -XQC_EILLPKT;
    }

    /* check fixed bit(0x40) = 1 */
    if ((pos[0] & 0x40) == 0) {
        xqc_log(c->log, XQC_LOG_ERROR, "|parse short header: fixed bit err|pos[0]:%d", (uint32_t)pos[0]);
        return -XQC_EILLPKT;
    }

    unsigned pktno_bytes = XQC_PACKET_HEADER_PKTNO_BYTES(pos);
    pos += 1;

    /* check dcid */
    xqc_cid_set(&(packet->pkt_dcid), pos, cid_len);
    pos += cid_len;
    if (xqc_conn_check_dcid(c, &(packet->pkt_dcid)) != XQC_OK) {
        /* log & ignore, the pkt might be corrupted */
        xqc_log(c->log, XQC_LOG_WARN, "|parse short header|invalid destination cid, pkt dcid: %s, conn scid: %s|",
                xqc_dcid_str(c->engine, &packet->pkt_dcid), xqc_scid_str(c->engine, &c->scid_set.user_scid));
        return -XQC_EILLPKT;
    }

    /* Length(i) */
    size = xqc_vint_read(pos, end, &length);
    if (size < 0
        || XQC_BUFF_LEFT_SIZE(pos, end) < size + length)
    {
        xqc_log(c->log, XQC_LOG_ERROR, "|length err|%ui|", length);
        return -XQC_EILLPKT;
    }
    pos += size;

    packet_in->last = pos + length;
    packet_in->pi_pkt.length = length;
    if (packet_in->last > end) {
        xqc_log(c->log, XQC_LOG_ERROR, "|illegal pkt with wrong length");
        return -XQC_EILLPKT;
    }

    /* packet number */
    packet_in->pi_pkt.pkt_num_offset = pos - packet_in->buf;
    if (packet_in->pi_pkt.length < pktno_bytes) {
        xqc_log(c->log, XQC_LOG_ERROR, "|illegal pkt with small length");
        return -XQC_EILLPKT;
    }
    xqc_packet_decode_packet_number(c, packet_in);
    pos += pktno_bytes;

    /* update pos */
    packet_in->pos = pos;

    if (c->conn_type == XQC_CONN_TYPE_CLIENT) {
        c->discard_vn_flag = 1;
    }

    return XQC_OK;
}

/*
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+
|1|1|T T|X X X X|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Version (32)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| DCID Len (8)  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|               Destination Connection ID (0..160)            ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| SCID Len (8)  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                 Source Connection ID (0..160)               ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
                     Long Header Packet Format
*/

ssize_t
xqc_gen_long_packet_header (xqc_packet_out_t *packet_out,
    const unsigned char *dcid, unsigned char dcid_len,
    const unsigned char *scid, unsigned char scid_len,
    xqc_proto_version_t ver,
    unsigned char pktno_class)
{
    unsigned char *dst_buf = packet_out->po_buf;
    size_t dst_buf_size = xqc_get_po_remained_size(packet_out);

    xqc_pkt_type_t type = packet_out->po_pkt.pkt_type;
    xqc_packet_number_t packet_number = packet_out->po_pkt.pkt_num;

    unsigned need = xqc_long_packet_header_size(dcid_len, scid_len, pktno_class, type);

    if (!xqc_check_proto_version_valid(ver)) {
        return -XQC_EPROTO;
    }

    if (need > dst_buf_size) {
        return -XQC_ENOBUF;
    }

    if (scid_len < 3) {
        return -XQC_EILLPKT;
    }

    unsigned char first_byte = 0xC0;
    first_byte |= type << 4;
    first_byte |= pktno_class;
    *dst_buf++ = first_byte;

    memcpy(dst_buf, xqc_proto_version_field[ver], XQC_PROTO_VERSION_LEN);
    dst_buf += XQC_PROTO_VERSION_LEN;

    *dst_buf = dcid_len;
    dst_buf++;
    memcpy(dst_buf, dcid, dcid_len);
    dst_buf += dcid_len;

    *dst_buf = scid_len;
    dst_buf++;
    memcpy(dst_buf, scid, scid_len);
    dst_buf += scid_len;

    dst_buf += XQC_PKTLEN_BYTES;

    packet_out->po_ppktno = dst_buf;
    dst_buf += xqc_write_packet_number(dst_buf, packet_number, pktno_class);
    packet_out->po_payload = dst_buf;

    return need;
}

/*
+-+-+-+-+-+-+-+-+
|1|1| 0 |R R|P P|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Version (32)                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| DCID Len (8)  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|               Destination Connection ID (0..160)            ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| SCID Len (8)  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                 Source Connection ID (0..160)               ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           Length (i)                        ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Packet Number (8/16/24/32)               ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          Payload (*)                        ...
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

                      Figure 12: Initial Packet
*/
static xqc_int_t
xqc_packet_parse_initial(xqc_connection_t *c, xqc_packet_in_t *packet_in)
{
    unsigned char *pos = packet_in->pos;
    unsigned char *end = packet_in->last;
    int size = 0;
    uint64_t length = 0;

    unsigned pktno_bytes = XQC_PACKET_HEADER_PKTNO_BYTES(packet_in->buf);

    packet_in->pi_pkt.pkt_type = XQC_PTYPE_INIT;

    /* Length(i) */
    size = xqc_vint_read(pos, end, &length);
    if (size < 0
        || XQC_BUFF_LEFT_SIZE(pos, end) < size + length)
    {
        xqc_log(c->log, XQC_LOG_ERROR, "|length err|%ui|", length);
        return -XQC_EILLPKT;
    }
    pos += size;

    packet_in->last = pos + length;
    packet_in->pi_pkt.length = length;
    if (packet_in->last > end) {
        xqc_log(c->log, XQC_LOG_ERROR, "|illegal pkt with wrong length");
        return -XQC_EILLPKT;
    }

    /* packet number */
    packet_in->pi_pkt.pkt_num_offset = pos - packet_in->buf;
    if (packet_in->pi_pkt.length < pktno_bytes) {
        xqc_log(c->log, XQC_LOG_ERROR, "|illegal pkt with small length");
        return -XQC_EILLPKT;
    }
    xqc_packet_decode_packet_number(c, packet_in);
    pos += pktno_bytes;

    /* update pos */
    packet_in->pos = pos;

    return XQC_OK;
}

/*
Version Negotiation Packet {
    Header Form (1) = 1,
    Unused (7),
    Version (32) = 0,
    Destination Connection ID Length (8),
    Destination Connection ID (0..2040),
    Source Connection ID Length (8),
    Source Connection ID (0..2040),
    Supported Version (32) ...,
}
*/
static xqc_int_t
xqc_packet_parse_version_negotiation(xqc_connection_t *c, xqc_packet_in_t *packet_in)
{
    /* check original DCID */
    if (xqc_cid_is_equal(&c->original_dcid, &packet_in->pi_pkt.pkt_scid) != XQC_OK) {
        xqc_log(c->log, XQC_LOG_ERROR, "|version negotiation pkt SCID error|original_dcid:%s|scid:%s|",
                xqc_dcid_str(c->engine, &c->original_dcid), xqc_scid_str(c->engine, &packet_in->pi_pkt.pkt_scid));
        return -XQC_EILLPKT;
    }

    unsigned char *pos = packet_in->pos;
    unsigned char *end = packet_in->last;
    packet_in->pi_pkt.pkt_type = XQC_PTYPE_VERSION_NEGOTIATION;

    /* at least one version is carried in the VN packet */
    if (XQC_BUFF_LEFT_SIZE(pos, end) < XQC_PKTVER_BYTES) {
        return -XQC_EILLPKT;
    }

    /* check available states */
    if (c->conn_state != XQC_CONN_STATE_CLIENT_HANDSHAKE) {
        /* drop packet */
        xqc_log(c->log, XQC_LOG_WARN, "|packet_parse_version_negotiation|invalid state|%d|", c->conn_state);
        return -XQC_ESTATE;
    }

    /* check conn type, only client can receive a VN packet */
    if (c->conn_type != XQC_CONN_TYPE_CLIENT) {
        xqc_log(c->log, XQC_LOG_WARN, "|packet_parse_version_negotiation|invalid conn_type|%d|", c->conn_type);
        return -XQC_EPROTO;
    }

    /* check discard vn flag */
    if (c->discard_vn_flag != 0) {
        packet_in->pos = packet_in->last;
        return XQC_OK;
    }

    /* get Supported Version list */
    uint32_t supported_version_list[256];
    uint32_t supported_version_count = 0;
    while (XQC_BUFF_LEFT_SIZE(pos, end) >= XQC_PKTVER_BYTES) {
        uint32_t version = ntohl(*(uint32_t*)pos);
        if (version) {
            if (xqc_uint32_list_find(supported_version_list, supported_version_count, version) == -1) {
                if (supported_version_count < sizeof(supported_version_list) / sizeof(*supported_version_list)) {
                    supported_version_list[supported_version_count++] = version;
                }

            } else {
                xqc_log(c->log, XQC_LOG_WARN, "|packet_parse_version_negotiation|dup version|%ud|", version);
            }
        }
        pos += XQC_PKTVER_BYTES;
    }
    packet_in->pos = packet_in->last;

    /* VN packet returns the same version, nothing to be changed */
    if (xqc_uint32_list_find(supported_version_list, supported_version_count, c->version) != -1) {
        return XQC_OK;
    }

    /* chose a version both client and server support */
    uint32_t *config_version_list = c->engine->config->support_version_list;
    uint32_t config_version_count = c->engine->config->support_version_count;
    uint32_t version_chosen = 0;
    for (uint32_t i = 0; i < supported_version_count; ++i) {
        if (xqc_uint32_list_find(config_version_list, config_version_count, supported_version_list[i]) != -1) {
            version_chosen = supported_version_list[i];
            xqc_log(c->log, XQC_LOG_INFO, "|version negotiation|version:%ui|", version_chosen);
            break;
        }
    }
    xqc_log_event(c->log, TRA_VERSION_INFORMATION, config_version_count, config_version_list, supported_version_count,
                  supported_version_list, version_chosen);

    /* can't chose a version, abort the connection attempt */
    if (version_chosen == 0) {
        xqc_log(c->log, XQC_LOG_ERROR, "|can't negotiate a version|");
        return -XQC_ESYS;
    }

    /* translate version to enum, and set to the connection */
    for (uint32_t i = XQC_IDRAFT_INIT_VER + 1; i < XQC_IDRAFT_VER_NEGOTIATION; i++) {
        if (xqc_proto_version_value[i] == version_chosen) {
            c->version = i;
            break;
        }
    }

    /* TODO: connect the server with the new version, which is not defined by protocol */

    xqc_log(c->log, XQC_LOG_INFO, "|parse version negotiation packet suc|");

    /* set the discard vn flag to avoid a second negotiation */
    c->discard_vn_flag = 1;

    return XQC_OK;
}

xqc_int_t
xqc_packet_parse_long_header(xqc_connection_t *c, xqc_packet_in_t *packet_in)
{
    unsigned char *pos = packet_in->pos;
    unsigned char *end = packet_in->last;
    xqc_packet_t  *packet = &packet_in->pi_pkt;
    xqc_int_t ret = XQC_ERROR;

    if (XQC_BUFF_LEFT_SIZE(pos, end) < 1 + XQC_PKTVER_BYTES + 2) {
        return -XQC_EILLPKT;
    }

    /* get fixed_bit and packet type */
    uint8_t fixed_bit = pos[0] & 0x40;
    xqc_uint_t type = (pos[0] & 0x30) >> 4;
    pos++;

    /* version check */
    uint32_t version = xqc_parse_uint32(pos);
    if (version == 0) {
        /* version negotiation */
        if (c->conn_type == XQC_CONN_TYPE_SERVER) {
            return -XQC_EILLPKT;
        }
        type = XQC_PTYPE_VERSION_NEGOTIATION;
    }
    pos += XQC_PKTVER_BYTES;

    /* check fixed_bit */
    if (type != XQC_PTYPE_VERSION_NEGOTIATION && fixed_bit == 0) {
        return -XQC_EILLPKT;
    }

    /* get dcid */
    xqc_cid_t *dcid = &packet->pkt_dcid;
    dcid->cid_len = (uint8_t)(*pos);
    pos += 1;
    if ((XQC_BUFF_LEFT_SIZE(pos, end) < dcid->cid_len + 1)
        || (dcid->cid_len > XQC_MAX_CID_LEN))
    {
        xqc_log(c->log, XQC_LOG_ERROR, "|long header dcid len err|size:%d|cid_len:%d|",
                XQC_BUFF_LEFT_SIZE(pos, end), dcid->cid_len + 1);
        return -XQC_EILLPKT;
    }
    dcid->path_id = XQC_INITIAL_PATH_ID;
    xqc_memcpy(dcid->cid_buf, pos, dcid->cid_len);
    pos += dcid->cid_len;

    /* get scid */
    xqc_cid_t *scid = &packet->pkt_scid;
    scid->cid_len = (uint8_t)(*pos);
    pos += 1;
    if ((XQC_BUFF_LEFT_SIZE(pos, end) < scid->cid_len)
        || (scid->cid_len > XQC_MAX_CID_LEN))
    {
        xqc_log(c->log, XQC_LOG_ERROR, "|long header scid len err|size:%d|cid_len:%d|",
                XQC_BUFF_LEFT_SIZE(pos, end), scid->cid_len);
        return -XQC_EILLPKT;
    }
    scid->path_id = XQC_INITIAL_PATH_ID;
    xqc_memcpy(scid->cid_buf, pos, scid->cid_len);
    pos += scid->cid_len;

    /* update pos */
    packet_in->pos = pos;

    if (xqc_conn_is_dcid_done(c))
    {
        /* check cid */
        if (xqc_cid_set_search_cid(&c->scid_set, &(packet->pkt_dcid)) == NULL
            || xqc_cid_set_search_cid(&c->dcid_set, &(packet->pkt_scid)) == NULL)
        {
            /* log & ignore packet */
            xqc_log(c->log, XQC_LOG_ERROR, "|invalid dcid or scid|");
            return -XQC_EILLPKT;
        }
    }

    /* check protocol version */
    if (xqc_conn_version_check(c, version) != XQC_OK) {
        xqc_log(c->log, XQC_LOG_INFO, "|version not supported|v:%ui|", version);
        if (c->conn_type == XQC_CONN_TYPE_SERVER) { /* version negotiation only send by server */
            c->conn_flag |= XQC_CONN_FLAG_VERSION_NEGOTIATION;
        }
        return -XQC_EVERSION;
    }

    switch (type) {
    case XQC_PTYPE_INIT:
        ret = xqc_packet_parse_initial(c, packet_in);
        break;
    case XQC_PTYPE_VERSION_NEGOTIATION:
        ret = xqc_packet_parse_version_negotiation(c, packet_in);
        break;
    default:
        xqc_log(c->log, XQC_LOG_ERROR, "|invalid packet type|%ui|", type);
        ret = -XQC_EILLPKT;
        break;
    }

    if (ret == XQC_OK && c->conn_type == XQC_CONN_TYPE_CLIENT) {
        c->discard_vn_flag = 1;
    }

    return ret;
}
