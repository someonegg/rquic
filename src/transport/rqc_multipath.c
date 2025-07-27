/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "src/transport/rqc_multipath.h"
#include "src/transport/rqc_conn.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/transport/rqc_engine.h"
#include "src/transport/rqc_cid.h"
#include "src/transport/rqc_stream.h"
#include "src/transport/rqc_utils.h"
#include "src/transport/rqc_packet_out.h"
#include "src/transport/rqc_frame_parser.h"

#include "src/common/rqc_common.h"
#include "src/common/rqc_malloc.h"
#include "src/common/rqc_str_hash.h"
#include "src/common/rqc_hash.h"
#include "src/common/rqc_priority_q.h"
#include "src/common/rqc_memory_pool.h"
#include "src/common/rqc_random.h"

#include "rquic/rqc_errno.h"

#include <math.h>

rqc_bool_t
rqc_is_same_addr(const struct sockaddr *sa1, const struct sockaddr *sa2)
{
    struct sockaddr_in   *sin1, *sin2;
    struct sockaddr_in6  *sin61, *sin62;

    if (sa1->sa_family != sa2->sa_family) {
        return RQC_FALSE;
    }

    switch (sa1->sa_family) {

        case AF_INET6:
            sin61 = (struct sockaddr_in6 *) sa1;
            sin62 = (struct sockaddr_in6 *) sa2;

            if (memcmp(&sin61->sin6_addr, &sin62->sin6_addr, 16) != 0) {
                return RQC_FALSE;
            }

            if (sin61->sin6_port != sin62->sin6_port) {
                return RQC_FALSE;
            }

            break;

        default: /* AF_INET */

            sin1 = (struct sockaddr_in *) sa1;
            sin2 = (struct sockaddr_in *) sa2;

            if (sin1->sin_addr.s_addr != sin2->sin_addr.s_addr) {
                return RQC_FALSE;
            }

            if (sin1->sin_port != sin2->sin_port) {
                return RQC_FALSE;
            }

            break;
    }

    return RQC_TRUE;
}

rqc_bool_t
rqc_is_same_addr_as_any_path(rqc_connection_t *conn, const struct sockaddr *peer_addr)
{
    rqc_path_ctx_t  *path = conn->the_path;

    if (path && rqc_is_same_addr(peer_addr, (struct sockaddr *)path->peer_addr)) {
        return RQC_TRUE;
    }

    return RQC_FALSE;
}

rqc_int_t
rqc_generate_path_challenge_data(rqc_connection_t *conn, rqc_path_ctx_t *path)
{
    rqc_engine_t *engine = conn->engine;

    return rqc_get_random(engine->rand_generator,
                          path->path_challenge_data, RQC_PATH_CHALLENGE_DATA_LEN);
}

void
rqc_path_schedule_buf_destroy(rqc_path_ctx_t *path)
{
    for (rqc_send_type_t type = 0; type < RQC_SEND_TYPE_N; type++) {
        rqc_send_queue_destroy_packets_list(&path->path_schedule_buf[type]);
    }

    path->path_schedule_bytes = 0;
}

void
rqc_path_schedule_buf_pre_destroy(rqc_send_queue_t *send_queue, rqc_path_ctx_t *path)
{
    for (rqc_send_type_t type = 0; type < RQC_SEND_TYPE_N; type++) {
        rqc_send_queue_pre_destroy_packets_list(send_queue, &path->path_schedule_buf[type]);
    }

    path->path_schedule_bytes = 0;
}

void
rqc_path_send_buffer_append(rqc_path_ctx_t *path, rqc_packet_out_t *packet_out, rqc_list_head_t *head)
{
    /* remove from conn send queue and  add to the path schduled buffer */
    rqc_list_del_init(&packet_out->po_list);
    rqc_list_add_tail(&packet_out->po_list, head);

    if (!(packet_out->po_flag & RQC_POF_IN_PATH_BUF_LIST)) {
        packet_out->po_flag |= RQC_POF_IN_PATH_BUF_LIST;

        packet_out->po_cc_size = packet_out->po_used_size;
        if (RQC_IS_ACK_ELICITING(packet_out->po_frame_types)) {
            path->path_schedule_bytes += packet_out->po_cc_size;
        }
    }
}

void
rqc_path_send_buffer_remove(rqc_path_ctx_t *path, rqc_packet_out_t *packet_out)
{
    rqc_list_del_init(&packet_out->po_list);

    if (packet_out->po_flag & RQC_POF_IN_PATH_BUF_LIST) {
        packet_out->po_flag &= ~RQC_POF_IN_PATH_BUF_LIST;

        if (RQC_IS_ACK_ELICITING(packet_out->po_frame_types)) {
            path->path_schedule_bytes -= packet_out->po_cc_size;
        }
    }
}

void
rqc_path_send_buffer_clear(rqc_connection_t *conn, rqc_path_ctx_t *path, rqc_list_head_t *head, rqc_send_type_t send_type)
{
    rqc_packet_out_t *packet_out;
    rqc_list_head_t  *pos, *next;

    rqc_send_queue_t *send_queue = conn->conn_send_queue;

    rqc_list_for_each_reverse_safe(pos, next, &path->path_schedule_buf[send_type]) {
        packet_out = rqc_list_entry(pos, rqc_packet_out_t, po_list);
        rqc_path_send_buffer_remove(path, packet_out);

        if (head != NULL) {
             /* remove from path scheduled buffer & add to the head of conn send queue */
            rqc_send_queue_move_to_head(&packet_out->po_list, head);

        } else {
            /* 未指定 send_queue 则根据 packet 信息来决定放回 pto/lost/send */
            if (packet_out->po_flag & RQC_POF_TLP) {
                rqc_send_queue_move_to_head(&packet_out->po_list, &send_queue->sndq_pto_probe_packets);

            } else if (packet_out->po_flag & RQC_POF_LOST) {
                rqc_send_queue_move_to_head(&packet_out->po_list, &send_queue->sndq_lost_packets);

            } else {
                rqc_send_queue_move_to_head(&packet_out->po_list, &send_queue->sndq_send_packets);
            }
        }
    }

    path->path_schedule_bytes = 0;
}

static void
rqc_set_path_state(rqc_path_ctx_t *path, rqc_path_state_t dst_state)
{
    path->path_state = dst_state;
}

static void
rqc_path_destroy(rqc_path_ctx_t *path)
{
    if (path == NULL) {
        return;
    }

    if (path->path_send_ctl != NULL) {
        rqc_send_ctl_destroy(path->path_send_ctl);
        path->path_send_ctl = NULL;
    }

    if (path->path_pn_ctl != NULL) {
        rqc_pn_ctl_destroy(path->path_pn_ctl);
        path->path_pn_ctl = NULL;
    }

    rqc_path_schedule_buf_destroy(path);

    rqc_free((void *)path);
}

static rqc_path_ctx_t *
rqc_path_create(rqc_connection_t *conn, rqc_cid_t *scid, rqc_cid_t *dcid, uint64_t path_id)
{
    rqc_path_ctx_t *path = NULL;

    path = rqc_calloc(1, sizeof(rqc_path_ctx_t));
    if (path == NULL) {
        return NULL;
    }
    rqc_memzero(path, sizeof(rqc_path_ctx_t));

    path->path_state = RQC_PATH_STATE_INIT;
    path->parent_conn = conn;
    path->path_id = path_id;

    path->path_pn_ctl = rqc_pn_ctl_create(conn);
    if (path->path_pn_ctl == NULL) {
        goto err;
    }

    path->path_send_ctl = rqc_send_ctl_create(path);
    if (path->path_send_ctl == NULL) {
        goto err;
    }

    for (rqc_send_type_t type = 0; type < RQC_SEND_TYPE_N; type++) {
        rqc_init_list_head(&path->path_schedule_buf[type]);
    }

    /* cid & path_id init */
    if (scid == NULL) {
        if (rqc_get_unused_cid(&conn->scid_set, &path->path_scid, path_id) != RQC_OK) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|conn don't have available scid|");
            goto err;
        }

    } else {
        /* already have scid */
        rqc_cid_inner_t *inner_cid = rqc_cid_in_cid_set(&conn->scid_set, scid, path_id);
        if (inner_cid == NULL) {
            goto err;
        }

        rqc_cid_copy(&path->path_scid, &inner_cid->cid);
    }

    if (dcid == NULL) {
        if (rqc_get_unused_cid(&conn->dcid_set, &(path->path_dcid), path_id) != RQC_OK) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|MP|conn don't have available dcid|");
            goto err;
        }

    } else {
        /* already have dcid */
        rqc_cid_copy(&(path->path_dcid), dcid);
    }

    rqc_cid_set_update_state(&conn->dcid_set, path_id, RQC_CID_SET_USED);
    rqc_cid_set_update_state(&conn->scid_set, path_id, RQC_CID_SET_USED);

    path->path_create_time = rqc_monotonic_timestamp();

    return path;

err:
    rqc_path_destroy(path);
    return NULL;
}

static rqc_int_t
rqc_path_init(rqc_path_ctx_t *path, rqc_connection_t *conn)
{
    if (conn->peer_addrlen > 0) {
        rqc_memcpy(path->peer_addr, conn->peer_addr, conn->peer_addrlen);
        path->peer_addrlen = conn->peer_addrlen;
    }

    if (conn->local_addrlen > 0) {
        rqc_memcpy(path->local_addr, conn->local_addr, conn->local_addrlen);
        path->local_addrlen = conn->local_addrlen;
    }

    rqc_set_path_state(path, RQC_PATH_STATE_ACTIVE);

    return RQC_OK;
}

rqc_int_t
rqc_conn_init_paths_list(rqc_connection_t *conn)
{
    conn->the_path = rqc_conn_create_path_inner(conn,
                                                &conn->scid_set.user_scid,
                                                &conn->dcid_set.current_dcid,
                                                0);
    if (conn->the_path == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_conn_create_path_inner fail|");
        return -RQC_EMP_CREATE_PATH;
    }

    return RQC_OK;
}

void
rqc_conn_destroy_paths_list(rqc_connection_t *conn)
{
    rqc_path_destroy(conn->the_path);
}

rqc_path_ctx_t *
rqc_conn_create_path_inner(rqc_connection_t *conn, rqc_cid_t *scid, rqc_cid_t *dcid, uint64_t path_id)
{
    rqc_int_t ret = RQC_ERROR;
    rqc_path_ctx_t *path = NULL;

    path = rqc_path_create(conn, scid, dcid, path_id);
    if (path == NULL) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_path_create error|");
        return NULL;
    }

    ret = rqc_path_init(path, conn);
    if (ret != RQC_OK) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_path_init error|%d|", ret);
        return NULL;
    }
    rqc_log_event(conn->log, CON_PATH_ASSIGNED, path, conn);
    return path;
}

rqc_int_t
rqc_conn_server_init_path_addr(rqc_connection_t *conn, uint64_t path_id,
    const struct sockaddr *local_addr, socklen_t local_addrlen,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen)
{
    rqc_int_t ret = RQC_OK;

    rqc_path_ctx_t *path = conn->the_path;

    if (local_addr && local_addrlen > 0) {
        ret = rqc_memcpy_with_cap(path->local_addr, sizeof(path->local_addr), local_addr, local_addrlen);
        if (ret == RQC_OK) {
            path->local_addrlen = local_addrlen;
            path->addr_str_len = 0;

        } else {
            rqc_log(conn->log, RQC_LOG_ERROR,
                    "|local addr too large|addr_len:%d|", (int)local_addrlen);
            return -RQC_ENOBUF;
        }
    }

    if (peer_addr && peer_addrlen > 0) {
        ret = rqc_memcpy_with_cap(path->peer_addr, sizeof(path->peer_addr), peer_addr, peer_addrlen);
        if (ret == RQC_OK) {
            path->peer_addrlen = peer_addrlen;
            path->addr_str_len = 0;

        } else {
            rqc_log(conn->log, RQC_LOG_ERROR,
                    "|peer addr too large|addr_len:%d|", (int)peer_addrlen);
            return -RQC_ENOBUF;
        }
    }

    rqc_log(conn->engine->log, RQC_LOG_STATS, "|path:%ui|%s|", path_id, rqc_path_addr_str(path));

    return RQC_OK;
}

rqc_int_t
rqc_conn_client_init_path_addr(rqc_connection_t *conn)
{
    rqc_path_ctx_t *path = conn->the_path;

    if (conn->peer_addrlen > 0) {
        rqc_memcpy(path->peer_addr, conn->peer_addr, conn->peer_addrlen);
        path->peer_addrlen = conn->peer_addrlen;
    }

    if (conn->local_addrlen > 0) {
        rqc_memcpy(path->local_addr, conn->local_addr, conn->local_addrlen);
        path->local_addrlen = conn->local_addrlen;
    }

    rqc_log(conn->engine->log, RQC_LOG_STATS, "|path:%ui|%s|", path->path_id, rqc_path_addr_str(path));

    return RQC_OK;
}

void
rqc_conn_path_metrics_print(rqc_connection_t *conn, rqc_conn_stats_t *stats)
{
    rqc_path_ctx_t *path = conn->the_path;
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
