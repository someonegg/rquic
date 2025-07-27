/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "src/transport/rqc_recv_record.h"
#include "src/transport/rqc_packet.h"
#include "src/transport/rqc_conn.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/common/rqc_log.h"

void
rqc_recv_record_print(rqc_connection_t *conn, rqc_recv_record_t *recv_record, char *buff, unsigned buff_size)
{
    rqc_list_head_t *pos, *next;
    rqc_pktno_range_node_t *pnode;
    buff[0] = '\0';
    rqc_pktno_range_t range[3]; /* record up to 3 segments */
    memset(&range, 0, sizeof(range));
    int range_count = 0;

    rqc_list_for_each_safe(pos, next, &recv_record->list_head) {
        pnode = rqc_list_entry(pos, rqc_pktno_range_node_t, list);
        range[range_count].high = pnode->pktno_range.high;
        range[range_count].low = pnode->pktno_range.low;
        range_count++;
        if (range_count >= 3) {
            break;
        }
    }

    snprintf(buff, buff_size, "#%"PRIu64"-%"PRIu64"#%"PRIu64"-%"PRIu64"#%"PRIu64"-%"PRIu64"#v0429",
             range[0].high, range[0].low,
             range[1].high, range[1].low,
             range[2].high, range[2].low);
}

static int
rqc_pktno_range_can_merge(rqc_pktno_range_node_t *node, rqc_packet_number_t packet_number)
{
    if (node->pktno_range.low - 1 == packet_number) {
        --node->pktno_range.low;
        return 1;
    }

    if (node->pktno_range.high + 1 == packet_number) {
        ++node->pktno_range.high;
        return 1;
    }

    return 0;
}

/**
 * insert into range list when receive a new packet
 */
rqc_pkt_range_status
rqc_recv_record_add(rqc_recv_record_t *recv_record, rqc_packet_number_t packet_number)
{
    rqc_list_head_t *pos, *prev, *next;
    rqc_pktno_range_node_t *pnode, *prev_node;
    pnode = prev_node = NULL;
    pos = prev = NULL;
    int pos_find = 0;

    rqc_list_for_each_safe(pos, next, &recv_record->list_head) {
        pnode = rqc_list_entry(pos, rqc_pktno_range_node_t, list);
        if (packet_number <= pnode->pktno_range.high) {
            if (packet_number >= pnode->pktno_range.low) {
                return RQC_PKTRANGE_DUP;
            }

        } else {
            pos_find = 1;
            break;
        }
        prev = pos;
    }

    if (pos_find) {
        pnode = rqc_list_entry(pos, rqc_pktno_range_node_t, list);
    }

    if (prev) {
        prev_node = rqc_list_entry(prev, rqc_pktno_range_node_t, list);
    }

    if ((prev_node && rqc_pktno_range_can_merge(prev_node, packet_number))
        || (pnode && rqc_pktno_range_can_merge(pnode, packet_number)))
    {
        if (prev_node && pnode && (prev_node->pktno_range.low - 1 == pnode->pktno_range.high)) {
            prev_node->pktno_range.low = pnode->pktno_range.low;
            rqc_list_del_init(pos);
            recv_record->node_count--;
            rqc_free(pnode);
        }

    } else {
        rqc_pktno_range_node_t *new_node = rqc_calloc(1, sizeof(*new_node));
        if (!new_node) {
            return RQC_PKTRANGE_ERR;
        }
        new_node->pktno_range.low = new_node->pktno_range.high = packet_number;
        if (pos_find) {
            /* insert before pos */
            rqc_list_add_tail(&(new_node->list), pos);
            recv_record->node_count++;

            /* delete last node if exceed MAX RANGE */
            if (recv_record->node_count > RQC_MAX_ACK_RANGE_CNT) {
                rqc_list_for_each_reverse_safe(pos, next, &recv_record->list_head) {
                    pnode = rqc_list_entry(pos, rqc_pktno_range_node_t, list);
                    rqc_list_del_init(pos);
                    recv_record->node_count--;
                    rqc_free(pnode);
                    if (recv_record->node_count <= RQC_MAX_ACK_RANGE_CNT) {
                        break;
                    }
                }
            }
        } else {
            /* insert tail of the list */
            if (recv_record->node_count < RQC_MAX_ACK_RANGE_CNT) {
                rqc_list_add_tail(&(new_node->list), &recv_record->list_head);
                recv_record->node_count++;
            } else {
                rqc_free(new_node);
            }
        }
    }

    return RQC_PKTRANGE_OK;
}

/**
 * del packet number range < del_from
 */
void
rqc_recv_record_del(rqc_recv_record_t *recv_record, rqc_packet_number_t del_from)
{
    if (del_from < recv_record->rr_del_from) {
        return;
    }

    rqc_list_head_t *pos, *next;
    rqc_pktno_range_node_t *pnode;
    rqc_pktno_range_t *range;

    recv_record->rr_del_from = del_from;

    rqc_list_for_each_safe(pos, next, &recv_record->list_head) {
        pnode = rqc_list_entry(pos, rqc_pktno_range_node_t, list);
        range = &pnode->pktno_range;

        if (range->low < del_from) {
            if (range->high < del_from) {
                rqc_list_del_init(pos);
                recv_record->node_count--;
                rqc_free(pnode);

            } else {
                range->low = del_from;
            }
        }
    }
}

void
rqc_recv_record_destroy(rqc_recv_record_t *recv_record)
{
    rqc_list_head_t *pos, *next;
    rqc_pktno_range_node_t *pnode;
    rqc_list_for_each_safe(pos, next, &recv_record->list_head) {
        pnode = rqc_list_entry(pos, rqc_pktno_range_node_t, list);
        rqc_list_del_init(pos);
        rqc_free(pnode);
    }
    recv_record->node_count = 0;
    recv_record->rr_del_from = 0;
}

rqc_packet_number_t
rqc_recv_record_largest(rqc_recv_record_t *recv_record)
{
    rqc_pktno_range_node_t *pnode = NULL;
    rqc_list_head_t *pos, *next;
    rqc_list_for_each_safe(pos, next, &recv_record->list_head) {
        pnode = rqc_list_entry(pos, rqc_pktno_range_node_t, list);
        break;
    }

    if (pnode) {
        return pnode->pktno_range.high;

    } else {
        return 0;
    }
}

uint32_t
rqc_get_ack_frequency(rqc_connection_t *conn, rqc_path_ctx_t *path)
{
    if(rqc_conn_is_handshake_done(conn)
       && conn->conn_settings.adaptive_ack_frequency
       && path->path_send_ctl->ctl_ack_sent_cnt >= 100)
    {
        // slow down ack rate if we have sent more than 100 ACKs
        return rqc_max(conn->conn_settings.ack_frequency, 10);
    }

    return conn->conn_settings.ack_frequency;
}

void
rqc_maybe_should_ack(rqc_connection_t *conn, rqc_path_ctx_t *path, rqc_pn_ctl_t *pn_ctl, int out_of_order, rqc_usec_t now)
{
    /*
     * Generating Acknowledgements
     */

    if (path->path_flag & RQC_PATH_FLAG_SHOULD_ACK) {
        return;
    }

    rqc_send_ctl_t *send_ctl = path->path_send_ctl;
    uint32_t ack_frequency = rqc_get_ack_frequency(conn, path);

    if (send_ctl->ctl_ack_eliciting_pkt >= ack_frequency
        || (!rqc_conn_is_established(conn) && send_ctl->ctl_ack_eliciting_pkt > 0)
        || (out_of_order && send_ctl->ctl_ack_eliciting_pkt > 0))
    {
        path->path_flag |= RQC_PATH_FLAG_SHOULD_ACK;
        conn->ack_flag |= (1 << path->path_id);
        rqc_timer_unset(&send_ctl->path_timer_manager, RQC_TIMER_ACK);
    } else if (send_ctl->ctl_ack_eliciting_pkt > 0
         && !rqc_timer_is_set(&send_ctl->path_timer_manager, RQC_TIMER_ACK))
    {
        rqc_timer_set(&send_ctl->path_timer_manager, RQC_TIMER_ACK,
                      now, conn->local_settings.max_ack_delay * 1000);
    }
}

int
rqc_ack_sent_record_init(rqc_ack_sent_record_t *record)
{
    record->last_add_time = 0;
    record->ack_sent = rqc_rarray_create(8, sizeof(rqc_ack_sent_entry_t));
    if (!record->ack_sent) {
        return RQC_ERROR;
    }
    return RQC_OK;
}

void
rqc_ack_sent_record_destroy(rqc_ack_sent_record_t *record)
{
    if (record->ack_sent) {
        rqc_rarray_destroy(record->ack_sent);
        record->ack_sent = NULL;
    }
}

int
rqc_ack_sent_record_add(rqc_ack_sent_record_t *record, rqc_packet_out_t *packet_out, rqc_usec_t srtt, rqc_usec_t now)
{
    /* Record once per round trip */
    if (record->last_add_time + srtt > now) {
        return RQC_OK;
    }

    rqc_rarray_t *ra = record->ack_sent;

    if (rqc_rarray_full(ra)) {
        rqc_rarray_pop_back(ra);
    }

    rqc_ack_sent_entry_t *entry = rqc_rarray_push_front(ra);
    if (!entry) {
        return RQC_ERROR;
    }
    entry->pkt_num = packet_out->po_pkt.pkt_num;
    entry->largest_ack = packet_out->po_largest_ack;

    record->last_add_time = now;

    return RQC_OK;
}

rqc_packet_number_t
rqc_ack_sent_record_on_ack(rqc_ack_sent_record_t *record, rqc_ack_info_t *ack_info)
{
    rqc_pktno_range_t *range = &ack_info->ranges[0];
    rqc_rarray_t *ra = record->ack_sent;
    uint64_t size = rqc_rarray_size(ra);
    rqc_ack_sent_entry_t *entry;

    for (uint64_t i = 0; i < size; i++) {
        entry = rqc_rarray_get(ra, i);
        if (!entry) {
            return 0;
        }

        while (entry->pkt_num < range->low) {
            if (range == &ack_info->ranges[ack_info->n_ranges - 1]) {
                return 0;
            }
            ++range;
        }
        if (entry->pkt_num <= range->high && entry->pkt_num >= range->low) {
            rqc_rarray_pop_from(ra, i);
            return entry->largest_ack;
        }
    }
    return 0;
}
