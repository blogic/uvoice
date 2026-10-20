// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The production timer environment for the transaction layer: the uloop
 * environment of proto/shared/timer.c on the embedded struct proto_timer_env.
 * The transaction, UA, dialog and client layers reach uloop through this
 * environment alone, which is what lets the unit tests run the same state
 * machines on a virtual clock.
 */
#include <stddef.h>

#include <libubox/uloop.h>

#include "proto/sip/txn.h"

/* The uloop slot of proto/shared/timer.c lives in the timer's own scratch. */
_Static_assert(sizeof(struct uloop_timeout) + sizeof(void *) <=
	       PROTO_TIMER_PRIV_SIZE,
	       "PROTO_TIMER_PRIV_SIZE is too small for struct uloop_timeout");

/* sip_txn_env_of() converts a base pointer back to its environment. */
_Static_assert(offsetof(struct sip_txn_env, base) == 0,
	       "struct proto_timer_env is the first member of sip_txn_env");

void sip_txn_env_uloop_init(struct sip_txn_env *env)
{
	proto_timer_env_uloop_init(&env->base);
}
