/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include <xquic/xquic.h>
#include "src/transport/xqc_packet.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_conn.h"
#include "src/common/xqc_algorithm.h"
#include "src/common/utils/vint/xqc_variable_len_int.h"
#include "src/transport/xqc_defs.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/transport/xqc_recv_record.h"
#include "src/transport/xqc_packet_parser.h"
#include "src/transport/xqc_utils.h"
#include "src/transport/xqc_engine.h"

static const char * const pkt_type_2_str[XQC_PTYPE_NUM] = {
    [XQC_PTYPE_INIT]                = "INIT",
    [XQC_PTYPE_RSV1]                = "RSV1",
    [XQC_PTYPE_RSV2]                = "RSV2",
    [XQC_PTYPE_RSV3]                = "RSV3",
    [XQC_PTYPE_SHORT_HEADER]        = "SHORT_HEADER",
    [XQC_PTYPE_VERSION_NEGOTIATION] = "VERSION_NEGOTIATION",
};

const char *
xqc_pkt_type_2_str(xqc_pkt_type_t pkt_type)
{
    return pkt_type_2_str[pkt_type];
}

xqc_pkt_type_t
xqc_state_to_pkt_type(xqc_connection_t *conn)
{
    if (xqc_conn_is_handshake_done(conn)) {
        return XQC_PTYPE_SHORT_HEADER;
    }
    return XQC_PTYPE_INIT;
}

static xqc_int_t
xqc_packet_parse_single(xqc_connection_t *c, xqc_packet_in_t *packet_in)
{
    unsigned char *pos = packet_in->pos;
    xqc_int_t ret = XQC_ERROR;

    if (XQC_BUFF_LEFT_SIZE(pos, packet_in->last) == 0) {
        xqc_log(c->log, XQC_LOG_ERROR,
                "|xqc_packet_parse_short_header error:%d|", ret);
        return -XQC_EILLPKT;
    }

    if (XQC_PACKET_IS_SHORT_HEADER(pos)) {
        if ((c->conn_type == XQC_CONN_TYPE_CLIENT && !xqc_conn_is_handshake_done(c)) ||
            (c->conn_type == XQC_CONN_TYPE_SERVER && !xqc_conn_is_handshake_sent(c))) {
            xqc_log(c->log, XQC_LOG_INFO,
                    "|1RTT packet before handshake done|");
            return -XQC_EIGNORE_PKT;
        }

        ret = xqc_packet_parse_short_header(c, packet_in);
        if (ret != XQC_OK) {
            xqc_log(c->log, XQC_LOG_ERROR,
                    "|xqc_packet_parse_short_header error:%d|", ret);
            return ret;
        }

    } else if (XQC_PACKET_IS_LONG_HEADER(pos)) {
        ret = xqc_packet_parse_long_header(c, packet_in);
        if (XQC_OK != ret) {
            xqc_log(c->log, XQC_LOG_ERROR,
                    "|xqc_packet_parse_long_header error:%d|", ret);
            return ret;
        }

    } else {
        xqc_log(c->log, XQC_LOG_INFO, "unknown packet type, first byte[%d], "
                "skip all buf, skip length: %d", pos[0], packet_in->last - packet_in->pos);
        return -XQC_EIGNORE_PKT;
    }

    return ret;
}

xqc_int_t
xqc_packet_process_single(xqc_connection_t *conn,
    xqc_packet_in_t *packet_in)
{
    xqc_int_t ret = XQC_ERROR;

    /* parse packet */
    ret = xqc_packet_parse_single(conn, packet_in);
    if (XQC_OK != ret) {
        return ret;
    }

    /* those packets with no packet number, don't need to be process or put into CC */
    if (!xqc_has_packet_number(&packet_in->pi_pkt)) {
        return XQC_OK;
    }

    /* process frames */
    ret = xqc_process_frames(conn, packet_in);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_process_frames error|%d|", ret);
        return ret;
    }

    return XQC_OK;
}
