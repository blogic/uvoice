// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The plaintext packet trace rings (udsp.h struct udsp_pkt_trace).
 *
 * Two rings, one per direction, because the two producers are different
 * threads. The engine tick fills the transmit ring at the two tap sites of
 * core/stream.c, stream_pkt_send and stream_tx_gateway. The net thread fills
 * the receive ring in core/net.c, one record per slot it filled. One consumer
 * pops both, the caller of udsp_pkt_trace_read.
 *
 * The shape and the memory orders are those of core/jb_trace.h: each side loads
 * its own index relaxed, loads the other index acquire, and stores its own
 * index release. A full ring drops the new record and counts it, and the next
 * record that fits claims the count, so the gap the reader sees falls where the
 * loss happened.
 *
 * A record is 600 bytes, so a ring of 1024 slots costs 600 KiB and the pair
 * costs 1.2 MiB. That is too much for a handle to carry unused, so the rings
 * live on the heap. udsp_pkt_trace_enable allocates them on the control thread
 * and makes every page resident. udsp_close frees them once the two producers
 * have stopped.
 *
 * Neither producer allocates, locks, logs or reads a clock of its own. One
 * tap copies the payload once. Around that copy
 * it loads the ring pointer and the debug mask, loads the two ring indices,
 * claims the loss counter and publishes the head.
 *
 * The payload sits beside the record header instead of inside it. A producer
 * that built a whole record would put 600 bytes on a real-time stack and copy
 * them twice, and only the first len bytes of a slot mean anything.
 */
#ifndef UDSP_CORE_PKT_TRACE_H
#define UDSP_CORE_PKT_TRACE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "udsp/udsp.h"

/* At one packet per stream per 10 ms tick, this holds about ten seconds of one
 * busy call in each direction, which is longer than a reader that drains once
 * per second needs. One slot is the sentinel that tells a full ring from an
 * empty one, so 1023 records are usable. */
#define UDSP_PKT_TRACE_SLOTS	1024u

struct pkt_trace_ring {
	struct udsp_pkt_trace slot[UDSP_PKT_TRACE_SLOTS];
	atomic_uint head;		/* the producer of this direction */
	atomic_uint tail;		/* the reader */
	_Atomic uint_least32_t lost;
};

/* The scalar half of one record. The producer fills it on its stack and the
 * push writes it into the slot beside the payload. */
struct pkt_trace_meta {
	uint64_t stamp_ns;
	uint32_t gen;
	uint8_t line;
	uint8_t stream;
	uint8_t dir;			/* enum udsp_pkt_dir */
	uint8_t kind;			/* enum udsp_pkt_kind */
};

static inline void pkt_trace_init(struct pkt_trace_ring *r)
{
	atomic_init(&r->head, 0u);
	atomic_init(&r->tail, 0u);
	atomic_init(&r->lost, 0u);
}

static inline bool pkt_trace_push(struct pkt_trace_ring *r,
				  const struct pkt_trace_meta *m,
				  const void *data, size_t len)
{
	unsigned head = atomic_load_explicit(&r->head, memory_order_relaxed);
	unsigned next = (head + 1u) % UDSP_PKT_TRACE_SLOTS;
	struct udsp_pkt_trace *rec = &r->slot[head];

	if (next == atomic_load_explicit(&r->tail, memory_order_acquire)) {
		atomic_fetch_add_explicit(&r->lost, 1u, memory_order_release);

		return false;
	}

	if (len > UDSP_PKT_TRACE_MAX)
		len = UDSP_PKT_TRACE_MAX;
	rec->stamp_ns = m->stamp_ns;
	rec->gen = m->gen;
	rec->len = (uint16_t)len;
	rec->line = m->line;
	rec->stream = m->stream;
	rec->dir = m->dir;
	rec->kind = m->kind;
	rec->pad[0] = 0;
	rec->pad[1] = 0;
	if (len)
		memcpy(rec->data, data, len);
	/* Claimed after the record is written and before head is published, so
	 * the losses this record inherits are exactly the ones that preceded it
	 * (core/jb_trace.h). */
	rec->lost_before =
		(uint32_t)atomic_exchange_explicit(&r->lost, 0u,
						   memory_order_acq_rel);
	atomic_store_explicit(&r->head, next, memory_order_release);

	return true;
}

static inline bool pkt_trace_pop(struct pkt_trace_ring *r,
				 struct udsp_pkt_trace *out)
{
	unsigned tail = atomic_load_explicit(&r->tail, memory_order_relaxed);

	if (tail == atomic_load_explicit(&r->head, memory_order_acquire))
		return false;

	*out = r->slot[tail];
	atomic_store_explicit(&r->tail, (tail + 1u) % UDSP_PKT_TRACE_SLOTS,
			      memory_order_release);

	return true;
}

#endif
