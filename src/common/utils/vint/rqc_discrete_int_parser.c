/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "rqc_discrete_int_parser.h"

void
rqc_discrete_int_pctx_clear(rqc_discrete_int_pctx_t *pctx)
{
    pctx->vi = 0;
    pctx->left = 0;
}

ssize_t
rqc_discrete_vint_parse(const uint8_t *p, size_t sz,
    rqc_discrete_int_pctx_t *pctx, rqc_bool_t *fin)
{
    *fin = RQC_FALSE;
    if (sz == 0) {
        return 0;
    }

    const uint8_t *pos = p;

    /* read the first byte */
    if (pctx->left == 0) {
        pctx->left = rqc_get_varint_len(p);
        pctx->vi = rqc_get_varint_fb(p);

        pctx->left--;
        sz--;
        pos++;
    }

    size_t cnt = rqc_min(pctx->left, sz);
    const uint8_t *end = pos + cnt;
    while (pos < end) {
        pctx->vi = (pctx->vi << 8) + *pos;
        pos++;
    }

    pctx->left -= cnt;
    if (pctx->left == 0) {
        *fin = RQC_TRUE;
    }

    return (pos - p);
}

ssize_t
rqc_fixed_len_int_parse(const uint8_t *p, size_t sz, uint8_t len,
    rqc_discrete_int_pctx_t *pctx, rqc_bool_t *fin)
{
    *fin = RQC_FALSE;
    if (sz == 0) {
        return 0;
    }

    const uint8_t *pos = p;

    /* set remain bytes state if it is the first byte now */
    if (pctx->left == 0) {
        pctx->left = len;
    }

    size_t cnt = rqc_min(pctx->left, sz);
    const uint8_t *end = pos + cnt;
    while (pos < end) {
        pctx->vi = (pctx->vi << 8) + *pos;
        pos++;
    }

    pctx->left -= cnt;
    if (pctx->left == 0) {
        *fin = RQC_TRUE;
    }

    return (pos - p);
}
