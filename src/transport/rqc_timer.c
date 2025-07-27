#include "src/transport/rqc_timer.h"
#include "src/transport/rqc_engine.h"
#include "src/transport/rqc_conn.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/transport/rqc_stream.h"
#include "src/transport/rqc_utils.h"

static const char * const timer_type_2_str[RQC_TIMER_N] = {

    /* path level (path->path_send_ctl->path_timer_manager->timer[RQC_TIMER_N])*/
    [RQC_TIMER_ACK]             = "ACK",
    [RQC_TIMER_LOSS_DETECTION]  = "LOSS_DETECTION",
    [RQC_TIMER_PACING]          = "PACING",
    [RQC_TIMER_NAT_REBINDING]   = "NAT_REBINDING",

    /* connection level (conn->conn_timer_manager->timer[RQC_TIMER_N]) */
    [RQC_TIMER_CONN_IDLE]       = "CONN_IDLE",
    [RQC_TIMER_CONN_DRAINING]   = "CONN_DRAINING",
    [RQC_TIMER_STREAM_CLOSE]    = "STREAM_CLOSE",
    [RQC_TIMER_PING]            = "PING",
    [RQC_TIMER_LINGER_CLOSE]    = "LINGER_CLOSE",
};

const char *
rqc_timer_type_2_str(rqc_timer_type_t timer_type)
{
    return timer_type_2_str[timer_type];
}

/* timer callbacks */
void
rqc_timer_ack_timeout(rqc_timer_type_t type, rqc_usec_t now, void *user_data)
{
    rqc_send_ctl_t *send_ctl = (rqc_send_ctl_t *)user_data;

    rqc_connection_t *conn = send_ctl->ctl_conn;
    send_ctl->ctl_path->path_flag |= RQC_PATH_FLAG_SHOULD_ACK;
    conn->ack_flag |= (1 << send_ctl->ctl_path->path_id);
}

/**
 * OnLossDetectionTimeout
 */
void
rqc_timer_loss_detection_timeout(rqc_timer_type_t type, rqc_usec_t now, void *user_data)
{
    rqc_send_ctl_t *send_ctl = (rqc_send_ctl_t *)user_data;

    rqc_path_ctx_t *path = send_ctl->ctl_path;
    rqc_connection_t *conn = send_ctl->ctl_conn;

    rqc_usec_t loss_time;
    loss_time = rqc_send_ctl_get_earliest_loss_time(send_ctl);
    if (loss_time != 0) {
        /* Time threshold loss Detection */
        rqc_send_ctl_detect_lost(send_ctl, conn->conn_send_queue, now);
        rqc_send_ctl_set_loss_detection_timer(send_ctl);
        return;
    }

    if (send_ctl->ctl_bytes_in_flight > 0) {
        /*
         * PTO. Send new data if available, else retransmit old data.
         * If neither is available, send a single PING frame
         */
        rqc_path_send_one_or_two_ack_elicit_pkts(path);
    }

    send_ctl->ctl_pto_count++;
    conn->max_pto_cnt = rqc_max(send_ctl->ctl_pto_count, conn->max_pto_cnt);
    rqc_send_ctl_set_loss_detection_timer(send_ctl);
}

void
rqc_timer_pacing_timeout(rqc_timer_type_t type, rqc_usec_t now, void *user_data)
{
    rqc_send_ctl_t *send_ctl = (rqc_send_ctl_t *)user_data;

    rqc_pacing_t *pacing = &send_ctl->ctl_pacing;
    rqc_pacing_on_timeout(pacing);
}

void
rqc_timer_nat_rebinding_timeout(rqc_timer_type_t type, rqc_usec_t now, void *user_data)
{
    rqc_send_ctl_t *send_ctl = (rqc_send_ctl_t *)user_data;
    rqc_path_ctx_t *path = send_ctl->ctl_path;

    path->rebinding_addrlen = 0;
    path->rebinding_check_response = 0;
}

void
rqc_timer_conn_idle_timeout(rqc_timer_type_t type, rqc_usec_t now, void *user_data)
{
    rqc_connection_t *conn = (rqc_connection_t *)user_data;

    conn->conn_flag |= RQC_CONN_FLAG_TIME_OUT;

    RQC_CONN_CLOSE_MSG(conn, "idle timeout");
}

void
rqc_timer_conn_draining_timeout(rqc_timer_type_t type, rqc_usec_t now, void *user_data)
{
    rqc_connection_t *conn = (rqc_connection_t *)user_data;

    conn->conn_flag |= RQC_CONN_FLAG_TIME_OUT;
}

void
rqc_timer_stream_close_timeout(rqc_timer_type_t type, rqc_usec_t now, void *user_data)
{
    rqc_connection_t *conn = (rqc_connection_t *)user_data;

    rqc_list_head_t *pos, *next;
    rqc_stream_t *stream;
    rqc_usec_t min_expire = RQC_MAX_UINT64_VALUE, interval = 0;
    rqc_list_for_each_safe(pos, next, &conn->conn_closing_streams) {
        stream = rqc_list_entry(pos, rqc_stream_t, closing_stream_list);
        if (stream->stream_close_time <= now) {
            rqc_list_del_init(pos);
            RQC_STREAM_CLOSE_MSG(stream, "finished");
            rqc_destroy_stream(stream);

        } else {
            min_expire = rqc_min(min_expire, stream->stream_close_time);
        }
    }

    if (min_expire != RQC_MAX_UINT64_VALUE) {
        interval = (min_expire > now) ? (min_expire - now) : 0;
        rqc_timer_set(&conn->conn_timer_manager, RQC_TIMER_STREAM_CLOSE, now, interval);
    }
}

void
rqc_timer_ping_timeout(rqc_timer_type_t type, rqc_usec_t now, void *user_data)
{
    rqc_connection_t *conn = (rqc_connection_t *)user_data;

    conn->conn_flag |= RQC_CONN_FLAG_PING;

    if (conn->conn_settings.ping_on && conn->conn_type == RQC_CONN_TYPE_CLIENT) {
        rqc_timer_set(&conn->conn_timer_manager, RQC_TIMER_PING, now, RQC_PING_TIMEOUT * 1000);
    }
}

void
rqc_timer_linger_close_timeout(rqc_timer_type_t type, rqc_usec_t now, void *user_data)
{
    rqc_connection_t *conn = (rqc_connection_t *)user_data;

    conn->conn_flag &= ~RQC_CONN_FLAG_LINGER_CLOSING;

    rqc_int_t ret = rqc_conn_immediate_close(conn);
    if (ret) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_conn_immediate_close error|");
        return;
    }
}

/* timer callbacks end */

void
rqc_timer_init(rqc_timer_manager_t *manager, rqc_log_t *log, void *user_data)
{
    memset(manager->timer, 0, RQC_TIMER_N * sizeof(rqc_timer_t));
    manager->log = log;

    rqc_timer_t *timer;
    for (rqc_timer_type_t type = 0; type < RQC_TIMER_N; ++type) {
        timer = &manager->timer[type];
        if (type == RQC_TIMER_ACK) {
            timer->timeout_cb = rqc_timer_ack_timeout;
            timer->user_data = user_data;

        } else if (type == RQC_TIMER_LOSS_DETECTION) {
            timer->timeout_cb = rqc_timer_loss_detection_timeout;
            timer->user_data = user_data;

        } else if (type == RQC_TIMER_PACING) {
            timer->timeout_cb = rqc_timer_pacing_timeout;
            timer->user_data = user_data;

        } else if (type == RQC_TIMER_NAT_REBINDING) {
            timer->timeout_cb = rqc_timer_nat_rebinding_timeout;
            timer->user_data = user_data;

        } else if (type == RQC_TIMER_CONN_IDLE) {
            timer->timeout_cb = rqc_timer_conn_idle_timeout;
            timer->user_data = user_data;

        } else if (type == RQC_TIMER_CONN_DRAINING) {
            timer->timeout_cb = rqc_timer_conn_draining_timeout;
            timer->user_data = user_data;

        } else if (type == RQC_TIMER_STREAM_CLOSE) {
            timer->timeout_cb = rqc_timer_stream_close_timeout;
            timer->user_data = user_data;

        } else if (type == RQC_TIMER_PING) {
            timer->timeout_cb = rqc_timer_ping_timeout;
            timer->user_data = user_data;

        } else if (type == RQC_TIMER_LINGER_CLOSE) {
            timer->timeout_cb = rqc_timer_linger_close_timeout;
            timer->user_data = user_data;

        }
    }

    /* init gp timer list */
    rqc_init_list_head(&manager->gp_timer_list);
    manager->next_gp_timer_id = 0;
}

rqc_gp_timer_id_t rqc_timer_register_gp_timer(rqc_timer_manager_t *manager,
    char *timer_name, rqc_gp_timer_timeout_pt cb, void *user_data)
{
    if (timer_name == NULL
        || manager == NULL
        || cb == NULL)
    {
        return -RQC_EPARAM;
    }

    if (manager->next_gp_timer_id == RQC_GP_TIMER_ID_MAX) {
        return RQC_ERROR;
    }

    rqc_gp_timer_t *timer = (rqc_gp_timer_t*)rqc_calloc(1, sizeof(rqc_gp_timer_t));
    if (timer == NULL) {
        return -RQC_EMALLOC;
    }

    size_t name_len = strnlen(timer_name, 1024);
    timer->name = (char*)rqc_calloc(1, name_len + 1);
    if (timer->name == NULL) {
        rqc_free(timer);
        return -RQC_EMALLOC;
    }
    rqc_memcpy(timer->name, timer_name, name_len);

    timer->timer_is_set = RQC_FALSE;
    timer->id = manager->next_gp_timer_id++;
    timer->timeout_cb = cb;
    timer->user_data = user_data;

    rqc_list_add_tail(&timer->list, &manager->gp_timer_list);
    return timer->id;
}

rqc_int_t
rqc_timer_unregister_gp_timer(rqc_timer_manager_t *manager, rqc_gp_timer_id_t gp_timer_id)
{
    if (!manager || gp_timer_id >= manager->next_gp_timer_id) {
        return -RQC_EPARAM;
    }

    rqc_list_head_t *pos, *next;
    rqc_gp_timer_t *gp_timer;

    rqc_list_for_each_safe(pos, next, &manager->gp_timer_list) {
        gp_timer = rqc_list_entry(pos, rqc_gp_timer_t, list);
        if (gp_timer->id == gp_timer_id) {
            rqc_timer_destroy_gp_timer(gp_timer);
            return RQC_OK;
        }
    }
    return RQC_ERROR;
}

void
rqc_timer_destroy_gp_timer(rqc_gp_timer_t *gp_timer)
{
    rqc_list_del_init(&gp_timer->list);
    rqc_free(gp_timer->name);
    rqc_free(gp_timer);
}

void
rqc_timer_destroy_gp_timer_list(rqc_timer_manager_t *manager)
{
    rqc_list_head_t *pos, *next;
    rqc_gp_timer_t *gp_timer;

    rqc_list_for_each_safe(pos, next, &manager->gp_timer_list) {
        gp_timer = rqc_list_entry(pos, rqc_gp_timer_t, list);
        rqc_timer_destroy_gp_timer(gp_timer);
    }
}

/*
 * *****************TIMER END*****************
 */
