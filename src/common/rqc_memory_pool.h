/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_MEMORY_POOL_H_INCLUDED_
#define _RQC_MEMORY_POOL_H_INCLUDED_

#include <string.h>
#include <stdint.h>
#include <rquic/rquic.h>

#include "src/common/rqc_malloc.h"

#ifdef RQC_PROTECT_POOL_MEM
#ifndef RQC_SYS_WINDOWS
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h>
#endif
#endif

/* Interfaces:
 * rqc_memory_pool_t *rqc_create_pool(size_t size)
 * void rqc_destroy_pool(rqc_memory_pool_t* pool)
 * void* rqc_palloc(rqc_memory_pool_t *pool, size_t size)
 * void* rqc_pnalloc(rqc_memory_pool_t *pool, size_t size)
 * void* rqc_pcalloc(rqc_memory_pool_t *pool, size_t size)
 */

typedef struct rqc_memory_block_s {
    char       *last;
    char       *end;
    unsigned    failed;
    struct rqc_memory_block_s *next;
} rqc_memory_block_t;

typedef struct rqc_memory_large_s {
    struct rqc_memory_large_s *next;
    unsigned    size;
    char        data[0];
} rqc_memory_large_t;

typedef struct rqc_memory_pool_s {
    rqc_memory_block_t  block;
    rqc_memory_block_t *current;
    rqc_memory_large_t *large;  /* large chunk list */
    size_t              max;
#ifdef RQC_PROTECT_POOL_MEM
    rqc_bool_t          protect_block;
    size_t              page_size;
#endif
} rqc_memory_pool_t;

#define RQC_MAX_MALLOC_FROM_POOL (4096)

#ifdef RQC_PROTECT_POOL_MEM
static inline void *
rqc_mempool_malloc_protected(size_t size, size_t page_sz)
{
#ifndef RQC_SYS_WINDOWS
    int ret;
    void *ptr = NULL;
    ret = posix_memalign(&ptr, page_sz, page_sz + size);
    if (ret != 0) {
        return NULL;
    }
    ret = mprotect(ptr, page_sz, PROT_READ);
    if (ret != 0) {
        rqc_free(ptr);
        return NULL;
    }
    return (void*)((char*)ptr + page_sz);
#else
    return rqc_malloc(size);
#endif
}

static inline void
rqc_mempool_free_protected(void* ptr, size_t page_sz) {
#ifndef RQC_SYS_WINDOWS
    void *start;
    int ret;
    start = (void*)((char*)ptr - page_sz);
    ret = mprotect(start, page_sz, PROT_READ | PROT_WRITE | PROT_EXEC);
    assert(ret == 0);
    rqc_free(start);
#else
    rqc_free(ptr);
#endif
}
#endif

#ifdef RQC_PROTECT_POOL_MEM
static inline rqc_memory_pool_t *
rqc_create_pool(size_t size, rqc_bool_t protect_block)
#else
static inline rqc_memory_pool_t *
rqc_create_pool(size_t size)
#endif
{
    if (size <= sizeof(rqc_memory_pool_t)) {
        return NULL;
    }

#ifdef RQC_PROTECT_POOL_MEM
    char *m;
#ifndef RQC_SYS_WINDOWS
    size_t page_sz = sysconf(_SC_PAGESIZE);
#else
    size_t page_sz = 4096;
#endif
    if (protect_block) {
        m = rqc_mempool_malloc_protected(size, page_sz);
    } else {
        m = rqc_malloc(size);
    }
#else
    char *m = rqc_malloc(size);
#endif
    if (m == NULL) {
        return NULL;
    }

    rqc_memory_pool_t *pool = (rqc_memory_pool_t *)m;
    pool->block.last = m + sizeof(rqc_memory_pool_t);
    pool->block.end = m + size;
    pool->block.failed = 0;
    pool->block.next = NULL;
#ifdef RQC_PROTECT_POOL_MEM
    pool->protect_block = protect_block;
    pool->page_size = page_sz;
#endif

    pool->current = &pool->block;
    pool->large = NULL;
    pool->max = size - sizeof(rqc_memory_pool_t);
    if (pool->max > RQC_MAX_MALLOC_FROM_POOL) {
        pool->max = RQC_MAX_MALLOC_FROM_POOL;
    }

    return pool;
}

static inline void
rqc_destroy_pool(rqc_memory_pool_t *pool)
{
    rqc_memory_block_t *block = pool->block.next;
    while (block) {
        rqc_memory_block_t *p = block;
        block = block->next;
#ifdef RQC_PROTECT_POOL_MEM
        if (pool->protect_block) {
            rqc_mempool_free_protected(p, pool->page_size);

        } else {
            rqc_free(p);
        }
#else
        rqc_free(p);
#endif
    }

    rqc_memory_large_t *large = pool->large;
    while (large) {
        rqc_memory_large_t * p = large;
        large = large->next;
#ifdef RQC_PROTECT_POOL_MEM
        if (pool->protect_block) {
            rqc_mempool_free_protected(p, pool->page_size);

        } else {
            rqc_free(p);
        }
#else
        rqc_free(p);
#endif
    }

#ifdef RQC_PROTECT_POOL_MEM
    if (pool->protect_block) {
        rqc_mempool_free_protected(pool, pool->page_size);

    } else {
        rqc_free(pool);
    }
#else
        rqc_free(pool);
#endif
}

static inline void *
rqc_palloc_large(rqc_memory_pool_t *pool, size_t size)
{
#ifdef RQC_PROTECT_POOL_MEM
    rqc_memory_large_t *p;
    if (pool->protect_block) {
        p = rqc_mempool_malloc_protected(size + sizeof(rqc_memory_large_t), pool->page_size);

    } else {
        p = rqc_malloc(size + sizeof(rqc_memory_large_t));
    }
#else
    rqc_memory_large_t *p = rqc_malloc(size + sizeof(rqc_memory_large_t));
#endif

    if (p == NULL) {
        return NULL;
    }

    p->size = size;
    p->next = pool->large;
    pool->large = p;

    return p->data;
}

#define RQC_ALIGNMENT (16)
#define rqc_align_ptr(p, a) ((char *) (((uintptr_t)(p) + ((uintptr_t)a - 1)) & ~((uintptr_t)a - 1)))

static inline void *
rqc_palloc_block(rqc_memory_pool_t *pool, size_t size)
{
    size_t psize = pool->block.end - (char *)pool;

#ifdef RQC_PROTECT_POOL_MEM
    char *m;
    if (pool->protect_block) {
        m = rqc_mempool_malloc_protected(psize, pool->page_size);

    } else {
        m = rqc_malloc(psize);
    }
#else
    char *m = rqc_malloc(psize);
#endif

    if (m == NULL) {
        return NULL;
    }

    rqc_memory_block_t *b = (rqc_memory_block_t *)m;

    b->end = m + psize;

    m += sizeof(rqc_memory_block_t);
    m = rqc_align_ptr(m, RQC_ALIGNMENT);

    b->last = m + size;
    b->failed = 0;
    b->next = NULL;

    rqc_memory_block_t *block = pool->current;
    for (; block->next; block = block->next) {
        if (++block->failed > 4) {
            pool->current = block->next;
        }
    }

    block->next = b;

    return m;
}

/* aligned memory block access may be faster */
static inline void *
rqc_palloc(rqc_memory_pool_t *pool, size_t size)
{
    if (size < pool->max) {
        rqc_memory_block_t * block = pool->current;

        do {
            char *p = rqc_align_ptr(block->last, RQC_ALIGNMENT);
            if (block->end > p && (size_t)(block->end - p) >= size) {
                block->last = p + size;
                return p;
            }

            block = block->next;
        } while (block);

        return rqc_palloc_block(pool, size);
    }

    return rqc_palloc_large(pool, size);
}

/* allocate memory interface from pool, no alignment */
static inline void *
rqc_pnalloc(rqc_memory_pool_t *pool, size_t size)
{
    if (size < pool->max) {
        rqc_memory_block_t * block = pool->current;

        do {
            char *p = block->last;
            if (block->end > p && (size_t)(block->end - p) >= size) {
                block->last = p + size;
                return p;
            }

            block = block->next;
        } while (block);

        return rqc_palloc_block(pool, size);
    }

    return rqc_palloc_large(pool, size);
}

/* aligned and zeroed out */
static inline void *
rqc_pcalloc(rqc_memory_pool_t *pool, size_t size)
{
    void* p = rqc_palloc(pool, size);
    if (p) {
        memset(p, 0, size);
        return p;
    }
    return NULL;
}

/* TODO: rqc_pfree is needed */

#endif /*_RQC_MEMORY_POOL_H_INCLUDED_*/
