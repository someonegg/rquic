/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "rqc_log.h"
#include "src/transport/rqc_engine.h"

#ifdef PRINT_MALLOC
FILE *g_malloc_info_fp;
#endif

static rqc_bool_t log_disable = RQC_FALSE;

void
rqc_log_disable(rqc_bool_t disable)
{
    log_disable = disable;
}

void
rqc_log_level_set(rqc_log_t *log, rqc_log_level_t level)
{
    log->log_level = level;
}

rqc_log_type_t
rqc_log_event_type(rqc_log_level_t level)
{
    switch (level) {
    case RQC_LOG_REPORT:
        return GEN_REPORT;
    case RQC_LOG_FATAL:
        return GEN_FATAL;
    case RQC_LOG_ERROR:
        return GEN_ERROR;
    case RQC_LOG_WARN:
        return GEN_WARN;
    case RQC_LOG_STATS:
        return GEN_STATS;
    case RQC_LOG_INFO:
        return GEN_INFO;
    case RQC_LOG_DEBUG:
        return GEN_DEBUG;
    default:
        return GEN_DEBUG;
    }
}

rqc_log_level_t
rqc_log_type_2_level(rqc_log_type_t type)
{
    switch (type) {
    case GEN_REPORT:
        return RQC_LOG_REPORT;
    case GEN_FATAL:
        return RQC_LOG_FATAL;
    case GEN_ERROR:
        return RQC_LOG_ERROR;
    case GEN_WARN:
        return RQC_LOG_WARN;
    case GEN_STATS:
        return RQC_LOG_STATS;
    case GEN_INFO:
        return RQC_LOG_INFO;
    case GEN_DEBUG:
        return RQC_LOG_DEBUG;
    default:
        return RQC_LOG_DEBUG;
    }
}

qlog_event_importance_t
rqc_qlog_event_2_level(rqc_log_type_t type)
{
    switch (type) {
    /* draft-ietf-quic-qlog-quic-events */
    case CON_SERVER_LISTENING:
        return EVENT_IMPORTANCE_EXTRA;
    case CON_CONNECTION_STARTED:
    case CON_CONNECTION_CLOSED:
    case CON_CONNECTION_STATE_UPDATED:
    case CON_PATH_ASSIGNED:
        return EVENT_IMPORTANCE_BASE;

    case TRA_VERSION_INFORMATION:
    case TRA_ALPN_INFORMATION:
    case TRA_PARAMETERS_SET:
        return EVENT_IMPORTANCE_CORE;

    case TRA_PACKET_SENT:
    case TRA_PACKET_RECEIVED:
        return EVENT_IMPORTANCE_CORE;

    case TRA_PACKET_DROPPED:
    case TRA_PACKET_BUFFERED:
        return EVENT_IMPORTANCE_BASE;

    case TRA_PACKETS_ACKED:
    case TRA_DATAGRAM_DROPPED:
    case TRA_DATAGRAMS_SENT:
    case TRA_DATAGRAMS_RECEIVED:
        return EVENT_IMPORTANCE_EXTRA;

    case TRA_STREAM_STATE_UPDATED:
        return EVENT_IMPORTANCE_BASE;

    case TRA_FRAMES_PROCESSED:
        return EVENT_IMPORTANCE_EXTRA;

    case TRA_STREAM_DATA_MOVED:
    case TRA_DATAGRAM_DATA_MOVED:
        return EVENT_IMPORTANCE_BASE;

    case REC_METRICS_UPDATED:
        return EVENT_IMPORTANCE_EXTRA;

    case REC_PARAMETERS_SET:
        return EVENT_IMPORTANCE_BASE;

    case REC_CONGESTION_STATE_UPDATED:
        return EVENT_IMPORTANCE_BASE;

    case REC_LOSS_TIMER_UPDATED:
        return EVENT_IMPORTANCE_EXTRA;

    case REC_PACKET_LOST:
        return EVENT_IMPORTANCE_CORE;

    case REC_MARKED_FOR_RETRANSMIT:
        return EVENT_IMPORTANCE_EXTRA;

    default:
        return EVENT_IMPORTANCE_EXTRA;
    }
}

rqc_log_level_t
qlog_importance_2_log_level(rqc_log_type_t type){
    if (type >= GEN_REPORT){
        return rqc_log_type_2_level(type);
    }
    switch (type)
    {
    /* datagrame, packet, frame level event should be rqc_log_debug */
    case TRA_PACKETS_ACKED:
    case TRA_DATAGRAM_DROPPED:
    case TRA_DATAGRAMS_SENT:
    case TRA_DATAGRAMS_RECEIVED:
    case TRA_PACKET_SENT:
    case TRA_PACKET_RECEIVED:
    case TRA_FRAMES_PROCESSED:
    case TRA_STREAM_DATA_MOVED:
    case TRA_DATAGRAM_DATA_MOVED:
    case REC_LOSS_TIMER_UPDATED:
    case REC_PACKET_LOST:
    case REC_MARKED_FOR_RETRANSMIT:
    case TRA_PACKET_DROPPED:
    case TRA_PACKET_BUFFERED:
        return RQC_LOG_DEBUG;

    default:
        return RQC_LOG_INFO;
    }
}

const char *
rqc_log_type_str(rqc_log_type_t type)
{
    static const char *event_type2str[] = {
            [CON_SERVER_LISTENING]              = "server_listening",
            [CON_CONNECTION_STARTED]            = "connection_started",
            [CON_CONNECTION_CLOSED]             = "connection_closed",
            [CON_CONNECTION_STATE_UPDATED]      = "connection_state_updated",
            [CON_PATH_ASSIGNED]                 = "path_assigned",
            [TRA_VERSION_INFORMATION]           = "version_information",
            [TRA_ALPN_INFORMATION]              = "alpn_information",
            [TRA_PARAMETERS_SET]                = "tra_parameters_set",
            [TRA_PACKET_SENT]                   = "packet_sent",
            [TRA_PACKET_RECEIVED]               = "packet_received",
            [TRA_PACKET_DROPPED]                = "packet_dropped",
            [TRA_PACKET_BUFFERED]               = "packet_buffered",
            [TRA_PACKETS_ACKED]                 = "packets_acked",
            [TRA_DATAGRAMS_SENT]                = "datagrams_sent",
            [TRA_DATAGRAMS_RECEIVED]            = "datagrams_received",
            [TRA_DATAGRAM_DROPPED]              = "datagram_dropped",
            [TRA_STREAM_STATE_UPDATED]          = "stream_state_updated",
            [TRA_FRAMES_PROCESSED]              = "frames_processed",
            [TRA_STREAM_DATA_MOVED]             = "stream_data_moved",
            [TRA_DATAGRAM_DATA_MOVED]           = "datagram_data_moved",
            [REC_PARAMETERS_SET]                = "rec_parameters_set",
            [REC_METRICS_UPDATED]               = "rec_metrics_updated",
            [REC_CONGESTION_STATE_UPDATED]      = "congestion_state_updated",
            [REC_LOSS_TIMER_UPDATED]            = "loss_timer_updated",
            [REC_PACKET_LOST]                   = "packet_lost",
            [REC_MARKED_FOR_RETRANSMIT]         = "marked_for_retransmit",
            [GEN_REPORT]                        = "report",
            [GEN_FATAL]                         = "fatal",
            [GEN_ERROR]                         = "error",
            [GEN_WARN]                          = "warn",
            [GEN_STATS]                         = "stats",
            [GEN_INFO]                          = "info",
            [GEN_DEBUG]                         = "debug",
    };
    return event_type2str[type];
}

void
rqc_log_implement(rqc_log_t *log, rqc_log_type_t type, const char *func, const char *fmt, ...)
{
    /* do nothing if switch is off */
    if (log_disable) {
        return;
    }

    rqc_log_level_t level = rqc_log_type_2_level(type);
    if (level > log->log_level) {
        return;
    }

    unsigned char   buf[RQC_MAX_LOG_LEN] = {0};
    unsigned char  *p = buf;
    unsigned char  *last = buf + sizeof(buf);

    /* do not need time & level if use outside log format */
    if (log->log_timestamp) {
        /* time */
        char time[64];
        rqc_log_time(time, sizeof(time));
        p = rqc_sprintf(p, last, "[%s] ", time);
    }

    if (log->log_level_name) {
        /* log level */
        p = rqc_sprintf(p, last, "[%s] ", rqc_log_type_str(type));
    }

    if (log->scid != NULL) {
        p = rqc_sprintf(p, last, "|scid:%s|%s", log->scid, func);
    } else {
        p = rqc_sprintf(p, last, "|%s", func);
    }

    /* log */
    va_list args;
    va_start(args, fmt);
    p = rqc_vsprintf(p, last, fmt, args);
    va_end(args);

    if (p + 1 < last) {
        /* may use printf("%s") outside, add '\0' and don't count into size */
        *p = '\0';
    }

    /* RQC_LOG_STATS & RQC_LOG_REPORT are levels for statistic */
    if ((level == RQC_LOG_STATS || level == RQC_LOG_REPORT)
        && log->log_callbacks->rqc_log_write_stat)
    {
        log->log_callbacks->rqc_log_write_stat(level, buf, p - buf, log->user_data);

    } else if (log->log_callbacks->rqc_log_write_err) {
        log->log_callbacks->rqc_log_write_err(level, buf, p - buf, log->user_data);
    }

    /* if didn't set log callback, just return */
}

void
rqc_qlog_implement(rqc_log_t *log, rqc_log_type_t type, const char *func, const char *fmt, ...)
{
    /* do nothing if switch is off */
    if (log_disable) {
        return;
    }

    if (!log->log_event){
        return;
    }

    qlog_event_importance_t event_imp = rqc_qlog_event_2_level(type);
    /* EVENT_IMPORTANCE_SELECTED: events will be emitted based on their map log level and cur log level */
    rqc_log_level_t level = RQC_LOG_DEBUG;
    if (log->qlog_importance == EVENT_IMPORTANCE_SELECTED) {
        level = qlog_importance_2_log_level(type);
        if (level > log->log_level) {
            return;
        }
    }

    unsigned char   buf[RQC_MAX_LOG_LEN] = {0};
    unsigned char  *p = buf;
    unsigned char  *last = buf + sizeof(buf);

    /* do not need time & level if use outside log format */
    if (log->log_timestamp) {
        /* time */
        char time[64];
        rqc_log_time(time, sizeof(time));
        p = rqc_sprintf(p, last, "[%s] ", time);
    }

    if (log->log_level_name) {
        /* qlog event */
        p = rqc_sprintf(p, last, "[%s] ", rqc_log_type_str(type));
    }

    if (log->scid != NULL) {
        p = rqc_sprintf(p, last, "|scid:%s|%s", log->scid, func);
    } else {
        p = rqc_sprintf(p, last, "|%s", func);
    }

    /* log */
    va_list args;
    va_start(args, fmt);
    p = rqc_vsprintf(p, last, fmt, args);
    va_end(args);

    if (p + 1 < last) {
        /* may use printf("%s") outside, add '\0' and don't count into size */
        *p = '\0';
    }
    /* EVENT_IMPORTANCE_SELECTED: event logs output with rqc_log via rqc_log_write_err callback */
    if (log->qlog_importance == EVENT_IMPORTANCE_SELECTED) {
        if (log->log_callbacks->rqc_log_write_err) {
            log->log_callbacks->rqc_log_write_err(level, buf, p - buf, log->user_data);
        }
    }
    else if(log->log_callbacks->rqc_qlog_event_write) {
        log->log_callbacks->rqc_qlog_event_write(event_imp, buf, p - buf, log->user_data);
    }
}

void
rqc_log_time(char *buf, size_t buf_len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm tm;

#ifdef RQC_SYS_WINDOWS
    time_t t = tv.tv_sec;
#ifdef _USE_32BIT_TIME_T
    _localtime32_s(&tm, &t);
#else
    _localtime64_s(&tm, &t);
#endif

#else
    localtime_r(&tv.tv_sec, &tm);
#endif
    tm.tm_mon++;
    tm.tm_year += 1900;

#ifdef __APPLE__
    snprintf(buf, buf_len, "%4d/%02d/%02d %02d:%02d:%02d %06d",
             tm.tm_year, tm.tm_mon,
             tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec, tv.tv_usec);
#else
    snprintf(buf, buf_len, "%4d/%02d/%02d %02d:%02d:%02d %06ld",
             tm.tm_year, tm.tm_mon,
             tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec, tv.tv_usec);
#endif
}
