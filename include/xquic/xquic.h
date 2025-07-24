/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _XQUIC_H_INCLUDED_
#define _XQUIC_H_INCLUDED_

/**
 * Public API for using libxquic
 */
#include "xqc_configure.h"
#include "xquic_typedef.h"

#if defined(XQC_SYS_WINDOWS) && !defined(XQC_ON_MINGW)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#endif

#if defined(__SSE2__)
// SSE2
#include <emmintrin.h>
#define XQC_ON_x86
#elif defined(__ARM_NEON__) || defined(__ARM_NEON)
// NEON
#include <arm_neon.h>
#define XQC_ON_ARM
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief engine type definition
 */
typedef enum {
    XQC_ENGINE_SERVER   = 0,
    XQC_ENGINE_CLIENT   = 1
} xqc_engine_type_t;

/**
 * @brief supported versions for IETF drafts
 */
typedef enum xqc_proto_version_s {
    /** placeholder */
    XQC_IDRAFT_INIT_VER         = 0,

    /** former version of QUIC RFC 9000 */
    XQC_VERSION_V1              = 1,

    /** IETF Draft-29 */
    XQC_IDRAFT_VER_29           = 2,

    /** Special version for version negotiation. */
    XQC_IDRAFT_VER_NEGOTIATION  = 3,

    /** max value of proto value. */
    XQC_VERSION_MAX             = 4
} xqc_proto_version_t;

#define XQC_SUPPORT_VERSION_MAX         64

/**
 * the max message count of iovec in sendmmsg
 */
#define XQC_MAX_SEND_MSG_ONCE           32

#define XQC_INITIAL_PATH_ID             0

/**
 * @brief get timestamp callback function. this might be useful on different platforms
 * @return timestamp in microsecond
 */
typedef xqc_usec_t (*xqc_timestamp_pt)(void);

/**
 * @brief event timer callback function. MUST be set for both client and server
 * xquic don't have implementation of timer, but will tell the interval of timer by this function.
 * applications shall implement the timer, and invoke xqc_engine_main_logic after timer expires.
 *
 * @param wake_after interval of timer, with micro-second.
 * @param engine_user_data user_data of engine
 */
typedef void (*xqc_set_event_timer_pt)(xqc_usec_t wake_after, void *engine_user_data);

/**
 * @brief cid generate callback.
 *
 * @param ori_cid the original dcid sent by client.
 * @param cid_buf  buffer for cid generated
 * @param cid_buflen len for cid_buf
 * @param engine_user_data  user data of engine from `xqc_engine_create`
 * @return negative for failed, non-negative (including 0) for the length of bytes written. if the
 * count of written bytes is less than cid_buflen, xquic will fill rest of cid_buf with random bytes
 */
typedef ssize_t (*xqc_cid_generate_pt)(const xqc_cid_t *ori_cid, uint8_t *cid_buf,
    size_t cid_buflen, void *engine_user_data);

/**
 * @brief log callback functions
 */
typedef struct xqc_log_callbacks_s {
    /**
     * trace log callback function
     *
     * trace log including XQC_LOG_FATAL, XQC_LOG_ERROR, XQC_LOG_WARN, XQC_LOG_STATS, XQC_LOG_INFO,
     * XQC_LOG_DEBUG, xquic will output logs with the level higher or equal to the level configured
     * in xqc_log_init. Besides, when qlog enable and EVENT_IMPORTANCE_SELECTED importance is set, some
     * event log will output log by xqc_log_write_err callback.
     */
    void (*xqc_log_write_err)(xqc_log_level_t lvl, const void *buf, size_t size, void *engine_user_data);

    /**
     * statistic log callback function
     *
     * this function will be triggered when write XQC_LOG_REPORT or XQC_LOG_STATS level logs.
     * mainly when connection close, stream close.
     */
    void (*xqc_log_write_stat)(xqc_log_level_t lvl, const void *buf, size_t size, void *engine_user_data);

    /**
     * qlog event callback function
     *
     * qlog event importance including EVENT_IMPORTANCE_SELECTED, EVENT_IMPORTANCE_CORE, EVENT_IMPORTANCE_BASE,
     * EVENT_IMPORTANCE_EXTRA and EVENT_IMPORTANCE_REMOVED.
     * EVENT_IMPORTANCE_CORE, EVENT_IMPORTANCE_BASE and EVENT_IMPORTANCE_EXTRA follow the defination of qlog draft.
     * EVENT_IMPORTANCE_SELECTED works by xqc_log_write_err
     * EVENT_IMPORTANCE_REMOVED exits, because the last qlog draft remove some qlog event, but the current qvis tool
     * still need them.
     */
    void (*xqc_qlog_event_write)(qlog_event_importance_t imp, const void *buf, size_t size, void *engine_user_data);

} xqc_log_callbacks_t;

/**
 * @brief connection accept callback.
 *
 * this function is invoked when incoming a new QUIC connection. return 0 means accept this new
 * connection. return negative values if application layer will not accept the new connection
 * due to busy or some reason else
 *
 * @param user_data the user_data parameter of xqc_engine_packet_process
 * @return negative for refuse connection. 0 for accept
 */
typedef int (*xqc_server_accept_pt)(xqc_engine_t *engine, xqc_connection_t *conn,
    const xqc_cid_t *cid, void *user_data);

/**
 * @brief connection refused callback. corresponding to xqc_server_accept_pt callback function.
 * this function will be invoked when a QUIC connection is refused by xquic due to security
 * considerations, applications SHALL link the connection's lifetime between itself and xquic, and
 * free the context if it was created during xqc_server_accept_pt.
 *
 * @param user_data the user_data parameter of connection
 */
typedef void (*xqc_server_refuse_pt)(xqc_engine_t *engine, xqc_connection_t *conn,
    const xqc_cid_t *cid, void *user_data);

/**
 * @brief connection closing notify callback function.
 *
 * This function will be triggered when a connection is not available and will not send/receive data any more. this
 * callback is helpful to avoid attempts to send data on a closing connection. \n
 * NOTICE: this callback function will be triggered at the beginning of
 * connection close, while the conn_close_notify will be triggered at the end of
 * connection close.
 *
 * @param conn pointer of connection
 * @param cid connection id
 * @param err_code the reason of connection close
 * @param conn_user_data the user_data which will be used in callback functions
 * between xquic transport connection and application
 */
typedef xqc_int_t (*xqc_conn_closing_notify_pt)(xqc_connection_t *conn,
    const xqc_cid_t *cid, xqc_int_t err_code, void *conn_user_data);

/**
 * @brief general callback function definition for connection create and close
 *
 * @param conn_user_data the user_data which will be used in callback functions
 * between xquic transport connection and application
 * @param conn_proto_data the user_data which will be used in callback functions
 * between xquic transport connection and application-layer-protocol
 */
typedef int (*xqc_conn_notify_pt)(xqc_connection_t *conn, const xqc_cid_t *cid,
    void *conn_user_data, void *conn_proto_data);

/**
 * @brief handshake finished callback function
 *
 * this will be trigger when the QUIC connection handshake is completed
 */
typedef void (*xqc_handshake_finished_pt)(xqc_connection_t *conn, void *conn_user_data,
    void *conn_proto_data);

/**
 * @brief PING acked callback function.
 *
 * if application send a PING frame with xqc_conn_send_ping function, this callback function will be
 * triggered when this PING frame is acked by peer. noticing that PING frame do not need repair, it
 * might not be triggered if PING frame is lost or ACK frame is lost.
 * xquic might send PING frames  will not trigger this callback
 */
typedef void (*xqc_conn_ping_ack_notify_pt)(xqc_connection_t *conn, const xqc_cid_t *cid,
    void *ping_user_data, void *conn_user_data, void *conn_proto_data);

/**
 * @brief server peer addr changed notify
 *
 * this function will be trigger after receive peer's changed addr.
 *
 * @param conn connection handler
 * @param conn_user_data connection level user_data
 */
typedef void (*xqc_conn_peer_addr_changed_nofity_pt)(xqc_connection_t *conn, void *conn_user_data);

/**
 * @brief return value of xqc_socket_write_pt and xqc_send_mmsg_pt callback function
 */
#define XQC_SOCKET_ERROR                -1
#define XQC_SOCKET_EAGAIN               -2

/**
 * @brief writing data callback function
 *
 * @param buf  packet buffer
 * @param size  packet size
 * @param peer_addr  peer address
 * @param peer_addrlen  peer address length
 * @param conn_user_data user_data of connection
 * @return bytes of data which is successfully sent:
 * XQC_SOCKET_ERROR for error, xquic will destroy the connection
 * XQC_SOCKET_EAGAIN for EAGAIN, application could continue sending data with xqc_conn_continue_send
 * function when socket write event is ready
 */
typedef ssize_t (*xqc_socket_write_pt)(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *conn_user_data);

/**
 * @brief sendmmsg callback function. the implementation of this shall send data with sendmmsg
 *
 * @param msg_iov message vector
 * @param vlen vector of messages
 * @param peer_addr address of peer
 * @param peer_addrlen  length of peer_addr param
 * @param conn_user_data user_data of connection
 * @return count of messages that are successfully sent:
 * XQC_SOCKET_ERROR for error, xquic will destroy the connection
 * XQC_SOCKET_EAGAIN for EAGAIN, application could continue sending data with xqc_conn_continue_send
 * function when socket write event is ready
 */
typedef ssize_t (*xqc_send_mmsg_pt)(const struct iovec *msg_iov, unsigned int vlen,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *conn_user_data);

/**
 * @brief set data callback mode for a transport connection. this mode differs
 * from write_socket, which has a different user_data, once this callback
 * function is set, write_socket will be not functional until it is unset.
 *
 * @param buf packet buffer
 * @param size packet size
 * @param peer_addr peer address
 * @param peer_addrlen peer address length
 * @param cb_user_data user_data of xqc_conn_pkt_filter_callback_pt
 */
typedef ssize_t (*xqc_conn_pkt_filter_callback_pt)(const unsigned char *buf,
    size_t size, const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *cb_user_data);

/**
 * @brief general callback function definition for stream create, close, read and write.
 *
 * @param stream QUIC stream handler
 * @param strm_user_data stream level user_data, which was the parameter of xqc_stream_create set by
 * client, or the parameter of xqc_stream_set_user_data set by server
 * @return 0 for success, -1 for failure
 */
typedef xqc_int_t (*xqc_stream_notify_pt)(xqc_stream_t *stream,
    void *strm_user_data);

/**
 * @brief stream closing callback function, this will be triggered when some
 * error on a stream happens.
 *
 * @param stream QUIC stream handler
 * @param err_code error code
 * @param strm_user_data stream level user_data, which was the parameter of xqc_stream_create set by
 * client, or the parameter of xqc_stream_set_user_data set by server
 * @return 0 for success, -1 for failure
 */
typedef void (*xqc_stream_closing_notify_pt)(xqc_stream_t *stream,
    xqc_int_t err_code, void *strm_user_data);

/**
 * @brief tranport callback functions are more related to attributes of QUIC [Transport] but not ALPN.
 *
 * These callback functions are events of QUIC Transport layer, and need to
 * interact with application-layer, which have less thing to do with ALPN layer.
 *
 * These callback functions shall directly call back to application layer, with user_data from
 * struct xqc_connection_t. unless Application-Layer-Protocol take over them.
 *
 * Generally, xquic defines callbacks as below:
 * 1. Callbacks between Transport and Application:
 * QUIC events that are common between different Application Protocols,
 * and is much more convenient to interact with Application and Application Protocol.
 *
 * 2. Callbacks between Application Protocol and Application:
 * Application-Protocol events will interact with Application Layer. these callback functions are
 * defined by Application Protocol Layers.
 *
 * 3. Callbacks between Transport and Application Protocol:
 * QUIC events that might be more essential to Application-Layer-Protocols, especially stream data
 *
 * +------------------------------------------------------------------------------+
 * |                             Application                                      |
 * |                                 +-- Application Protocol defined callbacks --+
 * |                                 |             Application Protocol           |
 * +-------- transport callbacks ----+--------- app protocol callbacks -----------+
 * |                              Transport                                       |
 * +------------------------------------------------------------------------------+
 */
typedef struct xqc_transport_callbacks_s {
    /**
     * accept new connection callback. REQUIRED only for server \n
     * NOTICE: this is the headmost callback trigger by xquic, the user_data of server_accept is
     * what was passed into xqc_engine_packet_process
     */
    xqc_server_accept_pt            server_accept;

    /**
     * connection refused by xquic. REQUIRED only for server
     */
    xqc_server_refuse_pt            server_refuse;

    /**
     * write socket callback, ALTERNATIVE with write_mmsg
     */
    xqc_socket_write_pt             write_socket;

    /**
     * write socket with send_mmsg callback, ALTERNATIVE with write_socket
     */
    xqc_send_mmsg_pt                write_mmsg;

    /**
     * connection closing callback function. OPTIONAL for both client and server
     */
    xqc_conn_closing_notify_pt      conn_closing;

    /**
     * QUIC connection peer addr changed callback, REQUIRED for server.
     */
    xqc_conn_peer_addr_changed_nofity_pt    conn_peer_addr_changed_notify;

} xqc_transport_callbacks_t;

/**
 * @brief QUIC connection callback functions for Application-layer-Protocol.
 */
typedef struct xqc_conn_callbacks_s {

    /**
     * connection create notify callback. REQUIRED for server, OPTIONAL for client.
     *
     * this function will be invoked after connection is created, user can create application layer
     * context in this callback function
     *
     * return 0 for success, -1 for failure, e.g. malloc error, on which xquic will close connection
     */
    xqc_conn_notify_pt                  conn_create_notify;

    /**
     * connection close notify. REQUIRED for both client and server
     *
     * this function will be invoked after QUIC connection is closed. user can free application
     * level context created in conn_create_notify callback function
     */
    xqc_conn_notify_pt                  conn_close_notify;

    /**
     * handshake complete callback. OPTIONAL for client and server
     */
    xqc_handshake_finished_pt           conn_handshake_finished;

    /**
     * active PING acked callback. OPTIONAL for both client and server
     */
    xqc_conn_ping_ack_notify_pt         conn_ping_acked;

} xqc_conn_callbacks_t;

/**
 * @brief QUIC layer stream callback functions
 */
typedef struct xqc_stream_callbacks_s {
    /**
     * @brief stream read callback function. REQUIRED for both client and server
     *
     * this will be triggered when QUIC stream data is ready for read. application layer could read
     * data when xqc_stream_recv interface.
     */
    xqc_stream_notify_pt            stream_read_notify;

    /**
     * @brief stream write callback function. REQUIRED for both client and server
     *
     * when sending data with xqc_stream_send, xquic might be blocked or send part of the data. if
     * this callback function is triggered, applications can continue to send the rest data.
     */
    xqc_stream_notify_pt            stream_write_notify;

    /**
     * @brief stream create callback function. REQUIRED for server, OPTIONAL for client.
     *
     * this will be triggered when QUIC stream is created. applications can create its own stream
     * context in this callback function.
     */
    xqc_stream_notify_pt            stream_create_notify;

    /**
     * @brief stream close callback function. REQUIRED for both server and client.
     *
     * this will be triggered when QUIC stream is finally closed. xquic will close stream after
     * sending or receiving RESET_STREAM frame after 3 times of PTO, or when connection is closed.
     * Applications can free the context which was created in stream_create_notify here.
     */
    xqc_stream_notify_pt            stream_close_notify;

    /**
     * @brief stream reset callback function. OPTIONAL for both server and client
     *
     * this function will be triggered when a RESET_STREAM frame is received.
     */
    xqc_stream_closing_notify_pt    stream_closing_notify;

} xqc_stream_callbacks_t;

/**
 * @brief connection and stream callbacks for QUIC level, Application-Layer-Protocol shall implement
 * these callback functions and register ALP with xqc_engine_register_alpn
 */
typedef struct xqc_app_proto_callbacks_s {

    /**
     * @brief QUIC connection callback functions for Application-Layer-Protocol
     */
    xqc_conn_callbacks_t        conn_cbs;

    /**
     * @brief QUIC stream callback functions
     */
    xqc_stream_callbacks_t      stream_cbs;

} xqc_app_proto_callbacks_t;

/**
 * @brief congestion control algorithm parameters
 */
typedef struct xqc_cc_params_s {
    uint32_t    customize_on;
    uint32_t    init_cwnd;
    uint32_t    min_cwnd;
    uint32_t    expect_bw;
    uint32_t    max_expect_bw;
    uint8_t     bbr_enable_lt_bw;
    uint8_t     bbr_ignore_app_limit;
    uint32_t    cc_optimization_flags;
    /** 0 < delta <= delta_max, default 0.05, ->0 = more throughput-oriented */
    double      copa_delta_base;
    /** 0 < delta_max <= 1.0, default 0.5 */
    double      copa_delta_max;
    /**
     * 1.0 <= delta_ai_unit, default 1.0, greater values mean more aggressive
     * when Copa competes with loss-based CCAs.
     */
    double      copa_delta_ai_unit;
} xqc_cc_params_t;

/**
 * @brief congestion control callbacks
 */
typedef struct xqc_congestion_control_callback_s {
    /** Callback on initialization, for memory allocation */
    size_t (*xqc_cong_ctl_size)(void);

    /** Callback on connection initialization, support for passing in congestion algorithm parameters */
    void (*xqc_cong_ctl_init)(void *cong_ctl, xqc_send_ctl_t *ctl_ctx, xqc_cc_params_t cc_params);

    /** Callback when packet loss is detected, reduce congestion window according to algorithm */
    void (*xqc_cong_ctl_on_lost)(void *cong_ctl, xqc_usec_t lost_sent_time);

    /** Callback when packet acked, increase congestion window according to algorithm */
    void (*xqc_cong_ctl_on_ack)(void *cong_ctl, xqc_packet_out_t *po, xqc_usec_t now);

    /** Callback when sending a packet, to determine if the packet can be sent */
    uint64_t (*xqc_cong_ctl_get_cwnd)(void *cong_ctl);

    /** Callback when all packets are detected as lost within 1-RTT, reset the congestion window */
    void (*xqc_cong_ctl_reset_cwnd)(void *cong_ctl);

    /** If the connection is in slow start state */
    int (*xqc_cong_ctl_in_slow_start)(void *cong_ctl);

    /** If the connection is in recovery state. */
    int (*xqc_cong_ctl_in_recovery)(void *cong_ctl);

    /** This function is used by BBR and Cubic*/
    void (*xqc_cong_ctl_restart_from_idle)(void *cong_ctl, uint64_t arg);

    /** For BBR */
    void (*xqc_cong_ctl_on_ack_multiple_pkts)(void *cong_ctl, xqc_sample_t *sampler);

    /** initialize bbr */
    void (*xqc_cong_ctl_init_bbr)(void *cong_ctl, xqc_sample_t *sampler, xqc_cc_params_t cc_params);

    /** get pacing rate */
    uint32_t (*xqc_cong_ctl_get_pacing_rate)(void *cong_ctl);

    /** get estimation of bandwidth */
    uint32_t (*xqc_cong_ctl_get_bandwidth_estimate)(void *cong_ctl);

    xqc_bbr_info_interface_t *xqc_cong_ctl_info_cb;
} xqc_cong_ctrl_callback_t;

#ifdef XQC_ENABLE_BBR2
XQC_EXPORT_PUBLIC_API XQC_EXTERN const xqc_cong_ctrl_callback_t xqc_bbr2_cb;
#endif
XQC_EXPORT_PUBLIC_API XQC_EXTERN const xqc_cong_ctrl_callback_t xqc_bbr_cb;

/**
 * @struct xqc_config_t
 * QUIC config parameters
 */
typedef struct xqc_config_s {
    /** log level */
    xqc_log_level_t cfg_log_level;

    /** enable log based on event or not, non-zero for enable, 0 for not */
    xqc_flag_t      cfg_log_event;

    /** qlog event importance */
    qlog_event_importance_t cfg_qlog_importance;

    /** print timestamp in log or not, non-zero for print, 0 for not */
    xqc_flag_t      cfg_log_timestamp;

    /** print level name in log or not, non-zero for print, 0 for not */
    xqc_flag_t      cfg_log_level_name;

    /** connection memory pool size, which will be used for congestion control */
    size_t          conn_pool_size;

    /** bucket size of stream hash table in xqc_connection_t */
    size_t          streams_hash_bucket_size;

    /** bucket size of connection hash table in engine */
    size_t          conns_hash_bucket_size;

    /** capacity of connection priority queue in engine */
    size_t          conns_active_pq_capacity;

    /** capacity of wakeup connection priority queue in engine */
    size_t          conns_wakeup_pq_capacity;

    /** supported quic version list, actually draft-29 and quic-v1 is supported */
    uint32_t        support_version_list[XQC_SUPPORT_VERSION_MAX];

    /** supported quic version count */
    uint32_t        support_version_count;

    /** default connection id length */
    uint8_t         cid_len;

    /**
     * only for server, whether server will negotiate cid with client. non-zero for negotiate and 0
     * for not. when enable, server will not reuse client's original DCID, and generate its own cid.
     *
     * NOTICE: if length of client's original DCID is not equal to cid_len, server will always
     * generate its own cid, despite of the enable of cid negotiation.
     */
    uint8_t         cid_negotiate;

    /**
     * sendmmsg switch. non-zero for enable, 0 for disable.
     * if enabled, xquic will try to use write_mmsg callback function instead of write_socket.
     *
     * NOTICE: if sendmmsg is enabled, xquic will check write_mmsg callback function when creating
     * engine. if write_mmsg is NULL and sendmmsg_on is non-zero, xqc_engine_create will fail
     */
    int             sendmmsg_on;

    /**
     * @brief manually call mainlogic after stream/request send
     *
     */
    uint8_t         manually_triggered_send;

    /** for warning when the number of elements in one bucket exceeds the value of hash_conflict_threshold*/
    uint32_t        hash_conflict_threshold;
} xqc_config_t;

/**
 * @brief engine callback functions.
 */
typedef struct xqc_engine_callback_s {
    /** timer callback for event loop */
    xqc_set_event_timer_pt          set_event_timer;

    /** write log file callback, REQUIRED */
    xqc_log_callbacks_t             log_callbacks;

    /** custom cid generator, OPTIONAL for server */
    xqc_cid_generate_pt             cid_generate_cb;

    /** get realtime timestamp callback function. if not set, xquic will get timestamp with inner
       function xqc_now, which relies on gettimeofday */
    xqc_timestamp_pt                realtime_ts;

    /** get monotonic increasing timestamp callback function. if not set, xquic will get timestamp
       with inner function xqc_now, which relies on gettimeofday */
    xqc_timestamp_pt                monotonic_ts;

} xqc_engine_callback_t;

typedef struct xqc_linger_s {
    /** close connection after all data sent and acked, default: 0 */
    uint32_t                    linger_on;
    /** 3*PTO if linger_timeout is 0 */
    xqc_usec_t                  linger_timeout;
} xqc_linger_t;

/**
 * @brief structures of connection settings
 */
typedef struct xqc_conn_settings_s {
    /** default: 0 */
    int                         pacing_on;
    /** client sends PING to keepalive, default:0 */
    int                         ping_on;
    /** default: xqc_bbr_cb */
    xqc_cong_ctrl_callback_t    cong_ctrl_callback;
    xqc_cc_params_t             cc_params;
    /** socket option SO_SNDBUF, 0 for unlimited */
    uint32_t                    so_sndbuf;
    /**
     * default: XQC_SNDQ_PACKETS_USED_MAX.
     * It should be set to buffer 2xBDP packets at least for performance consideration.
     * The default value is 16000 pkts.
     */
    uint64_t                    sndq_packets_used_max;
    xqc_linger_t                linger;
    /** QUIC protocol version */
    xqc_proto_version_t         proto_version;
    /** initial idle timeout interval, effective before handshake completion */
    xqc_msec_t                  init_idle_time_out;
    /** idle timeout interval, effective after handshake completion */
    xqc_msec_t                  idle_time_out;
    int32_t                     spurious_loss_detect_on;
    size_t                      max_pkt_out_size;

    /** params for performance tuning */
    /** max ack delay: ms */
    uint32_t                    max_ack_delay;
    /** generate an ACK if received ack-eliciting pkts >= ack_frequency */
    uint32_t                    ack_frequency;
    uint8_t                     adaptive_ack_frequency;
    uint64_t                    loss_detection_pkt_thresh;
    double                      pto_backoff_factor;

    /**
     * The limitation on conn recv rate (only applied to stream data) in bytes per second.
     * NOTE: the minimal rate limitation is (63000/RTT) Bps. For instance, if RTT is 60ms,
     * the minimal valid rate limitation is about 1MBps. Any recv_rate_bytes_per_sec less
     * than the minimal valid rate limitation will not be guaranteed.
     * default: 0 (no limitation).
     */
    uint64_t                    recv_rate_bytes_per_sec;

    /**
     * The switch to enable stream-level recv rate throttling. Default: off (0)
     */
    uint8_t                     enable_stream_rate_limit;

    /**
     * initial recv window. Default: 0 (use the internal default value)
     */
    uint32_t                    init_recv_window;

#ifdef XQC_PROTECT_POOL_MEM
    uint8_t                     protect_pool_mem;
#endif

    /**
     * @brief intial_rtt (us). Default: 0 (use the internal default value -- 250000)
     *
     */
    xqc_usec_t                  initial_rtt;
    /**
     * @brief initial pto duration (us). Default: 0 (use the internal default value -- 3xinitial_rtt)
     *
     */
    xqc_usec_t                  initial_pto_duration;

    /**
     * @brief disable batch sending on the connection (default:0, not disable)
     */
    uint8_t                     disable_send_mmsg;

    /**
     * @brief control PTO value
     */
    uint8_t                     control_pto_value;

    uint64_t                    max_udp_payload_size;
} xqc_conn_settings_t;

typedef struct xqc_path_metrics_s {
    uint64_t            path_id;

    uint64_t            path_pkt_recv_count;
    uint64_t            path_pkt_send_count;

    uint64_t            path_send_bytes;
    uint64_t            path_recv_bytes;
    uint64_t            path_recv_effective_bytes;

    uint64_t            path_srtt;
} xqc_path_metrics_t;

/**
 * @brief connection stats
 */
typedef struct xqc_conn_stats_s {
    uint32_t            send_count;
    uint32_t            lost_count;
    uint32_t            tlp_count;
    uint32_t            spurious_loss_count;
    /** smoothed SRTT at present: initial value = 250000 */
    xqc_usec_t          srtt;
    /** minimum RTT until now: initial value = 0xFFFFFFFF */
    xqc_usec_t          min_rtt;
    /** initial value = 0 */
    uint64_t            inflight_bytes;
    uint32_t            recv_count;
    int                 spurious_loss_detect_on;
    int                 conn_err;
    char                ack_info[50];

    int                 total_rebind_count;
    int                 total_rebind_valid;

    xqc_path_metrics_t  path_info;

    char                alpn[XQC_MAX_ALPN_BUF_LEN];

    /** only accounts for stream packets */
    uint64_t            total_app_bytes;

    uint32_t            max_acked_mtu;

    xqc_usec_t          avg_close_time;
} xqc_conn_stats_t;

typedef struct xqc_conn_qos_stats_s {
    /** smoothed SRTT at present: initial value = 250000 */
    xqc_usec_t          srtt;
    /** minimum RTT until now: initial value = 0xFFFFFFFF */
    xqc_usec_t          min_rtt;
    /** initial value = 0 */
    uint64_t            inflight_bytes;
} xqc_conn_qos_stats_t;

/**
 * @brief stream stats
 */
typedef struct xqc_stream_stats_s {
    xqc_stream_type_t   stream_type;
    uint64_t            stream_err;

    uint64_t            send_bytes;
    uint64_t            recv_bytes;
    uint64_t            read_bytes;

    uint32_t            sent_pkt_cnt;
    uint32_t            retrans_pkt_cnt;

    xqc_usec_t          create_time;            /* stream create time */
    xqc_usec_t          close_time;             /* stream close time: fin/reset read */
    xqc_usec_t          first_write_time;       /* app send data */
    xqc_usec_t          first_snd_time;         /* socket send data */
    xqc_usec_t          first_rcv_time;         /* recv the first udp packet */
    xqc_usec_t          local_fin_write_time;   /* app send fin */
    xqc_usec_t          local_fin_snd_time;     /* socket send fin */
    xqc_usec_t          peer_fin_rcv_time;      /* quic stack rcv fin */
    xqc_usec_t          peer_fin_read_time;     /* app read fin */
    xqc_usec_t          all_data_acked_time;    /* all data sent & acked */

    xqc_usec_t          app_reset_time;         /* app snd reset */
    xqc_usec_t          local_reset_time;       /* socket snd reset */
    xqc_usec_t          peer_reset_time;        /* quic stack rcv reset */
} xqc_stream_stats_t;

/*************************************************************
 *  engine layer APIs
 *************************************************************/

/**
 * @brief Create new xquic engine.
 *
 * @param engine_type  XQC_ENGINE_SERVER or XQC_ENGINE_CLIENT
 * @param engine_config config for basic framework, quic, network, etc.
 * @param engine_callback environment callback functions, including timer, socket, log, etc.
 * @param transport_cbs transport callback functions
 * @param conn_callback default connection callback functions
 */
XQC_EXPORT_PUBLIC_API
xqc_engine_t *xqc_engine_create(xqc_engine_type_t engine_type,
    const xqc_config_t *engine_config,
    const xqc_engine_callback_t *engine_callback,
    const xqc_transport_callbacks_t *transport_cbs,
    void *user_data);

/**
 * @brief destroy engine. this is called after all connections are destroyed \n
 * NOTICE: MUST NOT be called in any xquic callback functions, for this function will destroy engine
 * immediately, result in segmentation fault.
 */
XQC_EXPORT_PUBLIC_API
void xqc_engine_destroy(xqc_engine_t *engine);

/**
 * @brief register alpn and connection and stream callbacks. user can implement his own application
 * protocol by registering alpn, and taking quic connection and streams as application connection
 * and request
 *
 * @param engine engine handler
 * @param alpn Application-Layer-Protocol, for example, hq-interop, or self-defined
 * @param alpn_len length of Application-Layer-Protocol string
 * @param ap_cbs connection and stream event callback functions for application-layer-protocol
 * @param alp_ctx the context of the upper layer protocol (e.g. the callback functions and default settings of the upper layer protocol)
 * @return XQC_EXPORT_PUBLIC_API
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_engine_register_alpn(xqc_engine_t *engine, const char *alpn, size_t alpn_len,
    xqc_app_proto_callbacks_t *ap_cbs, void *alp_ctx);

/**
 * @brief unregister an alpn and its quic connection callbacks
 *
 * @param engine engine handler
 * @param alpn Application-Layer-Protocol, for example, hq-interop, or self-defined
 * @param alpn_len length of alpn
 * @return XQC_EXPORT_PUBLIC_API
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_engine_unregister_alpn(xqc_engine_t *engine, const char *alpn, size_t alpn_len);

/**
 * @brief get the context an application layer protocol
 *
 * @param engine engine handler
 * @param alpn Application-Layer-Protocol, for example, hq-interop, or self-defined
 * @param alpn_len length of alpn
 * @return the context
 */
XQC_EXPORT_PUBLIC_API
void* xqc_engine_get_alpn_ctx(xqc_engine_t *engine, const char *alpn, size_t alpn_len);

/**
 * Pass received UDP packet payload into xquic engine.
 * @param recv_time   UDP packet received time in microsecond
 * @param user_data   connection user_data, server is NULL
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_engine_packet_process(xqc_engine_t *engine,
    const unsigned char *packet_in_buf, size_t packet_in_size,
    const struct sockaddr *local_addr, socklen_t local_addrlen,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    xqc_usec_t recv_time, void *user_data);

/**
 * @brief Process all connections, application implements MUST call this function in timer callback
 */
XQC_EXPORT_PUBLIC_API
void xqc_engine_main_logic(xqc_engine_t *engine);

/**
 * @brief get default config of xquic
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_engine_get_default_config(xqc_config_t *config, xqc_engine_type_t engine_type);

/**
 * Modify engine config before engine created. Default config will be used otherwise.
 * Item value 0 means use default value.
 * @return 0 for success, <0 for error. default value is used if config item is illegal
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_engine_set_config(xqc_engine_t *engine, const xqc_config_t *engine_config);

/**
 * @brief Set server's connection settings. it can be called anytime. settings will take effect on
 * new created connections
 */
XQC_EXPORT_PUBLIC_API
void xqc_server_set_conn_settings(xqc_engine_t *engine, const xqc_conn_settings_t *settings);

/**
 * @brief Set the log level of xquic
 *
 * @param log_level engine will print logs which level >= log_level
 */
XQC_EXPORT_PUBLIC_API
void xqc_engine_set_log_level(xqc_engine_t *engine, xqc_log_level_t log_level);

/**
 * @brief enable/disable the log module of xquic
 * @note  This function is not thread-safe.
 *
 * @param enable XQC_TRUE for disable, XQC_FALSE for enable
 */
XQC_EXPORT_PUBLIC_API
void xqc_log_disable(xqc_bool_t disable);

/**
 * user should call after a number of packet processed in xqc_engine_packet_process
 * call after recv a batch packets, may destroy connection when error
 */
XQC_EXPORT_PUBLIC_API
void xqc_engine_finish_recv(xqc_engine_t *engine);

/**
 * @brief only useful for manually triggered send mode
 *
 * @param engine
 * @return XQC_EXPORT_PUBLIC_API
 */
XQC_EXPORT_PUBLIC_API
void xqc_engine_finish_send(xqc_engine_t *engine);

XQC_EXPORT_PUBLIC_API
xqc_connection_t *xqc_engine_get_conn_by_scid(xqc_engine_t *engine,
    const xqc_cid_t *cid);

/*************************************************************
 *  QUIC layer APIs
 *************************************************************/
/**
 * Client connect
 * @param engine return from xqc_engine_create
 * @param conn_settings settings of connection
 * @param server_host server domain
 * @param alpn Application-Layer-Protocol, MUST NOT be NULL
 * @param peer_addr address of peer
 * @param peer_addrlen length of peer_addr
 * @param user_data application data, for connection usage
 * @return user should copy cid to your own memory, in case of cid destroyed in xquic library
 */
XQC_EXPORT_PUBLIC_API
const xqc_cid_t *xqc_connect(xqc_engine_t *engine,
    const xqc_conn_settings_t *conn_settings,
    const char *server_host, const char *alpn,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *user_data);

/**
 * Send CONNECTION_CLOSE to peer, conn_close_notify will callback when connection destroyed
 * @return 0 for success, <0 for error
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_conn_close(xqc_engine_t *engine, const xqc_cid_t *cid);

/**
 * @brief close connection with error code
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_conn_close_with_error(xqc_connection_t *conn, uint64_t err_code);

/**
 * Get errno when conn_close_notify, 0 For no-error
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_conn_get_errno(xqc_connection_t *conn);

/**
 * @brief get latest rtt sample of the initial path
 *
 */
XQC_EXPORT_PUBLIC_API
xqc_usec_t xqc_conn_get_lastest_rtt(xqc_engine_t *engine, const xqc_cid_t *cid);

/**
 * Server should set user_data when conn_create_notify callbacks
 */
XQC_EXPORT_PUBLIC_API
void xqc_conn_set_transport_user_data(xqc_connection_t *conn, void *user_data);

/**
 * @brief set application-layer-protocol user_data to xqc_connection_t. which will be used in
 * xqc_conn_callbacks_t
 */
XQC_EXPORT_PUBLIC_API
void xqc_conn_set_alp_user_data(xqc_connection_t *conn, void *proto_data);

/**
 * Server should get peer addr when conn_create_notify callbacks
 * @param peer_addr_len is a return value
 * @return XQC_OK for success, others for failure
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_conn_get_peer_addr(xqc_connection_t *conn, struct sockaddr *addr, socklen_t addr_cap,
    socklen_t *peer_addr_len);

/**
 * Server should get local addr when conn_create_notify callbacks
 * @param local_addr_len is a return value
 * @return XQC_OK for success, others for failure
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_conn_get_local_addr(xqc_connection_t *conn, struct sockaddr *addr, socklen_t addr_cap,
    socklen_t *local_addr_len);

/**
 * Send PING to peer, if ack received, conn_ping_acked will callback with user_data
 * @return 0 for success, <0 for error
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_conn_send_ping(xqc_engine_t *engine, const xqc_cid_t *cid, void *ping_user_data);

/**
 * @brief set the packet filter callback function, and replace write_socket. \n
 * NOTICE: this function is not conflict with send_mmsg.
 */
XQC_EXPORT_PUBLIC_API
void xqc_conn_set_pkt_filter_callback(xqc_connection_t *conn,
    xqc_conn_pkt_filter_callback_pt pf_cb, void *pf_cb_user_data);

/**
 * @brief unset the packet filter callback function, and restore write_socket
 */
XQC_EXPORT_PUBLIC_API
void xqc_conn_unset_pkt_filter_callback(xqc_connection_t *conn);

/**
 * @brief Create new stream in quic connection.
 * @param user_data  user_data for this stream
 */
XQC_EXPORT_PUBLIC_API
xqc_stream_t *xqc_stream_create(xqc_engine_t *engine,
    const xqc_cid_t *cid, xqc_stream_settings_t *settings, void *user_data);

XQC_EXPORT_PUBLIC_API
xqc_stream_t *xqc_stream_create_with_direction(xqc_connection_t *conn,
    xqc_stream_direction_t dir, void *user_data);

XQC_EXPORT_PUBLIC_API
xqc_stream_direction_t xqc_stream_get_direction(xqc_stream_t *strm);

/**
 * Server should set user_data when stream_create_notify callbacks
 */
XQC_EXPORT_PUBLIC_API
void xqc_stream_set_user_data(xqc_stream_t *stream, void *user_data);

XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_stream_update_settings(xqc_stream_t *stream,
    xqc_stream_settings_t *settings);

/**
 * Get connection's user_data by stream
 */
XQC_EXPORT_PUBLIC_API
void *xqc_get_conn_user_data_by_stream(xqc_stream_t *stream);

/**
 * Get connection's app_proto_user_data by stream
 */
XQC_EXPORT_PUBLIC_API
void *xqc_get_conn_alp_user_data_by_stream(xqc_stream_t *stream);

/**
 * Get stream ID
 */
XQC_EXPORT_PUBLIC_API
xqc_stream_id_t xqc_stream_id(xqc_stream_t *stream);

/**
 * Send RESET_STREAM to peer, stream_close_notify will callback when stream destroyed
 * @retval XQC_OK for success, others for failure
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_stream_close(xqc_stream_t *stream);

/**
 * Recv data in stream.
 * @return bytes read, -XQC_EAGAIN try next time, <0 for error
 */
XQC_EXPORT_PUBLIC_API
ssize_t xqc_stream_recv(xqc_stream_t *stream, unsigned char *recv_buf, size_t recv_buf_size,
    uint8_t *fin);

/**
 * Send data in stream.
 * @param fin  0 or 1,  1 - final data block send in this stream.
 * @return bytes sent, -XQC_EAGAIN try next time, <0 for error
 */
XQC_EXPORT_PUBLIC_API
ssize_t xqc_stream_send(xqc_stream_t *stream, unsigned char *send_data, size_t send_data_size,
    uint8_t fin);

/**
 * Get dcid and scid before process packet
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_packet_parse_cid(xqc_cid_t *dcid, xqc_cid_t *scid, uint8_t cid_len,
                               const unsigned char *buf, size_t size);

/**
 * @brief compare two cids
 * @return XQC_OK if equal, others if not equal
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_cid_is_equal(const xqc_cid_t *dst, const xqc_cid_t *src);

/**
 * Get scid in hex, end with '\0'
 * @param scid is returned from xqc_connect
 * @return user should copy return buffer to your own memory if you will access in the future
 */
XQC_EXPORT_PUBLIC_API
unsigned char *xqc_scid_str(xqc_engine_t *engine, const xqc_cid_t *scid);

XQC_EXPORT_PUBLIC_API
unsigned char *xqc_dcid_str(xqc_engine_t *engine, const xqc_cid_t *dcid);

XQC_EXPORT_PUBLIC_API
unsigned char *xqc_dcid_str_by_scid(xqc_engine_t *engine, const xqc_cid_t *scid);

XQC_EXPORT_PUBLIC_API
uint8_t xqc_engine_config_get_cid_len(xqc_engine_t *engine);

/**
 * User should call xqc_conn_continue_send when write event ready
 */
XQC_EXPORT_PUBLIC_API
xqc_int_t xqc_conn_continue_send(xqc_engine_t *engine, const xqc_cid_t *cid);

/**
 * User should call xqc_conn_continue_send when write event ready
 */
XQC_EXPORT_PUBLIC_API
void xqc_conn_continue_send_by_conn(xqc_connection_t *conn);

/**
 * User can get xqc_conn_stats_t by cid
 */
XQC_EXPORT_PUBLIC_API
xqc_conn_stats_t xqc_conn_get_stats(xqc_engine_t *engine, const xqc_cid_t *cid);

/**
 * User can get xqc_conn_qos_stats_t by cid
 */
XQC_EXPORT_PUBLIC_API
xqc_conn_qos_stats_t xqc_conn_get_qos_stats(xqc_engine_t *engine, const xqc_cid_t *cid);

/**
 * User get xqc_stream_stats_t
 */
XQC_EXPORT_PUBLIC_API
xqc_stream_stats_t xqc_stream_get_stats(xqc_stream_t *stream);

XQC_EXPORT_PUBLIC_API
xqc_conn_type_t xqc_conn_get_type(xqc_connection_t *conn);

/**
 * @brief Users call this function to get a template of conn settings, which serves
 *        as the starting point for users who want to refine conn settings according
 *        to their needs
 * @param settings_type there are different types of templates in XQUIC
 * @return conn settings
 */
XQC_EXPORT_PUBLIC_API
xqc_conn_settings_t xqc_conn_get_conn_settings_template(xqc_conn_settings_type_t settings_type);

#ifdef __cplusplus
}
#endif

#endif /* _XQUIC_H_INCLUDED_ */
