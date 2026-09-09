/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include <rquic/rquic.h>
#include "src/transport/rqc_engine.h"
#include "src/transport/rqc_client.h"
#include "src/transport/rqc_cid.h"
#include "src/transport/rqc_conn.h"
#include "src/transport/rqc_stream.h"
#include "src/transport/rqc_multipath.h"
#include "src/transport/rqc_utils.h"
#include "src/transport/rqc_defs.h"

rqc_connection_t *
rqc_client_connect(rqc_engine_t *engine,
    const rqc_conn_settings_t *conn_settings,
    const char *server_host, const char *alpn, const rqc_proto_ext_t *proto_ext,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *user_data)
{
    rqc_cid_t dcid;
    rqc_cid_t scid;

    if (rqc_generate_cid(engine, NULL, &scid, 0) != RQC_OK
        || rqc_generate_cid(engine, NULL, &dcid, 0) != RQC_OK)
    {
        rqc_log(engine->log, RQC_LOG_ERROR,
                "|generate dcid or scid error|");
        return NULL;
    }

    rqc_connection_t *xc = rqc_client_create_connection(engine, dcid, scid, conn_settings,
                                                        server_host, alpn, proto_ext, user_data);
    if (xc == NULL) {
        rqc_log(engine->log, RQC_LOG_ERROR,
                "|create connection error|");
        return NULL;
    }

    if (peer_addr && peer_addrlen > 0) {
        xc->peer_addrlen = peer_addrlen;
        memcpy(xc->peer_addr, peer_addr, peer_addrlen);
    }

    if (rqc_conn_client_init_path_addr(xc) != RQC_OK) {
        goto fail;
    }

    rqc_log_event(xc->log, CON_CONNECTION_STARTED, xc, RQC_LOG_REMOTE_EVENT);

    rqc_engine_remove_wakeup_queue(engine, xc);
    if (rqc_engine_add_active_queue(engine, xc) != RQC_OK) {
        goto fail;
    }

    /* All fallible setup must finish before delivering the connection. */
    /* conn_create callback */
    if (xc->app_proto_cbs.conn_cbs.conn_create_notify) {
        if (xc->app_proto_cbs.conn_cbs.conn_create_notify(xc, &xc->scid_set.user_scid,
                user_data, NULL, NULL, NULL)) {
            rqc_log(engine->log, RQC_LOG_INFO, "|destroy conn as create_notify return failure|conn:%p|%s",
                    xc, rqc_conn_addr_str(xc));
            goto fail;
        }

        xc->conn_flag |= RQC_CONN_FLAG_UPPER_CONN_EXIST;
    }

    rqc_engine_conn_logic(engine, xc);

    /* when the connection is destroyed in the main logic, we should return error to upper level */
    if (rqc_engine_conns_hash_find(engine, &scid, 's') == NULL) {
        return NULL;
    }

    return xc;

fail:
    rqc_engine_remove_active_queue(engine, xc);
    rqc_engine_remove_wakeup_queue(engine, xc);
    rqc_conn_destroy(xc);
    return NULL;
}

const rqc_cid_t *rqc_connect(rqc_engine_t *engine,
    const rqc_conn_settings_t *conn_settings,
    const char *server_host, const char *alpn, const rqc_proto_ext_t *proto_ext,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *user_data)
{
    rqc_connection_t *conn;

    if (NULL == alpn || strlen(alpn) > RQC_MAX_ALPN_LEN) {
        return NULL;
    }

    if (proto_ext && proto_ext->len > RQC_MAX_PROTO_EXT_LEN) {
        return NULL;
    }

    conn = rqc_client_connect(engine, conn_settings, server_host, alpn,
                              proto_ext, peer_addr, peer_addrlen, user_data);
    if (conn) {
        return &conn->scid_set.user_scid;
    }

    rqc_log(engine->log, RQC_LOG_ERROR, "|rqc_client_connect error|");
    return NULL;
}

rqc_connection_t *
rqc_client_create_connection(rqc_engine_t *engine,
    rqc_cid_t dcid, rqc_cid_t scid,
    const rqc_conn_settings_t *settings,
    const char *server_host, const char *alpn, const rqc_proto_ext_t *proto_ext,
    void *user_data)
{
    rqc_connection_t *xc = rqc_conn_create(engine, &dcid, &scid, settings, user_data,
                                           RQC_CONN_TYPE_CLIENT);
    if (xc == NULL) {
        return NULL;
    }

    /* save odcid */
    rqc_cid_copy(&(xc->original_dcid), &(xc->dcid_set.current_dcid));

    if (rqc_conn_client_on_alpn(xc, alpn, strlen(alpn)) != RQC_OK) {
        goto fail;
    }

    if (proto_ext && proto_ext->len > 0) {
        rqc_memcpy(xc->self_proto_ext.data, proto_ext->data, proto_ext->len);
        xc->self_proto_ext.len = proto_ext->len;
    }

    if (rqc_conn_send_handshake(xc) != RQC_OK) {
        goto fail;
    }

    return xc;

fail:
    rqc_log(xc->log, RQC_LOG_INFO, "|destroy conn as create failure|conn:%p|%s",
            xc, rqc_conn_addr_str(xc));
    rqc_conn_destroy(xc);
    return NULL;
}
