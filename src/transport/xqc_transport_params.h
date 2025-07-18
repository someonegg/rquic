/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef XQC_TRANSPORT_PARAMS_H_
#define XQC_TRANSPORT_PARAMS_H_

#include <xquic/xquic.h>
#include "src/transport/xqc_defs.h"

/* default value for max_ack_delay */
#define XQC_DEFAULT_MAX_ACK_DELAY               25

/* default value for ack_delay_exponent */
#define XQC_DEFAULT_ACK_DELAY_EXPONENT          3

/* default value for max_udp_payload_size */
#define XQC_DEFAULT_MAX_UDP_PAYLOAD_SIZE        1500

/* max buffer length of encoded transport parameter */
#define XQC_MAX_TRANSPORT_PARAM_BUF_LEN         512

/**
 * @brief definition of transport parameter types
 */
typedef enum {
    XQC_TRANSPORT_PARAM_MAX_IDLE_TIMEOUT                    = 0x0000,
    XQC_TRANSPORT_PARAM_MAX_UDP_PAYLOAD_SIZE                = 0x0001,
    XQC_TRANSPORT_PARAM_INITIAL_MAX_DATA                    = 0x0002,
    XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL  = 0x0003,
    XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE = 0x0004,
    XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_UNI         = 0x0005,
    XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_BIDI            = 0x0006,
    XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_UNI             = 0x0007,
    XQC_TRANSPORT_PARAM_ACK_DELAY_EXPONENT                  = 0x0008,
    XQC_TRANSPORT_PARAM_MAX_ACK_DELAY                       = 0x0009,
} xqc_transport_param_id_t;

/* transport parameters */
typedef struct {
    xqc_usec_t              max_idle_timeout;
    uint64_t                max_udp_payload_size;
    uint64_t                initial_max_data;
    uint64_t                initial_max_stream_data_bidi_local;
    uint64_t                initial_max_stream_data_bidi_remote;
    uint64_t                initial_max_stream_data_uni;
    uint64_t                initial_max_streams_bidi;
    uint64_t                initial_max_streams_uni;
    uint64_t                ack_delay_exponent;
    xqc_usec_t              max_ack_delay;
} xqc_transport_params_t;

/**
 * encode transport parameters.
 * @param params input transport parameter structure
 * @param out pointer of destination buffer
 * @param out_cap capacity of output data buffer
 * @param out_len encoded buffer len
 * @return XQC_OK for success, negative for failure
 */
xqc_int_t xqc_encode_transport_params(const xqc_transport_params_t *params,
    uint8_t *out, size_t out_cap, size_t *out_len);

/**
 * decode transport parameters.
 * @param params output transport parameter structure
 * @param in encoded transport parameter buf
 * @param in_len encoded transport parameter buf len
 * @return XQC_OK for success, negative for failure
 */
xqc_int_t xqc_decode_transport_params(xqc_transport_params_t *params,
    const uint8_t *in, size_t in_len);

xqc_int_t xqc_read_transport_params(char *tp_data, size_t tp_data_len,
    xqc_transport_params_t *params);

ssize_t xqc_write_transport_params(char *tp_buf, size_t cap,
    const xqc_transport_params_t *params);

void xqc_init_transport_params(xqc_transport_params_t *params);

#endif /* XQC_TRANSPORT_PARAMS_H_ */