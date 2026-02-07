/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_FRAME_PARSER_H_INCLUDED_
#define _RQC_FRAME_PARSER_H_INCLUDED_

#include <rquic/rquic_typedef.h>
#include "src/transport/rqc_frame.h"
#include "src/transport/rqc_packet_in.h"
#include "src/transport/rqc_packet_out.h"
#include "src/transport/rqc_recv_record.h"

#define RQC_PATH_CHALLENGE_DATA_LEN  8

/**
 * generate stream frame
 * @param written_size output size of the payload been written
 * @return size of stream frame
 */
ssize_t rqc_gen_stream_frame(rqc_packet_out_t *packet_out,
    rqc_stream_id_t stream_id, uint64_t offset, uint8_t fin,
    const unsigned char *payload, size_t size, size_t *written_size);

rqc_int_t rqc_parse_stream_frame(rqc_packet_in_t *packet_in, rqc_connection_t *conn,
    rqc_stream_frame_t *frame, rqc_stream_id_t *stream_id);

void rqc_gen_padding_frame(rqc_connection_t *conn, rqc_packet_out_t *packet_out);

rqc_int_t rqc_parse_padding_frame(rqc_packet_in_t *packet_in, rqc_connection_t *conn);

ssize_t rqc_gen_ping_frame(rqc_packet_out_t *packet_out);

rqc_int_t rqc_parse_ping_frame(rqc_packet_in_t *packet_in, rqc_connection_t *conn);

ssize_t rqc_gen_ack_frame(rqc_connection_t *conn, rqc_packet_out_t *packet_out, rqc_usec_t now, int ack_delay_exponent,
    rqc_recv_record_t *recv_record, rqc_usec_t largest_pkt_recv_time, int *has_gap, rqc_packet_number_t *largest_ack);

rqc_int_t rqc_parse_ack_frame(rqc_packet_in_t *packet_in, rqc_connection_t *conn, rqc_ack_info_t *ack_info);

ssize_t rqc_gen_conn_close_frame(rqc_packet_out_t *packet_out, uint64_t err_code, int is_app, int frame_type);

rqc_int_t rqc_parse_conn_close_frame(rqc_packet_in_t *packet_in, uint64_t *err_code, rqc_connection_t *conn);

ssize_t rqc_gen_reset_stream_frame(rqc_packet_out_t *packet_out, rqc_stream_id_t stream_id,
    uint64_t err_code, uint64_t final_size);

rqc_int_t rqc_parse_reset_stream_frame(rqc_packet_in_t *packet_in, rqc_stream_id_t *stream_id,
    uint64_t *err_code, uint64_t *final_size, rqc_connection_t *conn);

ssize_t rqc_gen_stop_sending_frame(rqc_packet_out_t *packet_out, rqc_stream_id_t stream_id,
    uint64_t err_code);

rqc_int_t rqc_parse_stop_sending_frame(rqc_packet_in_t *packet_in, rqc_stream_id_t *stream_id,
    uint64_t *err_code, rqc_connection_t *conn);

ssize_t rqc_gen_data_blocked_frame(rqc_packet_out_t *packet_out, uint64_t data_limit);

rqc_int_t rqc_parse_data_blocked_frame(rqc_packet_in_t *packet_in, uint64_t *data_limit, rqc_connection_t *conn);

ssize_t rqc_gen_stream_data_blocked_frame(rqc_packet_out_t *packet_out, rqc_stream_id_t stream_id, uint64_t stream_data_limit);

rqc_int_t rqc_parse_stream_data_blocked_frame(rqc_packet_in_t *packet_in, rqc_stream_id_t *stream_id, uint64_t *stream_data_limit, rqc_connection_t *conn);

ssize_t rqc_gen_streams_blocked_frame(rqc_packet_out_t *packet_out, uint64_t stream_limit, int bidirectional);

rqc_int_t rqc_parse_streams_blocked_frame(rqc_packet_in_t *packet_in, uint64_t *stream_limit, int *bidirectional, rqc_connection_t *conn);

ssize_t rqc_gen_max_data_frame(rqc_packet_out_t *packet_out, uint64_t max_data);

rqc_int_t rqc_parse_max_data_frame(rqc_packet_in_t *packet_in, uint64_t *max_data, rqc_connection_t *conn);

ssize_t rqc_gen_max_stream_data_frame(rqc_packet_out_t *packet_out, rqc_stream_id_t stream_id, uint64_t max_stream_data);

rqc_int_t rqc_parse_max_stream_data_frame(rqc_packet_in_t *packet_in, rqc_stream_id_t *stream_id, uint64_t *max_stream_data, rqc_connection_t *conn);

ssize_t rqc_gen_max_streams_frame(rqc_packet_out_t *packet_out, uint64_t max_streams, int bidirectional);

rqc_int_t rqc_parse_max_streams_frame(rqc_packet_in_t *packet_in, uint64_t *max_streams, int *bidirectional, rqc_connection_t *conn);

ssize_t rqc_gen_path_challenge_frame(rqc_packet_out_t *packet_out, unsigned char *data);

rqc_int_t rqc_parse_path_challenge_frame(rqc_packet_in_t *packet_in, unsigned char *data);

ssize_t rqc_gen_path_response_frame(rqc_packet_out_t *packet_out, unsigned char *data);

rqc_int_t rqc_parse_path_response_frame(rqc_packet_in_t *packet_in, unsigned char *data);

ssize_t rqc_gen_handshake_frame(rqc_packet_out_t *packet_out, const unsigned char *alpn, size_t alpn_len,
    const uint8_t *tp, size_t tp_len, const uint8_t *proto_ext, size_t proto_ext_len);

rqc_int_t rqc_parse_handshake_frame(rqc_packet_in_t *packet_in, rqc_connection_t *conn,
    unsigned char *alpn, size_t alpn_cap, size_t *alpn_len,
    unsigned char *tp, size_t tp_cap, size_t *tp_len,
    unsigned char *proto_ext, size_t proto_ext_cap, size_t *proto_ext_len);

#endif /*_RQC_FRAME_PARSER_H_INCLUDED_*/
