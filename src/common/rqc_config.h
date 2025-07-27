/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_H_CONFIG_INCLUDED_
#define _RQC_H_CONFIG_INCLUDED_

#include <rquic/rquic_typedef.h>

#define rqc_min(a, b) ((a) < (b) ? (a) : (b))
#define rqc_max(a, b) ((a) > (b) ? (a) : (b))
#define rqc_sub_abs(a, b) ((a) > (b) ? ((a) - (b)): ((b) - (a)))
#define rqc_clamp(a, min, max) rqc_max(rqc_min(a, max), min)

#ifdef RQC_SYS_WINDOWS
static const unsigned char LF = '\n';
static const unsigned char CR = '\r';
#else
#define LF     (unsigned char) '\n'
#define CR     (unsigned char) '\r'
#endif

#define CRLF   "\r\n"

#define RQC_PTR_SIZE 8

#define RQC_DECIMAL 10

#define RQC_INT32_LEN   (sizeof("-2147483648") - 1)
#define RQC_INT64_LEN   (sizeof("-9223372036854775808") - 1)

#if (RQC_PTR_SIZE == 4)
# define RQC_INT_T_LEN RQC_INT32_LEN
# define RQC_MAX_INT_T_VALUE  2147483647

#else
# define RQC_INT_T_LEN RQC_INT64_LEN
# define RQC_MAX_INT_T_VALUE  9223372036854775807
#endif

#define RQC_MAX_UINT32_VALUE  (uint32_t) 0xffffffff
#define RQC_MAX_INT32_VALUE   (uint32_t) 0x7fffffff

#define RQC_MAX_UINT64_VALUE  (uint64_t) 0xffffffffffffffff
#define RQC_MAX_INT64_VALUE   (uint64_t) 0x7fffffffffffffff

#define RQC_MICROS_PER_SECOND 1000000   /* 1s=1000000us */

#endif /*_RQC_H_CONFIG_INCLUDED_*/
