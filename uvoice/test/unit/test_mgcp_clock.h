// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The virtual clock and the transport capture of the suites that drive a
 * layer through struct proto_timer_env, and the MGCP transaction layer
 * through struct mgcp_txn_env. The test moves the clock by hand, a timer
 * table replaces uloop and a capture array replaces the socket, so every
 * timer and every send is observable to the millisecond and no case
 * sleeps.
 *
 * The suite declares its fixture state as a static named fk before it
 * includes this header. fk holds these fields:
 *   - the clock: uint64_t now, struct proto_timer *timer[],
 *     unsigned int timers and unsigned int fired;
 *   - the capture: struct fake_send send[], unsigned int sends and
 *     bool send_fail.
 * Each struct fake_send holds char buf[], size_t len, char host[64],
 * uint16_t port and uint64_t at_ms, the clock at the send. The sizes of the
 * two arrays set the limits. A memset of fk resets both; it does not reset
 * fake_timer_dropped.
 */
#ifndef UVOICE_TEST_MGCP_CLOCK_H
#define UVOICE_TEST_MGCP_CLOCK_H

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>

#include "proto/mgcp/txn.h"

/* A dropped timer looks like a state machine that never fires; the count
 * survives the reset of fk so a check at the end of the suite sees it. */
static unsigned int fake_timer_dropped;

static inline uint64_t fake_now(struct proto_timer_env *env)
{
	return fk.now;
}

static inline void fake_timer_drop(struct proto_timer *t)
{
	unsigned int i;

	for (i = 0; i < fk.timers; i++) {
		if (fk.timer[i] != t)
			continue;

		fk.timer[i] = fk.timer[--fk.timers];
		return;
	}
}

static inline void fake_timer_set(struct proto_timer_env *env,
				  struct proto_timer *t, unsigned int ms)
{
	fake_timer_drop(t);
	t->due_ms = fk.now + ms;

	if (fk.timers >= sizeof(fk.timer) / sizeof(fk.timer[0])) {
		fake_timer_dropped++;
		return;
	}

	fk.timer[fk.timers++] = t;
}

static inline void fake_timer_cancel(struct proto_timer_env *env,
				     struct proto_timer *t)
{
	fake_timer_drop(t);
}

static inline void fake_clock_bind(struct proto_timer_env *env)
{
	env->now_ms = fake_now;
	env->timer_set = fake_timer_set;
	env->timer_cancel = fake_timer_cancel;
}

/* The timers fire in the order of their due time, each at its own due
 * time, so a timer set from a callback inside the window fires inside it. */
static inline void fake_advance(uint64_t ms)
{
	uint64_t target = fk.now + ms;

	for (;;) {
		struct proto_timer *next = NULL;
		unsigned int i;

		for (i = 0; i < fk.timers; i++) {
			struct proto_timer *t = fk.timer[i];

			if (t->due_ms > target)
				continue;
			if (!next || t->due_ms < next->due_ms)
				next = t;
		}

		if (!next)
			break;

		fk.now = next->due_ms;
		fake_timer_drop(next);
		fk.fired++;
		proto_timer_expire(next);
	}

	fk.now = target;
}

/* the due time of an armed timer, or 0 */
static inline uint64_t fake_timer_due(const struct proto_timer *t)
{
	unsigned int i;

	for (i = 0; i < fk.timers; i++) {
		if (fk.timer[i] == t)
			return t->due_ms;
	}

	return 0;
}

#define FAKE_SEND_SLOTS		(sizeof(fk.send) / sizeof(fk.send[0]))
#define FAKE_SEND_TEXT_MAX	(sizeof(fk.send[0].buf) - 1)

/* The counter keeps counting past the capture array, so a flood never turns
 * into an accidental send failure. */
static inline int fake_send_fn(struct mgcp_txn_env *env, const void *buf,
			       size_t len, const struct sockaddr *dst,
			       socklen_t dst_len)
{
	const struct sockaddr_in *v4 = (const struct sockaddr_in *)dst;
	unsigned int i = fk.sends;
	size_t kept = len < FAKE_SEND_TEXT_MAX ? len : FAKE_SEND_TEXT_MAX;

	if (fk.send_fail)
		return -EHOSTUNREACH;

	if (i < FAKE_SEND_SLOTS) {
		fk.send[i].len = kept;
		memcpy(fk.send[i].buf, buf, kept);
		fk.send[i].buf[kept] = 0;
		fk.send[i].host[0] = 0;
		fk.send[i].port = 0;
		fk.send[i].at_ms = fk.now;
		if (dst && dst_len >= sizeof(*v4) &&
		    dst->sa_family == AF_INET) {
			inet_ntop(AF_INET, &v4->sin_addr, fk.send[i].host,
				  sizeof(fk.send[i].host));
			fk.send[i].port = ntohs(v4->sin_port);
		}
	}
	fk.sends++;

	return (int)len;
}

/* "" for a send that never happened or that fell past the capture array */
static inline const char *send_text(unsigned int idx)
{
	if (idx >= fk.sends || idx >= FAKE_SEND_SLOTS)
		return "";

	return fk.send[idx].buf;
}

static inline const char *last_text(void)
{
	return fk.sends ? send_text(fk.sends - 1) : "";
}

static inline socklen_t sa_v4(struct sockaddr_storage *ss, const char *host,
			      uint16_t port)
{
	struct sockaddr_in *v4 = (struct sockaddr_in *)ss;

	memset(ss, 0, sizeof(*ss));
	v4->sin_family = AF_INET;
	v4->sin_port = htons(port);
	inet_pton(AF_INET, host, &v4->sin_addr);

	return sizeof(*v4);
}

#endif
