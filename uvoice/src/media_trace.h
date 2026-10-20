// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The two media trace domains, and the one place that turns them into HAL
 * calls.
 *
 * TRACE_JB is the backend's trace: the daemon only tells the backend to arm
 * it, through hal_ops.debug_set, and the backend logs one line per jitter
 * buffer decision that changed something. Nothing of that path runs here.
 *
 * TRACE_MEDIA is this module's own: a one second sweep that asks every stream
 * inside the HAL cap for its jitter buffer counters, and for its media
 * counters where the backend answers them fast, and writes one line per
 * stream. It is a summary and not a per-decision trace, so an operator can
 * leave it on through a call.
 *
 * Zero cost while both are off. No timer is armed, no HAL op is called and the
 * hook is the only thing installed. A backend that answers HAL_UNSUPPORTED to
 * either op is not asked again inside a sweep and its streams are skipped,
 * which is the hal.h rule for that status.
 *
 * One thread. Every entry point runs on the uloop thread, like the trace
 * module it belongs to.
 */
#ifndef UVOICE_MEDIA_TRACE_H
#define UVOICE_MEDIA_TRACE_H

/* Installs the trace hook. Runs once, before the HAL and ubus come up, so a
 * domain the config or the command line named is applied by the first sync. */
void media_trace_init(void);

/*
 * Applies the current domain state to the HAL and to the sweep timer. main.c
 * calls it at the UVOICE_HAL_READY transition as well as from the hook,
 * because a domain switched on before the DSP was up has no backend to tell.
 */
void media_trace_sync(void);

/* Cancels the sweep. Runs before the HAL closes, so no sweep can name a
 * backend that is gone. */
void media_trace_done(void);

#endif
