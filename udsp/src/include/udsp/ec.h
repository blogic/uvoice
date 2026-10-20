// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * udsp echo canceller contract. Stable contract. One instance per line. The
 * NLMS LEC (dsp/lec.c) is the default; the vtable lets a host reference
 * (speexdsp MDF) stand in for A/B during tuning.
 *
 * Frames are rate/100 samples (80 at the 8 kHz default). run cancels the
 * near-end echo of ref (the DL frame, delayed by the measured loop delay of
 * prefill + DMA + SLIC) out of mic, in place. The tail is stated after the
 * bulk delay. freeze holds adaptation during a detected tone or fax carrier.
 * nlp toggles the non-linear processor and its comfort noise.
 */
#ifndef UDSP_EC_H
#define UDSP_EC_H

#include <stdbool.h>
#include <stdint.h>

struct udsp_ec;

struct udsp_ec_ops {
	const char *name;
	struct udsp_ec *(*create)(unsigned tail_ms);
	void (*destroy)(struct udsp_ec *e);
	void (*run)(struct udsp_ec *e, int16_t *mic, const int16_t *ref);
	void (*freeze)(struct udsp_ec *e, bool on);
	void (*nlp)(struct udsp_ec *e, bool on);
	/* Nullable, appended so every prior member keeps its offset: create
	 * at an explicit line rate; run then consumes the create-time frame
	 * length, rate_hz/100 samples (create above stays the 8 kHz form). A
	 * 16 kHz handle whose vtable lacks this member runs the line without
	 * echo cancellation and logs it; udsp_ec_nlms implements it, so only
	 * a substitute vtable hits that arm. */
	struct udsp_ec *(*create_rate)(unsigned tail_ms, unsigned rate_hz);
};

#endif
