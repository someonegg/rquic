/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include <rquic/rquic.h>
#include "src/transport/rqc_packet.h"
#include "src/transport/rqc_packet_out.h"
#include "src/transport/rqc_conn.h"
#include "src/common/rqc_algorithm.h"
#include "src/common/utils/vint/rqc_variable_len_int.h"
#include "src/transport/rqc_defs.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/transport/rqc_recv_record.h"
#include "src/transport/rqc_packet_parser.h"
#include "src/transport/rqc_utils.h"
#include "src/transport/rqc_engine.h"

static const char * const pkt_type_2_str[RQC_PTYPE_NUM] = {
    [RQC_PTYPE_INIT]                = "INIT",
    [RQC_PTYPE_RSV1]                = "RSV1",
    [RQC_PTYPE_RSV2]                = "RSV2",
    [RQC_PTYPE_RSV3]                = "RSV3",
    [RQC_PTYPE_SHORT_HEADER]        = "SHORT_HEADER",
    [RQC_PTYPE_VERSION_NEGOTIATION] = "VERSION_NEGOTIATION",
};

const char *
rqc_pkt_type_2_str(rqc_pkt_type_t pkt_type)
{
    return pkt_type_2_str[pkt_type];
}

rqc_pkt_type_t
rqc_state_to_pkt_type(rqc_connection_t *conn)
{
    if (rqc_conn_is_handshake_done(conn)) {
        return RQC_PTYPE_SHORT_HEADER;
    }
    return RQC_PTYPE_INIT;
}

static rqc_int_t
rqc_packet_parse_single(rqc_connection_t *c, rqc_packet_in_t *packet_in)
{
    unsigned char *pos = packet_in->pos;
    rqc_int_t ret = RQC_ERROR;

    if (RQC_BUFF_LEFT_SIZE(pos, packet_in->last) == 0) {
        rqc_log(c->log, RQC_LOG_ERROR,
                "|rqc_packet_parse_short_header error:%d|", ret);
        return -RQC_EILLPKT;
    }

    if (RQC_PACKET_IS_SHORT_HEADER(pos)) {
        if ((c->conn_type == RQC_CONN_TYPE_CLIENT && !rqc_conn_is_handshake_done(c)) ||
            (c->conn_type == RQC_CONN_TYPE_SERVER && !rqc_conn_is_handshake_sent(c))) {
            rqc_log(c->log, RQC_LOG_INFO,
                    "|1RTT packet before handshake done|");
            return -RQC_EIGNORE_PKT;
        }

        ret = rqc_packet_parse_short_header(c, packet_in);
        if (ret != RQC_OK) {
            rqc_log(c->log, RQC_LOG_ERROR,
                    "|rqc_packet_parse_short_header error:%d|", ret);
            return ret;
        }

    } else if (RQC_PACKET_IS_LONG_HEADER(pos)) {
        ret = rqc_packet_parse_long_header(c, packet_in);
        if (RQC_OK != ret) {
            rqc_log(c->log, RQC_LOG_ERROR,
                    "|rqc_packet_parse_long_header error:%d|", ret);
            return ret;
        }

    } else {
        rqc_log(c->log, RQC_LOG_INFO, "unknown packet type, first byte[%d], "
                "skip all buf, skip length: %d", pos[0], packet_in->last - packet_in->pos);
        return -RQC_EIGNORE_PKT;
    }

    return ret;
}

rqc_int_t
rqc_packet_process_single(rqc_connection_t *conn,
    rqc_packet_in_t *packet_in)
{
    rqc_int_t ret = RQC_ERROR;

    /* parse packet */
    ret = rqc_packet_parse_single(conn, packet_in);
    if (RQC_OK != ret) {
        return ret;
    }

    /* those packets with no packet number, don't need to be process or put into CC */
    if (!rqc_has_packet_number(&packet_in->pi_pkt)) {
        return RQC_OK;
    }

    /* process frames */
    ret = rqc_process_frames(conn, packet_in);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_process_frames error|%d|", ret);
        return ret;
    }

    return RQC_OK;
}
