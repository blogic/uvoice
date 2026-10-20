// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The timer helpers of proto/shared/timer.h and their uloop environment. The
 * helpers read nothing of uloop, so a suite drives them on a virtual clock
 * through an environment of its own. The uloop environment is the one place
 * here that knows uloop.
 */
#include <string.h>

#include <libubox/uloop.h>

#include "posix.h"
#include "proto/shared/timer.h"

void proto_timer_init(struct proto_timer *t, void *owner, unsigned int id,
		      proto_timer_cb cb)
{
	memset(t, 0, sizeof(*t));
	t->owner = owner;
	t->id = (uint8_t)id;
	t->fire = cb;
}

void proto_timer_arm(struct proto_timer_env *env, struct proto_timer *t,
		     unsigned int ms)
{
	if (t->armed)
		proto_timer_cancel(env, t);

	t->armed = true;
	if (env->timer_set)
		env->timer_set(env, t, ms);
}

void proto_timer_cancel(struct proto_timer_env *env, struct proto_timer *t)
{
	if (!t->armed)
		return;

	t->armed = false;
	if (env->timer_cancel)
		env->timer_cancel(env, t);
}

void proto_timer_expire(struct proto_timer *t)
{
	t->armed = false;
	if (t->fire)
		t->fire(t);
}

/*
 * The slot lives inside struct proto_timer, so a timer pool of any size costs
 * no allocation and no lookup table.
 */
struct timer_slot {
	struct uloop_timeout t;
	struct proto_timer *timer;
};

_Static_assert(sizeof(struct timer_slot) <= PROTO_TIMER_PRIV_SIZE,
	       "PROTO_TIMER_PRIV_SIZE is too small for struct uloop_timeout");

static void slot_expire(struct uloop_timeout *ut)
{
	struct timer_slot *slot = container_of(ut, struct timer_slot, t);

	proto_timer_expire(slot->timer);
}

static void uloop_timer_set(struct proto_timer_env *env, struct proto_timer *t,
			    unsigned int ms)
{
	struct timer_slot *slot = (struct timer_slot *)t->priv.bytes;

	slot->timer = t;
	slot->t.cb = slot_expire;
	uloop_timeout_set(&slot->t, (int)ms);
}

static void uloop_timer_cancel(struct proto_timer_env *env,
			       struct proto_timer *t)
{
	struct timer_slot *slot = (struct timer_slot *)t->priv.bytes;

	uloop_timeout_cancel(&slot->t);
}

static uint64_t uloop_now_ms(struct proto_timer_env *env)
{
	return mono_ms();
}

void proto_timer_env_uloop_init(struct proto_timer_env *env)
{
	env->now_ms = uloop_now_ms;
	env->timer_set = uloop_timer_set;
	env->timer_cancel = uloop_timer_cancel;
}
