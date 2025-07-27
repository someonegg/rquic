/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_TIME_H_INCLUDED_
#define _RQC_TIME_H_INCLUDED_

#include <time.h>
#include <rquic/rquic.h>
#include <rquic/rquic_typedef.h>

#if !defined(RQC_SYS_WINDOWS) || defined(RQC_ON_MINGW)
#include <sys/time.h>
#endif

#ifdef RQC_SYS_WINDOWS
#ifndef _GETTIMEOFDAY_DEFINED
int gettimeofday(struct timeval *tv, struct timezone *tz);
#endif
#endif

/**
 * @brief get realtime timestamp
 */
extern rqc_timestamp_pt rqc_realtime_timestamp;

/**
 * @brief get monotonic increasing timestamp
 */
extern rqc_timestamp_pt rqc_monotonic_timestamp;

#endif /* _RQC_TIME_H_INCLUDED_ */
