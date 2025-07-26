/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include <xquic/xquic.h>
#include "src/transport/xqc_engine.h"
#include "src/transport/xqc_client.h"
#include "src/transport/xqc_cid.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_stream.h"
#include "src/transport/xqc_multipath.h"
#include "src/transport/xqc_utils.h"
#include "src/transport/xqc_defs.h"

xqc_connection_t *
xqc_client_connect(xqc_engine_t *engine,
    const xqc_conn_settings_t *conn_settings,
    const char *server_host, const char *alpn,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *user_data)
{
    xqc_cid_t dcid;
    xqc_cid_t scid;

    if (xqc_generate_cid(engine, NULL, &scid, 0) != XQC_OK
        || xqc_generate_cid(engine, NULL, &dcid, 0) != XQC_OK)
    {
        xqc_log(engine->log, XQC_LOG_ERROR,
                "|generate dcid or scid error|");
        return NULL;
    }

    xqc_connection_t *xc = xqc_client_create_connection(engine, dcid, scid, conn_settings,
                                                        server_host, alpn, user_data);
    if (xc == NULL) {
        xqc_log(engine->log, XQC_LOG_ERROR,
                "|create connection error|");
        return NULL;
    }

    if (peer_addr && peer_addrlen > 0) {
        xc->peer_addrlen = peer_addrlen;
        memcpy(xc->peer_addr, peer_addr, peer_addrlen);
    }

    if (xqc_conn_client_init_path_addr(xc) != XQC_OK) {
        return NULL;
    }

    xqc_log_event(xc->log, CON_CONNECTION_STARTED, xc, XQC_LOG_REMOTE_EVENT);

    /* conn_create callback */
    if (xc->app_proto_cbs.conn_cbs.conn_create_notify) {
        if (xc->app_proto_cbs.conn_cbs.conn_create_notify(xc, &xc->scid_set.user_scid, user_data, NULL)) {
            xqc_log(engine->log, XQC_LOG_INFO, "|destroy conn as create_notify return failure|conn:%p|%s",
                    xc, xqc_conn_addr_str(xc));
            xqc_conn_destroy(xc);
            return NULL;
        }

        xc->conn_flag |= XQC_CONN_FLAG_UPPER_CONN_EXIST;
    }

    xqc_engine_remove_wakeup_queue(engine, xc);

    if (xqc_engine_add_active_queue(engine, xc) != XQC_OK) {
        return NULL;
    }

    xqc_engine_conn_logic(engine, xc);

    /* when the connection is destroyed in the main logic, we should return error to upper level */
    if (xqc_engine_conns_hash_find(engine, &scid, 's') == NULL) {
        return NULL;
    }

    return xc;
}

const xqc_cid_t *xqc_connect(xqc_engine_t *engine,
    const xqc_conn_settings_t *conn_settings,
    const char *server_host, const char *alpn,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *user_data)
{
    xqc_connection_t *conn;

    if (NULL == alpn || strlen(alpn) > XQC_MAX_ALPN_LEN) {
        return NULL;
    }

    conn = xqc_client_connect(engine, conn_settings, server_host, alpn,
                              peer_addr, peer_addrlen, user_data);
    if (conn) {
        return &conn->scid_set.user_scid;
    }

    xqc_log(engine->log, XQC_LOG_ERROR, "|xqc_client_connect error|");
    return NULL;
}

xqc_connection_t *
xqc_client_create_connection(xqc_engine_t *engine,
    xqc_cid_t dcid, xqc_cid_t scid,
    const xqc_conn_settings_t *settings,
    const char *server_host, const char *alpn,
    void *user_data)
{
    // TODOXXXX
    // xqc_int_t               ret;
    // xqc_transport_params_t  tp;
    // xqc_trans_settings_t   *local_settings;

    xqc_connection_t *xc = xqc_conn_create(engine, &dcid, &scid, settings, user_data,
                                           XQC_CONN_TYPE_CLIENT);
    if (xc == NULL) {
        return NULL;
    }

    /* save odcid */
    xqc_cid_copy(&(xc->original_dcid), &(xc->dcid_set.current_dcid));

    if (xqc_conn_client_on_alpn(xc, alpn, strlen(alpn)) != XQC_OK) {
        goto fail;
    }

    return xc;

fail:
    xqc_log(xc->log, XQC_LOG_INFO, "|destroy conn as create failure|conn:%p|%s",
            xc, xqc_conn_addr_str(xc));
    xqc_conn_destroy(xc);
    return NULL;
}
