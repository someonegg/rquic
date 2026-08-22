/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_STREAM_H_INCLUDED_
#define _RQC_STREAM_H_INCLUDED_

#include <rquic/rquic_typedef.h>
#include <rquic/rquic.h>
#include "src/common/rqc_list.h"
#include "src/transport/rqc_packet.h"

#define RQC_UNDEFINE_STREAM_ID RQC_MAX_UINT64_VALUE
#define RQC_STREAM_ATOMIC_FIXED_CAPACITY 4096

#define RQC_STREAM_CLOSE_MSG(stream, msg) do {      \
    if ((stream)->stream_close_msg == NULL) {       \
        (stream)->stream_close_msg = (msg);         \
    }                                               \
} while(0)                                          \

typedef enum {
    RQC_STREAM_FLAG_READY_TO_WRITE  = 1 << 0,
    RQC_STREAM_FLAG_READY_TO_READ   = 1 << 1,
    RQC_STREAM_FLAG_DATA_BLOCKED    = 1 << 2,
    RQC_STREAM_FLAG_NEED_CLOSE      = 1 << 3,
    RQC_STREAM_FLAG_FIN_WRITE       = 1 << 4,
    RQC_STREAM_FLAG_CLOSED          = 1 << 5,
    RQC_STREAM_FLAG_UNEXPECTED      = 1 << 6,
    RQC_STREAM_FLAG_DISCARDED       = 1 << 7,   /* stream create_notify with error, all stream data will be discarded */
} rqc_stream_flag_t;

typedef struct {
    uint64_t                fc_max_stream_data_can_send;
    uint64_t                fc_max_stream_data_can_recv;
    uint64_t                fc_stream_recv_window_size;
    rqc_usec_t              fc_last_window_update_time;
} rqc_stream_flow_ctl_t;

/* Put one STREAM frame */
typedef struct rqc_stream_frame_s {
    rqc_list_head_t         sf_list;
    unsigned char          *data;
    unsigned                data_length;
    uint64_t                data_offset;
    uint64_t                next_read_offset;   /* next offset in frame */
    unsigned char           fin;
} rqc_stream_frame_t;

/* Put all received STREAM data here */
typedef struct rqc_stream_data_in_s {
    /* A list of STREAM frame, order by offset */
    rqc_list_head_t         frames_tailq;       /* rqc_stream_frame_t */
    uint64_t                merged_offset_end;  /* [0,end) */
    uint64_t                next_read_offset;   /* next offset in stream */
    uint64_t                stream_length;
    rqc_bool_t              stream_determined;
} rqc_stream_data_in_t;

typedef struct rqc_stream_atomic_ctx_s {
    uint8_t                fixed_buf[RQC_STREAM_ATOMIC_FIXED_CAPACITY];
    uint8_t               *dynamic_buf;
    size_t                 pending_len;
    size_t                 pending_offset;
    uint8_t                fin;
    uint8_t                flush;
} rqc_stream_atomic_ctx_t;

struct rqc_stream_s {
    rqc_connection_t       *stream_conn;
    rqc_stream_id_t         stream_id;
    rqc_stream_type_t       stream_type;
    void                   *user_data;
    rqc_stream_callbacks_t *stream_if;

    rqc_stream_flow_ctl_t   stream_flow_ctl;
    rqc_list_head_t         write_stream_list,
                            read_stream_list,
                            closing_stream_list,
                            all_stream_list;

    uint64_t                stream_send_offset;
    uint64_t                stream_max_recv_offset;
    rqc_stream_flag_t       stream_flag;
    rqc_stream_data_in_t    stream_data_in;
    unsigned                stream_unacked_pkt;
    int64_t                 stream_refcnt;
    rqc_send_stream_state_t stream_state_send;
    rqc_recv_stream_state_t stream_state_recv;
    rqc_usec_t              stream_close_time;
    uint64_t                stream_err;
    const char             *stream_close_msg;

    struct {
        rqc_usec_t          create_time;
        rqc_usec_t          peer_fin_rcv_time;      /* quic stack rcv fin */
        rqc_usec_t          peer_fin_read_time;     /* app read fin */
        rqc_usec_t          local_fin_write_time;   /* app send fin */
        rqc_usec_t          local_fin_snd_time;     /* socket send fin */
        rqc_usec_t          local_fst_fin_snd_time;
        rqc_usec_t          first_write_time;       /* app send data */
        rqc_usec_t          first_snd_time;         /* socket send data */
        rqc_usec_t          first_fin_ack_time;
        rqc_usec_t          all_data_acked_time;
        rqc_usec_t          close_time;             /* stream close time: fin/reset read */
        rqc_usec_t          app_reset_time;         /* app snd reset */
        rqc_usec_t          local_reset_time;       /* socket snd reset */
        rqc_usec_t          peer_reset_time;        /* quic stack rcv reset */
        rqc_usec_t          first_rcv_time;         /* recv the first udp packet */
        uint32_t            sched_cwnd_blk_cnt;
        uint32_t            send_cwnd_blk_cnt;
        uint32_t            send_pacing_blk_cnt;
        rqc_usec_t          sched_cwnd_blk_duration;
        rqc_usec_t          send_cwnd_blk_duration;
        rqc_usec_t          send_pacing_blk_duration;
        uint32_t            retrans_pkt_cnt;
        uint32_t            sent_pkt_cnt;
        uint8_t             max_pto_backoff;
        rqc_usec_t          final_packet_time;      /* final arrived packets of current stream */
        rqc_usec_t          stream_recv_time;       /* stream received time */
    } stream_stats;

    uint64_t                recv_rate_bytes_per_sec;

    rqc_stream_priority_t   stream_priority;
    rqc_stream_atomic_ctx_t *atomic_ctx;
};

static inline rqc_stream_type_t
rqc_get_stream_type(rqc_stream_id_t stream_id)
{
    return stream_id & 0x03;
}

static inline rqc_int_t
rqc_stream_is_bidi(rqc_stream_id_t stream_id)
{
    return stream_id == 0x00 || !(stream_id & 0x02);
}

static inline rqc_int_t
rqc_stream_is_uni(rqc_stream_id_t stream_id)
{
    return stream_id & 0x02;
}

rqc_stream_t *rqc_create_stream_with_conn (rqc_connection_t *conn, rqc_stream_id_t stream_id,
    rqc_stream_type_t stream_type, rqc_stream_settings_t *settings, void *user_data);

void rqc_destroy_stream(rqc_stream_t *stream);

void rqc_process_write_streams(rqc_connection_t *conn);

void rqc_process_read_streams(rqc_connection_t *conn);

void rqc_stream_ready_to_write(rqc_stream_t *stream);

void rqc_stream_shutdown_write(rqc_stream_t *stream);

void rqc_stream_ready_to_read(rqc_stream_t *stream);

void rqc_stream_shutdown_read(rqc_stream_t *stream);

rqc_bool_t rqc_stream_is_terminal_state(rqc_stream_t *stream);

void rqc_stream_maybe_need_close(rqc_stream_t *stream);

rqc_stream_t *rqc_find_stream_by_id(rqc_stream_id_t stream_id, rqc_id_hash_table_t *streams_hash);

void rqc_stream_set_flow_ctl(rqc_stream_t *stream);

void rqc_stream_update_flow_ctl(rqc_stream_t *stream);

int rqc_stream_do_send_flow_ctl(rqc_stream_t *stream);

int rqc_stream_do_recv_flow_ctl(rqc_stream_t *stream);

int rqc_stream_do_create_flow_ctl(rqc_connection_t *conn, rqc_stream_id_t stream_id, rqc_stream_type_t stream_type);

rqc_stream_t *rqc_passive_create_stream(rqc_connection_t *conn, rqc_stream_id_t stream_id, void *user_data);

void rqc_destroy_stream_frame(rqc_stream_frame_t *stream_frame);

void rqc_destroy_frame_list(rqc_list_head_t *head);

void rqc_stream_refcnt_add(rqc_stream_t *stream);
void rqc_stream_refcnt_del(rqc_stream_t *stream);

void
rqc_stream_send_state_update(rqc_stream_t *stream, rqc_send_stream_state_t state);

void
rqc_stream_recv_state_update(rqc_stream_t *stream, rqc_recv_stream_state_t state);

void rqc_stream_closing(rqc_stream_t *stream, rqc_int_t err);

void rqc_stream_close_discarded_stream(rqc_stream_t *stream);

#endif /* _RQC_STREAM_H_INCLUDED_ */
