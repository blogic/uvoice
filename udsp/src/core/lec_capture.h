// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The ring of the canceller capture (udsp.h struct udsp_lec_capture).
 *
 * One ring holds the records of every line, because the engine tick runs
 * every line in one thread and is the one producer. One consumer pops it, the
 * caller of udsp_lec_capture_read. The ring is the heap form of core/spsc.h.
 * The tick claims the head slot, writes the record in place and publishes it.
 * A record of 1024 bytes therefore never sits on the stack of the tick.
 *
 * A ring of 1024 slots costs 1 MiB. That is too much for a handle to carry
 * unused, so udsp_lec_capture_enable allocates the ring on the control thread
 * and makes every page resident. udsp_close frees it once the tick stopped.
 *
 * The loss count lives in the channel and not in the ring (core/chan.h
 * lec_cap_lost). Each line therefore reads its own losses, although the lines
 * share the ring. The producer allocates nothing, takes no lock, logs nothing
 * and reads no clock of its own.
 */
#ifndef UDSP_CORE_LEC_CAPTURE_H
#define UDSP_CORE_LEC_CAPTURE_H

#include "udsp/udsp.h"

#include "../dsp/dsp.h"

#include "spsc.h"

/* At one record per line per 10 ms tick, this holds about ten seconds of one
 * line. A reader that drains once per second needs less. One slot tells a
 * full ring from an empty one, so 1023 records are usable. A test shrinks the
 * ring through UDSP_TEST_LEC_CAPTURE_SLOTS (core/udsp.c). */
#define UDSP_LEC_CAPTURE_SLOTS	1024u
/* The noise_ddbov of a floor that is not measured: the 127 dBov that
 * dsp/lec.h reports for it, in tenths of dB. */
#define UDSP_LEC_CAPTURE_NOISE_UNMEASURED	1270u

_Static_assert(UDSP_LEC_CAPTURE_SAMPLES == UDSP_FRAME_MAX,
	       "a capture plane must hold one frame of the widest line");

UDSP_SPSC_HEAP_DECLARE(lec_capture_ring, struct udsp_lec_capture);

#endif
