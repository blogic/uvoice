// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Double-buffer publisher for a plain-old-data snapshot (a C11 seqlock over
 * plain fields is a data race). One writer fills the slot the readers are not
 * using and flips an atomic index; a reader holds a count while it copies and
 * the writer skips that publish, so no slot is ever written and read at the
 * same time and no reader sees a mix of two updates.
 *
 * UDSP_SNAP_DECLARE(name, type) defines struct name plus name_publish() and
 * name_get(). Every record that one thread publishes for readers on other
 * threads instantiates it.
 */
#ifndef UDSP_CORE_SNAP_H
#define UDSP_CORE_SNAP_H

#include <stdatomic.h>
#include <stdbool.h>

#define UDSP_SNAP_DECLARE(name, type)					\
struct name {								\
	type slot[2];							\
	atomic_uint idx;						\
	atomic_uint readers;						\
};									\
									\
static inline bool name##_publish(struct name *s, const type *in)	\
{									\
	unsigned next = atomic_load(&s->idx) ^ 1u;			\
									\
	if (atomic_load(&s->readers) != 0)				\
		return false;						\
	s->slot[next] = *in;						\
	atomic_store(&s->idx, next);					\
									\
	return true;							\
}									\
									\
static inline void name##_get(struct name *s, type *out)		\
{									\
	atomic_fetch_add(&s->readers, 1u);				\
	*out = s->slot[atomic_load(&s->idx)];				\
	atomic_fetch_sub(&s->readers, 1u);				\
}									\
struct name

#endif
