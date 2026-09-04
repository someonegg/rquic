/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQUIC_TYPEDEF_H_INCLUDED_
#define _RQUIC_TYPEDEF_H_INCLUDED_

#include <stdint.h>
#include <stddef.h>
#include "rqc_errno.h"

#define RQC_EXTERN extern

/* defined UNIX system default */
#ifndef RQC_SYS_WINDOWS
#   define RQC_SYS_UNIX
#endif

#if defined(_WIN32) || defined(WIN32) || defined(RQC_SYS_WIN32)
#  if !defined(RQC_SYS_WIN32)
#  define RQC_SYS_WIN32
#  endif
#endif

#if defined(_WIN64) || defined(WIN64) || defined(RQC_SYS_WIN64)
#  if !defined(RQC_SYS_WIN64)
#  define RQC_SYS_WIN64
#  endif
#endif

#if defined(RQC_SYS_WIN32) || defined(RQC_SYS_WIN64)
#undef RQC_SYS_UNIX
#define RQC_SYS_WINDOWS
#endif

#if defined(__MINGW64__) || defined(__MINGW32__)
#  if !defined(RQC_ON_MINGW)
#  define RQC_ON_MINGW
#  endif
#endif

#if defined(RQC_SYS_WINDOWS) && !defined(RQC_ON_MINGW)
# undef RQC_EXTERN
# define RQC_EXTERN extern

#ifdef RQC_SYS_WIN64
    typedef __int64 ssize_t;
#elif defined(RQC_SYS_WIN32)
    typedef __int32 ssize_t;
#endif
#endif

/* TODO: there may be problems using -o2 under Android platform */
#if defined(__GNUC__) && !defined(ANDROID)
#   define RQC_UNLIKELY(cond) __builtin_expect(!!(cond), 0)
#   define RQC_LIKELY(cond) __builtin_expect(!!(cond), 1)
#else
#   define RQC_UNLIKELY(cond) cond
#   define RQC_LIKELY(cond) cond
#endif

typedef struct rqc_stream_s                 rqc_stream_t;
typedef struct rqc_connection_s             rqc_connection_t;
typedef struct rqc_conn_settings_s          rqc_conn_settings_t;
typedef struct rqc_engine_s                 rqc_engine_t;
typedef struct rqc_log_callbacks_s          rqc_log_callbacks_t;
typedef struct rqc_transport_callbacks_s    rqc_transport_callbacks_t;
typedef struct rqc_random_generator_s       rqc_random_generator_t;
typedef struct rqc_client_connection_s      rqc_client_connection_t;
typedef struct rqc_id_hash_table_s          rqc_id_hash_table_t;
typedef struct rqc_str_hash_table_s         rqc_str_hash_table_t;
typedef struct rqc_priority_queue_s         rqc_pq_t;
typedef struct rqc_wakeup_pq_s              rqc_wakeup_pq_t;
typedef struct rqc_log_s                    rqc_log_t;
typedef struct rqc_send_ctl_s               rqc_send_ctl_t;
typedef struct rqc_send_queue_s             rqc_send_queue_t;
typedef struct rqc_pn_ctl_s                 rqc_pn_ctl_t;
typedef struct rqc_packet_s                 rqc_packet_t;
typedef struct rqc_packet_in_s              rqc_packet_in_t;
typedef struct rqc_packet_out_s             rqc_packet_out_t;
typedef struct rqc_stream_frame_s           rqc_stream_frame_t;
typedef struct rqc_dtable_s                 rqc_dtable_t;
typedef struct rqc_sample_s                 rqc_sample_t;
typedef struct rqc_memory_pool_s            rqc_memory_pool_t;
typedef struct rqc_bbr_info_interface_s     rqc_bbr_info_interface_t;
typedef struct rqc_path_ctx_s               rqc_path_ctx_t;
typedef struct rqc_timer_manager_s          rqc_timer_manager_t;
typedef struct rqc_ping_record_s            rqc_ping_record_t;
typedef struct rqc_conn_qos_stats_s         rqc_conn_qos_stats_t;

typedef uint64_t        rqc_msec_t; /* store millisecond values */
typedef uint64_t        rqc_usec_t; /* store microsecond values */

typedef uint64_t        rqc_packet_number_t;
typedef uint64_t        rqc_stream_id_t;

typedef int32_t         rqc_int_t;
typedef uint32_t        rqc_uint_t;
typedef intptr_t        rqc_flag_t;
typedef uint8_t         rqc_bool_t;

/* values of rqc_bool_t */
#define RQC_TRUE        1
#define RQC_FALSE       0

/** restrictions of cid length */
#define RQC_MAX_CID_LEN 20
#define RQC_MIN_CID_LEN 4

/** restrictions of key length in lb cid encryption */
#define RQC_LB_CID_KEY_LEN 16

/**
 * @brief cid structure for rquic connection identification
 */
typedef struct rqc_cid_s {
    uint8_t             cid_len;
    uint8_t             cid_buf[RQC_MAX_CID_LEN];
    uint64_t            cid_seq_num;
    uint64_t            path_id; /**< preallocate for multi-path */
} rqc_cid_t;

typedef enum rqc_log_level_s {
    RQC_LOG_REPORT  = 0,
    RQC_LOG_FATAL   = 1,
    RQC_LOG_ERROR   = 2,
    RQC_LOG_WARN    = 3,
    RQC_LOG_STATS   = 4,
    RQC_LOG_INFO    = 5,
    RQC_LOG_DEBUG   = 6,
} rqc_log_level_t;

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

#define RQC_BBR_RTTVAR_COMPENSATION_ENABLED 0
typedef enum {
    RQC_BBR_FLAG_NONE = 0x00,
#if RQC_BBR_RTTVAR_COMPENSATION_ENABLED
    RQC_BBR_FLAG_RTTVAR_COMPENSATION = 0x01,
#endif
} rqc_bbr_optimization_flag_t;

#define RQC_BBR2_PLUS_ENABLED 0
typedef enum {
    RQC_BBR2_FLAG_NONE = 0x00,
#if RQC_BBR2_PLUS_ENABLED
    RQC_BBR2_FLAG_RTTVAR_COMPENSATION = 0x01,
    RQC_BBR2_FLAG_FAST_CONVERGENCE = 0x2,
#endif
} rqc_bbr2_optimization_flag_t;

#define RQC_EXPORT_PUBLIC_API   __attribute__((visibility("default")))

#ifdef RQC_SYS_WINDOWS
struct iovec {
    void   *iov_base;   /* [XSI] Base address of I/O memory region */
    size_t  iov_len;    /* [XSI] Size of region iov_base points to */
};

#if !(defined __MINGW32__) && !(defined __MINGW64__)
#undef RQC_EXPORT_PUBLIC_API
#define RQC_EXPORT_PUBLIC_API   _declspec(dllexport)
#endif

#endif

typedef enum {
    RQC_CONN_TYPE_CLIENT    = 0,
    RQC_CONN_TYPE_SERVER    = 1,
} rqc_conn_type_t;

typedef enum {
    RQC_STREAM_BIDI = 0,
    RQC_STREAM_UNI  = 1
} rqc_stream_direction_t;

/** A data block accepted by rqc_stream_sendv_atomic(). */
typedef struct rquic_iovec_s {
    const uint8_t *data;
    unsigned       len;
} rquic_iovec_t;

/* max alpn buffer length */
#define RQC_MAX_ALPN_BUF_LEN    256
/* max handshake protocol extension payload length */
#define RQC_MAX_PROTO_EXT_LEN   512

#define RQC_MAX_COMMON_BUF_LEN  64

typedef struct rqc_proto_ext_s {
    uint8_t data[RQC_MAX_PROTO_EXT_LEN];
    size_t  len;
} rqc_proto_ext_t;

typedef enum rqc_conn_settings_type_e {
    RQC_CONN_SETTINGS_DEFAULT       = 0,
    RQC_CONN_SETTINGS_LOW_DELAY     = 1,
} rqc_conn_settings_type_t;

typedef enum {
    RQC_STREAM_PRI_DEFAULT  = 0,
    RQC_STREAM_PRI_HIGH     = 1,
    RQC_STREAM_PRI_NORMAL   = 2,
} rqc_stream_priority_t;

typedef struct rqc_stream_settings_s {
    rqc_stream_priority_t stream_priority;
    uint64_t recv_rate_bytes_per_sec;
} rqc_stream_settings_t;

typedef enum {
    RQC_CLI_BID = 0,
    RQC_SVR_BID = 1,
    RQC_CLI_UNI = 2,
    RQC_SVR_UNI = 3,
} rqc_stream_type_t;

typedef enum {
    RQC_SEND_STREAM_ST_READY        = 0,
    RQC_SEND_STREAM_ST_SEND         = 1,
    RQC_SEND_STREAM_ST_DATA_SENT    = 2,
    RQC_SEND_STREAM_ST_DATA_RECVD   = 3,
    RQC_SEND_STREAM_ST_RESET_SENT   = 4,
    RQC_SEND_STREAM_ST_RESET_RECVD  = 5,
} rqc_send_stream_state_t;

typedef enum {
    RQC_RECV_STREAM_ST_RECV         = 0,
    RQC_RECV_STREAM_ST_SIZE_KNOWN   = 1,
    RQC_RECV_STREAM_ST_DATA_RECVD   = 2,
    RQC_RECV_STREAM_ST_DATA_READ    = 3,
    RQC_RECV_STREAM_ST_RESET_RECVD  = 4,
    RQC_RECV_STREAM_ST_RESET_READ   = 5,
} rqc_recv_stream_state_t;

#endif /*_RQUIC_TYPEDEF_H_INCLUDED_*/
