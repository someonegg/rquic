/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _XQUIC_TYPEDEF_H_INCLUDED_
#define _XQUIC_TYPEDEF_H_INCLUDED_

#include <stdint.h>
#include <stddef.h>
#include "xqc_errno.h"

#define XQC_EXTERN extern

/* defined UNIX system default */
#ifndef XQC_SYS_WINDOWS
#   define XQC_SYS_UNIX
#endif

#if defined(_WIN32) || defined(WIN32) || defined(XQC_SYS_WIN32)
#  if !defined(XQC_SYS_WIN32)
#  define XQC_SYS_WIN32
#  endif
#endif

#if defined(_WIN64) || defined(WIN64) || defined(XQC_SYS_WIN64)
#  if !defined(XQC_SYS_WIN64)
#  define XQC_SYS_WIN64
#  endif
#endif

#if defined(XQC_SYS_WIN32) || defined(XQC_SYS_WIN64)
#undef XQC_SYS_UNIX
#define XQC_SYS_WINDOWS
#endif

#if defined(__MINGW64__) || defined(__MINGW32__)
#  if !defined(XQC_ON_MINGW)
#  define XQC_ON_MINGW
#  endif
#endif

#if defined(XQC_SYS_WINDOWS) && !defined(XQC_ON_MINGW)
# undef XQC_EXTERN
# define XQC_EXTERN extern

#ifdef XQC_SYS_WIN64
    typedef __int64 ssize_t;
#elif defined(XQC_SYS_WIN32)
    typedef __int32 ssize_t;
#endif
#endif


/* TODO: there may be problems using -o2 under Android platform */
#if defined(__GNUC__) && !defined(ANDROID)
#   define XQC_UNLIKELY(cond) __builtin_expect(!!(cond), 0)
#   define XQC_LIKELY(cond) __builtin_expect(!!(cond), 1)
#else
#   define XQC_UNLIKELY(cond) cond
#   define XQC_LIKELY(cond) cond
#endif

typedef struct xqc_stream_s                 xqc_stream_t;
typedef struct xqc_connection_s             xqc_connection_t;
typedef struct xqc_conn_settings_s          xqc_conn_settings_t;
typedef struct xqc_engine_s                 xqc_engine_t;
typedef struct xqc_log_callbacks_s          xqc_log_callbacks_t;
typedef struct xqc_transport_callbacks_s    xqc_transport_callbacks_t;
typedef struct xqc_random_generator_s       xqc_random_generator_t;
typedef struct xqc_client_connection_s      xqc_client_connection_t;
typedef struct xqc_id_hash_table_s          xqc_id_hash_table_t;
typedef struct xqc_str_hash_table_s         xqc_str_hash_table_t;
typedef struct xqc_priority_queue_s         xqc_pq_t;
typedef struct xqc_wakeup_pq_s              xqc_wakeup_pq_t;
typedef struct xqc_log_s                    xqc_log_t;
typedef struct xqc_send_ctl_s               xqc_send_ctl_t;
typedef struct xqc_send_queue_s             xqc_send_queue_t;
typedef struct xqc_pn_ctl_s                 xqc_pn_ctl_t;
typedef struct xqc_packet_s                 xqc_packet_t;
typedef struct xqc_packet_in_s              xqc_packet_in_t;
typedef struct xqc_packet_out_s             xqc_packet_out_t;
typedef struct xqc_stream_frame_s           xqc_stream_frame_t;
typedef struct xqc_dtable_s                 xqc_dtable_t;
typedef struct xqc_sample_s                 xqc_sample_t;
typedef struct xqc_memory_pool_s            xqc_memory_pool_t;
typedef struct xqc_bbr_info_interface_s     xqc_bbr_info_interface_t;
typedef struct xqc_path_ctx_s               xqc_path_ctx_t;
typedef struct xqc_timer_manager_s          xqc_timer_manager_t;
typedef struct xqc_ping_record_s            xqc_ping_record_t;
typedef struct xqc_conn_qos_stats_s         xqc_conn_qos_stats_t;

typedef uint64_t        xqc_msec_t; /* store millisecond values */
typedef uint64_t        xqc_usec_t; /* store microsecond values */

typedef uint64_t        xqc_packet_number_t;
typedef uint64_t        xqc_stream_id_t;

typedef int32_t         xqc_int_t;
typedef uint32_t        xqc_uint_t;
typedef intptr_t        xqc_flag_t;
typedef uint8_t         xqc_bool_t;

/* values of xqc_bool_t */
#define XQC_TRUE        1
#define XQC_FALSE       0

/** restrictions of cid length */
#define XQC_MAX_CID_LEN 20
#define XQC_MIN_CID_LEN 4

/** restrictions of key length in lb cid encryption */
#define XQC_LB_CID_KEY_LEN 16

/** length of stateless reset token */
#define XQC_STATELESS_RESET_TOKENLEN    16

/**
 * @brief cid structure for xquic connection identification
 */
typedef struct xqc_cid_s {
    uint8_t             cid_len;
    uint8_t             cid_buf[XQC_MAX_CID_LEN];
    uint64_t            cid_seq_num;
    uint8_t             sr_token[XQC_STATELESS_RESET_TOKENLEN];
    uint64_t            path_id; /**< preallocate for multi-path */
} xqc_cid_t;

typedef enum xqc_log_level_s {
    XQC_LOG_REPORT  = 0,
    XQC_LOG_FATAL   = 1,
    XQC_LOG_ERROR   = 2,
    XQC_LOG_WARN    = 3,
    XQC_LOG_STATS   = 4,
    XQC_LOG_INFO    = 5,
    XQC_LOG_DEBUG   = 6,
} xqc_log_level_t;

/**
 * @brief qlog Importance level definition
 */
typedef enum qlog_event_importance_s {
    EVENT_IMPORTANCE_SELECTED   = 0,   /**< qlog will be emitted selectly */
    EVENT_IMPORTANCE_CORE       = 1,
    EVENT_IMPORTANCE_BASE       = 2,
    EVENT_IMPORTANCE_EXTRA      = 3,
    EVENT_IMPORTANCE_REMOVED    = 4,   /**< Currently, some events have been removed in the latest qlog draft. But old qvis need them! */
} qlog_event_importance_t;

#define XQC_BBR_RTTVAR_COMPENSATION_ENABLED 0
typedef enum {
    XQC_BBR_FLAG_NONE = 0x00,
#if XQC_BBR_RTTVAR_COMPENSATION_ENABLED
    XQC_BBR_FLAG_RTTVAR_COMPENSATION = 0x01,
#endif
} xqc_bbr_optimization_flag_t;

#define XQC_BBR2_PLUS_ENABLED 0
typedef enum {
    XQC_BBR2_FLAG_NONE = 0x00,
#if XQC_BBR2_PLUS_ENABLED
    XQC_BBR2_FLAG_RTTVAR_COMPENSATION = 0x01,
    XQC_BBR2_FLAG_FAST_CONVERGENCE = 0x2,
#endif
} xqc_bbr2_optimization_flag_t;

#define XQC_EXPORT_PUBLIC_API   __attribute__((visibility("default")))

#ifdef XQC_SYS_WINDOWS
struct iovec {
    void   *iov_base;   /* [XSI] Base address of I/O memory region */
    size_t  iov_len;    /* [XSI] Size of region iov_base points to */
};

#if !(defined __MINGW32__) && !(defined __MINGW64__)
#undef XQC_EXPORT_PUBLIC_API
#define XQC_EXPORT_PUBLIC_API   _declspec(dllexport) 
#endif

#endif



typedef enum {
    XQC_CONN_TYPE_CLIENT    = 0,
    XQC_CONN_TYPE_SERVER    = 1,
} xqc_conn_type_t;


typedef enum {
    XQC_STREAM_BIDI = 0,
    XQC_STREAM_UNI  = 1
} xqc_stream_direction_t;

/* max alpn buffer length */
#define XQC_MAX_ALPN_BUF_LEN    256

#define XQC_MAX_COMMON_BUF_LEN  64

typedef enum xqc_conn_settings_type_e {
    XQC_CONN_SETTINGS_DEFAULT       = 0,
    XQC_CONN_SETTINGS_LOW_DELAY     = 1,
} xqc_conn_settings_type_t;

typedef struct xqc_stream_settings_s {
    uint64_t recv_rate_bytes_per_sec;
} xqc_stream_settings_t;

#define XQC_STREAM_INFO_LEN 128

/**
 * @brief request statistics structure
 */
typedef struct xqc_request_stats_s {
    size_t      send_body_size;
    size_t      recv_body_size;

    /** QUIC layer error code, 0 for no error */
    int         stream_err;

    char        stream_info[XQC_STREAM_INFO_LEN];
    char        extern_stream_info[XQC_STREAM_INFO_LEN];
} xqc_request_stats_t;

#endif /*_XQUIC_TYPEDEF_H_INCLUDED_*/
