// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The timer and its environment, shared by every signaling protocol: one
 * clock, a timer pair, an entropy hook and a trace sink. A protocol state
 * machine runs its timers through the environment, so that a suite drives
 * the same machine on a virtual clock. Elsewhere a protocol can use uloop
 * directly.
 *
 * Embedding. A protocol whose layer needs more than the clock embeds struct
 * proto_timer_env as the first member of its own environment. The SIP
 * transaction environment does that and adds its transport, its flags and its
 * upcalls, because the destination of a SIP send is a SIP type. A pointer to
 * the embedding environment then converts to a pointer to the embedded one
 * and back.
 */
#ifndef UVOICE_PROTO_SHARED_TIMER_H
#define UVOICE_PROTO_SHARED_TIMER_H

#include <stdbool.h>
#include <stdint.h>

struct proto_timer;
struct proto_timer_env;

typedef void (*proto_timer_cb)(struct proto_timer *t);

/*
 * priv is environment scratch. The uloop environment stores its uloop_timeout
 * there, which keeps <libubox/uloop.h> out of this header and still costs no
 * allocation. The uloop binding asserts the size at compile time.
 */
#define PROTO_TIMER_PRIV_SIZE	72

/*
 * One timer. owner and id belong to the owner: the owner gives id its own
 * vocabulary, and this layer stores it and reads nothing from it. due_ms is
 * the expiry a virtual clock environment records; the uloop environment
 * leaves it alone.
 */
struct proto_timer {
	proto_timer_cb fire;
	void *owner;
	uint64_t due_ms;
	union {
		void *ptr;
		uint64_t u64;
		char bytes[PROTO_TIMER_PRIV_SIZE];
	} priv;
	uint8_t id;
	bool armed;
};

/*
 * The environment. now_ms is a monotonic clock in milliseconds. timer_set
 * and timer_cancel run one timer on it. rand32 can be NULL, and the caller
 * then uses a generator of its own. trace takes one preformatted line and can
 * be NULL.
 */
struct proto_timer_env {
	void *priv;

	uint64_t (*now_ms)(struct proto_timer_env *env);
	void (*timer_set)(struct proto_timer_env *env, struct proto_timer *t,
			  unsigned int ms);
	void (*timer_cancel)(struct proto_timer_env *env,
			     struct proto_timer *t);
	uint32_t (*rand32)(struct proto_timer_env *env);
	void (*trace)(struct proto_timer_env *env, const char *msg);
};

/* Clear t and store its owner, its id and the callback its expiry runs. id
 * is below 256, because the timer stores it in a uint8_t. */
void proto_timer_init(struct proto_timer *t, void *owner, unsigned int id,
		      proto_timer_cb cb);

/* Arm t to expire ms from now. A timer that is armed already starts again. */
void proto_timer_arm(struct proto_timer_env *env, struct proto_timer *t,
		     unsigned int ms);

/* Stop t. A timer that is not armed is left alone. */
void proto_timer_cancel(struct proto_timer_env *env, struct proto_timer *t);

/* The environment calls this when an armed timer expires: it clears armed
 * and runs the callback. */
void proto_timer_expire(struct proto_timer *t);

/*
 * Fill now_ms, timer_set and timer_cancel with the uloop implementation. It
 * leaves priv, rand32 and trace to the caller.
 */
void proto_timer_env_uloop_init(struct proto_timer_env *env);

#endif
