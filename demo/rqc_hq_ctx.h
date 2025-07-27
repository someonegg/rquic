/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef RQC_HQ_CTX_H
#define RQC_HQ_CTX_H

#include "rqc_hq.h"

rqc_int_t rqc_hq_ctx_get_callbacks(rqc_engine_t *engine, char *alpn, size_t alpn_len, rqc_hq_callbacks_t **hq_cbs);

#endif
