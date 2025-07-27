/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "rqc_transport_params.h"
#include "src/common/utils/vint/rqc_variable_len_int.h"
#include "src/common/rqc_str.h"
#include "src/transport/rqc_defs.h"
#include "src/transport/rqc_cid.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

/* ack_delay_exponent above 20 is invalid */
#define RQC_MAX_ACK_DELAY_EXPONENT          20

static ssize_t
rqc_transport_params_calc_length(const rqc_transport_params_t *params)
{
    size_t len = 0;

    if (params->max_idle_timeout) {
        len += rqc_put_varint_len(RQC_TRANSPORT_PARAM_MAX_IDLE_TIMEOUT) +
               rqc_put_varint_len(rqc_put_varint_len(params->max_idle_timeout)) +
               rqc_put_varint_len(params->max_idle_timeout);
    }

    if (params->max_udp_payload_size != RQC_DEFAULT_MAX_UDP_PAYLOAD_SIZE) {
        len += rqc_put_varint_len(RQC_TRANSPORT_PARAM_MAX_UDP_PAYLOAD_SIZE) +
               rqc_put_varint_len(rqc_put_varint_len(params->max_udp_payload_size)) +
               rqc_put_varint_len(params->max_udp_payload_size);
    }

    if (params->initial_max_data) {
        len += rqc_put_varint_len(RQC_TRANSPORT_PARAM_INITIAL_MAX_DATA) +
               rqc_put_varint_len(rqc_put_varint_len(params->initial_max_data)) +
               rqc_put_varint_len(params->initial_max_data);
    }

    if (params->initial_max_stream_data_bidi_local) {
        len += rqc_put_varint_len(RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL) +
               rqc_put_varint_len(rqc_put_varint_len(params->initial_max_stream_data_bidi_local)) +
               rqc_put_varint_len(params->initial_max_stream_data_bidi_local);
    }

    if (params->initial_max_stream_data_bidi_remote) {
        len += rqc_put_varint_len(RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE) +
               rqc_put_varint_len(rqc_put_varint_len(params->initial_max_stream_data_bidi_remote)) +
               rqc_put_varint_len(params->initial_max_stream_data_bidi_remote);
    }

    if (params->initial_max_stream_data_uni) {
        len += rqc_put_varint_len(RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_UNI) +
               rqc_put_varint_len(rqc_put_varint_len(params->initial_max_stream_data_uni)) +
               rqc_put_varint_len(params->initial_max_stream_data_uni);
    }

    if (params->initial_max_streams_bidi) {
        len += rqc_put_varint_len(RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_BIDI) +
               rqc_put_varint_len(rqc_put_varint_len(params->initial_max_streams_bidi)) +
               rqc_put_varint_len(params->initial_max_streams_bidi);
    }

    if (params->initial_max_streams_uni) {
        len += rqc_put_varint_len(RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_UNI) +
               rqc_put_varint_len(rqc_put_varint_len(params->initial_max_streams_uni)) +
               rqc_put_varint_len(params->initial_max_streams_uni);
    }

    if (params->ack_delay_exponent != RQC_DEFAULT_ACK_DELAY_EXPONENT) {
        len += rqc_put_varint_len(RQC_TRANSPORT_PARAM_ACK_DELAY_EXPONENT) +
               rqc_put_varint_len(rqc_put_varint_len(params->ack_delay_exponent)) +
               rqc_put_varint_len(params->ack_delay_exponent);
    }

    if (params->max_ack_delay != RQC_DEFAULT_MAX_ACK_DELAY) {
        len += rqc_put_varint_len(RQC_TRANSPORT_PARAM_MAX_ACK_DELAY) +
               rqc_put_varint_len(rqc_put_varint_len(params->max_ack_delay)) +
               rqc_put_varint_len(params->max_ack_delay);
    }

    return len;
}

/**
 * put variant int value param into buf
 */
inline static uint8_t*
rqc_put_varint_param(uint8_t* p, rqc_transport_param_id_t id, uint64_t v)
{
    p = rqc_put_varint(p, id);
    p = rqc_put_varint(p, rqc_put_varint_len(v));
    p = rqc_put_varint(p, v);
    return p;
}

rqc_int_t
rqc_encode_transport_params(const rqc_transport_params_t *params,
    uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t *p = out;
    size_t len = 0;

    /* calculate encoding length */
    len += rqc_transport_params_calc_length(params);
    if (out_cap < len) {
        return -RQC_ENOBUF;
    }

    if (params->max_idle_timeout) {
        p = rqc_put_varint_param(p, RQC_TRANSPORT_PARAM_MAX_IDLE_TIMEOUT,
                                 params->max_idle_timeout);
    }

    if (params->max_udp_payload_size != RQC_DEFAULT_MAX_UDP_PAYLOAD_SIZE) {
        p = rqc_put_varint_param(p, RQC_TRANSPORT_PARAM_MAX_UDP_PAYLOAD_SIZE,
                                 params->max_udp_payload_size);
    }

    if (params->initial_max_data) {
        p = rqc_put_varint_param(p, RQC_TRANSPORT_PARAM_INITIAL_MAX_DATA,
                                 params->initial_max_data);
    }

    if (params->initial_max_stream_data_bidi_local) {
        p = rqc_put_varint_param(p, RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL,
                                 params->initial_max_stream_data_bidi_local);
    }

    if (params->initial_max_stream_data_bidi_remote) {
        p = rqc_put_varint_param(p, RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE,
                                 params->initial_max_stream_data_bidi_remote);
    }

    if (params->initial_max_stream_data_uni) {
        p = rqc_put_varint_param(p, RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_UNI,
                                 params->initial_max_stream_data_uni);
    }

    if (params->initial_max_streams_bidi) {
        p = rqc_put_varint_param(p, RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_BIDI,
                                 params->initial_max_streams_bidi);
    }

    if (params->initial_max_streams_uni) {
        p = rqc_put_varint_param(p, RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_UNI,
                                 params->initial_max_streams_uni);
    }

    if (params->ack_delay_exponent != RQC_DEFAULT_ACK_DELAY_EXPONENT) {
        p = rqc_put_varint_param(p, RQC_TRANSPORT_PARAM_ACK_DELAY_EXPONENT,
                                 params->ack_delay_exponent);
    }

    if (params->max_ack_delay != RQC_DEFAULT_MAX_ACK_DELAY) {
        p = rqc_put_varint_param(p, RQC_TRANSPORT_PARAM_MAX_ACK_DELAY,
                                 params->max_ack_delay);
    }

    if ((size_t)(p - out) != len) {
        return -RQC_EILLTP;
    }

    *out_len = len;
    return RQC_OK;
}

/* dst should be destination value point */
#define RQC_DECODE_VINT_VALUE(dst, p, end)                  \
    do {                                                    \
        ssize_t nread = rqc_vint_read((p), (end), (dst));   \
        if (nread < 0) {                                    \
            return -RQC_EILLTP;      \
        }                                                   \
        return RQC_OK;                                      \
    } while(0)

static rqc_int_t
rqc_decode_max_idle_timeout(rqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    RQC_DECODE_VINT_VALUE(&params->max_idle_timeout, p, end);
}

static rqc_int_t
rqc_decode_max_udp_payload_size(rqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    RQC_DECODE_VINT_VALUE(&params->max_udp_payload_size, p, end);
}

static rqc_int_t
rqc_decode_initial_max_data(rqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    RQC_DECODE_VINT_VALUE(&params->initial_max_data, p, end);
}

static rqc_int_t
rqc_decode_initial_max_stream_data_bidi_local(rqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    RQC_DECODE_VINT_VALUE(&params->initial_max_stream_data_bidi_local, p, end);
}

static rqc_int_t
rqc_decode_initial_max_stream_data_bidi_remote(rqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    RQC_DECODE_VINT_VALUE(&params->initial_max_stream_data_bidi_remote, p, end);
}

static rqc_int_t
rqc_decode_initial_max_stream_data_uni(rqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    RQC_DECODE_VINT_VALUE(&params->initial_max_stream_data_uni, p, end);
}

static rqc_int_t
rqc_decode_initial_max_streams_bidi(rqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    RQC_DECODE_VINT_VALUE(&params->initial_max_streams_bidi, p, end);
}

static rqc_int_t
rqc_decode_initial_max_streams_uni(rqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    RQC_DECODE_VINT_VALUE(&params->initial_max_streams_uni, p, end);
}

static rqc_int_t
rqc_decode_ack_delay_exponent(rqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    ssize_t nread = rqc_vint_read(p, end, &params->ack_delay_exponent);
    /* [TRANSPORT] Values above 20 are invalid */
    if (nread < 0 || params->ack_delay_exponent > RQC_MAX_ACK_DELAY_EXPONENT) {
        return -RQC_EILLTP;
    }
    return RQC_OK;
}

static rqc_int_t
rqc_decode_max_ack_delay(rqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    RQC_DECODE_VINT_VALUE(&params->max_ack_delay, p, end);
}

typedef enum {
    RQC_TP_DECODER_MAX_IDLE_TIMEOUT            = 0x0000,
    RQC_TP_DECODER_MAX_UDP_PAYLOAD_SIZE                ,
    RQC_TP_DECODER_INITIAL_MAX_DATA                    ,
    RQC_TP_DECODER_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL  ,
    RQC_TP_DECODER_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE ,
    RQC_TP_DECODER_INITIAL_MAX_STREAM_DATA_UNI         ,
    RQC_TP_DECODER_INITIAL_MAX_STREAMS_BIDI            ,
    RQC_TP_DECODER_INITIAL_MAX_STREAMS_UNI             ,
    RQC_TP_DECODER_ACK_DELAY_EXPONENT                  ,
    RQC_TP_DECODER_MAX_ACK_DELAY                       ,
    RQC_TP_DECODER_UNKNOWN
} rqc_tp_decoder_index_t;

/* decode value from p, and store value in the input params */
typedef rqc_int_t (*rqc_trans_param_decode_func)(
    rqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len);

rqc_trans_param_decode_func rqc_trans_param_decode_func_list[] = {
    rqc_decode_max_idle_timeout,
    rqc_decode_max_udp_payload_size,
    rqc_decode_initial_max_data,
    rqc_decode_initial_max_stream_data_bidi_local,
    rqc_decode_initial_max_stream_data_bidi_remote,
    rqc_decode_initial_max_stream_data_uni,
    rqc_decode_initial_max_streams_bidi,
    rqc_decode_initial_max_streams_uni,
    rqc_decode_ack_delay_exponent,
    rqc_decode_max_ack_delay,
};

/* convert param_type to param's index in rqc_trans_param_decode_func_list */
uint64_t
rqc_trans_param_get_index(uint64_t param_type)
{
    switch (param_type) {
    case RQC_TRANSPORT_PARAM_MAX_IDLE_TIMEOUT:
    case RQC_TRANSPORT_PARAM_MAX_UDP_PAYLOAD_SIZE:
    case RQC_TRANSPORT_PARAM_INITIAL_MAX_DATA:
    case RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL:
    case RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE:
    case RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_UNI:
    case RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_BIDI:
    case RQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_UNI:
    case RQC_TRANSPORT_PARAM_ACK_DELAY_EXPONENT:
    case RQC_TRANSPORT_PARAM_MAX_ACK_DELAY:
        return (rqc_tp_decoder_index_t)param_type;

    default:
        break;
    }

    return RQC_TP_DECODER_UNKNOWN;
}

/**
 * decode one param
 */
static inline rqc_int_t
rqc_decode_one_transport_param(rqc_transport_params_t *params,
    const uint8_t **start, const uint8_t *end)
{
    const uint8_t *p = *start;
    uint64_t param_type = 0;
    uint64_t param_len = 0;

    /* read param type */
    ssize_t nread = rqc_vint_read(p, end, &param_type);
    if (nread < 0) {
        return -RQC_EILLTP;
    }
    p += nread;

    /* read param len */
    nread = rqc_vint_read(p, end, &param_len);
    if (nread < 0 || p + nread + param_len > end ) {
        return -RQC_EILLTP;
    }
    p += nread;

    /*
     * read param value, note: some parameters are allowed to be zero-length.
     */
    uint64_t param_index = rqc_trans_param_get_index(param_type);
    if (param_index != RQC_TP_DECODER_UNKNOWN) {
        rqc_int_t ret = rqc_trans_param_decode_func_list[param_index](params, p, end,
                                                                      param_type, param_len);
        if (ret < 0) {
            return -RQC_EILLTP;
        }
    }

    p += param_len;

    *start = p;
    return RQC_OK;
}

rqc_int_t
rqc_decode_transport_params(rqc_transport_params_t *params,
    const uint8_t *in, size_t in_len)
{
    const uint8_t *p, *end;
    rqc_int_t ret = RQC_OK;

    p = in;
    end = in + in_len;

    /* Set default values */
    params->max_idle_timeout = 0;
    params->max_udp_payload_size = RQC_DEFAULT_MAX_UDP_PAYLOAD_SIZE;
    params->initial_max_data = 0;
    params->initial_max_streams_bidi = 0;
    params->initial_max_streams_uni = 0;
    params->initial_max_stream_data_bidi_local = 0;
    params->initial_max_stream_data_bidi_remote = 0;
    params->initial_max_stream_data_uni = 0;
    params->ack_delay_exponent = RQC_DEFAULT_ACK_DELAY_EXPONENT;
    params->max_ack_delay = RQC_DEFAULT_MAX_ACK_DELAY;

    while (p < end) {
        ret = rqc_decode_one_transport_param(params, &p, end);
        if (ret < 0) {
            return ret;
        }
    }

    if (end != p) {
        return -RQC_EILLTP;
    }

    return RQC_OK;
}

void
rqc_init_transport_params(rqc_transport_params_t *params)
{
    rqc_memzero(params, sizeof(rqc_transport_params_t));

    /* transport parameter related attributes */
    params->max_ack_delay = RQC_DEFAULT_MAX_ACK_DELAY;
    params->ack_delay_exponent = RQC_DEFAULT_ACK_DELAY_EXPONENT;
    params->max_udp_payload_size = RQC_DEFAULT_MAX_UDP_PAYLOAD_SIZE;
}
