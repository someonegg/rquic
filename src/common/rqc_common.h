/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_COMMON_H_INCLUDED_
#define _RQC_COMMON_H_INCLUDED_

#include <string.h>
#include <stdint.h>
#include <rquic/rqc_errno.h>
#include <rquic/rquic_typedef.h>

#ifndef RQC_LITTLE_ENDIAN
# define RQC_LITTLE_ENDIAN 1
#endif

#ifndef RQC_NONALIGNED
# define RQC_NONALIGNED 1
#endif

typedef unsigned char   u_char;

#define rqc_calc_delay(a, b) ((a) ? (a) - (b) : 0)

#define RQC_POW2_UPPER_ERROR 0

static inline uint64_t
rqc_pow2_upper(uint64_t n)
{
    if (n > 0x8000000000000000) {
        /* return zero mean error */
        return RQC_POW2_UPPER_ERROR;
    }
    uint64_t m = 1;
    for(; m < n; m = m << 1);
    return m;
}

#endif /*_RQC_COMMON_H_INCLUDED_*/
