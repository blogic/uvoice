// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The per-decision jitter buffer trace ring (udsp.h struct udsp_jb_trace).
 *
 * One producer is the engine tick, which pushes for every stream of every
 * line. One consumer is the caller of udsp_jb_trace_read. The shape and the
 * memory orders are those of core/event.c ev_ring_push and ev_ring_pop: each
 * side loads its own index relaxed, loads the other index acquire, and stores
 * its own index release. A full ring drops the new record and counts it, and
 * the next record that fits claims the count, so the gap the reader sees falls
 * where the loss happened instead of in front of the whole queue.
 *
 * The producer runs on the RT tick: it allocates nothing, locks nothing, logs
 * nothing and reads no clock of its own.
 */
#ifndef UDSP_CORE_JB_TRACE_H
#define UDSP_CORE_JB_TRACE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "udsp/udsp.h"

/* One slot is 40 bytes, so the ring costs 20 KiB per handle. A stream of
 * 10 ms packets writes at most two records per tick, so the ring holds about
 * 2.5 seconds of it, which is longer than a reader that polls once a second
 * needs. The worst case is 129 records per stream per tick; such a burst
 * fills the ring in four ticks, and the ring counts the loss. */
#define UDSP_JB_TRACE_SLOTS	512u

struct jb_trace_ring {
	struct udsp_jb_trace slot[UDSP_JB_TRACE_SLOTS];
	atomic_uint head;		/* engine tick */
	atomic_uint tail;		/* the reader */
	_Atomic uint_least32_t lost;
};

static inline void jb_trace_init(struct jb_trace_ring *r)
{
	atomic_init(&r->head, 0u);
	atomic_init(&r->tail, 0u);
	atomic_init(&r->lost, 0u);
}

static inline bool jb_trace_push(struct jb_trace_ring *r,
				 const struct udsp_jb_trace *rec)
{
	unsigned head = atomic_load_explicit(&r->head, memory_order_relaxed);
	unsigned next = (head + 1u) % UDSP_JB_TRACE_SLOTS;

	if (next == atomic_load_explicit(&r->tail, memory_order_acquire)) {
		atomic_fetch_add_explicit(&r->lost, 1u, memory_order_release);

		return false;
	}

	r->slot[head] = *rec;
	/* Claimed after the push succeeded and before head is published, so the
	 * losses this record inherits are exactly the ones that preceded it
	 * (core/event.c). */
	r->slot[head].lost_before =
		(uint32_t)atomic_exchange_explicit(&r->lost, 0u,
						   memory_order_acq_rel);
	atomic_store_explicit(&r->head, next, memory_order_release);

	return true;
}

static inline bool jb_trace_pop(struct jb_trace_ring *r,
				struct udsp_jb_trace *out)
{
	unsigned tail = atomic_load_explicit(&r->tail, memory_order_relaxed);

	if (tail == atomic_load_explicit(&r->head, memory_order_acquire))
		return false;

	*out = r->slot[tail];
	atomic_store_explicit(&r->tail, (tail + 1u) % UDSP_JB_TRACE_SLOTS,
			      memory_order_release);

	return true;
}

#endif
