#ifndef _RQC_TIMER_H_INCLUDED_
#define _RQC_TIMER_H_INCLUDED_

#include "src/common/rqc_time.h"
#include "src/common/rqc_common_inc.h"
#include "src/transport/rqc_packet.h"
#include "src/transport/rqc_packet_in.h"
#include "src/common/rqc_list.h"

/*
 * A connection will time out if no packets are sent or received for a
 * period longer than the time specified in the max_idle_timeout transport
 * parameter (see Section 10).  However, state in middleboxes might time
 * out earlier than that.  Though REQ-5 in [RFC4787] recommends a 2
 * minute timeout interval, experience shows that sending packets every
 * 15 to 30 seconds is necessary to prevent the majority of middleboxes
 * from losing state for UDP flows.
 */
#define RQC_PING_TIMEOUT                    15000

typedef enum rqc_timer_level {
    RQC_PATH_LEVEL_TIMER,
    RQC_CONN_LEVEL_TIMER,
} rqc_timer_level_t;

/* !!warning add to timer_type_2_str */
typedef enum rqc_timer_type {

    /* path level (path->path_send_ctl->path_timer_manager->timer[RQC_TIMER_N])*/
    RQC_TIMER_ACK,
    RQC_TIMER_LOSS_DETECTION,
    RQC_TIMER_PACING,
    RQC_TIMER_NAT_REBINDING,

    /* connection level (conn->conn_timer_manager->timer[RQC_TIMER_N]) */
    RQC_TIMER_CONN_IDLE,
    RQC_TIMER_CONN_DRAINING,
    RQC_TIMER_STREAM_CLOSE,
    RQC_TIMER_PING,
    RQC_TIMER_LINGER_CLOSE,

    RQC_TIMER_N,

} rqc_timer_type_t;

typedef int32_t rqc_gp_timer_id_t;

#define RQC_GP_TIMER_ID_MAX (0x7fffffff)

/* timer timeout callback */
typedef void (*rqc_timer_timeout_pt)(rqc_timer_type_t type, rqc_usec_t now, void *user_data);

typedef void (*rqc_gp_timer_timeout_pt)(rqc_gp_timer_id_t gp_timer_id, rqc_usec_t now, void *user_data);

typedef struct rqc_timer_s {
    uint8_t                     timer_is_set;
    rqc_usec_t                  expire_time;

    /* callback function and user_data */
    rqc_timer_timeout_pt        timeout_cb;
    void                       *user_data;
} rqc_timer_t;

/* general purpose timer */
typedef struct rqc_gp_timer_s {
    rqc_list_head_t             list;
    rqc_gp_timer_id_t           id;
    rqc_bool_t                  timer_is_set;
    rqc_usec_t                  expire_time;

    /* callback function and user_data */
    rqc_gp_timer_timeout_pt     timeout_cb;
    void                       *user_data;
    char                       *name;
} rqc_gp_timer_t;

typedef struct rqc_timer_manager_s {
    rqc_timer_t                 timer[RQC_TIMER_N];
    rqc_log_t                  *log;
    /* general purpose timer */
    rqc_list_head_t             gp_timer_list;
    rqc_gp_timer_id_t           next_gp_timer_id;
} rqc_timer_manager_t;

/* APIs for gp timer */
rqc_gp_timer_id_t rqc_timer_register_gp_timer(rqc_timer_manager_t *manager,
    char *timer_name, rqc_gp_timer_timeout_pt cb, void *user_data);

rqc_int_t rqc_timer_unregister_gp_timer(rqc_timer_manager_t *manager, rqc_gp_timer_id_t gp_timer_id);

void rqc_timer_destroy_gp_timer(rqc_gp_timer_t *gp_timer);

void rqc_timer_destroy_gp_timer_list(rqc_timer_manager_t *manager);

static inline rqc_int_t
rqc_timer_gp_timer_set(rqc_timer_manager_t *manager, rqc_gp_timer_id_t gp_timer_id, rqc_usec_t expire_time)
{
    if (!manager || gp_timer_id >= manager->next_gp_timer_id) {
        return -RQC_EPARAM;
    }

    rqc_list_head_t *pos, *next;
    rqc_gp_timer_t *gp_timer;

    rqc_list_for_each_safe(pos, next, &manager->gp_timer_list) {
        gp_timer = rqc_list_entry(pos, rqc_gp_timer_t, list);
        if (gp_timer->id == gp_timer_id) {
            gp_timer->expire_time = expire_time;
            gp_timer->timer_is_set = RQC_TRUE;
            return RQC_OK;
        }
    }
    return RQC_ERROR;
}

static inline rqc_int_t
rqc_timer_gp_timer_unset(rqc_timer_manager_t *manager, rqc_gp_timer_id_t gp_timer_id)
{
    if (!manager || gp_timer_id >= manager->next_gp_timer_id) {
        return -RQC_EPARAM;
    }

    rqc_list_head_t *pos, *next;
    rqc_gp_timer_t *gp_timer;

    rqc_list_for_each_safe(pos, next, &manager->gp_timer_list) {
        gp_timer = rqc_list_entry(pos, rqc_gp_timer_t, list);
        if (gp_timer->id == gp_timer_id) {
            gp_timer->expire_time = 0;
            gp_timer->timer_is_set = RQC_FALSE;
            return RQC_OK;
        }
    }
    return RQC_ERROR;
}

static inline rqc_int_t
rqc_timer_gp_timer_get_info(rqc_timer_manager_t *manager, rqc_gp_timer_id_t gp_timer_id, rqc_bool_t *is_set, rqc_usec_t *expire_time)
{
    if (!manager || gp_timer_id >= manager->next_gp_timer_id) {
        return -RQC_EPARAM;
    }

    rqc_list_head_t *pos, *next;
    rqc_gp_timer_t *gp_timer;

    rqc_list_for_each_safe(pos, next, &manager->gp_timer_list) {
        gp_timer = rqc_list_entry(pos, rqc_gp_timer_t, list);
        if (gp_timer->id == gp_timer_id) {
            *is_set = gp_timer->timer_is_set;
            *expire_time = gp_timer->expire_time;
            return RQC_OK;
        }
    }
    return RQC_ERROR;
}

const char *rqc_timer_type_2_str(rqc_timer_type_t timer_type);

void rqc_timer_init(rqc_timer_manager_t *manager, rqc_log_t *log, void *user_data);

static inline int
rqc_timer_is_set(rqc_timer_manager_t *manager, rqc_timer_type_t type)
{
    return manager->timer[type].timer_is_set;
}

static inline void
rqc_timer_set(rqc_timer_manager_t *manager, rqc_timer_type_t type, rqc_usec_t now, rqc_usec_t inter_time)
{
    manager->timer[type].timer_is_set = 1;
    manager->timer[type].expire_time = now + inter_time;
    rqc_log_event(manager->log, REC_LOSS_TIMER_UPDATED, manager, inter_time, (rqc_int_t) type, (rqc_int_t) RQC_LOG_TIMER_SET);
}

static inline void
rqc_timer_unset(rqc_timer_manager_t *manager, rqc_timer_type_t type)
{
    manager->timer[type].timer_is_set = 0;
    manager->timer[type].expire_time = 0;
    rqc_log_event(manager->log, REC_LOSS_TIMER_UPDATED, manager, 0, (rqc_int_t) type, (rqc_int_t) RQC_LOG_TIMER_CANCEL);
}

static inline void
rqc_timer_update(rqc_timer_manager_t *manager, rqc_timer_type_t type, rqc_usec_t now, rqc_usec_t inter_time)
{
    rqc_usec_t new_expire = now + inter_time;
    if (new_expire - manager->timer[type].expire_time < 1000) {
        return;
    }

    int was_set = manager->timer[type].timer_is_set;

    if (was_set) {
        /* update */
        manager->timer[type].expire_time = new_expire;
    } else {
        rqc_timer_set(manager, type, now, inter_time);
    }
}

static inline void
rqc_timer_expire(rqc_timer_manager_t *manager, rqc_usec_t now)
{
    rqc_timer_t *timer;
    for (rqc_timer_type_t type = 0; type < RQC_TIMER_N; ++type) {
        timer = &manager->timer[type];
        if (timer->timer_is_set && timer->expire_time <= now) {
            rqc_log_event(manager->log, REC_LOSS_TIMER_UPDATED, manager, 0, (rqc_int_t) type, (rqc_int_t) RQC_LOG_TIMER_EXPIRE);

            timer->timeout_cb(type, now, timer->user_data);

            /* unset timer if it is not updated in timeout_cb */
            if (timer->expire_time <= now) {
                rqc_timer_unset(manager, type);
            }
        }
    }

    /* expire gp timer */
    rqc_list_head_t *pos, *next;
    rqc_gp_timer_t *gp_timer;

    rqc_list_for_each_safe(pos, next, &manager->gp_timer_list) {
        gp_timer = rqc_list_entry(pos, rqc_gp_timer_t, list);
        if (gp_timer->timer_is_set && gp_timer->expire_time <= now) {
            gp_timer->timeout_cb(gp_timer->id, now, gp_timer->user_data);
            if (gp_timer->expire_time <= now) {
                rqc_timer_gp_timer_unset(manager, gp_timer->id);
            }
        }
    }
}

/*
 * *****************TIMER END*****************
 */

#endif /* _RQC_TIMER_H_INCLUDED_ */
