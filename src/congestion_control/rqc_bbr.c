/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <rquic/rquic_typedef.h>
#if !defined(RQC_SYS_WINDOWS) || defined(RQC_ON_MINGW)
#include <unistd.h>
#endif
#include "src/congestion_control/rqc_bbr.h"
#include "src/congestion_control/rqc_sample.h"
#include "src/common/rqc_time.h"
#include "src/common/rqc_random.h"
#include "src/common/rqc_config.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/transport/rqc_packet.h"

#define RQC_BBR_MAX_DATAGRAMSIZE    RQC_MSS
#define RQC_BBR_MIN_WINDOW          (4 * RQC_BBR_MAX_DATAGRAMSIZE)
#define RQC_BBR_MAX_WINDOW          (100 * RQC_BBR_MAX_DATAGRAMSIZE)
/* The RECOMMENDED value is the minimum of 10 * kMaxDatagramSize and max(2* kMaxDatagramSize, 14720)) */
/* same init window as cubic */
/* 32 is too aggressive. we have observed heavy bufferbloat events from online deployment */
/* 1440 * 10 / 1200 = 12 */
#define RQC_BBR_INITIAL_WINDOW  (32 * RQC_BBR_MAX_DATAGRAMSIZE)
/* Pacing gain cycle rounds */
#define RQC_BBR_CYCLE_LENGTH    8
#define RQC_BBR_INF             0x7fffffff
#define RQC_BBR_MAX_AI_SCALE    (~0U)

/* Size of window of bandwidth filter, in rtts */
const uint32_t rqc_bbr_bw_win_size = RQC_BBR_CYCLE_LENGTH + 2;
/* Window of min rtt filter, in sec */
const uint32_t rqc_bbr_minrtt_win_size = 10;
/* Minimum time spent in BBR_PROBE_RTT, in us*/
const uint32_t rqc_bbr_probertt_time_us = 100000;
/* Initial rtt before any samples are received, in ms  */
const uint64_t rqc_bbr_initial_rtt_ms = 100;
/* The gain of pacing rate for START_UP, 2/(ln2) */
const float rqc_bbr_high_gain = 2.885;
/* Gain in BBR_DRAIN */
const float rqc_bbr_drain_gain = 1.0 / 2.885;
/* Gain for cwnd in probe_bw, like slow start*/
const float rqc_bbr_cwnd_gain = 2.5;
/* Cycle of gains in PROBE_BW for pacing rate */
const float rqc_bbr_pacing_gain[] = {1.25, 0.75, 1, 1, 1, 1, 1, 1};
const float rqc_bbr_low_pacing_gain[] = {1.1, 0.9, 1, 1, 1, 1, 1, 1};
/* Minimum packets that need to ensure ack if there is delayed ack */
const uint32_t rqc_bbr_min_cwnd = 4 * RQC_BBR_MAX_DATAGRAMSIZE;
/* If bandwidth has increased by 1.25, there may be more bandwidth available */
const float rqc_bbr_fullbw_thresh = 1.1;
/* After 3 rounds bandwidth less than (1.25x), estimate the pipe is full */
const uint32_t rqc_bbr_fullbw_cnt = 3;
const float rqc_bbr_probe_rtt_gain = 0.75;
const uint32_t rqc_bbr_extra_ack_gain = 2;
const float rqc_bbr_max_extra_ack_time = 0.2;
const uint32_t rqc_bbr_ack_epoch_acked_reset_thresh = (1 << 20) * RQC_BBR_MAX_DATAGRAMSIZE;
const float rqc_bbr_pacing_rate_margin_percent = 0;

/* BBRv2 parameters */
const float rqc_bbr2_drain_gain = 0.75;
const float rqc_bbr2_startup_cwnd_gain = 2.885;
/* keep minrtt valid for 10s if it has not been changed */
const uint32_t rqc_bbr2_minrtt_win_size_us = 10000000;
/* probe new minrtt in 2.5s*/
const uint32_t rqc_bbr2_probertt_win_size_us = 10000000;
const bool rqc_bbr2_extra_ack_in_startup = 1;
/* 10 packet-timed rtt */
const uint32_t rqc_bbr2_extra_ack_win_rtt = 5;
/* 2 packet-timed rtt */
const uint32_t rqc_bbr2_extra_ack_win_rtt_in_startup = 1;
/* slow down */
const float rqc_bbr2_startup_pacing_gain_on_lost = 1.5;
const bool rqc_bbr2_slow_down_startup_on_lost = 0;

const uint32_t rqc_bbr_lt_bw_interval_min_rtts = 4;
const float rqc_bbr_lt_bw_loss_thresh = 0.2; // 20%
const float rqc_bbr_lt_bw_ratio = 0.125; // 1/8
const uint32_t rqc_bbr_lt_bw_diff = 4000; // 4000 B/s
const uint32_t rqc_bbr_lt_bw_interval_max_rtts = 16;
const uint32_t rqc_bbr_lt_bw_max_rtts = 48; // use lt_bw for 48xRTT at maximum

/* 5RTT */
#if RQC_BBR_RTTVAR_COMPENSATION_ENABLED
static const float rqc_bbr_windowed_max_rtt_win_size = 5;
static const float rqc_bbr_rtt_compensation_startup_thresh = 2;
static const float rqc_bbr_rtt_compensation_thresh = 1;
static const float rqc_bbr_rtt_compensation_cwnd_factor = 1;
#endif

static void rqc_bbr_enter_probe_bw(rqc_bbr_t *bbr, rqc_sample_t *sampler);

size_t
rqc_bbr_size()
{
    return sizeof(rqc_bbr_t);
}

static void
rqc_bbr_enter_startup(rqc_bbr_t *bbr)
{
    bbr->mode = BBR_STARTUP;
    bbr->pacing_gain = rqc_bbr_high_gain;
    bbr->cwnd_gain = rqc_bbr2_startup_cwnd_gain;
#if RQC_BBR_RTTVAR_COMPENSATION_ENABLED
    bbr->rtt_compensation_thresh = rqc_bbr_rtt_compensation_startup_thresh;
#endif
}

static void
rqc_bbr_init_pacing_rate(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    uint64_t bandwidth;
    if (sampler->srtt) {
        bbr->has_srtt = 1;
    }
    bandwidth = bbr->congestion_window * (uint64_t)MSEC2SEC
        / (sampler->srtt ? sampler->srtt : 1000);
    bbr->pacing_rate = bbr->pacing_gain * bandwidth;
}

static void
rqc_bbr_reset_lt_bw_sampling(rqc_bbr_t *bbr)
{
    bbr->lt_is_sampling = 0;
    bbr->lt_bw = 0;
    bbr->lt_use_bw = 0;
    bbr->lt_last_lost_pkt = 0;
    bbr->lt_last_delivered_bytes = 0;
    bbr->lt_last_stamp = 0;
    bbr->lt_rtt_cnt = 0;
}

static void
rqc_bbr_new_lt_bw_interval(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    bbr->lt_rtt_cnt = 0;
    bbr->lt_last_delivered_bytes = sampler->total_acked;
    bbr->lt_last_lost_pkt = sampler->total_lost_pkts;
    bbr->lt_last_stamp = sampler->now;
}

static void
rqc_bbr_finish_lt_bw_interval(rqc_bbr_t *bbr,
    rqc_sample_t *sampler, uint64_t bw)
{
    uint64_t diff = 0;
    if (bbr->lt_bw) {
        diff = bbr->lt_bw > bw ? bbr->lt_bw - bw : bw - bbr->lt_bw;
        /* the observed policing rate is consistent */
        if (diff <= (uint64_t)(bbr->lt_bw * rqc_bbr_lt_bw_ratio)
            || diff <= (rqc_bbr_lt_bw_diff))
        {
            bbr->lt_bw = (bw + bbr->lt_bw) >> 1;
            bbr->lt_use_bw = 1;
            bbr->lt_rtt_cnt = 0;
            bbr->pacing_gain = 1.0;
            return;
        }
    }
    bbr->lt_bw = bw;
    rqc_bbr_new_lt_bw_interval(bbr, sampler);
}

static void
rqc_bbr_update_lt_bw_sampling(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    if (bbr->lt_use_bw) {
        if (bbr->mode == BBR_PROBE_BW && bbr->round_start
            && ++bbr->lt_rtt_cnt >= rqc_bbr_lt_bw_max_rtts)
        {
            rqc_bbr_reset_lt_bw_sampling(bbr);
            rqc_bbr_enter_probe_bw(bbr, sampler);
            return;
        }
    }

    if (!bbr->lt_is_sampling) {
        if (sampler->loss == 0) {
            return;
        }
        bbr->lt_is_sampling = RQC_TRUE;
        rqc_bbr_new_lt_bw_interval(bbr, sampler);
    }

    if (sampler->is_app_limited) {
        rqc_bbr_reset_lt_bw_sampling(bbr);
        return;
    }

    if (bbr->round_start) {
        bbr->lt_rtt_cnt++;
    }

    if (bbr->lt_rtt_cnt < rqc_bbr_lt_bw_interval_min_rtts) {
        return; // wait a bit for sampling
    }

    if (bbr->lt_rtt_cnt > rqc_bbr_lt_bw_interval_max_rtts) {
        rqc_bbr_reset_lt_bw_sampling(bbr);
        return; // stop sampling as the interval is too long
    }

    if (sampler->loss == 0) {
        return; // wait for losses
    }

    uint64_t lost_bytes = (sampler->total_lost_pkts - bbr->lt_last_lost_pkt) * RQC_BBR_MAX_DATAGRAMSIZE;
    uint64_t acked_bytes = sampler->total_acked - bbr->lt_last_delivered_bytes;

    if (acked_bytes == 0
        || lost_bytes < (uint64_t)(acked_bytes * rqc_bbr_lt_bw_loss_thresh))
    {
        return; // wait for more losses
    }

    rqc_usec_t interval = sampler->now - bbr->lt_last_stamp;

    if (interval < 1000) {
        return; // interval is too small. let's wait.
    }

    uint64_t policing_rate = acked_bytes * 1000000 / interval;

    rqc_bbr_finish_lt_bw_interval(bbr, sampler, policing_rate);
}

static void
rqc_bbr_init(void *cong_ctl, rqc_sample_t *sampler, rqc_cc_params_t cc_params)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)(cong_ctl);
    uint64_t now = rqc_monotonic_timestamp();

    memset(bbr, 0, sizeof(*bbr));
    rqc_win_filter_reset(&bbr->bandwidth, 0, 0);
#if RQC_BBR_RTTVAR_COMPENSATION_ENABLED
    rqc_win_filter_reset(&bbr->max_rtt, 0, 0);
    bbr->max_rtt_win_len = rqc_bbr_windowed_max_rtt_win_size;
    if (cc_params.cc_optimization_flags & RQC_BBR_FLAG_RTTVAR_COMPENSATION) {
        bbr->rttvar_compensation_on = 1;
    }
#endif
#ifndef RQC_BBR_DISABLE_CWND_AI
    bbr->beyond_target_cwnd = 0;
    bbr->snd_cwnd_cnt_bytes = 0;
    bbr->ai_scale = 1;
    bbr->ai_scale_accumulated_bytes = 0;
#endif
    bbr->min_rtt = sampler->srtt ? sampler->srtt : RQC_BBR_INF;
    bbr->min_rtt_stamp = now;
    bbr->probe_rtt_min_us = sampler->srtt ? sampler->srtt : RQC_BBR_INF;
    bbr->probe_rtt_min_us_stamp = now;
    bbr->round_start = 0;
    bbr->round_cnt = 0;
    bbr->next_round_delivered = 0;
    bbr->probe_rtt_round_done = FALSE;
    bbr->probe_rtt_round_done_stamp = 0;
    bbr->packet_conservation = FALSE;
    bbr->prior_cwnd = 0;
    bbr->initial_congestion_window = RQC_BBR_INITIAL_WINDOW;
    bbr->min_cwnd = rqc_bbr_min_cwnd;
    bbr->has_srtt = 0;
    bbr->idle_restart = 0;
    bbr->packet_conservation = 0;
    bbr->recovery_mode = BBR_NOT_IN_RECOVERY;
    bbr->just_enter_recovery_mode = FALSE;
    bbr->just_exit_recovery_mode = FALSE;
    bbr->recovery_start_time = 0;
    bbr->extra_ack_stamp = now;
    bbr->epoch_ack = 0;
    bbr->extra_ack_round_rtt = 0;
    bbr->extra_ack_idx = 0;
    bbr->extra_ack[0] = 0;
    bbr->extra_ack[1] = 0;
    bbr->extra_ack_in_startup = rqc_bbr2_extra_ack_in_startup;
    bbr->extra_ack_win_len = rqc_bbr2_extra_ack_win_rtt;
    bbr->extra_ack_win_len_in_startup = rqc_bbr2_extra_ack_win_rtt_in_startup;
    bbr->full_bandwidth_cnt = 0;
    bbr->full_bandwidth_reached = FALSE;
    bbr->lt_bw_enabled = RQC_FALSE;
    bbr->ignore_app_limit = RQC_FALSE;

    if (cc_params.customize_on) {
        cc_params.init_cwnd *= RQC_BBR_MAX_DATAGRAMSIZE;
        cc_params.min_cwnd *= RQC_BBR_MAX_DATAGRAMSIZE;
        bbr->initial_congestion_window =
            cc_params.init_cwnd >= RQC_BBR_MIN_WINDOW
            && cc_params.init_cwnd <= RQC_BBR_MAX_WINDOW
            ? cc_params.init_cwnd : RQC_BBR_INITIAL_WINDOW;
        bbr->min_cwnd = cc_params.min_cwnd >= RQC_BBR_MIN_WINDOW
            && cc_params.min_cwnd <= RQC_BBR_MAX_WINDOW
            ? cc_params.min_cwnd : rqc_bbr_min_cwnd;

        if (cc_params.expect_bw > 0) {
            bbr->enable_expect_bw = TRUE;
            bbr->expect_bw = cc_params.expect_bw;
        }
        if (cc_params.max_expect_bw > 0) {
            bbr->enable_max_expect_bw = TRUE;
            bbr->max_expect_bw = cc_params.max_expect_bw;
        }
        if (cc_params.bbr_enable_lt_bw) {
            bbr->lt_bw_enabled = RQC_TRUE;
        }
        if (cc_params.bbr_ignore_app_limit) {
            bbr->ignore_app_limit = RQC_TRUE;
        }
    }

    bbr->congestion_window = bbr->initial_congestion_window;
    rqc_bbr_reset_lt_bw_sampling(bbr);
    rqc_bbr_enter_startup(bbr);
    rqc_bbr_init_pacing_rate(bbr, sampler);
}

static uint32_t
rqc_bbr_max_bw(rqc_bbr_t *bbr)
{
    return rqc_win_filter_get(&bbr->bandwidth);
}

static uint32_t
rqc_bbr_bw(rqc_bbr_t *bbr)
{
    return bbr->lt_use_bw && bbr->lt_bw_enabled ? bbr->lt_bw : rqc_bbr_max_bw(bbr);
}

static void
rqc_bbr_update_bandwidth(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    bbr->round_start = FALSE;
    /* Check whether the data is legal */
    if (/*sampler->delivered < 0 ||*/ sampler->interval <= 0) {
        return;
    }

    /*
     * check whether the next BBR cycle is reached
     * at the beginning of the cycle, the number of packets sent is less than or equal to
     * the maximum number of packets that have been sent when the current ack packet is sent.
     */
    if (bbr->next_round_delivered <= sampler->prior_delivered) {
        bbr->next_round_delivered = sampler->total_acked;
        bbr->round_cnt++;
        bbr->round_start = TRUE;
        bbr->packet_conservation = 0;
    }

    if (bbr->lt_bw_enabled
        && bbr->mode != BBR_STARTUP
        && bbr->mode != BBR_DRAIN)
    {
        rqc_bbr_update_lt_bw_sampling(bbr, sampler);
    }

    uint32_t bandwidth;
    /* Calculate the new bandwidth, bytes per second */
    bandwidth = 1.0 * sampler->delivered / sampler->interval * MSEC2SEC;

    if (bbr->enable_max_expect_bw && bandwidth >= bbr->max_expect_bw) {
        bandwidth = bbr->max_expect_bw;
    }

    /*
     * In a live video scenario, the applimit state often occurs,
     * causing the detection bandwidth increases but does not decrease.
     */
    if (bbr->ignore_app_limit || !sampler->is_app_limited || bandwidth >= rqc_bbr_max_bw(bbr)) {
        rqc_win_filter_max(&bbr->bandwidth, rqc_bbr_bw_win_size,
                           bbr->round_cnt, bandwidth);
    }
}

static uint32_t
rqc_bbr_bdp(rqc_bbr_t *bbr, uint64_t bw)
{
    return bbr->min_rtt * bw / MSEC2SEC;
}

#if RQC_BBR_RTTVAR_COMPENSATION_ENABLED
static uint32_t
rqc_bbr_compensate_cwnd_for_rttvar(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    rqc_usec_t srtt = sampler->srtt;
    rqc_usec_t recent_max_rtt = rqc_win_filter_get(&bbr->max_rtt);
    rqc_usec_t compensation_thresh = (1 + bbr->rtt_compensation_thresh) *
                                     bbr->min_rtt;
    uint32_t cwnd_addition = 0;
    if (recent_max_rtt >= compensation_thresh) {
        if (srtt > bbr->min_rtt) {
            rqc_usec_t rtt_var = (srtt - bbr->min_rtt);
            cwnd_addition = (rqc_bbr_max_bw(bbr) * rtt_var / MSEC2SEC) *
                                       rqc_bbr_rtt_compensation_cwnd_factor;

        } else {
            rqc_log(sampler->send_ctl->ctl_conn->log, RQC_LOG_WARN,
                    "|rttvar compensation|weird things happened|"
                    "|srtt %ui <= min_rtt %ui|",
                    srtt, bbr->min_rtt);
        }
    }
    return cwnd_addition;
}
#endif

static uint32_t
rqc_bbr_target_cwnd(rqc_bbr_t *bbr, float gain, uint64_t bw)
{
    if (bbr->min_rtt == RQC_BBR_INF) {
        return bbr->initial_congestion_window;
    }
    uint32_t cwnd = gain * rqc_bbr_bdp(bbr, bw);
    return rqc_max(cwnd, bbr->min_cwnd);
}

static bool
rqc_bbr_is_next_cycle_phase(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    bool is_full_length = (sampler->now - bbr->last_cycle_start) > bbr->min_rtt;
    uint32_t inflight = sampler->prior_inflight;
    bool should_advance_gain_cycling = is_full_length;
    if (bbr->pacing_gain > 1.0) {
        should_advance_gain_cycling = is_full_length
            && (sampler->loss
                || inflight >= rqc_bbr_target_cwnd(bbr,
                                                   bbr->pacing_gain,
                                                   rqc_bbr_max_bw(bbr)));
    }
    /* Drain to target: 1xBDP */
    if (bbr->pacing_gain < 1.0) {
        should_advance_gain_cycling = is_full_length
            || (inflight <= rqc_bbr_target_cwnd(bbr, 1.0, rqc_bbr_max_bw(bbr)));
    }
    return should_advance_gain_cycling;
}

static float
rqc_bbr_get_pacing_gain(rqc_bbr_t *bbr, uint32_t cycle_idx)
{
    if (bbr->lt_use_bw && bbr->lt_bw_enabled) {
        return 1.0;
    }

    if (bbr->enable_expect_bw && rqc_bbr_max_bw(bbr) >= bbr->expect_bw) {
        return rqc_bbr_low_pacing_gain[cycle_idx];
    }

    return rqc_bbr_pacing_gain[cycle_idx];
}

static void
rqc_bbr_update_cycle_phase(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    if (bbr->mode == BBR_PROBE_BW
        && rqc_bbr_is_next_cycle_phase(bbr, sampler))
    {
        bbr->cycle_idx = (bbr->cycle_idx + 1) % RQC_BBR_CYCLE_LENGTH;
        bbr->last_cycle_start = sampler->now;
        bbr->pacing_gain = rqc_bbr_get_pacing_gain(bbr, bbr->cycle_idx);
    }
}

static uint32_t
rqc_bbr_extra_ack(rqc_bbr_t *bbr)
{
    return rqc_max(bbr->extra_ack[0], bbr->extra_ack[1]);
}

static uint32_t
rqc_bbr_ack_aggregation_cwnd(rqc_bbr_t *bbr)
{
    uint32_t max_aggr_cwnd, aggr_cwnd = 0;
    if (rqc_bbr_extra_ack_gain
        && (bbr->full_bandwidth_reached || bbr->extra_ack_in_startup))
    {
        max_aggr_cwnd = rqc_bbr_bw(bbr) * rqc_bbr_max_extra_ack_time;
        aggr_cwnd = rqc_bbr_extra_ack_gain * rqc_bbr_extra_ack(bbr);
        aggr_cwnd = rqc_min(aggr_cwnd, max_aggr_cwnd);
    }
    return aggr_cwnd;
}

static void
rqc_update_ack_aggregation(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    uint32_t epoch, expected_ack, extra_ack;
    uint32_t extra_ack_win_thresh = bbr->extra_ack_win_len;
    if (!rqc_bbr_extra_ack_gain || sampler->delivered <= 0
        || sampler->interval <= 0 || sampler->acked <= 0)
    {
        return;
    }

    if (bbr->round_start) {
        bbr->extra_ack_round_rtt += 1;
        if (bbr->extra_ack_in_startup && !bbr->full_bandwidth_reached) {
            extra_ack_win_thresh = bbr->extra_ack_win_len_in_startup;
        }
        if (bbr->extra_ack_round_rtt >= extra_ack_win_thresh) {
            bbr->extra_ack_round_rtt = 0;
            bbr->extra_ack_idx = bbr->extra_ack_idx ? 0 : 1;
            bbr->extra_ack[bbr->extra_ack_idx] = 0;
        }
    }

    epoch = sampler->now - bbr->extra_ack_stamp;
    expected_ack = ((uint64_t)rqc_bbr_bw(bbr) * epoch) / MSEC2SEC;

    if (bbr->epoch_ack <= expected_ack
        || (bbr->epoch_ack + sampler->acked
            >= rqc_bbr_ack_epoch_acked_reset_thresh))
    {
        bbr->epoch_ack = 0;
        bbr->extra_ack_stamp = sampler->now;
        expected_ack = 0;
    }
    uint32_t cap = 0xFFFFFU * RQC_BBR_MAX_DATAGRAMSIZE;
    /* Compute excess data delivered, beyond what was expected. */
    bbr->epoch_ack = rqc_min(cap, bbr->epoch_ack + sampler->acked);
    extra_ack = bbr->epoch_ack - expected_ack;
    extra_ack = rqc_min(extra_ack, bbr->congestion_window);

    if (extra_ack > bbr->extra_ack[bbr->extra_ack_idx]) {
        bbr->extra_ack[bbr->extra_ack_idx] = extra_ack;
    }
}

static void
rqc_bbr_check_full_bw_reached(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    /*
     * we MUST only check whether full bw is reached ONCE per RTT!!!
     * Otherwise, startup may end too early due to multiple ACKs arrive in a RTT.
     */
    if (!bbr->round_start || bbr->full_bandwidth_reached
        || (!bbr->ignore_app_limit && sampler->is_app_limited))
    {
        /*
         * In a live video scenario, the applimit state often occurs,
         * causing the startup state to persist for a long time.
         */
        return;
    }

    if (bbr->enable_expect_bw && rqc_bbr_max_bw(bbr) >= bbr->expect_bw) {
        bbr->full_bandwidth_reached = TRUE;
        return;
    }

    uint32_t bw_thresh = bbr->last_bandwidth * rqc_bbr_fullbw_thresh;
    if (rqc_bbr_max_bw(bbr) >= bw_thresh) {
        bbr->last_bandwidth = rqc_bbr_max_bw(bbr);
        bbr->full_bandwidth_cnt = 0;
        return;
    }
    ++bbr->full_bandwidth_cnt;
    bbr->full_bandwidth_reached = bbr->full_bandwidth_cnt >= rqc_bbr_fullbw_cnt;
}

static void
rqc_bbr_enter_drain(rqc_bbr_t *bbr)
{
    bbr->mode = BBR_DRAIN;
    bbr->pacing_gain = rqc_bbr_drain_gain;
    bbr->cwnd_gain = rqc_bbr2_startup_cwnd_gain;
}

static void
rqc_bbr_enter_probe_bw(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    bbr->mode = BBR_PROBE_BW;
    bbr->cwnd_gain = rqc_bbr_cwnd_gain;
    bbr->cycle_idx = rqc_random() % (RQC_BBR_CYCLE_LENGTH - 1);
    bbr->cycle_idx = bbr->cycle_idx == 0 ? bbr->cycle_idx : bbr->cycle_idx + 1;
    bbr->pacing_gain = rqc_bbr_get_pacing_gain(bbr, bbr->cycle_idx);
    bbr->cycle_start_stamp = sampler->now;
    bbr->last_cycle_start = sampler->now;
}

static void
rqc_bbr_check_drain(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    if (bbr->mode == BBR_STARTUP && bbr->full_bandwidth_reached) {
        rqc_bbr_enter_drain(bbr);
    }

    if (bbr->mode == BBR_DRAIN
        && sampler->bytes_inflight <= rqc_bbr_target_cwnd(bbr,
                                                          1.0,
                                                          rqc_bbr_max_bw(bbr)))
    {
#if RQC_BBR_RTTVAR_COMPENSATION_ENABLED
        bbr->rtt_compensation_thresh = rqc_bbr_rtt_compensation_thresh;
#endif
        rqc_bbr_enter_probe_bw(bbr, sampler);
    }
}

static void
rqc_bbr_enter_probe_rtt(rqc_bbr_t *bbr)
{
    bbr->mode = BBR_PROBE_RTT;
    bbr->pacing_gain = 1;
    bbr->cwnd_gain = 1;
}

static void
rqc_bbr_save_cwnd(rqc_bbr_t *bbr)
{
    if (bbr->recovery_mode != BBR_IN_RECOVERY
        && bbr->mode != BBR_PROBE_RTT)
    {
        bbr->prior_cwnd = bbr->congestion_window;

    } else {
        bbr->prior_cwnd = rqc_max(bbr->congestion_window, bbr->prior_cwnd);
    }
}
static void
rqc_bbr_restore_cwnd(rqc_bbr_t *bbr)
{
    bbr->congestion_window = rqc_max(bbr->congestion_window, bbr->prior_cwnd);
}

static void
rqc_bbr_exit_probe_rtt(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    if (bbr->full_bandwidth_reached) {
        rqc_bbr_enter_probe_bw(bbr, sampler);

    } else {
        rqc_bbr_enter_startup(bbr);
    }
}

static void
rqc_bbr_check_probe_rtt_done(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    if (!bbr->probe_rtt_round_done_stamp
        || sampler->now < bbr->probe_rtt_round_done_stamp)
    {
        return;
    }
    /* schedule the next probeRTT round */
    bbr->probe_rtt_min_us_stamp = sampler->now;
    rqc_bbr_restore_cwnd(bbr);
    rqc_bbr_exit_probe_rtt(bbr, sampler);
}

static uint32_t
rqc_bbr_probe_rtt_cwnd(rqc_bbr_t *bbr)
{
    if (rqc_bbr_probe_rtt_gain == 0) {
        return bbr->min_cwnd;
    }

    return rqc_max(bbr->min_cwnd,
                   rqc_bbr_target_cwnd(bbr,
                                       rqc_bbr_probe_rtt_gain,
                                       rqc_bbr_max_bw(bbr)));
}

static void
rqc_bbr_update_min_rtt(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    bool probe_rtt_expired, min_rtt_expired;
    probe_rtt_expired = sampler->now > (bbr->probe_rtt_min_us_stamp +
        rqc_bbr2_probertt_win_size_us);
    if (sampler->rtt <= bbr->probe_rtt_min_us || probe_rtt_expired) {
        bbr->probe_rtt_min_us = sampler->rtt;
        bbr->probe_rtt_min_us_stamp = sampler->now;
    }
    min_rtt_expired = sampler->now >
                      (bbr->min_rtt_stamp + rqc_bbr2_minrtt_win_size_us);
    bbr->min_rtt_expired = min_rtt_expired;
    if (bbr->probe_rtt_min_us <= bbr->min_rtt || min_rtt_expired) {
#ifndef RQC_BBR_DISABLE_CWND_AI
        if (bbr->probe_rtt_min_us_stamp != bbr->min_rtt_stamp
            || min_rtt_expired)
        {
            /*
             * We should remove additional cwnd if background buffer-fillers
             * have gone away or we have increased our target_cwnd by increasing
             * min_rtt.
             */
            bbr->snd_cwnd_cnt_bytes = 0;
            bbr->beyond_target_cwnd = 0;
            /* If we have increased min_rtt, we should reset our accelerating factor. */
            if (min_rtt_expired) {
                bbr->ai_scale = 1;
                bbr->ai_scale_accumulated_bytes = 0;
            }
        }
#endif

        bbr->min_rtt = bbr->probe_rtt_min_us;
        bbr->min_rtt_stamp = bbr->probe_rtt_min_us_stamp;
    }

    if (probe_rtt_expired && bbr->mode != BBR_PROBE_RTT
        && !bbr->idle_restart)
    {
        rqc_bbr_enter_probe_rtt(bbr);
        rqc_bbr_save_cwnd(bbr);
        bbr->probe_rtt_round_done_stamp = 0;
    }
    if (bbr->mode == BBR_PROBE_RTT)
    {
        /* Ignore low rate samples during this mode. */
        rqc_send_ctl_t *send_ctl = sampler->send_ctl;
        send_ctl->ctl_app_limited = (send_ctl->ctl_delivered
            + send_ctl->ctl_bytes_in_flight)? (send_ctl->ctl_delivered
                + send_ctl->ctl_bytes_in_flight) : 1;
        if (!bbr->probe_rtt_round_done_stamp
            && (sampler->bytes_inflight <= rqc_bbr_probe_rtt_cwnd(bbr)))
        {
            bbr->probe_rtt_round_done_stamp = sampler->now +
                                              rqc_min(2*sampler->srtt,
                                              rqc_bbr_probertt_time_us);
            bbr->probe_rtt_round_done = FALSE;
            bbr->next_round_delivered = sampler->total_acked;

        } else if (bbr->probe_rtt_round_done_stamp) {
            if (bbr->round_start) {
                bbr->probe_rtt_round_done = TRUE;
            }
            if (bbr->probe_rtt_round_done) {
                rqc_bbr_check_probe_rtt_done(bbr, sampler);
            }
        }
    }

    /* Restart after idle ends only once we process a new S/ACK for data */
    if (sampler->delivered > 0) {
        bbr->idle_restart = 0;
    }
}

/*
static uint64_t
rqc_bbr_get_min_rtt(rqc_bbr_t *bbr)
{
    return bbr->min_rtt == 0 ? rqc_bbr_initial_rtt_ms * 1000 : bbr->min_rtt;
}
*/

static void
_rqc_bbr_set_pacing_rate_helper(rqc_bbr_t *bbr, float pacing_gain)
{
    uint32_t bandwidth, rate;
    bandwidth = rqc_bbr_bw(bbr);
    rate = bandwidth * pacing_gain * (1.0 - rqc_bbr_pacing_rate_margin_percent);
    if (bbr->full_bandwidth_reached || rate > bbr->pacing_rate) {
        bbr->pacing_rate = rate;
    }
}

static void
rqc_bbr_set_pacing_rate(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    if (!bbr->has_srtt && sampler->srtt) {
        rqc_bbr_init_pacing_rate(bbr, sampler);
    }
    _rqc_bbr_set_pacing_rate_helper(bbr, bbr->pacing_gain);
    if (bbr->pacing_rate == 0) {
        rqc_bbr_init_pacing_rate(bbr, sampler);
        rqc_log(sampler->send_ctl->ctl_conn->log, RQC_LOG_WARN,
                "|rate reached 0|reset pacing_rate:%ud|", bbr->pacing_rate);
    }
}

static void
rqc_bbr_modulate_cwnd_for_recovery(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    if (bbr->just_enter_recovery_mode) {
        bbr->just_enter_recovery_mode = FALSE;
        bbr->packet_conservation = 1;
        bbr->next_round_delivered = sampler->total_acked;
        bbr->congestion_window = sampler->send_ctl->ctl_bytes_in_flight +
                                 rqc_max(sampler->acked,
                                         RQC_BBR_MAX_DATAGRAMSIZE);

    } else if (bbr->just_exit_recovery_mode) {
        /*
         * exit recovery mode once any packet sent
         * during the recovery epoch is acked.
         */
        bbr->just_exit_recovery_mode = FALSE;
        bbr->packet_conservation = 0;
        rqc_bbr_restore_cwnd(bbr);
    }
    if (bbr->packet_conservation) {
        bbr->congestion_window = rqc_max(bbr->congestion_window,
                                 sampler->send_ctl->ctl_bytes_in_flight +
                                 sampler->acked);
    }
}

static void
rqc_bbr_reset_cwnd(void *cong_ctl)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong_ctl;
    rqc_bbr_save_cwnd(bbr);
    /* reduce cwnd to the minimal value */
    bbr->congestion_window = bbr->min_cwnd;
    /* cancel recovery state */
    if (bbr->recovery_mode == BBR_IN_RECOVERY) {
        bbr->recovery_mode = BBR_NOT_IN_RECOVERY;
        bbr->packet_conservation = 0;
        /* we do not restore cwnd here */
    }
    /* reset recovery start time in any case */
    bbr->recovery_start_time = 0;

    if (bbr->lt_bw_enabled) {
        rqc_bbr_reset_lt_bw_sampling(bbr);
    }

#ifndef RQC_BBR_DISABLE_CWND_AI
    /* If losses happened, we do not increase cwnd beyond target_cwnd. */
    bbr->snd_cwnd_cnt_bytes = 0;
    bbr->beyond_target_cwnd = 0;
    bbr->ai_scale = 1;
    bbr->ai_scale_accumulated_bytes = 0;
#endif
}

#ifndef RQC_BBR_DISABLE_CWND_AI
static void
rqc_bbr_cong_avoid_ai(rqc_bbr_t *bbr, uint32_t cwnd, uint32_t acked)
{
    /* growing 2x cwnd at maximum per RTT */
    uint32_t cwnd_thresh = rqc_max(RQC_BBR_MAX_DATAGRAMSIZE,
                                   cwnd / bbr->ai_scale);
    if (bbr->snd_cwnd_cnt_bytes >= cwnd_thresh) {
        bbr->beyond_target_cwnd += RQC_BBR_MAX_DATAGRAMSIZE;
        bbr->snd_cwnd_cnt_bytes = 0;
    }
    bbr->snd_cwnd_cnt_bytes += acked;
    if (bbr->snd_cwnd_cnt_bytes >= cwnd_thresh) {
        uint32_t delta = bbr->snd_cwnd_cnt_bytes / cwnd_thresh;
        bbr->snd_cwnd_cnt_bytes -= delta * cwnd_thresh;
        bbr->beyond_target_cwnd += delta * RQC_BBR_MAX_DATAGRAMSIZE;
    }
    /* update ai_scale: we want to double ai_scale when enough data is acked. */
    bbr->ai_scale_accumulated_bytes += acked;
    if (bbr->ai_scale_accumulated_bytes >= cwnd_thresh) {
        uint32_t delta = bbr->ai_scale_accumulated_bytes / cwnd_thresh;
        bbr->ai_scale = rqc_min(bbr->ai_scale + delta, RQC_BBR_MAX_AI_SCALE);
        bbr->ai_scale_accumulated_bytes -= delta * cwnd_thresh;
    }
}
#endif

static void
rqc_bbr_set_cwnd(rqc_bbr_t *bbr, rqc_sample_t *sampler)
{
    if (sampler->acked != 0) {
        rqc_send_ctl_t *send_ctl = sampler->send_ctl;

        uint32_t target_cwnd, extra_cwnd;
        target_cwnd = rqc_bbr_target_cwnd(bbr, bbr->cwnd_gain, rqc_bbr_bw(bbr));
        extra_cwnd = rqc_bbr_ack_aggregation_cwnd(bbr);
        target_cwnd += extra_cwnd;

#if RQC_BBR_RTTVAR_COMPENSATION_ENABLED
        if (bbr->rttvar_compensation_on) {
            uint32_t cwnd_for_rttvar;
            cwnd_for_rttvar = rqc_bbr_compensate_cwnd_for_rttvar(bbr, sampler);
            target_cwnd += cwnd_for_rttvar;
        }
#endif

        rqc_bbr_modulate_cwnd_for_recovery(bbr, sampler);
        if (!bbr->packet_conservation) {
            if (bbr->full_bandwidth_reached) {
#ifndef RQC_BBR_DISABLE_CWND_AI
                if ((bbr->congestion_window + sampler->acked
                    >= (target_cwnd + bbr->beyond_target_cwnd))
                    && sampler->send_ctl->ctl_is_cwnd_limited)
                {
                    /* We are limited by target_cwnd */
                    rqc_bbr_cong_avoid_ai(bbr, rqc_bbr_target_cwnd(bbr, 1.0), sampler->acked);
                }
                /* additive increasing target_cwnd */
                target_cwnd += bbr->beyond_target_cwnd;
#endif
                bbr->congestion_window = rqc_min(target_cwnd,
                                                bbr->congestion_window +
                                                sampler->acked);

            } else if (bbr->congestion_window < target_cwnd
                    || send_ctl->ctl_delivered < bbr->initial_congestion_window)
            {
                bbr->congestion_window += sampler->acked;
            }
        }
        bbr->congestion_window = rqc_max(bbr->congestion_window, bbr->min_cwnd);
    }
    if (bbr->mode == BBR_PROBE_RTT) {
        bbr->congestion_window = rqc_min(bbr->congestion_window,
                                         rqc_bbr_probe_rtt_cwnd(bbr));
    }
}

static void
rqc_bbr_on_lost(void *cong_ctl, rqc_usec_t lost_sent_time)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong_ctl;

    if (bbr->mode == BBR_STARTUP) {
        /* do not enter loss recovery state during startup */
        return;
    }

    /*
     * Unlike the definition of "recovery epoch" for loss-based CCs,
     * for the sake of resistance to losses, we MUST refresh the end of a
     * recovery epoch if further losses happen in the epoch. Otherwise, the
     * ability of BBR to sustain network where high loss rate presents
     * is hampered because of frequently entering packet conservation state.
     */

    rqc_bbr_save_cwnd(bbr);
    bbr->recovery_start_time = rqc_monotonic_timestamp();

#ifndef RQC_BBR_DISABLE_CWND_AI
    /* If losses happened, we do not increase cwnd beyond target_cwnd. */
    bbr->snd_cwnd_cnt_bytes = 0;
    bbr->beyond_target_cwnd = 0;
    bbr->ai_scale = 1;
    bbr->ai_scale_accumulated_bytes = 0;
#endif
}

static void
rqc_bbr_set_or_restore_pacing_gain_in_startup(void *cong_ctl)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong_ctl;
    if (bbr->mode == BBR_STARTUP) {
        if (bbr->recovery_mode == BBR_IN_RECOVERY) {
            bbr->pacing_gain = rqc_bbr2_startup_pacing_gain_on_lost;
        }
        if (bbr->recovery_mode == BBR_NOT_IN_RECOVERY) {
            bbr->pacing_gain = rqc_bbr_high_gain;
        }
    }
}

static void
rqc_bbr_update_recovery_mode(void *cong_ctl, rqc_sample_t *sampler)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong_ctl;
    if (sampler->po_sent_time <= bbr->recovery_start_time
        && bbr->recovery_mode == BBR_NOT_IN_RECOVERY)
    {
        bbr->just_enter_recovery_mode = TRUE;
        bbr->recovery_mode = BBR_IN_RECOVERY;

    }
    else if (sampler->po_sent_time > bbr->recovery_start_time
             && bbr->recovery_mode == BBR_IN_RECOVERY)
    {
        /* exit recovery mode once any packet sent during the recovery epoch is acked. */
        bbr->recovery_mode = BBR_NOT_IN_RECOVERY;
        bbr->just_exit_recovery_mode = TRUE;
        bbr->recovery_start_time = 0;
    }
}

static void
rqc_bbr_on_ack(void *cong_ctl, rqc_sample_t *sampler)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)(cong_ctl);
#if RQC_BBR_RTTVAR_COMPENSATION_ENABLED
    /* maintain windowed max rtt here */
    if (bbr->rttvar_compensation_on) {
        if (sampler->rtt >= 0) {
            rqc_usec_t last_max_rtt = rqc_win_filter_get(&bbr->max_rtt);
            rqc_win_filter_max(&bbr->max_rtt, bbr->max_rtt_win_len,
                                   bbr->round_cnt, sampler->rtt);
        }
    }
#endif
    /* Update model and state */
    rqc_bbr_update_bandwidth(bbr, sampler);
    rqc_update_ack_aggregation(bbr, sampler);
    rqc_bbr_update_cycle_phase(bbr, sampler);
    rqc_bbr_check_full_bw_reached(bbr, sampler);
    rqc_bbr_check_drain(bbr, sampler);
    rqc_bbr_update_min_rtt(bbr, sampler);

    rqc_bbr_update_recovery_mode(bbr, sampler);
    if (rqc_bbr2_slow_down_startup_on_lost) {
        rqc_bbr_set_or_restore_pacing_gain_in_startup(bbr);
    }
    /* Update control parameter */
    rqc_bbr_set_pacing_rate(bbr, sampler);
    rqc_bbr_set_cwnd(bbr, sampler);
}

static uint64_t
rqc_bbr_get_cwnd(void *cong_ctl)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)(cong_ctl);
    return bbr->congestion_window;
}

static uint32_t
rqc_bbr_get_pacing_rate(void *cong_ctl)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)(cong_ctl);
    rqc_usec_t min_rtt = (bbr->min_rtt && (bbr->min_rtt != RQC_BBR_INF) ? bbr->min_rtt : 10000);
    uint32_t min_pacing_rate = bbr->min_cwnd * (uint64_t)MSEC2SEC / min_rtt;
    return rqc_max(bbr->pacing_rate, bbr->pacing_gain * min_pacing_rate);
}

static uint32_t
rqc_bbr_get_bandwidth(void *cong_ctl)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)(cong_ctl);
    return rqc_bbr_bw(bbr);
}

static void
rqc_bbr_restart_from_idle(void *cong_ctl, uint64_t conn_delivered)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)(cong_ctl);
    uint64_t now = rqc_monotonic_timestamp();
    bbr->idle_restart = 1;
    bbr->extra_ack_stamp = now;
    bbr->epoch_ack = 0;
    rqc_sample_t sampler = {.now = now, .total_acked = conn_delivered};

    if (bbr->lt_bw_enabled) {
        rqc_bbr_reset_lt_bw_sampling(bbr);
    }

    if (bbr->mode == BBR_PROBE_BW) {
        _rqc_bbr_set_pacing_rate_helper(bbr, 1.0);
        if (bbr->pacing_rate == 0) {
            rqc_bbr_init_pacing_rate(bbr, &sampler);
        }
    } else if (bbr->mode == BBR_PROBE_RTT) {
        rqc_bbr_check_probe_rtt_done(bbr, &sampler);
    }
}

/* These functions are mainly for debug */
static uint8_t
rqc_bbr_info_mode(void *cong)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong;
    return bbr->mode;
}

static uint64_t
rqc_bbr_info_min_rtt(void *cong)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong;
    return bbr->min_rtt;
}

static uint8_t
rqc_bbr_info_idle_restart(void *cong)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong;
    return bbr->idle_restart;
}

static uint8_t
rqc_bbr_info_full_bw_reached(void *cong)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong;
    return bbr->full_bandwidth_reached;
}

static uint8_t
rqc_bbr_info_recovery_mode(void *cong)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong;
    return bbr->recovery_mode;
}

static uint64_t
rqc_bbr_info_recovery_start_time(void *cong)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong;
    return bbr->recovery_start_time;
}

static uint8_t
rqc_bbr_info_packet_conservation(void *cong)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong;
    return bbr->packet_conservation;
}

static uint8_t
rqc_bbr_info_round_start(void *cong)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong;
    return bbr->round_start;
}

static float
rqc_bbr_info_pacing_gain(void *cong)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong;
    return bbr->pacing_gain;
}

static float
rqc_bbr_info_cwnd_gain(void *cong)
{
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong;
    return bbr->cwnd_gain;
}

static int
rqc_bbr_in_recovery(void *cong) {
    rqc_bbr_t *bbr = (rqc_bbr_t *)cong;
    return bbr->recovery_start_time > 0;
}

static int
rqc_bbr_in_slow_start(void *cong)
{
    rqc_bbr_t *bbr = (rqc_bbr_t*)cong;
    return bbr->mode == BBR_STARTUP;
}

static rqc_bbr_info_interface_t rqc_bbr_info_cb = {
    .mode                 = rqc_bbr_info_mode,
    .min_rtt              = rqc_bbr_info_min_rtt,
    .idle_restart         = rqc_bbr_info_idle_restart,
    .full_bw_reached      = rqc_bbr_info_full_bw_reached,
    .recovery_mode        = rqc_bbr_info_recovery_mode,
    .recovery_start_time  = rqc_bbr_info_recovery_start_time,
    .packet_conservation  = rqc_bbr_info_packet_conservation,
    .round_start          = rqc_bbr_info_round_start,
    .pacing_gain          = rqc_bbr_info_pacing_gain,
    .cwnd_gain            = rqc_bbr_info_cwnd_gain,
};

const rqc_cong_ctrl_callback_t rqc_bbr_cb = {
    .rqc_cong_ctl_size                    = rqc_bbr_size,
    .rqc_cong_ctl_init_bbr                = rqc_bbr_init,
    .rqc_cong_ctl_on_ack_multiple_pkts    = rqc_bbr_on_ack,
    .rqc_cong_ctl_get_cwnd                = rqc_bbr_get_cwnd,
    .rqc_cong_ctl_get_pacing_rate         = rqc_bbr_get_pacing_rate,
    .rqc_cong_ctl_get_bandwidth_estimate  = rqc_bbr_get_bandwidth,
    .rqc_cong_ctl_restart_from_idle       = rqc_bbr_restart_from_idle,
    .rqc_cong_ctl_on_lost                 = rqc_bbr_on_lost,
    .rqc_cong_ctl_reset_cwnd              = rqc_bbr_reset_cwnd,
    .rqc_cong_ctl_info_cb                 = &rqc_bbr_info_cb,
    .rqc_cong_ctl_in_recovery             = rqc_bbr_in_recovery,
    .rqc_cong_ctl_in_slow_start           = rqc_bbr_in_slow_start,
};
