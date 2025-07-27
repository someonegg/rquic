/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "src/congestion_control/rqc_window_filter.h"

/**
 * As time advances, update the 3 estimate value
 * Check the third value was in the window on entry
 */

static uint64_t
rqc_win_filter_update(rqc_win_filter_t *w, uint64_t win,
                          struct rqc_win_sample *nsample)
{
    uint32_t dt = nsample->t - w->s[0].t;

    if (dt > win) {
        w->s[0] = w->s[1];
        w->s[1] = w->s[2];
        w->s[2] = *nsample;
        if (nsample->t - w->s[0].t > win) {
            w->s[0] = w->s[1];
            w->s[1] = w->s[2];
            w->s[2] = *nsample;
        }

    } else if ((w->s[1].t == w->s[0].t) && dt > win/4) {
        w->s[2] = w->s[1] = *nsample;

    } else if ((w->s[2].t == w->s[1].t) && dt > win/2) {
        w->s[2] = *nsample;
    }

    return w->s[0].val;
}

uint64_t
rqc_win_filter_max(rqc_win_filter_t *w, uint64_t win,
                       uint64_t t, uint64_t nval)
{
    struct rqc_win_sample nsample = {.t = t, .val = nval};

    if ((nval >= w->s[0].val) || (t - w->s[2].t > win)) {
        return rqc_win_filter_reset(w, t, nval);
    }

    if (nval >= w->s[1].val) {
        w->s[2] = w->s[1] = nsample;

    } else if (nval >= w->s[2].val) {
        w->s[2] = nsample;
    }

    return rqc_win_filter_update(w, win, &nsample);
}

uint64_t
rqc_win_filter_min(rqc_win_filter_t *w, uint64_t win,
                       uint64_t t, uint64_t nval)
{
    struct rqc_win_sample nsample = {.t = t, .val = nval};

    if ((nval <= w->s[0].val) || (t - w->s[2].t > win)) {
        return rqc_win_filter_reset(w, t, nval);
    }

    if (nval <= w->s[1].val) {
        w->s[2] = w->s[1] = nsample;

    } else if (nval <= w->s[2].val) {
        w->s[2] = nsample;
    }

    return rqc_win_filter_update(w, win, &nsample);
}
