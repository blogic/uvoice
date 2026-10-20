// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"
#include "cmd.h"
#include "event_priv.h"

#define EV_RING_ENGINE		256u
#define EV_RING_SLIC		64u
#define EV_RING_SMALL		8u
#define EV_RING_COMPLETION	16u
#define EV_STACK_BYTES		(128 * 1024)

static const unsigned ev_ring_cap[UDSP_EV_PROD_MAX] = {
	EV_RING_ENGINE, EV_RING_SLIC, EV_RING_SMALL, EV_RING_SMALL,
	EV_RING_COMPLETION, EV_RING_COMPLETION,
};

static bool ev_is_completion(enum udsp_ev_producer p)
{
	return p == UDSP_EV_PROD_JOB_SLIC || p == UDSP_EV_PROD_JOB_CTL;
}

static bool ev_ring_push(struct ev_ring *r, const struct udsp_event *ev)
{
	unsigned head = atomic_load_explicit(&r->head, memory_order_relaxed);
	unsigned next = (head + 1u) % r->cap;

	if (next == atomic_load_explicit(&r->tail, memory_order_acquire))
		return false;

	r->slot[head].ev = *ev;
	/* Claimed after the push succeeded and before head is published, so the
	 * losses this record inherits are exactly the ones that preceded it. A
	 * drain that wins the exchange burns them itself, which is the same
	 * position. */
	r->slot[head].lost_before =
		(uint32_t)atomic_exchange_explicit(&r->lost, 0u,
						   memory_order_acq_rel);
	atomic_store_explicit(&r->head, next, memory_order_release);

	return true;
}

static bool ev_ring_pop(struct ev_ring *r, struct ev_slot *out)
{
	unsigned tail = atomic_load_explicit(&r->tail, memory_order_relaxed);

	if (tail == atomic_load_explicit(&r->head, memory_order_acquire))
		return false;

	*out = r->slot[tail];
	atomic_store_explicit(&r->tail, (tail + 1u) % r->cap,
			      memory_order_release);

	return true;
}

static void ev_wake(struct udsp_emitter *e)
{
	uint64_t one = 1;

	/* The emitter clears the flag before it drains, so a producer pays at
	 * most one write per sleep and the RT tick never syscalls in vain. */
	if (!atomic_exchange_explicit(&e->armed, false, memory_order_acq_rel))
		return;
	if (write(e->wake_fd, &one, sizeof(one)) != (ssize_t)sizeof(one))
		return;
}

bool udsp_emit(struct udsp_emitter *e, enum udsp_ev_producer p,
	       const struct udsp_event *ev)
{
	if (!e || !ev || p >= UDSP_EV_PROD_MAX)
		return false;

	if (!ev_ring_push(&e->ring[p], ev)) {
		if (!ev_is_completion(p)) {
			atomic_fetch_add_explicit(&e->dropped, 1u,
						  memory_order_relaxed);
			atomic_fetch_add_explicit(&e->ring[p].lost, 1u,
						  memory_order_release);
		}
		return false;
	}
	ev_wake(e);

	return true;
}

bool udsp_emit_completion(struct udsp_emitter *e, enum udsp_ev_producer p,
			  uint32_t token, int32_t status)
{
	struct udsp_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = UDSP_EV_JOB_DONE;
	ev.stamp_ms = e && e->clock ? udsp_clock_now_ms(e->clock) : 0u;
	ev.u.job.token = token;
	/* At the origin: uvoice main.c casts this field to enum hal_status, so
	 * the errno the worker returned is translated here and never travels. */
	ev.u.job.status = (int32_t)cmd_job_status_map(status);

	return udsp_emit(e, p, &ev);
}

static uint16_t ev_seq_next(struct udsp_emitter *e)
{
	e->seq++;
	if (!e->seq)
		e->seq = 1;

	return e->seq;
}

/* hal.h class (a): the pump must never block, so a full fd drops the record
 * and the gap in seq tells the core before the counter is read. */
static void ev_send_hardware(struct udsp_emitter *e, struct udsp_event *ev)
{
	ssize_t n;

	ev->seq = ev_seq_next(e);
	n = send(e->fd, ev, sizeof(*ev), MSG_DONTWAIT | MSG_NOSIGNAL);
	if (n == (ssize_t)sizeof(*ev)) {
		atomic_fetch_add_explicit(&e->sent, 1u, memory_order_relaxed);
		return;
	}

	atomic_fetch_add_explicit(&e->dropped, 1u, memory_order_relaxed);
}

/* hal.h class (b): a lost completion strands a call, so this one waits. There
 * is no ceiling: the loop ends when the record is written or when udsp_close
 * stops the emitter, and uvoice's 30 s job timer is the outer net. */
static void ev_send_completion(struct udsp_emitter *e, struct udsp_event *ev)
{
	ev->seq = 0;

	for (;;) {
		struct pollfd pfd = { .fd = e->fd, .events = POLLOUT };
		struct timespec rel = { .tv_sec = 0, .tv_nsec = 50000000L };
		ssize_t n = send(e->fd, ev, sizeof(*ev),
				 MSG_DONTWAIT | MSG_NOSIGNAL);

		if (n == (ssize_t)sizeof(*ev)) {
			atomic_fetch_add_explicit(&e->sent, 1u,
						  memory_order_relaxed);
			return;
		}
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
		    errno != ENOBUFS && errno != EINTR)
			return;
		if (atomic_load_explicit(&e->quit, memory_order_acquire))
			return;
		ppoll(&pfd, 1, &rel, NULL);
	}
}

/* Burn one sequence number per record the producer could not queue, so the gap
 * uvoice main.c event_seq_check() sees always equals events_dropped. */
static void ev_seq_burn(struct udsp_emitter *e, uint32_t lost)
{
	while (lost--)
		ev_seq_next(e);
}

static bool ev_drain_ring(struct udsp_emitter *e, enum udsp_ev_producer prod)
{
	struct ev_slot slot;
	bool any = false;

	while (ev_ring_pop(&e->ring[prod], &slot)) {
		any = true;
		ev_seq_burn(e, slot.lost_before);
		if (ev_is_completion(prod))
			ev_send_completion(e, &slot.ev);
		else
			ev_send_hardware(e, &slot.ev);
	}
	/* Whatever is left had no following record, so its place is here, after
	 * everything the producer did manage to queue. */
	ev_seq_burn(e, (uint32_t)atomic_exchange_explicit(&e->ring[prod].lost,
							  0u,
							  memory_order_acquire));

	return any;
}

static bool ev_drain(struct udsp_emitter *e)
{
	bool any = false;

	/* Completions first: a hardware burst must not delay one. */
	static const enum udsp_ev_producer order[UDSP_EV_PROD_MAX] = {
		UDSP_EV_PROD_JOB_SLIC, UDSP_EV_PROD_JOB_CTL,
		UDSP_EV_PROD_ENGINE, UDSP_EV_PROD_SLIC,
		UDSP_EV_PROD_CTL, UDSP_EV_PROD_WATCHDOG,
	};

	for (unsigned p = 0; p < UDSP_EV_PROD_MAX; p++)
		any = ev_drain_ring(e, order[p]) || any;

	return any;
}

bool udsp_emitter_pump(struct udsp_emitter *e)
{
	return e ? ev_drain(e) : false;
}

static void *ev_loop(void *arg)
{
	struct udsp_emitter *e = arg;
	uint64_t next_check = udsp_mono_us() + (uint64_t)e->watchdog_ms * 1000u;

	for (;;) {
		struct pollfd pfd = { .fd = e->wake_fd, .events = POLLIN };
		struct timespec rel;
		uint64_t now, sink64;
		uint64_t left;

		atomic_store_explicit(&e->armed, true, memory_order_release);
		ev_drain(e);
		if (atomic_load_explicit(&e->quit, memory_order_acquire))
			break;

		now = udsp_mono_us();
		left = now >= next_check ? 0 : next_check - now;
		rel.tv_sec = (time_t)(left / 1000000u);
		rel.tv_nsec = (long)(left % 1000000u) * 1000L;
		if (ppoll(&pfd, 1, &rel, NULL) > 0) {
			while (read(e->wake_fd, &sink64, sizeof(sink64)) ==
			       (ssize_t)sizeof(sink64))
				continue;
		}
		atomic_store_explicit(&e->armed, false, memory_order_release);

		now = udsp_mono_us();
		if (now < next_check)
			continue;
		next_check = now + (uint64_t)e->watchdog_ms * 1000u;
		if (e->watchdog)
			e->watchdog(e->ctx);
	}

	atomic_store_explicit(&e->armed, false, memory_order_release);
	ev_drain(e);

	return NULL;
}

int udsp_emitter_init(struct udsp_emitter *e, int event_fd,
		      const struct udsp_clock *clock, unsigned watchdog_ms,
		      void (*watchdog)(void *ctx), void *ctx)
{
	if (!e || event_fd < 0)
		return -EINVAL;

	memset(e, 0, sizeof(*e));
	e->fd = event_fd;
	e->clock = clock;
	e->watchdog_ms = watchdog_ms ? watchdog_ms : 1000u;
	e->watchdog = watchdog;
	e->ctx = ctx;
	e->thread.done_fd = -1;
	atomic_init(&e->armed, false);
	atomic_init(&e->quit, false);
	atomic_init(&e->sent, 0u);
	atomic_init(&e->dropped, 0u);

	e->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (e->wake_fd < 0)
		return -errno;

	for (unsigned p = 0; p < UDSP_EV_PROD_MAX; p++) {
		e->ring[p].cap = ev_ring_cap[p];
		e->ring[p].slot = calloc(ev_ring_cap[p],
					 sizeof(*e->ring[p].slot));
		if (!e->ring[p].slot)
			return -ENOMEM;
		atomic_init(&e->ring[p].head, 0u);
		atomic_init(&e->ring[p].tail, 0u);
		atomic_init(&e->ring[p].lost, 0u);
	}

	return 0;
}

int udsp_emitter_start(struct udsp_emitter *e)
{
	return udsp_thread_start(&e->thread, "udsp-emit", ev_loop, e,
				 EV_STACK_BYTES, -1, -1, 0);
}

int udsp_emitter_stop(struct udsp_emitter *e, unsigned deadline_ms)
{
	uint64_t one = 1;

	if (!e || !e->thread.live)
		return 0;

	atomic_store_explicit(&e->quit, true, memory_order_release);
	if (write(e->wake_fd, &one, sizeof(one)) != (ssize_t)sizeof(one))
		return -EIO;

	return udsp_thread_stop_bounded(&e->thread, deadline_ms);
}

void udsp_emitter_release(struct udsp_emitter *e)
{
	if (!e)
		return;
	if (e->wake_fd >= 0) {
		close(e->wake_fd);
		e->wake_fd = -1;
	}
	for (unsigned p = 0; p < UDSP_EV_PROD_MAX; p++) {
		free(e->ring[p].slot);
		e->ring[p].slot = NULL;
	}
}
