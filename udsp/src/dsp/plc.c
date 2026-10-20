// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <math.h>
#include <string.h>

#include "plc.h"

#define PLC_CORRELATION_POWER	250.0f
#define PLC_DECIMATION		2U
#define PLC_ATTENUATION		0.2f

static int16_t sample_saturate(float sample)
{
	if (sample > 32767.0f)
		return 32767;
	if (sample < -32768.0f)
		return -32768;
	return (int16_t)sample;
}

static float float_saturate(float sample)
{
	if (sample > 32767.0f)
		return 32767.0f;
	if (sample < -32768.0f)
		return -32768.0f;
	return sample;
}

static void plc_frame_save(struct udsp_plc *plc, int16_t *samples)
{
	const struct udsp_plc_geometry *g = &plc->geometry;
	unsigned delay = g->pitch_max / 4;

	memmove(plc->history, plc->history + g->frame,
		(g->history_len - g->frame) * sizeof(plc->history[0]));
	memcpy(plc->history + g->history_len - g->frame, samples,
	       g->frame * sizeof(*samples));
	memcpy(samples, plc->history + g->history_len - g->frame - delay,
	       g->frame * sizeof(*samples));
}

static void pitch_samples_get(struct udsp_plc *plc, int16_t *samples,
			      unsigned count)
{
	while (count) {
		unsigned available = plc->pitch_length - plc->pitch_offset;
		unsigned copy = available < count ? available : count;
		unsigned i;

		for (i = 0; i < copy; i++)
			samples[i] = (int16_t)plc->pitch_buffer[
				plc->pitch_start + plc->pitch_offset + i];
		plc->pitch_offset += copy;
		if (plc->pitch_offset == plc->pitch_length)
			plc->pitch_offset = 0;
		samples += copy;
		count -= copy;
	}
}

static void overlap_float(const float *left, const float *right, float *out,
			  unsigned count)
{
	float increment;
	float left_weight;
	float right_weight;
	unsigned i;

	if (!count)
		return;
	increment = 1.0f / count;
	left_weight = 1.0f - increment;
	right_weight = increment;
	for (i = 0; i < count; i++) {
		out[i] = float_saturate(left_weight * left[i] +
					 right_weight * right[i]);
		left_weight -= increment;
		right_weight += increment;
	}
}

static void overlap_pcm(const int16_t *left, const int16_t *right,
			int16_t *out, unsigned count)
{
	float increment;
	float left_weight;
	float right_weight;
	unsigned i;

	if (!count)
		return;
	increment = 1.0f / count;
	left_weight = 1.0f - increment;
	right_weight = increment;
	for (i = 0; i < count; i++) {
		out[i] = sample_saturate(left_weight * left[i] +
					 right_weight * right[i]);
		left_weight -= increment;
		right_weight += increment;
	}
}

static float correlation_get(const struct udsp_plc *plc, const float *left,
			     const float *right, unsigned stride, float energy)
{
	float correlation = 0.0f;
	unsigned i;

	for (i = 0; i < plc->geometry.corr_len; i += stride)
		correlation += left[i] * right[i];
	if (energy < PLC_CORRELATION_POWER)
		energy = PLC_CORRELATION_POWER;
	return correlation / sqrtf(energy);
}

static unsigned pitch_find(const struct udsp_plc *plc)
{
	const struct udsp_plc_geometry *g = &plc->geometry;
	const float *left = plc->pitch_buffer + g->history_len - g->corr_len;
	const float *right = plc->pitch_buffer + g->history_len - g->corr_buf;
	const float *candidate = right;
	unsigned pitch_diff = g->pitch_max - g->pitch_min;
	float best_correlation;
	float energy = 0.0f;
	unsigned best_match = 0;
	unsigned start;
	unsigned end;
	unsigned i;
	unsigned offset;

	for (i = 0; i < g->corr_len; i += PLC_DECIMATION)
		energy += candidate[i] * candidate[i];
	best_correlation = correlation_get(plc, left, candidate,
					   PLC_DECIMATION, energy);
	for (offset = PLC_DECIMATION; offset <= pitch_diff;
	     offset += PLC_DECIMATION) {
		float correlation;

		energy -= candidate[0] * candidate[0];
		energy += candidate[g->corr_len] * candidate[g->corr_len];
		candidate += PLC_DECIMATION;
		correlation = correlation_get(plc, left, candidate,
					      PLC_DECIMATION, energy);
		if (correlation < best_correlation)
			continue;
		best_correlation = correlation;
		best_match = offset;
	}
	start = best_match >= PLC_DECIMATION - 1 ?
		best_match - (PLC_DECIMATION - 1) : 0;
	end = best_match + PLC_DECIMATION - 1;
	if (end > pitch_diff)
		end = pitch_diff;
	candidate = right + start;
	energy = 0.0f;
	for (i = 0; i < g->corr_len; i++)
		energy += candidate[i] * candidate[i];
	best_correlation = correlation_get(plc, left, candidate, 1, energy);
	best_match = start;
	for (offset = start + 1; offset <= end; offset++) {
		float correlation;

		energy -= candidate[0] * candidate[0];
		energy += candidate[g->corr_len] * candidate[g->corr_len];
		candidate++;
		correlation = correlation_get(plc, left, candidate, 1, energy);
		if (correlation <= best_correlation)
			continue;
		best_correlation = correlation;
		best_match = offset;
	}
	return g->pitch_max - best_match;
}

static void pitch_buffer_extend(struct udsp_plc *plc)
{
	unsigned history_len = plc->geometry.history_len;

	plc->pitch_length += plc->pitch;
	plc->pitch_start = history_len - plc->pitch_length;
	overlap_float(plc->last_quarter,
		      plc->pitch_buffer + plc->pitch_start - plc->pitch_overlap,
		      plc->pitch_buffer + history_len - plc->pitch_overlap,
		      plc->pitch_overlap);
}

static void concealed_frame_scale(const struct udsp_plc *plc, int16_t *samples)
{
	float gain = 1.0f - (plc->loss_frames - 1) * PLC_ATTENUATION;
	float step = PLC_ATTENUATION / plc->geometry.frame;
	unsigned i;

	for (i = 0; i < plc->geometry.frame; i++) {
		samples[i] = (int16_t)(samples[i] * gain);
		gain -= step;
	}
}

static void recovery_apply(struct udsp_plc *plc, int16_t *samples)
{
	int16_t synthetic[UDSP_PLC_FRAME_MAX];
	unsigned increment_count = plc->geometry.frame * 2 / 5;
	unsigned count = plc->pitch_overlap +
		(plc->loss_frames - 1) * increment_count;
	float gain = 1.0f - (plc->loss_frames - 1) * PLC_ATTENUATION;
	float increment;
	float synthetic_weight;
	float input_weight;
	unsigned i;

	if (count > plc->geometry.frame)
		count = plc->geometry.frame;
	pitch_samples_get(plc, synthetic, count);
	if (gain < 0.0f)
		gain = 0.0f;
	increment = 1.0f / count;
	synthetic_weight = (1.0f - increment) * gain;
	input_weight = increment;
	for (i = 0; i < count; i++) {
		samples[i] = sample_saturate(synthetic_weight * synthetic[i] +
					     input_weight * samples[i]);
		synthetic_weight -= increment * gain;
		input_weight += increment;
	}
}

void udsp_plc_init(struct udsp_plc *plc, unsigned rate, unsigned frame)
{
	struct udsp_plc_geometry *g;

	memset(plc, 0, sizeof(*plc));
	g = &plc->geometry;
	g->frame = frame;
	g->pitch_min = rate / 200;
	g->pitch_max = rate * 3 / 200;
	g->history_len = rate * 39 / 800;
	g->corr_len = rate / 50;
	g->corr_buf = rate * 7 / 200;
	/* The void API supports two rates, so clamp derived sizes to storage. */
	if (g->frame > UDSP_PLC_FRAME_MAX)
		g->frame = UDSP_PLC_FRAME_MAX;
	if (g->pitch_max > UDSP_PLC_QUARTER_MAX * 4)
		g->pitch_max = UDSP_PLC_QUARTER_MAX * 4;
	if (g->pitch_min > g->pitch_max)
		g->pitch_min = g->pitch_max;
	if (g->history_len > UDSP_PLC_HISTORY_MAX)
		g->history_len = UDSP_PLC_HISTORY_MAX;
	if (g->corr_len > g->history_len)
		g->corr_len = g->history_len;
	if (g->corr_buf > g->history_len)
		g->corr_buf = g->history_len;
}

void udsp_plc_reset(struct udsp_plc *plc)
{
	struct udsp_plc_geometry geometry = plc->geometry;

	memset(plc, 0, sizeof(*plc));
	plc->geometry = geometry;
}

void udsp_plc_frame_process(struct udsp_plc *plc, int16_t *samples)
{
	if (plc->loss_frames) {
		recovery_apply(plc, samples);
		plc->loss_frames = 0;
	}
	plc_frame_save(plc, samples);
}

void udsp_plc_generate(struct udsp_plc *plc, int16_t *samples)
{
	unsigned history_len = plc->geometry.history_len;
	unsigned frame = plc->geometry.frame;

	if (!plc->loss_frames) {
		unsigned i;

		for (i = 0; i < history_len; i++)
			plc->pitch_buffer[i] = plc->history[i];
		plc->pitch = pitch_find(plc);
		plc->pitch_overlap = plc->pitch / 4;
		if (plc->pitch_overlap > UDSP_PLC_QUARTER_MAX)
			plc->pitch_overlap = UDSP_PLC_QUARTER_MAX;
		memcpy(plc->last_quarter,
		       plc->pitch_buffer + history_len - plc->pitch_overlap,
		       plc->pitch_overlap * sizeof(plc->last_quarter[0]));
		plc->pitch_offset = 0;
		plc->pitch_length = plc->pitch;
		plc->pitch_start = history_len - plc->pitch_length;
		overlap_float(plc->last_quarter,
			      plc->pitch_buffer + plc->pitch_start -
			      plc->pitch_overlap,
			      plc->pitch_buffer + history_len - plc->pitch_overlap,
			      plc->pitch_overlap);
		for (i = 0; i < plc->pitch_overlap; i++)
			plc->history[history_len - plc->pitch_overlap + i] =
				(int16_t)plc->pitch_buffer[history_len -
				plc->pitch_overlap + i];
		pitch_samples_get(plc, samples, frame);
	} else if (plc->loss_frames == 1 || plc->loss_frames == 2) {
		int16_t old_pitch[UDSP_PLC_QUARTER_MAX];
		unsigned saved_offset = plc->pitch_offset;

		pitch_samples_get(plc, old_pitch, plc->pitch_overlap);
		plc->pitch_offset = saved_offset;
		while (plc->pitch_offset > plc->pitch)
			plc->pitch_offset -= plc->pitch;
		pitch_buffer_extend(plc);
		pitch_samples_get(plc, samples, frame);
		overlap_pcm(old_pitch, samples, samples, plc->pitch_overlap);
		concealed_frame_scale(plc, samples);
	} else if (plc->loss_frames > 5) {
		memset(samples, 0, frame * sizeof(*samples));
	} else {
		pitch_samples_get(plc, samples, frame);
		concealed_frame_scale(plc, samples);
	}
	plc->loss_frames++;
	plc_frame_save(plc, samples);
}
