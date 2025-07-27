/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_INT_HASH_H_INCLUDED_
#define _RQC_INT_HASH_H_INCLUDED_

#include <stdint.h>
#include <math.h>

#include "src/common/rqc_common.h"
#include "src/common/rqc_memory_pool.h"

typedef struct rqc_id_hash_element_s {
    uint64_t                    hash;
    void                       *value;
} rqc_id_hash_element_t;

typedef struct rqc_id_hash_node_s {
    struct rqc_id_hash_node_s  *next;
    rqc_id_hash_element_t       element;
} rqc_id_hash_node_t;

typedef struct rqc_id_hash_table_s {
    rqc_id_hash_node_t        **list;
    size_t                      count;
    size_t                      mask;
    rqc_allocator_t             allocator;
} rqc_id_hash_table_t;

static inline rqc_int_t
rqc_id_hash_init(rqc_id_hash_table_t *hash_tab,  rqc_allocator_t allocator, size_t bucket_num)
{
    hash_tab->allocator = allocator;
    bucket_num = rqc_pow2_upper(bucket_num);
    hash_tab->list = allocator.malloc(allocator.opaque, sizeof(rqc_id_hash_node_t *) * bucket_num);
    if (hash_tab->list == NULL) {
        return RQC_ERROR;
    }
    memset(hash_tab->list, 0, sizeof(rqc_id_hash_node_t *) * bucket_num);
    hash_tab->count = bucket_num;
    hash_tab->mask  = bucket_num - 1;
    return RQC_OK;
}

static inline void
rqc_id_hash_release(rqc_id_hash_table_t *hash_tab)
{
    rqc_allocator_t *a = &hash_tab->allocator;
    for (size_t i = 0; i < hash_tab->count; ++i) {
        rqc_id_hash_node_t *node = hash_tab->list[i];
        while (node) {
            rqc_id_hash_node_t *p = node;
            if (node->next == node) {
                break;
            }
            node = node->next;
            a->free(a->opaque, p);
        }
    }
    a->free(a->opaque, hash_tab->list);
}

static inline void *
rqc_id_hash_find(rqc_id_hash_table_t *hash_tab, uint64_t hash)
{
    uint64_t index = hash & hash_tab->mask;
    rqc_id_hash_node_t *node = hash_tab->list[index];

    while (node) {
        if (node->element.hash == hash) {
            return node->element.value;
        }

        if (node->next == node) {
            return NULL;
        }
        node = node->next;
    }

    return NULL;
}

static inline rqc_int_t
rqc_id_hash_add(rqc_id_hash_table_t *hash_tab, rqc_id_hash_element_t e)
{
    if (rqc_id_hash_find(hash_tab, e.hash)) {
        return RQC_ERROR;
    }

    uint64_t index = e.hash & hash_tab->mask;
    rqc_allocator_t *a = &hash_tab->allocator;

    rqc_id_hash_node_t *node = a->malloc(a->opaque, sizeof(rqc_id_hash_node_t));
    if (node == NULL) {
        return RQC_ERROR;
    }

    node->element = e;
    node->next = hash_tab->list[index];
    hash_tab->list[index] = node;

    return RQC_OK;
}

#define RQC_ID_HASH_LOOP -9

static inline rqc_int_t
rqc_id_hash_delete(rqc_id_hash_table_t* hash_tab, uint64_t hash)
{
    uint64_t index = hash & hash_tab->mask;
    rqc_allocator_t    *a     = &hash_tab->allocator;
    rqc_id_hash_node_t **pp   = &hash_tab->list[index];
    rqc_id_hash_node_t  *node = hash_tab->list[index];

    while (node) {
        if (node->element.hash == hash) {
            *pp = node->next;
            a->free(a->opaque, node);
            return RQC_OK;
        }

        if (node->next == node) {
            return RQC_ID_HASH_LOOP;
        }
        pp = &node->next;
        node = node->next;
    }

    return RQC_ERROR;
}

#endif
