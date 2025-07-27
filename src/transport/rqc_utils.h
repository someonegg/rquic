/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_UTILS_H_INCLUDED_
#define _RQC_UTILS_H_INCLUDED_

#include <rquic/rquic_typedef.h>
#include "src/common/rqc_priority_q.h"

typedef struct rqc_conns_pq_elem_s {
    rqc_pq_key_t        time_us;
    rqc_connection_t   *conn;
} rqc_conns_pq_elem_t;

int rqc_conns_pq_push(rqc_pq_t *pq, rqc_connection_t *conn, uint64_t time_us);

void rqc_conns_pq_pop(rqc_pq_t *pq);

rqc_conns_pq_elem_t *rqc_conns_pq_top(rqc_pq_t *pq);

rqc_connection_t *rqc_conns_pq_pop_top_conn(rqc_pq_t *pq);

void rqc_conns_pq_remove(rqc_pq_t *pq, rqc_connection_t *conn);

int rqc_insert_conns_hash(rqc_str_hash_table_t *conns_hash,
    rqc_connection_t *conn, const uint8_t *data, size_t len);

int rqc_remove_conns_hash(rqc_str_hash_table_t *conns_hash,
    rqc_connection_t *conn, const uint8_t *data, size_t len);

void *rqc_find_conns_hash(rqc_str_hash_table_t *conns_hash,
    rqc_connection_t *conn, const uint8_t *data, size_t len);

#endif /* _RQC_UTILS_H_INCLUDED_ */
