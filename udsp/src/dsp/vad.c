// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <math.h>
#include <string.h>

#include "dsp.h"

static const double spectrum_coefficient[] = {
	1.9938346674662559, 1.9753766811902755, 1.9447398407953531,
	1.9021130325903071, 1.8477590650225735, 1.7820130483767358,
	1.7052803287081844, 1.6180339887498949, 1.5208119312000619,
	1.4142135623730951, 1.2988960966603675, 1.1755705045849463,
	1.0449971294318978, 0.90798099947909361, 0.76536686473017967,
	0.6180339887498949, 0.46689072771181095, 0.31286893008046185,
	0.15691819145569, 1.2246467991473532e-16, -0.15691819145568975,
	-0.31286893008046118, -0.46689072771181067, -0.61803398874989468,
	-0.76536686473017945, -0.90798099947909339, -1.0449971294318976,
	-1.1755705045849461, -1.2988960966603671, -1.4142135623730949,
	-1.5208119312000619, -1.6180339887498947, -1.7052803287081844,
	-1.7820130483767356, -1.8477590650225735, -1.9021130325903071,
	-1.9447398407953531, -1.9753766811902753, -1.9938346674662559,
};

static double flatness_get(const int16_t *samples)
{
	double arithmetic = 0.0;
	double logarithmic = 0.0;
	const unsigned bins = sizeof(spectrum_coefficient) /
		sizeof(spectrum_coefficient[0]);
	unsigned bin;

	for (bin = 0; bin < bins; bin++) {
		double coefficient = spectrum_coefficient[bin];
		double q1 = 0.0;
		double q2 = 0.0;
		double power;
		unsigned n;

		for (n = 0; n < UDSP_FRAME_SAMPLES; n++) {
			double q0 = samples[n] + coefficient * q1 - q2;

			q2 = q1;
			q1 = q0;
		}
		power = q1 * q1 + q2 * q2 - coefficient * q1 * q2 + 1.0;
		arithmetic += power;
		logarithmic += log(power);
	}
	return exp(logarithmic / bins) / (arithmetic / bins);
}

void udsp_vad_init(struct udsp_vad *vad)
{
	memset(vad, 0, sizeof(*vad));
	vad->noise_energy = 1000.0;
}

bool udsp_vad_process(struct udsp_vad *vad, const int16_t *samples)
{
	double energy = 0.0;
	double flatness;
	bool voice;
	unsigned i;

	for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
		energy += (double)samples[i] * samples[i];
	energy /= UDSP_FRAME_SAMPLES;
	flatness = flatness_get(samples);
	voice = energy > 8000.0 && energy > vad->noise_energy * 4.0 &&
		flatness < 0.35;
	if (voice) {
		vad->hangover = 5;
		return true;
	}
	if (!vad->initialized) {
		vad->noise_energy = energy;
		vad->initialized = true;
	} else if (energy < vad->noise_energy) {
		vad->noise_energy = vad->noise_energy * 0.95 + energy * 0.05;
	} else {
		vad->noise_energy = vad->noise_energy * 0.98 + energy * 0.02;
	}
	if (!vad->hangover)
		return false;
	vad->hangover--;
	return true;
}
