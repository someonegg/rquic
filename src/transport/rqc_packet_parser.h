/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_PACKET_PARSER_H_INCLUDED_
#define _RQC_PACKET_PARSER_H_INCLUDED_

#include <rquic/rquic_typedef.h>
#include <rquic/rquic.h>
#include "src/transport/rqc_packet_in.h"
#include "src/transport/rqc_packet_out.h"

#define RQC_PKTVER_BYTES  4
#define RQC_PKTLEN_BYTES  2
#define RQC_PKTNO_CLASS   3 /* 4 byte */

unsigned rqc_short_packet_header_size(unsigned char dcid_len, unsigned char pktno_class);

unsigned rqc_long_packet_header_size(unsigned char dcid_len, unsigned char scid_len,
    unsigned char pktno_class, rqc_pkt_type_t type);

int rqc_write_packet_number(unsigned char *buf, rqc_packet_number_t packet_number, unsigned char pktno_class);

void rqc_update_packet_length(rqc_packet_out_t *packet_out);

void rqc_short_packet_update_dcid(rqc_packet_out_t *packet_out, rqc_cid_t dcid);

int rqc_gen_short_packet_header(rqc_packet_out_t *packet_out, unsigned char *dcid, unsigned char dcid_len,
    unsigned char pktno_class, rqc_packet_number_t packet_number);

rqc_int_t rqc_packet_parse_short_header(rqc_connection_t *c, rqc_packet_in_t *packet_in);

ssize_t rqc_gen_long_packet_header(rqc_packet_out_t *packet_out,
    const unsigned char *dcid, unsigned char dcid_len,
    const unsigned char *scid, unsigned char scid_len,
    rqc_proto_version_t ver,
    unsigned char pktno_class);

rqc_int_t rqc_packet_parse_long_header(rqc_connection_t *c, rqc_packet_in_t *packet_in);

#endif /* _RQC_PACKET_PARSER_H_INCLUDED_ */
