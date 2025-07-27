/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_RING_ARRAY_H_
#define _RQC_RING_ARRAY_H_

#include "src/common/rqc_common_inc.h"

typedef struct rqc_rarray_s rqc_rarray_t;

/**
 * @brief create a ring array with FIFO
 * @param cap the capacity of array
 * @param esize the size of each element
 * @return rqc_rarray_t* the pointer of ring array
 */
rqc_rarray_t *rqc_rarray_create(size_t cap, size_t esize);

/**
 * @brief destroy a ring array
 * @param ra pointer of ring array
 */
void rqc_rarray_destroy(rqc_rarray_t *ra);

/**
 * @brief get the element pointer at specified index
 * @param ra pointer of ring array
 * @param idx index of element, starts from 0
 * @return void* the memory block of input index. caller shall not remember this value
 */
void *rqc_rarray_get(rqc_rarray_t *ra, uint64_t idx);

/**
 * @brief get the count of elements
 */
size_t rqc_rarray_size(rqc_rarray_t *ra);

rqc_int_t rqc_rarray_full(rqc_rarray_t *ra);

/**
 * @brief get front element
 */
void *rqc_rarray_front(rqc_rarray_t *ra);

/**
 * @brief push element to the end of array
 * @param ra pointer of ring array
 * @param element pointer of element, will be copy to ring array with esize
 * @return rqc_int_t RQC_OK for success, others for failure
 */
void *rqc_rarray_push(rqc_rarray_t *ra);

/**
 * @brief push element to the front of array
 * @param ra pointer of ring array
 * @param element pointer of element, will be copy to ring array with esize
 * @return rqc_int_t RQC_OK for success, others for failure
 */
void *rqc_rarray_push_front(rqc_rarray_t *ra);

/**
 * @brief pop element from the front of array
 */
rqc_int_t rqc_rarray_pop_front(rqc_rarray_t *ra);

/**
 * @brief pop element from the end of array
 */
rqc_int_t rqc_rarray_pop_back(rqc_rarray_t *ra);

/**
 * @brief pop element from idx to the end
 */
rqc_int_t rqc_rarray_pop_from(rqc_rarray_t *ra, uint64_t idx);

/**
 * @brief resize ring array
 */
rqc_int_t rqc_rarray_resize(rqc_rarray_t *ra, uint64_t cap);

void rqc_rarray_reinit(rqc_rarray_t *ra);

#endif
