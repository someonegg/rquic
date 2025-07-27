/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef PLATFORM_H
#define PLATFORM_H

#if defined(_WIN64) || defined(WIN64) || defined(_WIN32) || defined(WIN32)
#define RQC_SYS_WINDOWS
#endif

#ifdef RQC_SYS_WINDOWS
# define EAGAIN  WSAEWOULDBLOCK
# define EINTR WSAEINTR
#endif

/**
 * @brief get system last errno
 *
 * @return int
 */
static inline int get_sys_errno()
{
    int err = 0;
#ifdef RQC_SYS_WINDOWS
    err = WSAGetLastError();
#else
    err = errno;
#endif
    return err;
}

static inline void set_sys_errno(int err)
{
#ifdef RQC_SYS_WINDOWS
    WSASetLastError(err);
#else
    errno = err;
#endif
}

/**
 * @brief init platform env if necessary
 *
 */
static inline void rqc_platform_init_env()
{
#ifdef RQC_SYS_WINDOWS
    int result = 0;
    // Initialize Winsock
    WSADATA wsaData;
    if ((result = WSAStartup(MAKEWORD(2, 2), &wsaData)) != 0) {
        printf("WSAStartup failed with error %d\n", result);
        exit(1);
    }
#endif
}
#endif
