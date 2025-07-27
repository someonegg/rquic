/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef RQC_HQ_DEFS_H
#define RQC_HQ_DEFS_H

#include <rquic/rquic.h>
#include "src/common/rqc_malloc.h"

#include <inttypes.h>

#define RQC_ALPN_HQ_INTEROP         "hq-interop"
#define RQC_ALPN_HQ_INTEROP_LEN     10
#define RQC_ALPN_HQ_29              "hq-29"
#define RQC_ALPN_HQ_29_LEN          5

static const char *const rqc_hq_alpn[] = {
    [RQC_IDRAFT_INIT_VER]        = "",                      /* placeholder */
    [RQC_VERSION_V1]             = RQC_ALPN_HQ_INTEROP,     /* QUIC v1 */
    [RQC_IDRAFT_VER_29]          = RQC_ALPN_HQ_29,          /* draft-29 */
    [RQC_IDRAFT_VER_NEGOTIATION] = "",
};

#define PRINT_LOG(format, ...) printf("%s|%d|"format"\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)

#endif
