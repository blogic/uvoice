// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <math.h>
#include <string.h>

#include "dsp.h"

static bool cadence_advance(struct udsp_tone_generator *generator)
{
	unsigned limit = generator->desc->cadence_count * 2 + 1;
	unsigned transitions = 0;

	while (transitions++ < limit) {
		const struct udsp_tone_cadence *cadence;
		uint32_t duration;

		cadence = &generator->desc->cadence[generator->cadence_index];
		duration = (generator->make ? cadence->make_ms :
			cadence->break_ms) * (generator->rate_hz / 1000);
		if (duration && generator->cadence_samples < duration)
			return true;
		generator->cadence_samples = 0;
		generator->make = !generator->make;
		if (generator->make) {
			generator->cadence_index++;
			if (generator->cadence_index < generator->desc->cadence_count)
				continue;
			if (!generator->desc->repeat) {
				generator->complete = true;
				return false;
			}
			generator->cadence_index = 0;
		}
	}
	generator->complete = true;
	return false;
}

bool udsp_tone_generator_init(struct udsp_tone_generator *generator,
			      const struct udsp_tone_desc *desc,
			      unsigned rate_hz)
{
	unsigned i;

	memset(generator, 0, sizeof(*generator));
	generator->rate_hz = rate_hz;
	if (!desc || desc->tone > UDSP_TONE_NUMBER_UNOBTAINABLE ||
	    desc->frequency_count > UDSP_TONE_FREQ_MAX ||
	    desc->cadence_count > UDSP_TONE_CADENCE_MAX)
		return false;
	for (i = 0; i < desc->frequency_count; i++) {
		if (desc->frequency_hz[i] < 100 ||
		    desc->frequency_hz[i] > 3400)
			return false;
	}
	generator->desc = desc;
	for (i = 0; i < desc->frequency_count; i++)
		generator->amplitude[i] =
			udsp_dbm0_amplitude(desc->level_dbm0[i]);
	generator->make = true;
	generator->complete = desc->tone == UDSP_TONE_NONE;
	return true;
}

bool udsp_tone_generator_render(struct udsp_tone_generator *generator,
				int16_t *samples, size_t count)
{
	size_t i;

	if (!generator->desc || generator->complete) {
		memset(samples, 0, count * sizeof(*samples));
		return false;
	}
	for (i = 0; i < count; i++) {
		double value = 0.0;
		unsigned frequency;

		if (generator->desc->cadence_count && !cadence_advance(generator)) {
			memset(samples + i, 0, (count - i) * sizeof(*samples));
			return false;
		}
		if (generator->make) {
			for (frequency = 0;
			     frequency < generator->desc->frequency_count;
			     frequency++) {
				value += generator->amplitude[frequency] *
					sin(generator->oscillator[frequency].phase);
				generator->oscillator[frequency].phase += 2.0 * M_PI *
					generator->desc->frequency_hz[frequency] /
					generator->rate_hz;
				if (generator->oscillator[frequency].phase >= 2.0 * M_PI)
					generator->oscillator[frequency].phase -= 2.0 * M_PI;
			}
		}
		samples[i] = udsp_sat16_round(value);
		generator->cadence_samples++;
	}
	return !generator->complete;
}
