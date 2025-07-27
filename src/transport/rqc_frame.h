/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_FRAME_H_INCLUDED_
#define _RQC_FRAME_H_INCLUDED_

#include <rquic/rquic_typedef.h>

typedef enum {
    RQC_FRAME_PADDING,
    RQC_FRAME_PING,
    RQC_FRAME_ACK,
    RQC_FRAME_HANDSHAKE,
    RQC_FRAME_RESET_STREAM,
    RQC_FRAME_STOP_SENDING,
    RQC_FRAME_STREAM,
    RQC_FRAME_MAX_DATA,
    RQC_FRAME_MAX_STREAM_DATA,
    RQC_FRAME_MAX_STREAMS,
    RQC_FRAME_DATA_BLOCKED,
    RQC_FRAME_STREAM_DATA_BLOCKED,
    RQC_FRAME_STREAMS_BLOCKED,
    RQC_FRAME_PATH_CHALLENGE,
    RQC_FRAME_PATH_RESPONSE,
    RQC_FRAME_CONNECTION_CLOSE,
    RQC_FRAME_Extension,
    RQC_FRAME_NUM,
} rqc_frame_type_t;

typedef enum {
    RQC_FRAME_BIT_PADDING               = 1ULL << RQC_FRAME_PADDING,
    RQC_FRAME_BIT_PING                  = 1ULL << RQC_FRAME_PING,
    RQC_FRAME_BIT_ACK                   = 1ULL << RQC_FRAME_ACK,
    RQC_FRAME_BIT_HANDSHAKE             = 1ULL << RQC_FRAME_HANDSHAKE,
    RQC_FRAME_BIT_RESET_STREAM          = 1ULL << RQC_FRAME_RESET_STREAM,
    RQC_FRAME_BIT_STOP_SENDING          = 1ULL << RQC_FRAME_STOP_SENDING,
    RQC_FRAME_BIT_STREAM                = 1ULL << RQC_FRAME_STREAM,
    RQC_FRAME_BIT_MAX_DATA              = 1ULL << RQC_FRAME_MAX_DATA,
    RQC_FRAME_BIT_MAX_STREAM_DATA       = 1ULL << RQC_FRAME_MAX_STREAM_DATA,
    RQC_FRAME_BIT_MAX_STREAMS           = 1ULL << RQC_FRAME_MAX_STREAMS,
    RQC_FRAME_BIT_DATA_BLOCKED          = 1ULL << RQC_FRAME_DATA_BLOCKED,
    RQC_FRAME_BIT_STREAM_DATA_BLOCKED   = 1ULL << RQC_FRAME_STREAM_DATA_BLOCKED,
    RQC_FRAME_BIT_STREAMS_BLOCKED       = 1ULL << RQC_FRAME_STREAMS_BLOCKED,
    RQC_FRAME_BIT_PATH_CHALLENGE        = 1ULL << RQC_FRAME_PATH_CHALLENGE,
    RQC_FRAME_BIT_PATH_RESPONSE         = 1ULL << RQC_FRAME_PATH_RESPONSE,
    RQC_FRAME_BIT_CONNECTION_CLOSE      = 1ULL << RQC_FRAME_CONNECTION_CLOSE,
    RQC_FRAME_BIT_Extension             = 1ULL << RQC_FRAME_Extension,
    RQC_FRAME_BIT_NUM                   = 1ULL << RQC_FRAME_NUM,
} rqc_frame_type_bit_t;

/*
 * Ack-eliciting Packet:  A QUIC packet that contains frames other than
      ACK, PADDING, and CONNECTION_CLOSE.  These cause a recipient to
      send an acknowledgment

      Connection close signals, including packets that contain
      CONNECTION_CLOSE frames, are not sent again when packet loss is
      detected, but as described in Section 10.
 */
#define RQC_IS_ACK_ELICITING(types) ((types) & ~(RQC_FRAME_BIT_ACK | RQC_FRAME_BIT_PADDING | RQC_FRAME_BIT_CONNECTION_CLOSE))

/*
 * https://tools.ietf.org/html/draft-ietf-quic-recovery-24#section-3
 * Packets containing frames besides ACK or CONNECTION_CLOSE frames
      count toward congestion control limits and are considered in-
      flight.

   PADDING frames cause packets to contribute toward bytes in flight
      without directly causing an acknowledgment to be sent.
 */
#define RQC_CAN_IN_FLIGHT(types) ((types) & ~(RQC_FRAME_BIT_ACK | RQC_FRAME_BIT_CONNECTION_CLOSE))

/*
 * PING and PADDING frames contain no information, so lost PING or
 *     PADDING frames do not require repair
 */
#define RQC_NEED_REPAIR(types) ((types) & ~(RQC_FRAME_BIT_ACK| RQC_FRAME_BIT_PADDING | RQC_FRAME_BIT_PING | RQC_FRAME_BIT_CONNECTION_CLOSE))

const char *rqc_frame_type_2_str(rqc_engine_t *engine, rqc_frame_type_bit_t type_bit);

unsigned int rqc_stream_frame_header_size(rqc_stream_id_t stream_id, uint64_t offset, size_t length);

rqc_int_t rqc_insert_stream_frame(rqc_connection_t *conn, rqc_stream_t *stream, rqc_stream_frame_t *new_frame);

rqc_int_t rqc_process_frames(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_padding_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_stream_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_ack_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_ping_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_conn_close_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_reset_stream_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_stop_sending_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_data_blocked_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_stream_data_blocked_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_streams_blocked_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_max_data_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_max_stream_data_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_max_streams_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_path_challenge_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_path_response_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

rqc_int_t rqc_process_handshake_frame(rqc_connection_t *conn, rqc_packet_in_t *packet_in);

#endif /* _RQC_FRAME_H_INCLUDED_ */
