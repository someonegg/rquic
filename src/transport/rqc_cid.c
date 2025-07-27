/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include <rquic/rquic.h>
#include "src/transport/rqc_cid.h"
#include "src/transport/rqc_engine.h"
#include "src/transport/rqc_conn.h"
#include "src/common/rqc_random.h"

rqc_int_t
rqc_generate_cid(rqc_engine_t *engine, rqc_cid_t *ori_cid, rqc_cid_t *cid,
    uint64_t cid_seq_num)
{
    unsigned char *buf;
    ssize_t        len, written;

    cid->cid_seq_num = cid_seq_num;
    cid->cid_len = engine->config->cid_len;

    buf = cid->cid_buf;
    len = cid->cid_len;

    if (engine->eng_callback.cid_generate_cb) {
        written = engine->eng_callback.cid_generate_cb(ori_cid, buf, len, engine->user_data);
        if (written < RQC_OK) {
            rqc_log(engine->log, RQC_LOG_ERROR, "|generate cid failed [ret=%z]|", written);
            return -RQC_EGENERATE_CID;
        }
        buf += written;
        len -= written;
    }

    if (len > 0 && (rqc_get_random(engine->rand_generator, buf, len) != RQC_OK)) {
        return -RQC_EGENERATE_CID;
    }

    cid->path_id = RQC_INITIAL_PATH_ID;

    return RQC_OK;
}

rqc_int_t
rqc_cid_is_equal(const rqc_cid_t *dst, const rqc_cid_t *src)
{
    if (dst == NULL || src == NULL) {
        return RQC_ERROR;
    }

    if (dst->cid_len != src->cid_len) {
        return RQC_ERROR;
    }

    if (rqc_memcmp(dst->cid_buf, src->cid_buf, dst->cid_len)) {
        return RQC_ERROR;
    }

    return RQC_OK;
}

void
rqc_cid_copy(rqc_cid_t *dst, rqc_cid_t *src)
{
    dst->cid_len = src->cid_len;
    rqc_memcpy(dst->cid_buf, src->cid_buf, dst->cid_len);
    dst->cid_seq_num = src->cid_seq_num;
    dst->path_id = src->path_id;
}

void
rqc_cid_init_zero(rqc_cid_t *cid)
{
    cid->cid_len = 0;
    cid->cid_seq_num = 0;
    cid->path_id = 0;
}

void
rqc_cid_set(rqc_cid_t *cid, const unsigned char *data, uint8_t len)
{
    cid->cid_len = len;
    if (len) {
        rqc_memcpy(cid->cid_buf, data, len);
    }
}

unsigned char *
rqc_dcid_str(rqc_engine_t *engine, const rqc_cid_t *dcid)
{
    rqc_hex_dump(engine->dcid_buf, dcid->cid_buf, dcid->cid_len);
    engine->dcid_buf[dcid->cid_len * 2] = '\0';
    return engine->dcid_buf;
}

unsigned char *
rqc_scid_str(rqc_engine_t *engine, const rqc_cid_t *scid)
{
    rqc_hex_dump(engine->scid_buf, scid->cid_buf, scid->cid_len);
    engine->scid_buf[scid->cid_len * 2] = '\0';
    return engine->scid_buf;
}

unsigned char *
rqc_dcid_str_by_scid(rqc_engine_t *engine, const rqc_cid_t *scid)
{
    rqc_connection_t *conn;
    conn = rqc_engine_conns_hash_find(engine, scid, 's');
    if (!conn) {
        rqc_log(engine->log, RQC_LOG_ERROR, "|can not find connection|");
        return NULL;
    }

    rqc_hex_dump(conn->dcid_set.current_dcid_str, conn->dcid_set.current_dcid.cid_buf,
                 conn->dcid_set.current_dcid.cid_len);
    conn->dcid_set.current_dcid_str[conn->dcid_set.current_dcid.cid_len * 2] = '\0';

    return conn->dcid_set.current_dcid_str;
}

void
rqc_init_cid_set(rqc_cid_set_t *cid_set)
{
    rqc_memzero(cid_set, sizeof(rqc_cid_set_t));
    rqc_init_list_head(&cid_set->cid_set_list);
}

void
rqc_cid_set_inner_init(rqc_cid_set_inner_t *cid_set_inner)
{
    rqc_memzero(cid_set_inner, sizeof(rqc_cid_set_inner_t));
    rqc_init_list_head(&cid_set_inner->cid_list);
    rqc_init_list_head(&cid_set_inner->next);
}

void
rqc_cid_set_inner_destroy(rqc_cid_set_inner_t *cid_set_inner)
{
    rqc_cid_inner_t *cid = NULL;
    rqc_list_head_t *pos, *next;

    rqc_list_for_each_safe(pos, next, &cid_set_inner->cid_list) {
        cid = rqc_list_entry(pos, rqc_cid_inner_t, list);
        rqc_list_del(pos);
        rqc_free(cid);
    }

    rqc_cid_set_inner_init(cid_set_inner);
}

void
rqc_destroy_cid_set(rqc_cid_set_t *cid_set)
{
    rqc_cid_set_inner_t *cid_set_inner = NULL;
    rqc_list_head_t *pos, *next;

    rqc_list_for_each_safe(pos, next, &cid_set->cid_set_list) {
        cid_set_inner = rqc_list_entry(pos, rqc_cid_set_inner_t, next);
        rqc_list_del(pos);
        rqc_cid_set_inner_destroy(cid_set_inner);
        rqc_free(cid_set_inner);
    }

    rqc_init_cid_set(cid_set);
}

rqc_cid_set_inner_t*
rqc_get_path_cid_set(rqc_cid_set_t *cid_set, uint64_t path_id)
{
    rqc_cid_set_inner_t *cid_set_inner = NULL;
    rqc_list_head_t *pos, *next;

    rqc_list_for_each_safe(pos, next, &cid_set->cid_set_list) {
        cid_set_inner = rqc_list_entry(pos, rqc_cid_set_inner_t, next);
        if (cid_set_inner->path_id == path_id) {
            return cid_set_inner;
        }
    }

    return NULL;
}

rqc_cid_set_inner_t*
rqc_get_next_unused_path_cid_set(rqc_cid_set_t *cid_set)
{
    rqc_cid_set_inner_t *cid_set_inner = NULL;
    rqc_list_head_t *pos, *next;

    rqc_list_for_each_safe(pos, next, &cid_set->cid_set_list) {
        cid_set_inner = rqc_list_entry(pos, rqc_cid_set_inner_t, next);
        if (cid_set_inner->set_state == RQC_CID_SET_UNUSED) {
            return cid_set_inner;
        }
    }

    return NULL;
}

int64_t
rqc_cid_set_get_unused_cnt(rqc_cid_set_t *cid_set, uint64_t path_id)
{
    rqc_cid_set_inner_t *inner_set;
    inner_set = rqc_get_path_cid_set(cid_set, path_id);
    if (inner_set) {
        return inner_set->unused_cnt;
    }
    return RQC_ERROR;
}

int64_t
rqc_cid_set_get_used_cnt(rqc_cid_set_t *cid_set, uint64_t path_id)
{
    rqc_cid_set_inner_t *inner_set;
    inner_set = rqc_get_path_cid_set(cid_set, path_id);
    if (inner_set) {
        return inner_set->used_cnt;
    }
    return RQC_ERROR;
}

int64_t
rqc_cid_set_get_retired_cnt(rqc_cid_set_t *cid_set, uint64_t path_id)
{
    rqc_cid_set_inner_t *inner_set;
    inner_set = rqc_get_path_cid_set(cid_set, path_id);
    if (inner_set) {
        return inner_set->retired_cnt;
    }
    return RQC_ERROR;
}

int64_t
rqc_cid_set_get_largest_seq_or_rpt(rqc_cid_set_t *cid_set, uint64_t path_id)
{
    rqc_cid_set_inner_t *inner_set;
    inner_set = rqc_get_path_cid_set(cid_set, path_id);
    if (inner_set) {
        return inner_set->largest_scid_seq_num;
    }
    return RQC_ERROR;
}

rqc_int_t
rqc_cid_set_set_largest_seq_or_rpt(rqc_cid_set_t *cid_set, uint64_t path_id, uint64_t val)
{
    rqc_cid_set_inner_t *inner_set;
    inner_set = rqc_get_path_cid_set(cid_set, path_id);
    if (inner_set) {
        inner_set->largest_scid_seq_num = val;
        return RQC_OK;
    }
    return RQC_ERROR;
}

rqc_int_t
rqc_cid_set_insert_cid(rqc_cid_set_t *cid_set,
    rqc_cid_t *cid, rqc_cid_state_t state, uint64_t limit, uint64_t path_id)
{
    rqc_cid_set_inner_t *inner_set;
    inner_set = rqc_get_path_cid_set(cid_set, path_id);
    if (!inner_set) {
        return -RQC_ECONN_CID_NOT_FOUND;
    }

    if ((inner_set->unused_cnt + inner_set->used_cnt) > limit) {
        return -RQC_EACTIVE_CID_LIMIT;
    }

    rqc_cid_inner_t *inner_cid = rqc_calloc(1, sizeof(rqc_cid_inner_t));
    if (inner_cid == NULL) {
        return -RQC_EMALLOC;
    }
    cid->path_id = path_id;

    rqc_cid_copy(&inner_cid->cid, cid);
    inner_cid->state = state;
    inner_cid->retired_ts = RQC_MAX_UINT64_VALUE;

    rqc_init_list_head(&inner_cid->list);
    rqc_list_add_tail(&inner_cid->list, &inner_set->cid_list);

    if (state == RQC_CID_UNUSED) {
        inner_set->unused_cnt++;

    } else if (state == RQC_CID_USED) {
        inner_set->used_cnt++;

    } else if (state == RQC_CID_RETIRED) {
        inner_set->retired_cnt++;
    }

    return RQC_OK;
}

rqc_int_t
rqc_cid_set_delete_cid(rqc_cid_set_t *cid_set, rqc_cid_t *cid, uint64_t path_id)
{
    rqc_cid_inner_t *inner_cid = NULL;
    rqc_list_head_t *pos, *next;

    rqc_cid_set_inner_t *inner_set;
    inner_set = rqc_get_path_cid_set(cid_set, path_id);

    if (!inner_set) {
        return RQC_ERROR;
    }

    rqc_list_for_each_safe(pos, next, &inner_set->cid_list) {
        inner_cid = rqc_list_entry(pos, rqc_cid_inner_t, list);
        if (rqc_cid_is_equal(cid, &inner_cid->cid) == RQC_OK) {

            if (inner_cid->state == RQC_CID_UNUSED) {
                inner_set->unused_cnt--;
                if (inner_cid->acked == RQC_CID_ACKED) {
                    inner_set->acked_unused--;
                }

            } else if (inner_cid->state == RQC_CID_USED) {
                inner_set->used_cnt--;

            } else if (inner_cid->state == RQC_CID_RETIRED) {
                inner_set->retired_cnt--;
            }

            rqc_list_del(pos);
            rqc_free(inner_cid);
            return RQC_OK;
        }
    }

    return RQC_ERROR;
}
rqc_cid_inner_t *
rqc_cid_set_search_cid(rqc_cid_set_t *cid_set,
    rqc_cid_t *cid)
{
    rqc_cid_inner_t *inner_cid;
    rqc_list_head_t *pos, *next;
    rqc_cid_set_inner_t *inner_set;
    rqc_list_head_t *pos_set, *next_set;

    rqc_list_for_each_safe(pos_set, next_set, &cid_set->cid_set_list) {
        inner_set = rqc_list_entry(pos_set, rqc_cid_set_inner_t, next);

        rqc_list_for_each_safe(pos, next, &inner_set->cid_list) {
            inner_cid = rqc_list_entry(pos, rqc_cid_inner_t, list);

            if (rqc_cid_is_equal(cid, &inner_cid->cid) == RQC_OK) {
                cid->cid_seq_num = inner_cid->cid.cid_seq_num;
                cid->path_id = inner_cid->cid.path_id;
                return inner_cid;
            }
        }
    }

    return NULL;
}

rqc_cid_inner_t *
rqc_cid_in_cid_set(rqc_cid_set_t *cid_set, rqc_cid_t *cid, uint64_t path_id)
{
    rqc_cid_inner_t *inner_cid = NULL;
    rqc_list_head_t *pos, *next;

    rqc_cid_set_inner_t *inner_set;
    inner_set = rqc_get_path_cid_set(cid_set, path_id);

    if (!inner_set) {
        return NULL;
    }

    rqc_list_for_each_safe(pos, next, &inner_set->cid_list) {
        inner_cid = rqc_list_entry(pos, rqc_cid_inner_t, list);
        if (rqc_cid_is_equal(cid, &inner_cid->cid) == RQC_OK) {
            cid->cid_seq_num = inner_cid->cid.cid_seq_num;
            cid->path_id = inner_cid->cid.path_id;
            return inner_cid;
        }
    }

    return NULL;
}

rqc_int_t
rqc_cid_switch_to_next_state(rqc_cid_set_t *cid_set, rqc_cid_inner_t *cid, rqc_cid_state_t next_state, uint64_t path_id)
{
    if (rqc_cid_in_cid_set(cid_set, &cid->cid, path_id) == NULL) {
        return -RQC_ECONN_CID_NOT_FOUND;
    }

    rqc_cid_state_t current_state = cid->state;

    if (current_state == next_state) {
        return RQC_OK;

    } else if (current_state > next_state) {
        return -RQC_ECID_STATE;
    }

    rqc_cid_set_inner_t *inner_set;
    inner_set = rqc_get_path_cid_set(cid_set, path_id);

    if (!inner_set) {
        return -RQC_ECONN_CID_NOT_FOUND;
    }

    if (current_state == RQC_CID_UNUSED) {
        inner_set->unused_cnt--;
        if (cid->acked == RQC_CID_ACKED) {
            inner_set->acked_unused--;
        }

    } else if (current_state == RQC_CID_USED) {
        inner_set->used_cnt--;

    } else if (current_state == RQC_CID_RETIRED) {
        inner_set->retired_cnt--;
    }

    cid->state = next_state;

    if (next_state == RQC_CID_USED) {
        inner_set->used_cnt++;

    } else if (next_state == RQC_CID_RETIRED) {
        inner_set->retired_cnt++;
    }

    return RQC_OK;
}

rqc_int_t
rqc_get_unused_cid(rqc_cid_set_t *cid_set, rqc_cid_t *cid, uint64_t path_id)
{
    rqc_cid_set_inner_t *inner_set;
    inner_set = rqc_get_path_cid_set(cid_set, path_id);

    if (!inner_set) {
        return -RQC_ECONN_NO_AVAIL_CID;
    }

    if (inner_set->unused_cnt == 0) {
        return -RQC_ECONN_NO_AVAIL_CID;
    }

    rqc_cid_inner_t *inner_cid;
    rqc_list_head_t *pos, *next;

    rqc_list_for_each_safe(pos, next, &inner_set->cid_list) {
        inner_cid = rqc_list_entry(pos, rqc_cid_inner_t, list);

        if (inner_cid->state == RQC_CID_UNUSED) {
            rqc_cid_copy(cid, &inner_cid->cid);
            return rqc_cid_switch_to_next_state(cid_set, inner_cid, RQC_CID_USED, path_id);
        }
    }

    return -RQC_ECONN_NO_AVAIL_CID;
}

rqc_cid_inner_t *
rqc_get_inner_cid_by_seq(rqc_cid_set_t *cid_set, uint64_t seq_num, uint64_t path_id)
{
    rqc_cid_inner_t *inner_cid = NULL;
    rqc_list_head_t *pos, *next;

    rqc_cid_set_inner_t *inner_set;
    inner_set = rqc_get_path_cid_set(cid_set, path_id);

    if (!inner_set) {
        return NULL;
    }

    rqc_list_for_each_safe(pos, next, &inner_set->cid_list) {
        inner_cid = rqc_list_entry(pos, rqc_cid_inner_t, list);

        if (inner_cid->cid.cid_seq_num == seq_num) {
            return inner_cid;
        }
    }

    return NULL;
}

rqc_int_t
rqc_cid_set_add_path(rqc_cid_set_t *cid_set, uint64_t path_id)
{
    rqc_cid_set_inner_t *inner_set;
    inner_set = rqc_get_path_cid_set(cid_set, path_id);

    if (inner_set) {
        return RQC_OK;
    }

    /* Note: the memory of inner_set will only be released on conn_destroy */
    inner_set = rqc_calloc(1, sizeof(rqc_cid_set_inner_t));
    if (!inner_set) {
        return -RQC_EMALLOC;
    }

    rqc_cid_set_inner_init(inner_set);
    rqc_list_add_tail(&inner_set->next, &cid_set->cid_set_list);
    inner_set->path_id = path_id;
    cid_set->set_cnt[RQC_CID_SET_UNUSED]++;
    return RQC_OK;
}

void
rqc_cid_set_update_state(rqc_cid_set_t *cid_set,
    uint64_t path_id, rqc_cid_set_state_t state)
{
    rqc_cid_set_inner_t *inner_set;
    inner_set = rqc_get_path_cid_set(cid_set, path_id);

    if (inner_set && inner_set->set_state != state) {
        cid_set->set_cnt[inner_set->set_state]--;
        cid_set->set_cnt[state]++;
        inner_set->set_state = state;
    }
}

void
rqc_cid_set_on_cid_acked(rqc_cid_set_t *cid_set, uint64_t path_id,
    uint64_t cid_seq)
{
    rqc_cid_set_inner_t *inner_set;
    rqc_list_head_t *pos, *next;
    rqc_cid_inner_t *inner_cid;
    inner_set = rqc_get_path_cid_set(cid_set, path_id);

    if (inner_set) {
        rqc_list_for_each_safe(pos, next, &inner_set->cid_list) {
            inner_cid = rqc_list_entry(pos, rqc_cid_inner_t, list);
            if (inner_cid->cid.cid_seq_num == cid_seq) {
                if (inner_cid->acked == RQC_CID_UNACKED
                    && inner_cid->state == RQC_CID_UNUSED)
                {
                    inner_set->acked_unused++;
                }
                inner_cid->acked = RQC_CID_ACKED;
            }
        }
    }
}
