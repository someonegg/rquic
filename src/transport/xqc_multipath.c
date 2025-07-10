/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "src/transport/xqc_multipath.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/transport/xqc_engine.h"
#include "src/transport/xqc_cid.h"
#include "src/transport/xqc_stream.h"
#include "src/transport/xqc_utils.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_frame_parser.h"

#include "src/common/xqc_common.h"
#include "src/common/xqc_malloc.h"
#include "src/common/xqc_str_hash.h"
#include "src/common/xqc_hash.h"
#include "src/common/xqc_priority_q.h"
#include "src/common/xqc_memory_pool.h"
#include "src/common/xqc_random.h"

#include "xquic/xqc_errno.h"

#include <math.h>

xqc_bool_t
xqc_is_same_addr(const struct sockaddr *sa1, const struct sockaddr *sa2)
{
    struct sockaddr_in   *sin1, *sin2;
    struct sockaddr_in6  *sin61, *sin62;

    if (sa1->sa_family != sa2->sa_family) {
        return XQC_FALSE;
    }

    switch (sa1->sa_family) {

        case AF_INET6:
            sin61 = (struct sockaddr_in6 *) sa1;
            sin62 = (struct sockaddr_in6 *) sa2;

            if (memcmp(&sin61->sin6_addr, &sin62->sin6_addr, 16) != 0) {
                return XQC_FALSE;
            }

            if (sin61->sin6_port != sin62->sin6_port) {
                return XQC_FALSE;
            }

            break;

        default: /* AF_INET */

            sin1 = (struct sockaddr_in *) sa1;
            sin2 = (struct sockaddr_in *) sa2;

            if (sin1->sin_addr.s_addr != sin2->sin_addr.s_addr) {
                return XQC_FALSE;
            }

            if (sin1->sin_port != sin2->sin_port) {
                return XQC_FALSE;
            }

            break;
    }

    return XQC_TRUE;
}

xqc_bool_t
xqc_is_same_addr_as_any_path(xqc_connection_t *conn, const struct sockaddr *peer_addr)
{
    xqc_path_ctx_t  *path = conn->the_path;

    if (path && xqc_is_same_addr(peer_addr, (struct sockaddr *)path->peer_addr)) {
        return XQC_TRUE;
    }

    return XQC_FALSE;
}

xqc_int_t
xqc_generate_path_challenge_data(xqc_connection_t *conn, xqc_path_ctx_t *path)
{
    xqc_engine_t *engine = conn->engine;

    return xqc_get_random(engine->rand_generator,
                          path->path_challenge_data, XQC_PATH_CHALLENGE_DATA_LEN);
}

void
xqc_path_schedule_buf_destroy(xqc_path_ctx_t *path)
{
    for (xqc_send_type_t type = 0; type < XQC_SEND_TYPE_N; type++) {
        xqc_send_queue_destroy_packets_list(&path->path_schedule_buf[type]);
    }

    path->path_schedule_bytes = 0;
}

void
xqc_path_schedule_buf_pre_destroy(xqc_send_queue_t *send_queue, xqc_path_ctx_t *path)
{
    for (xqc_send_type_t type = 0; type < XQC_SEND_TYPE_N; type++) {
        xqc_send_queue_pre_destroy_packets_list(send_queue, &path->path_schedule_buf[type]);
    }

    path->path_schedule_bytes = 0;
}

void
xqc_path_send_buffer_append(xqc_path_ctx_t *path, xqc_packet_out_t *packet_out, xqc_list_head_t *head)
{
    /* remove from conn send queue and  add to the path schduled buffer */
    xqc_list_del_init(&packet_out->po_list);
    xqc_list_add_tail(&packet_out->po_list, head);

    if (!(packet_out->po_flag & XQC_POF_IN_PATH_BUF_LIST)) {
        packet_out->po_flag |= XQC_POF_IN_PATH_BUF_LIST;

        packet_out->po_cc_size = packet_out->po_used_size;
        if (XQC_IS_ACK_ELICITING(packet_out->po_frame_types)) {
            path->path_schedule_bytes += packet_out->po_cc_size;
        }
    }
}

void
xqc_path_send_buffer_remove(xqc_path_ctx_t *path, xqc_packet_out_t *packet_out)
{
    xqc_list_del_init(&packet_out->po_list);

    if (packet_out->po_flag & XQC_POF_IN_PATH_BUF_LIST) {
        packet_out->po_flag &= ~XQC_POF_IN_PATH_BUF_LIST;

        if (XQC_IS_ACK_ELICITING(packet_out->po_frame_types)) {
            path->path_schedule_bytes -= packet_out->po_cc_size;
        }
    }
}

void
xqc_path_send_buffer_clear(xqc_connection_t *conn, xqc_path_ctx_t *path, xqc_list_head_t *head, xqc_send_type_t send_type)
{
    xqc_packet_out_t *packet_out;
    xqc_list_head_t  *pos, *next;

    xqc_send_queue_t *send_queue = conn->conn_send_queue;

    xqc_list_for_each_reverse_safe(pos, next, &path->path_schedule_buf[send_type]) {
        packet_out = xqc_list_entry(pos, xqc_packet_out_t, po_list);
        xqc_path_send_buffer_remove(path, packet_out);

        if (head != NULL) {
             /* remove from path scheduled buffer & add to the head of conn send queue */
            xqc_send_queue_move_to_head(&packet_out->po_list, head);

        } else {
            /* 未指定 send_queue 则根据 packet 信息来决定放回 pto/lost/send */
            if (packet_out->po_flag & XQC_POF_TLP) {
                xqc_send_queue_move_to_head(&packet_out->po_list, &send_queue->sndq_pto_probe_packets);

            } else if (packet_out->po_flag & XQC_POF_LOST) {
                xqc_send_queue_move_to_head(&packet_out->po_list, &send_queue->sndq_lost_packets);

            } else {
                xqc_send_queue_move_to_head(&packet_out->po_list, &send_queue->sndq_send_packets);
            }
        }
    }

    path->path_schedule_bytes = 0;
}

static void
xqc_set_path_state(xqc_path_ctx_t *path, xqc_path_state_t dst_state)
{
    path->path_state = dst_state;
}

static void
xqc_path_destroy(xqc_path_ctx_t *path)
{
    if (path == NULL) {
        return;
    }

    if (path->path_send_ctl != NULL) {
        xqc_send_ctl_destroy(path->path_send_ctl);
        path->path_send_ctl = NULL;
    }

    if (path->path_pn_ctl != NULL) {
        xqc_pn_ctl_destroy(path->path_pn_ctl);
        path->path_pn_ctl = NULL;
    }

    xqc_path_schedule_buf_destroy(path);

    xqc_free((void *)path);
}

static xqc_path_ctx_t *
xqc_path_create(xqc_connection_t *conn, xqc_cid_t *scid, xqc_cid_t *dcid, uint64_t path_id)
{
    xqc_path_ctx_t *path = NULL;

    path = xqc_calloc(1, sizeof(xqc_path_ctx_t));
    if (path == NULL) {
        return NULL;
    }
    xqc_memzero(path, sizeof(xqc_path_ctx_t));

    path->path_state = XQC_PATH_STATE_INIT;
    path->parent_conn = conn;
    path->path_id = path_id;

    path->path_pn_ctl = xqc_pn_ctl_create(conn);
    if (path->path_pn_ctl == NULL) {
        goto err;
    }

    path->path_send_ctl = xqc_send_ctl_create(path);
    if (path->path_send_ctl == NULL) {
        goto err;
    }

    for (xqc_send_type_t type = 0; type < XQC_SEND_TYPE_N; type++) {
        xqc_init_list_head(&path->path_schedule_buf[type]);
    }

    /* cid & path_id init */
    if (scid == NULL) {
        if (xqc_get_unused_cid(&conn->scid_set, &path->path_scid, path_id) != XQC_OK) {
            xqc_log(conn->log, XQC_LOG_ERROR, "|conn don't have available scid|");
            goto err;
        }

    } else {
        /* already have scid */
        xqc_cid_inner_t *inner_cid = xqc_cid_in_cid_set(&conn->scid_set, scid, path_id);
        if (inner_cid == NULL) {
            goto err;
        }

        xqc_cid_copy(&path->path_scid, &inner_cid->cid);
    }

    if (dcid == NULL) {
        if (xqc_get_unused_cid(&conn->dcid_set, &(path->path_dcid), path_id) != XQC_OK) {
            xqc_log(conn->log, XQC_LOG_ERROR, "|MP|conn don't have available dcid|");
            goto err;
        }

    } else {
        /* already have dcid */
        xqc_cid_copy(&(path->path_dcid), dcid);
    }

    xqc_cid_set_update_state(&conn->dcid_set, path_id, XQC_CID_SET_USED);
    xqc_cid_set_update_state(&conn->scid_set, path_id, XQC_CID_SET_USED);

    path->path_create_time = xqc_monotonic_timestamp();
    path->curr_pkt_out_size = conn->pkt_out_size;
    path->path_max_pkt_out_size = conn->max_pkt_out_size;

    return path;

err:
    xqc_path_destroy(path);
    return NULL;
}

static xqc_int_t
xqc_path_init(xqc_path_ctx_t *path, xqc_connection_t *conn)
{
    xqc_int_t ret = XQC_ERROR;

    if (conn->peer_addrlen > 0) {
        xqc_memcpy(path->peer_addr, conn->peer_addr, conn->peer_addrlen);
        path->peer_addrlen = conn->peer_addrlen;
    }

    if (conn->local_addrlen > 0) {
        xqc_memcpy(path->local_addr, conn->local_addr, conn->local_addrlen);
        path->local_addrlen = conn->local_addrlen;
    }

    xqc_set_path_state(path, XQC_PATH_STATE_ACTIVE);

    return XQC_OK;
}

xqc_int_t
xqc_conn_init_paths_list(xqc_connection_t *conn)
{
    conn->the_path = xqc_conn_create_path_inner(conn,
                                                &conn->scid_set.user_scid,
                                                &conn->dcid_set.current_dcid,
                                                0);
    if (conn->the_path == NULL) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_conn_create_path_inner fail|");
        return -XQC_EMP_CREATE_PATH;
    }

    return XQC_OK;
}

void
xqc_conn_destroy_paths_list(xqc_connection_t *conn)
{
    xqc_path_destroy(conn->the_path);
}

xqc_path_ctx_t *
xqc_conn_create_path_inner(xqc_connection_t *conn, xqc_cid_t *scid, xqc_cid_t *dcid, uint64_t path_id)
{
    xqc_int_t ret = XQC_ERROR;
    xqc_path_ctx_t *path = NULL;

    path = xqc_path_create(conn, scid, dcid, path_id);
    if (path == NULL) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_path_create error|");
        return NULL;
    }

    ret = xqc_path_init(path, conn);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|xqc_path_init error|%d|", ret);
        return NULL;
    }
    xqc_log_event(conn->log, CON_PATH_ASSIGNED, path, conn);
    return path;
}

xqc_int_t
xqc_conn_server_init_path_addr(xqc_connection_t *conn, uint64_t path_id,
    const struct sockaddr *local_addr, socklen_t local_addrlen,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen)
{
    xqc_int_t ret = XQC_OK;

    xqc_path_ctx_t *path = conn->the_path;

    if (local_addr && local_addrlen > 0) {
        ret = xqc_memcpy_with_cap(path->local_addr, sizeof(path->local_addr), local_addr, local_addrlen);
        if (ret == XQC_OK) {
            path->local_addrlen = local_addrlen;
            path->addr_str_len = 0;

        } else {
            xqc_log(conn->log, XQC_LOG_ERROR,
                    "|local addr too large|addr_len:%d|", (int)local_addrlen);
            return -XQC_ENOBUF;
        }
    }

    if (peer_addr && peer_addrlen > 0) {
        ret = xqc_memcpy_with_cap(path->peer_addr, sizeof(path->peer_addr), peer_addr, peer_addrlen);
        if (ret == XQC_OK) {
            path->peer_addrlen = peer_addrlen;
            path->addr_str_len = 0;

        } else {
            xqc_log(conn->log, XQC_LOG_ERROR,
                    "|peer addr too large|addr_len:%d|", (int)peer_addrlen);
            return -XQC_ENOBUF;
        }
    }

    xqc_log(conn->engine->log, XQC_LOG_STATS, "|path:%ui|%s|", path_id, xqc_path_addr_str(path));

    return XQC_OK;
}

xqc_int_t
xqc_conn_client_init_path_addr(xqc_connection_t *conn)
{
    xqc_path_ctx_t *path = conn->the_path;

    if (conn->peer_addrlen > 0) {
        xqc_memcpy(path->peer_addr, conn->peer_addr, conn->peer_addrlen);
        path->peer_addrlen = conn->peer_addrlen;
    }

    if (conn->local_addrlen > 0) {
        xqc_memcpy(path->local_addr, conn->local_addr, conn->local_addrlen);
        path->local_addrlen = conn->local_addrlen;
    }

    return XQC_OK;
}

void
xqc_conn_path_metrics_print(xqc_connection_t *conn, xqc_conn_stats_t *stats)
{
    xqc_path_ctx_t *path = conn->the_path;
    if (path == NULL || path->path_send_ctl == NULL) {
        return;
    }

    stats->path_info.path_id = path->path_id;
    stats->path_info.path_pkt_recv_count = path->path_send_ctl->ctl_recv_count;
    stats->path_info.path_pkt_send_count = path->path_send_ctl->ctl_send_count;
    stats->path_info.path_send_bytes = path->path_send_ctl->ctl_app_bytes_send;
    stats->path_info.path_recv_bytes = path->path_send_ctl->ctl_app_bytes_recv;

    stats->total_app_bytes += path->path_send_ctl->ctl_app_bytes_send + path->path_send_ctl->ctl_app_bytes_recv;
}
