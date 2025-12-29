/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_MALLOC_H_INCLUDED_
#define _RQC_MALLOC_H_INCLUDED_

#include <stdlib.h>
#include <stdio.h>

#ifdef PRINT_MALLOC
extern FILE *g_malloc_info_fp;
#include <unistd.h>
#define rqc_init_print() \
do { \
    if (g_malloc_info_fp) break; \
    char buff[1024]; \
    snprintf(buff, sizeof(buff), "malloc_free_%d", getpid()); \
    g_malloc_info_fp = fopen(buff, "w+");\
} while(0)
#endif

#ifdef PRINT_MALLOC
#define rqc_malloc(size) ({\
    rqc_init_print();\
    void *p = malloc((size));\
    fprintf(g_malloc_info_fp, "PRINT_MALLOC %p %zu %s:%d\n", p, (size_t)(size), __FILE__, __LINE__);\
    (p);\
    })
#else
static inline void *
rqc_malloc(size_t size)
{
    return malloc(size);
}
#endif

#ifdef PRINT_MALLOC
#define rqc_calloc(count, size) ({\
    rqc_init_print();\
    void *p = calloc(count, size);\
    fprintf(g_malloc_info_fp, "PRINT_MALLOC %p %zu %s:%d\n", p, size, __FILE__, __LINE__);\
    (p);\
    })
#else
static inline void *
rqc_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}
#endif

#ifdef PRINT_MALLOC
#define rqc_realloc(ptr, size) ({\
    rqc_init_print();\
    void *p = realloc(ptr, size); \
    fprintf(g_malloc_info_fp, "PRINT_FREE %p\n", ptr); \
    fprintf(g_malloc_info_fp, "PRINT_MALLOC %p %zu %s:%d\n", p, size, __FILE__, __LINE__);\
    (p);\
    })
#else
static inline void *
rqc_realloc(void *ptr, size_t size)
{
    return realloc(ptr, size);
}
#endif

static inline void
rqc_free(void *ptr)
{
#ifdef PRINT_MALLOC
    rqc_init_print();\
    fprintf(g_malloc_info_fp, "PRINT_FREE %p\n", ptr);
    free(ptr);
    return;
#endif
    free(ptr);
}

typedef void *(*rqc_malloc_wrap_t)(void *opaque, size_t size);
typedef void (*rqc_free_wrap_t)(void *opaque, void *ptr);

typedef struct rqc_allocator_s {
    rqc_malloc_wrap_t   malloc;
    rqc_free_wrap_t     free;
    void               *opaque;
} rqc_allocator_t;

static inline void *
rqc_malloc_wrap_default(void *opaque, size_t size)
{
    (void)opaque;
    return rqc_malloc(size);
}

static inline void
rqc_free_wrap_default(void *opaque, void *ptr)
{
    (void)opaque;
    rqc_free(ptr);
}

static rqc_allocator_t rqc_default_allocator = {
    &rqc_malloc_wrap_default,
    &rqc_free_wrap_default,
    NULL
};

#endif /*_RQC_MALLOC_H_INCLUDED_*/
