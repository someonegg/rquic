/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef RQUIC_RQC_LOG_EVENT_CALLBACK_H
#define RQUIC_RQC_LOG_EVENT_CALLBACK_H

#include "src/common/rqc_log.h"

void rqc_log_CON_SERVER_LISTENING_callback(rqc_log_t *log, const char *func, const struct sockaddr *peer_addr,
    socklen_t peer_addrlen);

void rqc_log_CON_CONNECTION_STARTED_callback(rqc_log_t *log, const char *func,
    rqc_connection_t *conn, rqc_int_t local);

void rqc_log_CON_CONNECTION_CLOSED_callback(rqc_log_t *log, const char *func,
    rqc_connection_t *conn);

void rqc_log_CON_CONNECTION_STATE_UPDATED_callback(rqc_log_t *log, const char *func,
    rqc_connection_t *conn);

void rqc_log_CON_PATH_ASSIGNED_callback(rqc_log_t *log, const char *func,
    rqc_path_ctx_t *path, rqc_connection_t *conn);

void rqc_log_TRA_VERSION_INFORMATION_callback(rqc_log_t *log, const char *func,
    uint32_t local_count, uint32_t *local_version, uint32_t remote_count,
    uint32_t *remote_version, uint32_t choose);

void rqc_log_TRA_ALPN_INFORMATION_callback(rqc_log_t *log, const char *func, const unsigned char * server_alpn_list,
    unsigned int server_alpn_list_len, const unsigned char *client_alpn_list, unsigned int client_alpn_list_len,
    const char *selected_alpn, size_t selected_alpn_len);

void rqc_log_TRA_PARAMETERS_SET_callback(rqc_log_t *log, const char *func, rqc_connection_t *conn,
    rqc_int_t local);

void rqc_log_TRA_PACKET_DROPPED_callback(rqc_log_t *log, const char *func, const char *trigger, rqc_int_t ret,
    const char * pi_pkt_type, rqc_packet_number_t pi_pkt_num);

void rqc_log_TRA_PACKET_RECEIVED_callback(rqc_log_t *log, const char *func,
    rqc_packet_in_t *packet_in);

void rqc_log_TRA_PACKET_SENT_callback(rqc_log_t *log, const char *func, rqc_connection_t *conn,
    rqc_packet_out_t *packet_out, rqc_path_ctx_t *path, rqc_usec_t send_time, ssize_t sent, rqc_bool_t with_pn);

void rqc_log_TRA_PACKET_BUFFERED_callback(rqc_log_t *log, const char *func,
    rqc_packet_in_t *packet_in);

void rqc_log_TRA_PACKETS_ACKED_callback(rqc_log_t *log, const char *func,
    rqc_packet_in_t *packet_in, rqc_packet_number_t high, rqc_packet_number_t low, uint64_t path_id);

void rqc_log_TRA_DATAGRAMS_SENT_callback(rqc_log_t *log, const char *func, ssize_t size, uint64_t path_id);

void rqc_log_TRA_DATAGRAMS_RECEIVED_callback(rqc_log_t *log, const char *func, ssize_t size, uint64_t path_id);

void rqc_log_TRA_STREAM_STATE_UPDATED_callback(rqc_log_t *log, const char *func,
    rqc_stream_t *stream, rqc_int_t stream_type, rqc_int_t state);

void rqc_log_TRA_FRAMES_PROCESSED_callback(rqc_log_t *log, const char *func, ...);

void rqc_log_TRA_STREAM_DATA_MOVED_callback(rqc_log_t *log, const char *func, rqc_stream_t *stream,
    rqc_bool_t is_recv, size_t read_or_write_size, size_t recv_buf_size, uint8_t fin, int ret,
    int pkt_type, int buff_1rtt, size_t offset);

void rqc_log_TRA_DATAGRAM_DATA_MOVED_callback(rqc_log_t *log, const char *func, rqc_stream_t *stream,
    size_t moved_data_len, const char *from, const char *to);

void rqc_log_REC_PARAMETERS_SET_callback(rqc_log_t *log, const char *func, rqc_send_ctl_t *send_ctl,
    uint8_t timer_granularity, rqc_cc_params_t cc_params);

void rqc_log_REC_METRICS_UPDATED_callback(rqc_log_t *log, const char *func, rqc_send_ctl_t *send_ctl);

void rqc_log_REC_CONGESTION_STATE_UPDATED_callback(rqc_log_t *log, const char *func,
    char *new_state);

void rqc_log_REC_LOSS_TIMER_UPDATED_callback(rqc_log_t *log, const char *func,
    rqc_timer_manager_t *timer_manager, rqc_usec_t inter_time, rqc_int_t type, rqc_int_t event);

void rqc_log_REC_PACKET_LOST_callback(rqc_log_t *log, const char *func, rqc_packet_out_t *packet_out,
    rqc_packet_number_t lost_pn, rqc_usec_t lost_send_time, rqc_usec_t loss_delay);

#endif /* RQUIC_RQC_LOG_EVENT_CALLBACK_H */
