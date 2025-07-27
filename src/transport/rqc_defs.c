/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "src/transport/rqc_defs.h"
#include "src/common/rqc_str.h"
#include <string.h>

const uint32_t rqc_proto_version_value[RQC_VERSION_MAX] = {
    0xFFFFFFFF,
    0x00000001,
    0xFF00001D,
    0x00000000,
};

const unsigned char rqc_proto_version_field[RQC_VERSION_MAX][RQC_PROTO_VERSION_LEN] = {
    [RQC_IDRAFT_INIT_VER]        = { 0xFF, 0xFF, 0xFF, 0xFF, },  /* placeholder */
    [RQC_VERSION_V1]             = { 0x00, 0x00, 0x00, 0x01, },
    [RQC_IDRAFT_VER_29]          = { 0xFF, 0x00, 0x00, 0x1D, },
    [RQC_IDRAFT_VER_NEGOTIATION] = { 0x00, 0x00, 0x00, 0x00, },
};
