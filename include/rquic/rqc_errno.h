/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_ERRNO_H_INCLUDED_
#define _RQC_ERRNO_H_INCLUDED_

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
    TRA_APPLICATION_ERROR           =  0xB,
} rqc_trans_err_code_t;

typedef enum {
    REQUEST_NO_ERROR             = 0x100,
    REQUEST_REJECTED             = 0x10B,
    REQUEST_CANCELLED            = 0x10C,
    REQUEST_INCOMPLETE           = 0x10D,
} rqc_request_err_code_t;

#define RQC_OK      0
#define RQC_ERROR   -1

/**
 * @brief rquic transport internal error codes: 6xx
 */
typedef enum {
    RQC_ENOBUF                          = 600,      /**< not enough buf space */
    RQC_EVINTREAD                       = 601,      /**< parse frame error */
    RQC_ENULLPTR                        = 602,      /**< empty pointer, usually a malloc failure */
    RQC_EMALLOC                         = 603,      /**< malloc failure */
    RQC_EILLPKT                         = 604,      /**< illegal packet, don't close connection, just drop it */
    RQC_EILLTP                          = 605,      /**< illegal transport parameter, close connection */
    RQC_EILLFRAME                       = 606,      /**< illegal stream & frame, close connection */
    RQC_ECREATE_CONN                    = 607,      /**< fail to create a connection */
    RQC_CLOSING                         = 608,      /**< connection is closing, operation denied */
    RQC_ECONN_NFOUND                    = 609,      /**< fail to find the corresponding connection */
    RQC_ESYS                            = 610,      /**< system error, usually a public library interface failure */
    RQC_EAGAIN                          = 611,      /**< write blocking, similar to EAGAIN */
    RQC_EPARAM                          = 612,      /**< wrong parameters */
    RQC_ESTATE                          = 613,      /**< abnormal connection status */
    RQC_ELIMIT                          = 614,      /**< exceed cache limit */
    RQC_EPROTO                          = 615,      /**< violation of protocol */
    RQC_ESOCKET                         = 616,      /**< socket interface error */
    RQC_EFATAL                          = 617,      /**< fatal error, engine will immediately destroy the connection */
    RQC_ECONN_BLOCKED                   = 618,      /**< connection-level flow control */
    RQC_ESTREAM_BLOCKED                 = 619,      /**< stream-level flow control */
    RQC_ESTREAM_NFOUND                  = 620,      /**< fail to find the corresponding stream */
    RQC_EWRITE_PKT                      = 621,      /**< fail to create a package or write a package header */
    RQC_ECREATE_STREAM                  = 622,      /**< fail to create stream */
    RQC_ESTREAM_RESET                   = 623,      /**< stream has been reset */
    RQC_EDUP_FRAME                      = 624,      /**< duplicate frames */
    RQC_EVERSION                        = 625,      /**< this version is not supported and requires negotiation */
    RQC_EWAITING                        = 626,      /**< need to wait */
    RQC_EIGNORE_PKT                     = 627,      /**< ignore unknown packet/frame, don't close connection */
    RQC_EGENERATE_CID                   = 628,      /**< connection ID generation error */
    RQC_ECONN_NO_AVAIL_CID              = 629,      /**< no available connection ID */
    RQC_ECONN_CID_NOT_FOUND             = 630,      /**< can't find cid in connection */
    RQC_ECID_STATE                      = 631,      /**< abnormal connection ID status */
    RQC_EACTIVE_CID_LIMIT               = 632,      /**< active cid exceed active_connection_id_limit */
    RQC_EALPN_NOT_SUPPORTED             = 633,      /**< alpn is not supported by server */
    RQC_EALPN_NOT_REGISTERED            = 634,      /**< alpn is not registered */
    RQC_EPACKET_FILETER_CALLBACK        = 635,      /**< error with packet filter callback function */

    RQC_EMP_CREATE_PATH                 = 660,      /**< create path error */

} rqc_transport_error_t;

#endif /* _RQC_ERRNO_H_INCLUDED_ */
