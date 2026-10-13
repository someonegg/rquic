#ifndef RQC_DATAGRAM_H_INCLUDED
#define RQC_DATAGRAM_H_INCLUDED

#include "src/transport/rqc_conn.h"
#include "src/transport/rqc_packet_out.h"
#include "src/transport/rqc_packet_in.h"

typedef struct rqc_datagram_record_s {
    struct rqc_datagram_record_s *next;
    uint64_t id;
    rqc_bool_t lost;
} rqc_datagram_record_t;

rqc_int_t rqc_datagram_process_frame(rqc_connection_t *conn, rqc_packet_in_t *pi);
void rqc_datagram_record_feedback(rqc_connection_t *conn, rqc_packet_out_t *po, rqc_bool_t lost);
void rqc_datagram_process_callbacks(rqc_connection_t *conn);
void rqc_datagram_cleanup(rqc_connection_t *conn);

#endif
