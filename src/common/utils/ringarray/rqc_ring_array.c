/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "rqc_ring_array.h"

typedef struct rqc_rarray_s {
    /* continuous memory of array, buf_size = cap * esize. this would be NULL if capacity is 0 */
    uint8_t        *buf;

    /* size of element */
    uint64_t        esize;

    /* capacity of array, aka, the max count of elements */
    uint64_t        cap;

    /* total count of elements stored in ring array */
    uint64_t        count;

    /* the index offset of first element */
    uint64_t        offset;

    /* the final size of buf is power of 2, mask is used to retrieve node */
    uint64_t        mask;

} rqc_rarray_s;

rqc_rarray_t *
rqc_rarray_create(size_t cap, size_t esize)
{
    rqc_rarray_t *ra = rqc_calloc(1, sizeof(rqc_rarray_t));
    if (ra == NULL) {
        return NULL;
    }

    uint64_t array_cap = 0;
    if (esize != 0) {
        array_cap = rqc_pow2_upper(cap);
        if (array_cap == RQC_POW2_UPPER_ERROR) {
            rqc_free(ra);
            return NULL;
        }
        ra->buf = rqc_malloc(array_cap * esize);
        if (ra->buf == NULL) {
            rqc_free(ra);
            return NULL;
        }
    }

    ra->cap = array_cap;
    ra->esize = esize;
    ra->mask = array_cap - 1;
    ra->offset = 0;
    ra->count = 0;

    return ra;
}

void
rqc_rarray_destroy(rqc_rarray_t *ra)
{
    if (ra->buf) {
        rqc_free(ra->buf);
    }

    rqc_free(ra);
}

/* check if the offset of element is legal. offset MUST be in range [0, capacity) */
static inline rqc_bool_t
rqc_rarray_check_range(rqc_rarray_t *ra, uint64_t offset)
{
    uint64_t eoffset = (ra->offset + ra->count) & ra->mask; /* end offset of rarray */
    if (ra->offset >= eoffset) {
        /*
         * input offset is always in range [0, capacity), if rollover,
         * ra->offset equals to eoffset, only if offset not exceed capacity,
         * it is always in range.
         */
        if (ra->count == 0) {
            return RQC_FALSE;
        }
        return offset >= ra->offset || offset < eoffset;

    } else {
        return offset >= ra->offset && offset < eoffset;
    }
}

void *
rqc_rarray_get(rqc_rarray_t *ra, uint64_t idx)
{
    if (ra == NULL || idx >= ra->cap) {
        return NULL;
    }

    /* check if idx is available */
    uint64_t offset = (idx + ra->offset) & ra->mask;
    if (rqc_rarray_check_range(ra, offset) == RQC_FALSE) {
        return NULL;
    }

    return ra->buf + offset * ra->esize;
}

size_t
rqc_rarray_size(rqc_rarray_t *ra)
{
    return ra->count;
}

rqc_int_t
rqc_rarray_full(rqc_rarray_t *ra)
{
    return ra->count >= ra->cap;
}

void *
rqc_rarray_front(rqc_rarray_t *ra)
{
    if (ra->count == 0) {
        return NULL;
    }

    return (ra->buf + ra->offset * ra->esize);
}

void *
rqc_rarray_push(rqc_rarray_t *ra)
{
    if (ra->count >= ra->cap) {
        return NULL;
    }

    void *buf = ra->buf + ((ra->offset + ra->count) & ra->mask) * ra->esize;
    ra->count++;
    return buf;
}

void *
rqc_rarray_push_front(rqc_rarray_t *ra)
{
    if (ra->count >= ra->cap) {
        return NULL;
    }

    ra->count++;
    ra->offset = (ra->offset -1) & ra->mask;
    return ra->buf + ra->offset * ra->esize;
}

rqc_int_t
rqc_rarray_pop_front(rqc_rarray_t *ra)
{
    if (ra->count == 0) {
        return RQC_ERROR;
    }

    /*
     * even all elements are pop, offset will not be reset,
     * and new insertion will continue from offset
     */
    ra->offset = (ra->offset + 1) & ra->mask;
    ra->count--;

    return RQC_OK;
}

rqc_int_t
rqc_rarray_pop_back(rqc_rarray_t *ra)
{
    if (ra->count == 0) {
        return RQC_ERROR;
    }

    ra->count--;
    return RQC_OK;
}

rqc_int_t
rqc_rarray_pop_from(rqc_rarray_t *ra, uint64_t idx)
{
    if (ra->count <= idx) {
        return RQC_ERROR;
    }

    ra->count = idx;
    return RQC_OK;
}

rqc_int_t
rqc_rarray_resize(rqc_rarray_t *ra, uint64_t cap)
{
    if (cap < ra->count) {
        return -RQC_EPARAM;

    } else if (cap <= ra->cap) {
        /* new capacity is smaller, do nothing */
        return RQC_OK;
    }

    uint64_t array_cap = rqc_pow2_upper(cap);
    if (array_cap == RQC_POW2_UPPER_ERROR) {
        return -RQC_EMALLOC;
    }
    uint8_t *buf = rqc_malloc(array_cap * ra->esize);
    if (buf == NULL) {
        return -RQC_EMALLOC;
    }

    if (ra->cap != 0) {
        /* copy data from original buf to the begin of new buf */
        uint64_t end = (ra->offset + ra->count) & ra->mask; /* end index */
        if (end >= ra->offset) {
            memcpy(buf, ra->buf + ra->offset * ra->esize, ra->count * ra->esize);

        } else {
            memcpy(buf, ra->buf + ra->offset * ra->esize, (ra->cap - ra->offset) * ra->esize);
            memcpy(buf + (ra->cap - ra->offset) * ra->esize, ra->buf, end * ra->esize);
        }

        rqc_free(ra->buf);
    }

    ra->buf = buf;
    ra->cap = array_cap;
    ra->mask = array_cap - 1;
    ra->offset = 0;

    return RQC_OK;
}

void
rqc_rarray_reinit(rqc_rarray_t *ra)
{
    rqc_memzero(ra->buf, ra->cap * ra->esize);
    ra->count = 0;
    ra->offset = 0;
}
