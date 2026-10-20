// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The single event emitter (event.h Single emitter). One thread owns every
 * write to the event fd; the engine and slic threads post fixed records to
 * their own single-producer rings and the emitter assigns seq and sends, so
 * socket write order always matches seq order and uvoice main.c
 * event_seq_check() never sees a false gap.
 *
 * Two classes (hal.h section 2). A hardware event is dropped and counted on a
 * full fd or a full ring and is never retried; either way its sequence number
 * is consumed, so the gap uvoice main.c event_seq_check() sees always equals
 * the drop count. A JOB_DONE completion has its own ring so a hardware burst
 * cannot displace it, carries seq 0, and is retried until it is written or
 * the emitter stops: a lost completion strands a call and uvoice's 30 s job
 * timer is the outer bound.
 *
 * The emitter also carries the watchdog: a deadline-based check that runs on
 * the wake whose deadline has passed, so wake traffic can neither starve it
 * nor make it fire early.
 */
#ifndef UDSP_CORE_EVENT_PRIV_H
#define UDSP_CORE_EVENT_PRIV_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "udsp/event.h"

#include "clock.h"
#include "thread.h"

/* One ring per producer thread keeps every ring single-producer, which is what
 * lets the whole path run without a lock. */
enum udsp_ev_producer {
	UDSP_EV_PROD_ENGINE = 0,	/* hardware events from the tick */
	UDSP_EV_PROD_SLIC,		/* hardware events from the slic thread */
	UDSP_EV_PROD_CTL,		/* hardware events from the control thread */
	UDSP_EV_PROD_WATCHDOG,		/* the emitter's own fault event */
	UDSP_EV_PROD_JOB_SLIC,		/* completions from the job queue */
	UDSP_EV_PROD_JOB_CTL,		/* completions the control thread adds */
	UDSP_EV_PROD_MAX,
};

/*
 * One queued record plus the losses that happened before it. The producer
 * attaches its pending loss to the next record it manages to queue, so the gap
 * the consumer sees falls where the loss happened instead of in front of every
 * record still queued, where its position would depend on scheduling.
 */
struct ev_slot {
	struct udsp_event ev;
	uint32_t lost_before;
};

struct ev_ring {
	struct ev_slot *slot;
	unsigned cap;
	atomic_uint head;
	atomic_uint tail;
	/*
	 * Records the producer dropped at a full ring. The consumer's only
	 * loss signal is a gap in seq, so the emitter
	 * advances the counter by this many and the gap the core sees always
	 * equals events_dropped. A drop at the socket already consumes its seq,
	 * because seq is assigned before the send.
	 *
	 * The producer claims this into the slot it just filled; what is left
	 * here is a tail of losses with no following record, and the drain
	 * burns it after the queue empties, which is chronologically exact.
	 */
	_Atomic uint_least32_t lost;
};

struct udsp_emitter {
	int fd;			/* the caller's event fd, never closed here */
	int wake_fd;
	const struct udsp_clock *clock;	/* the engine time base of the stamps */
	struct ev_ring ring[UDSP_EV_PROD_MAX];
	atomic_bool armed;
	atomic_bool quit;
	_Atomic uint_least64_t sent;
	_Atomic uint_least64_t dropped;
	uint16_t seq;		/* emitter thread only */
	unsigned watchdog_ms;
	void (*watchdog)(void *ctx);
	void *ctx;
	struct udsp_thread thread;
};

int udsp_emitter_init(struct udsp_emitter *e, int event_fd,
		      const struct udsp_clock *clock, unsigned watchdog_ms,
		      void (*watchdog)(void *ctx), void *ctx);
int udsp_emitter_start(struct udsp_emitter *e);
/* 0 joined, -ETIMEDOUT leaked (the caller then frees nothing). */
int udsp_emitter_stop(struct udsp_emitter *e, unsigned deadline_ms);
void udsp_emitter_release(struct udsp_emitter *e);

/* False when the producer ring was full: the record is dropped and counted,
 * which is the hal.h class (a) policy. Completion producers never drop. */
bool udsp_emit(struct udsp_emitter *e, enum udsp_ev_producer p,
	       const struct udsp_event *ev);
bool udsp_emit_completion(struct udsp_emitter *e, enum udsp_ev_producer p,
			  uint32_t token, int32_t status);

/* Test-only fault injection: one drain pass on the calling thread, so a test
 * can place a producer loss between two queued records and read the sequence
 * numbers the consumer would see. The emitter thread must not run during the
 * call. Returns true when it sent at least one record. */
bool udsp_emitter_pump(struct udsp_emitter *e);

static inline uint64_t udsp_emitter_sent(const struct udsp_emitter *e)
{
	return atomic_load_explicit(&e->sent, memory_order_relaxed);
}

static inline uint64_t udsp_emitter_dropped(const struct udsp_emitter *e)
{
	return atomic_load_explicit(&e->dropped, memory_order_relaxed);
}

#endif
