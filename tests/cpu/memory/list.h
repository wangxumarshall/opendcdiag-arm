/*
 *  list.h - minimal Linux-kernel-style doubly-linked list, for SDCShield
 *  memory tests that need an intrusive MPSC priority queue (e.g. the
 *  GlusterFS IOT scheduler exercised by memcpy_rewr).
 *
 *  This is a self-contained, arch-independent header: no sysfs, no kernel,
 *  no x86/ARM specifics. The macros mirror include/linux/list.h so the
 *  ported application code (which #include "list.h") compiles unchanged.
 *
 *  SPDX-License-Identifier: Apache-2.0
 */
#ifndef SANDSTONE_LIST_H
#define SANDSTONE_LIST_H

#include <stddef.h>     /* offsetof */

struct list_head {
    struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }

#define LIST_HEAD(name) \
    struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list)
{
    list->next = list;
    list->prev = list;
}

/* ---- internal add helpers ---- */
static inline void __list_add(struct list_head *n,
                              struct list_head *prev,
                              struct list_head *next)
{
    next->prev = n;
    n->next = next;
    n->prev = prev;
    prev->next = n;
}

/* add at head (LIFO) */
static inline void list_add(struct list_head *n, struct list_head *head)
{
    __list_add(n, head, head->next);
}

/* add at tail (FIFO) */
static inline void list_add_tail(struct list_head *n, struct list_head *head)
{
    __list_add(n, head->prev, head);
}

/* ---- delete helpers ---- */
static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    entry->next = NULL;
    entry->prev = NULL;
}

static inline void list_del_init(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
    INIT_LIST_HEAD(entry);
}

/* ---- queries ---- */
static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

/*
 * Rotate the first entry to the tail of the list. Used by the IOT
 * scheduler to give per-client queues fair round-robin service.
 */
static inline void list_rotate_left(struct list_head *head)
{
    struct list_head *first;
    if (!list_empty(head) && head->next != head->prev) {
        first = head->next;
        list_del(first);
        list_add_tail(first, head);
    }
}

/* ---- container accessors ---- */
#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#define list_first_entry(ptr, type, member) \
    container_of((ptr)->next, type, member)

#define list_last_entry(ptr, type, member) \
    container_of((ptr)->prev, type, member)

/*
 * prefetchw is a GCC builtin; no arch dependency. Used by the IOT
 * scheduler to warm the cache line before an atomic increment.
 */
#ifndef prefetchw
#define prefetchw(x) __builtin_prefetch((x), 1)
#endif

#endif /* SANDSTONE_LIST_H */
