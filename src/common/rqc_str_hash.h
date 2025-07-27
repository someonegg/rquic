/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_STR_HASH_INCLUDED_
#define _RQC_STR_HASH_INCLUDED_

#include <stdint.h>
#include <time.h>

#include "src/common/rqc_str.h"
#include "src/common/rqc_siphash.h"
#include "src/common/rqc_log.h"

/*
 * default log threshold of number of element in on bucket
 * test result of max conflict (default 1024*1024 hash buckets) in one bucket:
 * 100000 connections, the max conflict is 5
 * 1000000 connections, the max conflict is 24
 */
#define RQC_HASH_DEFAULT_CONFLICT_THRESHOLD     50
/* 10 second, log interval must not less then 10 second */
#define RQC_HASH_CONFLICT_LOG_INTERVAL  10

typedef struct rqc_str_hash_element_s {
    uint64_t    hash;
    rqc_str_t   str;
    void       *value;
} rqc_str_hash_element_t;

typedef struct rqc_str_hash_node_s {
    struct rqc_str_hash_node_s *next;
    rqc_str_hash_element_t      element;
} rqc_str_hash_node_t;

typedef struct rqc_str_hash_table_s {
    rqc_str_hash_node_t   **list;
    size_t                  count;
    rqc_allocator_t         allocator;  /* memory allocator */
    uint8_t                *conflict_stat;  /* statistic the number of elements in every bucket */
    rqc_siphash_ctx_t       siphash_ctx;    /* siphash context */
    uint32_t                conflict_thres; /* conflict threshold in one bucket, warning if exceeded */
    time_t                  last_log_time;  /* last timestamp(second) for logging the max conflict value*/
    rqc_log_t              *log;            /* pointer to engine's log*/
} rqc_str_hash_table_t;

/* calculate the hash value using the siphash algorithm */
static inline uint64_t
rqc_siphash_get_hash(rqc_siphash_ctx_t *ctx, const uint8_t *data, size_t len)
{
    uint64_t hash_value;
    if (rqc_siphash(ctx, data, len, (uint8_t *)(&hash_value), sizeof(hash_value)) == RQC_OK) {
        return hash_value;
    }
    /*
     * impossible, we set hash_size value 8 when we call rqc_siphash_init, sizeof(hash_value) is always 8
     * rqc_siphash return RQC_ERROR only when hash_size not equal sizeof(hash_value), it is impossible here
     */
    return 0;
}

static inline int
rqc_str_hash_init(rqc_str_hash_table_t *hash_tab,
                  rqc_allocator_t allocator, size_t bucket_num,
                  uint32_t conflict_thres, uint8_t *key,
                  size_t key_len, rqc_log_t *log)
{
    if (bucket_num == 0) { /* impossible */
        return RQC_ERROR;
    }
    if (key_len != RQC_SIPHASH_KEY_SIZE) { /* siphash key length must be 16 */
        return RQC_ERROR;
    }
    if (log == NULL) {
        return RQC_ERROR;
    }
    rqc_memzero(hash_tab, sizeof(rqc_str_hash_table_t));
    hash_tab->allocator = allocator;
    hash_tab->list = allocator.malloc(allocator.opaque, sizeof(rqc_str_hash_node_t *) * bucket_num);
    if (hash_tab->list == NULL) {
        return RQC_ERROR;
    }
    rqc_memzero(hash_tab->list, sizeof(rqc_str_hash_node_t *) * bucket_num);
    hash_tab->count = bucket_num;
    hash_tab->conflict_stat = allocator.malloc(allocator.opaque, sizeof(uint8_t) * bucket_num);
    if (hash_tab->conflict_stat == NULL) {
        goto fail;
    }
    rqc_memzero(hash_tab->conflict_stat, sizeof(uint8_t) * bucket_num);
    if (conflict_thres > 0) {
        hash_tab->conflict_thres = conflict_thres;
    } else {
        hash_tab->conflict_thres = RQC_HASH_DEFAULT_CONFLICT_THRESHOLD;
    }
    hash_tab->last_log_time = 0;
    hash_tab->log = log;
    if (rqc_siphash_init(&hash_tab->siphash_ctx, key, key_len,
                         RQC_DEFAULT_HASH_SIZE, RQC_SIPHASH_C_ROUNDS,
                         RQC_SIPHASH_D_ROUNDS) != RQC_OK)
    {
        goto fail;
    }
    return RQC_OK;

fail:
    if (hash_tab->list) {
        allocator.free(allocator.opaque, hash_tab->list);
    }
    if (hash_tab->conflict_stat) {
        allocator.free(allocator.opaque, hash_tab->conflict_stat);
    }
    return RQC_ERROR;
}

static inline void
rqc_str_hash_release(rqc_str_hash_table_t *hash_tab)
{
    rqc_allocator_t *a = &hash_tab->allocator;
    for (size_t i = 0; i < hash_tab->count; ++i) {
        rqc_str_hash_node_t *node = hash_tab->list[i];
        while (node) {
            rqc_str_hash_node_t *p = node;
            node = node->next;
            a->free(a->opaque, p);
        }
    }
    a->free(a->opaque, hash_tab->list);
    a->free(a->opaque, hash_tab->conflict_stat);
}

static inline void *
rqc_str_hash_find(rqc_str_hash_table_t *hash_tab, uint64_t hash, rqc_str_t str)
{
    uint64_t index = hash % hash_tab->count;
    rqc_str_hash_node_t *node = hash_tab->list[index];
    while (node) {
        if (node->element.hash == hash && rqc_str_equal(str, node->element.str)) {
            return node->element.value;
        }
        node = node->next;
    }
    return NULL;
}

static inline int
rqc_str_hash_add(rqc_str_hash_table_t *hash_tab, rqc_str_hash_element_t e)
{
    uint64_t index = e.hash % hash_tab->count;
    rqc_allocator_t *a = &hash_tab->allocator;
    rqc_str_hash_node_t *node = a->malloc(a->opaque, sizeof(rqc_str_hash_node_t));
    if (node == NULL) {
        return RQC_ERROR;
    }

    node->element = e;
    node->element.str.data = a->malloc(a->opaque, e.str.len);
    if (node->element.str.data == NULL) {
        a->free(a->opaque, node);
        return RQC_ERROR;
    }
    rqc_memcpy(node->element.str.data, e.str.data, e.str.len);
    node->element.str.len = e.str.len;

    node->next = hash_tab->list[index];
    hash_tab->list[index] = node;
    hash_tab->conflict_stat[index] += 1;
    if (hash_tab->conflict_stat[index] > hash_tab->conflict_thres) {
        time_t now_sec = time(NULL);
        if (now_sec >= hash_tab->last_log_time + RQC_HASH_CONFLICT_LOG_INTERVAL) {
            rqc_log(hash_tab->log, RQC_LOG_WARN,
                    "|rqc conn hash conflict exceed|index:%ui, number of elements:%d|",
                    index, hash_tab->conflict_stat[index]);
            hash_tab->last_log_time = now_sec;
        }
    }

    return RQC_OK;
}

static inline int
rqc_str_hash_delete(rqc_str_hash_table_t *hash_tab, uint64_t hash, rqc_str_t str)
{
    uint64_t index = hash % hash_tab->count;
    rqc_allocator_t        *a    = &hash_tab->allocator;
    rqc_str_hash_node_t   **pp   = &hash_tab->list[index];
    rqc_str_hash_node_t    *node = hash_tab->list[index];
    while (node) {
        if (node->element.hash == hash && rqc_str_equal(str, node->element.str)) {
            *pp = node->next;
            if (hash_tab->conflict_stat[index] > 0) {
                hash_tab->conflict_stat[index] -= 1;
            }
            a->free(a->opaque, node->element.str.data);
            a->free(a->opaque, node);
            return RQC_OK;
        }

        pp = &node->next;
        node = node->next;
    }

    return RQC_ERROR;
}

#endif /*_RQC_STR_HASH_INCLUDED_*/
