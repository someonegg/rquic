/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef RQC_DEFS_H
#define RQC_DEFS_H

#include <stdint.h>
#include <rquic/rquic.h>

/* default connection timeout(millisecond) */
#define RQC_CONN_DEFAULT_IDLE_TIMEOUT   75000
/* default connection initial timeout(millisecond) */
#define RQC_CONN_INITIAL_IDLE_TIMEOUT   5000

#define RQC_CONN_ADDR_VALIDATION_CID_ENTROPY 8

/* connection PTO packet count */
#define RQC_CONN_PTO_PKT_CNT_MAX        2

/* connection max UDP payload size */
#define RQC_CONN_MAX_UDP_PAYLOAD_SIZE   1500

/* connection active cid limit */
#define RQC_CONN_ACTIVE_CID_LIMIT       2

/* version definitions */
#define RQC_VERSION_V1_VALUE            0x00000001
#define RQC_IDRAFT_VER_29_VALUE         0xFF00001D

#define RQC_PROTO_VERSION_LEN           4

/* the value of max_streams transport parameter or MAX_STREAMS frame must <= 2^60 */
#define RQC_MAX_STREAMS                 ((uint64_t)1 << 60)

extern const uint32_t       rqc_proto_version_value[];
extern const unsigned char  rqc_proto_version_field[][RQC_PROTO_VERSION_LEN];

#define rqc_check_proto_version_valid(ver) \
        ((ver) > RQC_IDRAFT_INIT_VER && (ver) < RQC_IDRAFT_VER_NEGOTIATION)

/* max alpn length */
#define RQC_MAX_ALPN_LEN               (RQC_MAX_ALPN_BUF_LEN - 1)

#endif
