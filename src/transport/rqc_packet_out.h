/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_PACKET_OUT_H_INCLUDED_
#define _RQC_PACKET_OUT_H_INCLUDED_

#include <rquic/rquic_typedef.h>
#include "src/transport/rqc_packet.h"
#include "src/transport/rqc_frame.h"

/*
 * https://datatracker.ietf.org/doc/html/rfc9000#section-14.2
 * In the absence of these mechanisms, QUIC endpoints SHOULD NOT send
 * datagrams larger than the smallest allowed maximum datagram size.
 */
#define RQC_MAX_PACKET_OUT_SIZE  RQC_QUIC_MAX_MSS
#define RQC_PACKET_OUT_SIZE      RQC_QUIC_MIN_MSS
#define RQC_PACKET_OUT_EXT_SPACE (RQC_ACK_SPACE)
#define RQC_PACKET_OUT_BUF_CAP   (RQC_MAX_PACKET_OUT_SIZE + RQC_PACKET_OUT_EXT_SPACE)

#define RQC_MAX_STREAM_FRAME_IN_PO  3

typedef enum {
    RQC_POF_IN_FLIGHT           = 1 << 0,
    RQC_POF_LOST                = 1 << 1,
    RQC_POF_DCID_NOT_DONE       = 1 << 2,
    RQC_POF_TLP                 = 1 << 3,
    RQC_POF_STREAM_UNACK        = 1 << 4,
    RQC_POF_RETRANSED           = 1 << 5,
    RQC_POF_NOTIFY              = 1 << 6,  /* need to notify user when a packet is acked, lost, etc. */
    RQC_POF_RESEND              = 1 << 7,
    RQC_POF_IN_PATH_BUF_LIST    = 1 << 8, /* FIXED: reset when copy */
    RQC_POF_IN_UNACK_LIST       = 1 << 9, /* FIXED: reset when copy */
    RQC_POF_SPURIOUS_LOSS       = 1 << 10,
    RQC_POF_STREAM_NO_LEN       = 1 << 11,  /* for stream without LEN bit, shouldn't attach different frame to it */
} rqc_packet_out_flag_t;

typedef struct rqc_po_stream_frame_s {
    rqc_stream_id_t         ps_stream_id;
    uint64_t                ps_offset;
    unsigned int            ps_length;
    unsigned int            ps_type_offset;
    unsigned int            ps_length_offset;
    unsigned char           ps_is_used;
    unsigned char           ps_has_fin;     /* whether fin flag from stream frame is set  */
    unsigned char           ps_is_reset;    /* whether frame is RESET_STREAM */
} rqc_po_stream_frame_t;

typedef struct rqc_packet_out_s {
    rqc_packet_t            po_pkt;
    rqc_list_head_t         po_list;

    /* pointers should carefully assign in rqc_packet_out_copy */
    unsigned char          *po_buf;
    unsigned char          *po_ppktno;
    unsigned char          *po_payload;
    rqc_packet_out_t       *po_origin;          /* point to original packet before retransmitted */
    void                   *po_user_data;       /* used to differ inner PING and user PING */
    unsigned char          *po_padding;         /* used to reassemble packets carrying new header */

    size_t                  po_buf_cap;         /* capcacity of po_buf */
    unsigned int            po_buf_size;        /* size of po_buf can be used */
    unsigned int            po_used_size;
    unsigned int            po_cc_size;         /* TODO: check cc size != send size */
    unsigned int            po_ack_offset;
    rqc_packet_out_flag_t   po_flag;
    /* Largest Acknowledged in ACK frame, initiated to be 0 */
    rqc_packet_number_t     po_largest_ack;
    rqc_usec_t              po_sent_time;
    rqc_frame_type_bit_t    po_frame_types;

    /* the stream related to stream frame */
    rqc_po_stream_frame_t   po_stream_frames[RQC_MAX_STREAM_FRAME_IN_PO];
    unsigned int            po_stream_frames_idx;

    uint32_t                po_origin_ref_cnt;  /* reference count of original packet */
    uint32_t                po_acked;
    uint64_t                po_delivered;       /* the sum of delivered data before sending packet P */
    rqc_usec_t              po_delivered_time;  /* the time of last acked packet before sending packet P */
    rqc_usec_t              po_first_sent_time; /* the time of first sent packet during current sample period */
    rqc_bool_t              po_is_app_limited;

    /* For BBRv2 */
    /* the inflight bytes when the packet is sent (including itself) */
    uint64_t                po_tx_in_flight;
    /* how many packets have been lost when the packet is sent */
    uint32_t                po_lost;

    uint64_t                po_stream_offset;
    uint64_t                po_stream_id;

    /* ping notification */
    rqc_ping_record_t      *po_pr;

    rqc_usec_t              po_sched_cwnd_blk_ts;
    rqc_usec_t              po_send_cwnd_blk_ts;
    rqc_usec_t              po_send_pacing_blk_ts;
} rqc_packet_out_t;

void rqc_packet_out_remove_ack_frame(rqc_packet_out_t *po);

rqc_packet_out_t *rqc_packet_out_create(size_t po_buf_cap);

void rqc_packet_out_copy(rqc_packet_out_t *dst, rqc_packet_out_t *src);

rqc_packet_out_t *rqc_packet_out_get(rqc_send_queue_t *send_queue);

rqc_packet_out_t *rqc_packet_out_get_and_insert_send(rqc_send_queue_t *send_queue, enum rqc_pkt_type pkt_type);

void rqc_packet_out_destroy(rqc_packet_out_t *packet_out);

void rqc_maybe_recycle_packet_out(rqc_packet_out_t *packet_out, rqc_connection_t *conn);

rqc_packet_out_t *rqc_write_new_packet(rqc_connection_t *conn, rqc_pkt_type_t pkt_type);

rqc_packet_out_t *rqc_write_packet(rqc_connection_t *conn, rqc_pkt_type_t pkt_type, unsigned need);

rqc_packet_out_t *rqc_write_packet_for_stream(rqc_connection_t *conn, rqc_pkt_type_t pkt_type, unsigned need,
    rqc_stream_t *stream);

int rqc_write_packet_header(rqc_connection_t *conn, rqc_packet_out_t *packet_out);

rqc_int_t rqc_write_ack_to_packets(rqc_connection_t *conn);

int rqc_write_ping_to_packet(rqc_connection_t *conn,
    void *po_user_data, rqc_bool_t notify, rqc_ping_record_t *pr);

int rqc_write_conn_close_to_packet(rqc_connection_t *conn, uint64_t err_code);

int rqc_write_reset_stream_to_packet(rqc_connection_t *conn, rqc_stream_t *stream, uint64_t err_code, uint64_t final_size);

int rqc_write_stop_sending_to_packet(rqc_connection_t *conn, rqc_stream_t *stream, uint64_t err_code);

int rqc_write_data_blocked_to_packet(rqc_connection_t *conn, uint64_t data_limit);

int rqc_write_stream_data_blocked_to_packet(rqc_connection_t *conn, rqc_stream_id_t stream_id, uint64_t stream_data_limit);

int rqc_write_streams_blocked_to_packet(rqc_connection_t *conn, uint64_t stream_limit, int bidirectional);

int rqc_write_max_data_to_packet(rqc_connection_t *conn, uint64_t max_data);

int rqc_write_max_stream_data_to_packet(rqc_connection_t *conn,
rqc_stream_id_t stream_id, uint64_t max_stream_data, rqc_pkt_type_t rqc_pkt_type);

int rqc_write_max_streams_to_packet(rqc_connection_t *conn, uint64_t max_stream, int bidirectional);

int rqc_write_stream_frame_to_packet(rqc_connection_t *conn, rqc_stream_t *stream, rqc_pkt_type_t pkt_type,
    uint8_t fin, const unsigned char *payload, size_t payload_size, size_t *send_data_written);

rqc_int_t rqc_write_path_challenge_frame_to_packet(rqc_connection_t *conn, rqc_path_ctx_t *path);

rqc_int_t rqc_write_path_response_frame_to_packet(rqc_connection_t *conn, rqc_path_ctx_t *path,
    unsigned char *path_response_data);

rqc_int_t rqc_write_handshake_frame_to_packet(rqc_connection_t *conn,
    const unsigned char *alpn, size_t alpn_len, uint8_t *tp, size_t tp_len);

/**
 * @brief Get remained space size in packet out buff.
 *
 * @param conn
 * @param po
 * @return size_t
 */
size_t rqc_get_po_remained_size(rqc_packet_out_t *po);
size_t rqc_get_po_remained_size_with_ack_spc(rqc_packet_out_t *po);

#endif /* _RQC_PACKET_OUT_H_INCLUDED_ */
