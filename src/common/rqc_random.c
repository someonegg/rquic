/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */
#if (defined _WIN32) || (defined _WIN64)
#define _CRT_RAND_S
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>

#include <include/rquic/rquic_typedef.h>
#include "src/common/rqc_random.h"
#include "src/common/rqc_str.h"
#include "src/common/rqc_log.h"

#if !defined(RQC_SYS_WINDOWS) || defined(RQC_ON_MINGW)
#include <unistd.h>
#endif

#define RQC_RANDOM_BUFFER_SIZE 4096

long rqc_random(void) {
#ifdef RQC_SYS_WINDOWS
    unsigned int  val = 0;
    if (rand_s(&val) != 0) {
        val = rand();
    }
    return (long)(val & 0x7FFFFFFF);
#else
    return random();
#endif

}

rqc_random_generator_t *
rqc_random_generator_create(rqc_log_t *log)
{
    rqc_random_generator_t *rand_gen = rqc_malloc(sizeof(rqc_random_generator_t));
    if (rand_gen == NULL) {
        return NULL;
    }

    rqc_memzero(rand_gen, sizeof(rqc_random_generator_t));
    rand_gen->rand_buf.data = rqc_malloc(RQC_RANDOM_BUFFER_SIZE);
    if (rand_gen->rand_buf.data == NULL) {
        rqc_free(rand_gen);
        return NULL;
    }
    rand_gen->rand_buf_size = RQC_RANDOM_BUFFER_SIZE;
    rand_gen->rand_fd = -1;
    rand_gen->log = log;
#ifdef RQC_SYS_WINDOWS
    rand_gen->hProvider = 0;
    int res = CryptAcquireContextW(&rand_gen->hProvider, 0, 0, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT);
    assert(res);
#endif
    return rand_gen;
}

void
rqc_random_generator_destroy(rqc_random_generator_t *rand_gen)
{
#ifdef RQC_SYS_WINDOWS
    CryptReleaseContext(rand_gen->hProvider, 0);
#else
    if (rand_gen->rand_fd != -1) {
        close(rand_gen->rand_fd);
    }
#endif
    rqc_free(rand_gen->rand_buf.data);
    rqc_free(rand_gen);
}

rqc_int_t
rqc_get_random(rqc_random_generator_t *rand_gen, u_char *buf, size_t need_len)
{
#ifndef RQC_SYS_WINDOWS
    size_t total_read = 0;
    ssize_t bytes_read = 0;

    if ((size_t)rand_gen->rand_buf_offset >= rand_gen->rand_buf.len
        || rand_gen->rand_buf.len - (size_t)rand_gen->rand_buf_offset <= need_len) {

        /* not enough in rand_buf */
        if (rand_gen->rand_fd == -1) {
            rand_gen->rand_fd = open("/dev/urandom", O_RDONLY|O_NONBLOCK);
            if (rand_gen->rand_fd == -1) {
                rqc_log(rand_gen->log, RQC_LOG_WARN, "|random|can not open /dev/urandom|\n");
                return RQC_ERROR;
            }
        }

        total_read = 0;

        while (total_read < rand_gen->rand_buf_size) {

            bytes_read = read(rand_gen->rand_fd,
                              rand_gen->rand_buf.data + total_read,
                              rand_gen->rand_buf_size - total_read);

            if (bytes_read == -1) {
                if (errno == EINTR) {
                    continue;
                }

                if (errno == EAGAIN) {
                    break;
                }
            }

            if (bytes_read <= 0) {

                rqc_log(rand_gen->log, RQC_LOG_WARN, "|random|fail to read bytes from /dev/urandom|");

                close(rand_gen->rand_fd);
                rand_gen->rand_fd = -1;
                break;
            }

            total_read += bytes_read;
        }

        if (total_read < need_len) {
            rqc_log(rand_gen->log, RQC_LOG_WARN,
                    "|random|can not generate rand buf|%zu|%zu|", total_read, need_len);
            return RQC_ERROR;
        }

        rand_gen->rand_buf_offset = 0;
        rand_gen->rand_buf.len = total_read;
    }

    rqc_memcpy(buf, rand_gen->rand_buf.data + rand_gen->rand_buf_offset, need_len);

    rand_gen->rand_buf_offset += need_len;
#else
    DWORD result = CryptGenRandom(rand_gen->hProvider, need_len, buf);
    assert(result);
#endif
    return RQC_OK;
}
