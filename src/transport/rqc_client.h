/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_CLIENT_H_INCLUDED_
#define _RQC_CLIENT_H_INCLUDED_

#include <rquic/rquic_typedef.h>

rqc_connection_t *rqc_client_connect(rqc_engine_t *engine,
    const rqc_conn_settings_t *conn_settings,
    const char *server_host, const char *alpn, const rqc_proto_ext_t *proto_ext,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *user_data);

rqc_connection_t *rqc_client_create_connection(rqc_engine_t *engine,
    rqc_cid_t dcid, rqc_cid_t scid,
    const rqc_conn_settings_t *settings,
    const char *server_host, const char *alpn, const rqc_proto_ext_t *proto_ext,
    void *user_data);

#endif /* _RQC_CLIENT_H_INCLUDED_ */
