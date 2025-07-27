/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "rqc_log_event_callback.h"
#include "src/transport/rqc_conn.h"
#include "src/transport/rqc_stream.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/congestion_control/rqc_bbr_common.h"

void
rqc_log_CON_SERVER_LISTENING_callback(rqc_log_t *log, const char *func, const struct sockaddr *peer_addr,
    socklen_t peer_addrlen)
{
    struct sockaddr_in *sa_peer = (struct sockaddr_in *)peer_addr;
    if(peer_addr->sa_family ==  AF_INET){
        rqc_qlog_implement(log, CON_SERVER_LISTENING, func,
                          "|ip_v4:%s|port_v4:%d|",
                          rqc_peer_addr_str(log->engine, (struct sockaddr*)sa_peer, peer_addrlen),
                          ntohs(sa_peer->sin_port));
    }
    else{
        rqc_qlog_implement(log, CON_SERVER_LISTENING, func,
                          "|ip_v6:%s|port_v6:%d|",
                          rqc_peer_addr_str(log->engine, (struct sockaddr*)sa_peer, peer_addrlen),
                          ntohs(sa_peer->sin_port));
    }
}

void
rqc_log_CON_CONNECTION_STARTED_callback(rqc_log_t *log, const char *func, rqc_connection_t *conn, rqc_int_t local)
{
    if (local == RQC_LOG_LOCAL_EVENT) {
        struct sockaddr_in *sa_local = (struct sockaddr_in *)conn->local_addr;
        rqc_qlog_implement(log, CON_CONNECTION_STARTED, func,
                          "|local|src_ip:%s|src_port:%d|",
                          rqc_local_addr_str(conn->engine, (struct sockaddr*)sa_local, conn->local_addrlen),
                          ntohs(sa_local->sin_port));

    } else {
        struct sockaddr_in *sa_peer = (struct sockaddr_in *)conn->peer_addr;
        rqc_qlog_implement(log, CON_CONNECTION_STARTED, func,
                          "|remote|dst_ip:%s|dst_port:%d|scid:%s|dcid:%s|",
                          rqc_peer_addr_str(conn->engine, (struct sockaddr*)sa_peer, conn->peer_addrlen),
                          ntohs(sa_peer->sin_port), log->scid, rqc_dcid_str(conn->engine, &conn->dcid_set.current_dcid));
    }
}

void
rqc_log_CON_CONNECTION_CLOSED_callback(rqc_log_t *log, const char *func, rqc_connection_t *conn)
{
    if (conn->conn_err != 0){
        unsigned char log_buf[500];
        unsigned char *p = log_buf;
        unsigned char *last = log_buf + sizeof(log_buf);
        rqc_path_ctx_t *path = conn->the_path;
        if (path) {
            uint8_t idx = path->path_send_ctl->ctl_cwndlim_update_idx;
            p = rqc_sprintf(p, last, "<path:%ui, (%ui,%ui,%ui)>",
                path->path_id,
                rqc_calc_delay(path->path_send_ctl->ctl_recent_cwnd_limitation_time[idx], conn->conn_create_time) / 1000,
                rqc_calc_delay(path->path_send_ctl->ctl_recent_cwnd_limitation_time[(idx + 1) % 3], conn->conn_create_time) / 1000,
                rqc_calc_delay(path->path_send_ctl->ctl_recent_cwnd_limitation_time[(idx + 2) % 3], conn->conn_create_time) / 1000);
            if (p != last) {
                *p = '\0';
            }
        }
        rqc_qlog_implement(log, CON_CONNECTION_CLOSED, func,
                            "|err_code:%d|pkt_dropped:%d|recent_congestion:%s|",
                            conn->conn_err, conn->packet_dropped_count, log_buf);
    }
    else{
        rqc_qlog_implement(log, CON_CONNECTION_CLOSED, func,
                            "|err_code:%d|pkt_dropped:%d|",
                            conn->conn_err, conn->packet_dropped_count);
    }
}

void
rqc_log_CON_CONNECTION_STATE_UPDATED_callback(rqc_log_t *log, const char *func, rqc_connection_t *conn)
{
    rqc_qlog_implement(log, CON_CONNECTION_STATE_UPDATED, func,
                        "|new:%s|flag:%s|",
                        rqc_conn_state_2_str(conn->conn_state), rqc_conn_flag_2_str(conn, conn->conn_flag));
}

void
rqc_log_CON_PATH_ASSIGNED_callback(rqc_log_t *log, const char *func,
    rqc_path_ctx_t *path, rqc_connection_t *conn)
{
    rqc_qlog_implement(log, CON_PATH_ASSIGNED, func,
                      "|path_id:%ui|local_addr:%s|peer_addr:%s|dcid:%s|scid:%s|",
                     path->path_id,  rqc_conn_addr_str(conn), rqc_path_addr_str(path),
                     rqc_dcid_str(log->engine, &path->path_dcid), rqc_scid_str(log->engine, &path->path_scid));
}

void
rqc_log_TRA_VERSION_INFORMATION_callback(rqc_log_t *log, const char *func, uint32_t local_count,
    uint32_t *local_version, uint32_t remote_count, uint32_t *remote_version, uint32_t choose)
{
    unsigned char log_buf[RQC_MAX_LOG_LEN];
    unsigned char *p = log_buf;
    unsigned char *last = log_buf + sizeof(log_buf);

    p = rqc_sprintf(p, last, "local_version:");
    for (uint32_t i = 0; i < local_count; ++i) {
        p = rqc_sprintf(p, last, " %d", local_version[i]);
    }

    p = rqc_sprintf(p, last, "|remote_version:");
    for (uint32_t i = 0; i < remote_count; ++i) {
        p = rqc_sprintf(p, last, " %d", remote_version[i]);
    }

    if (p != last) {
        *p = '\0';
    }

    rqc_qlog_implement(log, TRA_VERSION_INFORMATION, func,
                      "|%s|choose:%d|", log_buf, choose);
}

void
rqc_log_TRA_ALPN_INFORMATION_callback(rqc_log_t *log, const char *func, const unsigned char * server_alpn_list,
    unsigned int server_alpn_list_len, const unsigned char *client_alpn_list, unsigned int client_alpn_list_len,
    const char *selected_alpn, size_t selected_alpn_len)
{
    unsigned char log_buf[RQC_MAX_LOG_LEN];
    unsigned char *p = log_buf;
    unsigned char *last = log_buf + sizeof(log_buf);
    p = rqc_sprintf(p, last, "client_alpn:");

    uint8_t alpn_len;
    size_t alpn_write_len;

    for (unsigned i = 0; i < client_alpn_list_len;) {
        alpn_len = client_alpn_list[i];
        alpn_write_len = alpn_len;
        p = rqc_sprintf(p, last, "%*s ", alpn_write_len, &client_alpn_list[i + 1]);
        i += alpn_len;
        i++;
    }
    p = rqc_sprintf(p, last, "|server_alpn:");

    for (unsigned i = 0; i < server_alpn_list_len;) {
        alpn_len = server_alpn_list[i];
        alpn_write_len = alpn_len;
        p = rqc_sprintf(p, last, "%*s ", alpn_write_len, &server_alpn_list[i + 1]);
        i += alpn_len;
        i++;
    }
    rqc_qlog_implement(log, TRA_ALPN_INFORMATION, func,
                      "|%s|selected_alpn:%*s|", log_buf, selected_alpn_len, selected_alpn);
  }

void
rqc_log_TRA_PARAMETERS_SET_callback(rqc_log_t *log, const char *func, rqc_connection_t *conn, rqc_int_t local)
{
    rqc_trans_settings_t *setting;
    if (local == RQC_LOG_LOCAL_EVENT) {
        setting = &conn->local_settings;

    } else {
        setting = &conn->remote_settings;
    }

    rqc_qlog_implement(log, TRA_PARAMETERS_SET, func,
                      "|%s|max_idle_timeout:%d|max_udp_payload_size:%d|max_data:%d|",
                      local == RQC_LOG_LOCAL_EVENT ? "local" : "remote",
                      setting->max_idle_timeout, setting->max_udp_payload_size, setting->max_data);
}

void
rqc_log_TRA_PACKET_RECEIVED_callback(rqc_log_t *log, const char *func, rqc_packet_in_t *packet_in)
{
    rqc_qlog_implement(log, TRA_PACKET_RECEIVED, func,
                      "|pkt_type:%s|pkt_num:%ui|len:%uz|frame_flag:%s|path_id:%ui|",
                      rqc_pkt_type_2_str(packet_in->pi_pkt.pkt_type), packet_in->pi_pkt.pkt_num,
                      packet_in->buf_size, rqc_frame_type_2_str(log->engine, packet_in->pi_frame_types), RQC_INITIAL_PATH_ID);
}

void
rqc_log_TRA_PACKET_DROPPED_callback(rqc_log_t *log, const char *func, const char *trigger, rqc_int_t ret,
    const char* pi_pkt_type, rqc_packet_number_t pi_pkt_num)
{
    rqc_qlog_implement(log, TRA_PACKET_DROPPED, func,
                    "|trigger:%s|ret:%d|pkt_type:%s|pkt_num:%ui|",
                    trigger, ret, pi_pkt_type, pi_pkt_num);
}

void
rqc_log_TRA_PACKET_SENT_callback(rqc_log_t *log, const char *func, rqc_connection_t *conn,
    rqc_packet_out_t *packet_out, rqc_path_ctx_t *path, rqc_usec_t send_time, ssize_t sent, rqc_bool_t with_pn)
{
    if (with_pn) {
        rqc_qlog_implement(log, TRA_PACKET_SENT, func,
                        "|<==|conn:%p|path_id:%ui|pkt_type:%s|pkt_num:%ui|size:%d|frame_flag:%s|"
                        "sent:%z|inflight:%ud|now:%ui|stream_id:%ui|stream_offset:%ui|",
                        conn, path->path_id,
                        rqc_pkt_type_2_str(packet_out->po_pkt.pkt_type), packet_out->po_pkt.pkt_num,
                        packet_out->po_used_size, rqc_frame_type_2_str(log->engine, packet_out->po_frame_types),
                        sent, path->path_send_ctl->ctl_bytes_in_flight, send_time,
                        packet_out->po_stream_id, packet_out->po_stream_offset);
    } else {
        rqc_qlog_implement(log, TRA_PACKET_SENT, func,
                       "|<==|conn:%p|path_id:%ui|pkt_type:%s|frame_flag:%s|size:%ud|sent:%z|",
                       conn, path->path_id, rqc_pkt_type_2_str(packet_out->po_pkt.pkt_type),
                       rqc_frame_type_2_str(log->engine, packet_out->po_frame_types), packet_out->po_used_size, sent);
    }
}

void
rqc_log_TRA_PACKET_BUFFERED_callback(rqc_log_t *log, const char *func, rqc_packet_in_t *packet_in)
{
    rqc_qlog_implement(log, TRA_PACKET_BUFFERED, func,
                      "|pkt_num:%ui|pkt_type:%d|len:%d|",
                      packet_in->pi_pkt.pkt_num, packet_in->pi_pkt.pkt_type, packet_in->buf_size);
}

void
rqc_log_TRA_PACKETS_ACKED_callback(rqc_log_t *log, const char *func, rqc_packet_in_t *packet_in,
    rqc_packet_number_t high, rqc_packet_number_t low, uint64_t path_id)
{
    rqc_qlog_implement(log, TRA_PACKETS_ACKED, func,
                      "|high:%d|low:%d|path_id:%ui|",
                      high, low, path_id);
}

void
rqc_log_TRA_DATAGRAMS_SENT_callback(rqc_log_t *log, const char *func, ssize_t size, uint64_t path_id)
{
    rqc_qlog_implement(log, TRA_DATAGRAMS_SENT, func,
                      "|size:%z|path_id:%ui|", size, path_id);
}

void
rqc_log_TRA_DATAGRAMS_RECEIVED_callback(rqc_log_t *log, const char *func, ssize_t size, uint64_t path_id)
{
    rqc_qlog_implement(log, TRA_DATAGRAMS_RECEIVED, func,
                      "|size:%d|path_id:%ui|", size, path_id);
}

void
rqc_log_TRA_STREAM_STATE_UPDATED_callback(rqc_log_t *log, const char *func, rqc_stream_t *stream,
    rqc_int_t stream_type, rqc_int_t state)
{
    if (stream_type == RQC_LOG_STREAM_SEND) {
        rqc_qlog_implement(log, TRA_STREAM_STATE_UPDATED, func,
                          "|stream_id:%d|send_stream|old:%d|new:%d|",
                          stream->stream_id, stream->stream_state_send, state);
    } else {
        rqc_qlog_implement(log, TRA_STREAM_STATE_UPDATED, func,
                          "|stream_id:%d|recv_stream|old:%d|new:%d|",
                          stream->stream_id, stream->stream_state_recv, state);
    }
}

void
rqc_log_TRA_FRAMES_PROCESSED_callback(rqc_log_t *log, const char *func, ...)
{
    va_list args;
    va_start(args, func);
    rqc_frame_type_t frame_type = va_arg(args, rqc_frame_type_t);
    switch (frame_type) {
    case RQC_FRAME_HANDSHAKE: {
        size_t alpn_len = va_arg(args, size_t);
        size_t tp_len = va_arg(args, size_t);
        rqc_qlog_implement(log, TRA_FRAMES_PROCESSED, func,
                          "|type:%d|alpn_len:%uz|tp_len:%uz|", frame_type, alpn_len, tp_len);
        break;
    }

    case RQC_FRAME_ACK: {
        rqc_ack_info_t *ack_info = va_arg(args, rqc_ack_info_t*);
        unsigned char buf[1024];
        unsigned char *p = buf;
        unsigned char *last = buf + sizeof(buf);

        for (int i = 0; i < ack_info->n_ranges; i++) {
            if (i == 0) {
                p = rqc_sprintf(p, last, "{%ui - %ui", ack_info->ranges[i].low, ack_info->ranges[i].high);

            } else {
                p = rqc_sprintf(p, last, ", %ui - %ui", ack_info->ranges[i].low, ack_info->ranges[i].high);
            }
        }

        p = rqc_sprintf(p, last, "}");
        if (p != last) {
            *p = '\0';
        }

        rqc_qlog_implement(log, TRA_FRAMES_PROCESSED, func,
                          "|type:%d|ack_delay:%ui|ack_range:%s|",
                          frame_type, ack_info->ack_delay, buf);
        break;
    }

    case RQC_FRAME_RESET_STREAM: {
        rqc_stream_id_t stream_id = va_arg(args, rqc_stream_id_t);
        uint64_t err_code = va_arg(args, uint64_t);
        uint64_t final_size = va_arg(args, uint64_t);
        rqc_qlog_implement(log, TRA_FRAMES_PROCESSED, func,
                          "|type:%d|stream_id:%ui|err_code:%ui|final_size:%ui|",
                          frame_type, stream_id, err_code, final_size);
        break;
    }

    case RQC_FRAME_STOP_SENDING: {
        rqc_stream_id_t stream_id = va_arg(args, rqc_stream_id_t);
        uint64_t err_code = va_arg(args, uint64_t);
        rqc_qlog_implement(log, TRA_FRAMES_PROCESSED, func,
                          "|type:%d|stream_id:%ui|err_code:%ui|", frame_type, stream_id, err_code);
        break;
    }

    case RQC_FRAME_STREAM: {
        rqc_stream_frame_t *frame = va_arg(args, rqc_stream_frame_t*);
        rqc_qlog_implement(log, TRA_FRAMES_PROCESSED, func,
                          "|type:%d|data_offset:%ui|data_length:%d|fin:%d|",
                          frame_type, frame->data_offset, frame->data_length, frame->fin);
        break;
    }

    case RQC_FRAME_MAX_DATA: {
        uint64_t max_data = va_arg(args, uint64_t);
        rqc_qlog_implement(log, TRA_FRAMES_PROCESSED, func,
                          "|type:%d|max_data:%ui|", frame_type, max_data);
        break;
    }

    case RQC_FRAME_MAX_STREAM_DATA: {
        rqc_stream_id_t stream_id = va_arg(args, rqc_stream_id_t);
        uint64_t max_stream_data = va_arg(args, uint64_t);
        rqc_qlog_implement(log, TRA_FRAMES_PROCESSED, func,
                          "|type:%d|stream_id:%ui|max_stream_data:%ui|",
                          frame_type, stream_id, max_stream_data);
        break;
    }
    case RQC_FRAME_MAX_STREAMS: {
        int bidirectional = va_arg(args, int);
        uint64_t max_streams = va_arg(args, uint64_t);
        if (bidirectional) {
            rqc_qlog_implement(log, TRA_FRAMES_PROCESSED, func,
                              "|type:%d|stream_type:bidirectional|maximum:%ui|",
                              frame_type, max_streams);

        } else {
            rqc_qlog_implement(log, TRA_FRAMES_PROCESSED, func,
                              "|type:%d|stream_type:unidirectional|maximum:%ui|",
                              frame_type, max_streams);
        }
        break;
    }

    case RQC_FRAME_DATA_BLOCKED: {
        uint64_t data_limit = va_arg(args, uint64_t);
        rqc_qlog_implement(log, TRA_FRAMES_PROCESSED, func,
                          "|type:%d|bidirectional|limit:%ui|",
                          frame_type, data_limit);
        break;
    }

    case RQC_FRAME_STREAM_DATA_BLOCKED: {
        rqc_stream_id_t stream_id = va_arg(args, rqc_stream_id_t);
        uint64_t stream_data_limit = va_arg(args, uint64_t);
        rqc_qlog_implement(log, TRA_FRAMES_PROCESSED, func,
                          "|type:%d|bidirectional|stream_id:%ui|limit:%ui|",
                          frame_type, stream_id, stream_data_limit);
        break;
    }

    case RQC_FRAME_STREAMS_BLOCKED: {
        int bidirectional = va_arg(args, int);
        uint64_t stream_limit = va_arg(args, uint64_t);
        if (bidirectional) {
            rqc_qlog_implement(log, TRA_FRAMES_PROCESSED, func,
                              "|type:%d|stream_type:bidirectional|limit:%ui|",
                              frame_type, stream_limit);

        } else {
            rqc_qlog_implement(log, TRA_FRAMES_PROCESSED, func,
                              "|type:%d|stream_type:unidirectional|limit:%ui|",
                              frame_type, stream_limit);
        }
        break;
    }

    case RQC_FRAME_CONNECTION_CLOSE: {
        uint64_t err_code = va_arg(args, uint64_t);
        rqc_qlog_implement(log, TRA_FRAMES_PROCESSED, func,
                          "|type:%d|err_code:%ui|", frame_type, err_code);
        break;
    }

    /* TODO: add log */
    case RQC_FRAME_PING:
    case RQC_FRAME_PADDING:
    case RQC_FRAME_PATH_CHALLENGE:
    case RQC_FRAME_PATH_RESPONSE:
    case RQC_FRAME_Extension:
        break;

    default:
        break;
    }
    va_end(args);
}

void
rqc_log_TRA_STREAM_DATA_MOVED_callback(rqc_log_t *log, const char *func, rqc_stream_t *stream,
                                        rqc_bool_t is_recv, size_t read_or_write_size, size_t recv_buf_size,
                                        uint8_t fin, int ret, int pkt_type, int buff_1rtt, size_t offset)
{
    if (is_recv) {
        rqc_qlog_implement(log, TRA_STREAM_DATA_MOVED, func,
                          "|stream_id:%ui|read:%uz|recv_buf_size:%uz|fin:%d|stream_length:%ui|next_read_offset:%ui|conn:%p"
                          "|from:transport|to:application|",
                          stream->stream_id, read_or_write_size, recv_buf_size, fin,
                          stream->stream_data_in.stream_length, stream->stream_data_in.next_read_offset,
                          stream->stream_conn);
    }
    else{
        rqc_connection_t *conn = stream->stream_conn;
        rqc_qlog_implement(log, TRA_STREAM_DATA_MOVED, func,
                          "|ret:%d|stream_id:%ui|stream_send_offset:%ui|pkt_type:%s|buff_1rtt:%d"
                          "|send_data_size:%uz|offset:%uz|fin:%d|stream_flag:%d|conn:%p|conn_state:%s|flag:%s"
                          "|from:application|to:transport|",
                          ret, stream->stream_id, stream->stream_send_offset, rqc_pkt_type_2_str(pkt_type),
                          buff_1rtt, read_or_write_size, offset, fin, stream->stream_flag, conn,
                          rqc_conn_state_2_str(conn->conn_state), rqc_conn_flag_2_str(conn, conn->conn_flag));
    }
}

void
rqc_log_TRA_DATAGRAM_DATA_MOVED_callback(rqc_log_t *log, const char *func, rqc_stream_t *stream,
                                        size_t moved_data_len, const char *from, const char *to)
{
    rqc_qlog_implement(log, TRA_STREAM_DATA_MOVED, func,
                          "|stream_id:%ui|length:%uz|from:%s|to:%s|",
                          stream->stream_id, moved_data_len, from, to);
}

void
rqc_log_REC_PARAMETERS_SET_callback(rqc_log_t *log, const char *func, rqc_send_ctl_t *send_ctl, uint8_t timer_granularity,
                                    rqc_cc_params_t cc_params)
{
    rqc_qlog_implement(log, REC_PARAMETERS_SET, func,
                      "|reordering_threshold:%ui|time_threshold:%d|timer_granularity:%d|initial_rtt:%ui|"
                      "initial_congestion_window:%d|minimum_congestion_window:%d|",
                      send_ctl->ctl_reordering_packet_threshold, send_ctl->ctl_reordering_time_threshold_shift, timer_granularity, send_ctl->ctl_srtt / 1000,
                      cc_params.init_cwnd, cc_params.min_cwnd);
}

void
rqc_log_REC_METRICS_UPDATED_callback(rqc_log_t *log, const char *func, rqc_send_ctl_t *send_ctl)
{
    uint64_t cwnd = send_ctl->ctl_cong_callback->rqc_cong_ctl_get_cwnd(send_ctl->ctl_cong);
    int64_t bw = 0;
    uint64_t pacing_rate = 0;
    int mode = 0;
    rqc_usec_t min_rtt = 0;

    if (send_ctl->ctl_cong_callback->rqc_cong_ctl_init_bbr) {
        bw = send_ctl->ctl_cong_callback->
                rqc_cong_ctl_get_bandwidth_estimate(send_ctl->ctl_cong);
        pacing_rate = send_ctl->ctl_cong_callback->
                rqc_cong_ctl_get_pacing_rate(send_ctl->ctl_cong);
        mode = send_ctl->ctl_cong_callback->rqc_cong_ctl_info_cb->mode(send_ctl->ctl_cong);
        min_rtt = send_ctl->ctl_cong_callback-> rqc_cong_ctl_info_cb->min_rtt(send_ctl->ctl_cong);
        rqc_qlog_implement(log, REC_METRICS_UPDATED, func,
                          "|cwnd:%ui|inflight:%ud|mode:%ud|applimit:%ud|pacing_rate:%ui|bw:%ui|srtt:%ui|"
                          "latest_rtt:%ui|ctl_rttvar:%ui|pto_count:%ud|min_rtt:%ui|send:%ud|lost:%ud|tlp:%ud|recv:%ud|",
                          cwnd, send_ctl->ctl_bytes_in_flight, mode, send_ctl->ctl_app_limited, pacing_rate, bw, send_ctl->ctl_srtt,
                          send_ctl->ctl_latest_rtt, send_ctl->ctl_pto_count, min_rtt, send_ctl->ctl_send_count, send_ctl->ctl_lost_count,
                          send_ctl->ctl_tlp_count, send_ctl->ctl_recv_count);

    } else {
        rqc_qlog_implement(log, REC_METRICS_UPDATED, func,
                          "|cwnd:%ui|inflight:%ud|applimit:%ud|srtt:%ui|latest_rtt:%ui|pto_count:%ud|"
                          "send:%ud|lost:%ud|tlp:%ud|recv:%ud|",
                          cwnd, send_ctl->ctl_bytes_in_flight, send_ctl->ctl_app_limited, send_ctl->ctl_srtt, send_ctl->ctl_latest_rtt, send_ctl->ctl_pto_count,
                          send_ctl->ctl_send_count, send_ctl->ctl_lost_count, send_ctl->ctl_tlp_count, send_ctl->ctl_recv_count);
    }
}

void
rqc_log_REC_CONGESTION_STATE_UPDATED_callback(rqc_log_t *log, const char *func, char *new_state)
{
    rqc_qlog_implement(log, REC_CONGESTION_STATE_UPDATED, func,
                      "|new_state:%s|", new_state);
}

void
rqc_log_REC_LOSS_TIMER_UPDATED_callback(rqc_log_t *log, const char *func,
    rqc_timer_manager_t *timer_manager, rqc_usec_t inter_time, rqc_int_t type, rqc_int_t event)
{
    if (type != RQC_TIMER_LOSS_DETECTION){
        return ;
    }
    if (event == RQC_LOG_TIMER_SET) {
        rqc_qlog_implement(log, REC_LOSS_TIMER_UPDATED, func,
                          "|event_type:set|type:%s|expire:%ui|interv:%ui|",
                          rqc_timer_type_2_str(type), timer_manager->timer[type].expire_time, inter_time);

    } else if (event == RQC_LOG_TIMER_EXPIRE) {
        rqc_qlog_implement(log, REC_LOSS_TIMER_UPDATED, func,
                          "|event_type:expired|type:%s|expire_time:%ui|",
                          rqc_timer_type_2_str(type), timer_manager->timer[type].expire_time);

    } else if (event == RQC_LOG_TIMER_CANCEL) {
        rqc_qlog_implement(log, REC_LOSS_TIMER_UPDATED, func,
                          "|event_type:cancel|type:%s|", rqc_timer_type_2_str(type));
    }
}

void
rqc_log_REC_PACKET_LOST_callback(rqc_log_t *log, const char *func, rqc_packet_out_t *packet_out,
                                rqc_packet_number_t lost_pn, rqc_usec_t lost_send_time, rqc_usec_t loss_delay)
{
    rqc_qlog_implement(log, REC_PACKET_LOST, func,
                      "|pkt_type:%d|pkt_num:%d|lost_pn:%ui|po_sent_time:%ui|"
                      "lost_send_time:%ui|loss_delay:%ui|frame:%s|repair:%d|",
                      packet_out->po_pkt.pkt_type, packet_out->po_pkt.pkt_num,
                      lost_pn, packet_out->po_sent_time, lost_send_time, loss_delay,
                      rqc_frame_type_2_str(log->engine, packet_out->po_frame_types), RQC_NEED_REPAIR(packet_out->po_frame_types));
}
