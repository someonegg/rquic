#include "demo_common.h"
#include "demo_event.h"
#include "demo_hq.h"
#include "demo_udp.h"

#include <rquic/rqc_errno.h>
#include <rquic/rquic.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct demo_server_s demo_server_t;

typedef struct demo_server_conn_s {
    demo_udp_write_ctx_t write_ctx;
    demo_server_t *server;
} demo_server_conn_t;

#define DEMO_FILE_CHUNK_SIZE 16384

typedef struct demo_server_stream_s {
    demo_hq_stream_t *stream;
    demo_server_t *server;
    FILE *file;
    unsigned char file_buf[DEMO_FILE_CHUNK_SIZE];
    size_t file_buf_len;
    size_t file_buf_off;
    int file_eof;
    int file_response;
    int file_done;
    int response_ready;
} demo_server_stream_t;

struct demo_server_s {
    const char *host;
    unsigned short port;
    const char *www_root;
    rqc_log_level_t log_level;

    demo_event_runtime_t event_runtime;
    rqc_engine_t *engine;
    demo_hq_adapter_t *hq_adapter;

    demo_udp_socket_t udp_socket;
    demo_udp_write_ctx_t write_ctx;
    demo_udp_read_ctx_t read_ctx;
};

static void
demo_server_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [--host ADDR] [--port PORT] [--www-root DIR] [--log-level LEVEL]\n"
        "\n"
        "Defaults: --host %s --port %d --log-level error\n",
        prog, DEMO_DEFAULT_HOST, DEMO_DEFAULT_PORT);
}

static int
demo_server_parse_args(int argc, char **argv, demo_server_t *server)
{
    server->host = DEMO_DEFAULT_HOST;
    server->port = DEMO_DEFAULT_PORT;
    server->log_level = RQC_LOG_ERROR;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            demo_server_usage(argv[0]);
            return 1;
        }
        if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            server->host = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            if (demo_parse_port(argv[++i], &server->port) != 0) {
                fprintf(stderr, "invalid --port value\n");
                return -1;
            }
            continue;
        }
        if (strcmp(argv[i], "--www-root") == 0 && i + 1 < argc) {
            server->www_root = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            if (demo_parse_log_level(argv[++i], &server->log_level) != 0) {
                fprintf(stderr, "invalid --log-level value\n");
                return -1;
            }
            continue;
        }
        fprintf(stderr, "unknown or incomplete option: %s\n", argv[i]);
        demo_server_usage(argv[0]);
        return -1;
    }

    if (server->www_root != NULL) {
        struct stat st;
        if (stat(server->www_root, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "invalid --www-root: not a directory\n");
            return -1;
        }
    }

    return 0;
}

static int
demo_server_accept(rqc_engine_t *engine, rqc_connection_t *conn,
    const rqc_cid_t *cid, void *user_data)
{
    demo_udp_write_ctx_t *shared_write_ctx = user_data;
    demo_server_t *server = shared_write_ctx != NULL ? shared_write_ctx->user_data : NULL;
    demo_server_conn_t *server_conn;

    if (server == NULL) {
        return -1;
    }

    server_conn = calloc(1, sizeof(*server_conn));
    if (server_conn == NULL) {
        return -1;
    }

    server_conn->server = server;
    server_conn->write_ctx.fd = server->udp_socket.fd;
    server_conn->write_ctx.fd4 = server->udp_socket.fd;
    server_conn->write_ctx.fd6 = server->udp_socket.fd;
    server_conn->write_ctx.write_watch.fd = -1;
    server_conn->write_ctx.runtime = &server->event_runtime;
    server_conn->write_ctx.engine = engine;
    memcpy(&server_conn->write_ctx.cid, cid, sizeof(server_conn->write_ctx.cid));
    server_conn->write_ctx.has_cid = 1;
    server_conn->write_ctx.user_data = server_conn;
    rqc_conn_set_transport_user_data(conn, &server_conn->write_ctx);
    return 0;
}

static void
demo_server_refuse(rqc_engine_t *engine, rqc_connection_t *conn,
    const rqc_cid_t *cid, void *user_data)
{
    demo_udp_write_ctx_t *write_ctx = user_data;
    demo_server_conn_t *server_conn = write_ctx != NULL ? write_ctx->user_data : NULL;

    (void)engine;
    (void)conn;
    (void)cid;

    if (server_conn != NULL) {
        demo_udp_write_ctx_cleanup(&server_conn->write_ctx);
        free(server_conn);
    }
}

static int
demo_server_hq_conn_create(demo_hq_conn_t *conn, const rqc_cid_t *cid,
    void *conn_user_data)
{
    (void)cid;
    demo_udp_write_ctx_t *write_ctx = conn_user_data;
    demo_server_conn_t *server_conn = write_ctx != NULL ? write_ctx->user_data : NULL;

    if (server_conn != NULL) {
        demo_hq_conn_set_user_data(conn, server_conn);
    }

    return 0;
}

static int
demo_server_hq_conn_close(demo_hq_conn_t *conn, const rqc_cid_t *cid,
    void *conn_user_data)
{
    (void)cid;
    (void)conn_user_data;

    demo_server_conn_t *server_conn = demo_hq_conn_get_user_data(conn);
    if (server_conn != NULL) {
        demo_udp_write_ctx_cleanup(&server_conn->write_ctx);
        free(server_conn);
    }

    return 0;
}

static int
demo_server_hq_stream_create(demo_hq_stream_t *stream, void *stream_user_data)
{
    demo_server_conn_t *server_conn;
    demo_server_stream_t *server_stream;

    (void)stream_user_data;

    server_stream = calloc(1, sizeof(*server_stream));
    if (server_stream == NULL) {
        return -1;
    }

    server_stream->stream = stream;
    server_conn = demo_hq_stream_get_conn_user_data(stream);
    server_stream->server = server_conn != NULL ? server_conn->server : NULL;
    demo_hq_stream_set_user_data(stream, server_stream);
    return 0;
}

static int
demo_server_hq_stream_close(demo_hq_stream_t *stream, void *stream_user_data)
{
    demo_server_stream_t *server_stream = stream_user_data;

    (void)stream;
    if (server_stream != NULL && server_stream->file != NULL) {
        fclose(server_stream->file);
    }
    free(server_stream);
    return 0;
}

static int
demo_server_set_text_response(demo_server_stream_t *server_stream,
    const char *text, size_t text_len)
{
    if (demo_hq_stream_set_response(server_stream->stream,
            (const unsigned char *)text, text_len) != RQC_OK)
    {
        return -1;
    }

    server_stream->response_ready = 1;
    return 0;
}

static int
demo_server_prepare_response(demo_server_stream_t *server_stream,
    const char *resource)
{
    char response[512];
    int written;

    written = snprintf(response, sizeof(response),
        "rquic demo response\nresource: %s\n", resource);
    if (written < 0 || (size_t)written >= sizeof(response)) {
        return -1;
    }

    return demo_server_set_text_response(server_stream, response, (size_t)written);
}

static int
demo_server_prepare_file_error(demo_server_stream_t *server_stream,
    const char *resource, const char *message)
{
    char response[512];
    int written;

    written = snprintf(response, sizeof(response),
        "rquic demo file error\nresource: %s\nerror: %s\n", resource, message);
    if (written < 0 || (size_t)written >= sizeof(response)) {
        return -1;
    }

    return demo_server_set_text_response(server_stream, response, (size_t)written);
}

static int
demo_server_path_within_root(const char *root, const char *path)
{
    char root_real[PATH_MAX];
    char path_real[PATH_MAX];
    size_t root_len;

    if (realpath(root, root_real) == NULL || realpath(path, path_real) == NULL) {
        return 0;
    }

    root_len = strlen(root_real);
    if (strncmp(root_real, path_real, root_len) != 0) {
        return 0;
    }

    return path_real[root_len] == '\0' || path_real[root_len] == '/';
}

static int
demo_server_prepare_file_response(demo_server_stream_t *server_stream,
    const char *resource)
{
    demo_server_t *server = server_stream->server;
    const char *relative = resource[0] == '/' ? resource + 1 : resource;
    char *path;
    size_t path_len;
    struct stat st;

    if (server == NULL || server->www_root == NULL) {
        return demo_server_prepare_response(server_stream, resource);
    }

    path_len = strlen(server->www_root) + 1 + strlen(relative) + 1;
    path = malloc(path_len);
    if (path == NULL) {
        return -1;
    }

    (void)snprintf(path, path_len, "%s/%s", server->www_root, relative);

    if (!demo_server_path_within_root(server->www_root, path)) {
        free(path);
        return demo_server_prepare_file_error(server_stream, resource,
            "file does not exist under --www-root");
    }

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        free(path);
        return demo_server_prepare_file_error(server_stream, resource,
            "resource is not a regular file");
    }

    server_stream->file = fopen(path, "rb");
    free(path);
    if (server_stream->file == NULL) {
        return demo_server_prepare_file_error(server_stream, resource,
            "failed to open file");
    }

    server_stream->file_response = 1;
    server_stream->response_ready = 1;
    return 0;
}

static int
demo_server_finish_file_response(demo_server_stream_t *server_stream)
{
    if (server_stream->file != NULL) {
        fclose(server_stream->file);
        server_stream->file = NULL;
    }
    server_stream->file_done = 1;
    return 0;
}

static int
demo_server_send_file_response(demo_server_stream_t *server_stream)
{
    ssize_t ret;

    while (!server_stream->file_done) {
        if (server_stream->file_buf_off < server_stream->file_buf_len) {
            size_t left = server_stream->file_buf_len - server_stream->file_buf_off;
            uint8_t fin = server_stream->file_eof ? 1 : 0;

            ret = demo_hq_stream_send_response_chunk(server_stream->stream,
                server_stream->file_buf + server_stream->file_buf_off, left, fin);
            if (ret == -RQC_EAGAIN) {
                return 0;
            }
            if (ret < 0) {
                return -1;
            }
            if (ret == 0) {
                return 0;
            }

            server_stream->file_buf_off += (size_t)ret;
            if (server_stream->file_buf_off == server_stream->file_buf_len
                && server_stream->file_eof)
            {
                return demo_server_finish_file_response(server_stream);
            }
            continue;
        }

        server_stream->file_buf_len = fread(server_stream->file_buf, 1,
            sizeof(server_stream->file_buf), server_stream->file);
        server_stream->file_buf_off = 0;
        server_stream->file_eof = feof(server_stream->file) ? 1 : 0;

        if (server_stream->file_buf_len > 0) {
            continue;
        }
        if (ferror(server_stream->file)) {
            return -1;
        }

        ret = demo_hq_stream_send_response_chunk(server_stream->stream,
            (const unsigned char *)"", 0, 1);
        if (ret == -RQC_EAGAIN) {
            return 0;
        }
        if (ret < 0) {
            return -1;
        }
        return demo_server_finish_file_response(server_stream);
    }

    return 0;
}

static void
demo_server_finish_send_batch(demo_server_stream_t *server_stream)
{
    rqc_engine_finish_send(server_stream->server->engine);
}

static int
demo_server_hq_stream_read(demo_hq_stream_t *stream, void *stream_user_data)
{
    demo_server_stream_t *server_stream = stream_user_data;
    char resource[DEMO_PACKET_BUF_SIZE];
    uint8_t fin = 0;
    ssize_t ret;

    if (server_stream == NULL) {
        return -1;
    }

    ret = demo_hq_stream_recv_request(stream, resource, sizeof(resource), &fin);
    if (ret < 0) {
        fprintf(stderr, "server failed to read request: %zd\n", ret);
        return -1;
    }

    if (fin && !server_stream->response_ready) {
        if (demo_server_prepare_file_response(server_stream, resource) != 0) {
            return -1;
        }
        if (server_stream->file_response) {
            ret = demo_server_send_file_response(server_stream);
            demo_server_finish_send_batch(server_stream);
            if (ret != 0) {
                fprintf(stderr, "server failed to send file response\n");
                return -1;
            }
        } else {
            ret = demo_hq_stream_send_pending_response(stream);
            if (ret < 0 && ret != -RQC_EAGAIN) {
                fprintf(stderr, "server failed to send response: %zd\n", ret);
                return -1;
            }
        }
    }

    return 0;
}

static int
demo_server_hq_stream_write(demo_hq_stream_t *stream, void *stream_user_data)
{
    demo_server_stream_t *server_stream = stream_user_data;
    ssize_t ret;

    if (server_stream == NULL || !server_stream->response_ready) {
        return 0;
    }

    if (server_stream->file_response) {
        ret = demo_server_send_file_response(server_stream);
        demo_server_finish_send_batch(server_stream);
        if (ret != 0) {
            fprintf(stderr, "server failed to continue file response\n");
            return -1;
        }
        return 0;
    }

    ret = demo_hq_stream_send_pending_response(stream);
    if (ret < 0 && ret != -RQC_EAGAIN) {
        fprintf(stderr, "server failed to continue response: %zd\n", ret);
        return -1;
    }

    return 0;
}

static int
demo_server_init_engine(demo_server_t *server)
{
    rqc_config_t config;
    rqc_engine_callback_t engine_cb;
    rqc_transport_callbacks_t transport_cb;
    rqc_conn_settings_t conn_settings;
    demo_hq_callbacks_t hq_cb;

    if (rqc_engine_get_default_config(&config, RQC_ENGINE_SERVER) != RQC_OK) {
        return -1;
    }
    config.cfg_log_level = server->log_level;

    memset(&engine_cb, 0, sizeof(engine_cb));
    engine_cb.set_event_timer = demo_set_event_timer;
    engine_cb.log_callbacks = demo_log_callbacks;

    memset(&transport_cb, 0, sizeof(transport_cb));
    transport_cb.server_accept = demo_server_accept;
    transport_cb.server_refuse = demo_server_refuse;
    transport_cb.write_socket = demo_udp_write_socket;

    server->engine = rqc_engine_create(RQC_ENGINE_SERVER, &config, &engine_cb,
        &transport_cb, &server->event_runtime);
    if (server->engine == NULL) {
        return -1;
    }
    demo_event_runtime_set_engine(&server->event_runtime, server->engine);

    memset(&conn_settings, 0, sizeof(conn_settings));
    conn_settings.cong_ctrl_callback = rqc_bbr_cb;
    conn_settings.cc_params.customize_on = 1;
    conn_settings.cc_params.init_cwnd = 32;
    conn_settings.proto_version = RQC_VERSION_V1;
    conn_settings.init_idle_time_out = 60000;
    conn_settings.idle_time_out = 60000;
    conn_settings.max_pkt_out_size = 1350;
    conn_settings.adaptive_ack_frequency = 1;
    rqc_server_set_conn_settings(server->engine, &conn_settings);

    memset(&hq_cb, 0, sizeof(hq_cb));
    hq_cb.conn_create = demo_server_hq_conn_create;
    hq_cb.conn_close = demo_server_hq_conn_close;
    hq_cb.stream_create = demo_server_hq_stream_create;
    hq_cb.stream_close = demo_server_hq_stream_close;
    hq_cb.stream_read = demo_server_hq_stream_read;
    hq_cb.stream_write = demo_server_hq_stream_write;

    server->hq_adapter = demo_hq_register(server->engine, &hq_cb);
    return server->hq_adapter != NULL ? 0 : -1;
}

static void
demo_server_cleanup(demo_server_t *server)
{
    demo_hq_adapter_unregister(server->hq_adapter);
    if (server->engine != NULL) {
        rqc_engine_destroy(server->engine);
    }
    demo_hq_adapter_destroy(server->hq_adapter);
    demo_udp_write_ctx_cleanup(&server->write_ctx);
    demo_udp_socket_close(&server->udp_socket);
    demo_event_runtime_cleanup(&server->event_runtime);
}

int
main(int argc, char **argv)
{
    demo_server_t server;
    int arg_ret;

    memset(&server, 0, sizeof(server));
    demo_udp_socket_init(&server.udp_socket);

    arg_ret = demo_server_parse_args(argc, argv, &server);
    if (arg_ret > 0) {
        return 0;
    }
    if (arg_ret < 0) {
        return 1;
    }

    if (demo_event_runtime_init(&server.event_runtime) != 0
        || demo_server_init_engine(&server) != 0
        || demo_udp_bind(&server.udp_socket, server.host, server.port) != 0)
    {
        fprintf(stderr, "failed to initialize demo server\n");
        demo_server_cleanup(&server);
        return 1;
    }

    server.write_ctx.fd = server.udp_socket.fd;
    server.write_ctx.fd4 = server.udp_socket.fd;
    server.write_ctx.fd6 = server.udp_socket.fd;
    server.write_ctx.write_watch.fd = -1;
    server.write_ctx.runtime = &server.event_runtime;
    server.write_ctx.engine = server.engine;
    server.write_ctx.user_data = &server;

    server.read_ctx.socket = &server.udp_socket;
    server.read_ctx.engine = server.engine;
    server.read_ctx.packet_user_data = &server.write_ctx;
    demo_event_runtime_set_read(&server.event_runtime, server.udp_socket.fd,
        demo_udp_read_event_cb, &server.read_ctx);

    printf("demo_server listening on %s:%hu\n", server.host, server.port);
    fflush(stdout);
    if (demo_event_runtime_run(&server.event_runtime) != 0) {
        demo_server_cleanup(&server);
        return 1;
    }

    demo_server_cleanup(&server);
    return 0;
}
