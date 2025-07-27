/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_PACING_H_INCLUDED_
#define _RQC_PACING_H_INCLUDED_

#include <rquic/rquic_typedef.h>

typedef struct rqc_pacing_s {
    int             pacing_on;
    uint32_t        bytes_budget;
    rqc_usec_t      last_sent_time;
    rqc_send_ctl_t *ctl_ctx;
    uint32_t        pending_budget;
} rqc_pacing_t;

int rqc_pacing_is_on(rqc_pacing_t *pacing);

void rqc_pacing_init(rqc_pacing_t *pacing, int pacing_on, rqc_send_ctl_t *send_ctl);

void rqc_pacing_on_timeout(rqc_pacing_t *pacing);

void rqc_pacing_on_packet_sent(rqc_pacing_t *pacing, uint32_t bytes);

void rqc_pacing_on_app_limit(rqc_pacing_t *pacing);

int rqc_pacing_can_write(rqc_pacing_t *pacing, uint32_t total_bytes);

uint64_t rqc_pacing_rate_calc(rqc_pacing_t *pacing);

#endif /* _RQC_PACING_H_INCLUDED_ */
