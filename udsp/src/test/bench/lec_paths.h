// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Echo paths, levels and the fixed-seed noise source shared by the LEC unit
 * suite (test/test_lec.c) and the G.168-derived bench (test/bench/bench_lec.c).
 * Both need the same hybrids to mean the same thing, so they read them from
 * one file.
 *
 * Everything here is test scaffolding: it is the oracle side of the suite and
 * shares no code with dsp/lec.c, so a defect in the canceller cannot hide in
 * a matching defect here.
 */
#ifndef UDSP_TEST_LEC_PATHS_H
#define UDSP_TEST_LEC_PATHS_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "../../dsp/dsp.h"
#include "../level.h"

/*
 * 80 ms at 16 kHz: the longest response any case builds is the 60 ms flat
 * hybrid of the tail-bound case (961 taps at 16 kHz), and the wideband tail
 * itself caps at 64 ms (LEC_TAIL_MS_WB_MAX), so this covers both with margin.
 */
#define LEC_PATH_TAPS_MAX	1280u

/*
 * A path is built at one line rate and runs one frame of that rate per call,
 * so the shapes below state their geometry in milliseconds and sample it at
 * the rate they are given. Every constant divides exactly at 8 kHz, so the
 * narrowband shapes sample with no rounding.
 *
 * The drive history is LEC_PATH_TAPS_MAX long whatever the response is, so it
 * does not depend on the shape that happens to be reading it. A path change
 * then carries a history long enough for the longest destination, and the
 * convolution reads the same ring whatever the response.
 */
struct echo_path {
	unsigned frame;
	unsigned taps;
	unsigned pos;
	double cubic;
	float h[LEC_PATH_TAPS_MAX];
	float hist[2u * LEC_PATH_TAPS_MAX];
};

static inline unsigned path_samples_get(double ms, unsigned rate)
{
	return (unsigned)lrint(ms * (double)rate / 1000.0);
}

/*
 * The dispersive hybrid oscillates at 4 kHz, the narrowband band edge, so one
 * sample is a half cycle at 8 kHz and a quarter cycle at 16 kHz: the exact
 * cosine values below keep the taps free of any libm rounding at both rates,
 * where cos(pi * k) would only be exactly +-1 by the grace of the library.
 */
static inline double path_edge_cos(unsigned k, unsigned rate)
{
	static const double quarter[4] = { 1.0, 0.0, -1.0, 0.0 };

	return quarter[(k * (UDSP_SAMPLE_RATE_WB / rate)) % 4u];
}

/* The frame is the line frame of the rate the path is built at, because the
 * canceller the path feeds runs exactly one such frame per call. */
static inline void path_geometry_set(struct echo_path *path, unsigned rate)
{
	memset(path, 0, sizeof(*path));
	path->frame = rate / 100u;
}

static inline uint32_t rng_next(uint32_t *state)
{
	uint32_t value = *state;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	*state = value;
	return value;
}

static inline int16_t sample_clamp(double value)
{
	if (value > 32767.0)
		return 32767;
	if (value < -32768.0)
		return -32768;
	return (int16_t)lrint(value);
}

/* phase counts the samples the tone has written, so two calls with equal
 * phases write two tones that start together. */
static inline void tone_fill(int16_t *dst, unsigned count, double level_dbm0,
			     double hz, unsigned long *phase, unsigned rate)
{
	double amplitude = dbm0_rms_get(level_dbm0) * sqrt(2.0);
	unsigned i;

	for (i = 0; i < count; i++)
		dst[i] = sample_clamp(amplitude *
				      sin(2.0 * M_PI * hz *
					  (double)(*phase + i) /
					  rate));
	*phase += count;
}

static inline void noise_fill(int16_t *dst, unsigned count, double level_dbm0,
			      uint32_t *state)
{
	double amplitude = dbm0_rms_get(level_dbm0) * sqrt(3.0);
	unsigned i;

	for (i = 0; i < count; i++) {
		double unit = rng_next(state) / 4294967295.0 * 2.0 - 1.0;

		dst[i] = sample_clamp(unit * amplitude);
	}
}

/*
 * Scales the response to a stated echo return loss. For a white reference the
 * echo power is the reference power times the sum of the squared taps, so the
 * sum is what the ERL fixes. Called on a live path it changes the hybrid
 * without touching its history, which is how a path step is staged.
 */
static inline void path_erl_set(struct echo_path *path, double erl_db)
{
	double energy = 0.0;
	double scale;
	unsigned k;

	for (k = 0; k < path->taps; k++)
		energy += (double)path->h[k] * path->h[k];
	scale = sqrt(pow(10.0, -erl_db / 10.0) / energy);
	for (k = 0; k < path->taps; k++)
		path->h[k] = (float)(path->h[k] * scale);
}

/*
 * Moves the far-end history of one hybrid into the hybrid that replaces it, so
 * a path change is only a change of response. Without it the new path starts
 * with an empty delay line and returns no echo at all until its own length has
 * refilled, which is a dropout followed by a change and reads as part of the
 * reconvergence time.
 *
 * src->hist[src->pos + k] is the drive sample delayed by k, so the carry is a
 * straight walk of the shared delays. The whole ring moves, not the source's
 * response length: a destination that reaches further back than the source's
 * own taps would otherwise read zeros there, which is a cold tail inside the
 * new response and not a path change (S7 switches a 25 ms hybrid for a 40 ms
 * one, so a carry of the source's length leaves 15 ms of it cold). The
 * destination ring is written at position 0 and the doubled half is kept in
 * step, the invariant path_run relies on.
 */
static inline void path_history_carry(struct echo_path *dst,
				      const struct echo_path *src)
{
	unsigned k;

	dst->pos = 0;
	for (k = 0; k < LEC_PATH_TAPS_MAX; k++) {
		float value = src->hist[src->pos + k];

		dst->hist[k] = value;
		dst->hist[k + LEC_PATH_TAPS_MAX] = value;
	}
}

static inline void path_flat_init(struct echo_path *path, unsigned rate,
				  unsigned delay_ms, double erl_db)
{
	unsigned delay = path_samples_get(delay_ms, rate);

	path_geometry_set(path, rate);
	path->taps = delay + 1u;
	path->h[delay] = 1.0f;
	path_erl_set(path, erl_db);
}

/* A 3 ms decaying oscillation at the 4 kHz band edge, 0.75 ms of decay. */
static inline void path_dispersive_init(struct echo_path *path, unsigned rate,
					double erl_db)
{
	double per_ms = (double)rate / 1000.0;
	unsigned k;

	path_geometry_set(path, rate);
	path->taps = path_samples_get(3.0, rate);
	for (k = 0; k < path->taps; k++)
		path->h[k] = (float)(exp(-(double)k / (0.75 * per_ms)) *
				     path_edge_cos(k, rate));
	path_erl_set(path, erl_db);
}

/*
 * A composite hybrid derived from G.168, not the Annex D coefficient tables
 * (the recommendation text is not in this tree): a flat bulk section, a
 * dispersive main reflection from 4 to 12 ms (891 Hz, 1.75 ms of decay) and a
 * second weaker reflection from 20 to 25 ms (637 Hz, 1.25 ms of decay), which
 * is the shape the Annex D models have.
 */
static inline void path_composite_init(struct echo_path *path, unsigned rate,
				       double erl_db)
{
	double per_ms = (double)rate / 1000.0;
	unsigned main_from = path_samples_get(4.0, rate);
	unsigned main_to = path_samples_get(12.0, rate);
	unsigned late_from = path_samples_get(20.0, rate);
	unsigned k;

	path_geometry_set(path, rate);
	path->taps = path_samples_get(25.0, rate);
	for (k = main_from; k < main_to; k++)
		path->h[k] = (float)(exp(-(double)(k - main_from) /
					 (1.75 * per_ms)) *
				     sin(5.6 / per_ms * (double)(k -
								 main_from)));
	for (k = late_from; k < path->taps; k++)
		path->h[k] = (float)(0.35 * exp(-(double)(k - late_from) /
						(1.25 * per_ms)) *
				     sin(4.0 / per_ms * (double)(k -
								 late_from)));
	path_erl_set(path, erl_db);
}

/* A hybrid whose whole response sits at 28 ms, past a 16 or 24 ms tail:
 * 509 Hz with 2.5 ms of decay, running to 40 ms. */
static inline void path_late_init(struct echo_path *path, unsigned rate,
				  double erl_db)
{
	double per_ms = (double)rate / 1000.0;
	unsigned from = path_samples_get(28.0, rate);
	unsigned k;

	path_geometry_set(path, rate);
	path->taps = path_samples_get(40.0, rate);
	for (k = from; k < path->taps; k++)
		path->h[k] = (float)(exp(-(double)(k - from) / (2.5 * per_ms)) *
				     sin(3.2 / per_ms * (double)(k - from)));
	path_erl_set(path, erl_db);
}

/*
 * Wideband only: a 6 kHz resonance from 2 to 10 ms with 1.5 ms of decay, the
 * upper-octave hybrid the two-band fallback decision turns on (a hybrid
 * response crossing 4 kHz loses coherence at the band split). 6 kHz has no
 * narrowband form, so a request at 8 kHz builds nothing: an empty path faults
 * at once in path_run under the sanitizers instead of quietly standing in
 * with a plausible low-band hybrid.
 */
static inline void path_upper_init(struct echo_path *path, unsigned rate,
				   double erl_db)
{
	double per_ms = (double)rate / 1000.0;
	unsigned from = path_samples_get(2.0, rate);
	unsigned k;

	path_geometry_set(path, rate);
	if (rate != UDSP_SAMPLE_RATE_WB)
		return;
	path->taps = path_samples_get(10.0, rate);
	for (k = from; k < path->taps; k++)
		path->h[k] = (float)(exp(-(double)(k - from) / (1.5 * per_ms)) *
				     sin(2.0 * M_PI * 6.0 * (double)(k - from) /
					 per_ms));
	path_erl_set(path, erl_db);
}

/*
 * The weak hybrid of the field lines: a balance network that matches the loop
 * at the low end and mismatches with frequency. The wall-clock-aligned traces
 * of real calls from a cell phone to an AN7581 line read a median raw return
 * loss of 30.4 dB with a tenth percentile of 26.0 dB. Those two numbers differ
 * only because the loss depends on the frequency of the frame: a flat hybrid
 * returns one number whatever the source does.
 *
 * Two sections. The transformer leak sits at 0.5 ms and is a first
 * difference, so the residual rises 6 dB per octave; the balance mismatch
 * runs from 3 to 15 ms, resonates at 2400 Hz and decays over 3 ms. erl_db
 * fixes the white-noise return loss, the quantity path_erl_set normalizes,
 * and a colored frame sees more or less of it.
 *
 * The two sections are equally weighted, which is what calibrates the model
 * against the field: at erl_db 28 the loss this shape returns to the word
 * source, measured in 100 ms blocks the way the traces were measured, reads
 * p10 25.9 and p50 30.6 dB against the field's p10 26.0 and p50 30.4. A
 * weaker resonance narrows that spread and stops modeling the line.
 *
 * 2400 Hz is inside the narrowband band, so the shape exists at both line
 * rates, unlike the upper-octave hybrid above.
 */
static inline void path_weak_init(struct echo_path *path, unsigned rate,
				  double erl_db)
{
	double per_ms = (double)rate / 1000.0;
	unsigned early = path_samples_get(0.5, rate);
	unsigned from = path_samples_get(3.0, rate);
	unsigned k;

	path_geometry_set(path, rate);
	path->taps = path_samples_get(15.0, rate);
	path->h[early] = 1.0f;
	path->h[early + 1u] = -1.0f;
	for (k = from; k < path->taps; k++)
		path->h[k] += (float)(exp(-(double)(k - from) /
					  (3.0 * per_ms)) *
				      sin(2.0 * M_PI * 2400.0 *
					  (double)(k - from) / rate));
	path_erl_set(path, erl_db);
}

/*
 * Far-end words: speech-shaped bursts that stop abruptly, with a pause of
 * 300 ms or more after each one. The field echo traces show the far caller
 * hears the last syllable of their own word, so the 100 ms after a word
 * offset is the window a residual has to be measured in. Neither bench source
 * has those properties: the composite source pauses for 150 ms inside a
 * 500 ms cycle and never changes its spectrum, and white noise has no offsets
 * at all.
 *
 * One word is a syllabic raised-cosine envelope at 3.5 Hz over two-pole
 * colored noise, and the color is drawn per word. A hybrid whose return
 * loss depends on frequency then returns a different loss for each word,
 * which is the spread of the reference traces; the level of a word is the
 * envelope whatever its color, because the frame is rescaled after the
 * color filter runs.
 *
 * offset is true on the last frame of a word, so the caller measures the
 * frames that follow it.
 */
/*
 * The unvoiced onset of a word, in 10 ms frames and in dB under the voiced
 * body. A fricative is quieter than the vowel that follows it, and it is the
 * first thing on the line after the pause, so the reference window maximum
 * the detector reads is the fricative's own frame: what the detector then
 * compares is the hybrid's return loss at the fricative's frequencies.
 */
#define WORD_ONSET_FRAMES	4u
#define WORD_ONSET_DB		8.0

struct word_source {
	uint32_t state;
	unsigned rate;
	unsigned pos;
	unsigned word;
	bool talking;
	bool offset;
	double level_dbm0;
	double m1;
	double m2;
};

static inline void word_source_init(struct word_source *w, unsigned rate,
				    double level_dbm0, uint32_t seed)
{
	memset(w, 0, sizeof(*w));
	w->state = seed ? seed : 1u;
	w->rate = rate;
	w->level_dbm0 = level_dbm0;
	w->talking = true;
}

static inline void word_source_restart(struct word_source *w)
{
	w->pos = 0;
	w->talking = true;
	w->offset = false;
}

static inline void word_fill(struct word_source *w, int16_t *dst,
			     unsigned frame)
{
	/* Word and pause lengths in 10 ms frames. The shortest pause is
	 * 300 ms, six times the 48 ms tail, so the echo of one word has ended
	 * long before the next one starts. */
	static const unsigned length[4] = { 18u, 25u, 32u, 21u };
	static const unsigned pause[4] = { 30u, 55u, 38u, 45u };
	static const double colour[4][2] = {
		{ 1.2, -0.6 }, { 1.7, -0.9 }, { 0.5, 0.2 }, { -0.4, -0.3 },
	};
	/* The unvoiced onset: a two-pole resonance near the top of the band,
	 * which is where a fricative puts its energy and where a balance
	 * network matched for the voiceband leaks. */
	static const double onset[2] = { -1.1, -0.5 };
	bool unvoiced = w->pos < WORD_ONSET_FRAMES;
	const double *tap = unvoiced ? onset : colour[w->word % 4u];
	unsigned index = w->word % 4u;
	double energy = 0.0;
	double target;
	double scale;
	unsigned k;

	w->offset = false;
	if (!w->talking) {
		memset(dst, 0, frame * sizeof(*dst));
		if (++w->pos < pause[index])
			return;
		w->pos = 0;
		w->word++;
		w->talking = true;
		return;
	}
	target = dbm0_rms_get(w->level_dbm0 +
			      7.0 * cos(2.0 * M_PI * 3.5 * (double)w->pos *
					(double)frame / (double)w->rate) -
			      (unvoiced ? WORD_ONSET_DB : 0.0));
	noise_fill(dst, frame, w->level_dbm0, &w->state);
	for (k = 0; k < frame; k++) {
		double y = 0.3 * dst[k] + tap[0] * w->m1 + tap[1] * w->m2;

		w->m2 = w->m1;
		w->m1 = y;
		dst[k] = sample_clamp(y);
		energy += y * y;
	}
	scale = energy > 0.0 ? target / sqrt(energy / frame) : 0.0;
	for (k = 0; k < frame; k++)
		dst[k] = sample_clamp((double)dst[k] * scale);
	if (++w->pos < length[index])
		return;
	/* The word stops here. No fade, and the next frame is digital
	 * silence: at that edge the far caller hears the echo of the word. */
	w->pos = 0;
	w->talking = false;
	w->offset = true;
}

/*
 * The non-linearity sits on the path input, where a real hybrid puts it: the
 * part of the distortion orthogonal to the reference is what no linear filter
 * can remove, and it is what the NLP has to hide.
 */
static inline void path_run(struct echo_path *path, const int16_t *ref,
			    int16_t *echo)
{
	unsigned i;
	unsigned k;

	for (i = 0; i < path->frame; i++) {
		double drive = ref[i];
		double acc = 0.0;

		if (path->cubic != 0.0)
			drive -= path->cubic * drive * drive * drive /
				 (32768.0 * 32768.0);
		path->pos = path->pos ? path->pos - 1u : LEC_PATH_TAPS_MAX - 1u;
		path->hist[path->pos] = (float)drive;
		path->hist[path->pos + LEC_PATH_TAPS_MAX] = (float)drive;
		for (k = 0; k < path->taps; k++)
			acc += (double)path->h[k] * path->hist[path->pos + k];
		echo[i] = sample_clamp(acc);
	}
}

#define REF_DELAY_MAX	LEC_PATH_TAPS_MAX

/*
 * Moves the reference of the canceller against the drive of the path. win[k]
 * of dsp/lec.c hist_push is the reference delayed by k, and hist[pos + k] of
 * path_run is the drive delayed by k, so equal inputs put tap 0 of the
 * canceller on tap 0 of the path. A positive offset delays the reference, and
 * tap 0 of the canceller then sits offset samples into the response. A
 * negative offset delays the drive, which adds margin before the response.
 * The delay line sits on the side that lags: a trimmed path would take the
 * early echo out of the microphone, and a late reference must leave it there.
 * Offset 0 copies the drive to both outputs, so a caller at 0 sees the frames
 * it saw without the delay line.
 */
struct ref_delay {
	int offset;
	unsigned pos;
	int16_t ring[REF_DELAY_MAX];
};

/* |offset| must be under REF_DELAY_MAX. */
static inline void ref_delay_init(struct ref_delay *d, int offset)
{
	memset(d, 0, sizeof(*d));
	d->offset = offset;
}

static inline void ref_delay_run(struct ref_delay *d, const int16_t *drive,
				 unsigned count, int16_t *path_in,
				 int16_t *ref)
{
	unsigned lag = (unsigned)(d->offset < 0 ? -d->offset : d->offset);
	unsigned i;

	for (i = 0; i < count; i++) {
		int16_t now = drive[i];
		int16_t late;

		d->ring[d->pos] = now;
		late = d->ring[(d->pos + REF_DELAY_MAX - lag) % REF_DELAY_MAX];
		d->pos = (d->pos + 1u) % REF_DELAY_MAX;
		path_in[i] = d->offset < 0 ? late : now;
		ref[i] = d->offset > 0 ? late : now;
	}
}

/*
 * A captured echo path: the least-squares fit of a canceller-off call trace
 * at the 8 kHz of the PCMU wire, with the levels of its capture. The tables
 * are in lec_paths_field.h. src/test/tools/path_fit.py fits them, and
 * src/test/tools/lec_paths_field_gen.py writes that header from them.
 */
#define FIELD_PATH_RATE		8000u
#define FIELD_PATH_TAPS		512u

_Static_assert(2u * FIELD_PATH_TAPS <= LEC_PATH_TAPS_MAX,
	       "a field path interpolated to 16 kHz must fit in an echo path");

/*
 * The levels are on the far-active frames of the capture, in wire units. The
 * ERLE figures are those of an ideal filter of 48 or 64 ms, with its first
 * tap 2 ms before the onset or at the onset. residual_rms is the rms of what
 * the 64 ms fit from 2 ms before the onset leaves: the near-end floor plus the
 * echo that the fit does not model. The bench injects it as the floor. name
 * has 13 characters at most.
 */
struct field_path {
	const char *name;
	const char *platform;
	const char *backend;
	const char *source;
	unsigned line_rate;
	unsigned onset_tap;
	double onset_ms;
	double uplink_db;
	double far_rms;
	double residual_rms;
	double erle_48_db;
	double erle_48_onset_db;
	double erle_64_db;
	double erle_64_onset_db;
	const float *h;
};

/*
 * An odd tap of the interpolation by two: a half-band lowpass at 4 kHz, the
 * ideal response sampled at the odd lags of 16 kHz under a Blackman window in
 * the cos(pi j / 64) form: a 129-point window, cut at its 127 inner lags. The
 * sign alternates with the lag. It is an explicit flip, the rule
 * of path_edge_cos(), and not the sign of sin(). The table is zero outside
 * its 512 taps.
 */
static inline float field_odd_tap_get(const float *h8, unsigned k)
{
	double sign = 1.0;
	double acc = 0.0;
	unsigned i;

	for (i = 0; i < 32u; i++, sign = -sign) {
		double j = 2.0 * i + 1.0;
		double w = 0.42 + 0.5 * cos(M_PI * j / 64.0) +
			   0.08 * cos(2.0 * M_PI * j / 64.0);
		unsigned after = k + 1u + i;
		double left = k >= i ? h8[k - i] : 0.0;
		double right = after < FIELD_PATH_TAPS ? h8[after] : 0.0;

		acc += sign / (M_PI * j) * w * (left + right);
	}
	return (float)acc;
}

/*
 * At 8 kHz the path is the table. At 16 kHz the path is the table
 * interpolated by two. The filter has zero phase, so tap 2k stays on tap k
 * and the onset keeps its place. The band above 4 kHz stays empty, because
 * the PCMU wire carried nothing there. The factor 0.5 on the even taps keeps
 * the response in the band, since the 16 kHz path has two taps per table tap.
 * Any other rate builds nothing, as path_upper_init does.
 *
 * The gain stays that of the table. The taps are the measured path in wire
 * units. path_erl_set fixes a loss for white noise, which the capture did not
 * measure, and at 16 kHz it would put back the 3 dB that the band limit takes
 * out.
 */
static inline void path_field_init(struct echo_path *path, unsigned rate,
				   const struct field_path *table)
{
	unsigned k;

	path_geometry_set(path, rate);
	if (rate == FIELD_PATH_RATE) {
		path->taps = FIELD_PATH_TAPS;
		memcpy(path->h, table->h, FIELD_PATH_TAPS * sizeof(*path->h));
		return;
	}
	if (rate != 2u * FIELD_PATH_RATE)
		return;
	path->taps = 2u * FIELD_PATH_TAPS;
	for (k = 0; k < FIELD_PATH_TAPS; k++) {
		path->h[2u * k] = 0.5f * table->h[k];
		path->h[2u * k + 1u] = field_odd_tap_get(table->h, k);
	}
}

#endif
