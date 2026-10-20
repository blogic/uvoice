// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <math.h>
#include <string.h>

#include "dsp.h"

static const char digit_map[4][4] = {
	{ '1', '2', '3', 'A' },
	{ '4', '5', '6', 'B' },
	{ '7', '8', '9', 'C' },
	{ '*', '0', '#', 'D' },
};

static const double row_frequency[4] = { 697.0, 770.0, 852.0, 941.0 };
static const double column_frequency[4] = { 1209.0, 1336.0, 1477.0, 1633.0 };

static bool digit_frequencies_get(char digit, double *low, double *high)
{
	unsigned row;

	if (digit >= 'a' && digit <= 'd')
		digit -= 'a' - 'A';
	for (row = 0; row < 4; row++) {
		unsigned column;

		for (column = 0; column < 4; column++) {
			if (digit_map[row][column] != digit)
				continue;
			*low = row_frequency[row];
			*high = column_frequency[column];
			return true;
		}
	}
	return false;
}

void udsp_dtmf_generator_init(struct udsp_dtmf_generator *generator,
			      char digit, int level_dbm0, unsigned rate_hz)
{
	memset(generator, 0, sizeof(*generator));
	generator->rate_hz = rate_hz;
	generator->digit = digit;
	generator->level_dbm0 = level_dbm0;
}

bool udsp_dtmf_generator_render(struct udsp_dtmf_generator *generator,
				int16_t *samples, size_t count)
{
	double low_frequency;
	double high_frequency;
	double low_amplitude;
	double high_amplitude;
	size_t i;

	if (!digit_frequencies_get(generator->digit, &low_frequency,
				   &high_frequency)) {
		memset(samples, 0, count * sizeof(*samples));
		return false;
	}
	low_amplitude = udsp_dbm0_amplitude(generator->level_dbm0);
	high_amplitude = udsp_dbm0_amplitude(generator->level_dbm0 +
					     UDSP_DTMF_TWIST_DB);
	for (i = 0; i < count; i++) {
		double value = low_amplitude * sin(generator->low.phase) +
			high_amplitude * sin(generator->high.phase);

		samples[i] = udsp_sat16_round(value);
		generator->low.phase += 2.0 * M_PI * low_frequency /
			generator->rate_hz;
		generator->high.phase += 2.0 * M_PI * high_frequency /
			generator->rate_hz;
		if (generator->low.phase >= 2.0 * M_PI)
			generator->low.phase -= 2.0 * M_PI;
		if (generator->high.phase >= 2.0 * M_PI)
			generator->high.phase -= 2.0 * M_PI;
	}
	return true;
}
