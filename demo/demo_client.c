#include "demo_common.h"
#include "demo_event.h"
#include "demo_hq.h"
#include "demo_udp.h"

#include <rquic/rqc_errno.h>
#include <rquic/rquic.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct demo_client_s demo_client_t;

typedef struct demo_client_conn_s {
    demo_client_t *client;
    demo_udp_write_ctx_t write_ctx;
    demo_hq_conn_t *hq_conn;
    rqc_cid_t cid;
} demo_client_conn_t;

struct demo_client_s {
    const char *host;
    unsigned short port;
    const char *path;
    const char *output_path;
    rqc_log_level_t log_level;

    demo_event_runtime_t event_runtime;
    rqc_engine_t *engine;
    demo_hq_adapter_t *hq_adapter;

    demo_udp_peer_t peer;
    demo_udp_socket_t udp_socket;
    demo_udp_read_ctx_t read_ctx;

    demo_client_conn_t conn;
    demo_hq_stream_t *stream;
    FILE *output;
    size_t response_bytes;
    int exit_code;
};

static void
demo_client_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [--host ADDR] [--port PORT] [--path /resource] [--output FILE] [--log-level LEVEL]\n"
        "\n"
        "Defaults: --host %s --port %d --path %s --log-level error\n",
        prog, DEMO_DEFAULT_HOST, DEMO_DEFAULT_PORT, DEMO_DEFAULT_PATH);
}

static int
demo_client_parse_args(int argc, char **argv, demo_client_t *client)
{
    char errbuf[128];

    client->host = DEMO_DEFAULT_HOST;
    client->port = DEMO_DEFAULT_PORT;
    client->path = DEMO_DEFAULT_PATH;
    client->log_level = RQC_LOG_ERROR;
    client->exit_code = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            demo_client_usage(argv[0]);
            return 1;
        }
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            client->host = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (demo_parse_port(argv[++i], &client->port) != 0) {
                fprintf(stderr, "invalid --port value\n");
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
            client->path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            client->output_path = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            if (demo_parse_log_level(argv[++i], &client->log_level) != 0) {
                fprintf(stderr, "invalid --log-level value\n");
                return -1;
            }
            continue;
        }
        fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
        demo_client_usage(argv[0]);
        return -1;
    }

    if (demo_validate_resource_path(client->path, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "invalid --path: %s\n", errbuf);
        return -1;
    }

    return 0;
}

static int
demo_client_open_output(demo_client_t *client)
{
    if (client->output_path == NULL) {
        return 0;
    }

    client->output = fopen(client->output_path, "wb");
    if (client->output == NULL) {
        fprintf(stderr, "failed to open --output file\n");
        return -1;
    }

    return 0;
}

static int
demo_client_close_output(demo_client_t *client)
{
    if (client->output == NULL) {
        return 0;
    }

    if (fclose(client->output) != 0) {
        client->output = NULL;
        fprintf(stderr, "failed to close --output file\n");
        return -1;
    }

    client->output = NULL;
    return 0;
}

static int
demo_client_write_response(demo_client_t *client, const unsigned char *buf,
    size_t len)
{
    FILE *out = client->output != NULL ? client->output : stdout;

    if (len == 0) {
        return 0;
    }

    if (fwrite(buf, 1, len, out) != len) {
        fprintf(stderr, "client failed to write response\n");
        return -1;
    }

    return 0;
}

static int
demo_client_hq_conn_create(demo_hq_conn_t *conn, const rqc_cid_t *cid,
    void *conn_user_data)
{
    demo_udp_write_ctx_t *write_ctx = (demo_udp_write_ctx_t *)conn_user_data;
    demo_client_conn_t *client_conn = write_ctx != NULL ? write_ctx->user_data : NULL;

    if (client_conn == NULL) {
        return -1;
    }

    client_conn->hq_conn = conn;
    memcpy(&client_conn->cid, cid, sizeof(client_conn->cid));
    demo_hq_conn_set_user_data(conn, client_conn);
    return 0;
}

static int
demo_client_hq_conn_close(demo_hq_conn_t *conn, const rqc_cid_t *cid,
    void *conn_user_data)
{
    demo_client_conn_t *client_conn = demo_hq_conn_get_user_data(conn);

    (void)cid;
    (void)conn_user_data;

    if (client_conn != NULL) {
        client_conn->hq_conn = NULL;
        if (client_conn->client != NULL && client_conn->client->exit_code != 0) {
            fprintf(stderr, "client connection closed before response completed\n");
            demo_event_runtime_stop(&client_conn->client->event_runtime);
        }
    }

    return 0;
}

static int
demo_client_hq_stream_create(demo_hq_stream_t *stream, void *stream_user_data)
{
    (void)stream;
    (void)stream_user_data;
    return 0;
}

static int
demo_client_hq_stream_close(demo_hq_stream_t *stream, void *stream_user_data)
{
    (void)stream;
    (void)stream_user_data;
    return 0;
}

static int
demo_client_hq_stream_write(demo_hq_stream_t *stream, void *stream_user_data)
{
    ssize_t ret;

    ret = demo_hq_stream_send_pending_request(stream);
    (void)stream_user_data;
    if (ret < 0 && ret != -RQC_EAGAIN) {
        fprintf(stderr, "client failed to continue request: %zd\n", ret);
        return -1;
    }

    return 0;
}

static int
demo_client_hq_stream_read(demo_hq_stream_t *stream, void *stream_user_data)
{
    demo_client_t *client = stream_user_data;
    unsigned char buf[4096];
    uint8_t fin = 0;
    ssize_t ret;

    if (client == NULL) {
        return -1;
    }

    do {
        ret = demo_hq_stream_recv_response(stream, buf, sizeof(buf), &fin);
        if (ret == -RQC_EAGAIN) {
            break;
        }
        if (ret < 0) {
            fprintf(stderr, "client failed to read response: %zd\n", ret);
            client->exit_code = 1;
            demo_event_runtime_stop(&client->event_runtime);
            return -1;
        }

        if (ret > 0) {
            client->response_bytes += (size_t)ret;
            if (demo_client_write_response(client, buf, (size_t)ret) != 0) {
                client->exit_code = 1;
                demo_event_runtime_stop(&client->event_runtime);
                return -1;
            }
        }
    } while (ret > 0 && !fin);

    if (fin) {
        if (demo_client_close_output(client) != 0) {
            client->exit_code = 1;
            demo_event_runtime_stop(&client->event_runtime);
            return -1;
        }
        printf("OK bytes=%zu\n", client->response_bytes);
        client->exit_code = 0;
        demo_event_runtime_stop(&client->event_runtime);
    }

    return 0;
}

static int
demo_client_init_engine(demo_client_t *client)
{
    rqc_config_t config;
    rqc_engine_callback_t engine_cb;
    rqc_transport_callbacks_t transport_cb;
    demo_hq_callbacks_t hq_cb;

    if (rqc_engine_get_default_config(&config, RQC_ENGINE_CLIENT) != RQC_OK) {
        return -1;
    }
    config.cfg_log_level = client->log_level;

    memset(&engine_cb, 0, sizeof(engine_cb));
    engine_cb.set_event_timer = demo_set_event_timer;
    engine_cb.log_callbacks = demo_log_callbacks;

    memset(&transport_cb, 0, sizeof(transport_cb));
    transport_cb.write_socket = demo_udp_write_socket;

    client->engine = rqc_engine_create(RQC_ENGINE_CLIENT, &config, &engine_cb,
        &transport_cb, &client->event_runtime);
    if (client->engine == NULL) {
        return -1;
    }
    demo_event_runtime_set_engine(&client->event_runtime, client->engine);

    memset(&hq_cb, 0, sizeof(hq_cb));
    hq_cb.conn_create = demo_client_hq_conn_create;
    hq_cb.conn_close = demo_client_hq_conn_close;
    hq_cb.stream_create = demo_client_hq_stream_create;
    hq_cb.stream_close = demo_client_hq_stream_close;
    hq_cb.stream_read = demo_client_hq_stream_read;
    hq_cb.stream_write = demo_client_hq_stream_write;

    client->hq_adapter = demo_hq_register(client->engine, &hq_cb);
    return client->hq_adapter != NULL ? 0 : -1;
}

static int
demo_client_start_request(demo_client_t *client)
{
    rqc_conn_settings_t settings;
    const rqc_cid_t *cid;
    ssize_t sent;

    memset(&settings, 0, sizeof(settings));
    settings.cong_ctrl_callback = rqc_bbr_cb;
    settings.cc_params.customize_on = 1;
    settings.cc_params.init_cwnd = 32;
    settings.proto_version = RQC_VERSION_V1;
    settings.init_idle_time_out = 5000;
    settings.idle_time_out = 5000;
    settings.max_pkt_out_size = 1350;
    settings.adaptive_ack_frequency = 1;

    client->conn.client = client;
    client->conn.write_ctx.fd = client->udp_socket.fd;
    client->conn.write_ctx.fd4 = client->udp_socket.fd;
    client->conn.write_ctx.fd6 = client->udp_socket.fd;
    client->conn.write_ctx.write_watch.fd = -1;
    client->conn.write_ctx.runtime = &client->event_runtime;
    client->conn.write_ctx.engine = client->engine;
    client->conn.write_ctx.user_data = &client->conn;

    cid = demo_hq_connect(client->engine, &settings, client->host,
        (const struct sockaddr *)&client->peer.addr, client->peer.addrlen,
        &client->conn.write_ctx);
    if (cid == NULL || client->conn.hq_conn == NULL) {
        fprintf(stderr, "client failed to create connection\n");
        return -1;
    }
    memcpy(&client->conn.cid, cid, sizeof(client->conn.cid));
    memcpy(&client->conn.write_ctx.cid, cid, sizeof(client->conn.write_ctx.cid));
    client->conn.write_ctx.has_cid = 1;

    client->stream = demo_hq_stream_create(client->engine, client->conn.hq_conn,
        &client->conn.cid, client);
    if (client->stream == NULL) {
        fprintf(stderr, "client failed to create stream\n");
        return -1;
    }

    if (demo_hq_stream_set_request(client->stream, client->path) != RQC_OK) {
        fprintf(stderr, "client failed to prepare request\n");
        return -1;
    }

    sent = demo_hq_stream_send_pending_request(client->stream);
    if (sent < 0 && sent != -RQC_EAGAIN) {
        fprintf(stderr, "client failed to send request: %zd\n", sent);
        return -1;
    }

    return 0;
}

static void
demo_client_cleanup(demo_client_t *client)
{
    demo_hq_adapter_unregister(client->hq_adapter);
    if (client->engine != NULL) {
        rqc_engine_destroy(client->engine);
    }
    demo_hq_adapter_destroy(client->hq_adapter);
    (void)demo_client_close_output(client);
    demo_udp_write_ctx_cleanup(&client->conn.write_ctx);
    demo_udp_socket_close(&client->udp_socket);
    demo_event_runtime_cleanup(&client->event_runtime);
}

int
main(int argc, char **argv)
{
    demo_client_t client;
    int arg_ret;

    memset(&client, 0, sizeof(client));
    demo_udp_socket_init(&client.udp_socket);

    arg_ret = demo_client_parse_args(argc, argv, &client);
    if (arg_ret > 0) {
        return 0;
    }
    if (arg_ret < 0) {
        return 1;
    }

    if (demo_event_runtime_init(&client.event_runtime) != 0
        || demo_client_init_engine(&client) != 0
        || demo_udp_resolve_peer(client.host, client.port, &client.peer) != 0
        || demo_udp_open_client(&client.udp_socket, &client.peer) != 0
        || demo_client_open_output(&client) != 0)
    {
        fprintf(stderr, "failed to initialize demo client\n");
        demo_client_cleanup(&client);
        return 1;
    }

    client.read_ctx.socket = &client.udp_socket;
    client.read_ctx.engine = client.engine;
    client.read_ctx.packet_user_data = &client.conn.write_ctx;
    demo_event_runtime_set_read(&client.event_runtime, client.udp_socket.fd,
        demo_udp_read_event_cb, &client.read_ctx);

    if (demo_client_start_request(&client) != 0) {
        demo_client_cleanup(&client);
        return 1;
    }

    if (demo_event_runtime_run(&client.event_runtime) != 0) {
        client.exit_code = 1;
    }

    demo_client_cleanup(&client);
    return client.exit_code;
}
