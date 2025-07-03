/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _XQC_ERRNO_H_INCLUDED_
#define _XQC_ERRNO_H_INCLUDED_

/**
 * @brief QUIC Transport Protocol error codes
 */
typedef enum {
    TRA_NO_ERROR                    =  0x0,
    TRA_INTERNAL_ERROR              =  0x1,
    TRA_CONNECTION_REFUSED_ERROR    =  0x2,
    TRA_FLOW_CONTROL_ERROR          =  0x3,
    TRA_STREAM_LIMIT_ERROR          =  0x4,
    TRA_STREAM_STATE_ERROR          =  0x5,
    TRA_FINAL_SIZE_ERROR            =  0x6,
    TRA_FRAME_ENCODING_ERROR        =  0x7,
    TRA_TRANSPORT_PARAMETER_ERROR   =  0x8,
    TRA_CONNECTION_ID_LIMIT_ERROR   =  0x9,
    TRA_PROTOCOL_VIOLATION          =  0xA,
    TRA_INVALID_TOKEN               =  0xB,
    TRA_APPLICATION_ERROR           =  0xC,
    TRA_CRYPTO_BUFFER_EXCEEDED      =  0xD,
    TRA_0RTT_TRANS_PARAMS_ERROR     =  0xE,   /**< MUST delete the current saved 0RTT transport parameters */
    TRA_HS_CERTIFICATE_VERIFY_FAIL  =  0x1FE, /**< for handshake certificate verify error */
    TRA_CRYPTO_ERROR                =  0x1FF, /**< 0x1XX */
} xqc_trans_err_code_t;


/**
 * @brief Multipath error codes
 */
typedef enum {
    TRA_MP_PROTOCOL_VIOLATION       = 0x1001d76d3ded42f3
} xqc_mp_err_code_t;


#define TRA_CRYPTO_ERROR_BASE   0x100

typedef enum {
    REQUEST_NO_ERROR             = 0x100,
    REQUEST_REJECTED             = 0x10B,
    REQUEST_CANCELLED            = 0x10C,
    REQUEST_INCOMPLETE           = 0x10D,
} xqc_request_err_code_t;


#define XQC_OK      0
#define XQC_ERROR   -1


/**
 * @brief xquic transport internal error codes: 6xx 
 */
typedef enum {
    XQC_ENOBUF                          = 600,      /**< not enough buf space */
    XQC_EVINTREAD                       = 601,      /**< parse frame error */
    XQC_ENULLPTR                        = 602,      /**< empty pointer, usually a malloc failure */
    XQC_EMALLOC                         = 603,      /**< malloc failure */
    XQC_EILLPKT                         = 604,      /**< illegal packet, don't close connection, just drop it */
    XQC_ELEVEL                          = 605,      /**< incorrect encryption level */
    XQC_ECREATE_CONN                    = 606,      /**< fail to create a connection */
    XQC_CLOSING                         = 607,      /**< connection is closing, operation denied */
    XQC_ECONN_NFOUND                    = 608,      /**< fail to find the corresponding connection */
    XQC_ESYS                            = 609,      /**< system error, usually a public library interface failure */
    XQC_EAGAIN                          = 610,      /**< write blocking, similar to EAGAIN */
    XQC_EPARAM                          = 611,      /**< wrong parameters */
    XQC_ESTATE                          = 612,      /**< abnormal connection status */
    XQC_ELIMIT                          = 613,      /**< exceed cache limit */
    XQC_EPROTO                          = 614,      /**< violation of protocol */
    XQC_ESOCKET                         = 615,      /**< socket interface error */
    XQC_EFATAL                          = 616,      /**< fatal error, engine will immediately destroy the connection */
    XQC_ESTREAM_ST                      = 617,      /**< abnormal flow status */
    XQC_ESEND_RETRY                     = 618,      /**< send retry failure */
    XQC_ECONN_BLOCKED                   = 619,      /**< connection-level flow control */
    XQC_ESTREAM_BLOCKED                 = 620,      /**< stream-level flow control */
    XQC_EENCRYPT                        = 621,      /**< encryption error */
    XQC_EDECRYPT                        = 622,      /**< decryption error */
    XQC_ESTREAM_NFOUND                  = 623,      /**< fail to find the corresponding stream */
    XQC_EWRITE_PKT                      = 624,      /**< fail to create a package or write a package header */
    XQC_ECREATE_STREAM                  = 625,      /**< fail to create stream */
    XQC_ESTREAM_RESET                   = 626,      /**< stream has been reset */
    XQC_EDUP_FRAME                      = 627,      /**< duplicate frames */
    XQC_EFINAL_SIZE                     = 628,      /**< STREAM frame final size error */
    XQC_EVERSION                        = 629,      /**< this version is not supported and requires negotiation */
    XQC_EWAITING                        = 630,      /**< need to wait */
    XQC_EIGNORE_PKT                     = 631,      /**< ignore unknown packet/frame, don't close connection */
    XQC_EGENERATE_CID                   = 632,      /**< connection ID generation error */
    XQC_EANTI_AMPLIFICATION_LIMIT       = 633,      /**< server reached the anti-amplification limit */
    XQC_ECONN_NO_AVAIL_CID              = 634,      /**< no available connection ID */
    XQC_ECONN_CID_NOT_FOUND             = 635,      /**< can't find cid in connection */
    XQC_EILLEGAL_FRAME                  = 636,      /**< illegal stream & frame, close connection */
    XQC_ECID_STATE                      = 637,      /**< abnormal connection ID status */
    XQC_EACTIVE_CID_LIMIT               = 638,      /**< active cid exceed active_connection_id_limit */
    XQC_EALPN_NOT_SUPPORTED             = 639,      /**< alpn is not supported by server */
    XQC_EALPN_NOT_REGISTERED            = 640,      /**< alpn is not registered */
    XQC_ESTATELESS_RESET                = 641,      /**< connection is reset by peer */
    XQC_EPACKET_FILETER_CALLBACK        = 642,      /**< error with packet filter callback function */

    XQC_EMP_NOT_SUPPORT_MP              = 650,      /**< Multipath - don't support multipath */
    XQC_EMP_NO_AVAIL_PATH_ID            = 651,      /**< Multipath - no available path id */
    XQC_EMP_CREATE_PATH                 = 652,      /**< Multipath - create path error */
    XQC_EMP_PATH_NOT_FOUND              = 653,      /**< Multipath - can't find path in paths_list */
    XQC_EMP_PATH_STATE_ERROR            = 654,      /**< Multipath - abnormal path status */
    XQC_EMP_SCHEDULE_PATH               = 655,      /**< Multipath - fail to schedule path for sending */
    XQC_EMP_NO_ACTIVE_PATH              = 656,      /**< Multipath - no another active path */
    XQC_EMP_INVALID_MP_VERTION          = 657,      /**< Multipath - the multipath version value is invalid */
    XQC_EMP_NO_AVAILABLE_CID_FOR_PATH   = 658,      /**< Multipath - there's no available unused cid for this path */

    XQC_EFEC_NOT_SUPPORT_FEC            = 660,      /**< FEC - fec not supported */
    XQC_EFEC_SCHEME_ERROR               = 661,      /**< FEC - no available scheme */
    XQC_EFEC_SYMBOL_ERROR               = 662,      /**< FEC - symbol value error */
    XQC_EFEC_TOLERABLE_ERROR            = 663,      /**< FEC - tolerable error */
    
    XQC_EENCRYPT_LB_CID                 = 670,      /**< load balance connection ID encryption error */
    XQC_EENCRYPT_AES_128_ECB            = 671,      /**< aes_128_ecb algorithm error */

    XQC_EDGRAM_NOT_SUPPORTED            = 680,      /**< Datagram - not supported */
    XQC_EDGRAM_TOO_LARGE                = 681,      /**< Datagram - payload size too large */

    XQC_EPMTUD_PROBING_SIZE             = 682,      /**< PMTUD - probing size error */

    XQC_EACK_EXT_ABN_VAL                = 690,      /**< ACK Extension - abnormal value */

    XQC_E_MAX,
} xqc_transport_error_t;

#define TRANS_ERR_START 600
static const int TRANS_ERR_CNT = XQC_E_MAX - TRANS_ERR_START;


/**
 * @brief xquic TLS internal error codes: 7xx 
 */
typedef enum {
    XQC_TLS_INVALID_ARGUMENT            = 700,
    XQC_TLS_UNKNOWN_PKT_TYPE            = 701,
    XQC_TLS_NOBUF                       = 702,
    XQC_TLS_PROTO                       = 703,
    XQC_TLS_INVALID_STATE               = 704,
    XQC_TLS_ACK_FRAME                   = 705,
    XQC_TLS_STREAM_ID_BLOCKED           = 706,
    XQC_TLS_STREAM_IN_USE               = 707,
    XQC_TLS_STREAM_DATA_BLOCKED         = 708,
    XQC_TLS_FLOW_CONTROL                = 709,
    XQC_TLS_STREAM_LIMIT                = 710,
    XQC_TLS_FINAL_OFFSET                = 711,
    XQC_TLS_CRYPTO                      = 712,
    XQC_TLS_PKT_NUM_EXHAUSTED           = 713,
    XQC_TLS_REQUIRED_TRANSPORT_PARAM    = 714,
    XQC_TLS_MALFORMED_TRANSPORT_PARAM   = 715,
    XQC_TLS_FRAME_ENCODING              = 716,
    XQC_TLS_DECRYPT                     = 717,
    XQC_TLS_STREAM_SHUT_WR              = 718,
    XQC_TLS_STREAM_NOT_FOUND            = 719,
    XQC_TLS_VERSION_NEGOTIATION         = 720,
    XQC_TLS_STREAM_STATE                = 721,
    XQC_TLS_NOKEY                       = 722,
    XQC_TLS_EARLY_DATA_REJECTED         = 723,
    XQC_TLS_RECV_VERSION_NEGOTIATION    = 724,
    XQC_TLS_CLOSING                     = 725,
    XQC_TLS_DRAINING                    = 726,
    XQC_TLS_TRANSPORT_PARAM             = 727,
    XQC_TLS_DISCARD_PKT                 = 728,
    XQC_TLS_FATAL                       = 729,
    XQC_TLS_NOMEM                       = 730,
    XQC_TLS_CALLBACK_FAILURE            = 731,
    XQC_TLS_INTERNAL                    = 732,
    XQC_TLS_DATA_REJECT                 = 733,
    XQC_TLS_CLIENT_INITIAL_ERROR        = 734,
    XQC_TLS_CLIENT_REINTIAL_ERROR       = 735,
    XQC_TLS_ENCRYPT_DATA_ERROR          = 736,
    XQC_TLS_DECRYPT_DATA_ERROR          = 737,
    XQC_TLS_CRYPTO_CTX_NEGOTIATED_ERROR = 738, 
    XQC_TLS_SET_TRANSPORT_PARAM_ERROR   = 739,
    XQC_TLS_SET_CIPHER_SUITES_ERROR     = 740,
    XQC_TLS_DERIVE_KEY_ERROR            = 741,
    XQC_TLS_DO_HANDSHAKE_ERROR          = 742,
    XQC_TLS_POST_HANDSHAKE_ERROR        = 743,
    XQC_TLS_UPDATE_KEY_ERROR            = 744,
    XQC_TLS_DECRYPT_WHEN_KU_ERROR       = 745,

    XQC_TLS_ERR_MAX,
} xqc_tls_error_t;

#define TLS_ERR_START 700
static const int TLS_ERR_CNT = XQC_TLS_ERR_MAX - TLS_ERR_START;


#endif /* _XQC_ERRNO_H_INCLUDED_ */
