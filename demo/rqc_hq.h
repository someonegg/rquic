/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef RQC_HQ_H
#define RQC_HQ_H

#include <rquic/rquic.h>
#include <rquic/rquic_typedef.h>
#include <rquic/rqc_errno.h>

typedef struct rqc_hq_conn_s    rqc_hq_conn_t;
typedef struct rqc_hq_request_s rqc_hq_request_t;

typedef int (*rqc_hq_conn_notify_pt)(rqc_hq_conn_t *conn, const rqc_cid_t *cid,
    void *conn_user_data);

/**
 * @brief application-layer-protocol callback funcitons for hq
 */
typedef struct rqc_hq_conn_callbacks_s {

    /**
     * connection create notify callback. REQUIRED for server, OPTIONAL for client.
     */
    rqc_hq_conn_notify_pt               conn_create_notify;

    /**
     * connection close notify. REQUIRED for both client and server
     */
    rqc_hq_conn_notify_pt               conn_close_notify;

} rqc_hq_conn_callbacks_t;

typedef int (*rqc_hq_req_create_notify_pt)(rqc_hq_request_t *hq_req, void *req_user_data);

typedef int (*rqc_hq_req_close_notify_pt)(rqc_hq_request_t *hq_req, void *req_user_data);

typedef int (*rqc_hq_req_read_notify_pt)(rqc_hq_request_t *hq_req, void *req_user_data);

typedef int (*rqc_hq_req_write_notify_pt)(rqc_hq_request_t *hq_req, void *req_user_data);

typedef struct rqc_hq_request_callbacks_s {
    /**
     * stream create callback function. REQUIRED for server, OPTIONAL for client.
     */
    rqc_hq_req_create_notify_pt         req_create_notify;

    /**
     * stream close callback function. REQUIRED for both server and client.
     */
    rqc_hq_req_close_notify_pt          req_close_notify;

    /**
     * hq request read callback function. REQUIRED for both client and server
     */
    rqc_hq_req_read_notify_pt           req_read_notify;

    /**
     * stream write callback function. REQUIRED for both client and server
     */
    rqc_hq_req_write_notify_pt          req_write_notify;

} rqc_hq_request_callbacks_t;

/**
 * @brief hq callbacks
 */
typedef struct rqc_hq_callbacks_s {

    /* hq connection callbacks */
    rqc_hq_conn_callbacks_t     hqc_cbs;

    /* hq request callbacks */
    rqc_hq_request_callbacks_t  hqr_cbs;

} rqc_hq_callbacks_t;

/**
 * @brief init the environment of hq, MUST be invoked before create hq connection
 */
rqc_int_t
rqc_hq_ctx_init(rqc_engine_t *engine, rqc_hq_callbacks_t *hq_cbs);

rqc_int_t
rqc_hq_ctx_destroy(rqc_engine_t *engine);

/**
 * @brief hq connection functions
 */

const rqc_cid_t *
rqc_hq_connect(rqc_engine_t *engine,
    const rqc_conn_settings_t *conn_settings,
    const char *server_host,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *user_data);

rqc_int_t
rqc_hq_conn_close(rqc_engine_t *engine, rqc_hq_conn_t *hqc, const rqc_cid_t *cid);

void
rqc_hq_conn_set_user_data(rqc_hq_conn_t *hqc, void *user_data);

rqc_int_t
rqc_hq_conn_get_peer_addr(rqc_hq_conn_t *hqc, struct sockaddr *addr, socklen_t addr_cap,
    socklen_t *peer_addr_len);

/**
 * @brief hq request functions
 */

rqc_hq_request_t *
rqc_hq_request_create(rqc_engine_t *engine, rqc_hq_conn_t *hqc, const rqc_cid_t *cid,
    void *user_data);

void
rqc_hq_request_destroy(rqc_hq_request_t * hqr);

void rqc_hq_request_set_user_data(rqc_hq_request_t *hqr, void *user_data);

ssize_t
rqc_hq_request_send_req(rqc_hq_request_t *hqr, const char *resource);

ssize_t
rqc_hq_request_recv_req(rqc_hq_request_t *hqr, char *res_buf, size_t buf_sz, uint8_t *fin);

ssize_t
rqc_hq_request_send_rsp(rqc_hq_request_t *hqr, const uint8_t *res_buf, size_t res_buf_len,
    uint8_t fin);

ssize_t
rqc_hq_request_recv_rsp(rqc_hq_request_t *hqr, char *res_buf, size_t buf_sz, uint8_t *fin);

rqc_int_t
rqc_hq_request_close(rqc_hq_request_t *hqr);

rqc_stream_stats_t
rqc_hq_request_get_stats(rqc_hq_request_t *hqr);

#endif
