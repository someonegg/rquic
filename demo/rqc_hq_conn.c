/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "rqc_hq_conn.h"
#include "rqc_hq_defs.h"
#include "rqc_hq_ctx.h"

#include "src/common/rqc_common_inc.h"
#include "src/transport/rqc_engine.h"
#include "src/transport/rqc_conn.h"

rqc_hq_conn_t *
rqc_hq_conn_create(rqc_connection_t *conn, const rqc_cid_t *cid, void *user_data)
{
    rqc_hq_conn_t *hqc = rqc_calloc(1, sizeof(rqc_hq_conn_t));
    if (NULL == hqc) {
        return NULL;
    }

    rqc_hq_callbacks_t *hq_cbs = NULL;
    rqc_int_t ret;

    ret = rqc_hq_ctx_get_callbacks(conn->engine, conn->alpn, conn->alpn_len, &hq_cbs);

    if (ret != RQC_OK || hq_cbs == NULL) {
        PRINT_LOG("|create hq conn failed");
        rqc_free(hqc);
        return NULL;
    }

    hqc->user_data = user_data;
    hqc->log = conn->log;
    hqc->conn = conn;
    hqc->hqc_cbs = hq_cbs->hqc_cbs;
    hqc->hqr_cbs = hq_cbs->hqr_cbs;

    rqc_conn_set_alp_user_data(conn, hqc);

    return hqc;
}

void
rqc_hq_conn_destroy(rqc_hq_conn_t *hqc)
{
    if (hqc) {
        rqc_free(hqc);
    }
}

const rqc_cid_t*
rqc_hq_connect(rqc_engine_t *engine,
    const rqc_conn_settings_t *conn_settings,
    const char *server_host,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *user_data)
{
    /* HQ is also known as HTTP/0.9, here it is used as interop protocol */
    const rqc_cid_t *cid = rqc_connect(engine, conn_settings,
        server_host, rqc_hq_alpn[conn_settings->proto_version],
        NULL,
        peer_addr, peer_addrlen, user_data);

    return cid;
}

rqc_int_t
rqc_hq_conn_close(rqc_engine_t *engine, rqc_hq_conn_t *hqc, const rqc_cid_t *cid)
{
    return rqc_conn_close(engine, cid);
}

void
rqc_hq_conn_set_user_data(rqc_hq_conn_t *hqc, void *user_data)
{
    hqc->user_data = user_data;
}

rqc_int_t
rqc_hq_conn_get_peer_addr(rqc_hq_conn_t *hqc, struct sockaddr *addr, socklen_t addr_cap,
    socklen_t *peer_addr_len)
{
    return rqc_conn_get_peer_addr(hqc->conn, addr, addr_cap, peer_addr_len);
}

rqc_int_t
rqc_hq_conn_create_notify(rqc_connection_t *conn, const rqc_cid_t *cid,
    void *conn_user_data, void *conn_proto_data,
    const rqc_proto_ext_t *proto_ext, rqc_proto_ext_t *resp_proto_ext)
{
    (void)proto_ext;
    (void)resp_proto_ext;
    /* here conn_user_data is the app-layer user_data */
    rqc_hq_conn_t *hqc = rqc_hq_conn_create(conn, cid, conn_user_data);
    if (NULL == hqc) {
        PRINT_LOG("|create hq conn failed");
        return -RQC_EMALLOC;
    }

    if (hqc->hqc_cbs.conn_create_notify) {
        /* NOTICE: if hqc is created passively, hqc->user_data is NULL */
        return hqc->hqc_cbs.conn_create_notify(hqc, cid, hqc->user_data);
    }

    return RQC_OK;
}

rqc_int_t
rqc_hq_conn_close_notify(rqc_connection_t *conn, const rqc_cid_t *cid,
    void *conn_user_data, void *conn_proto_data)
{
    rqc_int_t ret = RQC_OK;

    rqc_hq_conn_t *hqc = (rqc_hq_conn_t *)conn_proto_data;
    if (hqc->hqc_cbs.conn_close_notify) {
        ret = hqc->hqc_cbs.conn_close_notify(hqc, cid, hqc->user_data);
        if (ret != RQC_OK) {
            return ret;
        }
    }

    rqc_hq_conn_destroy(hqc);

    return RQC_OK;
}

void
rqc_hq_conn_handshake_finished(rqc_connection_t *conn, void *conn_user_data,
    void *conn_proto_data)
{
    return;
}

/* connection callback over quic Transport layere */
const rqc_conn_callbacks_t hq_conn_callbacks = {
    .conn_create_notify         = rqc_hq_conn_create_notify,
    .conn_close_notify          = rqc_hq_conn_close_notify,
    .conn_handshake_finished    = rqc_hq_conn_handshake_finished,
};
