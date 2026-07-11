#ifndef DEMO_COMMON_H
#define DEMO_COMMON_H

#include <stddef.h>

#include <rquic/rquic.h>

#define DEMO_DEFAULT_HOST "127.0.0.1"
#define DEMO_DEFAULT_PORT 8443
#define DEMO_DEFAULT_PATH "/hello"
#define DEMO_PACKET_BUF_SIZE 65535
#define DEMO_SOCKET_BUF_SIZE (1024 * 1024)

extern const rqc_log_callbacks_t demo_log_callbacks;

int demo_parse_log_level(const char *value, rqc_log_level_t *level);
const char *demo_log_level_name(rqc_log_level_t level);
int demo_parse_port(const char *value, unsigned short *port);

int demo_validate_resource_path(const char *path, char *errbuf, size_t errbuf_size);

int demo_get_sys_errno(void);
void demo_set_sys_errno(int err);
const char *demo_strerror(int err);

rqc_usec_t demo_now(void);

#endif
