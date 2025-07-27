/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */

#ifndef _RQC_H_LIST_INCLUDE_
#define _RQC_H_LIST_INCLUDE_

#include <stddef.h>
#include <assert.h>
#include "rqc_common.h"

#define RQC_LIST_POISON1  ((void *) 0x1)
#define RQC_LIST_POISON2  ((void *) 0x2)

/* the structure can be chained with a linked list by embedding rqc_list_head_t
 *
 * Interfaces:
 * rqc_list_head_init(), rqc_init_list_head()
 * rqc_list_add(), rqc_list_add_tail()
 * rqc_list_del(), rqc_list_del_init()
 * rqc_list_replace()
 * rqc_list_empty()
 * rqc_list_for_each(), rqc_list_for_each_safe()
 * rqc_list_entry()
 * rqc_list_splice(), rqc_list_splice_init()
 * rqc_list_splice_tail(), rqc_list_splice_tail_init()
 */

typedef struct rqc_list_head_s {
    struct rqc_list_head_s *prev;
    struct rqc_list_head_s *next;
} rqc_list_head_t;

#define rqc_list_head_init(name) { &(name), &(name) }

#if GNU11
# define container_of(ptr, type, member) ({                  \
    const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
    (type *)( (char *)__mptr - offsetof(type,member) );})
#else
#define container_of(ptr, type, member) (type *)( (char *)ptr - offsetof(type, member) )
#endif

#define rqc_list_entry(ptr, type, member) container_of(ptr, type, member)

static inline void
rqc_init_list_head(rqc_list_head_t *list)
{
    list->prev = list;
    list->next = list;
}

static inline void
__rqc_list_add(rqc_list_head_t *node, rqc_list_head_t *prev, rqc_list_head_t *next)
{
#if (RQC_DEBUG)
    assert(next->prev == prev && prev->next == next && node != prev && node != next);
#endif

    next->prev = node;
    node->next = next;
    node->prev = prev;
    prev->next = node;
}

static inline void
rqc_list_add(rqc_list_head_t *node, rqc_list_head_t *head)
{
    __rqc_list_add(node, head, head->next);
}

static inline void
rqc_list_add_tail(rqc_list_head_t *node, rqc_list_head_t *head)
{
    __rqc_list_add(node, head->prev, head);
}

static inline void
__rqc_list_del(rqc_list_head_t *prev, rqc_list_head_t *next)
{
    next->prev = prev;
    prev->next = next;
}

static inline void
__rqc_list_del_entry(rqc_list_head_t *entry)
{
#if (RQC_DEBUG)
    rqc_list_head_t *prev, *next;

    prev = entry->prev;
    next = entry->next;

    assert(prev != NULL && next != NULL);
    assert(next != RQC_LIST_POISON1
           && prev != RQC_LIST_POISON2
           && prev->next == entry
           && next->prev == entry);
#endif

    __rqc_list_del(entry->prev, entry->next);
}

static inline void
rqc_list_del(rqc_list_head_t *entry)
{
    __rqc_list_del_entry(entry);
    entry->next = RQC_LIST_POISON1;
    entry->prev = RQC_LIST_POISON2;
}

static inline void
rqc_list_del_init(rqc_list_head_t *entry)
{
    __rqc_list_del_entry(entry);
    rqc_init_list_head(entry);
}

static inline void
rqc_list_replace(rqc_list_head_t *old, rqc_list_head_t *node)
{
    node->next = old->next;
    node->next->prev = node;
    node->prev = old->prev;
    node->prev->next = node;
}

static inline int
rqc_list_empty(const rqc_list_head_t *head)
{
    return head->next == head;
}

static inline rqc_bool_t
rqc_list_is_inited(const rqc_list_head_t *head)
{
    return head->next != NULL;
}

static inline void
__rqc_list_splice(const rqc_list_head_t *list,
    rqc_list_head_t *prev, rqc_list_head_t *next)
{
    rqc_list_head_t *first = list->next;
    rqc_list_head_t *last = list->prev;

    first->prev = prev;
    prev->next = first;

    last->next = next;
    next->prev = last;
}

static inline void
rqc_list_splice(const rqc_list_head_t *list, rqc_list_head_t *head)
{
    if (!rqc_list_empty(list)) {
        __rqc_list_splice(list, head, head->next);
    }
}

static inline void
rqc_list_splice_tail(rqc_list_head_t *list, rqc_list_head_t *head)
{
    if (!rqc_list_empty(list)) {
        __rqc_list_splice(list, head->prev, head);
    }
}

static inline void
rqc_list_splice_init(rqc_list_head_t *list, rqc_list_head_t *head)
{
    if (!rqc_list_empty(list)) {
        __rqc_list_splice(list, head, head->next);
        rqc_init_list_head(list);
    }
}

static inline void
rqc_list_splice_tail_init(rqc_list_head_t *list, rqc_list_head_t *head)
{
    if (!rqc_list_empty(list)) {
        __rqc_list_splice(list, head->prev, head);
        rqc_init_list_head(list);
    }
}

#define rqc_list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

#define rqc_list_for_each_from(pos, head) \
    for (; pos != (head); pos = pos->next)

#define rqc_list_for_each_reverse(pos, head) \
    for (pos = (head)->prev; pos != (head); pos = pos->prev)

#define rqc_list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; \
        pos != (head); \
        pos = n, n = pos->next)

#define rqc_list_for_each_reverse_safe(pos, n, head) \
    for (pos = (head)->prev, n = pos->prev; \
        pos != (head); \
        pos = n, n = pos->prev)

#endif /*_RQC_H_LIST_INCLUDE_*/
