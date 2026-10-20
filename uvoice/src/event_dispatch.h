// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The HAL event dispatch, in a translation unit of its own.
 *
 * These bodies decide what happens to every hardware record the backend
 * writes: which line event reaches the line shell, what a fault word means,
 * and how many records the transport lost. They live outside main.c, which a
 * test binary cannot link: main() owns the uloop, the config, the ubus
 * connection and the socketpair, and linking main.c into a suite brings all of
 * it. A test binary gets these bodies without main(), so the
 * fault-gate variants and the sequence walk can be driven by a suite rather
 * than argued by inspection.
 *
 * The unit asks the daemon two questions it cannot answer itself, and they are
 * the only two: whether the teardown already owns the daemon, and to leave for
 * the procd respawn. uvoiced defines both in main.c over its own statics
 * (dispatch_exit_request over exit_code plus uloop_end, dispatch_stop_pending
 * over stop_under_way); the dispatch suite defines its own recorders instead.
 * Everything else the unit reads is already external: uv, line_hal_event and
 * hal_backend_ops.
 */
#ifndef UVOICE_EVENT_DISPATCH_H
#define UVOICE_EVENT_DISPATCH_H

#include <stdbool.h>

#include "hal/hal.h"

void event_dispatch(const struct hal_event *ev);
void event_seq_check(const struct hal_event *ev);
void event_fault_dispatch(const struct hal_event *ev);

void dispatch_exit_request(void);
bool dispatch_stop_pending(void);

#endif
