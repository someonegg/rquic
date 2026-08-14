
/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_ENGINE_H_INCLUDED_
#define _RQC_ENGINE_H_INCLUDED_

#include <rquic/rquic_typedef.h>
#include <rquic/rquic.h>
#include "src/common/rqc_list.h"

typedef enum {
    RQC_ENG_FLAG_RUNNING    = 1 << 0,
    RQC_ENG_FLAG_NO_DESTROY = 1 << 1,
} rqc_engine_flag_t;

typedef struct rqc_alpn_registration_s {
    rqc_list_head_t             head;

    /* content of application layer protocol */
    char                       *alpn;

    /* length of alpn string */
    size_t                      alpn_len;

    /* Application-Layer-Protocol callback functions */
    rqc_app_proto_callbacks_t   ap_cbs;

    void                       *alp_ctx;

} rqc_alpn_registration_t;

typedef struct rqc_engine_s {
    /* for engine itself */
    rqc_engine_type_t               eng_type;
    rqc_engine_callback_t           eng_callback;
    rqc_engine_flag_t               eng_flag;

    /* for connections */
    rqc_config_t                   *config;
    rqc_str_hash_table_t           *conns_hash;             /* scid */
    rqc_pq_t                       *conns_active_pq;        /* In process */
    rqc_pq_t                       *conns_wait_wakeup_pq;   /* Need wakeup after next tick time */

    rqc_log_t                      *log;
    rqc_random_generator_t         *rand_generator;

    /* for user */
    void                           *user_data;

    /* last relative interval passed to set_event_timer */
    rqc_usec_t                      last_wake_after;

    /* callback functions for connection transport events */
    rqc_transport_callbacks_t       transport_cbs;

    /* list of rqc_alpn_registration_t */
    rqc_list_head_t                 alpn_reg_list;

    rqc_conn_settings_t             default_conn_settings;

    char                            scid_buf[RQC_MAX_CID_LEN * 2 + 1];
    char                            dcid_buf[RQC_MAX_CID_LEN * 2 + 1];
    char                            conn_flag_str_buf[1024];
    char                            frame_type_buf[128];
    char                            local_addr_str[INET6_ADDRSTRLEN];
    char                            peer_addr_str[INET6_ADDRSTRLEN];

} rqc_engine_t;

rqc_usec_t rqc_engine_wakeup_after(rqc_engine_t *engine);

/**
 * Create engine config.
 * @param engine_type  RQC_ENGINE_SERVER or RQC_ENGINE_CLIENT
 */
rqc_config_t *rqc_engine_config_create(rqc_engine_type_t engine_type);

void rqc_engine_config_destroy(rqc_config_t *config);

/**
 * @return > 0 : user should call rqc_engine_main_logic after N ms
 */
rqc_usec_t rqc_engine_wakeup_after(rqc_engine_t *engine);

void rqc_engine_wakeup_once(rqc_engine_t *engine);

rqc_connection_t *rqc_engine_conns_hash_find(rqc_engine_t *engine, const rqc_cid_t *cid, char type);

void rqc_engine_process_conn(rqc_connection_t *conn, rqc_usec_t now);

void rqc_engine_main_logic_internal(rqc_engine_t *engine);

void rqc_engine_conn_logic(rqc_engine_t *engine, rqc_connection_t *conn);

rqc_int_t rqc_engine_add_wakeup_queue(rqc_engine_t *engine, rqc_connection_t *conn);

rqc_int_t rqc_engine_remove_wakeup_queue(rqc_engine_t *engine, rqc_connection_t *conn);

rqc_int_t rqc_engine_add_active_queue(rqc_engine_t *engine, rqc_connection_t *conn);

rqc_int_t rqc_engine_remove_active_queue(rqc_engine_t *engine, rqc_connection_t *conn);

rqc_int_t rqc_engine_get_alpn_callbacks(rqc_engine_t *engine, const char *alpn,
    size_t alpn_len, rqc_app_proto_callbacks_t *cbs);

rqc_bool_t rqc_engine_is_sendmmsg_on(rqc_engine_t *engine, rqc_connection_t *conn);

#endif
