// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The virtual clock of the suites that drive a SIP layer through struct
 * sip_txn_env. The test moves the clock by hand and a timer table replaces
 * uloop, so every timer is observable to the millisecond and no case sleeps.
 *
 * The suite declares its fixture state as a static named fk before it
 * includes this header. fk holds the clock fields that the functions below
 * read: uint64_t now, struct sip_timer *timer[], unsigned int timers and
 * unsigned int fired. A memset of fk resets the clock. It does not reset
 * fake_timer_dropped and fake_timer_hook, which live outside fk.
 */
#ifndef UVOICE_TEST_CLOCK_H
#define UVOICE_TEST_CLOCK_H

#include "proto/sip/txn.h"

/* A dropped timer looks like a state machine that never fires. The count
 * survives the reset of fk, so the check at the end of a suite also sees a
 * timer that an earlier case dropped. */
static unsigned int fake_timer_dropped;

/* Each hook sees a set or a cancel before the table changes, so a suite can
 * log the timer requests of the code under test. */
static struct {
	void (*set)(const struct sip_timer *t, unsigned int ms);
	void (*cancel)(const struct sip_timer *t);
} fake_timer_hook;

static inline uint64_t fake_now(struct proto_timer_env *env)
{
	return fk.now;
}

static inline void fake_timer_drop(struct sip_timer *t)
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
				  struct sip_timer *t, unsigned int ms)
{
	if (fake_timer_hook.set)
		fake_timer_hook.set(t, ms);

	fake_timer_drop(t);
	t->due_ms = fk.now + ms;

	if (fk.timers >= sizeof(fk.timer) / sizeof(fk.timer[0])) {
		fake_timer_dropped++;
		return;
	}

	fk.timer[fk.timers++] = t;
}

static inline void fake_timer_cancel(struct proto_timer_env *env,
				     struct sip_timer *t)
{
	if (fake_timer_hook.cancel)
		fake_timer_hook.cancel(t);

	fake_timer_drop(t);
}

static inline void fake_clock_bind(struct sip_txn_env *env)
{
	env->base.now_ms = fake_now;
	env->base.timer_set = fake_timer_set;
	env->base.timer_cancel = fake_timer_cancel;
}

/* The timers fire in the order of their due time, and each one reads the
 * clock at its own due time, so a timer set from a callback inside the
 * window also fires inside it. */
static inline void fake_advance(uint64_t ms)
{
	uint64_t target = fk.now + ms;

	for (;;) {
		struct sip_timer *next = NULL;
		unsigned int i;

		for (i = 0; i < fk.timers; i++) {
			struct sip_timer *t = fk.timer[i];

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
		sip_timer_expire(next);
	}

	fk.now = target;
}

#endif
