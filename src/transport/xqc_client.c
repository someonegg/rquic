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
#include "src/tls/xqc_tls.h"

xqc_connection_t *
xqc_client_connect(xqc_engine_t *engine, const xqc_conn_settings_t *conn_settings,
    const char *server_host, int no_crypto_flag,
    const xqc_conn_ssl_config_t *conn_ssl_config, const char *alpn,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *user_data)
{
    xqc_cid_t dcid;
    xqc_cid_t scid;

    if (NULL == conn_ssl_config) {
        xqc_log(engine->log, XQC_LOG_ERROR,
                "|xqc_conn_ssl_config is NULL|");
        return NULL;
    }

    if (xqc_generate_cid(engine, NULL, &scid, 0) != XQC_OK
        || xqc_generate_cid(engine, NULL, &dcid, 0) != XQC_OK)
    {
        xqc_log(engine->log, XQC_LOG_ERROR,
                "|generate dcid or scid error|");
        return NULL;
    }

    xqc_connection_t *xc = xqc_client_create_connection(engine, dcid, scid, conn_settings,
                                                        server_host, no_crypto_flag,
                                                        conn_ssl_config, alpn, user_data);
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

const xqc_cid_t *
xqc_connect(xqc_engine_t *engine, const xqc_conn_settings_t *conn_settings,
    const char *server_host, int no_crypto_flag,
    const xqc_conn_ssl_config_t *conn_ssl_config, const struct sockaddr *peer_addr,
    socklen_t peer_addrlen, const char *alpn, void *user_data)
{
    xqc_connection_t *conn;

    if (NULL == alpn || strlen(alpn) > XQC_MAX_ALPN_LEN) {
        return NULL;
    }

    conn = xqc_client_connect(engine, conn_settings, server_host, no_crypto_flag,
                              conn_ssl_config, alpn, peer_addr, peer_addrlen, user_data);
    if (conn) {
        return &conn->scid_set.user_scid;
    }

    xqc_log(engine->log, XQC_LOG_ERROR, "|xqc_client_connect error|");
    return NULL;
}

xqc_int_t
xqc_client_create_tls(xqc_connection_t *conn, const xqc_conn_ssl_config_t *conn_ssl_config,
    const char *hostname, int no_crypto_flag, const char *alpn)
{
    xqc_int_t           ret;
    xqc_tls_config_t    cfg = {0};
    uint8_t             tp_buf[XQC_MAX_TRANSPORT_PARAM_BUF_LEN] = {0};
    uint8_t            *session_ticket_buf;
    uint8_t            *alpn_buf;
    size_t              alpn_cap;
    unsigned char      *hostname_buf;
    size_t              host_cap;

    /* init tls config */
    cfg.cert_verify_flag = conn_ssl_config->cert_verify_flag;
    cfg.no_crypto_flag = no_crypto_flag;

    /* copy alpn */
    alpn_cap = strlen(alpn) + 1;
    cfg.alpn = xqc_malloc(alpn_cap);
    if (NULL == cfg.alpn) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|malloc for alpn fail|");
        ret = -XQC_EMALLOC;
        goto end;
    }
    memcpy(cfg.alpn, alpn, alpn_cap);

    /* copy hostname */
    host_cap = strlen(hostname) + 1;
    cfg.hostname = xqc_malloc(host_cap);
    if (NULL == cfg.alpn) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|malloc for alpn fail|");
        ret = -XQC_EMALLOC;
        goto end;
    }
    memcpy(cfg.hostname, hostname, host_cap);

    /* encode local transport parameters, and set to tls config */
    cfg.trans_params = tp_buf;
    ret = xqc_conn_encode_local_tp(conn, cfg.trans_params,
                                   XQC_MAX_TRANSPORT_PARAM_BUF_LEN, &cfg.trans_params_len);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|encode transport parameter error|ret:%d", ret);
        goto end;
    }

    /* create tls instance */
    conn->tls = xqc_tls_create(conn->engine->tls_ctx, &cfg, conn->log, conn);
    if (NULL == conn->tls) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|create tls instance error");
        ret = -XQC_EMALLOC;
        goto end;
    }

    /* start handshake */
    ret = xqc_tls_init(conn->tls, conn->version, &conn->original_dcid);
    if (ret != XQC_OK) {
        xqc_log(conn->log, XQC_LOG_ERROR, "|init tls error");
        goto end;
    }

end:
    if (cfg.session_ticket) {
        xqc_free(cfg.session_ticket);
    }

    if (cfg.alpn) {
        xqc_free(cfg.alpn);
    }

    if (cfg.hostname) {
        xqc_free(cfg.hostname);
    }

    return ret;
}

xqc_connection_t *
xqc_client_create_connection(xqc_engine_t *engine, xqc_cid_t dcid, xqc_cid_t scid,
    const xqc_conn_settings_t *settings, const char *server_host, int no_crypto_flag,
    const xqc_conn_ssl_config_t *conn_ssl_config, const char *alpn, void *user_data)
{
    xqc_int_t               ret;
    xqc_transport_params_t  tp;
    xqc_trans_settings_t   *local_settings;

    xqc_connection_t *xc = xqc_conn_create(engine, &dcid, &scid, settings, user_data,
                                           XQC_CONN_TYPE_CLIENT);
    if (xc == NULL) {
        return NULL;
    }

    /* save odcid */
    xqc_cid_copy(&(xc->original_dcid), &(xc->dcid_set.current_dcid));

    /* create initial crypto stream, which MUST be created before tls for storing ClientHello */
    xc->crypto_stream[XQC_ENC_LEV_INIT] = xqc_create_crypto_stream(xc, XQC_ENC_LEV_INIT, user_data);
    if (!xc->crypto_stream[XQC_ENC_LEV_INIT]) {
        goto fail;
    }

    /* set no crypto option */
    local_settings = &xc->local_settings;
    if (no_crypto_flag == 1) {
        local_settings->no_crypto = XQC_TRUE;  /* no_crypto 1 means do not crypto*/

    } else {
        local_settings->no_crypto = XQC_FALSE;
    }

    /* create and init tls, startup ClientHello */
    if (xqc_client_create_tls(xc, conn_ssl_config, server_host, no_crypto_flag, alpn) != XQC_OK) {
        goto fail;
    }

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

