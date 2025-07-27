
/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_RANDOM_H_INCLUDED_
#define _RQC_RANDOM_H_INCLUDED_

#include <sys/types.h>
#include <rquic/rquic_typedef.h>
#include "src/common/rqc_str.h"
#include "src/common/rqc_log.h"
#include "src/common/rqc_common.h"
#ifdef RQC_SYS_WINDOWS
#include <wincrypt.h>
#undef PKCS7_SIGNER_INFO
#undef X509_CERT_PAIR
#undef X509_EXTENSIONS
#undef X509_NAME
#endif

typedef struct rqc_random_generator_s {
    /* for random */
    rqc_int_t               rand_fd;           /* init_value: -1 */
    off_t                   rand_buf_offset;   /* used offset */
    size_t                  rand_buf_size;     /* total buffer size */
    rqc_str_t               rand_buf;          /* buffer for random bytes*/

    rqc_log_t              *log;
#ifdef RQC_SYS_WINDOWS
    HCRYPTPROV              hProvider;
#endif
} rqc_random_generator_t;

rqc_int_t rqc_get_random(rqc_random_generator_t *rand_gen, u_char *buf, size_t need_len);
rqc_random_generator_t *rqc_random_generator_create(rqc_log_t *log);
void rqc_random_generator_destroy(rqc_random_generator_t *rand_gen);
long rqc_random(void);

#endif /* _RQC_RANDOM_H_INCLUDED_ */
