// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libubox/utils.h>

#include "hal_relay.h"

#include "../posix.h"
#include "../trace.h"

/* The base is the reserved bit and the mask is everything below it, so the or
 * in hal_relay_stats_token_next cannot carry out of the half however far the
 * counter has run. */
_Static_assert((HAL_RELAY_STATS_TOKEN_BASE & HAL_RELAY_STATS_TOKEN_MASK) == 0,
	       "the stats token base overlaps its own counter");
_Static_assert((HAL_RELAY_STATS_TOKEN_BASE | HAL_RELAY_STATS_TOKEN_MASK) ==
	       0xffffffffu,
	       "the stats token base and mask do not cover the word");

static bool event_send(struct hal_relay *r, const struct hal_event *ev)
{
	ssize_t len = send(r->core_fd, ev, sizeof(*ev), MSG_DONTWAIT | MSG_NOSIGNAL);

	return len == (ssize_t)sizeof(*ev);
}

/*
 * The write watcher exists only while a completion is waiting for room. An
 * armed watcher on an empty queue would wake the loop on every turn, because
 * an fd with room is writable for ever.
 *
 * A registration that fails must not strand the queue. Nothing else would wake
 * those records, and the core would wait out its 30 s job backstop with room on
 * the fd, so the retry timer takes the watcher's place until one registration
 * succeeds or the queue empties.
 */
static void completion_watch_set(struct hal_relay *r, bool on)
{
	if (!on) {
		uloop_timeout_cancel(&r->comp_retry);
		if (r->core_out.registered)
			uloop_fd_delete(&r->core_out);
		return;
	}

	if (r->core_out.registered)
		return;

	if (uloop_fd_add(&r->core_out, ULOOP_WRITE) == 0) {
		uloop_timeout_cancel(&r->comp_retry);
		return;
	}

	ULOG_ERR("%s: completion watcher: %s, retrying in %u ms\n", r->tag,
		 strerror(errno), HAL_RELAY_COMP_RETRY_MS);
	uloop_timeout_set(&r->comp_retry, HAL_RELAY_COMP_RETRY_MS);
}

/* Oldest first, so the core reads the completions in the order the jobs
 * finished. A record that still does not fit leaves the rest queued and asks
 * for the watcher again: this drain also runs from the forward path and from
 * the retry timer, and neither of those holds a registration of its own. */
static void completion_drain(struct hal_relay *r)
{
	while (r->comp_len) {
		if (!event_send(r, &r->comp[r->comp_head])) {
			completion_watch_set(r, true);
			return;
		}

		r->comp_head = (r->comp_head + 1) % HAL_RELAY_COMP_MAX;
		r->comp_len--;
	}

	completion_watch_set(r, false);
}

static void completion_retry(struct uloop_timeout *t)
{
	struct hal_relay *r = container_of(t, struct hal_relay, comp_retry);

	completion_drain(r);
}

static void completion_ready(struct uloop_fd *ufd, unsigned int events)
{
	struct hal_relay *r = container_of(ufd, struct hal_relay, core_out);

	completion_drain(r);
}

/*
 * A class (b) record is never dropped and never waits (hal.h section 2). The
 * send is tried at once while the queue is empty, because that is the whole
 * cost on a core that keeps up; a full fd queues the record and arms the write
 * watcher instead. A queue that is not empty takes every later record without
 * a try of its own, or a completion would overtake the one before it.
 */
void hal_relay_completion_send(struct hal_relay *r, const struct hal_event *ev)
{
	unsigned int tail;

	if (!r->comp_len && event_send(r, ev))
		return;

	/* The overflow is unreachable from the bounded job model. A drop
	 * increments comp_lost and reaches the core through
	 * hal_counters.completions_lost; events_dropped stays the class (a)
	 * number that hal.h section 2 describes. */
	if (r->comp_len >= HAL_RELAY_COMP_MAX) {
		r->comp_lost++;
		if (ev->type == HAL_EVENT_JOB_DONE)
			ULOG_ERR("%s: queued record type %u token %u dropped, the "
				 "queue is full\n", r->tag, ev->type,
				 ev->u.job.token);
		else
			ULOG_ERR("%s: queued record type %u dropped, the queue is "
				 "full\n", r->tag, ev->type);
		return;
	}

	tail = (r->comp_head + r->comp_len) % HAL_RELAY_COMP_MAX;
	r->comp[tail] = *ev;
	r->comp_len++;
	completion_watch_set(r, true);
}

/*
 * The completion the backend posts itself, for the token the core is waiting
 * on. It carries seq 0, so it stays outside the core's gap accounting, and the
 * stamp it takes here travels with it however long the queue holds it.
 */
void hal_relay_completion_post(struct hal_relay *r, uint32_t token,
			       enum hal_status st)
{
	struct hal_event ev = { .type = HAL_EVENT_JOB_DONE };

	ev.u.job.token = token;
	ev.u.job.status = (int32_t)st;
	ev.stamp_ms = uvoice_mono_ms();

	hal_relay_completion_send(r, &ev);
}

/*
 * A hardware event is dropped on a full fd and counted (hal.h section 2 (a)),
 * the same policy an engine emitter applies one hop earlier. The gap the core
 * then sees in seq is the fault indicator; nothing is queued and nothing is
 * retried.
 *
 * A queued completion goes first. Room that appears on the core fd belongs to
 * the record already waiting for it: a hardware event that took it would push a
 * job out to the core's 30 s backstop, so the queue is drained here before
 * anything else is written, and this record is dropped while the queue is still
 * not empty.
 */
static void event_forward(struct hal_relay *r, const struct hal_event *ev)
{
	if (r->comp_len)
		completion_drain(r);

	if (!r->comp_len && event_send(r, ev))
		return;

	r->dropped++;
	TRACE(TRACE_HAL, "%s: event type %u line %u dropped: %s", r->tag,
	      ev->type, ev->line, strerror(errno));
}

static void relay_read(struct uloop_fd *ufd, unsigned int events)
{
	struct hal_relay *r = container_of(ufd, struct hal_relay, read_fd);
	struct hal_event ev;
	ssize_t len;

	while (true) {
		len = recv(ufd->fd, &ev, sizeof(ev), MSG_DONTWAIT);
		if (len < 0 && errno == EINTR)
			continue;
		if (len < 0)
			return;
		if (len != (ssize_t)sizeof(ev)) {
			/* SOCK_SEQPACKET keeps message boundaries, so this is a
			 * record neither contract describes. */
			ULOG_ERR("%s: short engine event: %zd bytes\n", r->tag, len);
			continue;
		}

		if (r->intercept && r->intercept(r->ctx, &ev))
			continue;

		/* An engine's own completions are class (b) as well: a lost one
		 * strands the job. */
		if (ev.type == HAL_EVENT_JOB_DONE)
			hal_relay_completion_send(r, &ev);
		else
			event_forward(r, &ev);
	}
}

/*
 * CLOEXEC because the firewall helper and the event publisher fork children
 * that must not inherit a voice fd; the engine dups its end with
 * F_DUPFD_CLOEXEC, so its copy already carries it.
 */
int hal_relay_open(struct hal_relay *r, int core_fd, const char *tag,
		   hal_relay_intercept_fn intercept, void *ctx)
{
	int fd[2];

	r->core_fd = core_fd;
	r->tag = tag;
	r->intercept = intercept;
	r->ctx = ctx;
	r->core_out.fd = core_fd;
	r->core_out.cb = completion_ready;
	r->comp_retry.cb = completion_retry;
	r->lib_fd = -1;
	r->read_fd.fd = -1;

	if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
		       fd))
		return -1;

	r->read_fd.fd = fd[0];
	r->read_fd.cb = relay_read;
	r->lib_fd = fd[1];

	if (uloop_fd_add(&r->read_fd, ULOOP_READ) < 0) {
		close(fd[0]);
		close(fd[1]);
		r->read_fd.fd = -1;
		r->lib_fd = -1;
		return -1;
	}

	return 0;
}

void hal_relay_close(struct hal_relay *r)
{
	/* The core fd belongs to the core, which closes it after the backend is
	 * gone; only the write watcher and its retry timer are the relay's to
	 * drop, and this call drops both. */
	completion_watch_set(r, false);
	uloop_timeout_cancel(&r->comp_retry);

	if (r->read_fd.registered)
		uloop_fd_delete(&r->read_fd);

	if (r->read_fd.fd >= 0)
		close(r->read_fd.fd);
	if (r->lib_fd >= 0)
		close(r->lib_fd);

	r->read_fd.fd = -1;
	r->lib_fd = -1;
}

uint32_t hal_relay_stats_token_next(struct hal_relay *r)
{
	r->stats_token = (r->stats_token + 1u) & HAL_RELAY_STATS_TOKEN_MASK;

	return HAL_RELAY_STATS_TOKEN_BASE | r->stats_token;
}
