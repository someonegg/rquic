/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_CID_H_INCLUDED_
#define _RQC_CID_H_INCLUDED_

#include <rquic/rquic_typedef.h>
#include "src/common/rqc_list.h"

#define RQC_DEFAULT_CID_LEN 8

typedef enum {
    RQC_CID_UNUSED,
    RQC_CID_USED,
    RQC_CID_RETIRED,
    RQC_CID_REMOVED,
} rqc_cid_state_t;

typedef enum {
    RQC_CID_SET_UNUSED,
    RQC_CID_SET_USED,
    RQC_CID_SET_ABANDONED,
    RQC_CID_SET_MAX_STATE,
} rqc_cid_set_state_t;

typedef enum {
    RQC_CID_UNACKED = 0,
    RQC_CID_ACKED   = 1
} rqc_cid_flag_t;

typedef struct rqc_cid_inner_s {
    rqc_list_head_t   list;
    rqc_cid_t         cid;
    rqc_cid_state_t   state;
    rqc_usec_t        retired_ts;
    rqc_cid_flag_t    acked;
} rqc_cid_inner_t;

typedef struct rqc_cid_set_inner_s {
    rqc_list_head_t     next;    /* a list of cid inner structures */
    rqc_list_head_t     cid_list;
    uint64_t            unused_cnt;
    uint64_t            used_cnt;
    uint64_t            retired_cnt;
    uint64_t            path_id;
    union {
    uint64_t            largest_scid_seq_num;    /* for scid set */
    uint64_t            largest_retire_prior_to; /* for dcid set */
    };
    rqc_cid_set_state_t set_state;
    uint32_t            acked_unused;
} rqc_cid_set_inner_t;

typedef struct rqc_cid_set_s {
    rqc_list_head_t   cid_set_list; /* a list of rqc_cid_set_inner_t */
    union {
    unsigned char     original_scid_str[RQC_MAX_CID_LEN * 2 + 1];
    unsigned char     current_dcid_str[RQC_MAX_CID_LEN * 2 + 1];
    };
    union {
    rqc_cid_t         user_scid;    /* one of the USED SCIDs, for create/close notify */
    rqc_cid_t         current_dcid; /* one of the USED DCIDs, for send packets */
    };
    uint32_t          set_cnt[RQC_CID_SET_MAX_STATE];
} rqc_cid_set_t;

rqc_int_t rqc_generate_cid(rqc_engine_t *engine, rqc_cid_t *ori_cid, rqc_cid_t *cid,
    uint64_t cid_seq_num);

void rqc_cid_copy(rqc_cid_t *dst, rqc_cid_t *src);
void rqc_cid_init_zero(rqc_cid_t *cid);
void rqc_cid_set(rqc_cid_t *cid, const unsigned char *data, uint8_t len);

void rqc_init_cid_set(rqc_cid_set_t *cid_set);
void rqc_destroy_cid_set(rqc_cid_set_t *cid_set);

rqc_int_t rqc_cid_set_insert_cid(rqc_cid_set_t *cid_set, rqc_cid_t *cid,
    rqc_cid_state_t state, uint64_t limit, uint64_t path_id);
rqc_int_t rqc_cid_set_delete_cid(rqc_cid_set_t *cid_set,
    rqc_cid_t *cid, uint64_t path_id);

rqc_cid_inner_t *rqc_get_inner_cid_by_seq(rqc_cid_set_t *cid_set,
    uint64_t seq_num, uint64_t path_id);
rqc_cid_inner_t *rqc_cid_in_cid_set(rqc_cid_set_t *cid_set,
    rqc_cid_t *cid, uint64_t path_id);
rqc_cid_inner_t *rqc_cid_set_search_cid(rqc_cid_set_t *cid_set,
    rqc_cid_t *cid);

rqc_int_t rqc_cid_switch_to_next_state(rqc_cid_set_t *cid_set,
    rqc_cid_inner_t *cid, rqc_cid_state_t state, uint64_t path_id);

rqc_int_t rqc_get_unused_cid(rqc_cid_set_t *cid_set,
    rqc_cid_t *cid, uint64_t path_id);

void rqc_cid_set_inner_init(rqc_cid_set_inner_t *cid_set_inner);
void rqc_cid_set_inner_destroy(rqc_cid_set_inner_t *cid_set_inner);
rqc_cid_set_inner_t* rqc_get_path_cid_set(rqc_cid_set_t *cid_set, uint64_t path_id);
int64_t rqc_cid_set_get_unused_cnt(rqc_cid_set_t *cid_set, uint64_t path_id);
int64_t rqc_cid_set_get_used_cnt(rqc_cid_set_t *cid_set, uint64_t path_id);\
int64_t rqc_cid_set_get_retired_cnt(rqc_cid_set_t *cid_set, uint64_t path_id);
int64_t rqc_cid_set_get_largest_seq_or_rpt(rqc_cid_set_t *cid_set, uint64_t path_id);
rqc_int_t rqc_cid_set_set_largest_seq_or_rpt(rqc_cid_set_t *cid_set, uint64_t path_id, uint64_t val);

rqc_int_t rqc_cid_set_add_path(rqc_cid_set_t *cid_set, uint64_t path_id);

void rqc_cid_set_update_state(rqc_cid_set_t *cid_set, uint64_t path_id, rqc_cid_set_state_t state);

rqc_cid_set_inner_t* rqc_get_next_unused_path_cid_set(rqc_cid_set_t *cid_set);
void rqc_cid_set_on_cid_acked(rqc_cid_set_t *cid_set, uint64_t path_id, uint64_t cid_seq);

#endif /* _RQC_CID_H_INCLUDED_ */
