/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef TEST_COMMON_H
#define TEST_COMMON_H

/* definition for connection */
#define DEFAULT_SERVER_ADDR "127.0.0.1"
#define DEFAULT_SERVER_PORT 8443

#define PATH_LEN            1024
#define RESOURCE_LEN        1024
#define AUTHORITY_LEN       128
#define URL_LEN             1024

/* request method */
typedef enum request_method_e {
    REQUEST_METHOD_GET,
    REQUEST_METHOD_POST,
} REQUEST_METHOD;

char method_s[][16] = {
    {"GET"},
    {"POST"}
};

const char *line_break = "\n";

static size_t READ_FILE_BUF_LEN = 2 *1024 * 1024;

#define DEBUG ;
// #define DEBUG printf("%s:%d (%s)\n",__FILE__, __LINE__ ,__FUNCTION__);

extern long xqc_random(void);
extern xqc_usec_t xqc_now();

int
xqc_demo_read_file_data(char * data, size_t data_len, char *filename)
{
    int ret = 0;
    size_t total_len, read_len;
    FILE *fp = fopen(filename, "rb");
    if (fp == NULL) {
        ret = -1;
        goto end;
    }

    fseek(fp, 0 , SEEK_END);
    total_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (total_len > data_len) {
        ret = -1;
        goto end;
    }

    read_len = fread(data, 1, total_len, fp);
    if (read_len != total_len) {
        ret = -1;
        goto end;
    }

    ret = read_len;

end:
    if (fp) {
        fclose(fp);
    }
    return ret;
}

#endif
