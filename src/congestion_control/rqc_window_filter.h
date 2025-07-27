/*
 * Copyright (c) 2022, Alibaba Group Holding Limited
 *
 * Implements Kathleen Nichols' algorithm for tracking the minimum (or maximum)
 * estimate of a stream of samples over some fixed time interval
 */

#ifndef _RQC_WIN_FILTER_H_INCLUDED_
#define _RQC_WIN_FILTER_H_INCLUDED_

#include <rquic/rquic_typedef.h>
#include <rquic/rquic.h>

struct rqc_win_sample {
    uint64_t t;
    uint64_t val;
};

typedef struct {
    struct rqc_win_sample s[3];
} rqc_win_filter_t;

static inline uint64_t
rqc_win_filter_get(const rqc_win_filter_t *w)
{
    return w->s[0].val;
}

static inline uint64_t
rqc_win_filter_reset(rqc_win_filter_t *w, uint64_t t, uint64_t nval)
{
    struct rqc_win_sample nsample = {.t = t, .val = nval };
    w->s[0] = w->s[1] = w->s[2] = nsample;
    return w->s[0].val;
}

uint64_t rqc_win_filter_max(rqc_win_filter_t *w,
                                uint64_t win,
                                uint64_t t,
                                uint64_t nval);

uint64_t rqc_win_filter_min(rqc_win_filter_t *w,
                                uint64_t win,
                                uint64_t t,
                                uint64_t nval);

#endif
