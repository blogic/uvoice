// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <math.h>

#include "dsp.h"

void udsp_gain_apply(int16_t *samples, size_t count, int half_db_steps)
{
	double factor = pow(10.0, half_db_steps / 40.0);
	size_t i;

	for (i = 0; i < count; i++) {
		double sample = samples[i] * factor;

		samples[i] = udsp_sat16_round(sample);
	}
}
