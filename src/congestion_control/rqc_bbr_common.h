/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_BBR_COMMON_H_INCLUDED_
#define _RQC_BBR_COMMON_H_INCLUDED_

#include <rquic/rquic_typedef.h>

typedef struct rqc_bbr_info_interface_s {
    uint8_t     (*mode)(void *cong);
    uint64_t    (*min_rtt)(void *cong);
    uint8_t     (*idle_restart)(void *cong);
    uint8_t     (*full_bw_reached)(void *cong);
    uint8_t     (*recovery_mode)(void *cong);
    uint64_t    (*recovery_start_time)(void *cong);
    uint8_t     (*packet_conservation)(void *cong);
    uint8_t     (*round_start)(void *cong);
    float       (*pacing_gain)(void *cong);
    float       (*cwnd_gain)(void *cong);
} rqc_bbr_info_interface_t;

#endif
