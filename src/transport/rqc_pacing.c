/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "src/transport/rqc_pacing.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/transport/rqc_packet.h"
#include "src/common/rqc_log.h"

#define RQC_MIN_BURST_NUM (2 * RQC_MSS)
#define RQC_MAX_BURST_NUM (10 * RQC_MSS)
#define TRUE 1
#define FALSE 0
#define RQC_CLOCK_GRANULARITY_US 1000 /* 1ms */
#define RQC_PACING_DELAY_US RQC_CLOCK_GRANULARITY_US
#define RQC_DEFAULT_PACING_RATE(init_rtt) (((2 * RQC_MSS * 1000000ULL)/(init_rtt)))

void
rqc_pacing_init(rqc_pacing_t *pacing, int pacing_on, rqc_send_ctl_t *send_ctl)
{
    pacing->bytes_budget = RQC_MAX_BURST_NUM;
    pacing->last_sent_time = 0;
    pacing->ctl_ctx = send_ctl;
    pacing->pacing_on = pacing_on;
    pacing->pending_budget = 0;
    if (send_ctl->ctl_cong_callback->rqc_cong_ctl_on_ack_multiple_pkts) {
        pacing->pacing_on = 1;
    }
}

uint64_t
rqc_pacing_rate_calc(rqc_pacing_t *pacing)
{
    /* see linux kernel tcp_update_pacing_rate(struct sock *sk) */
    uint64_t pacing_rate;
    uint64_t cwnd;
    rqc_send_ctl_t *send_ctl = pacing->ctl_ctx;
    if (send_ctl->ctl_cong_callback->rqc_cong_ctl_get_pacing_rate) {
        pacing_rate = send_ctl->ctl_cong_callback->
                      rqc_cong_ctl_get_pacing_rate(send_ctl->ctl_cong);
        return pacing_rate;
    }

    cwnd = send_ctl->ctl_cong_callback->rqc_cong_ctl_get_cwnd(send_ctl->ctl_cong);

    rqc_usec_t srtt = send_ctl->ctl_srtt;
    if (srtt == 0) {
        srtt = send_ctl->ctl_conn->conn_settings.initial_rtt;
    }

    /* bytes can be sent per second */
    pacing_rate = cwnd * 1000000 / srtt;
    if (pacing_rate == 0) {
        pacing_rate = RQC_DEFAULT_PACING_RATE(srtt);
        rqc_log(pacing->ctl_ctx->ctl_conn->log, RQC_LOG_ERROR,
                "|pacing_rate zero|cwnd:%ui|srtt:%ui|", cwnd, srtt);
    }

    if (send_ctl->ctl_cong_callback->rqc_cong_ctl_in_slow_start
        && send_ctl->ctl_cong_callback->rqc_cong_ctl_in_slow_start(send_ctl->ctl_cong))
    {
        pacing_rate *= 2;

    } else {
        pacing_rate = pacing_rate * 12 / 10;
    }

    return pacing_rate;
}

static uint32_t
rqc_pacing_max_burst_size(rqc_pacing_t *pacing)
{
    rqc_usec_t t_diff = (RQC_PACING_DELAY_US + RQC_CLOCK_GRANULARITY_US);
    uint64_t max_burst_bytes = t_diff * rqc_pacing_rate_calc(pacing)
                                / 1000000;
    return rqc_max(RQC_MAX_BURST_NUM, max_burst_bytes);
}

static uint32_t
rqc_pacing_calc_budget(rqc_pacing_t *pacing, rqc_usec_t now)
{
    uint32_t budget = pacing->bytes_budget;
    uint32_t max_burst_bytes = rqc_pacing_max_burst_size(pacing);
    if (pacing->last_sent_time == 0) {
        budget = max_burst_bytes;

    } else {
        budget += (now - pacing->last_sent_time) * rqc_pacing_rate_calc(pacing)
                    / 1000000;
    }
    return rqc_min(budget, max_burst_bytes);
}

void
rqc_pacing_on_timeout(rqc_pacing_t *pacing)
{
    rqc_usec_t now = rqc_monotonic_timestamp();
    uint32_t budget = rqc_pacing_calc_budget(pacing, now);
    pacing->bytes_budget = rqc_max(budget, pacing->bytes_budget + pacing->pending_budget);
    pacing->pending_budget = 0;
    pacing->last_sent_time = now;
}

void
rqc_pacing_on_packet_sent(rqc_pacing_t *pacing, uint32_t bytes)
{
    rqc_usec_t now = rqc_monotonic_timestamp();
    uint32_t budget = rqc_pacing_calc_budget(pacing, now);
    if (bytes > budget) {
        budget = 0;

    } else {
        budget -= bytes;
    }
    pacing->bytes_budget = budget;
    pacing->last_sent_time = now;
}

rqc_usec_t
rqc_pacing_time_until_send(rqc_pacing_t *pacing, uint32_t bytes)
{
    if (pacing->bytes_budget >= bytes) {
        return 0;
    }
    rqc_usec_t delay_us;
    delay_us = (uint64_t)(bytes - pacing->bytes_budget) * 1000000
            / rqc_pacing_rate_calc(pacing);
    delay_us = rqc_max(delay_us, RQC_PACING_DELAY_US);
    pacing->pending_budget = bytes - pacing->bytes_budget;
    return delay_us;
}

int
rqc_pacing_can_write(rqc_pacing_t *pacing, uint32_t total_bytes)
{
    rqc_send_ctl_t *send_ctl = pacing->ctl_ctx;
    if (rqc_timer_is_set(&send_ctl->path_timer_manager, RQC_TIMER_PACING)) {
        return FALSE;
    }

    uint64_t delay = rqc_pacing_time_until_send(pacing, total_bytes);

    if (delay != 0) {
        rqc_timer_update(&send_ctl->path_timer_manager, RQC_TIMER_PACING, rqc_monotonic_timestamp(), delay);
        return FALSE;
    }

    return TRUE;
}

void
rqc_pacing_on_app_limit(rqc_pacing_t *pacing) {
    pacing->bytes_budget = RQC_MAX_BURST_NUM;
    pacing->last_sent_time = rqc_monotonic_timestamp();
}

int
rqc_pacing_is_on(rqc_pacing_t *pacing) {
    return pacing->pacing_on;
}
