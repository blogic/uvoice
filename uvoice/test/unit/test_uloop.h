// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The bounded loop window of the unit suites that link libubox. The suite
 * calls uloop_init() before the first window and uloop_done() at the end.
 */
#ifndef UVOICE_TEST_ULOOP_H
#define UVOICE_TEST_ULOOP_H

#include <libubox/uloop.h>

static inline void uloop_pump_stop(struct uloop_timeout *t)
{
	uloop_end();
}

/* The test-timing rule of the README: drive the loop to the deadline, never
 * sleep on a wall clock. */
static inline void uloop_pump(unsigned int ms)
{
	static struct uloop_timeout stop;

	stop.cb = uloop_pump_stop;
	uloop_timeout_set(&stop, (int)ms);
	uloop_cancelled = false;
	uloop_run();
	uloop_timeout_cancel(&stop);
}

#endif
