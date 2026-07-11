#include "demo_common.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#if defined(RQC_SYS_WINDOWS)
#include <winsock2.h>
#endif

static void
demo_set_path_error(char *errbuf, size_t errbuf_size, const char *msg)
{
    if (errbuf != NULL && errbuf_size > 0) {
        (void)snprintf(errbuf, errbuf_size, "%s", msg);
    }
}

static int
demo_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static int
demo_validate_path_segment(const char *path, size_t start, size_t end,
    char *errbuf, size_t errbuf_size)
{
    char segment[DEMO_PACKET_BUF_SIZE];
    size_t out = 0;

    for (size_t i = start; i < end; i++) {
        unsigned char ch = (unsigned char)path[i];

        if (ch == '%') {
            int hi;
            int lo;
            if (i + 2 >= end) {
                demo_set_path_error(errbuf, errbuf_size,
                    "resource path contains an invalid percent escape");
                return -1;
            }
            hi = demo_hex_value(path[i + 1]);
            lo = demo_hex_value(path[i + 2]);
            if (hi < 0 || lo < 0) {
                demo_set_path_error(errbuf, errbuf_size,
                    "resource path contains an invalid percent escape");
                return -1;
            }
            ch = (unsigned char)((hi << 4) | lo);
            i += 2;
        }

        if (ch == '/' || ch == '\\' || ch == ':' || ch == '\0') {
            demo_set_path_error(errbuf, errbuf_size,
                "resource path contains an unsafe escaped character");
            return -1;
        }
        if (out + 1 >= sizeof(segment)) {
            demo_set_path_error(errbuf, errbuf_size,
                "resource path segment is too long");
            return -1;
        }
        segment[out++] = (char)ch;
    }

    segment[out] = '\0';
    if (strcmp(segment, ".") == 0 || strcmp(segment, "..") == 0) {
        demo_set_path_error(errbuf, errbuf_size,
            "resource path must not contain . or .. segments");
        return -1;
    }

    return 0;
}

int
demo_validate_resource_path(const char *path, char *errbuf, size_t errbuf_size)
{
    size_t start = 1;
    size_t len;

    if (path == NULL || path[0] == '\0') {
        demo_set_path_error(errbuf, errbuf_size, "resource path is empty");
        return -1;
    }

    if (path[0] != '/') {
        demo_set_path_error(errbuf, errbuf_size,
            "resource path must start with /");
        return -1;
    }

    len = strlen(path);
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '\\' || path[i] == ':') {
            demo_set_path_error(errbuf, errbuf_size,
                "resource path must not contain backslash or colon");
            return -1;
        }
        if ((unsigned char)path[i] < 0x20 || path[i] == 0x7f) {
            demo_set_path_error(errbuf, errbuf_size,
                "resource path must not contain control characters");
            return -1;
        }
    }

    for (size_t i = 1; i <= len; i++) {
        if (path[i] == '/' || path[i] == '\0') {
            if (demo_validate_path_segment(path, start, i, errbuf, errbuf_size) != 0) {
                return -1;
            }
            start = i + 1;
        }
    }

    return 0;
}

const char *
demo_log_level_name(rqc_log_level_t level)
{
    switch (level) {
    case RQC_LOG_REPORT:
        return "report";
    case RQC_LOG_FATAL:
        return "fatal";
    case RQC_LOG_ERROR:
        return "error";
    case RQC_LOG_WARN:
        return "warn";
    case RQC_LOG_STATS:
        return "stats";
    case RQC_LOG_INFO:
        return "info";
    case RQC_LOG_DEBUG:
        return "debug";
    default:
        return "unknown";
    }
}

int
demo_parse_log_level(const char *value, rqc_log_level_t *level)
{
    char *end = NULL;
    long numeric;

    if (value == NULL || level == NULL) {
        return -1;
    }

    if (strcmp(value, "report") == 0) {
        *level = RQC_LOG_REPORT;
        return 0;
    }
    if (strcmp(value, "fatal") == 0) {
        *level = RQC_LOG_FATAL;
        return 0;
    }
    if (strcmp(value, "error") == 0) {
        *level = RQC_LOG_ERROR;
        return 0;
    }
    if (strcmp(value, "warn") == 0 || strcmp(value, "warning") == 0) {
        *level = RQC_LOG_WARN;
        return 0;
    }
    if (strcmp(value, "stats") == 0) {
        *level = RQC_LOG_STATS;
        return 0;
    }
    if (strcmp(value, "info") == 0) {
        *level = RQC_LOG_INFO;
        return 0;
    }
    if (strcmp(value, "debug") == 0) {
        *level = RQC_LOG_DEBUG;
        return 0;
    }

    errno = 0;
    numeric = strtol(value, &end, 10);
    if (errno == 0 && end != value && *end == '\0'
        && numeric >= RQC_LOG_REPORT && numeric <= RQC_LOG_DEBUG)
    {
        *level = (rqc_log_level_t)numeric;
        return 0;
    }

    return -1;
}

int
demo_parse_port(const char *value, unsigned short *port)
{
    char *end = NULL;
    long parsed;

    if (value == NULL || port == NULL) {
        return -1;
    }

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed <= 0 || parsed > 65535) {
        return -1;
    }

    *port = (unsigned short)parsed;
    return 0;
}

int
demo_get_sys_errno(void)
{
#if defined(RQC_SYS_WINDOWS)
    return WSAGetLastError();
#else
    return errno;
#endif
}

void
demo_set_sys_errno(int err)
{
#if defined(RQC_SYS_WINDOWS)
    WSASetLastError(err);
#else
    errno = err;
#endif
}

const char *
demo_strerror(int err)
{
#if defined(RQC_SYS_WINDOWS)
    static char buf[64];
    (void)snprintf(buf, sizeof(buf), "winsock error %d", err);
    return buf;
#else
    return strerror(err);
#endif
}

rqc_usec_t
demo_now(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (rqc_usec_t)tv.tv_sec * 1000000 + (rqc_usec_t)tv.tv_usec;
}

static void
demo_log_write_stderr(rqc_log_level_t lvl, const void *buf, size_t size,
    void *engine_user_data)
{
    (void)engine_user_data;
    (void)fprintf(stderr, "[rquic:%s] ", demo_log_level_name(lvl));
    (void)fwrite(buf, 1, size, stderr);
    (void)fputc('\n', stderr);
}

static void
demo_qlog_write_stderr(qlog_event_importance_t imp, const void *buf, size_t size,
    void *engine_user_data)
{
    (void)engine_user_data;
    (void)fprintf(stderr, "[rquic:qlog:%d] ", (int)imp);
    (void)fwrite(buf, 1, size, stderr);
    (void)fputc('\n', stderr);
}

const rqc_log_callbacks_t demo_log_callbacks = {
    .rqc_log_write_err = demo_log_write_stderr,
    .rqc_log_write_stat = demo_log_write_stderr,
    .rqc_qlog_event_write = demo_qlog_write_stderr,
};
