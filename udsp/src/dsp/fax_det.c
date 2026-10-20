// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <math.h>
#include <string.h>

#include "fax_det.h"

#define FRAME_MS 10U
/*
 * A -43 dBm0 sine has 161.6 peak units and 1.044e6 energy per frame.
 * This threshold keeps four percent margin below that value.
 */
#define TONE_MIN_ENERGY 1000000.0
/* The 0.65 purity gate rejects mixed tones; the value is calibrated. */
#define TONE_DOMINANCE_RATIO 0.65
/*
 * T.30 4.2 specifies 1100 Hz plus or minus 38 Hz. The tone runs 0.5 s on
 * and 3 s off, with timing tolerance of plus or minus 15 percent. This ratio
 * captures at least 1100 Hz plus or minus 40 Hz in an 80-sample window.
 */
#define CNG_DOMINANCE_RATIO 0.55
#define CNG_MIN_FRAMES 43U
#define CNG_MAX_FRAMES 58U
#define CNG_REARM_FRAMES 100U
#define ANSWER_MIN_FRAMES 50U
#define MODEM_MIN_FRAMES 101U
/* Two 32 Hz bins tolerate steady-tone bin flap without permitting drift. */
#define MODEM_ANCHOR_BIN_RANGE 2U
#define REVERSAL_MIN_FRAMES 42U
#define REVERSAL_MAX_FRAMES 48U
/* V.8 specifies about 20 percent AM, which yields about 0.1 here. */
#define ANSWER_AM_MIN_RATIO 0.05
#define V21_FLAG_MIN 3U
/* The detector permits 100 ms between validated V.21 flags. */
#define V21_RELEASE_FRAMES 10U
/* The V.21 correlators must capture this share of the frame energy. */
#define V21_CAPTURE_RATIO 0.6
#define V21_MIN_ENERGY 250000.0
#define V21_TABLE_SIZE 160U
/*
 * The high pass the hold energy is measured through, a = 1/(1 + 2*pi*fc/fs)
 * for a cut of 250 Hz at 8000 Hz. G.168 7.4 measures the hold and release
 * levels in the 390 to 3000 Hz band, and this filter stands for the lower edge
 * of it: on the total frame energy a sub-200 Hz hum alone would hold the
 * disabler for ever, which is the opposite of the release rule. The upper edge
 * is left to the band limiting of the path. Both are approximations of the
 * per-band sensitivities of the recommendation.
 */
#define HOLD_HIGHPASS_ALPHA 0.836

static const double named_frequencies[] = { 1100.0, 2100.0 };

static uint32_t duration_get(uint32_t frames)
{
	if (frames > UINT32_MAX / FRAME_MS)
		return UINT32_MAX;
	return frames * FRAME_MS;
}

static size_t result_add(const struct udsp_fax_detector *detector,
			 struct udsp_fax_result *results, size_t count,
			 size_t capacity, uint16_t tone, bool start,
			 uint32_t frames)
{
	uint32_t duration;

	if (count >= capacity)
		return count;
	duration = duration_get(frames);
	/* event.h stores tone duration in uint16_t. */
	if (duration > UINT16_MAX)
		duration = UINT16_MAX;
	results[count].tone = tone;
	results[count].direction = detector->direction;
	results[count].start = start;
	results[count].duration_ms = duration;
	return count + 1U;
}

static bool tone_dominant(double power, double energy, double ratio)
{
	if (energy < TONE_MIN_ENERGY)
		return false;
	return power * 2.0 > energy * UDSP_FRAME_SAMPLES * ratio;
}

static void banks_process(struct udsp_fax_detector *detector,
			  const int16_t *samples, double *energy)
{
	unsigned i;

	*energy = 0.0;
	for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
		*energy += (double)samples[i] * samples[i];
	udsp_goertzel_reset(&detector->named_bank);
	udsp_goertzel_reset(&detector->modem_low_bank);
	udsp_goertzel_reset(&detector->modem_mid_bank);
	udsp_goertzel_reset(&detector->modem_high_bank);
	/* Every reader of a bank power goes through tone_dominant, which
	 * refuses a frame under TONE_MIN_ENERGY whatever the bins read, so the
	 * bins of a quiet frame are never observed and stay at the reset. */
	if (*energy < TONE_MIN_ENERGY)
		return;
	udsp_goertzel_process(&detector->named_bank, samples, UDSP_FRAME_SAMPLES);
	udsp_goertzel_process(&detector->modem_low_bank, samples, UDSP_FRAME_SAMPLES);
	udsp_goertzel_process(&detector->modem_mid_bank, samples, UDSP_FRAME_SAMPLES);
	udsp_goertzel_process(&detector->modem_high_bank, samples, UDSP_FRAME_SAMPLES);
}

static void hold_energy_update(struct udsp_fax_detector *detector,
			       const int16_t *samples)
{
	double energy = 0.0;
	unsigned i;

	for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
		double in = samples[i];
		double out = HOLD_HIGHPASS_ALPHA * (detector->hold_previous_out +
						    in - detector->hold_previous_in);

		detector->hold_previous_in = in;
		detector->hold_previous_out = out;
		energy += out * out;
	}
	detector->hold_energy = energy;
}

static void answer_metrics_reset(struct udsp_fax_detector *detector)
{
	detector->ans_frames = 0;
	detector->ans_misses = 0;
	detector->ans_previous_valid = false;
	detector->reversal_count = 0;
	detector->reversal_valid = false;
	detector->last_reversal_frame = 0;
	detector->am_sum = 0.0;
	detector->am_cos = 0.0;
	detector->am_sin = 0.0;
	detector->am_basis_cos = 0.0;
	detector->am_basis_sin = 0.0;
}

static void answer_phase_update(struct udsp_fax_detector *detector,
				const int16_t *samples, double energy)
{
	double in_phase = 0.0;
	double quadrature = 0.0;
	double magnitude;
	unsigned i;

	for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
		in_phase += samples[i] * cos(detector->ans_phase);
		quadrature += samples[i] * sin(detector->ans_phase);
		detector->ans_phase += 2.0 * M_PI * 2100.0 / UDSP_SAMPLE_RATE;
		if (detector->ans_phase >= 2.0 * M_PI)
			detector->ans_phase -= 2.0 * M_PI;
	}
	magnitude = hypot(in_phase, quadrature);
	if (energy < TONE_MIN_ENERGY || magnitude < 1000.0)
		return;
	if (detector->ans_previous_valid) {
		double dot = in_phase * detector->ans_previous_i +
			quadrature * detector->ans_previous_q;
		double previous = hypot(detector->ans_previous_i,
					detector->ans_previous_q);

		if (dot < -0.5 * magnitude * previous) {
			uint32_t interval = detector->ans_frames -
				detector->last_reversal_frame;

			if (detector->reversal_count && interval >= REVERSAL_MIN_FRAMES &&
			    interval <= REVERSAL_MAX_FRAMES)
				detector->reversal_valid = true;
			detector->reversal_count = 1;
			detector->last_reversal_frame = detector->ans_frames;
		}
	}
	detector->ans_previous_i = in_phase;
	detector->ans_previous_q = quadrature;
	detector->ans_previous_valid = true;
}

static void answer_am_update(struct udsp_fax_detector *detector, double energy)
{
	double amplitude = sqrt(2.0 * energy / UDSP_FRAME_SAMPLES);
	double angle = 2.0 * M_PI * 15.0 * detector->ans_frames * FRAME_MS / 1000.0;
	double cosine = cos(angle);
	double sine = sin(angle);

	detector->am_sum += amplitude;
	detector->am_cos += amplitude * cosine;
	detector->am_sin += amplitude * sine;
	detector->am_basis_cos += cosine;
	detector->am_basis_sin += sine;
}

static double answer_am_ratio_get(const struct udsp_fax_detector *detector)
{
	double mean;
	double cosine;
	double sine;

	if (!detector->ans_frames || detector->am_sum <= 0.0)
		return 0.0;
	mean = detector->am_sum / detector->ans_frames;
	cosine = detector->am_cos - mean * detector->am_basis_cos;
	sine = detector->am_sin - mean * detector->am_basis_sin;
	return hypot(cosine, sine) / detector->am_sum;
}

static void answer_update(struct udsp_fax_detector *detector,
			  const int16_t *samples, double energy, bool answer_tone)
{
	if (!answer_tone) {
		if (detector->ans_frames && detector->ans_misses++ == 0)
			detector->ans_frames++;
		return;
	}
	if (!detector->ans_frames)
		answer_metrics_reset(detector);
	detector->ans_frames++;
	detector->ans_misses = 0;
	answer_phase_update(detector, samples, energy);
	answer_am_update(detector, energy);
}

static uint16_t answer_tone_get(const struct udsp_fax_detector *detector)
{
	if (detector->ans_frames < ANSWER_MIN_FRAMES)
		return UDSP_TONE_DET_OTHER;
	if (detector->reversal_valid)
		return UDSP_TONE_DET_ANSBAR;
	if (detector->reversal_count && detector->ans_frames -
	    detector->last_reversal_frame <= REVERSAL_MAX_FRAMES)
		return UDSP_TONE_DET_OTHER;
	if (answer_am_ratio_get(detector) >= ANSWER_AM_MIN_RATIO)
		return UDSP_TONE_DET_ANSAM;
	return UDSP_TONE_DET_CED;
}

static size_t active_start(struct udsp_fax_detector *detector,
			   struct udsp_fax_result *results, size_t count,
			   size_t capacity, uint16_t tone, uint32_t frames)
{
	detector->active_tone = tone;
	detector->active_frames = frames;
	detector->active_misses = 0;
	if (tone == UDSP_TONE_DET_MODEM)
		detector->active_modem_anchor_bin = detector->modem_bin;
	return result_add(detector, results, count, capacity, tone, true, frames);
}

static size_t active_end(struct udsp_fax_detector *detector,
			 struct udsp_fax_result *results, size_t count,
			 size_t capacity)
{
	if (!detector->active_tone)
		return count;
	count = result_add(detector, results, count, capacity,
			   detector->active_tone, false, detector->active_frames);
	detector->active_tone = UDSP_TONE_DET_OTHER;
	detector->active_frames = 0;
	detector->active_misses = 0;
	detector->active_modem_anchor_bin = 0;
	return count;
}

static size_t answer_report(struct udsp_fax_detector *detector,
			    struct udsp_fax_result *results, size_t count,
			    size_t capacity)
{
	uint16_t tone = answer_tone_get(detector);

	if (tone == UDSP_TONE_DET_OTHER)
		return count;
	if (!detector->active_tone)
		return active_start(detector, results, count, capacity, tone,
				    detector->ans_frames);
	if (detector->active_tone == tone)
		return count;
	if (detector->active_tone == UDSP_TONE_DET_ANSAM &&
	    tone == UDSP_TONE_DET_CED)
		return count;
	if (detector->active_tone != UDSP_TONE_DET_CED &&
	    detector->active_tone != UDSP_TONE_DET_ANSAM)
		return count;
	count = active_end(detector, results, count, capacity);
	return active_start(detector, results, count, capacity, tone,
			    detector->ans_frames);
}

static size_t cng_update(struct udsp_fax_detector *detector, bool cng_tone,
			 struct udsp_fax_result *results, size_t count,
			 size_t capacity)
{
	uint32_t duration;

	if (detector->cng_rearm_required) {
		if (cng_tone) {
			detector->cng_gap_frames = 0;
			return count;
		}
		if (++detector->cng_gap_frames < CNG_REARM_FRAMES)
			return count;
		detector->cng_gap_frames = 0;
		detector->cng_rearm_required = false;
		return count;
	}
	if (cng_tone) {
		detector->cng_frames++;
		detector->cng_good_frames++;
		detector->cng_misses = 0;
		return count;
	}
	if (!detector->cng_frames)
		return count;
	if (detector->cng_misses++ == 0) {
		detector->cng_frames++;
		return count;
	}
	duration = detector->cng_frames;
	if (detector->cng_good_frames >= CNG_MIN_FRAMES &&
	    detector->cng_good_frames <= CNG_MAX_FRAMES) {
		count = result_add(detector, results, count, capacity,
				   UDSP_TONE_DET_CNG, true, duration);
		count = result_add(detector, results, count, capacity,
				   UDSP_TONE_DET_CNG, false, duration);
		detector->cng_rearm_required = true;
	}
	detector->cng_frames = 0;
	detector->cng_good_frames = 0;
	detector->cng_misses = 0;
	return count;
}

static bool v21_bit_add(struct udsp_v21_track *track)
{
	double mark_power = track->mark_i * track->mark_i +
		track->mark_q * track->mark_q;
	double space_power = track->space_i * track->space_i +
		track->space_q * track->space_q;
	double high = mark_power > space_power ? mark_power : space_power;
	double low = mark_power > space_power ? space_power : mark_power;
	double capture_power = mark_power + space_power;
	bool valid = track->energy >= V21_MIN_ENERGY && high > low * 1.5 &&
		capture_power >= V21_CAPTURE_RATIO * track->energy;
	bool bit = mark_power > space_power;

	track->mark_i = 0.0;
	track->mark_q = 0.0;
	track->space_i = 0.0;
	track->space_q = 0.0;
	track->energy = 0.0;
	track->samples = 0;
	if (!valid) {
		track->flags = 0;
		track->bits_since_flag++;
		return false;
	}
	track->shift = (uint8_t)((track->shift >> 1) | (bit ? 0x80 : 0));
	track->bits_since_flag++;
	if (track->shift != 0x7e)
		return false;
	if (track->flags && track->bits_since_flag != 8U)
		track->flags = 0;
	track->flags++;
	track->bits_since_flag = 0;
	return true;
}

static bool v21_track_process(struct udsp_v21_track *track, int16_t sample,
			      double mark_cos, double mark_sin,
			      double space_cos, double space_sin)
{
	if (track->delay) {
		track->delay--;
		return false;
	}
	track->mark_i += sample * mark_cos;
	track->mark_q += sample * mark_sin;
	track->space_i += sample * space_cos;
	track->space_q += sample * space_sin;
	track->energy += (double)sample * sample;
	track->samples++;
	track->baud_accumulator += 300U;
	if (track->baud_accumulator < UDSP_SAMPLE_RATE)
		return false;
	track->baud_accumulator -= UDSP_SAMPLE_RATE;
	return v21_bit_add(track);
}

static bool v21_process(struct udsp_fax_detector *detector,
			const int16_t *samples)
{
	bool flag = false;
	unsigned i;

	for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
		unsigned phase = (unsigned)(detector->sample_count + i) %
			V21_TABLE_SIZE;
		unsigned mark = phase * 33U % V21_TABLE_SIZE;
		unsigned space = phase * 37U % V21_TABLE_SIZE;
		double mark_sin = detector->v21_sine[mark];
		double mark_cos = detector->v21_sine[
			(mark + V21_TABLE_SIZE / 4U) % V21_TABLE_SIZE];
		double space_sin = detector->v21_sine[space];
		double space_cos = detector->v21_sine[
			(space + V21_TABLE_SIZE / 4U) % V21_TABLE_SIZE];
		unsigned track_index;

		for (track_index = 0; track_index < UDSP_V21_TRACKS;
		     track_index++)
			flag |= v21_track_process(&detector->v21[track_index],
				samples[i], mark_cos, mark_sin, space_cos,
				space_sin);
	}
	return flag;
}

static unsigned v21_flags_get(const struct udsp_fax_detector *detector)
{
	unsigned maximum = 0;
	unsigned i;

	for (i = 0; i < UDSP_V21_TRACKS; i++) {
		if (detector->v21[i].flags > maximum)
			maximum = detector->v21[i].flags;
	}
	return maximum;
}

static bool modem_peak_get(const struct udsp_fax_detector *detector,
			   double energy, unsigned *bin)
{
	double maximum = 0.0;
	unsigned maximum_bin = 0;
	unsigned i;

	for (i = 0; i < detector->modem_low_bank.count; i++) {
		double power = udsp_goertzel_power(&detector->modem_low_bank, i);

		if (power > maximum) {
			maximum = power;
			maximum_bin = i;
		}
	}
	for (i = 0; i < detector->modem_mid_bank.count; i++) {
		double power = udsp_goertzel_power(&detector->modem_mid_bank, i);

		if (power > maximum) {
			maximum = power;
			maximum_bin = 16U + i;
		}
	}
	for (i = 0; i < detector->modem_high_bank.count; i++) {
		double power = udsp_goertzel_power(&detector->modem_high_bank, i);

		if (power > maximum) {
			maximum = power;
			maximum_bin = 32U + i;
		}
	}
	if (!tone_dominant(maximum, energy, TONE_DOMINANCE_RATIO))
		return false;
	*bin = maximum_bin;
	return true;
}

static bool modem_anchor_contains(unsigned anchor_bin, unsigned bin)
{
	return anchor_bin <= bin + MODEM_ANCHOR_BIN_RANGE &&
		bin <= anchor_bin + MODEM_ANCHOR_BIN_RANGE;
}

static void modem_update(struct udsp_fax_detector *detector, bool dominant,
			 unsigned bin, bool suppressed)
{
	if (!dominant || suppressed) {
		if (detector->modem_frames && detector->modem_misses++ == 0)
			detector->modem_frames++;
		else if (detector->modem_misses > 1U)
			detector->modem_frames = 0;
		return;
	}
	if (!detector->modem_frames) {
		detector->modem_candidate_anchor_bin = bin;
		detector->modem_bin = bin;
		detector->modem_frames = 1;
		detector->modem_misses = 0;
		return;
	}
	if (modem_anchor_contains(detector->modem_candidate_anchor_bin, bin)) {
		detector->modem_bin = bin;
		detector->modem_frames++;
		detector->modem_misses = 0;
		return;
	}
	detector->modem_candidate_anchor_bin = bin;
	detector->modem_bin = bin;
	detector->modem_frames = 1;
	detector->modem_misses = 0;
}

static size_t active_update(struct udsp_fax_detector *detector,
			    struct udsp_fax_result *results, size_t count,
			    size_t capacity, bool answer_tone,
			    bool v21_present, bool modem_present)
{
	bool present = false;

	if (!detector->active_tone)
		return count;
	detector->active_frames++;
	if (detector->active_tone == UDSP_TONE_DET_MODEM && answer_tone &&
	    answer_tone_get(detector) != UDSP_TONE_DET_OTHER) {
		count = active_end(detector, results, count, capacity);
		answer_metrics_reset(detector);
		return count;
	}
	if (detector->active_tone == UDSP_TONE_DET_CED ||
	    detector->active_tone == UDSP_TONE_DET_ANSAM ||
	    detector->active_tone == UDSP_TONE_DET_ANSBAR)
		present = answer_tone || detector->ans_misses <= 1U;
	else if (detector->active_tone == UDSP_TONE_DET_V21_PREAMBLE)
		present = v21_present;
	else if (detector->active_tone == UDSP_TONE_DET_MODEM)
		present = modem_present;
	if (present) {
		detector->active_misses = 0;
		return count;
	}
	if (detector->active_misses++ == 0)
		return count;
	return active_end(detector, results, count, capacity);
}

void udsp_fax_detector_init(struct udsp_fax_detector *detector,
			    enum udsp_tone_dir direction)
{
	double modem_low[UDSP_GOERTZEL_MAX];
	double modem_mid[UDSP_GOERTZEL_MAX];
	double modem_high[UDSP_GOERTZEL_MAX];
	unsigned i;

	memset(detector, 0, sizeof(*detector));
	detector->direction = (uint8_t)direction;
	udsp_goertzel_init(&detector->named_bank, named_frequencies,
			    sizeof(named_frequencies) / sizeof(named_frequencies[0]));
	for (i = 0; i < UDSP_GOERTZEL_MAX; i++) {
		modem_low[i] = 1200.0 + 32.0 * i;
		modem_mid[i] = 1200.0 + 32.0 * (i + 16U);
		modem_high[i] = 1200.0 + 32.0 * (i + 32U);
	}
	udsp_goertzel_init(&detector->modem_low_bank, modem_low,
			    UDSP_GOERTZEL_MAX);
	udsp_goertzel_init(&detector->modem_mid_bank, modem_mid,
			    UDSP_GOERTZEL_MAX);
	udsp_goertzel_init(&detector->modem_high_bank, modem_high,
			    UDSP_GOERTZEL_MAX);
	/*
	 * V.21 phases repeat in 160 samples: 1650/8000=33/160 and
	 * 1850/8000=37/160.
	 */
	for (i = 0; i < V21_TABLE_SIZE; i++)
		detector->v21_sine[i] = sin(2.0 * M_PI * i /
			V21_TABLE_SIZE);
	for (i = 0; i < UDSP_V21_TRACKS; i++)
		detector->v21[i].delay = i * 7U;
}

size_t udsp_fax_detector_process(struct udsp_fax_detector *detector,
				 const int16_t *samples,
				 struct udsp_fax_result *results,
				 size_t capacity)
{
	double energy;
	double cng_power;
	double answer_power;
	unsigned modem_bin = 0;
	unsigned flags;
	uint16_t answer_code;
	bool cng_tone;
	bool answer_tone;
	bool v21_flag;
	bool v21_present;
	bool modem_dominant;
	bool modem_present;
	size_t count = 0;

	if (!detector || !samples || (!results && capacity))
		return 0;
	banks_process(detector, samples, &energy);
	hold_energy_update(detector, samples);
	cng_power = udsp_goertzel_power(&detector->named_bank, 0);
	answer_power = udsp_goertzel_power(&detector->named_bank, 1);
	cng_tone = tone_dominant(cng_power, energy, CNG_DOMINANCE_RATIO);
	answer_tone = tone_dominant(answer_power, energy,
		TONE_DOMINANCE_RATIO);
	answer_update(detector, samples, energy, answer_tone);
	v21_flag = v21_process(detector, samples);
	if (v21_flag)
		detector->v21_last_flag_frame = detector->frame_count;
	flags = v21_flags_get(detector);
	v21_present = flags >= V21_FLAG_MIN ||
		(detector->active_tone == UDSP_TONE_DET_V21_PREAMBLE &&
		 detector->frame_count - detector->v21_last_flag_frame <=
		 V21_RELEASE_FRAMES);
	modem_dominant = modem_peak_get(detector, energy, &modem_bin);
	modem_update(detector, modem_dominant, modem_bin,
		     cng_tone || answer_tone || flags > 0);
	if (detector->active_tone == UDSP_TONE_DET_MODEM) {
		modem_present = modem_dominant &&
			modem_anchor_contains(
				detector->active_modem_anchor_bin, modem_bin);
	} else {
		modem_present = modem_dominant && modem_bin == detector->modem_bin;
	}
	count = active_update(detector, results, count, capacity, answer_tone,
			      v21_present, modem_present);
	count = cng_update(detector, cng_tone, results, count, capacity);
	answer_code = answer_tone_get(detector);
	if (!detector->active_tone && answer_tone &&
	    answer_code != UDSP_TONE_DET_OTHER)
		count = answer_report(detector, results, count, capacity);
	else if (answer_tone && (detector->active_tone == UDSP_TONE_DET_CED ||
		 detector->active_tone == UDSP_TONE_DET_ANSAM))
		count = answer_report(detector, results, count, capacity);
	if (!detector->active_tone && flags >= V21_FLAG_MIN)
		count = active_start(detector, results, count, capacity,
				     UDSP_TONE_DET_V21_PREAMBLE,
				     flags * 8U / 3U);
	if (!detector->active_tone && detector->modem_frames >= MODEM_MIN_FRAMES) {
		count = active_start(detector, results, count, capacity,
				     UDSP_TONE_DET_MODEM, detector->modem_frames);
	}
	if (detector->ans_frames && detector->ans_misses > 1U &&
	    detector->active_tone != UDSP_TONE_DET_CED &&
	    detector->active_tone != UDSP_TONE_DET_ANSAM &&
	    detector->active_tone != UDSP_TONE_DET_ANSBAR)
		answer_metrics_reset(detector);
	detector->frame_count++;
	detector->sample_count += UDSP_FRAME_SAMPLES;
	return count;
}

size_t udsp_fax_detector_flush(struct udsp_fax_detector *detector,
			       struct udsp_fax_result *results,
			       size_t capacity)
{
	size_t count = 0;

	if (!detector || (!results && capacity))
		return 0;
	count = active_end(detector, results, count, capacity);
	detector->cng_frames = 0;
	detector->cng_good_frames = 0;
	answer_metrics_reset(detector);
	return count;
}
