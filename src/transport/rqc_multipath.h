
/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef RQC_MULTIPATH_H
#define RQC_MULTIPATH_H

#include <rquic/rquic_typedef.h>
#include <rquic/rquic.h>
#include "src/transport/rqc_cid.h"
#include "src/common/rqc_common.h"
#include "src/transport/rqc_packet.h"
#include "src/transport/rqc_recv_record.h"
#include "src/transport/rqc_frame_parser.h"

/* path state */
typedef enum {
    RQC_PATH_STATE_INIT       = 0,    /* initial state */
    RQC_PATH_STATE_VALIDATING = 1,    /* PATH_CHALLENGE sent/received on new path */
    RQC_PATH_STATE_ACTIVE     = 2,    /* PATH_RESPONSE received */
    RQC_PATH_STATE_CLOSING    = 3,    /* PATH_ABANDONED sent or received */
    RQC_PATH_STATE_CLOSED     = 4,    /* PATH_ABANDONED acked or draining timeout */
} rqc_path_state_t;

typedef enum {
    RQC_SEND_TYPE_NORMAL,
    RQC_SEND_TYPE_NORMAL_HIGH_PRI,
    RQC_SEND_TYPE_RETRANS,
    RQC_SEND_TYPE_PTO_PROBE,
    RQC_SEND_TYPE_N,
} rqc_send_type_t;

typedef enum {
    RQC_PATH_FLAG_SOCKET_ERROR    = 1 << 0,
    RQC_PATH_FLAG_SHOULD_ACK      = 1 << 1,
} rqc_path_flag_t;

/* path context */
struct rqc_path_ctx_s {

    /* Path_identifier */
    uint64_t            path_id;    /* path identifier */
    rqc_cid_t           path_scid;
    rqc_cid_t           path_dcid;

    /* Path_address: 4-tuple */
    unsigned char       peer_addr[sizeof(struct sockaddr_in6)],
                        local_addr[sizeof(struct sockaddr_in6)];
    socklen_t           peer_addrlen,
                        local_addrlen;

    char                addr_str[2*(RQC_MAX_CID_LEN + INET6_ADDRSTRLEN) + 10];
    socklen_t           addr_str_len;

    /* server receives a packet from different address (NAT rebinding) */
    uint32_t            rebinding_count;
    /* server validate NAT rebinding (PATH_CHALLENGE & PATH_RESPONSE) */
    uint32_t            rebinding_valid;

    unsigned char       rebinding_addr[sizeof(struct sockaddr_in6)];
    socklen_t           rebinding_addrlen;
    int                 rebinding_check_response;

    /* Path_state */
    rqc_path_state_t    path_state;
    unsigned char       path_challenge_data[RQC_PATH_CHALLENGE_DATA_LEN];

    rqc_path_flag_t     path_flag;

    /* Path cc & ack tracking */
    rqc_send_ctl_t     *path_send_ctl;
    rqc_pn_ctl_t       *path_pn_ctl;

    /* path send buffer, used to store packets scheduled to this path */
    rqc_list_head_t     path_schedule_buf[RQC_SEND_TYPE_N];
    uint32_t            path_schedule_bytes;

    /* related structs */
    rqc_connection_t   *parent_conn;

    /* Path_metrics */
    rqc_path_metrics_t  path_metrics;
    rqc_usec_t          path_create_time;
    rqc_usec_t          path_destroy_time;
};

rqc_bool_t rqc_is_same_addr(const struct sockaddr *sa1, const struct sockaddr *sa2);
rqc_bool_t rqc_is_same_addr_as_any_path(rqc_connection_t *conn, const struct sockaddr *peer_addr);

rqc_int_t rqc_generate_path_challenge_data(rqc_connection_t *conn, rqc_path_ctx_t *path);

void rqc_path_schedule_buf_destroy(rqc_path_ctx_t *path);
void rqc_path_schedule_buf_pre_destroy(rqc_send_queue_t *send_queue, rqc_path_ctx_t *path);
void rqc_path_send_buffer_append(rqc_path_ctx_t *path, rqc_packet_out_t *packet_out, rqc_list_head_t *head);
void rqc_path_send_buffer_remove(rqc_path_ctx_t *path, rqc_packet_out_t *packet_out);
void rqc_path_send_buffer_clear(rqc_connection_t *conn, rqc_path_ctx_t *path, rqc_list_head_t *head, rqc_send_type_t send_type);

/* init path_list & initial path for connection */
rqc_int_t rqc_conn_init_paths_list(rqc_connection_t *conn);

/* destroy all the paths of the connection */
void rqc_conn_destroy_paths_list(rqc_connection_t *conn);

/* create path inner */
rqc_path_ctx_t *rqc_conn_create_path_inner(rqc_connection_t *conn, rqc_cid_t *scid, rqc_cid_t *dcid, uint64_t path_id);

/* server update client addr when recv path_challenge frame */
rqc_int_t rqc_conn_server_init_path_addr(rqc_connection_t *conn, uint64_t path_id,
    const struct sockaddr *local_addr, socklen_t local_addrlen,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen);

rqc_int_t rqc_conn_client_init_path_addr(rqc_connection_t *conn);

/* path statistics */
void rqc_conn_path_metrics_print(rqc_connection_t *conn, rqc_conn_stats_t *stats);

#endif /* RQC_MULTIPATH_H */
