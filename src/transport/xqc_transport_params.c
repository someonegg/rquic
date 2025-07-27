/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "xqc_transport_params.h"
#include "src/common/utils/vint/xqc_variable_len_int.h"
#include "src/common/xqc_str.h"
#include "src/transport/xqc_defs.h"
#include "src/transport/xqc_cid.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

/* ack_delay_exponent above 20 is invalid */
#define XQC_MAX_ACK_DELAY_EXPONENT          20

static ssize_t
xqc_transport_params_calc_length(const xqc_transport_params_t *params)
{
    size_t len = 0;

    if (params->max_idle_timeout) {
        len += xqc_put_varint_len(XQC_TRANSPORT_PARAM_MAX_IDLE_TIMEOUT) +
               xqc_put_varint_len(xqc_put_varint_len(params->max_idle_timeout)) +
               xqc_put_varint_len(params->max_idle_timeout);
    }

    if (params->max_udp_payload_size != XQC_DEFAULT_MAX_UDP_PAYLOAD_SIZE) {
        len += xqc_put_varint_len(XQC_TRANSPORT_PARAM_MAX_UDP_PAYLOAD_SIZE) +
               xqc_put_varint_len(xqc_put_varint_len(params->max_udp_payload_size)) +
               xqc_put_varint_len(params->max_udp_payload_size);
    }

    if (params->initial_max_data) {
        len += xqc_put_varint_len(XQC_TRANSPORT_PARAM_INITIAL_MAX_DATA) +
               xqc_put_varint_len(xqc_put_varint_len(params->initial_max_data)) +
               xqc_put_varint_len(params->initial_max_data);
    }

    if (params->initial_max_stream_data_bidi_local) {
        len += xqc_put_varint_len(XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL) +
               xqc_put_varint_len(xqc_put_varint_len(params->initial_max_stream_data_bidi_local)) +
               xqc_put_varint_len(params->initial_max_stream_data_bidi_local);
    }

    if (params->initial_max_stream_data_bidi_remote) {
        len += xqc_put_varint_len(XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE) +
               xqc_put_varint_len(xqc_put_varint_len(params->initial_max_stream_data_bidi_remote)) +
               xqc_put_varint_len(params->initial_max_stream_data_bidi_remote);
    }

    if (params->initial_max_stream_data_uni) {
        len += xqc_put_varint_len(XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_UNI) +
               xqc_put_varint_len(xqc_put_varint_len(params->initial_max_stream_data_uni)) +
               xqc_put_varint_len(params->initial_max_stream_data_uni);
    }

    if (params->initial_max_streams_bidi) {
        len += xqc_put_varint_len(XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_BIDI) +
               xqc_put_varint_len(xqc_put_varint_len(params->initial_max_streams_bidi)) +
               xqc_put_varint_len(params->initial_max_streams_bidi);
    }

    if (params->initial_max_streams_uni) {
        len += xqc_put_varint_len(XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_UNI) +
               xqc_put_varint_len(xqc_put_varint_len(params->initial_max_streams_uni)) +
               xqc_put_varint_len(params->initial_max_streams_uni);
    }

    if (params->ack_delay_exponent != XQC_DEFAULT_ACK_DELAY_EXPONENT) {
        len += xqc_put_varint_len(XQC_TRANSPORT_PARAM_ACK_DELAY_EXPONENT) +
               xqc_put_varint_len(xqc_put_varint_len(params->ack_delay_exponent)) +
               xqc_put_varint_len(params->ack_delay_exponent);
    }

    if (params->max_ack_delay != XQC_DEFAULT_MAX_ACK_DELAY) {
        len += xqc_put_varint_len(XQC_TRANSPORT_PARAM_MAX_ACK_DELAY) +
               xqc_put_varint_len(xqc_put_varint_len(params->max_ack_delay)) +
               xqc_put_varint_len(params->max_ack_delay);
    }

    return len;
}

/**
 * put variant int value param into buf
 */
inline static uint8_t*
xqc_put_varint_param(uint8_t* p, xqc_transport_param_id_t id, uint64_t v)
{
    p = xqc_put_varint(p, id);
    p = xqc_put_varint(p, xqc_put_varint_len(v));
    p = xqc_put_varint(p, v);
    return p;
}

xqc_int_t
xqc_encode_transport_params(const xqc_transport_params_t *params,
    uint8_t *out, size_t out_cap, size_t *out_len)
{
    uint8_t *p = out;
    size_t len = 0;

    /* calculate encoding length */
    len += xqc_transport_params_calc_length(params);
    if (out_cap < len) {
        return -XQC_ENOBUF;
    }

    if (params->max_idle_timeout) {
        p = xqc_put_varint_param(p, XQC_TRANSPORT_PARAM_MAX_IDLE_TIMEOUT,
                                 params->max_idle_timeout);
    }

    if (params->max_udp_payload_size != XQC_DEFAULT_MAX_UDP_PAYLOAD_SIZE) {
        p = xqc_put_varint_param(p, XQC_TRANSPORT_PARAM_MAX_UDP_PAYLOAD_SIZE,
                                 params->max_udp_payload_size);
    }

    if (params->initial_max_data) {
        p = xqc_put_varint_param(p, XQC_TRANSPORT_PARAM_INITIAL_MAX_DATA,
                                 params->initial_max_data);
    }

    if (params->initial_max_stream_data_bidi_local) {
        p = xqc_put_varint_param(p, XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL,
                                 params->initial_max_stream_data_bidi_local);
    }

    if (params->initial_max_stream_data_bidi_remote) {
        p = xqc_put_varint_param(p, XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE,
                                 params->initial_max_stream_data_bidi_remote);
    }

    if (params->initial_max_stream_data_uni) {
        p = xqc_put_varint_param(p, XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_UNI,
                                 params->initial_max_stream_data_uni);
    }

    if (params->initial_max_streams_bidi) {
        p = xqc_put_varint_param(p, XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_BIDI,
                                 params->initial_max_streams_bidi);
    }

    if (params->initial_max_streams_uni) {
        p = xqc_put_varint_param(p, XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_UNI,
                                 params->initial_max_streams_uni);
    }

    if (params->ack_delay_exponent != XQC_DEFAULT_ACK_DELAY_EXPONENT) {
        p = xqc_put_varint_param(p, XQC_TRANSPORT_PARAM_ACK_DELAY_EXPONENT,
                                 params->ack_delay_exponent);
    }

    if (params->max_ack_delay != XQC_DEFAULT_MAX_ACK_DELAY) {
        p = xqc_put_varint_param(p, XQC_TRANSPORT_PARAM_MAX_ACK_DELAY,
                                 params->max_ack_delay);
    }

    if ((size_t)(p - out) != len) {
        return -XQC_EILLTP;
    }

    *out_len = len;
    return XQC_OK;
}

/* dst should be destination value point */
#define XQC_DECODE_VINT_VALUE(dst, p, end)                  \
    do {                                                    \
        ssize_t nread = xqc_vint_read((p), (end), (dst));   \
        if (nread < 0) {                                    \
            return -XQC_EILLTP;      \
        }                                                   \
        return XQC_OK;                                      \
    } while(0)

static xqc_int_t
xqc_decode_max_idle_timeout(xqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    XQC_DECODE_VINT_VALUE(&params->max_idle_timeout, p, end);
}

static xqc_int_t
xqc_decode_max_udp_payload_size(xqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    XQC_DECODE_VINT_VALUE(&params->max_udp_payload_size, p, end);
}

static xqc_int_t
xqc_decode_initial_max_data(xqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    XQC_DECODE_VINT_VALUE(&params->initial_max_data, p, end);
}

static xqc_int_t
xqc_decode_initial_max_stream_data_bidi_local(xqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    XQC_DECODE_VINT_VALUE(&params->initial_max_stream_data_bidi_local, p, end);
}

static xqc_int_t
xqc_decode_initial_max_stream_data_bidi_remote(xqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    XQC_DECODE_VINT_VALUE(&params->initial_max_stream_data_bidi_remote, p, end);
}

static xqc_int_t
xqc_decode_initial_max_stream_data_uni(xqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    XQC_DECODE_VINT_VALUE(&params->initial_max_stream_data_uni, p, end);
}

static xqc_int_t
xqc_decode_initial_max_streams_bidi(xqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    XQC_DECODE_VINT_VALUE(&params->initial_max_streams_bidi, p, end);
}

static xqc_int_t
xqc_decode_initial_max_streams_uni(xqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    XQC_DECODE_VINT_VALUE(&params->initial_max_streams_uni, p, end);
}

static xqc_int_t
xqc_decode_ack_delay_exponent(xqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    ssize_t nread = xqc_vint_read(p, end, &params->ack_delay_exponent);
    /* [TRANSPORT] Values above 20 are invalid */
    if (nread < 0 || params->ack_delay_exponent > XQC_MAX_ACK_DELAY_EXPONENT) {
        return -XQC_EILLTP;
    }
    return XQC_OK;
}

static xqc_int_t
xqc_decode_max_ack_delay(xqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len)
{
    XQC_DECODE_VINT_VALUE(&params->max_ack_delay, p, end);
}

typedef enum {
    XQC_TP_DECODER_MAX_IDLE_TIMEOUT            = 0x0000,
    XQC_TP_DECODER_MAX_UDP_PAYLOAD_SIZE                ,
    XQC_TP_DECODER_INITIAL_MAX_DATA                    ,
    XQC_TP_DECODER_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL  ,
    XQC_TP_DECODER_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE ,
    XQC_TP_DECODER_INITIAL_MAX_STREAM_DATA_UNI         ,
    XQC_TP_DECODER_INITIAL_MAX_STREAMS_BIDI            ,
    XQC_TP_DECODER_INITIAL_MAX_STREAMS_UNI             ,
    XQC_TP_DECODER_ACK_DELAY_EXPONENT                  ,
    XQC_TP_DECODER_MAX_ACK_DELAY                       ,
    XQC_TP_DECODER_UNKNOWN
} xqc_tp_decoder_index_t;

/* decode value from p, and store value in the input params */
typedef xqc_int_t (*xqc_trans_param_decode_func)(
    xqc_transport_params_t *params,
    const uint8_t *p, const uint8_t *end, uint64_t param_type, uint64_t param_len);

xqc_trans_param_decode_func xqc_trans_param_decode_func_list[] = {
    xqc_decode_max_idle_timeout,
    xqc_decode_max_udp_payload_size,
    xqc_decode_initial_max_data,
    xqc_decode_initial_max_stream_data_bidi_local,
    xqc_decode_initial_max_stream_data_bidi_remote,
    xqc_decode_initial_max_stream_data_uni,
    xqc_decode_initial_max_streams_bidi,
    xqc_decode_initial_max_streams_uni,
    xqc_decode_ack_delay_exponent,
    xqc_decode_max_ack_delay,
};

/* convert param_type to param's index in xqc_trans_param_decode_func_list */
uint64_t
xqc_trans_param_get_index(uint64_t param_type)
{
    switch (param_type) {
    case XQC_TRANSPORT_PARAM_MAX_IDLE_TIMEOUT:
    case XQC_TRANSPORT_PARAM_MAX_UDP_PAYLOAD_SIZE:
    case XQC_TRANSPORT_PARAM_INITIAL_MAX_DATA:
    case XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_LOCAL:
    case XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_BIDI_REMOTE:
    case XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAM_DATA_UNI:
    case XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_BIDI:
    case XQC_TRANSPORT_PARAM_INITIAL_MAX_STREAMS_UNI:
    case XQC_TRANSPORT_PARAM_ACK_DELAY_EXPONENT:
    case XQC_TRANSPORT_PARAM_MAX_ACK_DELAY:
        return (xqc_tp_decoder_index_t)param_type;

    default:
        break;
    }

    return XQC_TP_DECODER_UNKNOWN;
}

/**
 * decode one param
 */
static inline xqc_int_t
xqc_decode_one_transport_param(xqc_transport_params_t *params,
    const uint8_t **start, const uint8_t *end)
{
    const uint8_t *p = *start;
    uint64_t param_type = 0;
    uint64_t param_len = 0;

    /* read param type */
    ssize_t nread = xqc_vint_read(p, end, &param_type);
    if (nread < 0) {
        return -XQC_EILLTP;
    }
    p += nread;

    /* read param len */
    nread = xqc_vint_read(p, end, &param_len);
    if (nread < 0 || p + nread + param_len > end ) {
        return -XQC_EILLTP;
    }
    p += nread;

    /*
     * read param value, note: some parameters are allowed to be zero-length.
     */
    uint64_t param_index = xqc_trans_param_get_index(param_type);
    if (param_index != XQC_TP_DECODER_UNKNOWN) {
        xqc_int_t ret = xqc_trans_param_decode_func_list[param_index](params, p, end,
                                                                      param_type, param_len);
        if (ret < 0) {
            return -XQC_EILLTP;
        }
    }

    p += param_len;

    *start = p;
    return XQC_OK;
}

xqc_int_t
xqc_decode_transport_params(xqc_transport_params_t *params,
    const uint8_t *in, size_t in_len)
{
    const uint8_t *p, *end;
    xqc_int_t ret = XQC_OK;

    p = in;
    end = in + in_len;

    /* Set default values */
    params->max_idle_timeout = 0;
    params->max_udp_payload_size = XQC_DEFAULT_MAX_UDP_PAYLOAD_SIZE;
    params->initial_max_data = 0;
    params->initial_max_streams_bidi = 0;
    params->initial_max_streams_uni = 0;
    params->initial_max_stream_data_bidi_local = 0;
    params->initial_max_stream_data_bidi_remote = 0;
    params->initial_max_stream_data_uni = 0;
    params->ack_delay_exponent = XQC_DEFAULT_ACK_DELAY_EXPONENT;
    params->max_ack_delay = XQC_DEFAULT_MAX_ACK_DELAY;

    while (p < end) {
        ret = xqc_decode_one_transport_param(params, &p, end);
        if (ret < 0) {
            return ret;
        }
    }

    if (end != p) {
        return -XQC_EILLTP;
    }

    return XQC_OK;
}

void
xqc_init_transport_params(xqc_transport_params_t *params)
{
    xqc_memzero(params, sizeof(xqc_transport_params_t));

    /* transport parameter related attributes */
    params->max_ack_delay = XQC_DEFAULT_MAX_ACK_DELAY;
    params->ack_delay_exponent = XQC_DEFAULT_ACK_DELAY_EXPONENT;
    params->max_udp_payload_size = XQC_DEFAULT_MAX_UDP_PAYLOAD_SIZE;
}
