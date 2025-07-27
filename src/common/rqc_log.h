/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_H_LOG_INCLUDED_
#define _RQC_H_LOG_INCLUDED_

#include <time.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <rquic/rquic.h>
#include <rquic/rquic_typedef.h>
#include "src/common/rqc_config.h"
#include "src/common/rqc_malloc.h"
#include "src/common/rqc_str.h"
#include "src/common/rqc_time.h"

#if !defined(RQC_SYS_WINDOWS) || defined(RQC_ON_MINGW)
#include <unistd.h>
#endif

/* max length for log buffer */
#define RQC_MAX_LOG_LEN 2048

#define RQC_LOG_REMOTE_EVENT    0
#define RQC_LOG_LOCAL_EVENT     1

#define RQC_LOG_STREAM_SEND     0
#define RQC_LOG_STREAM_RECV     1

#define RQC_LOG_DECODER_EVENT   0
#define RQC_LOG_ENCODER_EVENT   1

#define RQC_LOG_DTABLE_INSERTED 0
#define RQC_LOG_DTABLE_EVICTED  1

#define RQC_LOG_BLOCK_PREFIX    0
#define RQC_LOG_HEADER_BLOCK    1
#define RQC_LOG_HEADER_FRAME    2

#define RQC_LOG_TIMER_SET       0
#define RQC_LOG_TIMER_EXPIRE    1
#define RQC_LOG_TIMER_CANCEL    2

typedef enum {
    /* connectivity event */
    CON_SERVER_LISTENING,
    CON_CONNECTION_STARTED,
    CON_CONNECTION_CLOSED,
    CON_CONNECTION_STATE_UPDATED,
    CON_PATH_ASSIGNED,

    /* transport event */
    TRA_VERSION_INFORMATION,
    TRA_ALPN_INFORMATION,
    TRA_PARAMETERS_SET,
    TRA_PACKET_SENT,
    TRA_PACKET_RECEIVED,
    TRA_PACKET_DROPPED,
    TRA_PACKET_BUFFERED,
    TRA_PACKETS_ACKED,
    TRA_DATAGRAMS_SENT,
    TRA_DATAGRAMS_RECEIVED,
    TRA_DATAGRAM_DROPPED,
    TRA_STREAM_STATE_UPDATED,
    TRA_FRAMES_PROCESSED,
    TRA_STREAM_DATA_MOVED,
    TRA_DATAGRAM_DATA_MOVED,

    /* recovery event */
    REC_PARAMETERS_SET,
    REC_METRICS_UPDATED,
    REC_CONGESTION_STATE_UPDATED,
    REC_LOSS_TIMER_UPDATED,
    REC_PACKET_LOST,
    REC_MARKED_FOR_RETRANSMIT,

    /* generic event */
    GEN_REPORT,
    GEN_FATAL,
    GEN_ERROR,
    GEN_WARN,
    GEN_STATS,
    GEN_INFO,
    GEN_DEBUG,
} rqc_log_type_t;

typedef struct rqc_log_s {
    rqc_log_level_t                 log_level;
    qlog_event_importance_t         qlog_importance;
    rqc_flag_t                      log_event; /* 1:enable log event, 0:disable log event */
    rqc_flag_t                      log_timestamp; /* 1:add timestamp before log, 0:don't need timestamp */
    rqc_flag_t                      log_level_name; /* 1:add level name before log, 0:don't need level name */
    unsigned char                  *scid;
    rqc_engine_t                   *engine;
    rqc_log_callbacks_t            *log_callbacks;
    void                           *user_data;
} rqc_log_t;

static inline rqc_log_t *
rqc_log_init(rqc_log_level_t log_level, rqc_flag_t log_event, qlog_event_importance_t qlog_imp, rqc_flag_t log_timestamp, rqc_flag_t log_level_name,
    rqc_engine_t *engine, rqc_log_callbacks_t *log_callbacks, void *user_data)
{
    rqc_log_t* log = rqc_malloc(sizeof(rqc_log_t));
    if (log == NULL) {
        return NULL;
    }

    log->log_level = log_level;
    log->user_data = user_data;
    log->scid = NULL;
    log->log_event = log_event;
    log->log_timestamp = log_timestamp;
    log->log_level_name = log_level_name;
    log->log_callbacks = log_callbacks;
    log->qlog_importance = qlog_imp;
    log->engine = engine;
    return log;
}

static inline void
rqc_log_release(rqc_log_t* log)
{
    rqc_free(log);
    log = NULL;
}

void
rqc_log_level_set(rqc_log_t *log, rqc_log_level_t level);

rqc_log_level_t
rqc_log_type_2_level(rqc_log_type_t type);

qlog_event_importance_t
rqc_qlog_event_2_level(rqc_log_type_t type);

rqc_log_level_t
qlog_importance_2_log_level(rqc_log_type_t type);

rqc_log_type_t
rqc_log_event_type(rqc_log_level_t level);

const char *
rqc_log_type_str(rqc_log_type_t type);

void
rqc_log_time(char *buf, size_t buf_len);

void
rqc_log_implement(rqc_log_t *log, rqc_log_type_t type, const char *func, const char *fmt, ...);

void
rqc_qlog_implement(rqc_log_t *log, rqc_log_type_t type, const char *func, const char *fmt, ...);

#ifndef RQC_DISABLE_LOG
    #ifndef RQC_ONLY_ERROR_LOG
    #define rqc_log(log, level, ...) \
    do { \
        if ((log)->log_level >= level) { \
            rqc_log_implement(log, rqc_log_event_type(level), __FUNCTION__, __VA_ARGS__); \
        } \
    } while (0)
    #else
    #define rqc_log(log, level, ...) \
        do { \
            if (RQC_LOG_ERROR >= level) { \
                rqc_log_implement(log, rqc_log_event_type(level), __FUNCTION__, __VA_ARGS__); \
            } \
        } while (0)
    #endif

    #ifdef RQC_ENABLE_EVENT_LOG
    #define rqc_log_event(log, type, ...) \
    do {                                  \
        if ((log)->log_event) {           \
            if ((log)->qlog_importance >= rqc_qlog_event_2_level(type) || \
                ((log)->qlog_importance == EVENT_IMPORTANCE_SELECTED && \
                (log)->log_level >= qlog_importance_2_log_level(type))) { \
                rqc_log_##type##_callback(log, __FUNCTION__, __VA_ARGS__); \
            }                             \
        }                                 \
    } while (0)
    #else
    #define rqc_log_event(log, type, ...)
    #endif
#else
#define rqc_log(log, level, ...)
#define rqc_log_event(log, type, ...)
#endif

#define rqc_conn_log(conn, level, fmt, ...) \
    rqc_log(conn->log, level, "|%s " fmt, rqc_conn_addr_str(conn), ##__VA_ARGS__ )

#define rqc_log_fatal(log, ...) \
    do {\
        if ((log)->log_level >= RQC_LOG_FATAL) { \
            rqc_log_implement(log, rqc_log_event_type(RQC_LOG_FATAL), __FUNCTION__, __VA_ARGS__); \
        } \
    } while (0)

#define rqc_log_error(log, ...) \
    do {\
        if ((log)->log_level >= RQC_LOG_ERROR) { \
            rqc_log_implement(log, rqc_log_event_type(RQC_LOG_ERROR), __FUNCTION__, __VA_ARGS__); \
        } \
    } while (0)

#define rqc_log_warn(log, ...) \
    do {\
        if ((log)->log_level >= RQC_LOG_WARN) { \
            rqc_log_implement(log, rqc_log_event_type(RQC_LOG_WARN), __FUNCTION__, __VA_ARGS__); \
        } \
    } while (0)

#define rqc_log_info(log, ...) \
    do {\
        if ((log)->log_level >= RQC_LOG_INFO) { \
            rqc_log_implement(log, rqc_log_event_type(RQC_LOG_INFO), __FUNCTION__, __VA_ARGS__); \
        } \
    } while (0)

#define rqc_log_debug(log, ...) \
    do {\
        if ((log)->log_level >= RQC_LOG_DEBUG) { \
            rqc_log_implement(log, rqc_log_event_type(RQC_LOG_DEBUG), __FUNCTION__, __VA_ARGS__); \
        } \
    } while (0)

#endif /*_RQC_H_LOG_INCLUDED_*/
