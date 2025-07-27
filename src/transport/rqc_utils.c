/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#include "src/common/rqc_common.h"
#include "src/common/rqc_malloc.h"
#include "src/common/rqc_str_hash.h"
#include "src/common/rqc_hash.h"
#include "src/common/rqc_log.h"
#include "src/transport/rqc_utils.h"
#include "src/transport/rqc_conn.h"
#include "src/common/rqc_time.h"

int
rqc_conns_pq_push(rqc_pq_t *pq, rqc_connection_t *conn, uint64_t time_us)
{
    rqc_conns_pq_elem_t *elem = (rqc_conns_pq_elem_t *)rqc_pq_push(pq, time_us, &conn);
    if (!elem) {
        rqc_log(conn->log, RQC_LOG_ERROR, "|rqc_pq_push error|count:%uz|capacity:%uz|", pq->count, pq->capacity);
        return -RQC_EMALLOC;
    }
    return RQC_OK;
}

void
rqc_conns_pq_pop(rqc_pq_t *pq)
{
    rqc_pq_pop(pq);
}

rqc_conns_pq_elem_t *
rqc_conns_pq_top(rqc_pq_t *pq)
{
    return  (rqc_conns_pq_elem_t *)rqc_pq_top(pq);
}

rqc_connection_t *
rqc_conns_pq_pop_top_conn(rqc_pq_t *pq)
{
    /* used to traverse conns_pq */
    rqc_conns_pq_elem_t *el = rqc_conns_pq_top(pq);
    if (RQC_UNLIKELY(el == NULL || el->conn == NULL)) {
        rqc_conns_pq_pop(pq);
        return NULL;
    }

    rqc_connection_t *conn = el->conn;
    rqc_conns_pq_pop(pq);
    return conn;
}

void
rqc_conns_pq_remove(rqc_pq_t *pq, rqc_connection_t *conn)
{
    rqc_pq_remove(pq, conn->wakeup_pq_index);
}

int
rqc_insert_conns_hash(rqc_str_hash_table_t *conns_hash, rqc_connection_t *conn,
    const uint8_t *data, size_t len)
{
    uint64_t hash = rqc_siphash_get_hash(&conns_hash->siphash_ctx, data, len);
    rqc_str_hash_element_t c = {
        .str    = {
            .data = (unsigned char *)data,
            .len = len
        },
        .hash   = hash,
        .value  = conn
    };

    if (rqc_str_hash_add(conns_hash, c) != RQC_OK) {
        return -RQC_EMALLOC;
    }

    return RQC_OK;
}

int
rqc_remove_conns_hash(rqc_str_hash_table_t *conns_hash, rqc_connection_t *conn,
    const uint8_t *data, size_t len)
{
    uint64_t hash = rqc_siphash_get_hash(&conns_hash->siphash_ctx, data, len);
    rqc_str_t str = {
        .data   = (unsigned char *)data,
        .len    = len,
    };

    if (rqc_str_hash_delete(conns_hash, hash, str)) {
        rqc_log(conn->log, RQC_LOG_INFO, "|rqc_str_hash_delete error|");
        return -RQC_ECONN_NFOUND;
    }
    return 0;
}

int
rqc_insert_conns_addr_hash(rqc_str_hash_table_t *conns_hash, rqc_connection_t *conn,
    const struct sockaddr *addr, socklen_t addrlen)
{
    uint64_t hash = rqc_siphash_get_hash(&conns_hash->siphash_ctx, (const uint8_t *)addr, addrlen);
    rqc_str_hash_element_t c = {
        .str    = {
            .data = (unsigned char *)addr,
            .len = addrlen
        },
        .hash   = hash,
        .value  = conn
    };

    if (rqc_str_hash_add(conns_hash, c) != RQC_OK) {
        return -RQC_EMALLOC;
    }
    return 0;
}

void *
rqc_find_conns_hash(rqc_str_hash_table_t *conns_hash, rqc_connection_t *conn,
    const uint8_t *data, size_t len)
{
    uint64_t hash = rqc_siphash_get_hash(&conns_hash->siphash_ctx, data, len);
    rqc_str_t str = {
        .data   = (unsigned char *)data,
        .len    = len,
    };

    return rqc_str_hash_find(conns_hash, hash, str);
}
