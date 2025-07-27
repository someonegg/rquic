/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_PACKET_H_INCLUDED_
#define _RQC_PACKET_H_INCLUDED_

#include <rquic/rquic_typedef.h>
#include "src/common/rqc_list.h"

#define RQC_QUIC_MIN_MSS                    1200
/* 1500 - 40 (IPv6) - 8 (UDP) - 16 (ACK) */
#define RQC_QUIC_MAX_MSS                    1436
#define RQC_ACK_SPACE                       16
#define RQC_MSS                             (RQC_QUIC_MAX_MSS + RQC_ACK_SPACE)

#define RQC_PACKET_INITIAL_MIN_LENGTH           RQC_QUIC_MIN_MSS

#define RQC_PACKET_IS_LONG_HEADER(buf)          ((buf[0] & 0x80) == 0x80)
#define RQC_PACKET_IS_SHORT_HEADER(buf)         ((buf[0] & 0xC0) == 0x40)
#define RQC_PACKET_LONG_HEADER_GET_TYPE(buf)    ((buf[0] & 0x30) >> 4)
#define RQC_PACKET_HEADER_PKTNO_BYTES(buf)      ((buf[0] & 0x03) + 1)

typedef enum rqc_pkt_type {
    RQC_PTYPE_INIT  = 0,
    RQC_PTYPE_RSV1  = 1,
    RQC_PTYPE_RSV2  = 2,
    RQC_PTYPE_RSV3  = 3,
    RQC_PTYPE_SHORT_HEADER,
    RQC_PTYPE_VERSION_NEGOTIATION,
    RQC_PTYPE_NUM,
} rqc_pkt_type_t;

struct rqc_packet_s {
    rqc_packet_number_t     pkt_num;
    rqc_pkt_type_t          pkt_type;
    rqc_cid_t               pkt_dcid;
    rqc_cid_t               pkt_scid;

    /*
     * length is the sum of pkt_numlen and the length of QUIC packet payload.
     */
    uint64_t                length;
    size_t                  pkt_num_offset;

};

#define rqc_parse_uint16(p) ((p)[0] << 8 | (p)[1])
#define rqc_parse_uint32(p) ((p)[0] << 24 | (p)[1] << 16 | (p)[2] << 8 | (p)[3])

/* check if the packet has packet number */
static inline uint8_t
rqc_has_packet_number(rqc_packet_t *pkt)
{
    /* VERSION_NEGOTIATION packet don't have packet number */
    if (RQC_UNLIKELY(RQC_PTYPE_VERSION_NEGOTIATION == pkt->pkt_type))
    {
        return RQC_FALSE;
    }
    return RQC_TRUE;
}

const char *rqc_pkt_type_2_str(rqc_pkt_type_t pkt_type);

rqc_pkt_type_t rqc_state_to_pkt_type(rqc_connection_t *conn);

/**
 * process a single QUIC packet from packet_in
 */
rqc_int_t rqc_packet_process_single(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

#endif /* _RQC_PACKET_H_INCLUDED_ */
