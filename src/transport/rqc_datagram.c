#include "src/transport/rqc_datagram.h"
#include "src/transport/rqc_engine.h"
#include "src/transport/rqc_send_queue.h"
#include "src/transport/rqc_packet_parser.h"
#include "src/common/utils/vint/rqc_variable_len_int.h"

static rqc_bool_t
rqc_datagram_open(rqc_connection_t *conn)
{
    return conn->conn_state < RQC_CONN_STATE_CLOSING
        && !(conn->conn_flag & (RQC_CONN_IMMEDIATE_CLOSE_FLAGS | RQC_CONN_FLAG_LINGER_CLOSING));
}

size_t
rqc_datagram_get_mss(rqc_connection_t *conn)
{
    if (!conn || !rqc_conn_is_established(conn)
        || !conn->remote_settings.max_datagram_frame_size) {
        return 0;
    }
    /* rquic has a length field in its short header and no encryption tag. */
    size_t header = rqc_short_packet_header_size(conn->dcid_set.current_dcid.cid_len, RQC_PKTNO_CLASS);
    uint64_t udp_limit = rqc_min(conn->local_settings.max_udp_payload_size,
                                 conn->remote_settings.max_udp_payload_size);
    if (udp_limit < RQC_PACKET_OUT_EXT_SPACE) { return 0; }
    uint64_t packet_limit = rqc_min(conn->pkt_out_size, udp_limit - RQC_PACKET_OUT_EXT_SPACE);
    if (packet_limit <= header) { return 0; }
    size_t frame_limit = (size_t)rqc_min(packet_limit - header,
                                      conn->remote_settings.max_datagram_frame_size);
    if (frame_limit < 2) { return 0; }
    size_t len = frame_limit - 2;
    while (len + 1 + rqc_put_varint_len(len) > frame_limit) { --len; }
    return len;
}

void
rqc_datagram_set_user_data(rqc_connection_t *conn, void *data)
{
    if (conn) { conn->datagram_user_data = data; }
}

void *
rqc_datagram_get_user_data(rqc_connection_t *conn)
{
    return conn ? conn->datagram_user_data : NULL;
}

static rqc_int_t
rqc_datagram_submit(rqc_connection_t *conn, const void *data, size_t len, uint64_t *id)
{
    if (!conn || (!data && len)) { return -RQC_EPARAM; }
    if (!rqc_datagram_open(conn)) { return -RQC_CLOSING; }
    if (!rqc_conn_is_established(conn)) {
        conn->datagram_wait_write = 1;
        return -RQC_EAGAIN;
    }
    if (!conn->remote_settings.max_datagram_frame_size) {
        return -RQC_EDATAGRAM_NOT_SUPPORTED;
    }
    size_t mss = rqc_datagram_get_mss(conn);
    /* An MSS of zero can still permit an empty message (two-byte frame). */
    size_t header = rqc_short_packet_header_size(conn->dcid_set.current_dcid.cid_len, RQC_PKTNO_CLASS);
    if (len > mss || conn->remote_settings.max_datagram_frame_size < 2
        || conn->pkt_out_size < header + 2
        || conn->local_settings.max_udp_payload_size < header + 2 + RQC_PACKET_OUT_EXT_SPACE
        || conn->remote_settings.max_udp_payload_size < header + 2 + RQC_PACKET_OUT_EXT_SPACE) {
        return -RQC_EDATAGRAM_TOO_LARGE;
    }
    if (!rqc_send_queue_can_write(conn->conn_send_queue)) {
        conn->datagram_wait_write = 1;
        return -RQC_EAGAIN;
    }
    if (conn->datagram_next_id == UINT64_MAX) { return -RQC_ELIMIT; }
    /* Allocate tracking at submission so ACK/loss never needs allocation. */
    rqc_datagram_record_t *record = rqc_calloc(1, sizeof(*record));
    if (!record) { return -RQC_EMALLOC; }
    rqc_conn_check_app_limit(conn);
    rqc_packet_out_t *po = rqc_write_new_packet(conn, RQC_PTYPE_SHORT_HEADER);
    if (!po) { rqc_free(record); return -RQC_EMALLOC; }
    /* Reserve the usual ACK budget without exceeding either UDP limit. */
    po->po_buf_size = (unsigned int)rqc_min(po->po_buf_size,
        rqc_min(conn->local_settings.max_udp_payload_size,
                conn->remote_settings.max_udp_payload_size) - RQC_PACKET_OUT_EXT_SPACE);
    unsigned char *p = po->po_buf + po->po_used_size;
    *p++ = 0x31;
    p = rqc_put_varint(p, len);
    if (len) { rqc_memcpy(p, data, len); }
    po->po_used_size = (unsigned int)(p - po->po_buf + len);
    po->po_frame_types |= RQC_FRAME_BIT_DATAGRAM;
    record->id = conn->datagram_next_id++;
    po->po_datagram = record;
    if (id) { *id = record->id; }
    return RQC_OK;
}

static void
rqc_datagram_commit(rqc_connection_t *conn, uint8_t flush)
{
    if (!conn || !rqc_datagram_open(conn)) { return; }
    rqc_engine_remove_wakeup_queue(conn->engine, conn);
    rqc_engine_add_active_queue(conn->engine, conn);
    if (flush) { rqc_engine_conn_logic(conn->engine, conn); }
}

rqc_int_t
rqc_datagram_send(rqc_connection_t *conn, const void *data, size_t len, uint64_t *id, uint8_t flush)
{
    if (!conn || (!data && len) || flush > 1) { return -RQC_EPARAM; }
    rqc_int_t ret = rqc_datagram_submit(conn, data, len, id);
    rqc_datagram_commit(conn, flush);
    return ret;
}

rqc_int_t
rqc_datagram_send_multiple(rqc_connection_t *conn, const rquic_iovec_t *messages,
    size_t count, uint64_t *ids, size_t *accepted_count, uint8_t flush)
{
    if (accepted_count) { *accepted_count = 0; }
    if (!accepted_count || !conn || (!messages && count) || flush > 1) { return -RQC_EPARAM; }
    rqc_int_t ret = rqc_datagram_open(conn) ? RQC_OK : -RQC_CLOSING;
    for (size_t i = 0; ret == RQC_OK && i < count; ++i) {
        ret = rqc_datagram_submit(conn, messages[i].data, messages[i].len, ids ? &ids[i] : NULL);
        if (ret == RQC_OK) { ++*accepted_count; }
    }
    rqc_datagram_commit(conn, flush);
    return ret;
}

rqc_int_t
rqc_datagram_process_frame(rqc_connection_t *conn, rqc_packet_in_t *pi)
{
    unsigned char *start = pi->pos, *p = start;
    uint64_t type, len;
    ssize_t n = rqc_vint_read(p, pi->last, &type);
    if (n < 0) { goto malformed; }
    p += n;
    if (type == 0x31) {
        n = rqc_vint_read(p, pi->last, &len);
        if (n < 0) { goto malformed; }
        p += n;
        if (len > (uint64_t)(pi->last - p)) { goto malformed; }
    } else if (type == 0x30) {
        len = pi->last - p;
    } else { goto malformed; }
    if (!rqc_conn_is_handshake_recvd(conn) || pi->pi_pkt.pkt_type != RQC_PTYPE_SHORT_HEADER
        || !conn->local_settings.max_datagram_frame_size
        || (uint64_t)(p - start) + len > conn->local_settings.max_datagram_frame_size) {
        RQC_CONN_ERR(conn, TRA_PROTOCOL_VIOLATION);
        return -RQC_EPROTO;
    }
    pi->pos = p + len;
    pi->pi_frame_types |= RQC_FRAME_BIT_DATAGRAM;
    if (rqc_datagram_open(conn) && conn->app_proto_cbs.dgram_cbs.datagram_read_notify) {
        rqc_bool_t was_running = conn->engine->eng_flag & RQC_ENG_FLAG_RUNNING;
        conn->engine->eng_flag |= RQC_ENG_FLAG_RUNNING;
        conn->datagram_callback_running = 1;
        conn->app_proto_cbs.dgram_cbs.datagram_read_notify(conn, p, (size_t)len, conn->datagram_user_data);
        conn->datagram_callback_running = 0;
        if (!was_running) { conn->engine->eng_flag &= ~RQC_ENG_FLAG_RUNNING; }
    }
    return RQC_OK;
malformed:
    RQC_CONN_ERR(conn, TRA_FRAME_ENCODING_ERROR);
    return -RQC_EILLFRAME;
}

void
rqc_datagram_record_feedback(rqc_connection_t *conn, rqc_packet_out_t *po, rqc_bool_t lost)
{
    rqc_datagram_record_t *record = po->po_datagram;
    if (!record) { return; }
    po->po_datagram = NULL;
    record->lost = lost;
    record->next = NULL;
    if (conn->datagram_feedback_tail) { conn->datagram_feedback_tail->next = record; }
    else { conn->datagram_feedback_head = record; }
    conn->datagram_feedback_tail = record;
}

void
rqc_datagram_cleanup(rqc_connection_t *conn)
{
    while (conn->datagram_feedback_head) {
        rqc_datagram_record_t *record = conn->datagram_feedback_head;
        conn->datagram_feedback_head = record->next;
        rqc_free(record);
    }
    conn->datagram_feedback_tail = NULL;
}

void
rqc_datagram_process_callbacks(rqc_connection_t *conn)
{
    if (!rqc_datagram_open(conn)) { rqc_datagram_cleanup(conn); return; }
    rqc_datagram_callbacks_t *cbs = &conn->app_proto_cbs.dgram_cbs;
    rqc_bool_t was_running = conn->engine->eng_flag & RQC_ENG_FLAG_RUNNING;
    conn->engine->eng_flag |= RQC_ENG_FLAG_RUNNING;
    conn->datagram_callback_running = 1;
    if (rqc_conn_is_established(conn)) {
        size_t mss = rqc_datagram_get_mss(conn);
        if (!conn->datagram_mss_known || conn->datagram_mss != mss) {
            conn->datagram_mss = mss;
            conn->datagram_mss_known = 1;
            if (cbs->datagram_mss_updated_notify) {
                cbs->datagram_mss_updated_notify(conn, mss, conn->datagram_user_data);
            }
        }
        /* Notify also on a negative negotiation result so blocked callers can
         * retry and observe NOT_SUPPORTED instead of waiting indefinitely. */
        if (rqc_datagram_open(conn) && conn->datagram_wait_write
            && (!conn->remote_settings.max_datagram_frame_size
                || rqc_send_queue_can_write(conn->conn_send_queue))) {
            conn->datagram_wait_write = 0;
            if (cbs->datagram_write_notify) { cbs->datagram_write_notify(conn, conn->datagram_user_data); }
        }
    }
    while (rqc_datagram_open(conn) && conn->datagram_feedback_head) {
        rqc_datagram_record_t *record = conn->datagram_feedback_head;
        conn->datagram_feedback_head = record->next;
        if (!record->next) { conn->datagram_feedback_tail = NULL; }
        uint64_t id = record->id;
        rqc_bool_t lost = record->lost;
        rqc_free(record);
        if (lost) {
            if (cbs->datagram_lost_notify) { cbs->datagram_lost_notify(conn, id, conn->datagram_user_data); }
        } else {
            if (cbs->datagram_acked_notify) { cbs->datagram_acked_notify(conn, id, conn->datagram_user_data); }
        }
    }
    conn->datagram_callback_running = 0;
    if (!was_running) { conn->engine->eng_flag &= ~RQC_ENG_FLAG_RUNNING; }
    if (!rqc_datagram_open(conn)) { rqc_datagram_cleanup(conn); }
}
