// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * This API converts signed Q0 PCM between 8 kHz and 16 kHz in fixed 2:1
 * steps. Down2 writes n_in / 2 samples; up2 writes 2 * n_in samples.
 * Down2 accepts at most UDSP_FRAME_MAX inputs, and up2 accepts at most
 * UDSP_FRAME_MAX / 2 inputs. A trailing odd down2 sample is ignored; the
 * engine only issues even counts. Use one state instance for each direction.
 * Reset each instance before its first use. The stream layer resets its
 * instances on adopt and reconfig; each detector feed resets its instance on
 * arm. Down2 permits in-place operation, but up2 requires separate buffers.
 *
 * Each direction has a 23-sample group delay at 16 kHz. An up2-then-down2
 * pair has a 45-sample composite delay, which is 22.5 samples at 8 kHz.
 * The folded gain of 2 puts the passband peak 0.013 dB above unity. Thus, an
 * input within 0.013 dB of full scale can clip. A reset also costs a
 * 23-sample ramp with transient overshoot.
 */
#ifndef UDSP_RESAMP_H
#define UDSP_RESAMP_H

#include <stdint.h>

#define UDSP_RESAMP_TAPS	47U
#define UDSP_RESAMP_DELAY	23U

/*
 * The input window is kept twice, so a tap loop reads it as one forward run
 * with no wrap test per tap: history[index + k] is the input delayed by k
 * samples for every k under UDSP_RESAMP_TAPS. index is the position of the
 * newest sample and walks downwards; a push writes the sample at index and at
 * index + UDSP_RESAMP_TAPS.
 */
struct udsp_resamp {
	int16_t history[2U * UDSP_RESAMP_TAPS];
	unsigned index;
};

void udsp_resamp_reset(struct udsp_resamp *rs);
void udsp_resamp_down2(struct udsp_resamp *rs, const int16_t *in,
		       unsigned n_in, int16_t *out);
void udsp_resamp_up2(struct udsp_resamp *rs, const int16_t *in,
		     unsigned n_in, int16_t *out);

#ifdef UDSP_RESAMP_TEST
int16_t udsp_resamp_coefficient_get(unsigned index);
#endif

#endif
