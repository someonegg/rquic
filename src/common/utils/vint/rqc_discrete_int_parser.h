/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_DISCRETE_INT_PARSER_H_
#define _RQC_DISCRETE_INT_PARSER_H_

#include "include/rquic/rquic_typedef.h"
#include "rqc_variable_len_int.h"
#include "src/common/rqc_config.h"
#include "src/common/rqc_common.h"
#include <sys/types.h>

/* state for parsing discrete int */
typedef struct {
    uint64_t    vi;     /* temporary or final value */
    size_t      left;   /* bytes needs to be read to finish the discrete int */
} rqc_discrete_int_pctx_t;

/**
 * @brief reset parse context
 */
void rqc_discrete_int_pctx_clear(rqc_discrete_int_pctx_t *pctx);

/**
 * @brief parse vint from buffers which might be truncated
 * @param p:    input buffer
 * @param sz:   buffer size
 * @param st:   parse state
 * @param fin:  output for parse finished
 */
ssize_t rqc_discrete_vint_parse(const uint8_t *p, size_t sz,
    rqc_discrete_int_pctx_t *pctx, rqc_bool_t *fin);

/**
 * @brief parse fixed len int from buffers which might be truncated
 */
ssize_t rqc_fixed_len_int_parse(const uint8_t *p, size_t sz, uint8_t len,
    rqc_discrete_int_pctx_t *pctx, rqc_bool_t *fin);

#endif
