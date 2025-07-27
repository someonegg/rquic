/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include <rquic/rquic.h>
#include "src/transport/rqc_engine.h"
#include "src/common/rqc_str.h"
#include "src/common/rqc_random.h"
#include "src/common/rqc_priority_q.h"
#include "src/common/rqc_str_hash.h"
#include "src/common/rqc_hash.h"
#include "src/transport/rqc_defs.h"
#include "src/transport/rqc_conn.h"
#include "src/transport/rqc_send_ctl.h"
#include "src/transport/rqc_stream.h"
#include "src/transport/rqc_packet_parser.h"
#include "src/transport/rqc_frame_parser.h"
#include "src/transport/rqc_packet_in.h"
#include "src/transport/rqc_packet.h"
#include "src/transport/rqc_cid.h"
#include "src/transport/rqc_utils.h"
#include "src/transport/rqc_timer.h"
#include "src/transport/rqc_packet_out.h"

rqc_config_t default_client_config = {
    .cfg_log_level             = RQC_LOG_WARN,
    .cfg_log_event             = 1,
    .cfg_qlog_importance       = EVENT_IMPORTANCE_SELECTED,
    .cfg_log_timestamp         = 1,
    .cfg_log_level_name        = 1,
    .conn_pool_size            = 4096,
    .streams_hash_bucket_size  = 1024,
    .conns_hash_bucket_size    = 1024,
    .hash_conflict_threshold   = RQC_HASH_DEFAULT_CONFLICT_THRESHOLD,
    .conns_active_pq_capacity  = 128,
    .conns_wakeup_pq_capacity  = 128,
    .support_version_count     = 1,
    .support_version_list[0]   = RQC_VERSION_V1_VALUE,
    .cid_len                   = RQC_DEFAULT_CID_LEN,
    .cid_negotiate             = 1,
    .sendmmsg_on               = 0,
    .manually_triggered_send   = 0,
};

rqc_config_t default_server_config = {
    .cfg_log_level             = RQC_LOG_WARN,
    .cfg_log_event             = 1,
    .cfg_qlog_importance       = EVENT_IMPORTANCE_SELECTED,
    .cfg_log_timestamp         = 1,
    .cfg_log_level_name        = 1,
    .conn_pool_size            = 4096,
    .streams_hash_bucket_size  = 1024,
    .conns_hash_bucket_size    = 1024*1024, /* too many connections will affect lookup performance */
    .hash_conflict_threshold   = RQC_HASH_DEFAULT_CONFLICT_THRESHOLD,
    .conns_active_pq_capacity  = 1024,
    .conns_wakeup_pq_capacity  = 16*1024,
    .support_version_count     = 2,
    .support_version_list      = {RQC_VERSION_V1_VALUE, RQC_IDRAFT_VER_29_VALUE},
    .cid_len                   = RQC_DEFAULT_CID_LEN,
    .cid_negotiate             = 0,
    .sendmmsg_on               = 0,
    .manually_triggered_send   = 0,
};

void
rqc_engine_free_alpn_list(rqc_engine_t *engine);

rqc_int_t
rqc_set_config(rqc_config_t *dst, const rqc_config_t *src)
{
    if (src->conn_pool_size > 0) {
        dst->conn_pool_size = src->conn_pool_size;
    }

    if (src->streams_hash_bucket_size > 0) {
        dst->streams_hash_bucket_size = src->streams_hash_bucket_size;
    }

    if (src->conns_hash_bucket_size > 0) {
        dst->conns_hash_bucket_size = src->conns_hash_bucket_size;
    }
    if (src->hash_conflict_threshold > 0) {
        dst->hash_conflict_threshold = src->hash_conflict_threshold;
    }

    if (src->conns_active_pq_capacity > 0) {
        dst->conns_active_pq_capacity = src->conns_active_pq_capacity;
    }

    if (src->conns_wakeup_pq_capacity > 0) {
        dst->conns_wakeup_pq_capacity = src->conns_wakeup_pq_capacity;
    }

    if (src->support_version_count > 0 && src->support_version_count <= RQC_SUPPORT_VERSION_MAX) {
        dst->support_version_count = src->support_version_count;
        for (int i = 0; i < src->support_version_count; ++i) {
            dst->support_version_list[i] = src->support_version_list[i];
        }

    } else if (src->support_version_count > RQC_SUPPORT_VERSION_MAX) {
        return RQC_ERROR;
    }

    if (src->cid_len > 0 && src->cid_len <= RQC_MAX_CID_LEN) {
        dst->cid_len = src->cid_len;

    } else if (src->cid_len > RQC_MAX_CID_LEN) {
        return RQC_ERROR;
    }

    dst->cid_negotiate = src->cid_negotiate;
    dst->cfg_log_level = src->cfg_log_level;
    dst->cfg_log_event = src->cfg_log_event;
    dst->cfg_qlog_importance = src->cfg_qlog_importance;
    dst->cfg_log_timestamp = src->cfg_log_timestamp;
    dst->cfg_log_level_name = src->cfg_log_level_name;
    dst->sendmmsg_on = src->sendmmsg_on;

    return RQC_OK;
}

rqc_int_t
rqc_engine_get_default_config(rqc_config_t *config, rqc_engine_type_t engine_type)
{
    if (engine_type == RQC_ENGINE_SERVER) {
        return rqc_set_config(config, &default_server_config);

    } else {
        return rqc_set_config(config, &default_client_config);
    }
}

rqc_int_t
rqc_engine_set_config(rqc_engine_t *engine, const rqc_config_t *engine_config)
{
    return rqc_set_config(engine->config, engine_config);
}

rqc_config_t *
rqc_engine_config_create(rqc_engine_type_t engine_type)
{
    rqc_config_t *config = rqc_malloc(sizeof(rqc_config_t));
    if (config == NULL) {
        return NULL;
    }

    rqc_memzero(config, sizeof(rqc_config_t));

    if (engine_type == RQC_ENGINE_SERVER) {
        rqc_set_config(config, &default_server_config);

    } else if (engine_type == RQC_ENGINE_CLIENT) {
        rqc_set_config(config, &default_client_config);
    }

    return config;
}

void
rqc_engine_config_destroy(rqc_config_t *config)
{
    rqc_free(config);
}

void
rqc_engine_set_log_level(rqc_engine_t *engine, rqc_log_level_t log_level)
{
    rqc_log_level_set(engine->log, log_level);
}

rqc_str_hash_table_t *
rqc_engine_conns_hash_create(rqc_config_t *config, uint8_t *key, size_t key_len, rqc_log_t *log)
{
    rqc_str_hash_table_t *hash_table = rqc_malloc(sizeof(rqc_str_hash_table_t));
    if (hash_table == NULL) {
        return NULL;
    }

    if (rqc_str_hash_init(hash_table, rqc_default_allocator,
            config->conns_hash_bucket_size, config->hash_conflict_threshold,
            key, key_len, log))
    {
        goto fail;
    }

    return hash_table;

fail:
    rqc_free(hash_table);
    return NULL;
}

void
rqc_engine_conns_hash_destroy(rqc_str_hash_table_t *hash_table)
{
    rqc_str_hash_release(hash_table);
    rqc_free(hash_table);
}

int rqc_engine_conn_pq_operator(rqc_pq_t *pq, rqc_pq_element_t *e)
{
    rqc_connection_t **conn;
    conn = (rqc_connection_t**)e->data;
    if (conn && *conn) {
        (*conn)->wakeup_pq_index = rqc_pq_element_index(pq, e);
    }
    return RQC_OK;
}

rqc_pq_t *
rqc_engine_conns_pq_create(rqc_config_t *config, uint8_t is_wakeup)
{
    rqc_pq_t *q = rqc_malloc(sizeof(rqc_pq_t));
    if (q == NULL) {
        return NULL;
    }

    size_t capacity = is_wakeup == 1 ?
                      config->conns_wakeup_pq_capacity :
                      config->conns_active_pq_capacity;

    rqc_memzero(q, sizeof(rqc_pq_t));
    if (rqc_pq_init(q, sizeof(rqc_conns_pq_elem_t),
        capacity, rqc_default_allocator,
        rqc_pq_revert_cmp, rqc_engine_conn_pq_operator))
    {
        goto fail;
    }

    return q;

fail:
    rqc_pq_destroy(q);
    rqc_free(q);
    return NULL;
}

rqc_connection_t *
rqc_engine_conns_hash_find(rqc_engine_t *engine, const rqc_cid_t *cid, char type)
{
    if (cid == NULL || cid->cid_len == 0) {
        return NULL;
    }

    uint64_t hash;
    rqc_str_t str;
    str.data = (unsigned char *)cid->cid_buf;
    str.len = cid->cid_len;

    hash = rqc_siphash_get_hash(&engine->conns_hash->siphash_ctx, cid->cid_buf, cid->cid_len);
    return rqc_str_hash_find(engine->conns_hash, hash, str);
}

rqc_connection_t *
rqc_engine_get_conn_by_scid(rqc_engine_t *engine, const rqc_cid_t *cid)
{
    return rqc_engine_conns_hash_find(engine, cid, 's');
}

void
rqc_engine_conns_pq_destroy(rqc_pq_t *q)
{
    rqc_pq_destroy(q);
    rqc_free(q);
}

rqc_usec_t
rqc_engine_wakeup_after(rqc_engine_t *engine)
{
    rqc_conns_pq_elem_t *el = rqc_conns_pq_top(engine->conns_wait_wakeup_pq);
    if (el) {
        rqc_usec_t now = rqc_monotonic_timestamp();
        return el->time_us > now ? el->time_us - now : 1;
    }

    return 0;
}

void
rqc_engine_wakeup_once(rqc_engine_t *engine)
{
    /* if interval is smaller, trigger the event with the new interval */
    if (engine->eng_callback.set_event_timer) {
        engine->eng_callback.set_event_timer(1, engine->user_data);
    }
}

void
rqc_engine_set_callback(rqc_engine_t *engine, const rqc_engine_callback_t *engine_callback,
    const rqc_transport_callbacks_t *transport_cbs)
{
    engine->eng_callback = *engine_callback;
    engine->transport_cbs = *transport_cbs;

    if (engine_callback->realtime_ts) {
        rqc_realtime_timestamp = engine_callback->realtime_ts;
    }

    if (engine_callback->monotonic_ts) {
        rqc_monotonic_timestamp = engine_callback->monotonic_ts;
    }
}

/**
 * @brief check the legitimacy of engine config
 */
rqc_bool_t
rqc_engine_check_config(rqc_engine_type_t engine_type,
    const rqc_config_t *engine_config, const rqc_transport_callbacks_t *transport_cbs)
{
    /* mismatch of sendmmsg_on enable and write_mmsg callback function */
    if (engine_config && engine_config->sendmmsg_on && transport_cbs->write_mmsg == NULL) {
        return RQC_FALSE;
    }

    return RQC_TRUE;
}

/**
 * Create new rquic engine.
 * @param engine_type  RQC_ENGINE_SERVER or RQC_ENGINE_CLIENT
 */
rqc_engine_t *
rqc_engine_create(rqc_engine_type_t engine_type,
    const rqc_config_t *engine_config,
    const rqc_engine_callback_t *engine_callback,
    const rqc_transport_callbacks_t *transport_cbs,
    void *user_data)
{
    rqc_engine_t *engine = NULL;
    uint8_t sipkey[RQC_SIPHASH_KEY_SIZE];

    /* check input parameter */
    if (rqc_engine_check_config(engine_type, engine_config, transport_cbs)
        == RQC_FALSE)
    {
        return NULL;
    }

    engine = rqc_malloc(sizeof(rqc_engine_t));
    if (engine == NULL) {
        goto fail;
    }
    rqc_memzero(engine, sizeof(rqc_engine_t));

    engine->eng_type = engine_type;

    /* init alpn list */
    rqc_init_list_head(&engine->alpn_reg_list);

    engine->config = rqc_engine_config_create(engine_type);
    if (engine->config == NULL) {
        goto fail;
    }

    if (engine_config != NULL
        && rqc_engine_set_config(engine, engine_config) != RQC_OK)
    {
        goto fail;
    }

    rqc_engine_set_callback(engine, engine_callback, transport_cbs);
    engine->user_data = user_data;
    engine->log = rqc_log_init(engine->config->cfg_log_level,
                               engine->config->cfg_log_event,
                               engine->config->cfg_qlog_importance,
                               engine->config->cfg_log_timestamp,
                               engine->config->cfg_log_level_name, engine,
                               &engine->eng_callback.log_callbacks, engine->user_data);
    if (engine->log == NULL) {
        goto fail;
    }

    engine->rand_generator = rqc_random_generator_create(engine->log);
    if (engine->rand_generator == NULL) {
        goto fail;
    }
    rqc_get_random(engine->rand_generator, sipkey, sizeof(sipkey));

    engine->conns_hash = rqc_engine_conns_hash_create(engine->config, sipkey, sizeof(sipkey), engine->log);
    if (engine->conns_hash == NULL) {
        goto fail;
    }

    engine->conns_active_pq = rqc_engine_conns_pq_create(engine->config, 0);
    if (engine->conns_active_pq == NULL) {
        goto fail;
    }

    engine->conns_wait_wakeup_pq = rqc_engine_conns_pq_create(engine->config, 1);
    if (engine->conns_wait_wakeup_pq == NULL) {
        goto fail;
    }

    engine->default_conn_settings = internal_default_conn_settings;

    return engine;

fail:
    rqc_engine_destroy(engine);
    return NULL;
}

void
rqc_engine_destroy(rqc_engine_t *engine)
{
    rqc_connection_t *conn;

    if (engine == NULL) {
        return;
    }

    rqc_engine_free_alpn_list(engine);

    /* free destroy first, then destroy others */
    if (engine->conns_active_pq) {
        while (!rqc_pq_empty(engine->conns_active_pq)) {
            conn = rqc_conns_pq_pop_top_conn(engine->conns_active_pq);
            if (conn == NULL) {
                if (engine->log) {
                    rqc_log(engine->log, RQC_LOG_ERROR, "|NULL ptr, skip|");
                }
                continue;
            }

            conn->conn_flag &= ~RQC_CONN_FLAG_TICKING;
            /* active connections should never present in the wakeup queue */
            rqc_conn_destroy(conn);
        }
    }

    if (engine->conns_wait_wakeup_pq) {
        while (!rqc_pq_empty(engine->conns_wait_wakeup_pq)) {
            /* get conn from pq top and pop */
            conn = rqc_conns_pq_pop_top_conn(engine->conns_wait_wakeup_pq);
            if (conn == NULL) {
                if (engine->log) {
                    rqc_log(engine->log, RQC_LOG_ERROR, "|NULL ptr, skip|");
                }
                continue;
            }
            conn->conn_flag &= ~RQC_CONN_FLAG_WAIT_WAKEUP;
            rqc_conn_destroy(conn);
        }
    }

    if (engine->conns_active_pq) {
        rqc_engine_conns_pq_destroy(engine->conns_active_pq);
        engine->conns_active_pq = NULL;
    }

    if (engine->conns_wait_wakeup_pq) {
        rqc_engine_conns_pq_destroy(engine->conns_wait_wakeup_pq);
        engine->conns_wait_wakeup_pq = NULL;
    }

    if (engine->config) {
        rqc_engine_config_destroy(engine->config);
        engine->config = NULL;
    }

    if (engine->rand_generator) {
        rqc_random_generator_destroy(engine->rand_generator);
        engine->rand_generator = NULL;
    }

    if (engine->conns_hash) {
        rqc_engine_conns_hash_destroy(engine->conns_hash);
        engine->conns_hash = NULL;
    }

    if (engine->log) {
        rqc_log_release(engine->log);
    }

    rqc_free(engine);
}

#define RQC_CHECK_IMMEDIATE_CLOSE() do {                        \
    if (RQC_UNLIKELY(conn->conn_flag & RQC_CONN_IMMEDIATE_CLOSE_FLAGS)) {     \
        rqc_conn_immediate_close(conn);                         \
        goto end;                                               \
    }                                                           \
} while(0);                                                     \

void
rqc_engine_process_conn(rqc_connection_t *conn, rqc_usec_t now)
{
    int ret;

    rqc_conn_timer_expire(conn, now);

    /* notify closing event as soon as possible */
    rqc_conn_closing_notify(conn);

    if (RQC_UNLIKELY(conn->conn_flag & RQC_CONN_FLAG_TIME_OUT)) {
        conn->conn_state = RQC_CONN_STATE_CLOSED;
        rqc_log_event(conn->log, CON_CONNECTION_STATE_UPDATED, conn);
        return;
    }
    RQC_CHECK_IMMEDIATE_CLOSE();

    if (RQC_UNLIKELY(conn->conn_flag & RQC_CONN_FLAG_LINGER_CLOSING)) {
        if (rqc_send_queue_out_queue_empty(conn->conn_send_queue)) {
            rqc_conn_log(conn, RQC_LOG_INFO, "|out queue empty, close connection|");
            rqc_timer_unset(&conn->conn_timer_manager, RQC_TIMER_LINGER_CLOSE);
            rqc_conn_immediate_close(conn);
            conn->conn_flag &= ~RQC_CONN_FLAG_LINGER_CLOSING;
        }
        goto end;
    }

    if (RQC_UNLIKELY(conn->conn_state >= RQC_CONN_STATE_CLOSING)) {
        goto end;
    }

    if (RQC_UNLIKELY(!rqc_list_empty(&conn->conn_send_queue->sndq_buff_1rtt_packets)
        && rqc_conn_is_established(conn))) {
        rqc_conn_write_buffed_1rtt_packets(conn);
    }
    RQC_CHECK_IMMEDIATE_CLOSE();

    if (rqc_conn_is_established(conn)) {
        rqc_process_read_streams(conn);
        if (rqc_send_queue_can_write(conn->conn_send_queue)) {
            if (conn->conn_send_queue->sndq_full) {
                if (rqc_send_queue_release_enough_space(conn->conn_send_queue)) {
                    conn->conn_send_queue->sndq_full = RQC_FALSE;
                    rqc_process_write_streams(conn);
                }
            } else {
                rqc_process_write_streams(conn);
            }
        }
    }
    RQC_CHECK_IMMEDIATE_CLOSE();

    if (conn->ack_flag) {
        ret = rqc_write_ack_to_packets(conn);
        if (ret) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_write_ack_to_packets error|");
            RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
        }
    }
    RQC_CHECK_IMMEDIATE_CLOSE();

    if (RQC_UNLIKELY(conn->conn_flag & RQC_CONN_FLAG_PING)) {
        ret = rqc_conn_send_ping_internal(conn, NULL, RQC_FALSE);
        if (ret) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_conn_send_ping_internal error|");
            RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
        }
    }
    RQC_CHECK_IMMEDIATE_CLOSE();

    /* server send version negotiation */
    if (RQC_UNLIKELY(conn->conn_flag & RQC_CONN_FLAG_VERSION_NEGOTIATION)) {
        ret = rqc_conn_send_version_negotiation(conn);
        if (ret) {
            rqc_log(conn->log, RQC_LOG_ERROR, "|send version negotiation error|");
        }
    }

end:
    conn->packet_need_process_count = 0;
    conn->conn_flag &= ~RQC_CONN_FLAG_NEED_RUN;
    return;
}

void rqc_engine_finish_recv (rqc_engine_t *engine) {
    rqc_engine_main_logic_internal(engine);
}

void rqc_engine_finish_send (rqc_engine_t *engine) {
    rqc_engine_main_logic_internal(engine);
}

void rqc_engine_main_logic_internal(rqc_engine_t *engine) {
    if (engine->eng_flag & RQC_ENG_FLAG_NO_DESTROY) {
        return;
    }

    engine->eng_flag |= RQC_ENG_FLAG_NO_DESTROY;
    rqc_engine_main_logic(engine);
    engine->eng_flag &= ~RQC_ENG_FLAG_NO_DESTROY;
}

void
rqc_engine_conn_logic(rqc_engine_t *engine, rqc_connection_t *conn)
{
    if (engine->eng_flag & RQC_ENG_FLAG_RUNNING) {
        return;
    }

    engine->eng_flag |= RQC_ENG_FLAG_RUNNING;

    rqc_usec_t now = rqc_monotonic_timestamp();
    rqc_usec_t wake_after;
    rqc_engine_process_conn(conn, now);

    if (RQC_LIKELY(conn->conn_state != RQC_CONN_STATE_CLOSED)) {
        conn->last_ticked_time = now;
        rqc_conn_schedule_packets_to_paths(conn);

        if (rqc_engine_is_sendmmsg_on(engine, conn)) {
            rqc_conn_transmit_pto_probe_packets_batch(conn);
            rqc_conn_retransmit_lost_packets_batch(conn);
            rqc_conn_send_packets_batch(conn);

        } else {
            rqc_conn_transmit_pto_probe_packets(conn);
            rqc_conn_retransmit_lost_packets(conn);
            rqc_conn_send_packets(conn);
        }

        if (RQC_LIKELY(conn->conn_state != RQC_CONN_STATE_CLOSED)) {
            conn->next_tick_time = rqc_conn_next_wakeup_time(conn);
            if (RQC_LIKELY(conn->next_tick_time != 0)) {
                rqc_engine_remove_active_queue(engine, conn);
                rqc_engine_add_wakeup_queue(engine, conn);
                goto finish;
            }
        }
    }

    conn->next_tick_time = 0;
    rqc_engine_remove_active_queue(engine, conn);
    rqc_engine_add_wakeup_queue(engine, conn);

finish:
    if (!rqc_pq_empty(engine->conns_active_pq)) {
        /* If there are other acitve connections, we must wakeup immediately. */
        rqc_engine_wakeup_once(engine);

    } else {
        wake_after = rqc_engine_wakeup_after(engine);
        if (wake_after > 0) {
            engine->eng_callback.set_event_timer(wake_after, engine->user_data);
        }
    }

    engine->eng_flag &= ~RQC_ENG_FLAG_RUNNING;
    return;
}

/**
 * Process all connections
 */
void
rqc_engine_main_logic(rqc_engine_t *engine)
{
    if (engine->eng_flag & RQC_ENG_FLAG_RUNNING) {
        return;
    }
    engine->eng_flag |= RQC_ENG_FLAG_RUNNING;

    rqc_usec_t now = rqc_monotonic_timestamp();
    rqc_connection_t *conn;

    while (!rqc_pq_empty(engine->conns_wait_wakeup_pq)) {
        rqc_conns_pq_elem_t *el = rqc_conns_pq_top(engine->conns_wait_wakeup_pq);
        if (RQC_UNLIKELY(el == NULL || el->conn == NULL)) {
            rqc_log(engine->log, RQC_LOG_ERROR, "|wakeup|NULL ptr, skip|");
            rqc_conns_pq_pop(engine->conns_wait_wakeup_pq);    /* no push between top and pop */
            continue;
        }
        conn = el->conn;

        if (el->time_us <= now) {
            rqc_engine_remove_wakeup_queue(engine, conn);
            if (rqc_engine_add_active_queue(engine, conn) != RQC_OK) {
                rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_conns_pq_push error|");
                RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
            }

        } else {
            break;
        }
    }

    while (!rqc_pq_empty(engine->conns_active_pq)) {
        conn = rqc_conns_pq_pop_top_conn(engine->conns_active_pq);

        if (RQC_UNLIKELY(conn == NULL)) {
            rqc_log(engine->log, RQC_LOG_ERROR, "|active|NULL ptr, skip|");
            continue;
        }

        now = rqc_monotonic_timestamp();
        rqc_engine_process_conn(conn, now);

        if (RQC_LIKELY(conn->conn_state != RQC_CONN_STATE_CLOSED)) {
            conn->last_ticked_time = now;
            rqc_conn_schedule_packets_to_paths(conn);

            if (rqc_engine_is_sendmmsg_on(engine, conn)) {
                rqc_conn_transmit_pto_probe_packets_batch(conn);
                rqc_conn_retransmit_lost_packets_batch(conn);
                rqc_conn_send_packets_batch(conn);

            } else {
                rqc_conn_transmit_pto_probe_packets(conn);
                rqc_conn_retransmit_lost_packets(conn);
                rqc_conn_send_packets(conn);
            }

            if (RQC_LIKELY(conn->conn_state != RQC_CONN_STATE_CLOSED)) {
                conn->next_tick_time = rqc_conn_next_wakeup_time(conn);
                if (RQC_LIKELY(conn->next_tick_time != 0)) {
                    conn->conn_flag &= ~RQC_CONN_FLAG_TICKING;
                    rqc_engine_add_wakeup_queue(engine, conn);
                    continue;
                }
            }
        }

        /* conn should be destroyed ( closed or next_tick_time = 0) */
        conn->conn_flag &= ~RQC_CONN_FLAG_TICKING;
        if (!(engine->eng_flag & RQC_ENG_FLAG_NO_DESTROY)) {
            rqc_log(engine->log, RQC_LOG_INFO, "|conn:%p|%s|"
                    "conn_state:%ud|next_tick_time:%ui",
                    conn, rqc_conn_addr_str(conn),
                    conn->conn_state, conn->next_tick_time);
            rqc_conn_destroy(conn);

        } else {
            conn->next_tick_time = 0;
            rqc_engine_add_wakeup_queue(engine, conn);
        }
    }

    rqc_usec_t wake_after = rqc_engine_wakeup_after(engine);
    if (wake_after > 0) {
        engine->eng_callback.set_event_timer(wake_after, engine->user_data);
    }

    engine->eng_flag &= ~RQC_ENG_FLAG_RUNNING;

    return;
}

/**
 * Pass received UDP packet payload into rquic engine.
 * @param recv_time   UDP packet received time in microsecond
 */
rqc_int_t
rqc_engine_packet_process(rqc_engine_t *engine,
    const unsigned char *packet_in_buf, size_t packet_in_size,
    const struct sockaddr *local_addr, socklen_t local_addrlen,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    rqc_usec_t recv_time, void *user_data)
{
    rqc_int_t ret;
    rqc_connection_t *conn = NULL;
    rqc_cid_t dcid, scid;   /* dcid: cid of peer; scid: cid of endpoint */

    rqc_cid_init_zero(&dcid);
    rqc_cid_init_zero(&scid);

    /* reverse packet's dcid/scid to endpoint's scid/dcid */
    ret = rqc_packet_parse_cid(&scid, &dcid, engine->config->cid_len,
                               (unsigned char *)packet_in_buf, packet_in_size);
    if (RQC_UNLIKELY(ret != RQC_OK)) {
        rqc_log_event(engine->log, TRA_PACKET_DROPPED, "fail to parse cid", ret, "unknown", 0);
        return -RQC_EILLPKT;
    }

    conn = rqc_engine_conns_hash_find(engine, &scid, 's');

    /* can't find a connection by the cid from the packet */
    if (RQC_UNLIKELY(conn == NULL)) {
        if (RQC_PACKET_IS_LONG_HEADER(packet_in_buf)) {
            /* server creates connection when receiving a initial packet */
            if (engine->eng_type == RQC_ENGINE_SERVER
                && (RQC_PACKET_LONG_HEADER_GET_TYPE(packet_in_buf) == RQC_PTYPE_INIT)
                     && (local_addr != NULL && peer_addr != NULL))
            {
                conn = rqc_conn_server_create(engine, local_addr, local_addrlen,
                                            peer_addr, peer_addrlen, &dcid, &scid,
                                            &engine->default_conn_settings, user_data);
                rqc_log_event(engine->log, CON_SERVER_LISTENING, peer_addr, peer_addrlen);
                if (conn == NULL) {
                    rqc_log(engine->log, RQC_LOG_ERROR, "|fail to create connection|");
                    return -RQC_ECREATE_CONN;
                }
            }
        }
    }

    /* can't find a conneciton */
    if (NULL == conn) {
        return -RQC_ECONN_NFOUND;
    }

    if (RQC_UNLIKELY(conn->local_addrlen == 0)) {
        ret = rqc_memcpy_with_cap(conn->local_addr, sizeof(conn->local_addr),
                                  local_addr, local_addrlen);
        if (ret == RQC_OK) {
            conn->local_addrlen = local_addrlen;

        } else {
            rqc_log(conn->log, RQC_LOG_ERROR,
                    "|local addr too large|addr_len:%d|", (int)local_addrlen);
        }
        rqc_log_event(conn->log, CON_CONNECTION_STARTED, conn, RQC_LOG_LOCAL_EVENT);
    }

    /* NAT rebinding */
    if (engine->eng_type == RQC_ENGINE_SERVER
        && (peer_addr != NULL && peer_addrlen != 0)
        && !rqc_is_same_addr_as_any_path(conn, peer_addr))
    {
        rqc_path_ctx_t *path = conn->the_path;
        if ((path != NULL) && (path->path_state == RQC_PATH_STATE_ACTIVE)) {

            if ((path->rebinding_addrlen == 0)
                && !rqc_timer_is_set(&path->path_send_ctl->path_timer_manager, RQC_TIMER_NAT_REBINDING))
            {
                /* set rebinding_addr & send PATH_CHALLENGE */
                ret = rqc_memcpy_with_cap(path->rebinding_addr, sizeof(path->rebinding_addr),
                                          peer_addr, peer_addrlen);
                if (ret == RQC_OK) {
                    path->rebinding_addrlen = peer_addrlen;

                } else {
                    rqc_log(conn->log, RQC_LOG_ERROR,
                            "|REBINDING|peer addr too large|addr_len:%d|", (int)peer_addrlen);
                }

                ret = rqc_conn_send_path_challenge(conn, path);
                if (ret == RQC_OK) {
                    rqc_log(conn->log, RQC_LOG_INFO, "|REBINDING|path:%ui|send PATH_CHALLENGE|addr:%s|", path->path_id, rqc_path_addr_str(path));
                    path->rebinding_count++;
                    rqc_usec_t pto = rqc_conn_get_max_pto(conn);
                    rqc_timer_set(&path->path_send_ctl->path_timer_manager,
                                  RQC_TIMER_NAT_REBINDING, recv_time, 3 * pto);

                } else {
                    rqc_log(engine->log, RQC_LOG_ERROR, "|REBINDING|rqc_conn_send_path_challenge error|conn:%p|path:%ui|ret:%d|", conn, path->path_id, ret);
                    path->rebinding_addrlen = 0;
                }

            } else if ((path->rebinding_check_response == 0)
                       && rqc_is_same_addr(peer_addr, (struct sockaddr *)path->rebinding_addr))
            {
                /* PATH_RESPONSE recv from rebinding_addr */
                path->rebinding_check_response = 1;
                rqc_log(conn->log, RQC_LOG_INFO, "|REBINDING|path:%ui|recv_addr = rebinding_addr|check PATH_RESPONSE|", path->path_id);
            }
        }

    }

    /* process packets */
    ret = rqc_conn_process_packet(conn, packet_in_buf, packet_in_size, recv_time);

    conn->rcv_pkt_stats.conn_udp_pkts++;

    if (ret) {
        rqc_log(engine->log, RQC_LOG_ERROR, "|fail to process packets|conn:%p|ret:%d|", conn, ret);
        RQC_CONN_ERR(conn, TRA_FRAME_ENCODING_ERROR);
        goto after_process;
    }

    rqc_conn_process_packet_recved_path(conn, &scid, packet_in_size, recv_time);

    rqc_timer_set(&conn->conn_timer_manager, RQC_TIMER_CONN_IDLE,
                  recv_time, rqc_conn_get_idle_timeout(conn) * 1000);

after_process:
    rqc_engine_remove_wakeup_queue(engine, conn);

    if (rqc_engine_add_active_queue(engine, conn) != RQC_OK) {
        rqc_log(engine->log, RQC_LOG_ERROR, "|rqc_conns_pq_push error|conn:%p|", conn);
        RQC_CONN_ERR(conn, TRA_INTERNAL_ERROR);
        rqc_conn_destroy(conn);
        return -RQC_EFATAL;
    }

    /* main logic */
    if (++conn->packet_need_process_count >= RQC_MAX_PACKET_PROCESS_BATCH
        || conn->conn_err != 0 || conn->conn_flag & RQC_CONN_FLAG_NEED_RUN)
    {
        rqc_engine_main_logic_internal(engine);
        if (rqc_engine_conns_hash_find(engine, &scid, 's') == NULL) {
            /* to inform upper module when destroy connection in main logic  */
            return  -RQC_ECONN_NFOUND;
        }
    }

    return ret;
}

uint8_t
rqc_engine_config_get_cid_len(rqc_engine_t *engine)
{
    return engine->config->cid_len;
}

rqc_int_t
rqc_engine_add_alpn(rqc_engine_t *engine, const char *alpn, size_t alpn_len,
    rqc_app_proto_callbacks_t *ap_cbs, void *alp_ctx)
{
    rqc_alpn_registration_t *registration = rqc_calloc(1, sizeof(rqc_alpn_registration_t));
    if (NULL == registration) {
        rqc_log(engine->log, RQC_LOG_ERROR, "|create alpn registration error!");
        return -RQC_EMALLOC;
    }

    registration->alpn = rqc_malloc(alpn_len + 1);
    if (NULL == registration->alpn) {
        rqc_log(engine->log, RQC_LOG_ERROR, "|create alpn buffer error!");
        rqc_free(registration);
        return -RQC_EMALLOC;
    }

    rqc_init_list_head(&registration->head);
    rqc_memcpy(registration->alpn, alpn, alpn_len);
    registration->alpn[alpn_len] = '\0';
    registration->alpn_len = alpn_len;
    registration->ap_cbs = *ap_cbs;
    registration->alp_ctx = alp_ctx;

    rqc_list_add_tail(&registration->head, &engine->alpn_reg_list);

    rqc_log(engine->log, RQC_LOG_INFO, "|alpn registered|alpn:%s|", alpn);
    return RQC_OK;
}

rqc_int_t
rqc_engine_register_alpn(rqc_engine_t *engine, const char *alpn, size_t alpn_len,
    rqc_app_proto_callbacks_t *ap_cbs, void *alp_ctx)
{
    rqc_list_head_t *pos, *next;
    rqc_alpn_registration_t *alpn_reg;

    if (NULL == alpn || 0 == alpn_len || alpn_len > RQC_MAX_ALPN_LEN) {
        return -RQC_EPARAM;
    }

    /* check if alpn exists */
    rqc_list_for_each_safe(pos, next, &engine->alpn_reg_list) {
        alpn_reg = rqc_list_entry(pos, rqc_alpn_registration_t, head);
        if (alpn_len == alpn_reg->alpn_len
            && rqc_memcmp(alpn, alpn_reg->alpn, alpn_len) == 0)
        {
            /* if found registration, update */
            alpn_reg->ap_cbs = *ap_cbs;
            alpn_reg->alp_ctx = alp_ctx;
            return RQC_OK;
        }
    }

    /* not registered, add into alpn_reg_list */
    return rqc_engine_add_alpn(engine, alpn, alpn_len, ap_cbs, alp_ctx);
}

void*
rqc_engine_get_alpn_ctx(rqc_engine_t *engine, const char *alpn, size_t alpn_len)
{
    rqc_list_head_t *pos, *next;
    rqc_alpn_registration_t *alpn_reg;

    rqc_list_for_each_safe(pos, next, &engine->alpn_reg_list) {
        alpn_reg = rqc_list_entry(pos, rqc_alpn_registration_t, head);
        if (alpn_reg && alpn_len == alpn_reg->alpn_len
            && rqc_memcmp(alpn, alpn_reg->alpn, alpn_len) == 0)
        {
            return alpn_reg->alp_ctx;
        }
    }

    return NULL;
}

rqc_int_t
rqc_engine_unregister_alpn(rqc_engine_t *engine, const char *alpn, size_t alpn_len)
{
    rqc_list_head_t *pos, *next;
    rqc_alpn_registration_t *alpn_reg;

    rqc_list_for_each_safe(pos, next, &engine->alpn_reg_list) {
        alpn_reg = rqc_list_entry(pos, rqc_alpn_registration_t, head);
        if (alpn_reg && alpn_len == alpn_reg->alpn_len
            && rqc_memcmp(alpn, alpn_reg->alpn, alpn_len) == 0)
        {
            rqc_list_del(&alpn_reg->head);

            /* remove registration */
            if (alpn_reg->alpn) {
                rqc_free(alpn_reg->alpn);
            }

            rqc_free(alpn_reg);

            return RQC_OK;
        }
    }

    return -RQC_EALPN_NOT_REGISTERED;
}

rqc_int_t
rqc_engine_get_alpn_callbacks(rqc_engine_t *engine, const char *alpn, size_t alpn_len,
    rqc_app_proto_callbacks_t *cbs)
{
    rqc_list_head_t *pos, *next;
    rqc_alpn_registration_t *alpn_reg;

    if (NULL == alpn || 0 == alpn_len) {
        return -RQC_EPARAM;
    }

    rqc_list_for_each_safe(pos, next, &engine->alpn_reg_list) {
        alpn_reg = rqc_list_entry(pos, rqc_alpn_registration_t, head);
        if (alpn_len == alpn_reg->alpn_len
            && rqc_memcmp(alpn, alpn_reg->alpn, alpn_len) == 0)
        {
            /* if found registration, update */
            *cbs = alpn_reg->ap_cbs;
            return RQC_OK;
        }
    }

    return -RQC_EALPN_NOT_SUPPORTED;
}

void
rqc_engine_free_alpn_list(rqc_engine_t *engine)
{
    /* free alpn registrations */
    rqc_list_head_t *pos, *next;
    rqc_alpn_registration_t *alpn_reg;
    rqc_list_for_each_safe(pos, next, &engine->alpn_reg_list) {
        alpn_reg = rqc_list_entry(pos, rqc_alpn_registration_t, head);

        if (alpn_reg) {
            if (alpn_reg->alpn) {
                rqc_free(alpn_reg->alpn);
            }

            rqc_list_del(&alpn_reg->head);
            rqc_free(alpn_reg);
        }
    }
}

rqc_bool_t
rqc_engine_is_sendmmsg_on(rqc_engine_t *engine, rqc_connection_t *conn)
{
    return engine->config->sendmmsg_on
        && engine->transport_cbs.write_mmsg
        && (!conn->conn_settings.disable_send_mmsg);
}

rqc_int_t
rqc_engine_add_wakeup_queue(rqc_engine_t *engine, rqc_connection_t *conn)
{
    if (!(conn->conn_flag & (RQC_CONN_FLAG_WAIT_WAKEUP | RQC_CONN_FLAG_TICKING))) {
        if(rqc_conns_pq_push(engine->conns_wait_wakeup_pq,
                             conn, conn->next_tick_time) != RQC_OK)
        {
            return -RQC_EMALLOC;
        }
        conn->conn_flag |= RQC_CONN_FLAG_WAIT_WAKEUP;
    }
    return RQC_OK;
}

rqc_int_t
rqc_engine_remove_wakeup_queue(rqc_engine_t *engine, rqc_connection_t *conn)
{
    if ((conn->conn_flag & RQC_CONN_FLAG_WAIT_WAKEUP)) {
        rqc_conns_pq_remove(engine->conns_wait_wakeup_pq, conn);
        conn->conn_flag &= ~RQC_CONN_FLAG_WAIT_WAKEUP;
    }
    return RQC_OK;
}

rqc_int_t
rqc_engine_add_active_queue(rqc_engine_t *engine, rqc_connection_t *conn)
{
    rqc_int_t ret = RQC_OK;
    if (!(conn->conn_flag & (RQC_CONN_FLAG_WAIT_WAKEUP | RQC_CONN_FLAG_TICKING))) {
        ret = rqc_conns_pq_push(engine->conns_active_pq,
                                conn, conn->last_ticked_time);
        if (ret == 0) {
            conn->conn_flag |= RQC_CONN_FLAG_TICKING;
            ret = RQC_OK;
        }
    }
    return ret;
}

rqc_int_t
rqc_engine_remove_active_queue(rqc_engine_t *engine, rqc_connection_t *conn)
{
    if ((conn->conn_flag & RQC_CONN_FLAG_TICKING)) {
        rqc_conns_pq_remove(engine->conns_active_pq, conn);
        conn->conn_flag &= ~RQC_CONN_FLAG_TICKING;
    }
    return RQC_OK;
}
