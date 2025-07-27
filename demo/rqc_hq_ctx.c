/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "rqc_hq_ctx.h"
#include "rqc_hq.h"
#include "rqc_hq_defs.h"
#include "rqc_hq_conn.h"
#include "rqc_hq_request.h"
#include "src/common/rqc_malloc.h"
#include <rquic/rqc_errno.h>

typedef struct rqc_hq_ctx_s {
    rqc_hq_callbacks_t  hq_cbs;
} rqc_hq_ctx_t;

rqc_hq_ctx_t*
rqc_hq_ctx_create(rqc_hq_callbacks_t *hq_cbs)
{
    rqc_hq_ctx_t *hq_ctx = NULL;

    hq_ctx = rqc_malloc(sizeof(rqc_hq_callbacks_t));
    if (hq_ctx) {
        hq_ctx->hq_cbs = *hq_cbs;
    }

    return hq_ctx;
}

rqc_int_t
rqc_hq_ctx_init(rqc_engine_t *engine, rqc_hq_callbacks_t *hq_cbs)
{
    if (engine == NULL || hq_cbs == NULL) {
        return -RQC_EPARAM;
    }

    rqc_hq_ctx_t *hq_ctx;
    rqc_int_t ret = RQC_OK;
    rqc_app_proto_callbacks_t ap_cbs = {
        .conn_cbs   = hq_conn_callbacks,
        .stream_cbs = hq_stream_callbacks
    };

    hq_ctx = rqc_hq_ctx_create(hq_cbs);
    if (hq_ctx == NULL) {
        ret = -RQC_EMALLOC;
        goto error;
    }

    /* register ALPN and Application-Layer-Protocol callbacks */
    if (rqc_engine_register_alpn(engine, RQC_ALPN_HQ_INTEROP, RQC_ALPN_HQ_INTEROP_LEN, &ap_cbs, hq_ctx) != RQC_OK) {
        rqc_free(hq_ctx);
        ret = -RQC_EFATAL;
        goto error;
    }

    hq_ctx = rqc_hq_ctx_create(hq_cbs);
    if (hq_ctx == NULL) {
        ret = -RQC_EMALLOC;
        goto error;
    }

    /* register ALPN and Application-Layer-Protocol callbacks */
    if (rqc_engine_register_alpn(engine, RQC_ALPN_HQ_29, RQC_ALPN_HQ_29_LEN, &ap_cbs, hq_ctx) != RQC_OK) {
        rqc_free(hq_ctx);
        ret = -RQC_EFATAL;
        goto error;
    }

    return ret;
error:
    rqc_hq_ctx_destroy(engine);
    return ret;
}

rqc_int_t
rqc_hq_ctx_destroy(rqc_engine_t *engine)
{
    rqc_hq_ctx_t *hq_ctx;

    hq_ctx = rqc_engine_get_alpn_ctx(engine, RQC_ALPN_HQ_29, RQC_ALPN_HQ_29_LEN);
    if (hq_ctx) {
        rqc_free(hq_ctx);
    }

    hq_ctx = rqc_engine_get_alpn_ctx(engine, RQC_ALPN_HQ_INTEROP, RQC_ALPN_HQ_INTEROP_LEN);
    if (hq_ctx) {
        rqc_free(hq_ctx);
    }

    rqc_engine_unregister_alpn(engine, RQC_ALPN_HQ_29, RQC_ALPN_HQ_29_LEN);
    rqc_engine_unregister_alpn(engine, RQC_ALPN_HQ_INTEROP, RQC_ALPN_HQ_INTEROP_LEN);
    return RQC_OK;
}

rqc_int_t
rqc_hq_ctx_get_callbacks(rqc_engine_t *engine, char *alpn, size_t alpn_len, rqc_hq_callbacks_t **hq_cbs)
{
    rqc_hq_ctx_t *hq_ctx;

    hq_ctx = rqc_engine_get_alpn_ctx(engine, RQC_ALPN_HQ_29, RQC_ALPN_HQ_29_LEN);

    if (hq_ctx == NULL) {
        return -RQC_EFATAL;
    }

    *hq_cbs = &hq_ctx->hq_cbs;
    return RQC_OK;
}