/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef RQC_TRANSPORT_PARAMS_H_
#define RQC_TRANSPORT_PARAMS_H_

#include <rquic/rquic.h>
#include "src/transport/rqc_defs.h"

/* default value for max_ack_delay */
#define RQC_DEFAULT_MAX_ACK_DELAY               25

/* default value for ack_delay_exponent */
#define RQC_DEFAULT_ACK_DELAY_EXPONENT          3

/* default value for max_udp_payload_size */
#define RQC_DEFAULT_MAX_UDP_PAYLOAD_SIZE        1500

/* max buffer length of encoded transport parameter */
#define RQC_MAX_TRANSPORT_PARAM_BUF_LEN         256

/**
 * @brief definition of transport parameter types
 */
typedef enum {
    RQC_TRANSPORT_PARAM_MAX_IDLE_TIMEOUT                    = 0x0000,
    RQC_TRANSPORT_PARAM_MAX_UDP_PAYLOAD_SIZE                = 0x0001,
    RQC_TRANSPORT_PARAM_INITIAL_MAX_DATA                    = 0x0002,
    RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL  = 0x0003,
    RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE = 0x0004,
    RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_UNI         = 0x0005,
    RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_BIDI            = 0x0006,
    RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_UNI             = 0x0007,
    RQC_TRANSPORT_PARAM_ACK_DELAY_EXPONENT                  = 0x0008,
    RQC_TRANSPORT_PARAM_MAX_ACK_DELAY                       = 0x0009,
} rqc_transport_param_id_t;

/* transport parameters */
typedef struct {
    rqc_usec_t              max_idle_timeout;
    uint64_t                max_udp_payload_size;
    uint64_t                initial_max_data;
    uint64_t                initial_max_stream_data_bidi_local;
    uint64_t                initial_max_stream_data_bidi_remote;
    uint64_t                initial_max_stream_data_uni;
    uint64_t                initial_max_streams_bidi;
    uint64_t                initial_max_streams_uni;
    uint64_t                ack_delay_exponent;
    rqc_usec_t              max_ack_delay;
} rqc_transport_params_t;

/**
 * encode transport parameters.
 * @param params input transport parameter structure
 * @param out pointer of destination buffer
 * @param out_cap capacity of output data buffer
 * @param out_len encoded buffer len
 * @return RQC_OK for success, negative for failure
 */
rqc_int_t rqc_encode_transport_params(const rqc_transport_params_t *params,
    uint8_t *out, size_t out_cap, size_t *out_len);

/**
 * decode transport parameters.
 * @param params output transport parameter structure
 * @param in encoded transport parameter buf
 * @param in_len encoded transport parameter buf len
 * @return RQC_OK for success, negative for failure
 */
rqc_int_t rqc_decode_transport_params(rqc_transport_params_t *params,
    const uint8_t *in, size_t in_len);

void rqc_init_transport_params(rqc_transport_params_t *params);

#endif /* RQC_TRANSPORT_PARAMS_H_ */
