/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "src/common/rqc_memory_pool.h"
#include "src/common/rqc_id_hash.h"
#include "src/transport/rqc_conn.h"
#include "src/transport/rqc_stream.h"
#include "src/transport/rqc_packet_parser.h"
#include "src/transport/rqc_frame_parser.h"
#include "src/transport/rqc_packet_out.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/transport/rqc_frame.h"
#include "src/transport/rqc_engine.h"
#include "src/transport/rqc_packet.h"
#include "src/transport/rqc_utils.h"
#include "src/transport/rqc_pacing.h"

static rqc_stream_id_t
rqc_gen_stream_id(rqc_connection_t *conn, rqc_stream_type_t type)
{
    rqc_stream_id_t sid = 0;
    if (type == RQC_CLI_BID || type == RQC_SVR_BID) {
        sid = conn->cur_stream_id_bidi_local++;

    } else if (type == RQC_CLI_UNI || type == RQC_SVR_UNI) {
        sid = conn->cur_stream_id_uni_local++;
    }

    sid = sid << 2 | type;
    return sid;
}

void
rqc_stream_ready_to_write(rqc_stream_t *stream)
{
    if (!(stream->stream_flag & RQC_STREAM_FLAG_READY_TO_WRITE)) {
        rqc_list_add_tail(&stream->write_stream_list, &stream->stream_conn->conn_write_streams);
        stream->stream_flag |= RQC_STREAM_FLAG_READY_TO_WRITE;
    }

    rqc_engine_remove_wakeup_queue(stream->stream_conn->engine, stream->stream_conn);
    rqc_engine_add_active_queue(stream->stream_conn->engine, stream->stream_conn);
}

void
rqc_stream_shutdown_write(rqc_stream_t *stream)
{
    if (stream->stream_flag & RQC_STREAM_FLAG_READY_TO_WRITE) {
        rqc_list_del_init(&stream->write_stream_list);
        stream->stream_flag &= ~RQC_STREAM_FLAG_READY_TO_WRITE;
    }
}

void
rqc_stream_ready_to_read(rqc_stream_t *stream)
{
    if (!(stream->stream_flag & RQC_STREAM_FLAG_READY_TO_READ)) {
        rqc_list_add_tail(&stream->read_stream_list, &stream->stream_conn->conn_read_streams);
        stream->stream_flag |= RQC_STREAM_FLAG_READY_TO_READ;
    }

    rqc_engine_remove_wakeup_queue(stream->stream_conn->engine, stream->stream_conn);
    rqc_engine_add_active_queue(stream->stream_conn->engine, stream->stream_conn);
}

void
rqc_stream_shutdown_read(rqc_stream_t *stream)
{
    if (stream->stream_flag & RQC_STREAM_FLAG_READY_TO_READ) {
        rqc_list_del_init(&stream->read_stream_list);
        stream->stream_flag &= ~RQC_STREAM_FLAG_READY_TO_READ;
    }
}

rqc_bool_t
rqc_stream_is_terminal_state(rqc_stream_t *stream)
{
    rqc_bool_t check_send_state, check_recv_state;
    rqc_conn_type_t conn_type = stream->stream_conn->conn_type;

    if (stream->stream_type == RQC_CLI_BID || stream->stream_type == RQC_SVR_BID) {
        check_send_state = RQC_TRUE;
        check_recv_state = RQC_TRUE;
    } else if ((stream->stream_type == RQC_CLI_UNI && conn_type == RQC_CONN_TYPE_CLIENT)
               || (stream->stream_type == RQC_SVR_UNI && conn_type == RQC_CONN_TYPE_SERVER)) {
        /* Uni Stream Send side */
        check_send_state = RQC_TRUE;
        check_recv_state = RQC_FALSE;
    } else {
        /* Uni Stream Recv side */
        check_send_state = RQC_FALSE;
        check_recv_state = RQC_TRUE;
    }

    if (check_send_state && check_recv_state) {
        return (stream->stream_state_send == RQC_SEND_STREAM_ST_DATA_RECVD
                || stream->stream_state_send == RQC_SEND_STREAM_ST_RESET_RECVD)
                && (stream->stream_state_recv == RQC_RECV_STREAM_ST_DATA_READ
                || stream->stream_state_recv == RQC_RECV_STREAM_ST_RESET_READ);
    } else if (check_send_state) {
        return stream->stream_state_send == RQC_SEND_STREAM_ST_DATA_RECVD
               || stream->stream_state_send == RQC_SEND_STREAM_ST_RESET_RECVD;
    } else {
        return stream->stream_state_recv == RQC_RECV_STREAM_ST_DATA_READ
               || stream->stream_state_recv == RQC_RECV_STREAM_ST_RESET_READ;
    }
}

void
rqc_stream_maybe_need_close(rqc_stream_t *stream)
{
    if (stream->stream_flag & RQC_STREAM_FLAG_NEED_CLOSE) {
        return;
    }

    if (stream->stream_state_send == RQC_SEND_STREAM_ST_DATA_RECVD
        && stream->stream_stats.all_data_acked_time == 0)
    {
        stream->stream_stats.all_data_acked_time = rqc_monotonic_timestamp();
    }

    stream->stream_stats.max_pto_backoff = rqc_max(stream->stream_stats.max_pto_backoff, rqc_conn_get_max_pto_backoff(stream->stream_conn, 1));

    if (rqc_stream_is_terminal_state(stream))
    {
        stream->stream_flag |= RQC_STREAM_FLAG_NEED_CLOSE;
        rqc_usec_t now = rqc_monotonic_timestamp();
        if (stream->stream_stats.close_time == 0) {
            stream->stream_stats.close_time = now;
        }

        rqc_timer_manager_t *timer_manager = &stream->stream_conn->conn_timer_manager;
        rqc_usec_t pto = rqc_conn_get_max_pto(stream->stream_conn);
        rqc_usec_t new_expire = now + 3 * pto;
        if ((timer_manager->timer[RQC_TIMER_STREAM_CLOSE].timer_is_set
            && new_expire < timer_manager->timer[RQC_TIMER_STREAM_CLOSE].expire_time)
            || !timer_manager->timer[RQC_TIMER_STREAM_CLOSE].timer_is_set)
        {
            rqc_timer_set(timer_manager, RQC_TIMER_STREAM_CLOSE, now, 3 * pto);
        }
        stream->stream_close_time = new_expire;
        rqc_list_add_tail(&stream->closing_stream_list, &stream->stream_conn->conn_closing_streams);
        rqc_stream_shutdown_read(stream);
        rqc_stream_shutdown_write(stream);
    }
}

void
rqc_stream_close_discarded_stream(rqc_stream_t *stream)
{
    rqc_timer_manager_t    *timer_manager;
    rqc_usec_t              now;
    rqc_usec_t              pto;
    rqc_usec_t              new_expire;

    if (stream->stream_flag & RQC_STREAM_FLAG_NEED_CLOSE) {
        return;
    }

    stream->stream_flag |= RQC_STREAM_FLAG_NEED_CLOSE;

    now = rqc_monotonic_timestamp();
    pto = rqc_conn_get_max_pto(stream->stream_conn);
    new_expire = now + 3 * pto;

    timer_manager = &stream->stream_conn->conn_timer_manager;

    if ((timer_manager->timer[RQC_TIMER_STREAM_CLOSE].timer_is_set
        && new_expire < timer_manager->timer[RQC_TIMER_STREAM_CLOSE].expire_time)
        || !timer_manager->timer[RQC_TIMER_STREAM_CLOSE].timer_is_set)
    {
        rqc_timer_set(timer_manager, RQC_TIMER_STREAM_CLOSE, now, 3 * pto);
    }

    stream->stream_close_time = new_expire;

    rqc_list_add_tail(&stream->closing_stream_list, &stream->stream_conn->conn_closing_streams);
    rqc_stream_shutdown_read(stream);
    rqc_stream_shutdown_write(stream);
}

rqc_stream_t *
rqc_find_stream_by_id(rqc_stream_id_t stream_id, rqc_id_hash_table_t *streams_hash)
{
    rqc_stream_t *stream = rqc_id_hash_find(streams_hash, stream_id);
    return stream;
}

void
rqc_stream_set_flow_ctl(rqc_stream_t *stream)
{
    rqc_trans_settings_t *local_settings = &stream->stream_conn->local_settings;
    rqc_trans_settings_t *remote_settings = &stream->stream_conn->remote_settings;
    rqc_connection_t *conn = stream->stream_conn;

    if ((remote_settings->max_stream_data_bidi_remote
         && remote_settings->max_stream_data_bidi_local
         && remote_settings->max_stream_data_uni) == RQC_FALSE)
    {
        remote_settings = &stream->stream_conn->local_settings;
    }
    /*
     * initial_max_stream_data_bidi_local (0x0005):  This parameter is an
     * integer value specifying the initial flow control limit for
     * locally-initiated bidirectional streams.  This limit applies to
     * newly created bidirectional streams opened by the endpoint that
     * sends the transport parameter.  In client transport parameters,
     * this applies to streams with an identifier with the least
     * significant two bits set to 0x0; in server transport parameters,
     * this applies to streams with the least significant two bits set to
     * 0x1.
     *
     *  initial_max_stream_data_bidi_remote (0x0006):  This parameter is an
     * integer value specifying the initial flow control limit for peer-
     * initiated bidirectional streams.  This limit applies to newly
     * created bidirectional streams opened by the endpoint that receives
     * the transport parameter.  In client transport parameters, this
     * applies to streams with an identifier with the least significant
     * two bits set to 0x1; in server transport parameters, this applies
     * to streams with the least significant two bits set to 0x0.
     */
    if (conn->conn_type == RQC_CONN_TYPE_CLIENT) {
        if (stream->stream_type == RQC_CLI_BID) {
            stream->stream_flow_ctl.fc_max_stream_data_can_send = remote_settings->max_stream_data_bidi_remote;
            stream->stream_flow_ctl.fc_max_stream_data_can_recv = local_settings->max_stream_data_bidi_local;
            stream->stream_flow_ctl.fc_stream_recv_window_size = local_settings->max_stream_data_bidi_local;

        } else if (stream->stream_type == RQC_SVR_BID) {
            /*
             * in server transport parameters,
             * this applies to streams with the least significant two bits set to 0x1
             */
            stream->stream_flow_ctl.fc_max_stream_data_can_send = remote_settings->max_stream_data_bidi_local;
            stream->stream_flow_ctl.fc_max_stream_data_can_recv = local_settings->max_stream_data_bidi_remote;
            stream->stream_flow_ctl.fc_stream_recv_window_size = local_settings->max_stream_data_bidi_remote;

        } else {
            stream->stream_flow_ctl.fc_max_stream_data_can_send = remote_settings->max_stream_data_uni;
            stream->stream_flow_ctl.fc_max_stream_data_can_recv = local_settings->max_stream_data_uni;
            stream->stream_flow_ctl.fc_stream_recv_window_size = local_settings->max_stream_data_uni;
        }

    } else { /* conn->conn_type == RQC_CONN_TYPE_SERVER */
        if (stream->stream_type == RQC_CLI_BID) {
            stream->stream_flow_ctl.fc_max_stream_data_can_send = remote_settings->max_stream_data_bidi_local;
            stream->stream_flow_ctl.fc_max_stream_data_can_recv = local_settings->max_stream_data_bidi_remote;
            stream->stream_flow_ctl.fc_stream_recv_window_size = local_settings->max_stream_data_bidi_remote;

        } else if (stream->stream_type == RQC_SVR_BID) {
            stream->stream_flow_ctl.fc_max_stream_data_can_send = remote_settings->max_stream_data_bidi_remote;
            stream->stream_flow_ctl.fc_max_stream_data_can_recv = local_settings->max_stream_data_bidi_local;
            stream->stream_flow_ctl.fc_stream_recv_window_size = local_settings->max_stream_data_bidi_local;

        } else {
            stream->stream_flow_ctl.fc_max_stream_data_can_send = remote_settings->max_stream_data_uni;
            stream->stream_flow_ctl.fc_max_stream_data_can_recv = local_settings->max_stream_data_uni;
            stream->stream_flow_ctl.fc_stream_recv_window_size = local_settings->max_stream_data_uni;
        }
    }
}

void
rqc_stream_update_flow_ctl(rqc_stream_t *stream)
{
    rqc_trans_settings_t *remote_settings = &stream->stream_conn->remote_settings;
    rqc_connection_t *conn = stream->stream_conn;

    if (conn->conn_type == RQC_CONN_TYPE_CLIENT) {
        if (stream->stream_type == RQC_CLI_BID) {
            stream->stream_flow_ctl.fc_max_stream_data_can_send = remote_settings->max_stream_data_bidi_remote;

        } else if (stream->stream_type == RQC_SVR_BID) {
            /*
             * in server transport parameters,
             * this applies to streams with the least significant two bits set to 0x1
             */
            stream->stream_flow_ctl.fc_max_stream_data_can_send = remote_settings->max_stream_data_bidi_local;

        } else {
            stream->stream_flow_ctl.fc_max_stream_data_can_send = remote_settings->max_stream_data_uni;
        }

    } else { /* conn->conn_type == RQC_CONN_TYPE_SERVER */
        if (stream->stream_type == RQC_CLI_BID) {
            stream->stream_flow_ctl.fc_max_stream_data_can_send = remote_settings->max_stream_data_bidi_local;

        } else if (stream->stream_type == RQC_SVR_BID) {
            stream->stream_flow_ctl.fc_max_stream_data_can_send = remote_settings->max_stream_data_bidi_remote;

        } else {
            stream->stream_flow_ctl.fc_max_stream_data_can_send = remote_settings->max_stream_data_uni;
        }
    }
}

uint64_t
rqc_stream_get_init_max_stream_data(rqc_stream_t *stream)
{
    rqc_connection_t *conn = stream->stream_conn;
    if (stream->stream_type == RQC_SVR_BID) {
        if (conn->conn_type == RQC_CONN_TYPE_SERVER) {
            return conn->local_settings.max_stream_data_bidi_local;

        } else {
            return conn->local_settings.max_stream_data_bidi_remote;
        }

    } else if (stream->stream_type == RQC_CLI_BID) {
        if (conn->conn_type == RQC_CONN_TYPE_SERVER) {
            return conn->local_settings.max_stream_data_bidi_remote;

        } else {
            return conn->local_settings.max_stream_data_bidi_local;
        }

    } else {
        return conn->local_settings.max_stream_data_uni;
    }
}

int
rqc_stream_do_send_flow_ctl(rqc_stream_t *stream)
{
    int ret = RQC_OK;

    /* connection level */
    if (stream->stream_conn->conn_flow_ctl.fc_data_sent + stream->stream_conn->pkt_out_size > stream->stream_conn->conn_flow_ctl.fc_max_data_can_send) {
        rqc_log(stream->stream_conn->log, RQC_LOG_INFO, "|rqc_stream_send|exceed max_data:%ui|",
                stream->stream_conn->conn_flow_ctl.fc_max_data_can_send);

        stream->stream_conn->conn_flag |= RQC_CONN_FLAG_DATA_BLOCKED;
        rqc_write_data_blocked_to_packet(stream->stream_conn, stream->stream_conn->conn_flow_ctl.fc_max_data_can_send);
        ret = -RQC_ECONN_BLOCKED;
    }

    /* stream level */
    if (stream->stream_send_offset + stream->stream_conn->pkt_out_size > stream->stream_flow_ctl.fc_max_stream_data_can_send) {
        rqc_log(stream->stream_conn->log, RQC_LOG_INFO, "|rqc_stream_send|exceed max_stream_data:%ui|",
                stream->stream_flow_ctl.fc_max_stream_data_can_send);

        stream->stream_flag |= RQC_STREAM_FLAG_DATA_BLOCKED;
        rqc_write_stream_data_blocked_to_packet(stream->stream_conn, stream->stream_id,
                                                stream->stream_flow_ctl.fc_max_stream_data_can_send);
        ret = -RQC_ESTREAM_BLOCKED;
    }
    return ret;
}

int
rqc_stream_do_recv_flow_ctl(rqc_stream_t *stream)
{
    rqc_connection_t *conn = stream->stream_conn;
    rqc_usec_t now = rqc_monotonic_timestamp();

    /* increase recv window */
    rqc_usec_t min_srtt = rqc_conn_get_min_srtt(conn, 0);
    rqc_usec_t max_srtt = 0;

    /* stream level */
    uint64_t available_window = stream->stream_flow_ctl.fc_max_stream_data_can_recv - stream->stream_data_in.next_read_offset;

    if (available_window < stream->stream_flow_ctl.fc_stream_recv_window_size / 2) {

        if (!stream->recv_rate_bytes_per_sec) {
            if (stream->stream_flow_ctl.fc_last_window_update_time
                && (now - stream->stream_flow_ctl.fc_last_window_update_time < 2 * min_srtt))
            {
                stream->stream_flow_ctl.fc_stream_recv_window_size
                        = rqc_min(stream->stream_flow_ctl.fc_stream_recv_window_size * 2, RQC_MAX_RECV_WINDOW);
            }

        } else {

            if (!max_srtt) {
                max_srtt = rqc_conn_get_max_srtt(conn);
            }

            stream->stream_flow_ctl.fc_stream_recv_window_size = stream->recv_rate_bytes_per_sec * max_srtt / 1000000;
            stream->stream_flow_ctl.fc_stream_recv_window_size = rqc_max(conn->conn_settings.init_recv_window, stream->stream_flow_ctl.fc_stream_recv_window_size);
            stream->stream_flow_ctl.fc_stream_recv_window_size = rqc_min(RQC_MAX_RECV_WINDOW, stream->stream_flow_ctl.fc_stream_recv_window_size);
        }

        stream->stream_flow_ctl.fc_last_window_update_time = now;

        if (stream->stream_flow_ctl.fc_stream_recv_window_size > available_window) {
            stream->stream_flow_ctl.fc_max_stream_data_can_recv += (stream->stream_flow_ctl.fc_stream_recv_window_size - available_window);
            rqc_write_max_stream_data_to_packet(conn, stream->stream_id, stream->stream_flow_ctl.fc_max_stream_data_can_recv, RQC_PTYPE_SHORT_HEADER);
        }
    }

    /* connection level */
    available_window = conn->conn_flow_ctl.fc_max_data_can_recv - conn->conn_flow_ctl.fc_data_read;

    if (available_window < conn->conn_flow_ctl.fc_recv_windows_size / 2) {

        if (!conn->conn_settings.recv_rate_bytes_per_sec) {

            if (conn->conn_flow_ctl.fc_last_window_update_time
                && (now - conn->conn_flow_ctl.fc_last_window_update_time < 2 * min_srtt))
            {
                conn->conn_flow_ctl.fc_recv_windows_size
                        = rqc_min(conn->conn_flow_ctl.fc_recv_windows_size * 2, RQC_MAX_RECV_WINDOW);
            }

            if (conn->conn_flow_ctl.fc_recv_windows_size < 1.5 * stream->stream_flow_ctl.fc_stream_recv_window_size) {
                conn->conn_flow_ctl.fc_recv_windows_size = (uint64_t)(1.5 * stream->stream_flow_ctl.fc_stream_recv_window_size);
            }

        } else {

            if (!max_srtt) {
                max_srtt = rqc_conn_get_max_srtt(conn);
            }

            conn->conn_flow_ctl.fc_recv_windows_size = conn->conn_settings.recv_rate_bytes_per_sec * max_srtt / 1000000;
            conn->conn_flow_ctl.fc_recv_windows_size = rqc_max(RQC_MIN_RECV_WINDOW, conn->conn_flow_ctl.fc_recv_windows_size);
            conn->conn_flow_ctl.fc_recv_windows_size = rqc_min(RQC_MAX_RECV_WINDOW, conn->conn_flow_ctl.fc_recv_windows_size);
        }

        conn->conn_flow_ctl.fc_last_window_update_time = now;

        if (conn->conn_flow_ctl.fc_recv_windows_size > available_window) {
            conn->conn_flow_ctl.fc_max_data_can_recv += (conn->conn_flow_ctl.fc_recv_windows_size - available_window);
            rqc_write_max_data_to_packet(conn, conn->conn_flow_ctl.fc_max_data_can_recv);
        }
    }

    return RQC_OK;
}

int
rqc_stream_do_create_flow_ctl(rqc_connection_t *conn, rqc_stream_id_t stream_id, rqc_stream_type_t stream_type)
{
    if (stream_id == RQC_UNDEFINE_STREAM_ID) { /* sending part */
        if (stream_type == RQC_CLI_BID || stream_type == RQC_SVR_BID) {
            if (conn->cur_stream_id_bidi_local >= conn->conn_flow_ctl.fc_max_streams_bidi_can_send) {
                rqc_log(conn->log, RQC_LOG_ERROR, "|exceed max_streams_bidi_can_send:%ui|",
                        conn->conn_flow_ctl.fc_max_streams_bidi_can_send);
                rqc_write_streams_blocked_to_packet(conn, conn->conn_flow_ctl.fc_max_streams_bidi_can_send, 1);
                return -RQC_EPROTO;
            }

        } else {
            if (conn->cur_stream_id_uni_local >= conn->conn_flow_ctl.fc_max_streams_uni_can_send) {
                rqc_log(conn->log, RQC_LOG_ERROR, "|exceed max_streams_uni_can_send:%ui|",
                        conn->conn_flow_ctl.fc_max_streams_uni_can_send);
                rqc_write_streams_blocked_to_packet(conn, conn->conn_flow_ctl.fc_max_streams_uni_can_send, 0);
                return -RQC_EPROTO;
            }
        }

    } else { /* receiving part */
        stream_type = rqc_get_stream_type(stream_id);
        if (stream_type == RQC_CLI_BID || stream_type == RQC_SVR_BID) {
            if (stream_id >= 4 * conn->conn_flow_ctl.fc_max_streams_bidi_can_recv + stream_type) {
                rqc_log(conn->log, RQC_LOG_ERROR, "|exceed max_streams_bidi_can_recv:%ui|",
                        conn->conn_flow_ctl.fc_max_streams_bidi_can_recv);
                RQC_CONN_ERR(conn, TRA_STREAM_LIMIT_ERROR);
                return -RQC_EPROTO;
            }
            /* increase max streams */
            if ((stream_id >> 2) >= conn->conn_flow_ctl.fc_max_streams_bidi_can_recv / 2) {
                conn->conn_flow_ctl.fc_max_streams_bidi_can_recv += conn->local_settings.max_streams_bidi;
                rqc_write_max_streams_to_packet(conn, conn->conn_flow_ctl.fc_max_streams_bidi_can_recv, 1);
            }

        } else {
            if (stream_id >= 4 * conn->conn_flow_ctl.fc_max_streams_uni_can_recv + stream_type) {
                rqc_log(conn->log, RQC_LOG_ERROR, "|exceed max_streams_uni_can_recv:%ui|",
                        conn->conn_flow_ctl.fc_max_streams_uni_can_recv);
                RQC_CONN_ERR(conn, TRA_STREAM_LIMIT_ERROR);
                return -RQC_EPROTO;
            }
            /* increase max streams */
            if ((stream_id >> 2) >= conn->conn_flow_ctl.fc_max_streams_uni_can_recv / 2) {
                conn->conn_flow_ctl.fc_max_streams_uni_can_recv += conn->local_settings.max_streams_uni;
                rqc_write_max_streams_to_packet(conn, conn->conn_flow_ctl.fc_max_streams_uni_can_recv, 0);
            }
        }
    }
    return RQC_OK;
}

rqc_stream_t *
rqc_stream_create(rqc_engine_t *engine, const rqc_cid_t *cid, rqc_stream_settings_t *settings,
    void *user_data)
{
    rqc_connection_t *conn;
    rqc_stream_t *stream;

    conn = rqc_engine_conns_hash_find(engine, cid, 's');
    if (!conn) {
        rqc_log(engine->log, RQC_LOG_ERROR, "|can not find connection|cid:%s",
                rqc_scid_str(engine, cid));
        return NULL;
    }

    stream = rqc_create_stream_with_conn(conn, RQC_UNDEFINE_STREAM_ID, RQC_CLI_BID, settings, user_data);
    if (!stream) {
        rqc_log(engine->log, RQC_LOG_ERROR, "|rqc_create_stream_with_conn error|");
        return NULL;
    }

    conn->cli_bidi_streams++;

    return stream;
}

rqc_stream_t *
rqc_stream_create_with_direction(rqc_connection_t *conn,
    rqc_stream_direction_t dir, void *user_data)
{
    rqc_stream_type_t type;

    /* get stream type */
    if (RQC_CONN_TYPE_CLIENT == rqc_conn_get_type(conn)) {
        if (RQC_STREAM_BIDI == dir) {
            type = RQC_CLI_BID;

        } else {
            type = RQC_CLI_UNI;
        }

    } else {
        if (RQC_STREAM_BIDI == dir) {
            type = RQC_SVR_BID;

        } else {
            type = RQC_SVR_UNI;
        }
    }

    /* create stream */
    return rqc_create_stream_with_conn(conn, RQC_UNDEFINE_STREAM_ID, type,
                                       NULL, user_data);
}

rqc_stream_t *
rqc_create_stream_with_conn(rqc_connection_t *conn, rqc_stream_id_t stream_id,
    rqc_stream_type_t stream_type, rqc_stream_settings_t *settings, void *user_data)
{
    rqc_int_t   ret;

    if (conn->conn_state >= RQC_CONN_STATE_CLOSING) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|conn closing, cannot create stream|type:%d|conn_state:%s|flag:%s|",
                conn->conn_type, rqc_conn_state_2_str(conn->conn_state), rqc_conn_flag_2_str(conn, conn->conn_flag));
        return NULL;
    }

    if (rqc_stream_do_create_flow_ctl(conn, stream_id, stream_type)) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_stream_do_create_flow_ctl error|");
        return NULL;
    }

    rqc_stream_t *stream = rqc_calloc(1, sizeof(rqc_stream_t));
    if (stream == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_calloc error|");
        return NULL;
    }
    rqc_list_add_tail(&stream->all_stream_list, &conn->conn_all_streams);

    stream->stream_conn = conn;
    stream->stream_if = &conn->app_proto_cbs.stream_cbs;
    stream->user_data = user_data;
    stream->stream_state_send = RQC_SEND_STREAM_ST_READY;
    stream->stream_state_recv = RQC_RECV_STREAM_ST_RECV;

    stream->stream_refcnt = 0;
    rqc_memset(&stream->stream_stats, 0, sizeof(stream->stream_stats));
    stream->stream_stats.create_time = rqc_monotonic_timestamp();

    rqc_stream_set_flow_ctl(stream);

    rqc_init_list_head(&stream->stream_data_in.frames_tailq);

    if (stream_id == RQC_UNDEFINE_STREAM_ID) {
        stream->stream_type = stream_type;
        stream->stream_id = rqc_gen_stream_id(conn, stream->stream_type);

    } else {
        stream->stream_id = stream_id;
        stream->stream_type = rqc_get_stream_type(stream_id);
    }

    rqc_id_hash_element_t e = {stream->stream_id, stream};
    if (rqc_id_hash_add(conn->streams_hash, e)) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_id_hash_add error|");
        goto error;
    }

    /* newly initiated stream is writable */
    if (stream_id == RQC_UNDEFINE_STREAM_ID) {
        rqc_stream_ready_to_write(stream);
    }

    stream->recv_rate_bytes_per_sec = 0;

    if (settings) {
        stream->stream_priority = settings->stream_priority;
        if (conn->conn_settings.enable_stream_rate_limit
            && stream->stream_type == RQC_CLI_BID)
        {
            stream->recv_rate_bytes_per_sec = settings->recv_rate_bytes_per_sec;
        }
    }

    if (stream->stream_if->stream_create_notify) {
        ret = stream->stream_if->stream_create_notify(stream, stream->user_data);
        if (RQC_OK != ret) {
            rqc_log(conn->log, RQC_LOG_WARN, "|stream create notify error|"
                    "|stream_id:%ui", stream->stream_id);
            stream->stream_flag |= RQC_STREAM_FLAG_DISCARDED;
        }
    }

    rqc_log(conn->log, RQC_LOG_INFO, "|success|flag:%d|stream_id:%ui|conn:%p",
            stream->stream_flag, stream->stream_id, stream->stream_conn);

    return stream;

error:

    rqc_destroy_stream(stream);
    return NULL;
}

void
rqc_stream_set_user_data(rqc_stream_t *stream, void *user_data)
{
    stream->user_data = user_data;
}

void *
rqc_get_conn_user_data_by_stream(rqc_stream_t *stream)
{
    return stream->stream_conn->user_data;
}

void *
rqc_get_conn_alp_user_data_by_stream(rqc_stream_t *stream)
{
    return stream->stream_conn->proto_data;
}

rqc_stream_id_t
rqc_stream_id(rqc_stream_t *stream)
{
    return stream->stream_id;
}

rqc_usec_t
rqc_update_avg_time(rqc_usec_t new_time, rqc_usec_t ori_time, rqc_int_t ori_time_num)
{
    return (rqc_usec_t)(1.0 * new_time / (ori_time_num + 1) + 1.0 * ori_time / (ori_time_num + 1) * ori_time_num);
}

rqc_bool_t
rqc_is_stream_finished(rqc_stream_t *stream)
{
    rqc_bool_t check_send_state, check_recv_state;
    rqc_conn_type_t conn_type = stream->stream_conn->conn_type;

    if (stream->stream_type == RQC_CLI_BID || stream->stream_type == RQC_SVR_BID) {
        check_send_state = RQC_TRUE;
        check_recv_state = RQC_TRUE;
    } else if ((stream->stream_type == RQC_CLI_UNI && conn_type == RQC_CONN_TYPE_CLIENT)
               || (stream->stream_type == RQC_SVR_UNI && conn_type == RQC_CONN_TYPE_SERVER)) {
        /* Uni Stream Send side */
        check_send_state = RQC_TRUE;
        check_recv_state = RQC_FALSE;
    } else {
        /* Uni Stream Recv side */
        check_send_state = RQC_FALSE;
        check_recv_state = RQC_TRUE;
    }

    if (check_send_state && check_recv_state) {
        return (stream->stream_state_send == RQC_SEND_STREAM_ST_DATA_RECVD)
                && (stream->stream_state_recv == RQC_RECV_STREAM_ST_DATA_READ);
    } else if (check_send_state) {
        return stream->stream_state_send == RQC_SEND_STREAM_ST_DATA_RECVD;
    } else {
        return stream->stream_state_recv == RQC_RECV_STREAM_ST_DATA_READ;
    }
}

void
rqc_destroy_stream(rqc_stream_t *stream)
{
    rqc_log(stream->stream_conn->log, RQC_LOG_INFO, "|send_state:%d|recv_state:%d|stream_id:%ui|stream_type:%d|",
            stream->stream_state_send, stream->stream_state_recv, stream->stream_id, stream->stream_type);

    if (rqc_is_stream_finished(stream)) {
        if(stream->stream_conn) {
            stream->stream_conn->finished_streams++;
        }
    }

    if (stream->stream_if->stream_close_notify
        && !(stream->stream_flag & RQC_STREAM_FLAG_DISCARDED))
    {
        stream->stream_if->stream_close_notify(stream, stream->user_data);
    }

    rqc_list_del_init(&stream->all_stream_list);

    rqc_destroy_frame_list(&stream->stream_data_in.frames_tailq);

    int ret = rqc_id_hash_delete(stream->stream_conn->streams_hash, stream->stream_id);
    if (ret != RQC_OK) {
        rqc_log(stream->stream_conn->log, RQC_LOG_ERROR, "|delete stream error|conn:%p|stream_id:%ui|ret:%d|",
                stream->stream_conn, stream->stream_id, ret);
    }

    if (rqc_id_hash_delete(stream->stream_conn->passive_streams_hash, stream->stream_id) == RQC_ID_HASH_LOOP) {
        rqc_id_hash_table_t* hash_tab = stream->stream_conn->passive_streams_hash;
        rqc_id_hash_node_t* node = hash_tab->list[stream->stream_id % hash_tab->count];
        rqc_log(stream->stream_conn->log, RQC_LOG_ERROR, "|stream_id:%ui|hash:%ui|value:%p|node:%p|next:%p|",
                stream->stream_id, node->element.hash, node->element.value, node, node->next);
    }

    rqc_stream_shutdown_write(stream);
    rqc_stream_shutdown_read(stream);

    stream->stream_flag |= RQC_STREAM_FLAG_CLOSED;

    if (stream->stream_conn->log->log_level >= RQC_LOG_STATS) {
#define __calc_delay(a, b) ((a && b && (a > b))? (a) - (b) : 0)
        rqc_log(stream->stream_conn->log, RQC_LOG_STATS,
            "|err:0x%xi|close_msg:%s|"
            "send_state:%d|recv_state:%d|stream_id:%ui|stream_type:%d|"
            "send_bytes:%ui|read_bytes:%ui|recv_bytes:%ui|stream_len:%ui|"
            "create_time:%ui|wrt_delay:%ui|"
            "snd_delay:%ui|finwrt_delay:%ui|finsnd_delay:%ui|"
            "finrcv_delay:%ui|finread_delay:%ui|all_acked_delay:%ui|"
            "firstfinack_dely:%ui|close_delay:%ui|"
            "apprst_delay:%ui|rstsnd_delay:%ui|rstrcv_delay:%ui|"
            "%s|retrans:%d|",
            stream->stream_err, stream->stream_close_msg ? stream->stream_close_msg : "",
            stream->stream_state_send, stream->stream_state_recv,
            stream->stream_id, stream->stream_type,
            stream->stream_send_offset,
            stream->stream_data_in.next_read_offset,
            stream->stream_data_in.merged_offset_end,
            stream->stream_data_in.stream_length,
            stream->stream_stats.create_time,
            __calc_delay(stream->stream_stats.first_write_time, stream->stream_stats.create_time),
            __calc_delay(stream->stream_stats.first_snd_time, stream->stream_stats.create_time),
            __calc_delay(stream->stream_stats.local_fin_write_time, stream->stream_stats.create_time),
            __calc_delay(stream->stream_stats.local_fin_snd_time, stream->stream_stats.create_time),
            __calc_delay(stream->stream_stats.peer_fin_rcv_time, stream->stream_stats.create_time),
            __calc_delay(stream->stream_stats.peer_fin_read_time, stream->stream_stats.create_time),
            __calc_delay(stream->stream_stats.all_data_acked_time, stream->stream_stats.create_time),
            __calc_delay(stream->stream_stats.first_fin_ack_time, stream->stream_stats.create_time),
            __calc_delay(stream->stream_stats.close_time, stream->stream_stats.create_time),
            __calc_delay(stream->stream_stats.app_reset_time, stream->stream_stats.create_time),
            __calc_delay(stream->stream_stats.local_reset_time, stream->stream_stats.create_time),
            __calc_delay(stream->stream_stats.peer_reset_time, stream->stream_stats.create_time),
            rqc_conn_addr_str(stream->stream_conn), stream->stream_stats.retrans_pkt_cnt
        );
#undef __calc_delay
    }

    rqc_free(stream);
}

rqc_int_t
rqc_stream_close(rqc_stream_t *stream)
{
    rqc_int_t ret;
    rqc_connection_t *conn = stream->stream_conn;
    rqc_log(conn->log, RQC_LOG_INFO, "|stream_id:%ui|stream_state_send:%d|stream_state_recv:%d|conn:%p|conn_state:%s|flag:%s|",
            stream->stream_id, stream->stream_state_send, stream->stream_state_recv, conn,
            rqc_conn_state_2_str(conn->conn_state), rqc_conn_flag_2_str(conn, conn->conn_flag));

    RQC_STREAM_CLOSE_MSG(stream, "local reset");

    if (stream->stream_state_send >= RQC_SEND_STREAM_ST_RESET_SENT) {
        return RQC_OK;
    }
    if (conn->conn_state >= RQC_CONN_STATE_CLOSING) {
        return RQC_OK;
    }

    rqc_send_queue_drop_stream_frame_packets(conn, stream->stream_id);
    ret = rqc_write_reset_stream_to_packet(conn, stream, REQUEST_CANCELLED, stream->stream_send_offset);
    if (ret < 0) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_reset_stream_to_packet error|%d|", ret);
        RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
    }

    /* A STOP_SENDING frame can be sent for streams in the "Recv" or "Size
       Known" states */
    if (stream->stream_state_recv == RQC_RECV_STREAM_ST_RECV
        || stream->stream_state_recv == RQC_RECV_STREAM_ST_SIZE_KNOWN)
    {
        ret = rqc_write_stop_sending_to_packet(conn, stream, REQUEST_CANCELLED);
        if (ret < 0) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_stop_sending_to_packet error|%d|", ret);
            RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
        }
    }

    stream->stream_stats.max_pto_backoff = rqc_max(stream->stream_stats.max_pto_backoff, rqc_conn_get_max_pto_backoff(conn, 1));

    rqc_engine_remove_wakeup_queue(conn->engine, conn);
    rqc_engine_add_active_queue(conn->engine, conn);

    rqc_stream_shutdown_write(stream);
    rqc_engine_conn_logic(conn->engine, conn);
    return RQC_OK;
}

rqc_stream_stats_t
rqc_stream_get_stats(rqc_stream_t *stream)
{
    rqc_stream_stats_t stats;
    rqc_memzero(&stats, sizeof(stats));
    stats.stream_type          = stream->stream_type;
    stats.stream_err           = stream->stream_err;
    stats.send_bytes           = stream->stream_send_offset;
    stats.recv_bytes           = stream->stream_data_in.merged_offset_end;
    stats.read_bytes           = stream->stream_data_in.next_read_offset;
    stats.sent_pkt_cnt         = stream->stream_stats.sent_pkt_cnt;
    stats.retrans_pkt_cnt      = stream->stream_stats.retrans_pkt_cnt;
    stats.create_time          = stream->stream_stats.create_time;
    stats.close_time           = stream->stream_stats.close_time;
    stats.first_write_time     = stream->stream_stats.first_write_time;
    stats.first_snd_time       = stream->stream_stats.first_snd_time;
    stats.first_rcv_time       = stream->stream_stats.first_rcv_time;
    stats.local_fin_write_time = stream->stream_stats.local_fin_write_time;
    stats.local_fin_snd_time   = stream->stream_stats.local_fin_snd_time;
    stats.peer_fin_rcv_time    = stream->stream_stats.peer_fin_rcv_time;
    stats.peer_fin_read_time   = stream->stream_stats.peer_fin_read_time;
    stats.all_data_acked_time  = stream->stream_stats.all_data_acked_time;
    stats.app_reset_time       = stream->stream_stats.app_reset_time;
    stats.local_reset_time     = stream->stream_stats.local_reset_time;
    stats.peer_reset_time      = stream->stream_stats.peer_reset_time;
    return stats;
}

rqc_int_t
rqc_insert_passive_stream_hash(rqc_connection_t *conn, int64_t cur_max_sid, rqc_stream_id_t stream_id)
{
    rqc_stream_type_t type = rqc_get_stream_type(stream_id);
    for (int64_t sid = cur_max_sid + 1; sid <= (stream_id >> 2u); ++sid) {
        rqc_id_hash_element_t e = {(uint64_t)sid << 2u | type, conn};
        if (rqc_id_hash_add(conn->passive_streams_hash, e)) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_id_hash_add error|stream_id:%ui|", stream_id);
            RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
        }
    }
    return RQC_OK;
}

rqc_stream_t *
rqc_passive_create_stream(rqc_connection_t *conn, rqc_stream_id_t stream_id, void *user_data)
{
    if (rqc_stream_do_create_flow_ctl(conn, stream_id, rqc_get_stream_type(stream_id)) != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_stream_do_create_flow_ctl error|");
        return NULL;
    }

    int64_t sid = stream_id >> 2u;
    if (rqc_stream_is_bidi(stream_id) && sid > conn->max_stream_id_bidi_remote) {
        rqc_insert_passive_stream_hash(conn, conn->max_stream_id_bidi_remote, stream_id);
        conn->max_stream_id_bidi_remote = sid;

    } else if (!rqc_stream_is_bidi(stream_id) && sid > conn->max_stream_id_uni_remote) {
        rqc_insert_passive_stream_hash(conn, conn->max_stream_id_uni_remote, stream_id);
        conn->max_stream_id_uni_remote = sid;

    } else {
        if (!rqc_id_hash_find(conn->passive_streams_hash, stream_id)) {
            /* already closed */
            rqc_log(conn->log, RQC_LOG_WARN, "|stream already closed|stream_id:%ui|", stream_id);
            return NULL;
        }
    }

    rqc_stream_t *stream = rqc_create_stream_with_conn(conn, stream_id, 0, NULL, user_data);
    if (stream == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_create_stream_with_conn error|stream_id:%ui|", stream_id);
        RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
        return NULL;
    }

    rqc_stream_type_t stream_type;
    stream_type = rqc_get_stream_type(stream_id);

    if (stream_type == RQC_CLI_BID) {
        conn->cli_bidi_streams++;

    } else if (stream_type == RQC_SVR_BID) {
        conn->svr_bidi_streams++;
    }

    return stream;
}

rqc_int_t
rqc_stream_update_settings(rqc_stream_t *stream,
    rqc_stream_settings_t *settings)
{
    rqc_connection_t *conn = NULL;
    rqc_usec_t max_srtt = 0;
    uint64_t new_offset = 0;

    if (stream && settings
        && settings->stream_priority)
    {
        stream->stream_priority = settings->stream_priority;
    }

    if (stream && settings
        && settings->recv_rate_bytes_per_sec)
    {
        conn = stream->stream_conn;
        if (conn->conn_settings.enable_stream_rate_limit) {
            stream->recv_rate_bytes_per_sec = settings->recv_rate_bytes_per_sec;
            max_srtt = rqc_conn_get_max_srtt(conn);
            stream->stream_flow_ctl.fc_stream_recv_window_size = stream->recv_rate_bytes_per_sec * max_srtt / 1000000;
            stream->stream_flow_ctl.fc_stream_recv_window_size = rqc_max(conn->conn_settings.init_recv_window, stream->stream_flow_ctl.fc_stream_recv_window_size);
            stream->stream_flow_ctl.fc_stream_recv_window_size = rqc_min(RQC_MAX_RECV_WINDOW, stream->stream_flow_ctl.fc_stream_recv_window_size);
            new_offset = stream->stream_data_in.next_read_offset + stream->stream_flow_ctl.fc_stream_recv_window_size;

            if(new_offset > stream->stream_flow_ctl.fc_max_stream_data_can_recv) {
                stream->stream_flow_ctl.fc_max_stream_data_can_recv = new_offset;
                rqc_write_max_stream_data_to_packet(conn, stream->stream_id, stream->stream_flow_ctl.fc_max_stream_data_can_recv, RQC_PTYPE_NUM);
            }
        }
    }

    return RQC_OK;
}

ssize_t
rqc_stream_recv(rqc_stream_t *stream, unsigned char *recv_buf, size_t recv_buf_size, uint8_t *fin)
{
    rqc_list_head_t *pos, *next;
    rqc_stream_frame_t *stream_frame = NULL;
    size_t read = 0;
    size_t frame_left;
    *fin = 0;

    if (stream->stream_state_recv >= RQC_RECV_STREAM_ST_RESET_RECVD) {
        stream->stream_state_recv = RQC_RECV_STREAM_ST_RESET_READ;
        rqc_stream_shutdown_read(stream);
        rqc_stream_maybe_need_close(stream);
        return -RQC_ESTREAM_RESET;
    }

    rqc_list_for_each_safe(pos, next, &stream->stream_data_in.frames_tailq) {
        stream_frame = rqc_list_entry(pos, rqc_stream_frame_t, sf_list);

        if (stream_frame->data_offset > stream->stream_data_in.merged_offset_end) {
            break;
        }

        if (read >= recv_buf_size) {
            break;
        }
        /*
         *     |------------------------|
         *        |----------|
         */

        /* already read */
        if (stream_frame->data_offset + stream_frame->data_length < stream->stream_data_in.next_read_offset) {
            /* free frame */
            rqc_list_del_init(&stream_frame->sf_list);
            rqc_free(stream_frame->data);
            rqc_free(stream_frame);
            continue;
        }

        /*
         *        |----------|
         *             |-------|
         */
        if (stream_frame->data_offset < stream->stream_data_in.next_read_offset) {
            uint64_t offset = stream->stream_data_in.next_read_offset - stream_frame->data_offset;
            stream_frame->next_read_offset = rqc_max(stream_frame->next_read_offset, offset);
        }

        frame_left = stream_frame->data_length - stream_frame->next_read_offset;

        if (read + frame_left <= recv_buf_size) {
            memcpy(recv_buf + read, stream_frame->data + stream_frame->next_read_offset, frame_left);
            stream->stream_data_in.next_read_offset += frame_left;
            stream_frame->next_read_offset = stream_frame->data_length;
            read += frame_left;
            /* free frame */
            rqc_list_del_init(&stream_frame->sf_list);
            rqc_free(stream_frame->data);
            rqc_free(stream_frame);

        } else {
            memcpy(recv_buf + read, stream_frame->data + stream_frame->next_read_offset, recv_buf_size - read);
            stream_frame->next_read_offset += recv_buf_size - read;
            stream->stream_data_in.next_read_offset += recv_buf_size - read;
            read = recv_buf_size;
            break;
        }

    }

    if (stream->stream_data_in.stream_determined
        && stream->stream_data_in.next_read_offset == stream->stream_data_in.stream_length)
    {
        *fin = 1;
        stream->stream_stats.peer_fin_read_time = rqc_monotonic_timestamp();
        if (stream->stream_state_recv == RQC_RECV_STREAM_ST_DATA_RECVD) {
            rqc_stream_recv_state_update(stream, RQC_RECV_STREAM_ST_DATA_READ);
            rqc_stream_maybe_need_close(stream);
        }
    }

    stream->stream_conn->conn_flow_ctl.fc_data_read += read;

    rqc_log_event(stream->stream_conn->log, TRA_STREAM_DATA_MOVED, stream, 1, read,
                  recv_buf_size, *fin, 0, 0, 0, 0);
    rqc_stream_shutdown_read(stream);

    int ret = rqc_stream_do_recv_flow_ctl(stream);
    if (ret) {
        rqc_log(stream->stream_conn->log, RQC_LOG_ERROR, "|rqc_stream_do_recv_flow_ctl error|stream_id:%ui|", stream->stream_id);
        return ret;
    }

    return (read == 0 && *fin == 0) ? -RQC_EAGAIN : read;
}

ssize_t
rqc_stream_send(rqc_stream_t *stream, unsigned char *send_data, size_t send_data_size, uint8_t fin)
{
    rqc_connection_t *conn = stream->stream_conn;
    if (conn->conn_state >= RQC_CONN_STATE_CLOSING) {
        rqc_conn_log(conn, RQC_LOG_INFO, "|conn closing, cannot send|stream_id:%ui|", stream->stream_id);
        rqc_stream_shutdown_write(stream);
        return -RQC_CLOSING;
    }
    if (stream->stream_state_send >= RQC_SEND_STREAM_ST_RESET_SENT) {
        rqc_conn_log(conn, RQC_LOG_INFO, "|stream reset sent, cannot send|stream_id:%ui|", stream->stream_id);
        rqc_stream_shutdown_write(stream);
        return -RQC_ESTREAM_RESET;
    }
    if (stream->stream_flag & RQC_STREAM_FLAG_FIN_WRITE) {
        rqc_conn_log(conn, RQC_LOG_WARN, "|fin write, cannot send|stream_id:%ui|", stream->stream_id);
        rqc_stream_shutdown_write(stream);
        return 0;
    }
    int ret;
    rqc_stream_ready_to_write(stream);
    size_t send_data_written = 0;
    size_t offset = 0; /* the written offset in send_data */
    uint8_t fin_only = fin && !send_data_size;
    uint8_t fin_only_done = 0;
    rqc_pkt_type_t pkt_type = RQC_PTYPE_SHORT_HEADER;
    int check_app_limit = 1;

    if (!rqc_conn_is_established(conn)) {
        ret = -RQC_EAGAIN;
        goto do_buff;
    }

    while (offset < send_data_size || fin_only) {

        if (pkt_type == RQC_PTYPE_SHORT_HEADER) {
            ret = rqc_stream_do_send_flow_ctl(stream);
            if (ret) {
                ret = -RQC_EAGAIN;
                goto do_buff;
            }
        }

        if (!rqc_send_queue_can_write(conn->conn_send_queue)) {
            conn->conn_send_queue->sndq_full = RQC_TRUE;
            ret = -RQC_EAGAIN;
            goto do_buff;
        }

        if (check_app_limit) {
            rqc_conn_check_app_limit(conn);
            check_app_limit = 0;
        }

        ret = rqc_write_stream_frame_to_packet(conn, stream, pkt_type,
                                               fin,
                                               send_data + offset,
                                               send_data_size - offset,
                                               &send_data_written);
        if (ret) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_stream_frame_to_packet error|");
            RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
            return ret;
        }

        offset += send_data_written;
        if (fin_only) {
            fin_only_done = 1;
            break;
        }
    }

    rqc_stream_shutdown_write(stream);

do_buff:
    if ((!conn->first_data_send_time) && ((stream->stream_type == RQC_CLI_BID) || (stream->stream_type == RQC_SVR_BID))) {
        conn->first_data_send_time = rqc_monotonic_timestamp();
    }

    rqc_log_event(conn->log, TRA_STREAM_DATA_MOVED, stream, 0, send_data_size, 0, fin, ret, pkt_type, 0, offset);

    rqc_engine_remove_wakeup_queue(conn->engine, conn);
    rqc_engine_add_active_queue(conn->engine, conn);

    /* update max_pto stats */
    stream->stream_stats.max_pto_backoff = rqc_max(stream->stream_stats.max_pto_backoff, rqc_conn_get_max_pto_backoff(conn, 1));

    /* application layer call the main logic */
    rqc_engine_conn_logic(conn->engine, conn);

    if (offset == 0 && !fin_only_done) {
        if (ret == -RQC_EAGAIN) {
            return -RQC_EAGAIN; /* -RQC_EAGAIN not means error */
        } else {
            RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
            return ret;
        }
    }
    return offset;
}

void
rqc_process_write_streams(rqc_connection_t *conn)
{
    rqc_int_t ret;
    rqc_stream_t *stream;
    rqc_list_head_t *pos, *next;

    rqc_list_for_each_safe(pos, next, &conn->conn_write_streams) {
        stream = rqc_list_entry(pos, rqc_stream_t, write_stream_list);
        if (stream->stream_flag & RQC_STREAM_FLAG_DATA_BLOCKED
            || conn->conn_flag & RQC_CONN_FLAG_DATA_BLOCKED)
        {
            continue;
        }
        if (stream->stream_if->stream_write_notify == NULL) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|stream_write_notify is NULL|flag:%d|stream_id:%ui|conn:%p|",
                    stream->stream_flag, stream->stream_id, stream->stream_conn);
            RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
            return;
        }
        ret = stream->stream_if->stream_write_notify(stream, stream->user_data);
        if (ret < 0) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|stream_write_notify err:%d|flag:%d|stream_id:%ui|conn:%p|",
                    ret, stream->stream_flag, stream->stream_id, stream->stream_conn);
            RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
        }
    }
}

void
rqc_process_read_streams(rqc_connection_t *conn)
{
    rqc_int_t ret;
    rqc_stream_t *stream;
    rqc_list_head_t *pos, *next;

    rqc_list_for_each_safe(pos, next, &conn->conn_read_streams) {
        if (pos->next == pos) {
            rqc_log(conn->log, RQC_LOG_FATAL, "|pos:%p|conn:%p|",
                     pos, conn);
            RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
            return;
        }
        stream = rqc_list_entry(pos, rqc_stream_t, read_stream_list);
        if (stream->stream_if->stream_read_notify == NULL) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|stream_read_notify is NULL|flag:%d|stream_id:%ui|conn:%p|",
                    stream->stream_flag, stream->stream_id, stream->stream_conn);
            RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
            return;
        }
        ret = stream->stream_if->stream_read_notify(stream, stream->user_data);
        if (ret < 0) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|stream_read_notify err:%d|flag:%d|stream_id:%ui|conn:%p|",
                    ret, stream->stream_flag, stream->stream_id, stream->stream_conn);
            RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
        }
    }
}

void
rqc_destroy_stream_frame(rqc_stream_frame_t *stream_frame)
{
    if (stream_frame) {
        if (stream_frame->data) {
            rqc_free(stream_frame->data);
        }

        rqc_free(stream_frame);
    }
}

void
rqc_destroy_frame_list(rqc_list_head_t *head)
{
    rqc_list_head_t *pos, *next;
    rqc_stream_frame_t *stream_frame;
    rqc_list_for_each_safe(pos, next, head) {
        stream_frame = rqc_list_entry(pos, rqc_stream_frame_t, sf_list);
        rqc_list_del_init(pos);
        rqc_destroy_stream_frame(stream_frame);
    }
}

/* used to count reference */
void
rqc_stream_refcnt_add(rqc_stream_t *stream)
{
    stream->stream_refcnt++;
}

void
rqc_stream_refcnt_del(rqc_stream_t *stream)
{
    stream->stream_refcnt--;
}

void
rqc_stream_send_state_update(rqc_stream_t *stream, rqc_send_stream_state_t state)
{
    rqc_log_event(stream->stream_conn->log, TRA_STREAM_STATE_UPDATED, stream, RQC_LOG_STREAM_SEND, state);
    stream->stream_state_send = state;
}

void
rqc_stream_recv_state_update(rqc_stream_t *stream, rqc_recv_stream_state_t state)
{
    rqc_log_event(stream->stream_conn->log, TRA_STREAM_STATE_UPDATED, stream, RQC_LOG_STREAM_RECV, state);
    stream->stream_state_recv = state;
}

rqc_stream_direction_t
rqc_stream_get_direction(rqc_stream_t *strm)
{
    return rqc_stream_is_uni(strm->stream_id)
        ? RQC_STREAM_UNI : RQC_STREAM_BIDI;
}

void
rqc_stream_closing(rqc_stream_t *stream, rqc_int_t err)
{
    if (stream->stream_if->stream_closing_notify) {
        stream->stream_if->stream_closing_notify(stream, err,
                                                 stream->user_data);
    }
}
