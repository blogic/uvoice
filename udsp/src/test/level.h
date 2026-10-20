// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The level plan of the test scaffolding: dBm0 against int16 samples, and a
 * loss in half-dB steps. The suites and the benches read one file, so a level
 * means the same thing in each of them.
 */
#ifndef UDSP_TEST_LEVEL_H
#define UDSP_TEST_LEVEL_H

#include <math.h>
#include <stdint.h>

#include "../dsp/dsp.h"

/*
 * 0 dBm0 is 3.14 dB below a full-scale sine: the level plan that
 * dsp/tone_gen.c already encodes.
 */
static inline double dbm0_rms_get(double level_dbm0)
{
	return 32767.0 * pow(10.0, (level_dbm0 - 3.14) / 20.0) / sqrt(2.0);
}

static inline double rms_dbm0_get(double rms)
{
	if (rms < 1e-6)
		return -200.0;
	return 20.0 * log10(rms / dbm0_rms_get(0.0));
}

/*
 * The arithmetic of udsp_gain_apply in dsp/gain.c, for a loss into another
 * buffer. test_lec and bench_lec do not link dsp/gain.c, and a loss that
 * rounds as the engine rounds keeps the reference of a case equal to the
 * reference the engine would give the canceller.
 */
static inline void level_loss_apply(int16_t *dst, const int16_t *src,
				    unsigned count, unsigned half_db)
{
	double factor = pow(10.0, -(double)half_db / 40.0);
	unsigned i;

	for (i = 0; i < count; i++)
		dst[i] = udsp_sat16_round(src[i] * factor);
}

#endif
