// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Single-producer single-consumer ring, one macro for the identical control,
 * net and slic rings. Each side loads its own index relaxed, loads the other
 * index acquire, and stores its own index release, so the ring takes no lock.
 * A full ring drops the push and returns false. The producer writes a slot
 * before it publishes the head, and the consumer reads a slot only after it
 * observes the head, so a slot is never read before it is fully written.
 *
 * UDSP_SPSC_DECLARE(name, type, depth) defines struct name plus name_init,
 * name_push and name_pop. depth stays a macro argument so the modulo folds to a
 * constant. A ring whose producer fills a slot from scalars keeps its own push;
 * a ring that also counts a dropped record (core/event.c, core/jb_trace.h)
 * keeps its own form.
 *
 * UDSP_SPSC_HEAP_DECLARE(name, type) is the same ring with a depth set at run
 * time, a power of two, so the index wraps with a mask. The caller allocates
 * name_bytes(depth) and calls name_init. The producer writes a slot in place:
 * name_claim returns the head slot, or NULL on a full ring, and name_publish
 * stores the head. The form serves a record too large to build on the stack
 * of a real-time thread and copy again. It also serves a ring that a test
 * shrinks.
 */
#ifndef UDSP_CORE_SPSC_H
#define UDSP_CORE_SPSC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#define UDSP_SPSC_DECLARE(name, type, depth)				\
struct name {								\
	type slot[depth];						\
	atomic_uint head;						\
	atomic_uint tail;						\
};									\
									\
static inline void name##_init(struct name *r)				\
{									\
	memset(r, 0, sizeof(*r));					\
	atomic_init(&r->head, 0u);					\
	atomic_init(&r->tail, 0u);					\
}									\
									\
static inline bool name##_push(struct name *r, const type *v)		\
{									\
	unsigned head = atomic_load_explicit(&r->head,			\
					     memory_order_relaxed);	\
	unsigned next = (head + 1u) % (depth);				\
									\
	if (next == atomic_load_explicit(&r->tail,			\
					 memory_order_acquire))		\
		return false;						\
	r->slot[head] = *v;						\
	atomic_store_explicit(&r->head, next, memory_order_release);	\
									\
	return true;							\
}									\
									\
static inline bool name##_pop(struct name *r, type *out)		\
{									\
	unsigned tail = atomic_load_explicit(&r->tail,			\
					     memory_order_relaxed);	\
									\
	if (tail == atomic_load_explicit(&r->head,			\
					 memory_order_acquire))		\
		return false;						\
	*out = r->slot[tail];						\
	atomic_store_explicit(&r->tail, (tail + 1u) % (depth),		\
			      memory_order_release);			\
									\
	return true;							\
}									\
struct name

#define UDSP_SPSC_HEAP_DECLARE(name, type)				\
struct name {								\
	atomic_uint head;						\
	atomic_uint tail;						\
	unsigned mask;							\
	type slot[];							\
};									\
									\
static inline size_t name##_bytes(unsigned depth)			\
{									\
	return sizeof(struct name) + (size_t)depth * sizeof(type);	\
}									\
									\
static inline void name##_init(struct name *r, unsigned depth)		\
{									\
	memset(r, 0, name##_bytes(depth));				\
	atomic_init(&r->head, 0u);					\
	atomic_init(&r->tail, 0u);					\
	r->mask = depth - 1u;						\
}									\
									\
static inline type *name##_claim(struct name *r)			\
{									\
	unsigned head = atomic_load_explicit(&r->head,			\
					     memory_order_relaxed);	\
									\
	if (((head + 1u) & r->mask) ==					\
	    atomic_load_explicit(&r->tail, memory_order_acquire))	\
		return NULL;						\
									\
	return &r->slot[head];						\
}									\
									\
static inline void name##_publish(struct name *r)			\
{									\
	unsigned head = atomic_load_explicit(&r->head,			\
					     memory_order_relaxed);	\
									\
	atomic_store_explicit(&r->head, (head + 1u) & r->mask,		\
			      memory_order_release);			\
}									\
									\
static inline bool name##_pop(struct name *r, type *out)		\
{									\
	unsigned tail = atomic_load_explicit(&r->tail,			\
					     memory_order_relaxed);	\
									\
	if (tail == atomic_load_explicit(&r->head,			\
					 memory_order_acquire))		\
		return false;						\
	*out = r->slot[tail];						\
	atomic_store_explicit(&r->tail, (tail + 1u) & r->mask,		\
			      memory_order_release);			\
									\
	return true;							\
}									\
struct name

#endif
