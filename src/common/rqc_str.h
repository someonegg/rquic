/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_STR_H_INCLUDED_
#define _RQC_STR_H_INCLUDED_

#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <stddef.h>
#include <sys/types.h>

#include <include/rquic/rquic_typedef.h>
#include "src/common/rqc_config.h"

#ifndef  RQC_SYS_WINDOWS
#include <sys/resource.h>
#endif

typedef struct rqc_str_s {
    size_t          len;
    unsigned char  *data;
} rqc_str_t;

#define rqc_string(str)         { sizeof(str) - 1, (unsigned char *) str }
#define rqc_null_string         { 0, NULL }
#define rqc_str_set(str, text)  (str)->len = sizeof(text) - 1; (str)->data = (unsigned char *) text
#define rqc_str_null(str)       (str)->len = 0; (str)->data = NULL

#define rqc_str_equal(s1, s2)   ((s1).len == (s2).len && memcmp((s1).data, (s2).data, (s1).len) == 0)

#define rqc_tolower(c)          (unsigned char) ((c >= 'A' && c <= 'Z') ? (c | 0x20) : c)
#define rqc_toupper(c)          (unsigned char) ((c >= 'a' && c <= 'z') ? (c & ~0x20) : c)

#define rqc_memzero(buf, n)     (void) memset(buf, 0, n)
#define rqc_memset(buf, c, n)   (void) memset(buf, c, n)

#define rqc_memcpy(dst, src, n) (void) memcpy(dst, src, n)
#define rqc_cpymem(dst, src, n) (((unsigned char *) memcpy(dst, src, n)) + (n))

#define rqc_memcmp(s1, s2, n)   memcmp((const char *) s1, (const char *) s2, n)

#define rqc_lengthof(x)         (sizeof(x) - 1)

unsigned char *rqc_hex_dump(unsigned char *dst, const unsigned char *src, size_t len);
unsigned char *rqc_vsprintf(unsigned char *buf, unsigned char *last, const char *fmt, va_list args);
unsigned char *rqc_sprintf_num(unsigned char *buf, unsigned char *last,
    uint64_t ui64, unsigned char zero, uintptr_t hexadecimal, uintptr_t width);

static inline int
rqc_memcpy_with_cap(void *dst, size_t cap, const void *src, size_t n)
{
    if (n == 0) {
        return RQC_OK;
    }

    if (n <= cap) {
        rqc_memcpy(dst, src, n);
        return RQC_OK;
    }

    return -RQC_ENOBUF;
}

static inline void
rqc_str_tolower(unsigned char *dst, unsigned char *src, size_t n)
{
    while (n) {
        *dst = rqc_tolower(*src);
        dst++;
        src++;
        n--;
    }
}

static inline unsigned char *
rqc_sprintf(unsigned char *buf, unsigned char *last, const char *fmt, ...)
{
    unsigned char *p;
    va_list args;
    va_start(args, fmt);
    p = rqc_vsprintf(buf, last, fmt, args);
    va_end(args);
    return p;
}

inline static rqc_bool_t
rqc_memeq(const void *s1, const void *s2, size_t n)
{
    return n == 0 || memcmp(s1, s2, n) == 0;
}

inline static rqc_bool_t
rqc_char_is_letter_or_number(char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9'))
    {
        return RQC_TRUE;
    }
    return RQC_FALSE;
}

#endif /*_RQC_STR_H_INCLUDED_*/
