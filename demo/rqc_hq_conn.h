/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef RQC_HQ_CONN_H
#define RQC_HQ_CONN_H

#include "rqc_hq.h"

typedef struct rqc_hq_conn_s {

    rqc_hq_conn_callbacks_t     hqc_cbs;
    rqc_hq_request_callbacks_t  hqr_cbs;

    rqc_connection_t           *conn;

    rqc_log_t                  *log;

    void                       *user_data;

} rqc_hq_conn_s;

extern const rqc_conn_callbacks_t hq_conn_callbacks;

#endif