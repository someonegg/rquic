#include "src/transport/xqc_timer.h"
#include "src/transport/xqc_engine.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/transport/xqc_stream.h"
#include "src/transport/xqc_utils.h"

static const char * const timer_type_2_str[XQC_TIMER_N] = {

    /* path level (path->path_send_ctl->path_timer_manager->timer[XQC_TIMER_N])*/
    [XQC_TIMER_ACK]             = "ACK",
    [XQC_TIMER_LOSS_DETECTION]  = "LOSS_DETECTION",
    [XQC_TIMER_PACING]          = "PACING",
    [XQC_TIMER_NAT_REBINDING]   = "NAT_REBINDING",

    /* connection level (conn->conn_timer_manager->timer[XQC_TIMER_N]) */
    [XQC_TIMER_CONN_IDLE]       = "CONN_IDLE",
    [XQC_TIMER_CONN_DRAINING]   = "CONN_DRAINING",
    [XQC_TIMER_STREAM_CLOSE]    = "STREAM_CLOSE",
    [XQC_TIMER_PING]            = "PING",
    [XQC_TIMER_LINGER_CLOSE]    = "LINGER_CLOSE",
};

const char *
xqc_timer_type_2_str(xqc_timer_type_t timer_type)
{
    return timer_type_2_str[timer_type];
}

/* timer callbacks */
void
xqc_timer_ack_timeout(xqc_timer_type_t type, xqc_usec_t now, void *user_data)
{
    xqc_send_ctl_t *send_ctl = (xqc_send_ctl_t *)user_data;

    xqc_connection_t *conn = send_ctl->ctl_conn;
    send_ctl->ctl_path->path_flag |= XQC_PATH_FLAG_SHOULD_ACK;
    conn->ack_flag |= (1 << send_ctl->ctl_path->path_id);
}

/**
 * OnLossDetectionTimeout
 */
void
xqc_timer_loss_detection_timeout(xqc_timer_type_t type, xqc_usec_t now, void *user_data)
{
    xqc_send_ctl_t *send_ctl = (xqc_send_ctl_t *)user_data;

    xqc_path_ctx_t *path = send_ctl->ctl_path;
    xqc_connection_t *conn = send_ctl->ctl_conn;

    xqc_usec_t loss_time;
    loss_time = xqc_send_ctl_get_earliest_loss_time(send_ctl);
    if (loss_time != 0) {
        /* Time threshold loss Detection */
        xqc_send_ctl_detect_lost(send_ctl, conn->conn_send_queue, now);
        xqc_send_ctl_set_loss_detection_timer(send_ctl);
        return;
    }

    if (send_ctl->ctl_bytes_in_flight > 0) {
        /*
         * PTO. Send new data if available, else retransmit old data.
         * If neither is available, send a single PING frame
         */
        xqc_path_send_one_or_two_ack_elicit_pkts(path);
    }

    send_ctl->ctl_pto_count++;
    conn->max_pto_cnt = xqc_max(send_ctl->ctl_pto_count, conn->max_pto_cnt);
    xqc_send_ctl_set_loss_detection_timer(send_ctl);
}

void
xqc_timer_pacing_timeout(xqc_timer_type_t type, xqc_usec_t now, void *user_data)
{
    xqc_send_ctl_t *send_ctl = (xqc_send_ctl_t *)user_data;

    xqc_pacing_t *pacing = &send_ctl->ctl_pacing;
    xqc_pacing_on_timeout(pacing);
}

void
xqc_timer_nat_rebinding_timeout(xqc_timer_type_t type, xqc_usec_t now, void *user_data)
{
    xqc_send_ctl_t *send_ctl = (xqc_send_ctl_t *)user_data;
    xqc_path_ctx_t *path = send_ctl->ctl_path;

    path->rebinding_addrlen = 0;
    path->rebinding_check_response = 0;
}

void
xqc_timer_conn_idle_timeout(xqc_timer_type_t type, xqc_usec_t now, void *user_data)
{
    xqc_connection_t *conn = (xqc_connection_t *)user_data;

    conn->conn_flag |= XQC_CONN_FLAG_TIME_OUT;

    XQC_CONN_CLOSE_MSG(conn, "idle timeout");
}

void
xqc_timer_conn_draining_timeout(xqc_timer_type_t type, xqc_usec_t now, void *user_data)
{
    xqc_connection_t *conn = (xqc_connection_t *)user_data;

    conn->conn_flag |= XQC_CONN_FLAG_TIME_OUT;
}

void
xqc_timer_stream_close_timeout(xqc_timer_type_t type, xqc_usec_t now, void *user_data)
{
    xqc_connection_t *conn = (xqc_connection_t *)user_data;

    xqc_list_head_t *pos, *next;
    xqc_stream_t *stream;
    xqc_usec_t min_expire = XQC_MAX_UINT64_VALUE, interval = 0;
    xqc_list_for_each_safe(pos, next, &conn->conn_closing_streams) {
        stream = xqc_list_entry(pos, xqc_stream_t, closing_stream_list);
        if (stream->stream_close_time <= now) {
            xqc_list_del_init(pos);
            XQC_STREAM_CLOSE_MSG(stream, "finished");
            xqc_destroy_stream(stream);

        } else {
            min_expire = xqc_min(min_expire, stream->stream_close_time);
        }
    }

    if (min_expire != XQC_MAX_UINT64_VALUE) {
        interval = (min_expire > now) ? (min_expire - now) : 0;
        xqc_timer_set(&conn->conn_timer_manager, XQC_TIMER_STREAM_CLOSE, now, interval);
    }
}

void
xqc_timer_ping_timeout(xqc_timer_type_t type, xqc_usec_t now, void *user_data)
{
    xqc_connection_t *conn = (xqc_connection_t *)user_data;

    conn->conn_flag |= XQC_CONN_FLAG_PING;

    if (conn->conn_settings.ping_on && conn->conn_type == XQC_CONN_TYPE_CLIENT) {
        xqc_timer_set(&conn->conn_timer_manager, XQC_TIMER_PING, now, XQC_PING_TIMEOUT * 1000);
    }
}

void
xqc_timer_linger_close_timeout(xqc_timer_type_t type, xqc_usec_t now, void *user_data)
{
    xqc_connection_t *conn = (xqc_connection_t *)user_data;

    conn->conn_flag &= ~XQC_CONN_FLAG_LINGER_CLOSING;

    xqc_int_t ret = xqc_conn_immediate_close(conn);
    if (ret) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_conn_immediate_close error|");
        return;
    }
}

/* timer callbacks end */

void
xqc_timer_init(xqc_timer_manager_t *manager, xqc_log_t *log, void *user_data)
{
    memset(manager->timer, 0, XQC_TIMER_N * sizeof(xqc_timer_t));
    manager->log = log;

    xqc_timer_t *timer;
    for (xqc_timer_type_t type = 0; type < XQC_TIMER_N; ++type) {
        timer = &manager->timer[type];
        if (type == XQC_TIMER_ACK) {
            timer->timeout_cb = xqc_timer_ack_timeout;
            timer->user_data = user_data;

        } else if (type == XQC_TIMER_LOSS_DETECTION) {
            timer->timeout_cb = xqc_timer_loss_detection_timeout;
            timer->user_data = user_data;

        } else if (type == XQC_TIMER_PACING) {
            timer->timeout_cb = xqc_timer_pacing_timeout;
            timer->user_data = user_data;

        } else if (type == XQC_TIMER_NAT_REBINDING) {
            timer->timeout_cb = xqc_timer_nat_rebinding_timeout;
            timer->user_data = user_data;

        } else if (type == XQC_TIMER_CONN_IDLE) {
            timer->timeout_cb = xqc_timer_conn_idle_timeout;
            timer->user_data = user_data;

        } else if (type == XQC_TIMER_CONN_DRAINING) {
            timer->timeout_cb = xqc_timer_conn_draining_timeout;
            timer->user_data = user_data;

        } else if (type == XQC_TIMER_STREAM_CLOSE) {
            timer->timeout_cb = xqc_timer_stream_close_timeout;
            timer->user_data = user_data;

        } else if (type == XQC_TIMER_PING) {
            timer->timeout_cb = xqc_timer_ping_timeout;
            timer->user_data = user_data;

        } else if (type == XQC_TIMER_LINGER_CLOSE) {
            timer->timeout_cb = xqc_timer_linger_close_timeout;
            timer->user_data = user_data;

        }
    }

    /* init gp timer list */
    xqc_init_list_head(&manager->gp_timer_list);
    manager->next_gp_timer_id = 0;
}

xqc_gp_timer_id_t xqc_timer_register_gp_timer(xqc_timer_manager_t *manager,
    char *timer_name, xqc_gp_timer_timeout_pt cb, void *user_data)
{
    if (timer_name == NULL
        || manager == NULL
        || cb == NULL)
    {
        return -XQC_EPARAM;
    }

    if (manager->next_gp_timer_id == XQC_GP_TIMER_ID_MAX) {
        return XQC_ERROR;
    }

    xqc_gp_timer_t *timer = (xqc_gp_timer_t*)xqc_calloc(1, sizeof(xqc_gp_timer_t));
    if (timer == NULL) {
        return -XQC_EMALLOC;
    }

    size_t name_len = strnlen(timer_name, 1024);
    timer->name = (char*)xqc_calloc(1, name_len + 1);
    if (timer->name == NULL) {
        xqc_free(timer);
        return -XQC_EMALLOC;
    }
    xqc_memcpy(timer->name, timer_name, name_len);

    timer->timer_is_set = XQC_FALSE;
    timer->id = manager->next_gp_timer_id++;
    timer->timeout_cb = cb;
    timer->user_data = user_data;

    xqc_list_add_tail(&timer->list, &manager->gp_timer_list);
    return timer->id;
}

xqc_int_t
xqc_timer_unregister_gp_timer(xqc_timer_manager_t *manager, xqc_gp_timer_id_t gp_timer_id)
{
    if (!manager || gp_timer_id >= manager->next_gp_timer_id) {
        return -XQC_EPARAM;
    }

    xqc_list_head_t *pos, *next;
    xqc_gp_timer_t *gp_timer;

    xqc_list_for_each_safe(pos, next, &manager->gp_timer_list) {
        gp_timer = xqc_list_entry(pos, xqc_gp_timer_t, list);
        if (gp_timer->id == gp_timer_id) {
            xqc_timer_destroy_gp_timer(gp_timer);
            return XQC_OK;
        }
    }
    return XQC_ERROR;
}

void
xqc_timer_destroy_gp_timer(xqc_gp_timer_t *gp_timer)
{
    xqc_list_del_init(&gp_timer->list);
    xqc_free(gp_timer->name);
    xqc_free(gp_timer);
}

void
xqc_timer_destroy_gp_timer_list(xqc_timer_manager_t *manager)
{
    xqc_list_head_t *pos, *next;
    xqc_gp_timer_t *gp_timer;

    xqc_list_for_each_safe(pos, next, &manager->gp_timer_list) {
        gp_timer = xqc_list_entry(pos, xqc_gp_timer_t, list);
        xqc_timer_destroy_gp_timer(gp_timer);
    }
}

/*
 * *****************TIMER END*****************
 */
