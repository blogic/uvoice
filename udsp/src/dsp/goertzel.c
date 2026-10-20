// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <math.h>
#include <string.h>

#include "dsp.h"

void udsp_goertzel_init(struct udsp_goertzel_bank *bank,
			const double *frequencies, unsigned count)
{
	unsigned i;

	memset(bank, 0, sizeof(*bank));
	bank->count = count > UDSP_GOERTZEL_MAX ? UDSP_GOERTZEL_MAX : count;
	for (i = 0; i < bank->count; i++) {
		bank->coeff[i] = 2.0 * cos(2.0 * M_PI * frequencies[i] /
					    UDSP_SAMPLE_RATE);
		bank->sine[i] = sin(2.0 * M_PI * frequencies[i] /
				    UDSP_SAMPLE_RATE);
	}
}

void udsp_goertzel_reset(struct udsp_goertzel_bank *bank)
{
	memset(bank->q1, 0, sizeof(bank->q1));
	memset(bank->q2, 0, sizeof(bank->q2));
}

void udsp_goertzel_reset_index(struct udsp_goertzel_bank *bank, unsigned index)
{
	if (index >= bank->count)
		return;
	bank->q1[index] = 0.0;
	bank->q2[index] = 0.0;
}

void udsp_goertzel_process(struct udsp_goertzel_bank *bank,
			   const int16_t *samples, size_t count)
{
	size_t n;

	for (n = 0; n < count; n++) {
		unsigned i;

		for (i = 0; i < bank->count; i++) {
			double q0 = samples[n] + bank->coeff[i] * bank->q1[i] -
				bank->q2[i];

			bank->q2[i] = bank->q1[i];
			bank->q1[i] = q0;
		}
	}
}

void udsp_goertzel_process_index(struct udsp_goertzel_bank *bank, unsigned index,
				 const int16_t *samples, size_t count)
{
	size_t n;

	if (index >= bank->count)
		return;
	for (n = 0; n < count; n++) {
		double q0 = samples[n] + bank->coeff[index] * bank->q1[index] -
			bank->q2[index];

		bank->q2[index] = bank->q1[index];
		bank->q1[index] = q0;
	}
}

double udsp_goertzel_power(const struct udsp_goertzel_bank *bank,
			   unsigned index)
{
	if (index >= bank->count)
		return 0.0;
	return bank->q1[index] * bank->q1[index] +
		bank->q2[index] * bank->q2[index] -
		bank->coeff[index] * bank->q1[index] * bank->q2[index];
}

void udsp_goertzel_result(const struct udsp_goertzel_bank *bank, unsigned index,
			  struct udsp_goertzel_iq *out)
{
	if (index >= bank->count) {
		out->re = 0.0;
		out->im = 0.0;
		return;
	}
	out->re = bank->q1[index] - bank->q2[index] * (bank->coeff[index] / 2.0);
	out->im = bank->q2[index] * bank->sine[index];
}
