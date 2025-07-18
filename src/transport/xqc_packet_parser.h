/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _XQC_PACKET_PARSER_H_INCLUDED_
#define _XQC_PACKET_PARSER_H_INCLUDED_

#include <xquic/xquic_typedef.h>
#include <xquic/xquic.h>
#include "src/transport/xqc_packet_in.h"
#include "src/transport/xqc_packet_out.h"

#define XQC_PKTVER_BYTES  4
#define XQC_PKTLEN_BYTES  2
#define XQC_PKTNO_CLASS   3 /* 4 byte */

unsigned xqc_short_packet_header_size(unsigned char dcid_len, unsigned char pktno_class);

unsigned xqc_long_packet_header_size(unsigned char dcid_len, unsigned char scid_len,
    unsigned char pktno_class, xqc_pkt_type_t type);

int xqc_write_packet_number(unsigned char *buf, xqc_packet_number_t packet_number, unsigned char pktno_class);

void xqc_update_packet_length(xqc_packet_out_t *packet_out);

void xqc_short_packet_update_dcid(xqc_packet_out_t *packet_out, xqc_cid_t dcid);

int xqc_gen_short_packet_header(xqc_packet_out_t *packet_out, unsigned char *dcid, unsigned char dcid_len,
    unsigned char pktno_class, xqc_packet_number_t packet_number);

xqc_int_t xqc_packet_parse_short_header(xqc_connection_t *c, xqc_packet_in_t *packet_in);

ssize_t xqc_gen_long_packet_header(xqc_packet_out_t *packet_out,
    const unsigned char *dcid, unsigned char dcid_len,
    const unsigned char *scid, unsigned char scid_len,
    xqc_proto_version_t ver,
    unsigned char pktno_class);

xqc_int_t xqc_packet_parse_long_header(xqc_connection_t *c, xqc_packet_in_t *packet_in);

#endif /* _XQC_PACKET_PARSER_H_INCLUDED_ */
