// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The async-signal-safe safe state. At attach the plug-in hands over a
 * pre-framed spidev transfer that opens every linefeed and stops every ring;
 * this module lays it out into spidev messages once, at arm time, and the
 * fatal-signal handler does nothing but issue them and re-raise. It never
 * calls the plug-in, dlopen'd code, pthread, malloc or the host semaphore.
 *
 * Per-byte chip select is the measured production framing on MT7987, which
 * costs one segment per byte and therefore up to
 * ceil(UDSP_SLIC_SAFE_MAX / SLIC_HOST_SEGS_MAX) = 16 bare ioctls.
 *
 * Limit: the blit can interleave with live slic-thread traffic,
 * and the safe state can be overwritten in the window between the blit and
 * the re-raise. The design is best effort by construction.
 */
#ifndef UDSP_CORE_SAFE_STATE_H
#define UDSP_CORE_SAFE_STATE_H

#include "../slic/host_linux.h"

/* Redirects the blit so a host test can record the messages instead of issuing
 * them; ctx is the recorder's page. Passing NULL restores the ioctl. */
void udsp_safe_state_msg_set(slic_host_msg_fn fn, void *ctx);

#endif
