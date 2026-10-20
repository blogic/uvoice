// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Unit suite for the NLMS line echo canceller (dsp/lec.c).
 *
 * The test scaffolding in bench/lec_paths.h builds every echo path and every
 * level. The ERLE reference is therefore the injected echo signal, never the
 * estimate of the canceller. A case can pass only if the residual echo really
 * fell against a signal that the test knows independently.
 *
 * The cases assert 25 dB of ERLE after one second of adaptation, at the 48 ms
 * default tail. bench_lec reads about 39 dB there on the composite hybrid at
 * -10 dBm0 (its per-path gate table). The hybrid, not the algorithm, bounds a
 * 16 ms tail.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "udsp/udsp.h"

#include "../dsp/dsp.h"
#include "../dsp/lec.h"
#include "bench/lec_paths.h"
#include "bench/lec_paths_field.h"
#include "bench/lec_replay.h"
#include "util.h"

/* The engine tick is 10 ms at every line rate (core/clock.h), so a frame
 * count is a time and reads the same at 8 and 16 kHz. */
#define FRAMES_PER_SECOND	100u

/* Only the wideband leg adds the rate to a case name. An 8 kHz case name
 * stays the bare name, so a case that gains a 16 kHz leg keeps its name. */
static void case_rate(unsigned rate, const char *what)
{
	char name[160];

	snprintf(name, sizeof(name), "%s%s", what,
		 rate == UDSP_SAMPLE_RATE_WB ? " at 16 kHz" : "");
	CASE(name);
}

struct dt_result {
	double erle_pre;
	double erle_post;
	double passthrough_db;
	float coef_before;
	float coef_after;
	uint32_t detected_frames;
	uint32_t diverge_resets;
};

static double energy_dbm0_get(double energy, unsigned count)
{
	return rms_dbm0_get(sqrt(energy / count));
}

/*
 * The path carries the rate it was built at, and the canceller instance the
 * caller pairs with it runs the same frame, so one count serves both sides.
 * offset moves the reference against the drive (bench/lec_paths.h
 * ref_delay_run). half_db is a loss ahead of the reference alone, as the
 * engine applies it (udsp_ec_nlms_ref_loss_set): the drive of the path keeps
 * the line level.
 */
static double erle_offset_run(struct udsp_ec *ec, struct echo_path *path,
			      uint32_t *state, double far_dbm0, int offset,
			      unsigned half_db, unsigned frames,
			      unsigned measure)
{
	struct ref_delay delay;
	int16_t source[UDSP_FRAME_MAX];
	int16_t drive[UDSP_FRAME_MAX];
	int16_t ref[UDSP_FRAME_MAX];
	int16_t echo[UDSP_FRAME_MAX];
	int16_t mic[UDSP_FRAME_MAX];
	double echo_energy = 0.0;
	double residual_energy = 0.0;
	unsigned f;

	ref_delay_init(&delay, offset);
	for (f = 0; f < frames; f++) {
		unsigned i;

		noise_fill(source, path->frame, far_dbm0, state);
		ref_delay_run(&delay, source, path->frame, drive, ref);
		if (half_db)
			level_loss_apply(ref, ref, path->frame, half_db);
		path_run(path, drive, echo);
		memcpy(mic, echo, path->frame * sizeof(*mic));
		udsp_ec_nlms.run(ec, mic, ref);
		if (f + measure < frames)
			continue;
		for (i = 0; i < path->frame; i++) {
			echo_energy += (double)echo[i] * echo[i];
			residual_energy += (double)mic[i] * mic[i];
		}
	}
	if (residual_energy < 1e-6)
		return 200.0;
	return 10.0 * log10(echo_energy / residual_energy);
}

static double erle_run(struct udsp_ec *ec, struct echo_path *path,
		       uint32_t *state, double far_dbm0, unsigned frames,
		       unsigned measure)
{
	return erle_offset_run(ec, path, state, far_dbm0, 0, 0, frames,
			       measure);
}

static double level_run(struct udsp_ec *ec, struct echo_path *path,
			uint32_t *state, double far_dbm0, double near_dbm0,
			unsigned frames, unsigned measure)
{
	int16_t ref[UDSP_FRAME_MAX];
	int16_t echo[UDSP_FRAME_MAX];
	int16_t mic[UDSP_FRAME_MAX];
	uint32_t near_state = 0x2468acefu;
	double energy = 0.0;
	unsigned f;

	for (f = 0; f < frames; f++) {
		int16_t near[UDSP_FRAME_MAX];
		unsigned i;

		noise_fill(ref, path->frame, far_dbm0, state);
		path_run(path, ref, echo);
		noise_fill(near, path->frame, near_dbm0, &near_state);
		for (i = 0; i < path->frame; i++)
			mic[i] = sample_clamp((double)echo[i] + near[i]);
		udsp_ec_nlms.run(ec, mic, ref);
		if (f + measure < frames)
			continue;
		for (i = 0; i < path->frame; i++)
			energy += (double)mic[i] * mic[i];
	}
	return energy_dbm0_get(energy, measure * path->frame);
}

static void silence_run(struct udsp_ec *ec, double near_dbm0, unsigned frames)
{
	uint32_t near_state = 0x9e3779b9u;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	unsigned f;

	memset(ref, 0, sizeof(ref));
	for (f = 0; f < frames; f++) {
		noise_fill(mic, UDSP_FRAME_SAMPLES, near_dbm0, &near_state);
		udsp_ec_nlms.run(ec, mic, ref);
	}
}

static void tail_clamp_test(void)
{
	struct udsp_ec_nlms_info info;
	struct udsp_ec *ec;

	CASE("lec tail clamp below the supported range");
	ec = udsp_ec_nlms.create(15);
	CHECK(ec != NULL);
	if (!ec)
		return;
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 16);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.tail_ms, 16);
	CHECK_UINT(info.taps, 128);
	udsp_ec_nlms.destroy(ec);

	CASE("lec tail clamp above the supported range");
	ec = udsp_ec_nlms.create(129);
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 128);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.taps, 1024);
	udsp_ec_nlms.destroy(ec);

	CASE("lec tail request honoured inside the range");
	ec = udsp_ec_nlms.create(16);
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 16);
	udsp_ec_nlms.destroy(ec);
	ec = udsp_ec_nlms.create(48);
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 48);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.taps, 384);
	udsp_ec_nlms.destroy(ec);
	ec = udsp_ec_nlms.create(128);
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 128);
	udsp_ec_nlms.destroy(ec);

	CASE("lec tail zero request takes the default tail");
	ec = udsp_ec_nlms.create(0);
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 48);
	CHECK_UINT(udsp_ec_nlms_tail_get(NULL), 0);
	udsp_ec_nlms.destroy(ec);
	/* The wideband default is the wideband cap: real lines read more
	 * echo loss at 64 ms, and the tick budget is measured at that tail. */
	ec = udsp_ec_nlms.create_rate(0, UDSP_SAMPLE_RATE_WB);
	CHECK(ec != NULL);
	if (ec) {
		CHECK_UINT(udsp_ec_nlms_tail_get(ec), 64);
		udsp_ec_nlms_info_get(ec, &info);
		CHECK_UINT(info.taps, 1024);
		udsp_ec_nlms.destroy(ec);
	}
}

/*
 * The same three hybrids at both line rates. The wideband leg asserts the same
 * 25 dB in one second. At the same 48 ms tail, the 16 kHz instance carries
 * twice the taps of the 8 kHz one. The bar shows that the extra taps still
 * converge inside the second.
 */
static void convergence_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state;
	double erle;

	/*
	 * The NLP is off in every ERLE case: the oracle must see the linear
	 * residual, not the comfort noise that replaces it.
	 */
	case_rate(rate, "lec flat path converges");
	state = 0x13572468u;
	ec = udsp_ec_nlms.create_rate(48, rate);
	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, rate, 8, 12.0);
	erle = erle_run(ec, &path, &state, -10.0, FRAMES_PER_SECOND, 20);
	CHECK(erle >= 25.0);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.hold_frames, 0);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);

	/*
	 * The dispersive path runs 10 dB below the flat one: a step that is
	 * not normalized by the reference power cannot serve both levels.
	 */
	case_rate(rate, "lec dispersive path converges");
	state = 0x2468ace0u;
	ec = udsp_ec_nlms.create_rate(48, rate);
	udsp_ec_nlms.nlp(ec, false);
	path_dispersive_init(&path, rate, 12.0);
	erle = erle_run(ec, &path, &state, -20.0, FRAMES_PER_SECOND, 20);
	CHECK(erle >= 25.0);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.hold_frames, 0);
	udsp_ec_nlms.destroy(ec);

	case_rate(rate, "lec composite path converges");
	state = 0x0f1e2d3cu;
	ec = udsp_ec_nlms.create_rate(48, rate);
	udsp_ec_nlms.nlp(ec, false);
	path_composite_init(&path, rate, 12.0);
	erle = erle_run(ec, &path, &state, -10.0, FRAMES_PER_SECOND, 20);
	CHECK(erle >= 25.0);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.hold_frames, 0);
	udsp_ec_nlms.destroy(ec);
}

static void tail_bound_test(void)
{
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state = 0x5a5a1234u;
	double inside;
	double outside;

	ec = udsp_ec_nlms.create(48);
	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 40, 12.0);
	inside = erle_run(ec, &path, &state, -10.0, FRAMES_PER_SECOND, 20);
	udsp_ec_nlms.destroy(ec);

	ec = udsp_ec_nlms.create(48);
	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 60, 12.0);
	outside = erle_run(ec, &path, &state, -10.0, FRAMES_PER_SECOND, 20);
	udsp_ec_nlms.destroy(ec);

	/* The tail is counted after the bulk delay (ec.h): an echo further out
	 * than the tail is not the canceller's to remove. */
	CASE("lec tail bounds the cancelled delay");
	CHECK(inside >= 25.0);
	CHECK(outside < 3.0);
}

static void doubletalk_probe(unsigned rate, bool nlp, struct dt_result *result)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
	uint32_t state = 0x0badc0deu;
	uint32_t near_state = 0xfeedface;
	double product = 0.0;
	double near_energy = 0.0;
	unsigned f;

	memset(result, 0, sizeof(*result));
	udsp_ec_nlms.nlp(ec, nlp);
	path_flat_init(&path, rate, 8, 12.0);
	result->erle_pre = erle_run(ec, &path, &state, -10.0,
				    FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2,
				    20);
	udsp_ec_nlms_info_get(ec, &info);
	result->coef_before = info.coef_energy;
	for (f = 0; f < FRAMES_PER_SECOND / 2; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t near[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		unsigned i;

		noise_fill(ref, path.frame, -10.0, &state);
		path_run(&path, ref, echo);
		/* 3 dB above the echo the -12 dB hybrid returns. */
		noise_fill(near, path.frame, -19.0, &near_state);
		for (i = 0; i < path.frame; i++)
			mic[i] = sample_clamp((double)echo[i] + near[i]);
		udsp_ec_nlms.run(ec, mic, ref);
		udsp_ec_nlms_info_get(ec, &info);
		if (info.double_talk)
			result->detected_frames++;
		/* The detector needs the frame it fires in and the NLP one
		 * more to cross-fade out; measure the settled burst. */
		if (f < 5)
			continue;
		for (i = 0; i < path.frame; i++) {
			product += (double)mic[i] * near[i];
			near_energy += (double)near[i] * near[i];
		}
	}
	udsp_ec_nlms_info_get(ec, &info);
	result->coef_after = info.coef_energy;
	result->passthrough_db = -20.0 * log10(product / near_energy);
	result->erle_post = erle_run(ec, &path, &state, -10.0,
				     FRAMES_PER_SECOND / 2, 20);
	udsp_ec_nlms_info_get(ec, &info);
	result->diverge_resets = info.diverge_resets;
	udsp_ec_nlms.destroy(ec);
}

static void doubletalk_test(unsigned rate)
{
	struct dt_result linear;
	struct dt_result gated;

	doubletalk_probe(rate, false, &linear);
	doubletalk_probe(rate, true, &gated);

	/* The burst must leave the filter exactly where it found it: the frame
	 * the detector needs to see before it can fire is adapted on and then
	 * rolled back, so the whole burst is a no-op on the coefficients. */
	case_rate(rate, "lec double talk holds adaptation");
	CHECK(linear.erle_pre >= 25.0);
	CHECK(linear.detected_frames >= FRAMES_PER_SECOND / 4);
	CHECK(linear.erle_post >= 20.0);
	CHECK(linear.coef_after == linear.coef_before);
	CHECK(gated.coef_after == gated.coef_before);
	CHECK_UINT(linear.diverge_resets, 0);

	case_rate(rate, "lec double talk passes the near end");
	CHECK(gated.detected_frames >= FRAMES_PER_SECOND / 4);
	CHECK(gated.passthrough_db <= 3.0);
	CHECK(gated.passthrough_db >= -3.0);
}

static void freeze_test(void)
{
	struct udsp_ec_nlms_info before;
	struct udsp_ec_nlms_info after;
	struct echo_path old_path;
	struct echo_path new_path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x77778888u;
	double erle_old;
	double erle_held;
	double erle_new;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&old_path, UDSP_SAMPLE_RATE, 8, 12.0);
	path_dispersive_init(&new_path, UDSP_SAMPLE_RATE, 12.0);
	erle_old = erle_run(ec, &old_path, &state, -10.0,
			    FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20);
	udsp_ec_nlms_info_get(ec, &before);
	udsp_ec_nlms.freeze(ec, true);
	(void)erle_run(ec, &new_path, &state, -10.0, FRAMES_PER_SECOND, 1);
	udsp_ec_nlms_info_get(ec, &after);
	erle_held = erle_run(ec, &old_path, &state, -10.0,
			     FRAMES_PER_SECOND / 2, 20);

	CASE("lec freeze holds adaptation across a path change");
	CHECK(erle_old >= 25.0);
	CHECK(after.coef_energy == before.coef_energy);
	CHECK(after.hold_frames >= FRAMES_PER_SECOND);
	CHECK(erle_held >= erle_old - 3.0);
	CHECK_UINT(after.diverge_resets, 0);

	udsp_ec_nlms.freeze(ec, false);
	erle_new = erle_run(ec, &new_path, &state, -10.0,
			    FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20);
	CASE("lec freeze release reconverges on the new path");
	CHECK(erle_new >= 25.0);
	udsp_ec_nlms.destroy(ec);
}

static void nlp_test(void)
{
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state;
	double suppressed;
	double linear;
	double matched;

	state = 0x31415926u;
	ec = udsp_ec_nlms.create(48);
	path_composite_init(&path, UDSP_SAMPLE_RATE, 12.0);
	/* About -33 dB of residual non-linearity: with a purely linear path
	 * the two NLP modes are indistinguishable at this level. */
	path.cubic = 0.12;
	silence_run(ec, -200.0, 20);
	suppressed = level_run(ec, &path, &state, 0.0, -200.0,
			       2u * FRAMES_PER_SECOND, 20);
	udsp_ec_nlms.destroy(ec);

	state = 0x31415926u;
	ec = udsp_ec_nlms.create(48);
	udsp_ec_nlms.nlp(ec, false);
	path_composite_init(&path, UDSP_SAMPLE_RATE, 12.0);
	path.cubic = 0.12;
	silence_run(ec, -200.0, 20);
	linear = level_run(ec, &path, &state, 0.0, -200.0,
			   2u * FRAMES_PER_SECOND, 20);
	udsp_ec_nlms.destroy(ec);

	CASE("lec nlp suppresses the converged residual");
	CHECK(suppressed <= -60.0);

	CASE("lec nlp off passes the linear residual");
	CHECK(linear >= -55.0);
	CHECK(linear - suppressed >= 10.0);

	state = 0x27182818u;
	ec = udsp_ec_nlms.create(48);
	path_composite_init(&path, UDSP_SAMPLE_RATE, 12.0);
	path.cubic = 0.12;
	silence_run(ec, -50.0, 40);
	matched = level_run(ec, &path, &state, 0.0, -50.0,
			    2u * FRAMES_PER_SECOND, 20);
	udsp_ec_nlms.destroy(ec);

	CASE("lec nlp comfort noise matches the near-end floor");
	CHECK(fabs(matched + 50.0) <= 4.0);
}

static void saturation_test(void)
{
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	uint32_t state = 0x1a2b3c4du;
	unsigned i;
	unsigned rail = 0;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	(void)erle_run(ec, &path, &state, -10.0,
		       FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20);
	udsp_ec_nlms.freeze(ec, true);
	for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
		ref[i] = 32767;
	for (i = 0; i < 2u; i++) {
		unsigned n;

		for (n = 0; n < UDSP_FRAME_SAMPLES; n++)
			mic[n] = -32768;
		udsp_ec_nlms.run(ec, mic, ref);
	}
	for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
		if (mic[i] == -32768)
			rail++;
	}

	/* -32768 minus a positive estimate has no int16 form: it must clamp,
	 * not wrap. */
	CASE("lec output saturates");
	CHECK_UINT(rail, UDSP_FRAME_SAMPLES);
	udsp_ec_nlms.destroy(ec);
}

static void determinism_test(void)
{
	struct udsp_ec *first = udsp_ec_nlms.create(48);
	struct udsp_ec *second = udsp_ec_nlms.create(48);
	struct udsp_ec_nlms_info info_first;
	struct udsp_ec_nlms_info info_second;
	struct echo_path path_first;
	struct echo_path path_second;
	uint32_t state_first = 0x24681357u;
	uint32_t state_second = 0x24681357u;
	unsigned differences = 0;
	unsigned f;

	path_composite_init(&path_first, UDSP_SAMPLE_RATE, 12.0);
	path_composite_init(&path_second, UDSP_SAMPLE_RATE, 12.0);
	for (f = 0; f < FRAMES_PER_SECOND; f++) {
		int16_t ref_first[UDSP_FRAME_SAMPLES];
		int16_t ref_second[UDSP_FRAME_SAMPLES];
		int16_t mic_first[UDSP_FRAME_SAMPLES];
		int16_t mic_second[UDSP_FRAME_SAMPLES];

		noise_fill(ref_first, UDSP_FRAME_SAMPLES, -10.0, &state_first);
		noise_fill(ref_second, UDSP_FRAME_SAMPLES, -10.0,
			   &state_second);
		path_run(&path_first, ref_first, mic_first);
		path_run(&path_second, ref_second, mic_second);
		udsp_ec_nlms.run(first, mic_first, ref_first);
		udsp_ec_nlms.run(second, mic_second, ref_second);
		if (memcmp(mic_first, mic_second, sizeof(mic_first)))
			differences++;
	}
	udsp_ec_nlms_info_get(first, &info_first);
	udsp_ec_nlms_info_get(second, &info_second);

	CASE("lec run is deterministic");
	CHECK_UINT(differences, 0);
	CHECK(info_first.coef_energy == info_second.coef_energy);
	CHECK(info_first.erle_db == info_second.erle_db);
	udsp_ec_nlms.destroy(first);
	udsp_ec_nlms.destroy(second);
}

/*
 * A hybrid that gets louder leaves the echo estimate short. Once the detector
 * trusts the filter, it thresholds against that estimate, so the hold feeds
 * itself. Only the escape ends the hold.
 */
static void detector_latch_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x0c0ffee1u;
	double erle;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	(void)erle_run(ec, &path, &state, -10.0,
		       FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20);
	/* The same hybrid, 6 dB louder, with its history intact: a fresh path
	 * object would break the echo for one frame and collapse the tracker,
	 * which is the one thing that ends the hold by accident. */
	path_erl_set(&path, 6.0);
	erle = erle_run(ec, &path, &state, -10.0, 10u * FRAMES_PER_SECOND, 20);
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec detector releases a hybrid that got louder");
	CHECK(erle >= 25.0);
	CHECK(info.hold_frames >= 4u * FRAMES_PER_SECOND);
	CHECK(info.hold_frames <= 7u * FRAMES_PER_SECOND);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * A converged instance with the NLP on must not mute the caller the moment
 * the far end stops. With no reference, there is no echo to replace. The
 * detector also cannot flag near-end speech that no reference explains.
 */
static void nlp_near_only_test(void)
{
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x51ec0de5u;
	uint32_t near_state = 0xa1b2c3d4u;
	double product = 0.0;
	double near_energy = 0.0;
	unsigned f;

	path_composite_init(&path, UDSP_SAMPLE_RATE, 12.0);
	(void)erle_run(ec, &path, &state, -10.0,
		       FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20);
	for (f = 0; f < FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t near[UDSP_FRAME_SAMPLES];
		int16_t mic[UDSP_FRAME_SAMPLES];
		unsigned i;

		memset(ref, 0, sizeof(ref));
		noise_fill(near, UDSP_FRAME_SAMPLES, -20.0, &near_state);
		memcpy(mic, near, sizeof(mic));
		udsp_ec_nlms.run(ec, mic, ref);
		/* The tail window has to flush before the reference reads as
		 * silent, and the NLP takes one frame to fade out. */
		if (f < 20u)
			continue;
		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			product += (double)mic[i] * near[i];
			near_energy += (double)near[i] * near[i];
		}
	}

	CASE("lec nlp passes near speech with a silent far end");
	CHECK(-20.0 * log10(product / near_energy) <= 3.0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * A hybrid that steps to 3 dB ERL is louder than every expectation that the
 * detector holds, so no verdict releases the hold. Only the adaptation probe
 * at the end of the latch can prove that the line changed.
 */
static void path_step_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
	uint32_t state = 0x9a8b7c6du;
	double erle;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, rate, 8, 12.0);
	(void)erle_run(ec, &path, &state, -10.0,
		       FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20);
	path_erl_set(&path, 3.0);
	erle = erle_run(ec, &path, &state, -10.0, 8u * FRAMES_PER_SECOND, 20);
	udsp_ec_nlms_info_get(ec, &info);

	case_rate(rate,
		  "lec adaptation probe recovers a hybrid that got much louder");
	CHECK(erle >= 20.0);
	CHECK(info.probes >= 1u);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The latch escape reads five seconds of unbroken double talk as a hybrid that
 * its estimate does not explain. A talker who holds the line that long
 * therefore gets an adaptation probe run against their speech. The trial must
 * be thrown away, because what the filter learns from a near-end talker is not
 * the hybrid.
 */
static void sustained_doubletalk_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x0badc0deu;
	uint32_t near_state = 0xfeedfaceu;
	double product = 0.0;
	double near_energy = 0.0;
	double pre;
	double post;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	pre = erle_run(ec, &path, &state, -10.0,
		       FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20);
	for (f = 0; f < 12u * FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t echo[UDSP_FRAME_SAMPLES];
		int16_t near[UDSP_FRAME_SAMPLES];
		int16_t mic[UDSP_FRAME_SAMPLES];
		unsigned i;

		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
		path_run(&path, ref, echo);
		noise_fill(near, UDSP_FRAME_SAMPLES, -19.0, &near_state);
		for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
			mic[i] = sample_clamp((double)echo[i] + near[i]);
		udsp_ec_nlms.run(ec, mic, ref);
		if (f < 5u)
			continue;
		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			product += (double)mic[i] * near[i];
			near_energy += (double)near[i] * near[i];
		}
	}
	post = erle_run(ec, &path, &state, -10.0, FRAMES_PER_SECOND / 2, 20);
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec sustained double talk survives the adaptation probe");
	CHECK(info.probes >= 1u);
	CHECK(post >= pre - 6.0);
	CHECK(-20.0 * log10(product / near_energy) <= 3.0);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * Speech-shaped far end, as on a real line: syllabic raised-cosine envelope at
 * 3.5 Hz with a 14 dB swing, 2 s spurts with 0.5 s pauses, 40 percent
 * frame-granularity DTX inside a spurt (as PCMU sends it), and a spectral
 * color that cycles every two seconds. The white-noise cases miss every one
 * of these properties, and the bootstrap case needs them.
 */
static double syllabic_dbm0_get(unsigned frame, double mean_dbm0)
{
	double phase = 2.0 * M_PI * 3.5 * frame * UDSP_FRAME_SAMPLES /
		       UDSP_SAMPLE_RATE;

	return mean_dbm0 + 7.0 * cos(phase);
}

static void color_apply(int16_t *frame, unsigned index, double *m1, double *m2)
{
	double a = 0.0;
	double b = 0.0;
	unsigned k;

	switch ((index / (2u * FRAMES_PER_SECOND)) % 3u) {
	case 0: a = 1.2; b = -0.6; break;
	case 1: a = 1.7; b = -0.9; break;
	case 2: a = 0.5; b = 0.2; break;
	}
	for (k = 0; k < UDSP_FRAME_SAMPLES; k++) {
		double y = 0.3 * frame[k] + a * *m1 + b * *m2;

		*m2 = *m1;
		*m1 = y;
		frame[k] = sample_clamp(y);
	}
}

static void far_speech_fill(int16_t *ref, unsigned f, uint32_t *state,
			    double *m1, double *m2)
{
	bool talking = f % (5u * FRAMES_PER_SECOND / 2u) <
		       2u * FRAMES_PER_SECOND;
	bool gap;

	memset(ref, 0, UDSP_FRAME_SAMPLES * sizeof(*ref));
	if (!talking)
		return;
	noise_fill(ref, UDSP_FRAME_SAMPLES, syllabic_dbm0_get(f, -10.0), state);
	/* The DTX gap zeroes the frame after the color runs, so a gap frame is
	 * an exact zero on the wire while the color memory keeps running. */
	gap = rng_next(state) % 100u < 40u;
	color_apply(ref, f, m1, m2);
	if (gap)
		memset(ref, 0, UDSP_FRAME_SAMPLES * sizeof(*ref));
}

/* The fixed activity bar of dsp/lec.c (LEC_REF_ACTIVE_POWER), in the same
 * float arithmetic as the canceller, as a property of the signal. The bar
 * of the canceller equals it when the reference history holds a frame under
 * 396.8, and stands over it on a stationary far end. A case that needs the
 * view of the canceller reads reading.ref_bar, reading.background or
 * reading.window_run. */
static bool frame_loud(const int16_t *frame, unsigned count)
{
	return udsp_ec_nlms_frame_power_get(frame, count) >= 2500.0f;
}

/*
 * The bootstrap failure on a weak hybrid. Real hybrids can return 22 to
 * 24 dB of ERL, so near-end speech at -25 dBm0 sits far under the classic
 * quarter-power expectation. Pristine adaptation then absorbs every burst of
 * that speech, and the filter spends the call re-learning and never settles.
 * With the learned bound, the bursts read as double talk and roll back, and
 * the spurts between them are enough to converge. The burst schedule of the
 * near end ends with the run. A pristine tracker collapses inside a burst that
 * it cannot see, so the read at the end tells the two apart.
 */
static void bootstrap_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(0);
	uint32_t state = 0x13572468u;
	uint32_t near_state = 0x0badcafeu;
	double m1 = 0.0;
	double m2 = 0.0;
	double echo_energy = 0.0;
	double res_energy = 0.0;
	/* 58.5 s, so the run ends exactly with a near burst that overlaps a
	 * far spurt: a tracker blind to the burst collapses inside it, and the
	 * read at the end is the discriminating one. */
	unsigned total = 5850u;
	unsigned active = 0;
	unsigned burst_loud = 0;
	unsigned burst_flagged = 0;
	unsigned held_active;
	unsigned quiet_run = 1000u;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 20, 23.0);
	for (f = 0; f < total; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t echo[UDSP_FRAME_SAMPLES];
		int16_t mic[UDSP_FRAME_SAMPLES];
		bool burst = f % 150u >= 100u;
		bool loud;
		unsigned i;

		far_speech_fill(ref, f, &state, &m1, &m2);
		loud = frame_loud(ref, UDSP_FRAME_SAMPLES);
		if (loud)
			active++;
		path_run(&path, ref, echo);
		memcpy(mic, echo, sizeof(mic));
		/* 500 ms of near speech every 1.5 s, its own syllabic phase. */
		if (burst) {
			int16_t near[UDSP_FRAME_SAMPLES];

			noise_fill(near, UDSP_FRAME_SAMPLES,
				   syllabic_dbm0_get(f + 37u, -25.0),
				   &near_state);
			for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
				mic[i] = sample_clamp((double)mic[i] + near[i]);
		}
		udsp_ec_nlms.run(ec, mic, ref);
		udsp_ec_nlms_info_get(ec, &info);
		if (burst && loud) {
			burst_loud++;
			if (info.double_talk)
				burst_flagged++;
		}
		/* The oracle counts the echo the filter had to remove, on the
		 * frames the near end leaves clean (the probe's exclusion
		 * rule): a burst and its hangover would count near speech as
		 * residual. */
		quiet_run = burst ? 0 : quiet_run + 1u;
		if (f + 1000u < total || quiet_run <= 30u)
			continue;
		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			echo_energy += (double)echo[i] * echo[i];
			res_energy += (double)mic[i] * mic[i];
		}
	}
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec learned bound carries the bootstrap through near speech");
	/* The detector must see the near bursts, which sit under the classic
	 * quarter-power ceiling, and the filter they would have corrupted must
	 * genuinely cancel the echo. */
	CHECK(burst_flagged * 10u >= burst_loud * 6u);
	CHECK(res_energy > 0.0);
	CHECK(10.0 * log10(echo_energy / res_energy) >= 25.0);
	CHECK(info.erle_db >= 15.0f);
	/* Every inactive frame holds by definition; the fraction that means
	 * anything is the held share of the active frames. */
	held_active = info.hold_frames - (total - active);
	CHECK(held_active * 5u < active * 3u);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The gain estimator against reference DTX. Silent frames inside an active
 * tail window carry no echo. A tracker that still compares them against the
 * standing window maximum walks its minimum far under the hybrid. The
 * estimate must stay within a few dB of the true gain on a heavily gapped
 * reference.
 */
static void gain_bound_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x6a11b0b1u;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 2, 12.0);
	for (f = 0; f < 20u * FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t echo[UDSP_FRAME_SAMPLES];
		int16_t mic[UDSP_FRAME_SAMPLES];

		if (rng_next(&state) % 100u < 50u)
			memset(ref, 0, sizeof(ref));
		else
			noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
		path_run(&path, ref, echo);
		memcpy(mic, echo, sizeof(mic));
		udsp_ec_nlms.run(ec, mic, ref);
	}
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec gain estimator reads the hybrid through reference DTX");
	/* True power gain 10^-1.2 = 0.0631; the estimator may under-read by
	 * its minimum-statistics bias but must stay within 3 dB. */
	CHECK(info.echo_gain >= 0.0631f * 0.5f);
	CHECK(info.echo_gain <= 0.0631f * 2.0f);
	udsp_ec_nlms.destroy(ec);
}

/* One frame of the lockout-escape far signal: 300 ms spurts, 200 ms gaps,
 * every fifth pause 500 ms (the inter-sentence pause of real speech). */
struct dtx_far {
	uint32_t state;
	unsigned pos;
	unsigned spurt;
};

static void dtx_far_fill(struct dtx_far *g, int16_t *ref)
{
	unsigned gap = g->spurt == 4u ? 50u : 20u;

	if (g->pos < 30u)
		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &g->state);
	else
		memset(ref, 0, UDSP_FRAME_SAMPLES * sizeof(*ref));
	g->pos++;
	if (g->pos >= 30u + gap) {
		g->pos = 0;
		g->spurt = (g->spurt + 1u) % 5u;
	}
}

/*
 * The hot-hybrid lockout (SLIC digital loopback, 0 dB echo). The detector
 * reads the unit echo as double talk from the first frame, and the verdict
 * can never change. Only the latch escape ends it. DTX far speech carries
 * short gaps that the hangover bridges, and every few spurts a pause long
 * enough to clear it. A hard dt_run reset at each pause keeps the escape
 * from ever firing. The decayed run drops by four per clear frame of a pause,
 * and still reaches the latch.
 */
static void lockout_escape_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(0);
	struct dtx_far far = { .state = 0x0c0ffee1u };
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 0.0);
	for (f = 0; f < 30u * FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t echo[UDSP_FRAME_SAMPLES];
		int16_t mic[UDSP_FRAME_SAMPLES];

		dtx_far_fill(&far, ref);
		path_run(&path, ref, echo);
		memcpy(mic, echo, sizeof(mic));
		udsp_ec_nlms.run(ec, mic, ref);
	}
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec latch escape fires through far-end DTX");
	CHECK(info.probes >= 1u);
	CHECK(info.erle_db >= 15.0f);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

/* The microphone of one 8 kHz frame: the echo plus a near-end floor, or, when
 * floor_state is NULL, the echo alone and bit for bit. */
static void floor_add(int16_t *mic, const int16_t *echo, double floor_dbm0,
		      uint32_t *floor_state)
{
	int16_t hiss[UDSP_FRAME_SAMPLES];
	unsigned i;

	if (!floor_state) {
		memcpy(mic, echo, sizeof(hiss));
		return;
	}
	noise_fill(hiss, UDSP_FRAME_SAMPLES, floor_dbm0, floor_state);
	for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
		mic[i] = sample_clamp((double)echo[i] + hiss[i]);
}

/* Runs the escape shape until the first adaptation trial fires; the caller
 * takes over with the trial freshly started and its budget intact. */
static void latch_drive(struct udsp_ec *ec, struct echo_path *path,
			struct dtx_far *far, double floor_dbm0,
			uint32_t *floor_state)
{
	struct udsp_ec_nlms_info info;
	unsigned f;

	for (f = 0; f < 15u * FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t echo[UDSP_FRAME_SAMPLES];
		int16_t mic[UDSP_FRAME_SAMPLES];

		dtx_far_fill(far, ref);
		path_run(path, ref, echo);
		floor_add(mic, echo, floor_dbm0, floor_state);
		udsp_ec_nlms.run(ec, mic, ref);
		udsp_ec_nlms_info_get(ec, &info);
		if (info.probes)
			return;
	}
}

/*
 * The budget of the trial counts active frames only. A far-end pause right
 * after the escape must not consume the trial against silence. There the
 * filter cannot move, and the collapsed residual would fake the improvement
 * verdict.
 */
static void probe_pause_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(0);
	struct dtx_far far = { .state = 0x0c0ffee1u };
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 0.0);
	latch_drive(ec, &path, &far, 0.0, NULL);
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec probe trial waits out a far-end pause");
	CHECK_UINT(info.probes, 1);
	/* 1.2 s of silence is more than the whole 100-frame budget. */
	for (f = 0; f < 120u; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t echo[UDSP_FRAME_SAMPLES];
		int16_t mic[UDSP_FRAME_SAMPLES];

		memset(ref, 0, sizeof(ref));
		path_run(&path, ref, echo);
		memcpy(mic, echo, sizeof(mic));
		udsp_ec_nlms.run(ec, mic, ref);
	}
	for (f = 0; f < 4u * FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t echo[UDSP_FRAME_SAMPLES];
		int16_t mic[UDSP_FRAME_SAMPLES];

		dtx_far_fill(&far, ref);
		path_run(&path, ref, echo);
		memcpy(mic, echo, sizeof(mic));
		udsp_ec_nlms.run(ec, mic, ref);
	}
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.probes, 1);
	CHECK(info.erle_db >= 15.0f);
	udsp_ec_nlms.destroy(ec);
}

/*
 * A second latch trip while a trial runs must not restart the trial. The
 * snapshot would then hold half-adapted trial coefficients, which
 * probe_finish then cannot undo.
 */
static void probe_rearm_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(0);
	struct dtx_far far = { .state = 0x0c0ffee1u };
	uint32_t near_state = 0xfeedfaceu;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 0.0);
	latch_drive(ec, &path, &far, 0.0, NULL);
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec latch cannot re-arm inside a running trial");
	CHECK_UINT(info.probes, 1);
	/* Continuous near talk over a sparse far end: the double-talk run
	 * climbs a full latch period again while the trial, which spends
	 * active frames only, is still open. */
	for (f = 0; f < 6u * FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t echo[UDSP_FRAME_SAMPLES];
		int16_t near[UDSP_FRAME_SAMPLES];
		int16_t mic[UDSP_FRAME_SAMPLES];
		unsigned i;

		if (f % 10u == 0u)
			noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &far.state);
		else
			memset(ref, 0, sizeof(ref));
		path_run(&path, ref, echo);
		noise_fill(near, UDSP_FRAME_SAMPLES, -10.0, &near_state);
		for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
			mic[i] = sample_clamp((double)echo[i] + near[i]);
		udsp_ec_nlms.run(ec, mic, ref);
	}
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.probes, 1);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The combined-loss NLP gate. The far listener hears ERL plus ERLE, with the
 * hybrid loss taken from the conservative bound (echo_gain times headroom): a
 * 23 dB hybrid with a filter around 13 dB of ERLE already delivers the gate's
 * 33 dB, while the same filter on a 6 dB hybrid delivers about 20 and on a
 * 12 dB hybrid about 23, neither of which is enough to hand the line to the
 * comfort noise. The probe freezes all three instances mid-convergence, so the
 * tracker sits at the same ERLE by construction.
 *
 * half_db sets the reference loss of the instance and nothing else, so the
 * reference and the path stay those of a probe at 0.
 */
struct gate_probe {
	bool nlp_far;
	bool nlp_doubletalk;
	float erle;
	float echo_gain;
};

static void nlp_gate_probe(unsigned rate, unsigned half_db, double erl_db,
			   unsigned adapt_frames, struct gate_probe *out)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
	uint32_t state = 0x31415926u;
	uint32_t near_state = 0xfeedfaceu;
	unsigned frame = rate / 100u;
	int16_t ref[UDSP_FRAME_MAX];
	int16_t echo[UDSP_FRAME_MAX];
	int16_t mic[UDSP_FRAME_MAX];
	unsigned f;

	udsp_ec_nlms_ref_loss_set(ec, half_db);
	path_flat_init(&path, rate, 8, erl_db);
	for (f = 0; f < adapt_frames; f++) {
		noise_fill(ref, frame, -10.0, &state);
		path_run(&path, ref, echo);
		memcpy(mic, echo, frame * sizeof(*mic));
		udsp_ec_nlms.run(ec, mic, ref);
	}
	udsp_ec_nlms.freeze(ec, true);
	/* Two seconds of far single talk settle the gain tracker and the
	 * frozen filter's ERLE before the gate is read. */
	for (f = 0; f < 2u * FRAMES_PER_SECOND; f++) {
		noise_fill(ref, frame, -10.0, &state);
		path_run(&path, ref, echo);
		memcpy(mic, echo, frame * sizeof(*mic));
		udsp_ec_nlms.run(ec, mic, ref);
	}
	udsp_ec_nlms_info_get(ec, &info);
	out->nlp_far = info.nlp_active;
	out->erle = info.erle_db;
	out->echo_gain = info.echo_gain;
	noise_fill(ref, frame, -10.0, &state);
	path_run(&path, ref, echo);
	noise_fill(mic, frame, -15.0, &near_state);
	for (f = 0; f < frame; f++)
		mic[f] = sample_clamp((double)mic[f] + echo[f]);
	udsp_ec_nlms.run(ec, mic, ref);
	udsp_ec_nlms_info_get(ec, &info);
	out->nlp_doubletalk = !info.double_talk || info.nlp_active;
	udsp_ec_nlms.destroy(ec);
}

static void nlp_combined_loss_test(void)
{
	struct gate_probe weak;
	struct gate_probe hot;
	struct gate_probe mid;
	struct gate_probe edge;

	nlp_gate_probe(UDSP_SAMPLE_RATE, 0, 23.0, 36, &weak);
	nlp_gate_probe(UDSP_SAMPLE_RATE, 0, 6.0, 36, &hot);
	nlp_gate_probe(UDSP_SAMPLE_RATE, 0, 12.0, 36, &mid);
	nlp_gate_probe(UDSP_SAMPLE_RATE, 0, 17.0, 36, &edge);

	/* The fixtures sit at about 16.6 dB of tracker ERLE: under the 18 dB
	 * fast path, so the combined arm is the one engaging, and above the
	 * effective echo-likeness floor (a filter bootstrapping from zero
	 * carries its misalignment along the path, so the per-frame estimate
	 * under-reads the mic by two decades of the residual and the margin
	 * verdict holds only from about 16 dB). */
	CASE("lec nlp engages on the combined loss of a weak hybrid");
	CHECK(weak.erle >= 15.0f);
	CHECK(weak.erle < 17.9f);
	CHECK(weak.nlp_far);
	CHECK(!weak.nlp_doubletalk);

	CASE("lec nlp stays out of the way on a hot hybrid at the same erle");
	CHECK(hot.erle >= 15.0f);
	CHECK(hot.erle < 17.9f);
	CHECK(!hot.nlp_far);
	CHECK(!hot.nlp_doubletalk);

	/* About 26 dB of combined loss on the 12 dB hybrid: under the 33 dB
	 * gate, and the case that catches the gate constant drifting loose. */
	CASE("lec nlp refuses a combined loss under the gate");
	CHECK(mid.erle >= 15.0f);
	CHECK(mid.erle < 17.9f);
	CHECK(!mid.nlp_far);
	CHECK(!mid.nlp_doubletalk);

	/* On the 17 dB hybrid the conservative bound reads about 31 dB of
	 * combined loss and refuses; the raw minimum statistic would claim 34
	 * and engage. This is the pin on taking the loss from echo_gain times
	 * the headroom rather than from the raw lower bound. */
	CASE("lec nlp loss term takes the conservative bound");
	CHECK(edge.erle >= 15.0f);
	CHECK(edge.erle < 17.9f);
	CHECK(!edge.nlp_far);
	CHECK(!edge.nlp_doubletalk);
}

/* One frame of the turn-taking stimulus of nlp_echo_like_test. Returns true
 * on a frame that carries the near talker. */
static bool turn_frame_fill(unsigned f, int16_t *ref, int16_t *near,
			    uint32_t *state, uint32_t *near_state)
{
	unsigned phase = f % 16u;

	if (phase < 10u)
		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, state);
	else
		memset(ref, 0, UDSP_FRAME_SAMPLES * sizeof(*ref));
	if (phase < 12u || phase >= 14u)
		return false;
	noise_fill(near, UDSP_FRAME_SAMPLES, -34.0, near_state);
	return true;
}

/*
 * The frames that need echo-likeness are the far-end DTX gap frames inside a
 * still-active tail window. The near talker speaks in the pauses of the far
 * end (normal turn-taking). In those frames, the echo estimate is near zero,
 * and the classic window bound still explains a quiet talker, so the detector
 * cannot fire. The ERLE tracker does not update, because the frame is
 * inactive. Nothing else therefore releases the engaged gate: without the
 * verdict per frame, the comfort noise replaces the whole talker, and the
 * far end hears garbled speech.
 */
static void nlp_echo_like_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x31415926u;
	uint32_t near_state = 0xa5a5c3c3u;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t echo[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	unsigned engaged = 0;
	unsigned near_frames = 0;
	unsigned nlp_frames = 0;
	unsigned dt_frames = 0;
	unsigned f;

	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 23.0);
	for (f = 0; f < 36u; f++) {
		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
		path_run(&path, ref, echo);
		memcpy(mic, echo, sizeof(mic));
		udsp_ec_nlms.run(ec, mic, ref);
	}
	udsp_ec_nlms.freeze(ec, true);
	for (f = 0; f < FRAMES_PER_SECOND; f++) {
		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
		path_run(&path, ref, echo);
		memcpy(mic, echo, sizeof(mic));
		udsp_ec_nlms.run(ec, mic, ref);
		udsp_ec_nlms_info_get(ec, &info);
		if (info.nlp_active)
			engaged++;
	}
	/* 100 ms far spurts with 60 ms pauses; the near talker answers in the
	 * pauses, 1 dB under the echo, far below the classic bound. The
	 * discriminating frames are the gap frames whose window is still
	 * active while the frame's own estimate explains nothing. */
	for (f = 0; f < 2u * FRAMES_PER_SECOND; f++) {
		int16_t near[UDSP_FRAME_SAMPLES];
		bool near_now = turn_frame_fill(f, ref, near, &state,
						&near_state);
		unsigned i;

		path_run(&path, ref, echo);
		memcpy(mic, echo, sizeof(mic));
		if (near_now) {
			for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
				mic[i] = sample_clamp((double)mic[i] +
						      near[i]);
		}
		udsp_ec_nlms.run(ec, mic, ref);
		udsp_ec_nlms_info_get(ec, &info);
		if (!near_now)
			continue;
		near_frames++;
		if (info.nlp_active)
			nlp_frames++;
		if (info.double_talk)
			dt_frames++;
	}

	CASE("lec nlp releases for a near talker the detector cannot see");
	CHECK(engaged >= 80u);
	CHECK(near_frames >= 20u);
	CHECK_UINT(nlp_frames, 0);
	CHECK(dt_frames <= 5u);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The engine calls the reset when the generation of the applied reference
 * moves. The coefficients then subtract an estimate misaligned against the
 * re-timed reference. The instance must therefore really forget, and every
 * piece of adaptive state must go with the coefficients. A kept window ring
 * fires the detector against a reference that is gone, and a kept NLP
 * cross-fade rewrites the next frame with comfort noise. A kept
 * reference-energy sum biases the NLMS step for the life of the instance. A
 * kept gain estimate skips the warmup that guards the minimum.
 */
static void reset_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x77665544u;
	uint32_t near_state = 0x600dcafeu;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t echo[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	int16_t before[UDSP_FRAME_SAMPLES];
	double erle;
	unsigned f;

	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	for (f = 0; f < FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2; f++) {
		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
		path_run(&path, ref, echo);
		memcpy(mic, echo, sizeof(mic));
		udsp_ec_nlms.run(ec, mic, ref);
	}
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec reset clears the adaptive state whole");
	/* Converged, trusted and with the NLP engaged: the richest state a
	 * reset has to take apart. */
	CHECK(info.erle_db >= 18.0f);
	CHECK(info.nlp_active);
	CHECK(info.frames > 0u);
	CHECK(info.coef_energy > 0.0f);
	udsp_ec_nlms.freeze(ec, true);
	udsp_ec_nlms_reset(ec);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.frames, 0);
	CHECK_UINT(info.hold_frames, 0);
	CHECK(info.erle_db == 0.0f);
	CHECK(info.coef_energy == 0.0f);
	CHECK(info.echo_gain == 0.25f);
	CHECK(!info.double_talk);
	CHECK(!info.nlp_active);

	/* A silent reference under a loud near frame: a kept window ring
	 * fires the detector against the flushed reference, and a kept NLP
	 * cross-fade rewrites the output although the gate is down. */
	memset(ref, 0, sizeof(ref));
	noise_fill(mic, UDSP_FRAME_SAMPLES, -10.0, &near_state);
	memcpy(before, mic, sizeof(before));
	udsp_ec_nlms.run(ec, mic, ref);
	CHECK(memcmp(mic, before, sizeof(mic)) == 0);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK(!info.double_talk);

	/* With freeze on, the check frames cannot re-learn inside themselves:
	 * a filter that forgot cancels nothing, sample for sample, and the
	 * gain estimator is back inside its warmup. */
	for (f = 0; f < 2u; f++) {
		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
		path_run(&path, ref, echo);
		memcpy(mic, echo, sizeof(mic));
		udsp_ec_nlms.run(ec, mic, ref);
		CHECK(memcmp(mic, echo, sizeof(mic)) == 0);
	}
	udsp_ec_nlms_info_get(ec, &info);
	CHECK(info.echo_gain == 0.25f);
	udsp_ec_nlms.freeze(ec, false);
	udsp_ec_nlms.nlp(ec, false);

	CASE("lec reset instance reconverges");
	erle = erle_run(ec, &path, &state, -10.0,
			FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20);
	/* A fresh instance reaches about 59 dB here; 45 leaves headroom and
	 * still convicts a kept ref_power, whose biased NLMS denominator
	 * caps the same run near 31 dB. */
	CHECK(erle >= 45.0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The cost of the learned bound, as the bench row that changes to a late
 * hybrid shows it and the constants state it. After a hybrid step, the tighter
 * expectation holds more of the transition than a fixed 6 dB assumption
 * would. The escape must do the rest. This check bounds that cost against
 * regression. A constant change that slows the recovery further fails here.
 */
static void reconverge_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
	uint32_t state = 0x5eed5eedu;
	double erle;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, rate, 8, 23.0);
	(void)erle_run(ec, &path, &state, -10.0, 4u * FRAMES_PER_SECOND, 20);
	/* The hybrid steps 11 dB louder with its history intact. */
	path_erl_set(&path, 12.0);
	erle = erle_run(ec, &path, &state, -10.0, 10u * FRAMES_PER_SECOND, 20);
	udsp_ec_nlms_info_get(ec, &info);

	case_rate(rate,
		  "lec learned bound recovers a stepped hybrid in bounded time");
	CHECK(erle >= 25.0);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The hybrid-change lockout. A hybrid that changes while the far end keeps
 * talking leaves the filter predicting the echo of the path that is gone: the
 * two responses are uncorrelated, the residual carries both energies, and the
 * detector reads the canceller's own damage as a near-end talker. Without the
 * path-change escape, the hold stands for its whole LEC_GEIGEL_LATCH_MAX
 * period at both rates.
 *
 * The far-end history moves into the replacement hybrid, so the frames after
 * the change carry the echo of a different response and not the silence of an
 * unfilled one: a dropout ends the hold by accident and would prove nothing.
 *
 * The line converges for 4 s, so the trust count of dsp/lec.c completes before
 * the change. A detector that still waits for the count sets the classic bar,
 * and the frames after the change adapt with no hold to escape. The seeds are
 * entries 7 to 10 of the seed walk of the bench (summary_seed_get). After
 * 4 s, 14 of its first 40 entries latch at each rate, and these four latch at
 * both.
 */
struct lockout_result {
	double erle_late;
	unsigned hold_run;
	uint32_t probes;
	uint32_t diverge_resets;
};

static void lockout_drive(unsigned rate, uint32_t seed,
			  struct lockout_result *out)
{
	struct udsp_ec_nlms_info info;
	struct echo_path first;
	struct echo_path second;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
	uint32_t state = seed;
	double echo_energy = 0.0;
	double res_energy = 0.0;
	unsigned held = 0;
	unsigned run = 0;
	unsigned f;

	memset(out, 0, sizeof(*out));
	udsp_ec_nlms.nlp(ec, false);
	path_late_init(&first, rate, 12.0);
	path_flat_init(&second, rate, 8, 12.0);
	(void)erle_run(ec, &first, &state, -10.0, 4u * FRAMES_PER_SECOND, 20);
	path_history_carry(&second, &first);
	udsp_ec_nlms_info_get(ec, &info);
	held = info.hold_frames;
	for (f = 0; f < 2u * FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		unsigned i;

		noise_fill(ref, second.frame, -10.0, &state);
		path_run(&second, ref, echo);
		memcpy(mic, echo, second.frame * sizeof(*mic));
		udsp_ec_nlms.run(ec, mic, ref);
		udsp_ec_nlms_info_get(ec, &info);
		run = info.hold_frames != held ? run + 1u : 0u;
		if (run > out->hold_run)
			out->hold_run = run;
		held = info.hold_frames;
		/* The second second after the change. With the trust held at
		 * the change, the bench recovers from this change of hybrids
		 * within 950 ms on each of forty seeds. */
		if (f < FRAMES_PER_SECOND)
			continue;
		for (i = 0; i < second.frame; i++) {
			echo_energy += (double)echo[i] * echo[i];
			res_energy += (double)mic[i] * mic[i];
		}
	}
	out->erle_late = 10.0 * log10(echo_energy / res_energy);
	out->probes = info.probes;
	out->diverge_resets = info.diverge_resets;
	udsp_ec_nlms.destroy(ec);
}

static void lockout_release_test(unsigned rate)
{
	static const uint32_t seeds[] = {
		0x66db7877u, 0x0512f230u, 0xa34a6be9u, 0x4181e5a2u,
	};
	double worst_erle = 1e9;
	unsigned worst_run = 0;
	unsigned probes = 0;
	unsigned resets = 0;
	unsigned i;

	for (i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
		struct lockout_result result;

		lockout_drive(rate, seeds[i], &result);
		if (result.erle_late < worst_erle)
			worst_erle = result.erle_late;
		if (result.hold_run > worst_run)
			worst_run = result.hold_run;
		probes += result.probes;
		resets += result.diverge_resets;
	}

	case_rate(rate, "lec path-change escape releases the hybrid lockout");
	/* The latched instance measures -3.2 to -2.9 dB here and holds 200 of
	 * these 200 frames. The released one measures 32.6 to 38.3 dB and holds
	 * at most 31. */
	CHECK(worst_erle >= 20.0);
	CHECK(worst_run <= 100u);
	CHECK_UINT(probes, 4);
	CHECK_UINT(resets, 0);
}

/*
 * The escape must never treat a near-end talker over an active far end as a
 * path change. The canceller must hand that talker through unchanged.
 * Near-end speech adds the same energy to the residual as to the microphone,
 * so it cannot reach the verdict of the escape. This case checks that by a
 * sample-for-sample comparison of the near end out of the canceller with the
 * near end that went in. The comparison catches an attenuation, an added
 * residual and a swallowed talker alike. The correlation gain beside it checks
 * the level (0 dB is unattenuated).
 */
static void near_end_preserve_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
	uint32_t state = 0x0badc0deu;
	uint32_t near_state = 0xfeedfaceu;
	double product = 0.0;
	double near_energy = 0.0;
	double error_energy = 0.0;
	unsigned detected = 0;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, rate, 8, 12.0);
	(void)erle_run(ec, &path, &state, -10.0,
		       FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20);
	for (f = 0; f < 2u * FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t near[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		unsigned i;

		noise_fill(ref, path.frame, -10.0, &state);
		path_run(&path, ref, echo);
		/* 3 dB above the echo the -12 dB hybrid returns. */
		noise_fill(near, path.frame, -19.0, &near_state);
		for (i = 0; i < path.frame; i++)
			mic[i] = sample_clamp((double)echo[i] + near[i]);
		udsp_ec_nlms.run(ec, mic, ref);
		udsp_ec_nlms_info_get(ec, &info);
		if (info.double_talk)
			detected++;
		/* The detector needs the frame it fires in before it holds. */
		if (f < 5u)
			continue;
		for (i = 0; i < path.frame; i++) {
			double error = (double)mic[i] - near[i];

			product += (double)mic[i] * near[i];
			near_energy += (double)near[i] * near[i];
			error_energy += error * error;
		}
	}
	udsp_ec_nlms_info_get(ec, &info);

	case_rate(rate, "lec keeps the near end whole through double talk");
	CHECK(detected >= FRAMES_PER_SECOND);
	/* Measured 64.63 dB at 8 kHz and 64.78 dB at 16 kHz, the same figures
	 * to the last digit with the path-change escape in and without it: the
	 * near end comes out of the canceller as it went in. A near end halved
	 * on the way out reads 6.02 dB here and 6.02 dB of loss below. */
	CHECK(10.0 * log10(near_energy / error_energy) >= 30.0);
	CHECK(fabs(-20.0 * log10(product / near_energy)) <= 0.5);
	CHECK_UINT(info.probes, 0);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * A trial adapts against the verdict of the detector, so it moves the
 * trackers, the learned bound and the hangover with the coefficients. A
 * restore of the filter alone leaves the learned bound 0.31 dB louder than
 * before the trial. The classic expectation and the NLP loss gate read that
 * bound.
 *
 * Five seconds of unbroken near speech drive a trial that the near end fails;
 * a probe_finish that always keeps the trial filter fails this case.
 *
 * The first frame of the trial never adapts, because the detector holds when
 * the escape fires. At the frame where the probe count moves, the caller
 * therefore reads exactly the snapshot.
 */
static void probe_reject_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x0badc0deu;
	uint32_t near_state = 0xfeedfaceu;
	float coef_at_start = 0.0f;
	float erle_at_start = 0.0f;
	float gain_at_start = 0.0f;
	unsigned start_frame = 0;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	(void)erle_run(ec, &path, &state, -10.0,
		       FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20);
	for (f = 0; f < 7u * FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t echo[UDSP_FRAME_SAMPLES];
		int16_t near[UDSP_FRAME_SAMPLES];
		int16_t mic[UDSP_FRAME_SAMPLES];
		unsigned i;

		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
		path_run(&path, ref, echo);
		noise_fill(near, UDSP_FRAME_SAMPLES, -19.0, &near_state);
		for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
			mic[i] = sample_clamp((double)echo[i] + near[i]);
		udsp_ec_nlms.run(ec, mic, ref);
		udsp_ec_nlms_info_get(ec, &info);
		if (info.probes == 1u && !start_frame) {
			start_frame = f + 1u;
			coef_at_start = info.coef_energy;
			erle_at_start = info.erle_db;
			gain_at_start = info.echo_gain;
		}
		/* The countdown starts on the frame after the one that armed
		 * the trial, and every frame here adapts, so the verdict comes
		 * exactly LEC_PROBE_FRAMES later. */
		if (start_frame && f + 1u == start_frame + 100u)
			break;
	}
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec rejected trial gives the detector back");
	CHECK_UINT(info.probes, 1);
	CHECK(start_frame != 0u);
	/* Restoring the filter alone leaves the learned bound at 0.174430
	 * against the 0.162337 it started from. */
	CHECK(info.echo_gain == gain_at_start);
	CHECK(info.coef_energy == coef_at_start);
	CHECK(info.erle_db == erle_at_start);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The countdown of the trial spends the frames that the filter adapts on, not
 * the frames whose reference is active. Freeze holds the filter still for
 * reasons of the caller, and a countdown through it would close the trial on a
 * filter that never moved. It would judge the trial by a residual that could
 * not change. Measured with a countdown on the reference alone, the trial
 * closes inside a 2 s freeze. With that countdown, the filter is back at its
 * snapshot 50 frames after the release. With the adapt verdict, the trial is
 * still open and moving there.
 */
static void probe_freeze_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x0badc0deu;
	uint32_t near_state = 0xfeedfaceu;
	float coef_at_start = 0.0f;
	float coef_frozen = 0.0f;
	float coef_released = 0.0f;
	unsigned started = 0;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	(void)erle_run(ec, &path, &state, -10.0,
		       FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20);
	for (f = 0; f < 10u * FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t echo[UDSP_FRAME_SAMPLES];
		int16_t near[UDSP_FRAME_SAMPLES];
		int16_t mic[UDSP_FRAME_SAMPLES];
		unsigned i;

		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
		path_run(&path, ref, echo);
		noise_fill(near, UDSP_FRAME_SAMPLES, -19.0, &near_state);
		for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
			mic[i] = sample_clamp((double)echo[i] + near[i]);
		udsp_ec_nlms.run(ec, mic, ref);
		udsp_ec_nlms_info_get(ec, &info);
		if (!started && info.probes == 1u) {
			started = f + 1u;
			coef_at_start = info.coef_energy;
			udsp_ec_nlms.freeze(ec, true);
		}
		if (started && f + 1u == started + 2u * FRAMES_PER_SECOND) {
			coef_frozen = info.coef_energy;
			udsp_ec_nlms.freeze(ec, false);
		}
		if (started && f + 1u == started + 250u) {
			coef_released = info.coef_energy;
			break;
		}
	}
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec trial does not spend a frozen frame");
	CHECK(started != 0u);
	CHECK_UINT(info.probes, 1);
	/* The frozen filter cannot move, so the trial cannot be judged. */
	CHECK(coef_frozen == coef_at_start);
	/* Released: the trial is still open and still adapting. A countdown
	 * that spends the frozen frames closes it at 0.06305251 and leaves the
	 * filter there; this one reads 0.07716025. */
	CHECK(coef_released != coef_at_start);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The stand-down after a refused trial (dsp/lec.c,
 * LEC_TRIAL_STANDDOWN_FRAMES). A hybrid that changes under a near-end talker
 * is the one case that arms the escape and then refuses its trial. The
 * energy of the talker keeps the trial from the 3 dB residual cut that the
 * verdict wants. Without the stand-down, the escape re-arms twenty frames
 * later and adapts another second on the same talker, again and again.
 *
 * Measured over fifteen seconds at both rates: 10 to 11 trials without the
 * stand-down, 3 with it, and 2 with no escape at all. The stand-down covers
 * one whole latch cycle, so the measured gap between trials is 601 frames,
 * and 145 without the carry-on.
 */
static void probe_cooldown_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct echo_path first;
	struct echo_path second;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
	uint32_t state = 0x13572468u;
	uint32_t near_state = 0x13572468u ^ 0xfeedfaceu;
	double product = 0.0;
	double near_energy = 0.0;
	unsigned last_probe = 0;
	unsigned gap_min = 0;
	unsigned probes = 0;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_late_init(&first, rate, 12.0);
	path_flat_init(&second, rate, 8, 12.0);
	(void)erle_run(ec, &first, &state, -10.0, 2u * FRAMES_PER_SECOND, 20);
	path_history_carry(&second, &first);
	for (f = 0; f < 15u * FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t near[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		unsigned i;

		noise_fill(ref, second.frame, -10.0, &state);
		path_run(&second, ref, echo);
		/* 3 dB over the echo: loud enough to refuse the trial, quiet
		 * enough to leave the escape's ratio above its bar. */
		noise_fill(near, second.frame, -19.0, &near_state);
		for (i = 0; i < second.frame; i++)
			mic[i] = sample_clamp((double)echo[i] + near[i]);
		udsp_ec_nlms.run(ec, mic, ref);
		udsp_ec_nlms_info_get(ec, &info);
		if (info.probes != probes) {
			unsigned gap = last_probe ? f + 1u - last_probe : 0u;

			if (gap && (!gap_min || gap < gap_min))
				gap_min = gap;
			last_probe = f + 1u;
			probes = info.probes;
		}
		if (f < 2u * FRAMES_PER_SECOND)
			continue;
		for (i = 0; i < second.frame; i++) {
			product += (double)mic[i] * near[i];
			near_energy += (double)near[i] * near[i];
		}
	}
	udsp_ec_nlms_info_get(ec, &info);

	case_rate(rate, "lec escape stands down after a trial the line refused");
	CHECK(info.probes >= 2u);
	CHECK(info.probes <= 3u);
	/* One whole latch cycle between trials, whichever arm starts
	 * them: LEC_GEIGEL_LATCH_MAX plus LEC_PROBE_FRAMES. */
	CHECK(gap_min >= 600u);
	CHECK(fabs(-20.0 * log10(product / near_energy)) <= 0.5);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The other side of the same rule. A latch trial that the line refuses on its
 * own, with no stand-down pending, hands the escape nothing to carry. A
 * stand-down there too keeps a real hybrid change after a refused latch trial
 * in lockout for 452 frames. The narrow rule escapes it inside 34 frames.
 */
static void latch_refusal_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct echo_path first;
	struct echo_path second;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
	uint32_t state = 0x13572468u;
	uint32_t near_state = 0x13572468u ^ 0xfeedfaceu;
	double echo_energy = 0.0;
	double res_energy = 0.0;
	unsigned held;
	unsigned run = 0;
	unsigned hold_run = 0;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_late_init(&first, rate, 12.0);
	path_flat_init(&second, rate, 8, 12.0);
	(void)erle_run(ec, &first, &state, -10.0, 2u * FRAMES_PER_SECOND, 20);
	/* 6.5 s of near speech over the unchanged hybrid: the latch arm runs
	 * one trial and the talker refuses it. */
	for (f = 0; f < 650u; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t near[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		unsigned i;

		noise_fill(ref, first.frame, -10.0, &state);
		path_run(&first, ref, echo);
		noise_fill(near, first.frame, -19.0, &near_state);
		for (i = 0; i < first.frame; i++)
			mic[i] = sample_clamp((double)echo[i] + near[i]);
		udsp_ec_nlms.run(ec, mic, ref);
	}
	udsp_ec_nlms_info_get(ec, &info);
	held = info.hold_frames;
	path_history_carry(&second, &first);
	for (f = 0; f < 8u * FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		unsigned i;

		noise_fill(ref, second.frame, -10.0, &state);
		path_run(&second, ref, echo);
		memcpy(mic, echo, second.frame * sizeof(*mic));
		udsp_ec_nlms.run(ec, mic, ref);
		udsp_ec_nlms_info_get(ec, &info);
		run = info.hold_frames != held ? run + 1u : 0u;
		if (run > hold_run)
			hold_run = run;
		held = info.hold_frames;
		if (f < 4u * FRAMES_PER_SECOND)
			continue;
		for (i = 0; i < second.frame; i++) {
			echo_energy += (double)echo[i] * echo[i];
			res_energy += (double)mic[i] * mic[i];
		}
	}
	udsp_ec_nlms_info_get(ec, &info);

	case_rate(rate, "lec latch refusal alone does not stand the escape down");
	/* 452 held frames and 4.9 dB when a refused latch trial arms the
	 * stand-down; 33 and 72.8 dB when only the escape's own does. */
	CHECK(hold_run <= 100u);
	CHECK(10.0 * log10(echo_energy / res_energy) >= 20.0);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The bootstrap false detection. While the filter converges, its echo
 * estimate under-reads the microphone by exactly the residual it still
 * leaves; an expectation that ignores that term reads a 1.5 dB excursion of
 * the echo power as a near-end talker, and one detection costs the whole
 * 24-frame hangover in the middle of the bootstrap. The fixtures are the seeds
 * where that cost is highest: on the composite hybrid at the 64 ms tail it
 * leaves the only one of forty seeds under the 25 dB gate at 8 kHz (22.09 dB
 * with 25 frames held, 27.49 dB with none), and the late hybrid holds on eight
 * of forty seeds at 16 kHz and five of forty at 8 kHz.
 *
 * The far end talks alone here, so there is no near end to protect and every
 * held frame is a false detection by construction.
 */
struct bootstrap_fixture {
	unsigned rate;
	unsigned tail_ms;
	uint32_t seed;
	bool late;
	double erle_bar;
};

static void bootstrap_hold_test(void)
{
	static const struct bootstrap_fixture fixtures[] = {
		{ UDSP_SAMPLE_RATE, 64u, 0x5897463fu, false, 25.0 },
		{ UDSP_SAMPLE_RATE, 48u, 0x8c350b4cu, true, 25.0 },
		{ UDSP_SAMPLE_RATE_WB, 48u, 0xdfb95f5bu, true, 20.0 },
	};
	unsigned held = 0;
	unsigned resets = 0;
	unsigned under_bar = 0;
	unsigned i;

	for (i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); i++) {
		struct udsp_ec_nlms_info info;
		struct echo_path path;
		struct udsp_ec *ec;
		uint32_t state = fixtures[i].seed;
		double erle;

		ec = udsp_ec_nlms.create_rate(fixtures[i].tail_ms,
					      fixtures[i].rate);
		udsp_ec_nlms.nlp(ec, false);
		if (fixtures[i].late)
			path_late_init(&path, fixtures[i].rate, 12.0);
		else
			path_composite_init(&path, fixtures[i].rate, 12.0);
		erle = erle_run(ec, &path, &state, -10.0, FRAMES_PER_SECOND, 20);
		udsp_ec_nlms_info_get(ec, &info);
		held += info.hold_frames;
		resets += info.diverge_resets;
		if (erle < fixtures[i].erle_bar)
			under_bar++;
		udsp_ec_nlms.destroy(ec);
	}

	CASE("lec bootstrap holds nothing on far-end single talk");
	/* 25, 25 and 25 held frames without the misalignment term. */
	CHECK_UINT(held, 0);
	CHECK_UINT(under_bar, 0);
	CHECK_UINT(resets, 0);
}

static void watchdog_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(64);
	uint32_t state = 0x600d1ecu;

	udsp_ec_nlms.nlp(ec, false);
	path_composite_init(&path, UDSP_SAMPLE_RATE, 12.0);
	(void)erle_run(ec, &path, &state, -10.0, 2u * FRAMES_PER_SECOND, 20);
	udsp_ec_nlms_info_get(ec, &info);

	/* A converged filter carries the energy of the path it learned
	 * (10^-1.2 for a 12 dB ERL), an order of magnitude below the
	 * watchdog bound. */
	CASE("lec watchdog leaves a converged filter alone");
	CHECK_UINT(info.diverge_resets, 0);
	CHECK(info.coef_energy > 0.03f);
	CHECK(info.coef_energy < 0.13f);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The create_rate contract (ec.h) and the wideband tail cap. create() must
 * stay the 8 kHz form of the same call, sample for sample, because every
 * 8 kHz line reaches the canceller through it.
 */
static void create_rate_test(void)
{
	static const unsigned tails[] = { 0u, 15u, 16u, 48u, 64u, 129u };
	struct udsp_ec_nlms_info wide;
	struct udsp_ec_nlms_info narrow;
	struct echo_path path;
	struct udsp_ec *plain;
	struct udsp_ec *rated;
	struct udsp_ec *ec;
	uint32_t state = 0x4c454331u;
	unsigned differences = 0;
	unsigned i;

	CASE("lec create_rate builds the wideband instance");
	ec = udsp_ec_nlms.create_rate(48, UDSP_SAMPLE_RATE_WB);
	CHECK(ec != NULL);
	if (!ec)
		return;
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 48);
	udsp_ec_nlms_info_get(ec, &wide);
	CHECK_UINT(wide.taps, 768);
	udsp_ec_nlms.destroy(ec);
	ec = udsp_ec_nlms.create_rate(0, UDSP_SAMPLE_RATE_WB);
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 64);
	udsp_ec_nlms_info_get(ec, &wide);
	CHECK_UINT(wide.taps, 1024);
	udsp_ec_nlms.destroy(ec);

	/* ec.h states two line rates and nothing else; a handle at any other
	 * rate has no canceller rather than a filter of the wrong length. */
	CASE("lec create_rate refuses a rate that is not a line rate");
	CHECK(udsp_ec_nlms.create_rate(48, 0) == NULL);
	CHECK(udsp_ec_nlms.create_rate(48, 4000) == NULL);
	CHECK(udsp_ec_nlms.create_rate(48, 11025) == NULL);
	CHECK(udsp_ec_nlms.create_rate(48, 32000) == NULL);
	CHECK(udsp_ec_nlms.create_rate(48, 48000) == NULL);

	CASE("lec wideband tail clamps to the 64 ms cap");
	ec = udsp_ec_nlms.create_rate(128, UDSP_SAMPLE_RATE_WB);
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 64);
	udsp_ec_nlms_info_get(ec, &wide);
	CHECK_UINT(wide.taps, 1024);
	udsp_ec_nlms.destroy(ec);
	ec = udsp_ec_nlms.create_rate(64, UDSP_SAMPLE_RATE_WB);
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 64);
	udsp_ec_nlms.destroy(ec);
	/* 65 and 66 ms are the first two millisecond steps past the cap. A
	 * clamp that compares one too high still passes a 128 ms probe and
	 * fails here. */
	ec = udsp_ec_nlms.create_rate(65, UDSP_SAMPLE_RATE_WB);
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 64);
	udsp_ec_nlms_info_get(ec, &wide);
	CHECK_UINT(wide.taps, 1024);
	udsp_ec_nlms.destroy(ec);
	ec = udsp_ec_nlms.create_rate(66, UDSP_SAMPLE_RATE_WB);
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 64);
	udsp_ec_nlms_info_get(ec, &wide);
	CHECK_UINT(wide.taps, 1024);
	udsp_ec_nlms.destroy(ec);
	/* The floor of the range is the narrowband one at both rates. */
	ec = udsp_ec_nlms.create_rate(15, UDSP_SAMPLE_RATE_WB);
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 16);
	udsp_ec_nlms_info_get(ec, &wide);
	CHECK_UINT(wide.taps, 256);
	udsp_ec_nlms.destroy(ec);

	CASE("lec narrowband tail range survives the wideband cap");
	ec = udsp_ec_nlms.create_rate(128, UDSP_SAMPLE_RATE);
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 128);
	udsp_ec_nlms_info_get(ec, &narrow);
	CHECK_UINT(narrow.taps, 1024);
	udsp_ec_nlms.destroy(ec);
	ec = udsp_ec_nlms.create_rate(129, UDSP_SAMPLE_RATE);
	CHECK_UINT(udsp_ec_nlms_tail_get(ec), 128);
	udsp_ec_nlms.destroy(ec);

	CASE("lec create is create_rate at 8 kHz");
	for (i = 0; i < sizeof(tails) / sizeof(tails[0]); i++) {
		plain = udsp_ec_nlms.create(tails[i]);
		rated = udsp_ec_nlms.create_rate(tails[i], UDSP_SAMPLE_RATE);
		udsp_ec_nlms_info_get(plain, &narrow);
		udsp_ec_nlms_info_get(rated, &wide);
		CHECK_UINT(wide.tail_ms, narrow.tail_ms);
		CHECK_UINT(wide.taps, narrow.taps);
		udsp_ec_nlms.destroy(plain);
		udsp_ec_nlms.destroy(rated);
	}
	plain = udsp_ec_nlms.create(48);
	rated = udsp_ec_nlms.create_rate(48, UDSP_SAMPLE_RATE);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	for (i = 0; i < FRAMES_PER_SECOND; i++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t plain_out[UDSP_FRAME_MAX];
		int16_t rated_out[UDSP_FRAME_MAX];

		noise_fill(ref, path.frame, -10.0, &state);
		path_run(&path, ref, echo);
		memcpy(plain_out, echo, path.frame * sizeof(*plain_out));
		memcpy(rated_out, echo, path.frame * sizeof(*rated_out));
		udsp_ec_nlms.run(plain, plain_out, ref);
		udsp_ec_nlms.run(rated, rated_out, ref);
		if (memcmp(plain_out, rated_out,
			   path.frame * sizeof(*plain_out)))
			differences++;
	}
	CHECK_UINT(differences, 0);
	udsp_ec_nlms_info_get(plain, &narrow);
	udsp_ec_nlms_info_get(rated, &wide);
	CHECK(narrow.coef_energy == wide.coef_energy);
	CHECK(narrow.erle_db == wide.erle_db);
	udsp_ec_nlms.destroy(plain);
	udsp_ec_nlms.destroy(rated);
}

/* Power response of a hybrid at one frequency, straight from the taps: the
 * oracle that says which band the shape occupies. */
static double path_response_db(const struct echo_path *path, double hz,
			       unsigned rate)
{
	double re = 0.0;
	double im = 0.0;
	unsigned k;

	for (k = 0; k < path->taps; k++) {
		double angle = 2.0 * M_PI * hz * (double)k / rate;

		re += (double)path->h[k] * cos(angle);
		im -= (double)path->h[k] * sin(angle);
	}
	return 10.0 * log10(re * re + im * im);
}

/*
 * The upper octave. Every other hybrid in the suite lives under 4 kHz. No
 * other case therefore asks the wideband filter to learn a response that the
 * narrowband line cannot carry at all. The decision on a two-band fallback
 * depends on this case. A band-split canceller must hold coherence across
 * 4 kHz to reach the bar that the full-band filter reaches here.
 */
static void upper_band_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state = 0x60c7a5e5u;
	double erle;

	path_upper_init(&path, UDSP_SAMPLE_RATE_WB, 12.0);

	/* The band pin: without it a shape that quietly drifted into the low
	 * band would still converge and the case would prove nothing. */
	CASE("lec upper-band hybrid sits above the narrowband band edge");
	CHECK_UINT(path.taps, 160);
	CHECK(path_response_db(&path, 6000.0, UDSP_SAMPLE_RATE_WB) -
	      path_response_db(&path, 1000.0, UDSP_SAMPLE_RATE_WB) >= 20.0);
	CHECK(path_response_db(&path, 6000.0, UDSP_SAMPLE_RATE_WB) -
	      path_response_db(&path, 3000.0, UDSP_SAMPLE_RATE_WB) >= 10.0);

	CASE("lec upper-band hybrid converges at 16 kHz");
	ec = udsp_ec_nlms.create_rate(48, UDSP_SAMPLE_RATE_WB);
	udsp_ec_nlms.nlp(ec, false);
	erle = erle_run(ec, &path, &state, -10.0, FRAMES_PER_SECOND, 20);
	CHECK(erle >= 25.0);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The tap that the fused pass contends for. One pass over the coefficients
 * holds the windows of two consecutive samples at once, and they collide on
 * the last tap of the older window. The push of the next sample writes exactly
 * there, unless the history ring keeps one sample more than the window
 * (dsp/lec.c, hist_push).
 *
 * Every other hybrid in this suite puts its energy well inside the tail, where
 * a corrupted last tap costs a fraction of a dB, unseen. This hybrid
 * puts the whole echo on the last tap of the filter, so only that tap can
 * cancel anything. A ring one sample too short then cannot reach the bar.
 */
static void last_tap_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(16, rate);
	uint32_t state = 0x1a577a95u;
	unsigned taps = 16u * (rate / 1000u);
	double erle;

	udsp_ec_nlms.nlp(ec, false);
	/* A hybrid the shape helpers cannot state in whole milliseconds: one
	 * reflection at the last tap of a 16 ms tail. */
	path_geometry_set(&path, rate);
	path.taps = taps;
	path.h[taps - 1u] = 1.0f;
	path_erl_set(&path, 12.0);
	erle = erle_run(ec, &path, &state, -10.0, 2u * FRAMES_PER_SECOND, 20);
	udsp_ec_nlms_info_get(ec, &info);

	case_rate(rate, "lec cancels an echo that lands on the last tap");
	CHECK_UINT(info.taps, taps);
	CHECK(erle >= 25.0);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The update that the fused pass cannot carry. The last sample of a frame has
 * no estimate behind it to fuse with, so its update stands alone at the end of
 * frame_filter_run. A reference that is silent for the whole frame except its
 * last sample makes that update the only one of the frame. Every earlier
 * sample sees a zero window and a zero error. The filter therefore moves only
 * if the trailing update runs.
 */
static void last_sample_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(16, rate);
	int16_t ref[UDSP_FRAME_MAX];
	int16_t mic[UDSP_FRAME_MAX];
	unsigned frame = rate / 100u;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, rate, 0, 12.0);
	memset(ref, 0, sizeof(ref));
	ref[frame - 1u] = 32767;
	path_run(&path, ref, mic);
	udsp_ec_nlms.run(ec, mic, ref);
	udsp_ec_nlms_info_get(ec, &info);

	case_rate(rate, "lec adapts on the last sample of a frame");
	CHECK_UINT(info.frames, 1);
	CHECK_UINT(info.hold_frames, 0);
	CHECK(info.coef_energy > 0.0f);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The equivalence suite. On aarch64 the canceller carries two forms of the
 * per-tap loops: the canonical scalar one and an Advanced SIMD one that
 * accumulates the dot product in thirty-two partial sums instead of one. The
 * two are equivalent, not identical, so this suite runs each scenario through
 * both forms and bounds the difference.
 *
 * One hybrid drives both arms: the microphone frame is built once and handed
 * to two fresh instances, so the arms never see different samples and any
 * difference at the output is the canceller's own. Three things are read: how
 * far two emitted samples drift apart, whether the line delivers the same
 * ERLE, and whether any decision came out differently (a trial, a divergence
 * reset, a hold, a detector verdict, an engaged NLP).
 *
 * On a target with one path the cases print a skip note and pass: there is
 * nothing to compare against there.
 */
#define EQUIV_DELTA_MAX		4u
#define EQUIV_ERLE_DELTA_DB	0.1
#define EQUIV_MEASURE_FROM	20u

/*
 * What the two forms may differ by in the internal state at the end of a
 * scenario, on top of the emitted samples.
 *
 * The one difference between the forms is the order the dot product
 * accumulates in, worth about u*sqrt(taps) of relative error on the estimate
 * with u = 2^-24: 1.9e-6 at the 1024-tap ceiling. The coefficients inherit
 * that order through the step, and a sum of squares doubles a relative
 * figure, so coef_energy may differ by about 4e-6 in the worst case. The bar
 * is 1e-5, which is 14 times the worst value of this bench (7.1e-7 over
 * 96 runs on target boards) and 100 times inside the 1e-3 a single
 * mis-updated tap of a wrong kernel produces.
 *
 * erle_db is 10*log10 of a ratio whose numerator is input-derived and equal in
 * both arms; only the residual moves, by the estimate's perturbation divided
 * by the residual-to-microphone ratio. At 30 dB of tracked ERLE that is 5e-4
 * dB and it grows tenfold per 20 dB, so 0.05 dB covers past 65 dB. Measured
 * worst: 4.7e-4 dB.
 *
 * echo_gain is compared exactly, and that needs one more step than it looks.
 * echo_gain_update() consumes the frame's microphone power and the tail window
 * power, both computed from the input frames the two arms share, and it is
 * called on the frames whose reference is active, which is the same set in
 * both arms. Those updates are therefore identical by construction. The one
 * writer that is not input-derived is probe_finish(): its verdict compares the
 * live residual power against the residual measured before the trial, and the
 * residual is exactly what the two forms compute differently.
 *
 * A split verdict, one arm keeping the trial and the other restoring the
 * snapshot, is therefore the only route to a drifting echo_gain, and it cannot
 * pass unseen. The arm that kept the trial carries a whole second of
 * adaptation that the other threw away, so the two coefficient sets differ
 * macroscopically and the coef_energy bound above fails by orders of magnitude
 * before echo_gain is even read: the amplifying scenario's own trial moves
 * coef_energy from nothing to past the divergence bound, which is 1e5 times
 * the 1e-5 the comparison allows. The frame the trial starts on is pinned
 * separately, by the per-frame probes comparison, and the frame its countdown
 * ends on is pinned by the per-frame hold comparison, so the verdict is the
 * only thing left to argue about.
 *
 * The residual failure mode is a knife-edge verdict, where the trial's
 * improvement ratio sits within the rounding difference of LEC_PROBE_IMPROVE.
 * No run of this bench shows it. If it ever fires, it fails on
 * coef_energy rather than passing: read the printed coef figure first, because
 * a macroscopic one means a split verdict on a knife edge and not a defective
 * kernel.
 */
#define EQUIV_COEF_REL		1e-5
#define EQUIV_ERLE_STATE_DB	0.05
#define EQUIV_COVER_CASE	"lec equivalence scenarios reach every decision"

enum equiv_hybrid {
	EQUIV_FLAT,
	EQUIV_DISPERSIVE,
	EQUIV_COMPOSITE,
	EQUIV_LATE,
	EQUIV_UPPER,
};

struct equiv_scenario {
	const char *name;
	unsigned rate;
	unsigned tail_ms;
	uint32_t seed;
	unsigned frames;
	enum equiv_hybrid hybrid;
	enum equiv_hybrid change_to;
	unsigned change_at;
	double erl_db;
	/* The return loss of the hybrid the line changes to, when it differs
	 * from the one it starts on. 0 means the two match, which is what
	 * every scenario but the amplifying one says. */
	double change_erl_db;
	double near_dbm0;
	unsigned near_from;
	unsigned near_to;
	bool nlp;
	/*
	 * What this scenario must make the canceller do, asserted here rather
	 * than only in the suite total: a scenario that stopped reaching its
	 * decisions would otherwise keep passing on the decisions of another
	 * scenario.
	 * coef_live demands that the state comparison ran on a filter that
	 * carries energy, because rel_diff_get() of two zeroes is 0 and would
	 * satisfy the bound without comparing anything.
	 */
	unsigned min_probes;
	unsigned min_resets;
	unsigned min_hold_run;
	bool coef_live;
};

struct equiv_result {
	unsigned max_delta;
	unsigned diff_frames;
	unsigned decision_frames;
	unsigned dt_frames;
	unsigned nlp_frames;
	unsigned max_hold_run;
	bool coef_live;
	double erle_scalar;
	double erle_simd;
	struct udsp_ec_nlms_info scalar;
	struct udsp_ec_nlms_info simd;
};

/* What the scenarios between them must have made the canceller do, so the
 * per-frame decision oracle above is exercised rather than vacuous. */
struct equiv_fired {
	uint32_t probes;
	uint32_t resets;
	uint32_t holds;
	unsigned dt_frames;
	unsigned nlp_frames;
};

static void equiv_hybrid_build(struct echo_path *path, enum equiv_hybrid kind,
			       unsigned rate, double erl_db)
{
	switch (kind) {
	case EQUIV_DISPERSIVE:
		path_dispersive_init(path, rate, erl_db);
		return;
	case EQUIV_COMPOSITE:
		path_composite_init(path, rate, erl_db);
		return;
	case EQUIV_LATE:
		path_late_init(path, rate, erl_db);
		return;
	case EQUIV_UPPER:
		path_upper_init(path, rate, erl_db);
		return;
	default:
		path_flat_init(path, rate, 8, erl_db);
		return;
	}
}

static double rel_diff_get(float first, float second)
{
	double top = fabs((double)first) > fabs((double)second) ?
		     fabs((double)first) : fabs((double)second);

	if (top == 0.0)
		return 0.0;
	return fabs((double)first - (double)second) / top;
}

static void equiv_run(const struct equiv_scenario *s, struct equiv_result *out)
{
	struct udsp_ec *scalar = udsp_ec_nlms.create_rate(s->tail_ms, s->rate);
	struct udsp_ec *simd = udsp_ec_nlms.create_rate(s->tail_ms, s->rate);
	struct echo_path first;
	struct echo_path second;
	struct echo_path *active = &first;
	uint32_t state = s->seed;
	uint32_t near_state = s->seed ^ 0xfeedfaceu;
	unsigned frame = s->rate / 100u;
	double echo_energy = 0.0;
	double scalar_energy = 0.0;
	double simd_energy = 0.0;
	uint32_t held = 0;
	unsigned run = 0;
	unsigned f;

	memset(out, 0, sizeof(*out));
	udsp_ec_nlms.nlp(scalar, s->nlp);
	udsp_ec_nlms.nlp(simd, s->nlp);
	equiv_hybrid_build(&first, s->hybrid, s->rate, s->erl_db);
	equiv_hybrid_build(&second, s->change_to, s->rate,
			   s->change_erl_db != 0.0 ? s->change_erl_db :
						     s->erl_db);
	for (f = 0; f < s->frames; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		int16_t scalar_out[UDSP_FRAME_MAX];
		int16_t simd_out[UDSP_FRAME_MAX];
		unsigned i;

		if (s->change_at && f == s->change_at) {
			path_history_carry(&second, &first);
			active = &second;
		}
		noise_fill(ref, frame, -10.0, &state);
		path_run(active, ref, echo);
		memcpy(mic, echo, frame * sizeof(*mic));
		if (s->near_dbm0 < 0.0 && f >= s->near_from && f < s->near_to) {
			int16_t near[UDSP_FRAME_MAX];

			noise_fill(near, frame, s->near_dbm0, &near_state);
			for (i = 0; i < frame; i++)
				mic[i] = sample_clamp((double)mic[i] + near[i]);
		}
		memcpy(scalar_out, mic, frame * sizeof(*mic));
		memcpy(simd_out, mic, frame * sizeof(*mic));
		udsp_ec_nlms_path_set(UDSP_EC_NLMS_PATH_SCALAR);
		udsp_ec_nlms.run(scalar, scalar_out, ref);
		udsp_ec_nlms_info_get(scalar, &out->scalar);
		udsp_ec_nlms_path_set(UDSP_EC_NLMS_PATH_SIMD);
		udsp_ec_nlms.run(simd, simd_out, ref);
		udsp_ec_nlms_info_get(simd, &out->simd);
		/*
		 * Every decision on every frame: a probe that fires on frame
		 * 100 in one arm and on frame 200 in the other leaves the
		 * totals equal and the cancellers different.
		 */
		if (out->scalar.double_talk != out->simd.double_talk ||
		    out->scalar.nlp_active != out->simd.nlp_active ||
		    out->scalar.probes != out->simd.probes ||
		    out->scalar.diverge_resets != out->simd.diverge_resets ||
		    out->scalar.hold_frames != out->simd.hold_frames)
			out->decision_frames++;
		if (out->scalar.double_talk)
			out->dt_frames++;
		if (out->scalar.nlp_active)
			out->nlp_frames++;
		/* The longest unbroken hold, which is what says the detector
		 * latched rather than flickering (the lockout_drive idiom). */
		run = out->scalar.hold_frames != held ? run + 1u : 0u;
		if (run > out->max_hold_run)
			out->max_hold_run = run;
		held = out->scalar.hold_frames;
		if (memcmp(scalar_out, simd_out, frame * sizeof(*mic)))
			out->diff_frames++;
		for (i = 0; i < frame; i++) {
			unsigned delta = (unsigned)abs(scalar_out[i] -
						       simd_out[i]);

			if (delta > out->max_delta)
				out->max_delta = delta;
		}
		if (f < EQUIV_MEASURE_FROM)
			continue;
		for (i = 0; i < frame; i++) {
			echo_energy += (double)echo[i] * echo[i];
			scalar_energy += (double)scalar_out[i] * scalar_out[i];
			simd_energy += (double)simd_out[i] * simd_out[i];
		}
	}
	out->erle_scalar = 10.0 * log10(echo_energy / scalar_energy);
	out->erle_simd = 10.0 * log10(echo_energy / simd_energy);
	/* The state comparison is only a comparison if there is state: two
	 * zeroes satisfy a relative bound without meeting. */
	out->coef_live = out->scalar.coef_energy > 0.0f ||
			 out->simd.coef_energy > 0.0f;
	udsp_ec_nlms.destroy(scalar);
	udsp_ec_nlms.destroy(simd);
}

/*
 * The scenarios walk what the canceller does, not only what it computes: the
 * plain convergence, the wideband worst case at the 64 ms tail, the detector
 * and the NLP under a near-end burst, the path-change escape, a tap count that
 * leaves a remainder for the four-wide tail of the vector loop (17 ms at 8 kHz
 * is 136 taps, eight past the fourth thirty-two-tap block), five seconds of
 * near speech that ends in a refused trial, and the upper-octave wideband
 * hybrid.
 */
static const struct equiv_scenario equiv_scenarios[] = {
	{
		.name = "lec paths agree on the flat hybrid",
		.rate = UDSP_SAMPLE_RATE, .tail_ms = 48,
		.seed = 0x13572468u, .frames = 300,
		.hybrid = EQUIV_FLAT, .change_to = EQUIV_FLAT, .erl_db = 12.0,
	},
	{
		.name = "lec paths agree at the wideband 64 ms tail",
		.rate = UDSP_SAMPLE_RATE_WB, .tail_ms = 64,
		.seed = 0x0f1e2d3cu, .frames = 300,
		.hybrid = EQUIV_COMPOSITE, .change_to = EQUIV_COMPOSITE,
		.erl_db = 12.0,
	},
	{
		.name = "lec paths agree through double talk and the nlp",
		.rate = UDSP_SAMPLE_RATE_WB, .tail_ms = 48,
		.seed = 0x0badc0deu, .frames = 400,
		.hybrid = EQUIV_COMPOSITE, .change_to = EQUIV_COMPOSITE,
		.erl_db = 12.0,
		.near_dbm0 = -19.0, .near_from = 200, .near_to = 300,
		.nlp = true,
	},
	{
		.name = "lec paths agree across a hybrid change",
		.rate = UDSP_SAMPLE_RATE_WB, .tail_ms = 48,
		.seed = 0xc8a3febeu, .frames = 400,
		.hybrid = EQUIV_LATE, .change_to = EQUIV_FLAT,
		.change_at = 200, .erl_db = 12.0,
	},
	{
		.name = "lec paths agree on a tap count with a remainder",
		.rate = UDSP_SAMPLE_RATE, .tail_ms = 17,
		.seed = 0x5a5a1234u, .frames = 300,
		.hybrid = EQUIV_DISPERSIVE, .change_to = EQUIV_DISPERSIVE,
		.erl_db = 12.0,
	},
	{
		.name = "lec paths agree through a refused trial",
		.rate = UDSP_SAMPLE_RATE, .tail_ms = 48,
		.seed = 0xfeedfaceu, .frames = 800,
		.hybrid = EQUIV_FLAT, .change_to = EQUIV_FLAT, .erl_db = 12.0,
		.near_dbm0 = -19.0, .near_from = 150, .near_to = 800,
		/* Five seconds of near speech over an active far end is the
		 * latch arm's own trial (sustained_doubletalk_test). */
		.min_probes = 1, .coef_live = true,
	},
	{
		.name = "lec paths agree on the upper-octave hybrid",
		.rate = UDSP_SAMPLE_RATE_WB, .tail_ms = 48,
		.seed = 0x60c7a5e5u, .frames = 300,
		.hybrid = EQUIV_UPPER, .change_to = EQUIV_UPPER, .erl_db = 12.0,
	},
	/*
	 * The decisions the quiet hybrids never reach. A hybrid that amplifies
	 * is louder than every expectation the detector holds, so it holds
	 * from the first frame and the latch escape is the only way out; the
	 * trial it ends in then adapts against an echo whose solution carries
	 * more energy than a passive path can, and the divergence watchdog
	 * fires. One scenario therefore drives a hold that runs for the whole
	 * run, an adaptation trial, and a coefficient reset, which is what
	 * makes the per-frame decision oracle above worth reading.
	 */
	{
		.name = "lec paths agree on an amplifying hybrid",
		.rate = UDSP_SAMPLE_RATE, .tail_ms = 48,
		.seed = 0x0c0ffee1u, .frames = 1500,
		.hybrid = EQUIV_FLAT, .change_to = EQUIV_FLAT, .erl_db = -8.0,
		/*
		 * The step control throttles the step by the share of the
		 * microphone the far end can explain. On a hybrid this loud,
		 * the learned gain must climb before the step opens up again.
		 * The runaway therefore reaches the watchdog bound at 11.6 s on
		 * this shape and seed. The amplifying phase runs past that, so
		 * the scenario reaches the reset it is here for.
		 *
		 * The line goes passive for the last two seconds. The
		 * amplifying phase alone leaves nothing to compare: it ends
		 * inside the hold that follows the trial, and probe_diverged
		 * restores the snapshot of zeroes taken before the trial. Both
		 * arms then end at coefficient energy zero, and the state
		 * comparison is two zeroes against a relative bound. Two
		 * seconds of a hybrid the canceller can solve end the run on a
		 * converged filter instead (energy 0.063, tracked ERLE
		 * 55.7 dB). That is a real comparison, and it leaves every
		 * decision of the first nine seconds where it was.
		 */
		.change_at = 1300, .change_erl_db = 12.0,
		/*
		 * The hold runs unbroken until the latch escape fires, and the
		 * latch is LEC_GEIGEL_LATCH_MAX double-talk frames of dsp/lec.c
		 * (500). The run cannot be shorter than that: the counter
		 * climbs at most once per frame and the hold ends only when the
		 * trial starts. The trial then adapts against an echo louder
		 * than its own reference, which is a solution no passive path
		 * has, so the divergence watchdog fires as well.
		 */
		.min_probes = 1, .min_resets = 1, .min_hold_run = 500,
		.coef_live = true,
	},
};

static void equivalence_test(void)
{
	unsigned count = sizeof(equiv_scenarios) / sizeof(equiv_scenarios[0]);
	enum udsp_ec_nlms_path outer = udsp_ec_nlms_path_get();
	struct equiv_fired fired;
	unsigned i;

	memset(&fired, 0, sizeof(fired));
	if (!udsp_ec_nlms_path_set(UDSP_EC_NLMS_PATH_SIMD)) {
		for (i = 0; i <= count; i++) {
			CASE(i < count ? equiv_scenarios[i].name :
					 EQUIV_COVER_CASE);
			printf("# skip: this build has the %s path only\n",
			       udsp_ec_nlms_path_name(udsp_ec_nlms_path_get()));
		}
		return;
	}
	for (i = 0; i < count; i++) {
		struct equiv_result result;

		equiv_run(&equiv_scenarios[i], &result);
		CASE(equiv_scenarios[i].name);
		/* The record the on-target run leaves behind: the bounds are
		 * asserted, the measurements are printed beside them. */
		printf("# %u taps, worst sample delta %u of %u counts, %u of %u frames differ, erle %.2f / %.2f dB\n",
		       result.scalar.taps, result.max_delta, EQUIV_DELTA_MAX,
		       result.diff_frames, equiv_scenarios[i].frames,
		       result.erle_scalar, result.erle_simd);
		printf("#   fired: %u probes, %u resets, %u holds (longest run %u), %u dt frames, %u nlp frames; state: coef %.2e, erle %.2e dB\n",
		       result.scalar.probes, result.scalar.diverge_resets,
		       result.scalar.hold_frames, result.max_hold_run,
		       result.dt_frames, result.nlp_frames,
		       rel_diff_get(result.scalar.coef_energy,
				    result.simd.coef_energy),
		       fabs((double)result.scalar.erle_db -
			    result.simd.erle_db));
		CHECK(result.max_delta <= EQUIV_DELTA_MAX);
		CHECK(fabs(result.erle_scalar - result.erle_simd) <=
		      EQUIV_ERLE_DELTA_DB);
		/* A decision that came out differently is not a rounding
		 * difference: it is a different canceller. Compared on the
		 * frame it happened, not as a total that offsets hide in. */
		CHECK_UINT(result.decision_frames, 0);
		CHECK_UINT(result.simd.probes, result.scalar.probes);
		CHECK_UINT(result.simd.diverge_resets,
			   result.scalar.diverge_resets);
		CHECK_UINT(result.simd.hold_frames, result.scalar.hold_frames);
		CHECK_UINT(result.simd.taps, result.scalar.taps);
		/* The state the next frame would run on, not only the samples
		 * this one emitted. */
		CHECK(rel_diff_get(result.scalar.coef_energy,
				   result.simd.coef_energy) <=
		      EQUIV_COEF_REL);
		CHECK(result.simd.echo_gain == result.scalar.echo_gain);
		CHECK(fabs((double)result.scalar.erle_db -
			   result.simd.erle_db) <= EQUIV_ERLE_STATE_DB);
		/* This scenario's own decisions, so it cannot go quiet and
		 * lean on another one's totals, and the state guard above
		 * cannot pass on a filter that holds nothing. */
		CHECK(result.scalar.probes >= equiv_scenarios[i].min_probes);
		CHECK(result.scalar.diverge_resets >=
		      equiv_scenarios[i].min_resets);
		CHECK(result.max_hold_run >= equiv_scenarios[i].min_hold_run);
		CHECK(!equiv_scenarios[i].coef_live || result.coef_live);
		fired.probes += result.scalar.probes;
		fired.resets += result.scalar.diverge_resets;
		fired.holds += result.scalar.hold_frames;
		fired.dt_frames += result.dt_frames;
		fired.nlp_frames += result.nlp_frames;
	}
	CASE(EQUIV_COVER_CASE);
	printf("# %u probes, %u resets, %u held frames, %u dt frames, %u nlp frames over %u scenarios\n",
	       fired.probes, fired.resets, fired.holds, fired.dt_frames,
	       fired.nlp_frames, count);
	CHECK(fired.probes > 0u);
	CHECK(fired.resets > 0u);
	CHECK(fired.holds > 0u);
	CHECK(fired.dt_frames > 0u);
	CHECK(fired.nlp_frames > 0u);
	udsp_ec_nlms_path_set(outer);
}

/*
 * The weak hybrid. The suite above and the per-path bench table validate on
 * hybrids of 0 to 23 dB whose microphone carries the echo and nothing else.
 * Real lines can return 26 to 30 dB, and their microphones carry the near
 * end's own noise floor as well. The echo of a quiet far end then comes within
 * about 10 dB of that floor.
 *
 * The row is specified by the ratio of echo to floor rather than by two
 * absolute levels, because the failure depends on how close the two come.
 *
 * Without the near-end floor term of the detector and the step control, at
 * 8 kHz, 41 percent of the far-active single-talk frames carry a double-talk
 * verdict with the near end silent. The filter then reaches 1.6 dB. The bars
 * below are the figures of the current canceller with margin. Each of them
 * fails without the floor term and the step control.
 */
#define WEAK_FAR_DBM0		-25.0
#define WEAK_FLOOR_DBM0		-55.0
#define WEAK_ERL_DB		28.0
#define WEAK_WINDOW		10u

/*
 * The burst bar. Without the step control, the worst window of talkover_test
 * reads +45.9 dB at 8 kHz. With it, the worst window reads +2.1 dB at 8 kHz
 * and +0.6 dB at 16 kHz, which is a filter at pass-through with nothing to
 * learn from. Three decibels separates those by an order of magnitude and
 * leaves the pass-through case room for the regression estimate's own noise.
 */
#define TALK_BURST_DB		3.0

struct weak_stat {
	unsigned active;
	unsigned adapt;
	unsigned flagged;
	unsigned windows;
	double worst_window;
	double echo;
	double residual;
	double floor;
};

/*
 * One frame through the weak hybrid with the line's own floor under it. near
 * is NULL for single talk. The statistics split on the canceller's own
 * activity boundary, so a frame the canceller ignores is not counted against
 * it.
 */
static void weak_step(struct udsp_ec *ec, struct echo_path *path,
		      struct word_source *far, struct word_source *near,
		      uint32_t *floor_state, struct weak_stat *st,
		      uint32_t *held, double *window, bool floor_quiet)
{
	int16_t ref[UDSP_FRAME_MAX];
	int16_t echo[UDSP_FRAME_MAX];
	int16_t talk[UDSP_FRAME_MAX];
	int16_t hiss[UDSP_FRAME_MAX];
	int16_t mic[UDSP_FRAME_MAX];
	int16_t out[UDSP_FRAME_MAX];
	struct udsp_ec_nlms_info info;
	unsigned frame = path->frame;
	double mic_energy = 0.0;
	unsigned i;

	word_fill(far, ref, frame);
	path_run(path, ref, echo);
	memset(talk, 0, frame * sizeof(*talk));
	if (near)
		word_fill(near, talk, frame);
	if (floor_quiet)
		memset(hiss, 0, frame * sizeof(*hiss));
	else
		noise_fill(hiss, frame, WEAK_FLOOR_DBM0, floor_state);
	for (i = 0; i < frame; i++)
		mic[i] = sample_clamp((double)echo[i] + talk[i] + hiss[i]);
	memcpy(out, mic, frame * sizeof(*mic));
	udsp_ec_nlms.run(ec, out, ref);
	udsp_ec_nlms_info_get(ec, &info);
	for (i = 0; i < frame; i++) {
		mic_energy += (double)mic[i] * mic[i];
		st->echo += (double)echo[i] * echo[i];
		st->residual += (double)out[i] * out[i];
		st->floor += (double)hiss[i] * hiss[i];
		window[0] += (double)out[i] * out[i];
	}
	window[1] += mic_energy;
	/* A near source in one of its own pauses is not double talk, and a
	 * frame the canceller reads as silent is not one it could learn
	 * from: the counters take the frames each verdict is about. */
	if (frame_loud(ref, frame) && (!near || frame_loud(talk, frame))) {
		st->active++;
		if (info.hold_frames == *held)
			st->adapt++;
		if (info.double_talk)
			st->flagged++;
	}
	*held = info.hold_frames;
}

/*
 * What the filter removed: the residual with the injected floor energy taken
 * out. The floor and the misalignment are uncorrelated, so their energies add
 * and the subtraction is unbiased. This is the number the acceptance is
 * stated in, because the delivered ratio on such a line is bounded by the
 * floor whatever the canceller does.
 */
static double weak_filter_erle_get(double echo, double residual, double floor)
{
	double left = residual - floor;

	if (left < 1e-6)
		return 200.0;
	return 10.0 * log10(echo / left);
}

static void weak_hybrid_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(rate == UDSP_SAMPLE_RATE ?
						      48u : 64u, rate);
	struct word_source far;
	struct word_source near;
	struct weak_stat single;
	struct weak_stat both;
	uint32_t floor_state = 0x0badcafeu;
	uint32_t held = 0;
	double window[2] = { 0.0, 0.0 };
	unsigned f;

	memset(&single, 0, sizeof(single));
	memset(&both, 0, sizeof(both));
	single.worst_window = 200.0;
	udsp_ec_nlms.nlp(ec, false);
	path_weak_init(&path, rate, WEAK_ERL_DB);
	word_source_init(&far, rate, WEAK_FAR_DBM0, 0x13572468u);
	word_source_init(&near, rate, WEAK_FAR_DBM0, 0xfeedfaceu);
	for (f = 0; f < 20u * FRAMES_PER_SECOND; f++) {
		weak_step(ec, &path, &far, NULL, &floor_state, &single, &held,
			  window, false);
		if ((f + 1u) % WEAK_WINDOW || f < 5u * FRAMES_PER_SECOND)
			continue;
		if (window[1] > 0.0 && window[0] > 0.0) {
			double db = 10.0 * log10(window[1] / window[0]);

			single.windows++;
			if (db < single.worst_window)
				single.worst_window = db;
		}
		window[0] = window[1] = 0.0;
	}

	case_rate(rate, "lec adapts through single talk on the weak hybrid");
	/* The far end alone is on the line, so every far-active frame is one
	 * the filter must be allowed to learn from. */
	CHECK(single.active > 500u);
	CHECK(single.adapt * 10u >= single.active * 9u);
	CHECK(single.flagged * 20u <= single.active);
	/* The canceller must not hand the line more energy than it found in
	 * any window of it: the far end hears that as a repeated syllable. */
	CHECK(single.windows > 50u);
	CHECK(single.worst_window >= -1.0);
	CHECK(weak_filter_erle_get(single.echo, single.residual,
				   single.floor) >= 8.0);

	/* The near end joins, at the far end's level, on the same hybrid and
	 * the same floor: the detector that stopped hearing the floor must
	 * still hear a talker. */
	for (f = 0; f < 10u * FRAMES_PER_SECOND; f++)
		weak_step(ec, &path, &far, &near, &floor_state, &both, &held,
			  window, false);

	case_rate(rate, "lec still holds for a near talker on the weak hybrid");
	CHECK(both.active > 100u);
	CHECK(both.flagged * 10u >= both.active * 8u);
	CHECK((both.active - both.adapt) * 10u >= both.active * 8u);
	/* No escape trial and therefore no divergence: the verdicts here are
	 * the true ones and the pauses of ordinary speech pay the run down. */
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

#define DIVERGE_AMP_DB		-8.0
#define DIVERGE_WORDS_FRAMES	(5u * FRAMES_PER_SECOND)
#define DIVERGE_AMP_FRAMES	(30u * FRAMES_PER_SECOND)

/*
 * The divergence line. Far-end words converge the filter on the weak hybrid
 * for 5 s. A flat hybrid that amplifies by 8 dB, the hybrid of
 * watchdog_recovery_test, then replaces it under a white far end at
 * WEAK_FAR_DBM0. Its echo stands 8 dB over the reference, so the talker guard
 * of dsp/lec.c reads it as echo, and no passive path can return it.
 */
struct divergence_line {
	struct echo_path weak;
	struct echo_path amp;
	struct word_source far;
	struct udsp_ec *ec;
	uint32_t state;
};

static void divergence_line_init(struct divergence_line *d, unsigned rate)
{
	d->ec = udsp_ec_nlms.create_rate(48, rate);
	udsp_ec_nlms.nlp(d->ec, false);
	path_weak_init(&d->weak, rate, WEAK_ERL_DB);
	path_flat_init(&d->amp, rate, 8, DIVERGE_AMP_DB);
	word_source_init(&d->far, rate, WEAK_FAR_DBM0, 0x13572468u);
	d->state = 0x13572468u;
}

/* Frame f of the divergence line. The hybrid that amplifies takes over on
 * frame DIVERGE_WORDS_FRAMES. */
static void divergence_frame_run(struct divergence_line *d, unsigned f)
{
	int16_t ref[UDSP_FRAME_MAX];
	int16_t mic[UDSP_FRAME_MAX];

	if (f < DIVERGE_WORDS_FRAMES) {
		word_fill(&d->far, ref, d->weak.frame);
		path_run(&d->weak, ref, mic);
		udsp_ec_nlms.run(d->ec, mic, ref);
		return;
	}
	if (f == DIVERGE_WORDS_FRAMES)
		path_history_carry(&d->amp, &d->weak);
	noise_fill(ref, d->amp.frame, WEAK_FAR_DBM0, &d->state);
	path_run(&d->amp, ref, mic);
	udsp_ec_nlms.run(d->ec, mic, ref);
}

/*
 * On the hybrid that amplifies, a latch trial drives the coefficients through
 * LEC_COEF_ENERGY_MAX, and probe_diverged of dsp/lec.c ends it through the
 * reject path. The reject restores the filter that converged on the weak
 * hybrid, so a zeroed filter reads a coefficient energy of 0. The far end runs
 * without a pause, because the pauses of speech decay the latch run. The line
 * must run a trial and a reset, must never leave the filter zeroed, and must
 * reset at most once per trial. A near talker cannot carry a trial through
 * the bound (the comment above probe_diverged in dsp/lec.c). Without the
 * abort, the watchdog zeroes the filter 15 times.
 */
static void probe_divergence_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct divergence_line d;
	unsigned zeroed = 0;
	bool alive = false;
	unsigned f;

	divergence_line_init(&d, rate);
	for (f = 0; f < DIVERGE_WORDS_FRAMES + DIVERGE_AMP_FRAMES; f++) {
		divergence_frame_run(&d, f);
		udsp_ec_nlms_info_get(d.ec, &info);
		if (info.coef_energy > 0.0f)
			alive = true;
		else if (alive)
			zeroed++;
	}
	udsp_ec_nlms.destroy(d.ec);

	case_rate(rate, "lec undoes a trial the watchdog stops");
	printf("# %u trials, %u resets, %u zeroed frames\n", info.probes,
	       info.diverge_resets, zeroed);
	/*
	 * The latch still fires, and it must: five unbroken seconds of a
	 * louder echo than any expectation is the documented design, and a
	 * trial is how the canceller asks whether the hold was right.
	 */
	CHECK(info.probes >= 1u);
	/*
	 * A trial is exempt from the step throttle (talk_scale_get says why),
	 * so it adapts at the full step toward the solution of the hybrid
	 * that amplifies, and this scenario reaches the fault it is written
	 * for.
	 */
	CHECK(info.diverge_resets >= 1u);
	/* The filter is never left dead under a running trial. */
	CHECK_UINT(zeroed, 0);
	/*
	 * And one trial can trip the watchdog at most once, because the trip
	 * ends the trial.
	 */
	CHECK(info.diverge_resets <= info.probes);
}

/*
 * The floor has one way in here: the first reference-silent frame is taken
 * whole. The seed of dsp/lec.c never moves a measured floor, and the rise
 * test guards only a floor that the tracker holds.
 * A near-end talkspurt in a far-end pause therefore sets the floor to the
 * level of that speech. The step control of dsp/lec.c reads that floor.
 * Without the branch that lets an active frame's residual falsify the floor,
 * the filter never adapts again while the far end keeps talking.
 * Measured over the run below: 5.4 dB of ERLE, against 59 dB with the branch.
 */
static void noise_floor_stale_test(void)
{
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x51ade0f1u;
	uint32_t near_state = 0x600dcafeu;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	/* One far-end pause carrying near-end speech, before any quiet pause
	 * has measured the real floor. */
	memset(ref, 0, sizeof(ref));
	noise_fill(mic, UDSP_FRAME_SAMPLES, -10.0, &near_state);
	udsp_ec_nlms.run(ec, mic, ref);

	CASE("lec drops a near-end floor the residual falsifies");
	CHECK(erle_run(ec, &path, &state, -10.0,
		       FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20) >= 45.0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * One quiet frame is not evidence. The residual of an active frame bounds the
 * near-end floor from above, so a residual under the floor falsifies it. But a
 * 10 ms residual is one realization of a near end that is not stationary. If
 * a single quiet frame throws a valid floor away, the cost is high. The floor
 * is only measured on a reference-silent frame, so a line whose far end keeps
 * talking never gets it back. The two terms that read the floor (the
 * detector's near-end term and the step control) then act as on a line with
 * no floor.
 *
 * The case runs the weak line of weak_hybrid_test with a quiet moment in the
 * near end every tenth frame, each one isolated, and asks for the behavior
 * that a line with a floor shows. LEC_NOISE_STALE_FRAMES of dsp/lec.c is what
 * separates them: with it at 1, the verdicts come back on 37 percent of the
 * far-active frames.
 */
#define WEAK_OUTLIER_EVERY	10u

static void noise_floor_outlier_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
	struct word_source far;
	struct weak_stat st;
	uint32_t floor_state = 0x0badcafeu;
	uint32_t held = 0;
	double window[2] = { 0.0, 0.0 };
	unsigned f;

	memset(&st, 0, sizeof(st));
	udsp_ec_nlms.nlp(ec, false);
	path_weak_init(&path, rate, WEAK_ERL_DB);
	word_source_init(&far, rate, WEAK_FAR_DBM0, 0x13572468u);
	for (f = 0; f < 20u * FRAMES_PER_SECOND; f++) {
		/* The quiet moment goes on a frame the far end is talking on,
		 * which is the frame the falsification arm reads. A quiet
		 * moment in a far-end pause is not an outlier at all: it is a
		 * measurement of a floor that really is lower, and the tracker
		 * is supposed to follow it. */
		bool quiet = f % WEAK_OUTLIER_EVERY == 0u && far.talking;

		weak_step(ec, &path, &far, NULL, &floor_state, &st, &held,
			  window, quiet);
	}
	udsp_ec_nlms_info_get(ec, &info);

	case_rate(rate, "lec keeps a floor a single quiet frame contradicts");
	CHECK(st.active > 500u);
	CHECK(st.adapt * 10u >= st.active * 9u);
	CHECK(st.flagged * 20u <= st.active);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * A hybrid that amplifies is the one case where the filter estimate justifies
 * the microphone it chases, so the runaway is real. Every reset here is a
 * trial abort through probe_diverged: the detector holds this hybrid from its
 * first frame, so the runaway happens inside a trial. An instrumented run of
 * the whole suite counts 15 trial aborts and 0 resets through the plain zero
 * branch of divergence_check. That branch and its re-seed of the live power
 * trackers are the defensive policy for a divergence with no snapshot.
 *
 * The case asserts that no published field is ever not a number, and that one
 * trial cannot trip the watchdog twice. It also asserts that the canceller
 * keeps learning afterwards.
 */
static void watchdog_recovery_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x0c0ffee1u;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t echo[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	struct echo_path passive;
	unsigned finite_bad = 0;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, -8.0);
	path_flat_init(&passive, UDSP_SAMPLE_RATE, 8, 12.0);
	for (f = 0; f < 20u * FRAMES_PER_SECOND; f++) {
		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
		path_run(&path, ref, echo);
		memcpy(mic, echo, sizeof(mic));
		udsp_ec_nlms.run(ec, mic, ref);
		udsp_ec_nlms_info_get(ec, &info);
		if (!isfinite((double)info.coef_energy) ||
		    !isfinite((double)info.erle_db) ||
		    !isfinite((double)info.echo_gain))
			finite_bad++;
	}

	CASE("lec recovers from the watchdog with every tracker finite");
	/* The scenario has to reach the branch it is about. */
	CHECK(info.diverge_resets >= 1u);
	CHECK_UINT(finite_bad, 0);
	/*
	 * On this hybrid a divergence stays reachable under the step
	 * control. The microphone really does carry what the filter is
	 * chasing, so the step control has nothing to withhold and the
	 * runaway is the genuine one the watchdog exists for. No talk-over
	 * case reaches the watchdog, so this case pins the undo path of the
	 * trial. Every trip inside a trial ends that trial and puts the
	 * snapshot back, so one trial cannot trip the watchdog twice. The
	 * snapshot here is a filter of zeroes, because the detector holds
	 * this hybrid from its first frame, so a zero reading of the
	 * coefficient energy is the restore and not a zeroing.
	 */
	CHECK(info.diverge_resets <= info.probes);
	/*
	 * The line goes passive: a canceller carrying poison in either live
	 * power would take the zero step for ever and cancel nothing, so the
	 * recovery bar is itself the test of the re-seed. Two seconds is the
	 * bound.
	 */
	path_history_carry(&passive, &path);
	CHECK(erle_run(ec, &passive, &state, -10.0, 2u * FRAMES_PER_SECOND,
		       20) >= 25.0);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK(info.coef_energy > 0.0f);
	CHECK(isfinite((double)info.coef_energy));
	udsp_ec_nlms.destroy(ec);
}

/*
 * The frame the trial's countdown expires on. If ec_run resolves the
 * countdown before probe_diverged, a divergence there escapes the abort.
 * probe_finish then judges the diverged coefficients on a smoothed residual
 * that lags a divergent frame by a tenth. It can declare them an improvement
 * and keep them. The plain watchdog behind it then finds no trial open and
 * zeroes the line's filter.
 *
 * The case walks the boundary exactly: it counts the trial's budget in the
 * currency the trial spends, active frames, and puts the divergence on the
 * last one. A talker cannot cause that divergence. A full-scale talker on a
 * reference near the activity bar stands 47 to 48 dB over its window. The
 * talker guard of dsp/lec.c ends the trial before that frame adapts. A
 * talker under the guard does not carry the filter through the bound
 * (probe_divergence_test).
 *
 * The divergence therefore comes from the hybrid that amplifies by 8 dB,
 * which the guard reads as echo. A latch trial on the weak hybrid spends its
 * budget on the far end alone, and the reference then goes silent for
 * TRIAL_FLUSH_FRAMES frames. Those frames are inactive, so they spend no
 * budget, and they clear the history of the hybrid and of the canceller. The
 * hybrid that amplifies then takes the white far end of a fixed seed until
 * the coefficients cross the bound. A scout line counts those frames first,
 * with no budget spent before them, and the line spends the rest of the
 * budget before the silence. The crossing then falls on the last frame.
 *
 * A second instance is the independent check of what the abort had to
 * restore. It sees every frame this one sees, and the case freezes it on the
 * frame the trial starts, which is a frame the escape holds and therefore one
 * that changes no coefficient; its filter from then on is the snapshot. Two
 * frozen instances on identical frames produce identical output if and only
 * if they hold identical coefficients, so the comparison is a byte compare of
 * the filters through the only door the public interface has.
 */
#define TRIAL_BUDGET		100u
#define TRIAL_QUIET_DBM0	-48.0
#define TRIAL_LATCH_DBM0	-16.0
#define TRIAL_AMP_DB		-8.0
#define TRIAL_FLUSH_FRAMES	10u
#define TRIAL_AMP_SEED		0x0c0ffee1u

/* One frame to both instances, and the echo the hybrid returned. mic is
 * written in place by the canceller, so each arm gets its own copy. */
static void twin_step(struct udsp_ec *lead, struct udsp_ec *twin,
		      struct echo_path *path, const int16_t *ref,
		      const int16_t *near, int16_t *lead_out,
		      int16_t *twin_out, double *mic_energy)
{
	int16_t echo[UDSP_FRAME_MAX];
	unsigned i;

	path_run(path, ref, echo);
	for (i = 0; i < path->frame; i++)
		lead_out[i] = near ? sample_clamp((double)echo[i] + near[i]) :
				     echo[i];
	/* The microphone as the canceller receives it, before run() writes
	 * the residual over it. */
	if (mic_energy) {
		for (i = 0; i < path->frame; i++)
			*mic_energy += (double)lead_out[i] * lead_out[i];
	}
	memcpy(twin_out, lead_out, path->frame * sizeof(*lead_out));
	udsp_ec_nlms.run(lead, lead_out, ref);
	udsp_ec_nlms.run(twin, twin_out, ref);
}

/* The two instances of the boundary walk, the weak hybrid, the hybrid that
 * amplifies, and the outputs of the last frame. */
struct boundary_line {
	struct echo_path weak;
	struct echo_path amp;
	struct udsp_ec *lead;
	struct udsp_ec *twin;
	unsigned frame;
	uint32_t state;
	uint32_t near_state;
	uint32_t amp_state;
	int16_t lead_out[UDSP_FRAME_MAX];
	int16_t twin_out[UDSP_FRAME_MAX];
};

static void boundary_line_init(struct boundary_line *b, unsigned rate)
{
	memset(b, 0, sizeof(*b));
	b->lead = udsp_ec_nlms.create_rate(48, rate);
	b->twin = udsp_ec_nlms.create_rate(48, rate);
	b->frame = rate / 100u;
	b->state = 0x5eed10c5u;
	b->near_state = 0x0badf00du;
	udsp_ec_nlms.nlp(b->lead, false);
	udsp_ec_nlms.nlp(b->twin, false);
	path_weak_init(&b->weak, rate, WEAK_ERL_DB);
	path_flat_init(&b->amp, rate, 8, TRIAL_AMP_DB);
}

/* One frame of the far end on the weak hybrid, with a white talker at
 * TRIAL_LATCH_DBM0 when talk is set. */
static void boundary_weak_run(struct boundary_line *b, bool talk)
{
	int16_t ref[UDSP_FRAME_MAX];
	int16_t near[UDSP_FRAME_MAX];

	noise_fill(ref, b->frame, WEAK_FAR_DBM0, &b->state);
	if (talk)
		noise_fill(near, b->frame, TRIAL_LATCH_DBM0, &b->near_state);
	twin_step(b->lead, b->twin, &b->weak, ref, talk ? near : NULL,
		  b->lead_out, b->twin_out, NULL);
}

/*
 * Converge for 2 s, so the filter the abort has to give back is a real one.
 * A talker 9 dB over the far end then joins and stays: the run the clear
 * frames cannot pay down reaches LEC_GEIGEL_LATCH_MAX, and the latch starts a
 * trial. The talker stands under the talker guard. The frame the trial starts
 * on is a double-talk frame, so it adapts on nothing. The twin freezes on
 * that frame, and its coefficients are the snapshot from then on.
 */
static void boundary_trial_start(struct boundary_line *b,
				 struct udsp_ec_nlms_info *converged)
{
	struct udsp_ec_nlms_info info;
	unsigned f;

	for (f = 0; f < 2u * FRAMES_PER_SECOND; f++)
		boundary_weak_run(b, false);
	udsp_ec_nlms_info_get(b->lead, converged);
	for (f = 0; f < 20u * FRAMES_PER_SECOND; f++) {
		boundary_weak_run(b, true);
		udsp_ec_nlms_info_get(b->lead, &info);
		if (info.probes)
			break;
	}
	udsp_ec_nlms.freeze(b->twin, true);
}

/* The silent reference of TRIAL_FLUSH_FRAMES frames, and the hybrid that
 * amplifies after it, with its far end from TRIAL_AMP_SEED. */
static void boundary_flush_run(struct boundary_line *b)
{
	int16_t ref[UDSP_FRAME_MAX];
	unsigned f;

	memset(ref, 0, sizeof(ref));
	for (f = 0; f < TRIAL_FLUSH_FRAMES; f++)
		twin_step(b->lead, b->twin, &b->weak, ref, NULL, b->lead_out,
			  b->twin_out, NULL);
	path_history_carry(&b->amp, &b->weak);
	b->amp_state = TRIAL_AMP_SEED;
}

/* One frame of the white far end on the hybrid that amplifies. It returns
 * the talker verdict of the frame. */
static bool boundary_amp_run(struct boundary_line *b, double *mic_energy)
{
	struct udsp_ec_nlms_frame reading;
	int16_t ref[UDSP_FRAME_MAX];

	noise_fill(ref, b->frame, WEAK_FAR_DBM0, &b->amp_state);
	twin_step(b->lead, b->twin, &b->amp, ref, NULL, b->lead_out,
		  b->twin_out, mic_energy);
	udsp_ec_nlms_frame_get(b->lead, &reading);
	return reading.talker;
}

/* The frames of the hybrid that amplifies that a trial with its whole budget
 * left needs to cross the bound, or 0 when it does not cross it. */
static unsigned boundary_amp_count(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct boundary_line b;
	unsigned count = 0;
	unsigned f;

	boundary_line_init(&b, rate);
	boundary_trial_start(&b, &info);
	boundary_flush_run(&b);
	for (f = 1; f <= TRIAL_BUDGET && !count; f++) {
		boundary_amp_run(&b, NULL);
		udsp_ec_nlms_info_get(b.lead, &info);
		if (info.diverge_resets)
			count = f;
	}
	udsp_ec_nlms.destroy(b.lead);
	udsp_ec_nlms.destroy(b.twin);
	return count;
}

static void probe_boundary_test(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info lead_info;
	struct udsp_ec_nlms_info twin_info;
	struct boundary_line b;
	unsigned amp_frames = boundary_amp_count(rate);
	unsigned spent = 0;
	unsigned talker = 0;
	unsigned differ = 0;
	uint32_t held;
	double last_mic = 0.0;
	double last_out = 0.0;
	unsigned f;

	case_rate(rate, "lec aborts a trial that blows up on its last frame");
	CHECK(amp_frames > 1u);
	boundary_line_init(&b, rate);
	boundary_trial_start(&b, &lead_info);
	CHECK(lead_info.erle_db >= 18.0f);
	CHECK(lead_info.coef_energy > 0.0f);
	CHECK_UINT(lead_info.probes, 0);
	udsp_ec_nlms_info_get(b.lead, &lead_info);
	CHECK_UINT(lead_info.probes, 1);
	CHECK(lead_info.coef_energy > 0.0f);
	held = lead_info.hold_frames;

	/* The budget is spent in active frames: the far end alone first, and
	 * the hybrid that amplifies after the silence. */
	while (spent + amp_frames < TRIAL_BUDGET) {
		boundary_weak_run(&b, false);
		spent++;
	}
	boundary_flush_run(&b);
	for (f = 1; f < amp_frames; f++) {
		talker += boundary_amp_run(&b, NULL);
		spent++;
	}
	udsp_ec_nlms_info_get(b.lead, &lead_info);
	/* Only the silent frames held, so the trial adapted on the other
	 * TRIAL_BUDGET - 1 frames and has exactly one left. */
	CHECK_UINT(lead_info.hold_frames - held, TRIAL_FLUSH_FRAMES);
	CHECK_UINT(lead_info.diverge_resets, 0);
	CHECK_UINT(lead_info.probes, 1);

	talker += boundary_amp_run(&b, &last_mic);
	for (f = 0; f < b.frame; f++)
		last_out += (double)b.lead_out[f] * b.lead_out[f];
	udsp_ec_nlms_frame_get(b.lead, &reading);
	udsp_ec_nlms_info_get(b.lead, &lead_info);
	udsp_ec_nlms_info_get(b.twin, &twin_info);
	printf("# %u frames of the hybrid that amplifies cross the bound, "
	       "after %u of the far end alone; %u talker frames among them; "
	       "the last frame costs %.4f dB\n", amp_frames,
	       TRIAL_BUDGET - amp_frames, talker,
	       10.0 * log10(last_out / last_mic));

	/* The guard reads the louder echo as echo, not as a talker. */
	CHECK_UINT(talker, 0);
	/* The watchdog saw it, and it left the line's filter in place. */
	CHECK_UINT(lead_info.diverge_resets, 1);
	CHECK_UINT(reading.trial_end, UDSP_EC_NLMS_TRIAL_END_DIVERGED);
	CHECK(lead_info.coef_energy > 0.0f);
	/*
	 * What the line heard of that frame. The abort undoes the update, so
	 * the filter comparison below cannot see the one thing the frame
	 * really cost: the output, which was computed while the coefficients
	 * were diverging. That output is the only part of a reverted frame
	 * the far end can hear, and it is required to stay under the
	 * microphone that produced it. A filter running away adds energy, so
	 * this is the assertion that a reverted frame is not a burst, against
	 * the same bar the talk-over case calls one.
	 */
	CHECK(last_out <= last_mic * pow(10.0, TALK_BURST_DB / 10.0));
	/* The published energy is the restored filter's, to the bit: the twin
	 * holds that filter and computes it the same way. */
	CHECK(lead_info.coef_energy == twin_info.coef_energy);

	/* And the coefficients themselves are the snapshot, sample for
	 * sample, on frames neither instance can adapt on. */
	udsp_ec_nlms.freeze(b.lead, true);
	path_history_carry(&b.weak, &b.amp);
	for (f = 0; f < 20u; f++) {
		boundary_weak_run(&b, false);
		if (memcmp(b.lead_out, b.twin_out,
			   b.frame * sizeof(*b.lead_out)))
			differ++;
	}
	CHECK_UINT(differ, 0);

	/* The trial is over, not paused: the run counters went back to zero
	 * and the escape stood down, so no second trial follows it. */
	udsp_ec_nlms.freeze(b.lead, false);
	for (f = 0; f < 3u * FRAMES_PER_SECOND; f++)
		boundary_weak_run(&b, false);
	udsp_ec_nlms_info_get(b.lead, &lead_info);
	CHECK_UINT(lead_info.probes, 1);
	CHECK_UINT(lead_info.diverge_resets, 1);
	udsp_ec_nlms.destroy(b.lead);
	udsp_ec_nlms.destroy(b.twin);
}

/*
 * The hybrid that got louder. A line whose return loss steps from 23 dB to
 * 3 dB is the one case the learned gain cannot describe: the echo is twenty
 * decibels over anything the tracker has seen, the detector holds every frame
 * because the microphone exceeds every expectation it can form, the release
 * arm never opens because it only runs on clean frames, and path_change_get
 * cannot fire because a step in gain alone leaves the residual under its
 * ratio. The latch escape and its trial are the only way out, and they exist
 * for this case.
 *
 * The trial therefore cannot be throttled by the learned gain, or the
 * hypothesis vetoes its own test. With the step control applied to trial
 * frames, ten trials fire in sixty seconds and each fails its improvement
 * test. With the trial exempt, the line recovers in 5.5 s at both rates.
 *
 * The bound is the mechanism, not slack. The recovery cannot begin before the
 * latch does, and the latch is LEC_GEIGEL_LATCH_MAX active frames of
 * dsp/lec.c; the trial that follows carries the line past the bar inside two
 * measurement blocks. Measured at frame 550 at both rates on all twelve seeds
 * of the stride list, with no spread at all, because the latch is a frame
 * count and not a function of the noise. The bound is therefore the latch
 * plus three blocks, one block over the measurement, because the only thing a
 * different seed can move is which block first reads over the bar.
 */
#define STEP_FROM_DB		23.0
#define STEP_TO_DB		3.0
#define STEP_BAR_DB		10.0
#define STEP_BLOCK_FRAMES	25u
/* LEC_GEIGEL_LATCH_MAX of dsp/lec.c: the run the clear frames cannot pay
 * down, and the earliest this recovery can begin. */
#define STEP_LATCH_FRAMES	500u
#define STEP_SETTLE_MAX		(STEP_LATCH_FRAMES + 3u * STEP_BLOCK_FRAMES)
#define STEP_LIMIT_FRAMES	800u

static void louder_hybrid_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct echo_path first;
	struct echo_path second;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
	unsigned frame = rate / 100u;
	uint32_t state = 0x13572468u;
	double echo_energy = 0.0;
	double res_energy = 0.0;
	unsigned settled = 0;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&first, rate, 8, STEP_FROM_DB);
	path_flat_init(&second, rate, 8, STEP_TO_DB);
	CHECK(erle_run(ec, &first, &state, -10.0, 3u * FRAMES_PER_SECOND,
		       20) >= 25.0);
	path_history_carry(&second, &first);
	for (f = 0; f < STEP_LIMIT_FRAMES; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		unsigned i;

		noise_fill(ref, frame, -10.0, &state);
		path_run(&second, ref, echo);
		memcpy(mic, echo, frame * sizeof(*mic));
		udsp_ec_nlms.run(ec, mic, ref);
		for (i = 0; i < frame; i++) {
			echo_energy += (double)echo[i] * echo[i];
			res_energy += (double)mic[i] * mic[i];
		}
		if ((f + 1u) % STEP_BLOCK_FRAMES)
			continue;
		if (!settled && res_energy > 0.0 &&
		    10.0 * log10(echo_energy / res_energy) >= STEP_BAR_DB)
			settled = f + 1u;
		echo_energy = res_energy = 0.0;
	}
	udsp_ec_nlms_info_get(ec, &info);

	case_rate(rate, "lec recovers a hybrid that got twenty dB louder");
	/* The escape is the only way out of this one, so it has to fire. */
	CHECK(info.probes >= 1u);
	CHECK(settled > 0u);
	CHECK(settled <= STEP_SETTLE_MAX);
	CHECK(info.erle_db >= 25.0f);
	udsp_ec_nlms.destroy(ec);
}

/*
 * Talk-over. Without the step control, a line holds in single talk and does
 * not hold in talk-over: each watchdog reset falls in an interval with a
 * talk-over second in it. On the bench without the step control, each such
 * reset comes from the plain zero branch, not a trial.
 * Each frame that adapts while the near end talks passes the estimate arm of
 * the detector at nearly full step.
 *
 * One such frame is enough. At the levels of a real call the near end stands
 * about 15 dB over the echo, so a single full-step frame moves the
 * coefficient energy by two orders of magnitude, and the frames after it are
 * hidden by the estimate arm the contamination itself raised.
 *
 * The case runs both talkers as words at the levels a real call has: a duet
 * first, where the two never fall out of step, and then the same two skewed
 * against each other, which is what talk-over is. Without the step control,
 * the two phases give 24 resets at 8 kHz.
 */
#define TALK_NEAR_DBM0		-10.0
#define TALK_SKEW		15u
#define TALK_DUET_FRAMES	2000u
#define TALK_SKEWED_FRAMES	4000u
#define TALK_WINDOW		50u
#define TALK_WINDOW_MIN		10u

struct talk_stat {
	unsigned overlap;
	unsigned clean;
	unsigned clean_adapt;
	unsigned windows;
	unsigned zeroed;
	double worst_window;
	double win[2];
	unsigned win_n;
	unsigned win_used;
	bool alive;
};

/*
 * One frame with both talkers on the line. The echo window is the regression
 * gain of the output on the injected echo over half a second in which the
 * near end is silent, where the far caller hears their
 * own echo in the clear and a regression through near speech would measure
 * the near speech.
 */
static void talk_step(struct udsp_ec *ec, struct echo_path *path,
		      struct word_source *far, struct word_source *near,
		      uint32_t *floor_state, struct talk_stat *st,
		      uint32_t *held)
{
	int16_t ref[UDSP_FRAME_MAX];
	int16_t echo[UDSP_FRAME_MAX];
	int16_t talk[UDSP_FRAME_MAX];
	int16_t hiss[UDSP_FRAME_MAX];
	int16_t mic[UDSP_FRAME_MAX];
	struct udsp_ec_nlms_info info;
	unsigned frame = path->frame;
	double echo_energy = 0.0;
	double product = 0.0;
	bool far_on;
	bool near_on;
	unsigned i;

	word_fill(far, ref, frame);
	word_fill(near, talk, frame);
	path_run(path, ref, echo);
	noise_fill(hiss, frame, WEAK_FLOOR_DBM0, floor_state);
	for (i = 0; i < frame; i++)
		mic[i] = sample_clamp((double)echo[i] + talk[i] + hiss[i]);
	far_on = frame_loud(ref, frame);
	near_on = frame_loud(talk, frame);
	udsp_ec_nlms.run(ec, mic, ref);
	udsp_ec_nlms_info_get(ec, &info);
	if (info.coef_energy > 0.0f)
		st->alive = true;
	else if (st->alive)
		st->zeroed++;
	for (i = 0; i < frame; i++) {
		echo_energy += (double)echo[i] * echo[i];
		product += (double)mic[i] * echo[i];
	}
	if (far_on && near_on)
		st->overlap++;
	if (far_on && !near_on) {
		st->clean++;
		if (info.hold_frames == *held)
			st->clean_adapt++;
		st->win[0] += product;
		st->win[1] += echo_energy;
		st->win_used++;
	}
	*held = info.hold_frames;
	if (++st->win_n < TALK_WINDOW)
		return;
	if (st->win_used >= TALK_WINDOW_MIN && st->win[1] > 0.0) {
		double gain = 20.0 * log10(fabs(st->win[0] / st->win[1]) +
					   1e-12);

		st->windows++;
		if (gain > st->worst_window)
			st->worst_window = gain;
	}
	st->win_n = 0;
	st->win_used = 0;
	st->win[0] = st->win[1] = 0.0;
}

static void talkover_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
	struct word_source far;
	struct word_source near;
	struct talk_stat st;
	uint32_t floor_state = 0x0badcafeu;
	uint32_t state = 0x51ade0f1u;
	uint32_t held = 0;
	unsigned f;

	memset(&st, 0, sizeof(st));
	st.worst_window = -200.0;
	udsp_ec_nlms.nlp(ec, false);
	path_weak_init(&path, rate, WEAK_ERL_DB);
	word_source_init(&far, rate, WEAK_FAR_DBM0, 0x13572468u);
	word_source_init(&near, rate, TALK_NEAR_DBM0, 0xfeedfaceu);
	for (f = 0; f < TALK_DUET_FRAMES; f++)
		talk_step(ec, &path, &far, &near, &floor_state, &st, &held);
	/* The talkers fall out of step, which is what talk-over is: a partial
	 * overlap with clean far-end frames between the collisions. */
	for (f = 0; f < TALK_SKEW; f++) {
		int16_t skip[UDSP_FRAME_MAX];

		word_fill(&near, skip, rate / 100u);
	}
	for (f = 0; f < TALK_SKEWED_FRAMES; f++)
		talk_step(ec, &path, &far, &near, &floor_state, &st, &held);
	udsp_ec_nlms_info_get(ec, &info);

	case_rate(rate, "lec holds the filter through talk-over");
	/* The scenario has to reach the collisions it is about. */
	CHECK(st.overlap > 500u);
	CHECK(st.windows > 50u);
	CHECK_UINT(info.diverge_resets, 0);
	CHECK_UINT(st.zeroed, 0);
	/* And it must never hand the far end more of their own voice back
	 * than the hybrid returned. */
	CHECK(st.worst_window <= TALK_BURST_DB);
	/*
	 * The learned gain must still describe the hybrid after all that, not
	 * the talker. A passive line cannot return more than it receives, so
	 * a power gain at or above unity is a tracker that has been reading
	 * near-end speech: the release arm of echo_gain_update runs on clean
	 * frames only for exactly this reason, and without that gate the
	 * tracker climbs to 5.74 here, at both rates, against the 0.376 it
	 * holds with it. The line statistics report this value as
	 * echo_gain_micro.
	 */
	CHECK(info.echo_gain < 1.0f);
	/*
	 * The talk-over ends. A control that answers a talker by refusing to
	 * learn has to hand the line back afterwards, so the filter is
	 * required to be worth having again inside a second and a half.
	 */
	CHECK(erle_run(ec, &path, &state, WEAK_FAR_DBM0,
		       3u * FRAMES_PER_SECOND / 2u, 20) >= 6.0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * --path runs the whole suite on one form of the per-tap loops, which is how
 * the behavioral oracles are proved to pass identically on either. Without
 * it, the suite runs the form that the target selects by default.
 */
static bool path_select(int argc, char **argv)
{
	enum udsp_ec_nlms_path path;

	if (argc == 1)
		return true;
	if (argc == 3 && !strcmp(argv[1], "--path")) {
		for (path = UDSP_EC_NLMS_PATH_SCALAR;
		     path <= UDSP_EC_NLMS_PATH_SIMD; path++) {
			if (strcmp(argv[2], udsp_ec_nlms_path_name(path)))
				continue;
			if (udsp_ec_nlms_path_set(path))
				return true;
			fprintf(stderr,
				"test_lec: this build has no %s path\n",
				argv[2]);
			return false;
		}
	}
	fprintf(stderr, "usage: %s [--path scalar|simd]\n", argv[0]);
	return false;
}

static bool zero_skip_info_same(const struct udsp_ec_nlms_info *a,
				const struct udsp_ec_nlms_info *b)
{
	return a->tail_ms == b->tail_ms && a->taps == b->taps &&
	       a->frames == b->frames && a->hold_frames == b->hold_frames &&
	       a->diverge_resets == b->diverge_resets &&
	       a->probes == b->probes && a->coef_energy == b->coef_energy &&
	       a->erle_db == b->erle_db && a->echo_gain == b->echo_gain &&
	       a->double_talk == b->double_talk &&
	       a->nlp_active == b->nlp_active &&
	       a->nlp_active_frames == b->nlp_active_frames &&
	       a->nlp_wait_frames == b->nlp_wait_frames &&
	       a->nlp_erle_frames == b->nlp_erle_frames &&
	       a->nlp_unlike_frames == b->nlp_unlike_frames &&
	       a->nlp_dt_frames == b->nlp_dt_frames &&
	       a->nlp_quiet_frames == b->nlp_quiet_frames &&
	       a->coef_peak_tap == b->coef_peak_tap &&
	       a->noise_dbov == b->noise_dbov &&
	       a->nlp_dt_fired_frames == b->nlp_dt_fired_frames &&
	       a->ref_floor == b->ref_floor;
}

/*
 * A frame whose whole reference window is zero skips the tap loops (dsp/lec.c
 * frame_zero_run), on the claim that the skipped work produces nothing. The
 * two forms therefore run side by side over one stimulus, with every emitted
 * sample and state field compared bit for bit on every frame. The run
 * converges on a hybrid and falls silent for a second, so the window drains.
 * It resumes, falls silent again under near-end speech, and resumes, with the
 * NLP on so that the comfort noise path runs through the silence too. The skip
 * count must match the geometry: a pause of P frames skips P minus the frames
 * that the window needs to drain. The full form skips none.
 */
static void zero_skip_test(unsigned rate)
{
	struct udsp_ec *full = udsp_ec_nlms.create_rate(48, rate);
	struct udsp_ec *gated = udsp_ec_nlms.create_rate(48, rate);
	struct udsp_ec_nlms_info full_info;
	struct udsp_ec_nlms_info gated_info;
	struct echo_path path;
	uint32_t state = 0x5eed0123u ^ rate;
	uint32_t near_state = 0x0badbeefu;
	unsigned frame = rate / 100u;
	unsigned diff_frames = 0;
	unsigned state_frames = 0;
	unsigned expect_skip = 0;
	unsigned drain;
	unsigned f;

	udsp_ec_nlms.nlp(full, true);
	udsp_ec_nlms.nlp(gated, true);
	udsp_ec_nlms_zero_skip_set(full, false);
	udsp_ec_nlms_zero_skip_set(gated, true);
	path_composite_init(&path, rate, 12.0);
	udsp_ec_nlms_info_get(full, &full_info);
	drain = (full_info.taps + frame - 1u) / frame;
	for (f = 0; f < 500u; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		int16_t full_out[UDSP_FRAME_MAX];
		int16_t gated_out[UDSP_FRAME_MAX];
		bool pause = (f >= 200u && f < 300u) || (f >= 400u && f < 450u);
		unsigned pause_at = f >= 400u ? f - 400u : f - 200u;
		unsigned i;

		if (pause) {
			memset(ref, 0, frame * sizeof(*ref));
			if (pause_at >= drain)
				expect_skip++;
		} else {
			noise_fill(ref, frame, -10.0, &state);
		}
		path_run(&path, ref, mic);
		if (f >= 400u && f < 450u) {
			int16_t near[UDSP_FRAME_MAX];

			noise_fill(near, frame, -19.0, &near_state);
			for (i = 0; i < frame; i++)
				mic[i] = sample_clamp((double)mic[i] + near[i]);
		}
		memcpy(full_out, mic, frame * sizeof(*mic));
		memcpy(gated_out, mic, frame * sizeof(*mic));
		udsp_ec_nlms.run(full, full_out, ref);
		udsp_ec_nlms_info_get(full, &full_info);
		udsp_ec_nlms.run(gated, gated_out, ref);
		udsp_ec_nlms_info_get(gated, &gated_info);
		if (memcmp(full_out, gated_out, frame * sizeof(*mic)))
			diff_frames++;
		if (!zero_skip_info_same(&full_info, &gated_info))
			state_frames++;
	}

	case_rate(rate, "lec zero-reference skip matches the full form bit for bit");
	printf("# %u frames differ, %u states differ, %u of %u frames skipped, coef %.3e\n",
	       diff_frames, state_frames, gated_info.skip_frames,
	       gated_info.frames, (double)gated_info.coef_energy);
	CHECK_UINT(diff_frames, 0);
	CHECK_UINT(state_frames, 0);
	CHECK_UINT(gated_info.skip_frames, expect_skip);
	CHECK_UINT(expect_skip, 150u - 2u * drain);
	CHECK_UINT(full_info.skip_frames, 0);
	CHECK(gated_info.hold_frames >= gated_info.skip_frames);
	/* A comparison of two idle filters would pass on nothing. */
	CHECK(gated_info.coef_energy > 0.0f);
	udsp_ec_nlms.destroy(full);
	udsp_ec_nlms.destroy(gated);
}

static uint32_t verdict_sum_get(const struct udsp_ec_nlms_info *info)
{
	return info->nlp_active_frames + info->nlp_wait_frames +
	       info->nlp_erle_frames + info->nlp_unlike_frames +
	       info->nlp_dt_frames + info->nlp_quiet_frames;
}

/*
 * One frame through the hybrid and the canceller, with the near talker on top
 * of the echo when there is one. out takes the output of the frame. acted
 * counts the frames that read nlp_active, the per-frame view that
 * nlp_active_frames must match.
 */
static void verdict_out_step(struct udsp_ec *ec, struct echo_path *path,
			     const int16_t *ref, const int16_t *near,
			     int16_t *out, unsigned *acted)
{
	struct udsp_ec_nlms_info info;
	unsigned i;

	path_run(path, ref, out);
	if (near) {
		for (i = 0; i < path->frame; i++)
			out[i] = sample_clamp((double)out[i] + near[i]);
	}
	udsp_ec_nlms.run(ec, out, ref);
	udsp_ec_nlms_info_get(ec, &info);
	if (info.nlp_active)
		(*acted)++;
}

static void verdict_step(struct udsp_ec *ec, struct echo_path *path,
			 const int16_t *ref, const int16_t *near,
			 unsigned *acted)
{
	int16_t out[UDSP_FRAME_MAX];

	verdict_out_step(ec, path, ref, near, out, acted);
}

static void verdict_far_run(struct udsp_ec *ec, struct echo_path *path,
			    uint32_t *state, unsigned frames, unsigned *acted)
{
	int16_t ref[UDSP_FRAME_MAX];
	unsigned f;

	for (f = 0; f < frames; f++) {
		noise_fill(ref, path->frame, -10.0, state);
		verdict_step(ec, path, ref, NULL, acted);
	}
}

static void verdict_silence_run(struct udsp_ec *ec, struct echo_path *path,
				unsigned frames, unsigned *acted)
{
	int16_t ref[UDSP_FRAME_MAX];
	unsigned f;

	memset(ref, 0, sizeof(ref));
	for (f = 0; f < frames; f++)
		verdict_step(ec, path, ref, NULL, acted);
}

/* An instance whose processor was on for every frame counts each frame once,
 * and its active count is the count of the frames that read nlp_active. */
static void verdict_identity_check(const struct udsp_ec *ec, unsigned acted)
{
	struct udsp_ec_nlms_info info;

	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(verdict_sum_get(&info), info.frames);
	CHECK_UINT(info.nlp_active_frames, acted);
}

/*
 * The fixtures of nlp_gate_probe and reset_test with each frame read, at the
 * line rate: a flat hybrid at delay_ms, far-end noise from a zero filter with
 * the processor on, then the filter frozen.
 */
static struct udsp_ec *gate_fixture_rate_create(struct echo_path *path,
						unsigned rate,
						unsigned delay_ms,
						double erl_db, unsigned frames,
						uint32_t *state,
						unsigned *acted)
{
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);

	path_flat_init(path, rate, delay_ms, erl_db);
	verdict_far_run(ec, path, state, frames, acted);
	udsp_ec_nlms.freeze(ec, true);
	return ec;
}

/* The fixture above at 8 kHz on a flat 8 ms hybrid. */
static struct udsp_ec *gate_fixture_create(struct echo_path *path,
					   double erl_db, unsigned frames,
					   uint32_t *state, unsigned *acted)
{
	return gate_fixture_rate_create(path, UDSP_SAMPLE_RATE, 8u, erl_db,
					frames, state, acted);
}

/*
 * A window with no reference is the first refusal, and a processor that is
 * switched off is no refusal at all. Zero frames on a fresh instance take the
 * skip path of ec_run, which still reaches the gate. A count placed after the
 * return that leaves a silent frame alone reads no quiet frame here. A count
 * that files the switched-off frames as quiet reads 100.
 */
static void nlp_quiet_count_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	unsigned acted = 0;

	CASE("lec nlp counts a silent window as quiet and a switched-off "
	     "processor nowhere");
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	verdict_silence_run(ec, &path, 50u, &acted);
	verdict_identity_check(ec, acted);
	udsp_ec_nlms.nlp(ec, false);
	verdict_silence_run(ec, &path, 50u, &acted);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.skip_frames, 100);
	CHECK_UINT(info.nlp_quiet_frames, 50);
	CHECK_UINT(info.nlp_active_frames, 0);
	CHECK_UINT(info.nlp_wait_frames, 0);
	CHECK_UINT(info.nlp_erle_frames, 0);
	CHECK_UINT(info.nlp_unlike_frames, 0);
	CHECK_UINT(info.nlp_dt_frames, 0);
	CHECK_UINT(info.frames - verdict_sum_get(&info), 50);
	CHECK_UINT(acted, 0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The ERLE gate on the fixtures of nlp_gate_probe. On the 12 dB hybrid the
 * tracker sits under the 18 dB fast path and the combined loss under the
 * 33 dB gate, so the gate refuses the echo-like frames. On the 23 dB hybrid
 * the combined loss passes and the processor acts. A count that files an ERLE
 * refusal as a frame that is not echo-like fails the first half. A combined
 * arm that passes the frames it refuses fails it too.
 */
static void nlp_erle_count_test(void)
{
	static const double erl_db[2] = { 12.0, 23.0 };
	struct udsp_ec_nlms_info before;
	struct udsp_ec_nlms_info after;
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t erle[2];
	uint32_t active[2];
	unsigned i;

	CASE("lec nlp counts the frames its loss gate refuses");
	for (i = 0; i < 2u; i++) {
		uint32_t state = 0x31415926u;
		unsigned acted = 0;

		ec = gate_fixture_create(&path, erl_db[i], 36u, &state, &acted);
		udsp_ec_nlms_info_get(ec, &before);
		verdict_far_run(ec, &path, &state, 2u * FRAMES_PER_SECOND,
				&acted);
		udsp_ec_nlms_info_get(ec, &after);
		verdict_identity_check(ec, acted);
		erle[i] = after.nlp_erle_frames - before.nlp_erle_frames;
		active[i] = after.nlp_active_frames - before.nlp_active_frames;
		printf("# %.0f dB: erle %.1f dB, over 200 frames erle %u "
		       "unlike %u active %u\n", erl_db[i],
		       (double)after.erle_db, erle[i],
		       after.nlp_unlike_frames - before.nlp_unlike_frames,
		       active[i]);
		udsp_ec_nlms.destroy(ec);
	}
	CHECK(erle[0] >= 150u);
	CHECK_UINT(active[0], 0);
	CHECK(active[1] >= 150u);
}

/*
 * The near talker of nlp_echo_like_test answers in the far-end pauses, 1 dB
 * under the echo. The detector cannot see it, so the frame passes the
 * double-talk test and stops at echo-likeness. A count that files it under the
 * ERLE gate, or drops it, fails here.
 */
static void nlp_unlike_count_test(void)
{
	struct udsp_ec_nlms_info before;
	struct udsp_ec_nlms_info after;
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state = 0x31415926u;
	uint32_t near_state = 0xa5a5c3c3u;
	unsigned near_frames = 0;
	unsigned acted = 0;
	unsigned f;

	CASE("lec nlp counts a near talker the echo does not explain");
	ec = gate_fixture_create(&path, 23.0, 36u, &state, &acted);
	verdict_far_run(ec, &path, &state, FRAMES_PER_SECOND, &acted);
	udsp_ec_nlms_info_get(ec, &before);
	for (f = 0; f < 2u * FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t near[UDSP_FRAME_SAMPLES];
		bool near_now = turn_frame_fill(f, ref, near, &state,
						&near_state);

		if (near_now)
			near_frames++;
		verdict_step(ec, &path, ref, near_now ? near : NULL, &acted);
	}
	udsp_ec_nlms_info_get(ec, &after);
	printf("# %u near frames, unlike %u erle %u dt %u\n", near_frames,
	       after.nlp_unlike_frames - before.nlp_unlike_frames,
	       after.nlp_erle_frames - before.nlp_erle_frames,
	       after.nlp_dt_frames - before.nlp_dt_frames);
	CHECK(near_frames >= 20u);
	CHECK(after.nlp_unlike_frames - before.nlp_unlike_frames + 5u >=
	      near_frames);
	verdict_identity_check(ec, acted);
	udsp_ec_nlms.destroy(ec);
}

/*
 * A near talker 18 dB over the echo is double talk, and its frame is not
 * echo-like either. The gate reads the detector first, so the frame counts as
 * double talk. A gate that tests echo-likeness first files these frames as not
 * echo-like and fails here.
 */
static void nlp_dt_count_test(void)
{
	struct udsp_ec_nlms_info before;
	struct udsp_ec_nlms_info after;
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state = 0x31415926u;
	uint32_t near_state = 0xfeedfaceu;
	unsigned acted = 0;
	unsigned f;

	CASE("lec nlp counts double talk ahead of echo-likeness");
	ec = gate_fixture_create(&path, 23.0, 36u, &state, &acted);
	verdict_far_run(ec, &path, &state, FRAMES_PER_SECOND, &acted);
	udsp_ec_nlms_info_get(ec, &before);
	for (f = 0; f < 30u; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t near[UDSP_FRAME_SAMPLES];

		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
		noise_fill(near, UDSP_FRAME_SAMPLES, -15.0, &near_state);
		verdict_step(ec, &path, ref, near, &acted);
	}
	udsp_ec_nlms_info_get(ec, &after);
	printf("# of 30 frames: dt %u unlike %u\n",
	       after.nlp_dt_frames - before.nlp_dt_frames,
	       after.nlp_unlike_frames - before.nlp_unlike_frames);
	CHECK(after.nlp_dt_frames - before.nlp_dt_frames >= 25u);
	CHECK_UINT(after.nlp_active_frames - before.nlp_active_frames, 0);
	verdict_identity_check(ec, acted);
	udsp_ec_nlms.destroy(ec);
}

/*
 * A near burst at PAUSE_BURST_DBM0 over far-end digital silence. On a quiet
 * frame its microphone stands over the echo-likeness bar of a line with no
 * measured floor, or with a floor far under the burst. The burst stands
 * 27 dB or more under the echo of the fixtures that use it. The floor that it
 * can set therefore moves the bars of an active frame by less than 0.1 dB.
 */
#define PAUSE_BURST_DBM0	-60.0
/* The near talker of nlp_echo_like_test, 1 dB under the echo of its 23 dB
 * hybrid. */
#define FLAP_NEAR_DBM0		-34.0

/* One frame of far-end digital silence with white near noise at near_dbm0. */
static void near_step(struct udsp_ec *ec, struct echo_path *path,
		      double near_dbm0, uint32_t *near_state, unsigned *acted)
{
	int16_t ref[UDSP_FRAME_MAX];
	int16_t near[UDSP_FRAME_MAX];

	memset(ref, 0, sizeof(ref));
	noise_fill(near, path->frame, near_dbm0, near_state);
	verdict_step(ec, path, ref, near, acted);
}

/* The window of an instance in frames: the tail rounded up to whole 10 ms
 * frames (ec_create_rate of dsp/lec.c), 5 on the 48 ms tail and 7 on the
 * 64 ms tail. */
static unsigned window_frames_get(const struct udsp_ec *ec)
{
	return (udsp_ec_nlms_tail_get(ec) + 9u) / 10u;
}

/* A refused frame every 4th frame leaves 3 pass frames between two refusals,
 * the most that still holds the gate off. A refused frame every 5th frame is
 * the boundary, where the processor acts on one frame per cycle. */
#define FLAP_PERIOD		4u
#define FLAP_COST_PERIOD	5u
#define FLAP_FRAMES		(2u * FRAMES_PER_SECOND)

/*
 * The line of the flap cases, on the fixture of nlp_unlike_count_test: from a
 * complete run, the far end leaves a gap of one frame in every period. A
 * near talker 1 dB under the echo fills the gap, so the frame reads unlike
 * while the window stays active. The detector does not fire, and the ERLE
 * tracker does not read the gap frame, which is not active. The line stays
 * open for the checks of the case.
 */
struct flap_line {
	struct echo_path path;
	struct udsp_ec *ec;
	struct udsp_ec_nlms_info before;
	struct udsp_ec_nlms_info after;
	unsigned flaps;
	unsigned acted;
	unsigned start;
};

static void flap_run(struct flap_line *l, unsigned rate, unsigned period)
{
	uint32_t state = 0x31415926u;
	uint32_t near_state = 0xa5a5c3c3u;
	unsigned f;

	memset(l, 0, sizeof(*l));
	l->ec = gate_fixture_rate_create(&l->path, rate, 8u, 23.0, 36u, &state,
					 &l->acted);
	verdict_far_run(l->ec, &l->path, &state, FRAMES_PER_SECOND, &l->acted);
	udsp_ec_nlms_info_get(l->ec, &l->before);
	l->start = l->acted;
	for (f = 0; f < FLAP_FRAMES; f++) {
		if (f % period) {
			verdict_far_run(l->ec, &l->path, &state, 1u, &l->acted);
			continue;
		}
		near_step(l->ec, &l->path, FLAP_NEAR_DBM0, &near_state,
			  &l->acted);
		l->flaps++;
	}
	udsp_ec_nlms_info_get(l->ec, &l->after);
}

static void flap_print(const struct flap_line *l)
{
	printf("# engaged %u before the flap; %u flap frames: unlike %u, "
	       "wait %u, active %u, dt %u, erle %u, quiet %u\n",
	       l->before.nlp_active, l->flaps,
	       l->after.nlp_unlike_frames - l->before.nlp_unlike_frames,
	       l->after.nlp_wait_frames - l->before.nlp_wait_frames,
	       l->after.nlp_active_frames - l->before.nlp_active_frames,
	       l->after.nlp_dt_frames - l->before.nlp_dt_frames,
	       l->after.nlp_erle_frames - l->before.nlp_erle_frames,
	       l->after.nlp_quiet_frames - l->before.nlp_quiet_frames);
}

/*
 * The engage hysteresis against a flap, a refused frame every few frames of
 * far speech (heard as garbled speech), on the line of flap_run. The far end
 * leaves a gap of one frame in every 4. The first refusal cuts the complete
 * run to LEC_NLP_DECAY_FRAMES. After 3 pass frames, each later refusal meets
 * a run under 8 and zeroes it, so the processor never acts. At 8 kHz the
 * detector can fire on a gap frame, and its dt frames are refusals too. A
 * rule that keeps the run through a refused frame fails here, as does a
 * larger cut.
 */
static void nlp_hysteresis_test(unsigned rate)
{
	struct flap_line l;
	unsigned refused;

	flap_run(&l, rate, FLAP_PERIOD);
	refused = l.after.nlp_unlike_frames - l.before.nlp_unlike_frames +
		  l.after.nlp_dt_frames - l.before.nlp_dt_frames;
	case_rate(rate, "lec nlp stays off under a refused frame every few "
		  "frames of far speech");
	flap_print(&l);
	CHECK(l.before.nlp_active);
	CHECK_UINT(l.acted - l.start, 0);
	CHECK(refused >= l.flaps);
	CHECK_UINT(l.after.nlp_wait_frames - l.before.nlp_wait_frames,
		   FLAP_FRAMES - refused);
	verdict_identity_check(l.ec, l.acted);
	udsp_ec_nlms.destroy(l.ec);
}

/*
 * The cost of the cut run at the boundary of the flap guard, on the line of
 * flap_run. The far end leaves a gap of one frame in every 5, so 4 pass
 * frames stand between two refusals. Each refusal cuts the complete run to
 * LEC_NLP_DECAY_FRAMES, and the 4th pass frame completes it again. The
 * processor therefore acts on exactly one frame per cycle. A run zeroed on
 * each refusal acts on none, and a run kept through it acts on all 4.
 */
static void nlp_flap_cost_test(unsigned rate)
{
	struct flap_line l;

	flap_run(&l, rate, FLAP_COST_PERIOD);
	case_rate(rate, "lec nlp acts on one frame per cycle under a refused "
		  "frame every fifth frame, the cost of the cut run");
	flap_print(&l);
	CHECK(l.before.nlp_active);
	CHECK_UINT(l.flaps, FLAP_FRAMES / FLAP_COST_PERIOD);
	CHECK_UINT(l.acted - l.start, l.flaps);
	CHECK_UINT(l.after.nlp_unlike_frames - l.before.nlp_unlike_frames,
		   l.flaps);
	CHECK_UINT(l.after.nlp_wait_frames - l.before.nlp_wait_frames,
		   FLAP_FRAMES - 2u * l.flaps);
	verdict_identity_check(l.ec, l.acted);
	udsp_ec_nlms.destroy(l.ec);
}

#define CYCLE_FRAMES		10u

/*
 * One cycle of the engage-run cases: far frames of far-end noise, then
 * digital silence, with a near burst on the last frame of the cycle when
 * near_state is given. It returns the first frame of the cycle that reads
 * nlp_active, or CYCLE_FRAMES when none does.
 */
static unsigned cycle_run(struct udsp_ec *ec, struct echo_path *path,
			  uint32_t *state, unsigned far, uint32_t *near_state,
			  unsigned *acted)
{
	unsigned at = CYCLE_FRAMES;
	unsigned p;

	for (p = 0; p < CYCLE_FRAMES; p++) {
		unsigned was = *acted;

		if (p < far)
			verdict_far_run(ec, path, state, 1u, acted);
		else if (near_state && p + 1u == CYCLE_FRAMES)
			near_step(ec, path, PAUSE_BURST_DBM0, near_state,
				  acted);
		else
			verdict_silence_run(ec, path, 1u, acted);
		if (at == CYCLE_FRAMES && *acted != was)
			at = p;
	}
	return at;
}

#define SPURT_FAR		3u
#define SPURT_CYCLES		20u

/*
 * The complement of nlp_hysteresis_test: short far spurts with pauses, on the
 * settled fixture of that case. A cycle is 3 frames of far-end noise and 7
 * of digital silence, so each cycle holds 7 eligible frames, one short of the
 * run. Digital silence is echo-like, so a quiet frame keeps the run. A near
 * burst on the last frame before the first spurt zeroes it. The first spurt
 * therefore waits, and every later spurt acts from its first frame. A rule
 * that zeroes the run on a quiet frame with no far-end background fails here.
 */
static void nlp_spurt_test(unsigned rate)
{
	struct udsp_ec_nlms_info before;
	struct udsp_ec_nlms_info after;
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state = 0x31415926u;
	uint32_t near_state = 0xa5a5c3c3u;
	unsigned first_acted = 0;
	unsigned late = 0;
	unsigned acted = 0;
	unsigned c;

	ec = gate_fixture_rate_create(&path, rate, 8u, 23.0, 36u, &state,
				      &acted);
	verdict_far_run(ec, &path, &state, FRAMES_PER_SECOND, &acted);
	(void)cycle_run(ec, &path, &state, 0u, &near_state, &acted);
	udsp_ec_nlms_info_get(ec, &before);
	for (c = 0; c < SPURT_CYCLES; c++) {
		unsigned was = acted;
		unsigned at = cycle_run(ec, &path, &state, SPURT_FAR, NULL,
					&acted);

		if (!c)
			first_acted = acted - was;
		else
			late += at != 0u;
	}
	udsp_ec_nlms_info_get(ec, &after);

	case_rate(rate, "lec nlp acts from the second far spurt over echo-like "
		  "pauses");
	printf("# %u spurts: the first acts on %u frames, %u later spurts act "
	       "after their first frame; wait %u, active %u, quiet %u\n",
	       SPURT_CYCLES, first_acted, late,
	       after.nlp_wait_frames - before.nlp_wait_frames,
	       after.nlp_active_frames - before.nlp_active_frames,
	       after.nlp_quiet_frames - before.nlp_quiet_frames);
	CHECK_UINT(first_acted, 0);
	CHECK_UINT(late, 0);
	CHECK_UINT(after.nlp_wait_frames - before.nlp_wait_frames,
		   SPURT_FAR + window_frames_get(ec) - 1u);
	verdict_identity_check(ec, acted);
	udsp_ec_nlms.destroy(ec);
}

#define WAIT_FAR		4u
#define WAIT_BLOCKS		7u

/*
 * The engage run of LEC_NLP_ON_FRAMES on the fixture of reset_test, at the
 * line rate. A cycle holds 4 frames of far-end noise and 6 of digital
 * silence: positions 0 to 7 pass, and 8 and 9 are quiet. Digital silence is
 * echo-like and keeps the run, and a near burst on position 9 of every third
 * cycle zeroes it. The next cycle waits 7 frames and acts on its 8th, and
 * every other cycle acts on its first frame. A wrong count of the engage
 * frame fails here. So does a rule that engages early, zeroes the run on
 * silence or keeps it through the burst.
 */
static void nlp_wait_count_test(unsigned rate)
{
	struct udsp_ec_nlms_info before;
	struct udsp_ec_nlms_info after;
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state = 0x77665544u;
	uint32_t near_state = 0x600dcafeu;
	unsigned eighth = 0;
	unsigned first = 0;
	unsigned acted = 0;
	unsigned start;
	unsigned c;

	case_rate(rate, "lec nlp waits seven frames after a pause frame over "
		  "the echo-like bar and acts at once after an echo-like "
		  "pause");
	ec = gate_fixture_rate_create(&path, rate, 8u, 12.0, 150u, &state,
				      &acted);
	udsp_ec_nlms_info_get(ec, &before);
	CHECK(before.erle_db >= 18.0f);
	CHECK(before.nlp_active);
	start = acted;
	for (c = 0; c < 3u * WAIT_BLOCKS; c++) {
		unsigned at = cycle_run(ec, &path, &state, WAIT_FAR,
					c % 3u ? NULL : &near_state, &acted);

		if (c % 3u == 1u)
			eighth += at == 7u;
		else
			first += at == 0u;
	}
	udsp_ec_nlms_info_get(ec, &after);
	printf("# %u cycles: %u wait cycles act from position 7, %u others "
	       "from position 0; active %u wait %u quiet %u\n",
	       3u * WAIT_BLOCKS, eighth, first,
	       after.nlp_active_frames - before.nlp_active_frames,
	       after.nlp_wait_frames - before.nlp_wait_frames,
	       after.nlp_quiet_frames - before.nlp_quiet_frames);
	CHECK_UINT(after.nlp_active_frames - before.nlp_active_frames,
		   WAIT_BLOCKS * (8u + 1u + 8u));
	CHECK_UINT(after.nlp_wait_frames - before.nlp_wait_frames,
		   WAIT_BLOCKS * 7u);
	CHECK_UINT(after.nlp_quiet_frames - before.nlp_quiet_frames,
		   3u * WAIT_BLOCKS * 2u);
	CHECK_UINT(after.nlp_erle_frames - before.nlp_erle_frames, 0);
	CHECK_UINT(after.nlp_unlike_frames - before.nlp_unlike_frames, 0);
	CHECK_UINT(after.nlp_dt_frames - before.nlp_dt_frames, 0);
	CHECK_UINT(eighth, WAIT_BLOCKS);
	CHECK_UINT(first, 2u * WAIT_BLOCKS);
	CHECK_UINT(acted - start, after.nlp_active_frames -
		   before.nlp_active_frames);
	verdict_identity_check(ec, acted);
	udsp_ec_nlms.destroy(ec);
}

#define REFUSAL_LEAD_FRAMES	20u
#define REFUSAL_TAIL_FRAMES	100u
#define REFUSAL_DT_FRAMES	2u
#define REFUSAL_DT_NEAR_DBM0	-15.0
#define REFUSAL_ERL_STEP_DB	15.0
#define REFUSAL_ERLE_FRAMES	40u
/* A zeroed run acts on its 8th eligible frame (LEC_NLP_ON_FRAMES of
 * dsp/lec.c), 7 counted from 0. A run cut to LEC_NLP_DECAY_FRAMES acts on
 * the 4th eligible frame after the refusal, 3 counted from 0. */
#define REFUSAL_WAIT		7u
#define CUT_WAIT		3u

/*
 * The frames of the engage-run cases of one refusal. refused counts the
 * frames of that verdict, and like those of them whose microphone is
 * echo-like. other counts the frames of every other refusal. passes counts
 * the pass frames since the last refused frame, and at holds that count on
 * the first frame after a refusal that reads nlp_active.
 */
struct refusal_watch {
	unsigned verdict;
	unsigned refused;
	unsigned like;
	unsigned other;
	unsigned passes;
	unsigned at;
	bool waiting;
};

/* The echo-likeness test of doubletalk_update on a frame reading. 1.41 is
 * LEC_GEIGEL_MARGIN, 2.0 LEC_GEIGEL_FLOOR_HEADROOM and 2.8
 * LEC_GEIGEL_PAUSE_HEADROOM of dsp/lec.c. A window_run of 0 is a quiet
 * frame. */
static bool reading_echo_like_get(const struct udsp_ec_nlms_frame *r)
{
	float headroom = r->window_run ? 2.0f : 2.8f;

	return r->mic_power <= (r->estimate_power + r->noise_power * headroom) *
			       1.41f;
}

static void refusal_note(struct refusal_watch *w, const struct udsp_ec *ec)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;

	udsp_ec_nlms_frame_get(ec, &reading);
	udsp_ec_nlms_info_get(ec, &info);
	if (reading.nlp_verdict == w->verdict) {
		w->refused++;
		w->like += reading_echo_like_get(&reading);
		w->passes = 0;
		w->waiting = true;
		return;
	}
	if (reading.nlp_verdict != LECR_VERDICT_PASS) {
		w->other++;
		return;
	}
	if (w->waiting && info.nlp_active) {
		w->at = w->passes;
		w->waiting = false;
	}
	w->passes++;
}

static void refusal_far_run(struct udsp_ec *ec, struct echo_path *path,
			    uint32_t *state, unsigned frames,
			    struct refusal_watch *w, unsigned *acted)
{
	unsigned f;

	for (f = 0; f < frames; f++) {
		verdict_far_run(ec, path, state, 1u, acted);
		refusal_note(w, ec);
	}
}

static void refusal_print(const char *what, const struct refusal_watch *w)
{
	printf("# %s: %u refused frames, %u of them echo-like, %u other "
	       "refused frames; the processor acts on eligible frame %u after "
	       "the last refused frame, counted from 0\n", what, w->refused,
	       w->like, w->other, w->at);
}

/*
 * From a complete run, a near burst 7 dB over the echo fires the detector on
 * far speech. The burst frames and the hangover after them read dt, and the
 * microphone of a hangover frame is echo-like. The first of these refusals
 * cuts the run to LEC_NLP_DECAY_FRAMES, and the next one zeroes it. The far
 * speech after the hangover must wait 7 eligible frames and act on the 8th.
 * A rule that keeps the run through a dt frame fails here, and
 * nlp_erle_wait_test covers echo-like refused frames.
 */
static void nlp_dt_wait_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct refusal_watch w;
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state = 0x77665544u;
	uint32_t near_state = 0xfeedfaceu;
	unsigned acted = 0;
	unsigned f;

	memset(&w, 0, sizeof(w));
	w.verdict = LECR_VERDICT_DT;
	w.at = REFUSAL_TAIL_FRAMES;
	ec = gate_fixture_rate_create(&path, rate, 8u, 12.0, 150u, &state,
				      &acted);
	verdict_far_run(ec, &path, &state, REFUSAL_LEAD_FRAMES, &acted);
	udsp_ec_nlms_info_get(ec, &info);
	for (f = 0; f < REFUSAL_DT_FRAMES; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t near[UDSP_FRAME_MAX];

		noise_fill(ref, path.frame, -10.0, &state);
		noise_fill(near, path.frame, REFUSAL_DT_NEAR_DBM0, &near_state);
		verdict_step(ec, &path, ref, near, &acted);
		refusal_note(&w, ec);
	}
	refusal_far_run(ec, &path, &state, REFUSAL_TAIL_FRAMES, &w, &acted);

	case_rate(rate, "lec nlp waits seven eligible frames after the double "
		  "talk of a near burst");
	refusal_print("near burst", &w);
	CHECK(info.nlp_active);
	CHECK(w.refused > REFUSAL_DT_FRAMES);
	CHECK(w.like > 0u);
	CHECK_UINT(w.other, 0);
	CHECK_UINT(w.at, REFUSAL_WAIT);
	verdict_identity_check(ec, acted);
	udsp_ec_nlms.destroy(ec);
}

/*
 * From a complete run, the hybrid of the frozen fixture turns 3 dB quieter
 * while the far end talks. The filter then predicts more echo than the line
 * returns, so each frame stays echo-like, and the tracked ERLE falls under the
 * gate. The frames read erle until the hybrid returns and the tracker climbs
 * back. The first erle frame cuts the run to LEC_NLP_DECAY_FRAMES, and the
 * next one zeroes it. The far speech must then wait 7 eligible frames after
 * the last erle frame and act on the 8th. A rule that keeps the run through
 * an echo-like refused frame fails here.
 */
static void nlp_erle_wait_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct refusal_watch w;
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state = 0x77665544u;
	unsigned acted = 0;

	memset(&w, 0, sizeof(w));
	w.verdict = LECR_VERDICT_ERLE;
	w.at = REFUSAL_TAIL_FRAMES;
	ec = gate_fixture_rate_create(&path, rate, 8u, 12.0, 150u, &state,
				      &acted);
	verdict_far_run(ec, &path, &state, REFUSAL_LEAD_FRAMES, &acted);
	udsp_ec_nlms_info_get(ec, &info);
	path_erl_set(&path, REFUSAL_ERL_STEP_DB);
	refusal_far_run(ec, &path, &state, REFUSAL_ERLE_FRAMES, &w, &acted);
	path_erl_set(&path, 12.0);
	refusal_far_run(ec, &path, &state, REFUSAL_TAIL_FRAMES, &w, &acted);

	case_rate(rate, "lec nlp waits seven eligible frames after the erle "
		  "gate refuses far speech");
	refusal_print("quieter hybrid", &w);
	CHECK(info.nlp_active);
	CHECK(w.refused > 0u);
	CHECK_UINT(w.like, w.refused);
	CHECK_UINT(w.other, 0);
	CHECK_UINT(w.at, REFUSAL_WAIT);
	verdict_identity_check(ec, acted);
	udsp_ec_nlms.destroy(ec);
}

#define CUT_GAP_FRAMES		1u

/* One leg of nlp_cut_test from a complete run: refusals unlike frames, as
 * flap_run makes them, with CUT_GAP_FRAMES far frames between two of them,
 * then far speech. */
static void cut_leg_run(struct udsp_ec *ec, struct echo_path *path,
			uint32_t *state, uint32_t *near_state,
			unsigned refusals, struct refusal_watch *w,
			unsigned *acted)
{
	unsigned n;

	memset(w, 0, sizeof(*w));
	w->verdict = LECR_VERDICT_UNLIKE;
	w->at = REFUSAL_TAIL_FRAMES;
	for (n = 0; n < refusals; n++) {
		if (n)
			refusal_far_run(ec, path, state, CUT_GAP_FRAMES, w,
					acted);
		near_step(ec, path, FLAP_NEAR_DBM0, near_state, acted);
		refusal_note(w, ec);
	}
	refusal_far_run(ec, path, state, REFUSAL_TAIL_FRAMES, w, acted);
}

/*
 * The cut of a complete run, on the fixture of nlp_hysteresis_test. A single
 * unlike frame inside far speech cuts the run to LEC_NLP_DECAY_FRAMES, so
 * the processor acts on the 4th eligible frame after it. Of two unlike frames
 * with one pass frame between them, the second meets a run under 8 and
 * zeroes it. The processor then acts on the 8th eligible frame after it. A
 * run zeroed on every refusal fails the first leg, and a run kept through a
 * refusal fails both.
 */
static void nlp_cut_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct refusal_watch one;
	struct refusal_watch two;
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state = 0x31415926u;
	uint32_t near_state = 0xa5a5c3c3u;
	unsigned acted = 0;
	bool engaged;

	ec = gate_fixture_rate_create(&path, rate, 8u, 23.0, 36u, &state,
				      &acted);
	verdict_far_run(ec, &path, &state, FRAMES_PER_SECOND, &acted);
	udsp_ec_nlms_info_get(ec, &info);
	engaged = info.nlp_active;
	cut_leg_run(ec, &path, &state, &near_state, 1u, &one, &acted);
	cut_leg_run(ec, &path, &state, &near_state, 2u, &two, &acted);

	case_rate(rate, "lec nlp cuts a complete run on a single refused frame "
		  "and zeroes it on a second one 2 frames later");
	refusal_print("one unlike frame", &one);
	refusal_print("two unlike frames", &two);
	CHECK(engaged);
	CHECK_UINT(one.refused, 1);
	CHECK_UINT(two.refused, 2);
	CHECK_UINT(one.other + two.other, 0);
	CHECK_UINT(one.at, CUT_WAIT);
	CHECK_UINT(two.at, REFUSAL_WAIT);
	verdict_identity_check(ec, acted);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The peak tap of a filter on a flat hybrid is the tap of the hybrid delay,
 * counted from tap 0 of the reference the canceller reads. A fresh filter
 * reads 0 with no energy. A peak one tap off, the smallest coefficient in
 * place of the largest, or a constant fails here.
 */
static void peak_tap_test(unsigned rate)
{
	static const unsigned delay_ms[2] = { 8u, 20u };
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state = 0x13572468u;
	unsigned i;

	case_rate(rate, "lec peak tap names the tap of the hybrid delay");
	for (i = 0; i < 2u; i++) {
		ec = udsp_ec_nlms.create_rate(48, rate);
		udsp_ec_nlms.nlp(ec, false);
		udsp_ec_nlms_info_get(ec, &info);
		CHECK_UINT(info.coef_peak_tap, 0);
		CHECK(info.coef_energy == 0.0f);
		path_flat_init(&path, rate, delay_ms[i], 12.0);
		(void)erle_run(ec, &path, &state, -10.0, FRAMES_PER_SECOND, 1);
		udsp_ec_nlms_info_get(ec, &info);
		CHECK(info.coef_energy > 0.0f);
		CHECK_UINT(info.coef_peak_tap,
			   path_samples_get(delay_ms[i], rate));
		udsp_ec_nlms.destroy(ec);
	}
}

/*
 * The reset forgets the verdicts with the frames, because they describe one
 * alignment of the reference, and it forgets the peak with the filter. A
 * far-end pause and a near talker follow the fixture of reset_test, so each
 * of the six counts stands above 0 before the reset. So does the count of the
 * double-talk frames the detector fired on. A reset that leaves one of them,
 * or the peak, standing fails here. The trial and watchdog counters keep
 * their lifetime meaning.
 */
static void reset_count_test(void)
{
	struct udsp_ec_nlms_info before;
	struct udsp_ec_nlms_info after;
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state = 0x77665544u;
	uint32_t near_state = 0x600dcafeu;
	unsigned acted = 0;
	unsigned f;

	CASE("lec reset zeroes the processor counters and the peak tap");
	ec = gate_fixture_create(&path, 12.0, 150u, &state, &acted);
	verdict_silence_run(ec, &path, 10u, &acted);
	for (f = 0; f < 5u; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t near[UDSP_FRAME_SAMPLES];

		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
		noise_fill(near, UDSP_FRAME_SAMPLES, -15.0, &near_state);
		verdict_step(ec, &path, ref, near, &acted);
	}
	udsp_ec_nlms_info_get(ec, &before);
	printf("# before the reset: active %u wait %u erle %u unlike %u "
	       "dt %u fired %u quiet %u peak %u\n", before.nlp_active_frames,
	       before.nlp_wait_frames, before.nlp_erle_frames,
	       before.nlp_unlike_frames, before.nlp_dt_frames,
	       before.nlp_dt_fired_frames, before.nlp_quiet_frames,
	       before.coef_peak_tap);
	verdict_identity_check(ec, acted);
	CHECK(before.nlp_active_frames > 0u);
	CHECK(before.nlp_wait_frames >= 7u);
	CHECK(before.nlp_erle_frames > 0u);
	CHECK(before.nlp_unlike_frames > 0u);
	CHECK(before.nlp_dt_frames > 0u);
	CHECK(before.nlp_quiet_frames > 0u);
	CHECK(before.nlp_dt_fired_frames > 0u);
	CHECK_UINT(before.coef_peak_tap, 64);
	udsp_ec_nlms_reset(ec);
	udsp_ec_nlms_info_get(ec, &after);
	CHECK_UINT(after.nlp_active_frames, 0);
	CHECK_UINT(after.nlp_wait_frames, 0);
	CHECK_UINT(after.nlp_erle_frames, 0);
	CHECK_UINT(after.nlp_unlike_frames, 0);
	CHECK_UINT(after.nlp_dt_frames, 0);
	CHECK_UINT(after.nlp_quiet_frames, 0);
	CHECK_UINT(after.nlp_dt_fired_frames, 0);
	CHECK_UINT(after.coef_peak_tap, 0);
	CHECK_UINT(after.diverge_resets, before.diverge_resets);
	CHECK_UINT(after.probes, before.probes);
	udsp_ec_nlms.destroy(ec);
}

/* The ERLE tracker smooths over about ten frames, so a step waits that long
 * before it reads the tracker as settled. */
#define RAMP_STEP_DB		0.5
#define RAMP_SETTLED_DB		24.0f
#define RAMP_DWELL_FRAMES	10u

/*
 * The case covers the plain zero branch of the divergence watchdog; a trial
 * abort instead restores the snapshot and leaves energy in the filter. After
 * convergence the hybrid grows louder in 0.5 dB steps, each once the tracked
 * ERLE is back at 24 dB. The ERLE stays above 18 dB, so the detector stays
 * quiet; plain adaptation carries the coefficient energy past
 * LEC_COEF_ENERGY_MAX outside any trial. A zero branch that leaves the peak
 * standing fails. The ramp uses the 16 ms tail: talk_scale_get cuts the step
 * by the hybrid gain over the 6 dB assumption. In 10 s, a 48 ms tail reaches
 * -3.5 dB of ERL; the 16 ms tail trips the watchdog at -7 dB after 8.1 s.
 */
static void divergence_peak_test(void)
{
	struct udsp_ec_nlms_info before;
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(16);
	uint32_t state = 0x0c0ffee2u;
	double erl_db = 12.0;
	float erle_min = 200.0f;
	unsigned dwell = 0;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, erl_db);
	(void)erle_run(ec, &path, &state, -10.0, FRAMES_PER_SECOND, 1);
	udsp_ec_nlms_info_get(ec, &before);
	for (f = 0; f < 10u * FRAMES_PER_SECOND; f++) {
		(void)erle_run(ec, &path, &state, -10.0, 1u, 1u);
		udsp_ec_nlms_info_get(ec, &info);
		if (info.diverge_resets != before.diverge_resets)
			break;
		if (info.erle_db < erle_min)
			erle_min = info.erle_db;
		if (++dwell < RAMP_DWELL_FRAMES ||
		    info.erle_db < RAMP_SETTLED_DB)
			continue;
		erl_db -= RAMP_STEP_DB;
		path_erl_set(&path, erl_db);
		dwell = 0;
	}

	CASE("lec divergence reset clears the peak tap");
	printf("# frame %u, ERL %.1f dB, erle min %.1f dB, resets %u, "
	       "probes %u, coef %.3f, peak %u\n", f, erl_db,
	       (double)erle_min, info.diverge_resets, info.probes,
	       (double)info.coef_energy, info.coef_peak_tap);
	CHECK(info.diverge_resets > before.diverge_resets);
	CHECK_UINT(info.probes, before.probes);
	CHECK(info.coef_energy == 0.0f);
	CHECK_UINT(info.coef_peak_tap, 0);
	udsp_ec_nlms.destroy(ec);
}

#define FEED_PAUSE_FRAMES	10u

/* One frame of far end at -10 dBm0, or of silence, through the hybrid. */
static void feed_frame_make(struct echo_path *path, uint32_t *state, bool loud,
			    int16_t *ref, int16_t *echo)
{
	if (loud)
		noise_fill(ref, path->frame, -10.0, state);
	else
		memset(ref, 0, path->frame * sizeof(*ref));
	path_run(path, ref, echo);
}

/* Both instances run one frame, each on its own copy of the microphone. */
static void feed_pair_run(struct udsp_ec *a, struct udsp_ec *b,
			  const struct echo_path *path, const int16_t *ref,
			  const int16_t *mic, int16_t *out_a, int16_t *out_b)
{
	memcpy(out_a, mic, path->frame * sizeof(*mic));
	memcpy(out_b, mic, path->frame * sizeof(*mic));
	udsp_ec_nlms.run(a, out_a, ref);
	udsp_ec_nlms.run(b, out_b, ref);
}

/* The pause: a runs each loud frame, frozen by the caller, and b takes only
 * the reference. */
static void feed_pause_run(struct udsp_ec *a, struct udsp_ec *b,
			   struct echo_path *path, uint32_t *state)
{
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	unsigned f;

	for (f = 0; f < FEED_PAUSE_FRAMES; f++) {
		feed_frame_make(path, state, true, ref, mic);
		udsp_ec_nlms.run(a, mic, ref);
		udsp_ec_nlms_ref_feed(b, ref);
	}
}

static void feed_history_check(void)
{
	struct udsp_ec_nlms_info before;
	struct udsp_ec_nlms_info info;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	int16_t out_a[UDSP_FRAME_SAMPLES];
	int16_t out_b[UDSP_FRAME_SAMPLES];
	struct udsp_ec *a = udsp_ec_nlms.create(48);
	struct udsp_ec *b = udsp_ec_nlms.create(48);
	struct echo_path path;
	uint32_t state = 0x2718281du;
	unsigned f;

	udsp_ec_nlms.nlp(a, false);
	udsp_ec_nlms.nlp(b, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	for (f = 0; f < FRAMES_PER_SECOND; f++) {
		feed_frame_make(&path, &state, true, ref, mic);
		feed_pair_run(a, b, &path, ref, mic, out_a, out_b);
	}
	udsp_ec_nlms_info_get(a, &info);
	udsp_ec_nlms_info_get(b, &before);
	CHECK(info.coef_energy == before.coef_energy);
	/* A comparison of two idle filters would pass on nothing. */
	CHECK(before.coef_energy > 0.0f);
	udsp_ec_nlms.freeze(a, true);
	feed_pause_run(a, b, &path, &state);
	udsp_ec_nlms_info_get(b, &info);
	CHECK(info.coef_energy == before.coef_energy);
	CHECK_UINT(info.frames, before.frames);
	udsp_ec_nlms.freeze(b, true);
	feed_frame_make(&path, &state, true, ref, mic);
	feed_pair_run(a, b, &path, ref, mic, out_a, out_b);
	CHECK(!memcmp(out_a, out_b, path.frame * sizeof(*out_a)));
	udsp_ec_nlms.destroy(a);
	udsp_ec_nlms.destroy(b);
}

static void feed_window_check(void)
{
	struct udsp_ec_nlms_info a0, a1, b0, b1;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	int16_t out_a[UDSP_FRAME_SAMPLES];
	int16_t out_b[UDSP_FRAME_SAMPLES];
	struct udsp_ec *a = udsp_ec_nlms.create(48);
	struct udsp_ec *b = udsp_ec_nlms.create(48);
	struct echo_path path;
	uint32_t state = 0x31415926u;

	udsp_ec_nlms.nlp(a, false);
	udsp_ec_nlms.nlp(b, false);
	udsp_ec_nlms.freeze(a, true);
	udsp_ec_nlms.freeze(b, true);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	feed_pause_run(a, b, &path, &state);
	udsp_ec_nlms.nlp(a, true);
	udsp_ec_nlms.nlp(b, true);
	udsp_ec_nlms_info_get(a, &a0);
	udsp_ec_nlms_info_get(b, &b0);
	feed_frame_make(&path, &state, false, ref, mic);
	feed_pair_run(a, b, &path, ref, mic, out_a, out_b);
	udsp_ec_nlms_info_get(a, &a1);
	udsp_ec_nlms_info_get(b, &b1);
	printf("# quiet frames on the silent frame: run %u, fed %u\n",
	       a1.nlp_quiet_frames - a0.nlp_quiet_frames,
	       b1.nlp_quiet_frames - b0.nlp_quiet_frames);
	CHECK_UINT(a1.nlp_quiet_frames - a0.nlp_quiet_frames, 0);
	CHECK_UINT(b1.nlp_quiet_frames - b0.nlp_quiet_frames, 0);
	udsp_ec_nlms.destroy(a);
	udsp_ec_nlms.destroy(b);
}

static void feed_skip_run(struct udsp_ec *ec, unsigned loud_frames,
			  unsigned zero_frames)
{
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	uint32_t state = 0x16180339u;
	unsigned f;

	for (f = 0; f < loud_frames; f++) {
		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
		udsp_ec_nlms_ref_feed(ec, ref);
	}
	memset(ref, 0, sizeof(ref));
	for (f = 0; f < zero_frames; f++)
		udsp_ec_nlms_ref_feed(ec, ref);
	memset(mic, 0, sizeof(mic));
	udsp_ec_nlms.run(ec, mic, ref);
}

static void feed_skip_check(void)
{
	struct udsp_ec_nlms_info early;
	struct udsp_ec_nlms_info flushed;
	struct udsp_ec *a = udsp_ec_nlms.create(48);
	struct udsp_ec *b = udsp_ec_nlms.create(48);
	unsigned full;

	udsp_ec_nlms_info_get(a, &early);
	full = (early.taps + UDSP_FRAME_SAMPLES - 1u) / UDSP_FRAME_SAMPLES;
	feed_skip_run(a, full, full - 1u);
	feed_skip_run(b, full, full);
	udsp_ec_nlms_info_get(a, &early);
	udsp_ec_nlms_info_get(b, &flushed);
	printf("# skip frames after %u and %u fed zero frames: %u, %u\n",
	       full - 1u, full, early.skip_frames, flushed.skip_frames);
	CHECK_UINT(early.skip_frames, 0);
	CHECK_UINT(flushed.skip_frames, 1);
	udsp_ec_nlms.destroy(a);
	udsp_ec_nlms.destroy(b);
}

/*
 * Two instances take the same frames; over a pause A runs frozen and B takes
 * the reference feed. On one frozen frame the outputs must match sample for
 * sample: with the taps held, only the history can differ. The match is exact:
 * a level bar on a converging filter cannot tell a stale window from the
 * spread between frames. The match misses the window ring, which never reaches
 * the estimate; fresh instances fed loud frames must not count the next silent
 * frame as quiet. Fed zeros keep the exact window sum, so a zero frame takes
 * the skip path once they cover the history. A fresh history is zero, so a
 * feed that pushed nothing would skip one frame early.
 */
static void ref_feed_test(void)
{
	CASE("lec ref feed keeps the history in step");
	feed_history_check();
	feed_window_check();
	feed_skip_check();
}

/*
 * The main reflection of the composite hybrid starts 4 ms into the response,
 * and the case moves the reference against the drive. The onset then stands
 * 2 ms after tap 0, one period after it, or 1 ms before it. The delay sits on
 * the side that lags, so the echo before tap 0 stays in the microphone.
 *
 * The first two runs must read within 1 dB, and the third must read under
 * half of the first and under 3 dB. The millisecond before tap 0 holds 63 to
 * 65 percent of the echo energy, so no filter gains more than 2.0 dB. Each run
 * takes 3 s, because the start of a run depends on the initial history.
 */
static void ref_offset_test(unsigned rate)
{
	const char *what[3] = { "2 ms after tap 0", "one period after",
				"1 ms before" };
	int onset = (int)path_samples_get(4.0, rate);
	int margin[3] = { (int)path_samples_get(2.0, rate), (int)(rate / 100u),
			  -(int)path_samples_get(1.0, rate) };
	struct echo_path path;
	double erle[3];
	unsigned i;

	for (i = 0; i < 3u; i++) {
		struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
		uint32_t state = 0x0f1e2d3cu;

		udsp_ec_nlms.nlp(ec, false);
		path_composite_init(&path, rate, 12.0);
		erle[i] = erle_offset_run(ec, &path, &state, -10.0,
					  onset - margin[i], 0,
					  3u * FRAMES_PER_SECOND, 50u);
		udsp_ec_nlms.destroy(ec);
	}

	case_rate(rate, "lec echo loss needs the onset past the first tap");
	printf("# ERLE with the onset %s %.2f dB, %s %.2f dB, %s %.2f dB\n",
	       what[0], erle[0], what[1], erle[1], what[2], erle[2]);
	CHECK(erle[0] >= 25.0);
	CHECK(fabs(erle[1] - erle[0]) <= 1.0);
	CHECK(erle[2] < erle[0] / 2.0);
	CHECK(erle[2] < 3.0);
}

/*
 * A pass over a filter with no energy reports tap 0. A fresh instance takes
 * one frame with a loud reference and a silent microphone. The error is 0 on
 * every sample, so the frame adapts with a step of exactly 0. The frame still
 * marks the filter as written, so the pass after it reads every tap as 0. The
 * frame must count no hold, which shows that it adapted and that the pass ran.
 */
static void peak_zero_test(void)
{
	struct udsp_ec_nlms_info info;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x5a5a1234u;

	CASE("lec peak tap of a filter with no energy reads tap 0 "
	     "after a pass");
	udsp_ec_nlms.nlp(ec, false);
	noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
	memset(mic, 0, sizeof(mic));
	CHECK(frame_loud(ref, UDSP_FRAME_SAMPLES));
	udsp_ec_nlms.run(ec, mic, ref);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.frames, 1);
	CHECK_UINT(info.hold_frames, 0);
	CHECK_UINT(info.coef_peak_tap, 0);
	CHECK(info.coef_energy == 0.0f);
	udsp_ec_nlms.destroy(ec);
}

/*
 * Two taps of equal magnitude report the first of them. The reference is one
 * impulse on sample 0, so sample i of the frame updates tap i alone, against
 * that one reference sample. The microphone carries +1000 on sample 20 and
 * -1000 on sample 50, so both taps take the same error magnitude and tie. The
 * first index wins a tie, so the peak reads tap 20. The frame must count no
 * hold, so the update stands.
 */
static void peak_tie_test(void)
{
	struct udsp_ec_nlms_info info;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	struct udsp_ec *ec = udsp_ec_nlms.create(48);

	CASE("lec peak tap of a tie names the first tap");
	udsp_ec_nlms.nlp(ec, false);
	memset(ref, 0, sizeof(ref));
	memset(mic, 0, sizeof(mic));
	ref[0] = 16384;
	mic[20] = 1000;
	mic[50] = -1000;
	udsp_ec_nlms.run(ec, mic, ref);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.hold_frames, 0);
	CHECK(info.coef_energy > 0.0f);
	CHECK_UINT(info.coef_peak_tap, 20);
	udsp_ec_nlms.destroy(ec);
}

/* LEC_ERLE_ALPHA of dsp/lec.c: the pole of the live powers that a trial is
 * judged by. */
#define LIVE_ALPHA		0.1f
/* LEC_PROBE_IMPROVE of dsp/lec.c: the share a trial must remove. */
#define LIVE_IMPROVE		0.5f

/*
 * The live microphone and output powers of ec_run(), rebuilt from the frames
 * the caller passes in and gets back. The trial verdict reads the canceller's
 * own copy of the two, so a case can compute what a compare of the output
 * power alone would have decided.
 */
struct live_track {
	float mic;
	float res;
};

static void live_track_add(struct live_track *t, const int16_t *mic,
			   const int16_t *out, unsigned n)
{
	t->mic += (udsp_ec_nlms_frame_power_get(mic, n) - t->mic) * LIVE_ALPHA;
	t->res += (udsp_ec_nlms_frame_power_get(out, n) - t->res) * LIVE_ALPHA;
}

/* One 8 kHz frame with a near end added to the echo; out receives the output
 * of the canceller. */
static void trial_frame_run(struct udsp_ec *ec, struct echo_path *path,
			    const int16_t *ref, double near_dbm0,
			    uint32_t *near_state, struct live_track *live,
			    int16_t *out)
{
	int16_t echo[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];

	path_run(path, ref, echo);
	floor_add(mic, echo, near_dbm0, near_state);
	memcpy(out, mic, sizeof(mic));
	udsp_ec_nlms.run(ec, out, ref);
	live_track_add(live, mic, out, UDSP_FRAME_SAMPLES);
}

static void trial_step(struct udsp_ec *ec, struct echo_path *path,
		       const int16_t *ref, double near_dbm0,
		       uint32_t *near_state, struct live_track *live,
		       struct udsp_ec_nlms_info *info)
{
	int16_t out[UDSP_FRAME_SAMPLES];

	trial_frame_run(ec, path, ref, near_dbm0, near_state, live, out);
	udsp_ec_nlms_info_get(ec, info);
}

/*
 * The first trial of a run: the frame the probe count moves on, and the frame
 * TRIAL_BUDGET later. The start frame never adapts and every frame after it
 * does, so the verdict falls on that later frame.
 */
struct trial_watch {
	struct udsp_ec_nlms_info start;
	struct udsp_ec_nlms_info verdict;
	unsigned start_frame;
	bool started;
	bool reached;
	float res_before;
	float res_after;
};

static void trial_watch_step(struct trial_watch *w,
			     const struct udsp_ec_nlms_info *info,
			     const struct live_track *live, unsigned f)
{
	if (!w->started && info->probes == 1u) {
		w->started = true;
		w->start_frame = f;
		w->start = *info;
		w->res_before = live->res;
		return;
	}
	if (!w->started || w->reached || f != w->start_frame + TRIAL_BUDGET)
		return;
	w->reached = true;
	w->verdict = *info;
	w->res_after = live->res;
}

static void trial_watch_print(const char *what, const struct trial_watch *w)
{
	double ratio = w->res_before > 0.0f ?
		       (double)w->res_after / w->res_before : 0.0;

	printf("# %s: start %u, verdict %u, smoothed residual echo after "
	       "over before %.3f\n", what, w->start_frame,
	       w->start_frame + TRIAL_BUDGET, ratio);
}

#define PAUSE_FIRST		490u
#define PAUSE_END		510u
#define PAUSE_LAST_FRAME	620u

/*
 * A latch that comes due in a far-end pause starts its trial on the next
 * active frame. The hybrid steps from 12 dB to 3 dB of loss under a converged
 * filter. A near-end floor 6 dB under the echo joins the line. The detector
 * holds every frame, and the run reaches the latch inside a pause of 20
 * frames. The trial must start on frame 510, the first active frame after
 * the pause, and it must stand. A compare of the smoothed residual echo alone
 * would refuse it, and the test checks that from the frames it passes in.
 */
static void latch_pause_test(void)
{
	struct udsp_ec_nlms_info info;
	struct udsp_ec_nlms_info pause_info = { .probes = 0 };
	struct trial_watch watch = { .started = false };
	struct live_track live = { .mic = 0.0f };
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x9a8b7c6du;
	uint32_t floor_state = 0x0badcafeu;
	unsigned held = 0;
	bool start_loud = false;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	(void)erle_run(ec, &path, &state, -10.0,
		       FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20);
	path_erl_set(&path, 3.0);
	for (f = 0; f <= PAUSE_LAST_FRAME; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];

		if (f >= PAUSE_FIRST && f < PAUSE_END)
			memset(ref, 0, sizeof(ref));
		else
			noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
		trial_step(ec, &path, ref, -19.0, &floor_state, &live, &info);
		if (f < PAUSE_FIRST && info.double_talk)
			held++;
		if (f + 1u == PAUSE_FIRST)
			pause_info = info;
		if (!watch.started && info.probes == 1u)
			start_loud = frame_loud(ref, UDSP_FRAME_SAMPLES);
		trial_watch_step(&watch, &info, &live, f);
	}

	CASE("lec latch due in a far-end pause starts its trial on the next "
	     "active frame");
	trial_watch_print("latch after a pause", &watch);
	CHECK_UINT(pause_info.probes, 0);
	CHECK_UINT(held, PAUSE_FIRST);
	CHECK(watch.started);
	CHECK_UINT(watch.start_frame, PAUSE_END);
	CHECK(start_loud);
	CHECK(watch.reached);
	CHECK(!watch.verdict.double_talk);
	CHECK(watch.verdict.coef_energy >= 0.25f);
	CHECK_UINT(watch.verdict.probes, 1);
	CHECK_UINT(watch.verdict.diverge_resets, 0);
	CHECK(watch.res_after > LIVE_IMPROVE * watch.res_before);
	udsp_ec_nlms.destroy(ec);
}

/* The near end of a syllable case: a steady floor, or a talker on the
 * envelope of the far end. */
struct syllable_near {
	double dbm0;
	bool syllabic;
	uint32_t state;
};

#define SYLLABLE_FRAMES_MAX	(7u * FRAMES_PER_SECOND)
/* 3 s, so the trust count of dsp/lec.c completes before the syllables. */
#define SYLLABLE_TRAIN_FRAMES	(3u * FRAMES_PER_SECOND)

/* The far end follows the syllabic envelope from the first frame on, and the
 * run ends on the verdict of the first trial. */
static void syllable_run(struct udsp_ec *ec, struct echo_path *path,
			 uint32_t *state, struct syllable_near *near,
			 unsigned phi, struct trial_watch *watch)
{
	struct udsp_ec_nlms_info info;
	struct live_track live = { .mic = 0.0f };
	unsigned f;

	for (f = 0; f < SYLLABLE_FRAMES_MAX && !watch->reached; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		double near_dbm0 = near->syllabic ?
				   syllabic_dbm0_get(f + phi, near->dbm0) :
				   near->dbm0;

		noise_fill(ref, UDSP_FRAME_SAMPLES,
			   syllabic_dbm0_get(f + phi, -10.0), state);
		trial_step(ec, path, ref, near_dbm0, &near->state, &live,
			   &info);
		trial_watch_step(watch, &info, &live, f);
	}
}

/*
 * A trial is judged by the ratio of the residual echo to the microphone, not
 * by the residual echo alone. The far end follows a syllabic envelope with a
 * swing of 14 dB. At 3.5 cycles per second, a trial of 100 frames ends half a
 * cycle from its start. A trial on a hybrid 2.5 dB louder converges and must
 * stand, although it ends on a loud syllable. A trial on a near talker 3 dB
 * over the echo must fall and give back its snapshot, although it ends on a
 * quiet syllable. Each case also checks that a compare of the smoothed
 * residual echo alone would judge it the other way.
 */
static void probe_syllable_test(void)
{
	struct syllable_near floor = { .dbm0 = -30.0, .state = 0x0badcafeu };
	struct syllable_near talker = { .dbm0 = -19.0, .syllabic = true,
					.state = 0xfeedfaceu };
	struct trial_watch loud = { .started = false };
	struct trial_watch quiet = { .started = false };
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x9a8b7c6du;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	(void)erle_run(ec, &path, &state, -10.0, SYLLABLE_TRAIN_FRAMES, 20);
	udsp_ec_nlms_info_get(ec, &info);
	path_erl_set(&path, 9.5);
	syllable_run(ec, &path, &state, &floor, 5u, &loud);
	udsp_ec_nlms.destroy(ec);

	CASE("lec trial that ends on a loud syllable stands on a converged "
	     "filter");
	trial_watch_print("trial on a louder hybrid", &loud);
	CHECK(info.erle_db >= 18.0f);
	CHECK(loud.reached);
	CHECK(!loud.verdict.double_talk);
	CHECK(loud.verdict.coef_energy > loud.start.coef_energy);
	CHECK(loud.res_after > LIVE_IMPROVE * loud.res_before);

	ec = udsp_ec_nlms.create(48);
	state = 0x0badc0deu;
	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	(void)erle_run(ec, &path, &state, -10.0, SYLLABLE_TRAIN_FRAMES, 20);
	syllable_run(ec, &path, &state, &talker, 19u, &quiet);
	udsp_ec_nlms.destroy(ec);

	CASE("lec trial that ends on a quiet syllable falls on a filter that "
	     "did not converge");
	trial_watch_print("trial on a near talker", &quiet);
	CHECK(quiet.reached);
	CHECK(quiet.verdict.coef_energy == quiet.start.coef_energy);
	CHECK(quiet.verdict.erle_db == quiet.start.erle_db);
	CHECK(quiet.verdict.echo_gain == quiet.start.echo_gain);
	CHECK_UINT(quiet.verdict.diverge_resets, 0);
	CHECK(quiet.res_after <= LIVE_IMPROVE * quiet.res_before);
}

/* The dBov of udsp_cn_level_get (dsp/cn.c) for a noise_fill floor, before
 * the canceller rounds it to a whole dB. */
static double floor_dbov_get(double dbm0)
{
	return -20.0 * log10(dbm0_rms_get(dbm0) / 32767.0);
}

#define ONSET_FLOOR_DBM0	-40.0
#define ONSET_ADAPT_FRAMES	20u
#define ONSET_SILENT_FRAMES	8u
#define ONSET_MOVE_MAX		0.001f

/*
 * A trial takes an onset after a silent tail at a bounded step. A hot hybrid
 * behind a 12 ms bulk delay latches the detector over a near-end floor at
 * -40 dBm0. The trial adapts for 20 frames, then the reference falls silent
 * until the filter window holds no energy. An onset at -48 dBm0 under the
 * floor follows, and the bulk delay keeps its echo out of that frame. The
 * error on the onset frame is the floor alone, and the tracked floor in the
 * step denominator bounds the update. The onset frame must adapt, and the
 * coefficient energy must move by less than 0.001.
 */
static void probe_onset_test(void)
{
	struct udsp_ec_nlms_info info;
	struct udsp_ec_nlms_info quiet;
	struct live_track live = { .mic = 0.0f };
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	struct dtx_far far = { .state = 0x0c0ffee1u };
	uint32_t floor_state = 0x0badcafeu;
	int16_t ref[UDSP_FRAME_SAMPLES];
	double want = floor_dbov_get(ONSET_FLOOR_DBM0);
	uint32_t skipped;
	float move;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 12, 0.0);
	latch_drive(ec, &path, &far, ONSET_FLOOR_DBM0, &floor_state);
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec step stays bounded at an onset after a silent tail inside a "
	     "trial");
	CHECK_UINT(info.probes, 1);
	for (f = 0; f < ONSET_ADAPT_FRAMES; f++) {
		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &far.state);
		trial_step(ec, &path, ref, ONSET_FLOOR_DBM0, &floor_state,
			   &live, &info);
	}
	skipped = info.skip_frames;
	memset(ref, 0, sizeof(ref));
	for (f = 0; f < ONSET_SILENT_FRAMES; f++)
		trial_step(ec, &path, ref, ONSET_FLOOR_DBM0, &floor_state,
			   &live, &quiet);
	noise_fill(ref, UDSP_FRAME_SAMPLES, TRIAL_QUIET_DBM0, &far.state);
	CHECK(frame_loud(ref, UDSP_FRAME_SAMPLES));
	trial_step(ec, &path, ref, ONSET_FLOOR_DBM0, &floor_state, &live,
		   &info);
	move = fabsf(info.coef_energy - quiet.coef_energy);
	printf("# coefficient energy before the onset %.6f, after %.6f, "
	       "move %.3g\n", quiet.coef_energy, info.coef_energy, move);
	printf("# near-end floor %.2f dBov, tracked floor %.2f dBov\n", want,
	       info.noise_dbov);
	/* The last three silent frames skip the tap loops, which the canceller
	 * does only on a window energy of exactly zero. */
	CHECK_UINT(quiet.skip_frames - skipped, 3);
	CHECK_UINT(info.hold_frames, quiet.hold_frames);
	CHECK(move < ONSET_MOVE_MAX);
	CHECK_UINT(info.probes, 1);
	CHECK_UINT(info.diverge_resets, 0);
	/* The floor term of the step denominator bounds the step, so the case
	 * means nothing unless the floor is measured. */
	CHECK(fabs(info.noise_dbov - want) <= 1.0);
	udsp_ec_nlms.destroy(ec);
}

#define WINDOW_BURST_FRAMES	50u
#define WINDOW_PAUSE_FRAMES	8u
#define WINDOW_FLOOR_DBM0	-40.0

/*
 * The canceller measures the near-end floor only once its whole reference
 * window is quiet. Adaptation stays held, and a far-end burst behind a 40 ms
 * bulk delay starts on the first frame, so no quiet window comes first. The
 * echo tail then fills the first four frames of the pause, 18 dB over a
 * near-end floor at -40 dBm0. The test checks that the first of those frames
 * carries the echo. After eight frames of pause, the tracked floor must read
 * the near-end level within 1 dB.
 */
static void noise_floor_window_test(void)
{
	struct udsp_ec_nlms_info info;
	struct live_track live = { .mic = 0.0f };
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x51ade0f1u;
	uint32_t floor_state = 0x600dcafeu;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t out[UDSP_FRAME_SAMPLES];
	double want = floor_dbov_get(WINDOW_FLOOR_DBM0);
	int echo_dbov = 127;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	udsp_ec_nlms.freeze(ec, true);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 40, 12.0);
	for (f = 0; f < WINDOW_BURST_FRAMES; f++) {
		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
		trial_step(ec, &path, ref, WINDOW_FLOOR_DBM0, &floor_state,
			   &live, &info);
	}
	memset(ref, 0, sizeof(ref));
	for (f = 0; f < WINDOW_PAUSE_FRAMES; f++) {
		trial_frame_run(ec, &path, ref, WINDOW_FLOOR_DBM0,
				&floor_state, &live, out);
		if (f == 0u)
			echo_dbov = udsp_cn_level_get(out, UDSP_FRAME_SAMPLES);
	}
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec measures the near-end floor once the tail window is quiet");
	printf("# near-end floor %.2f dBov, pause frame 1 output %d dBov, "
	       "tracked floor %.2f dBov\n", want, echo_dbov, info.noise_dbov);
	CHECK(echo_dbov <= want - 10.0);
	CHECK(fabs(info.noise_dbov - want) <= 1.0);
	udsp_ec_nlms.destroy(ec);
}

#define NLP_FLOOR_DBM0		-48.0
#define NLP_FLOOR_TALK_DBM0	-40.0
#define NLP_FLOOR_DRAIN		10u
#define NLP_FLOOR_BURST		10u
#define NLP_FLOOR_CYCLE		13u
#define NLP_FLOOR_SETTLE	100u
#define NLP_FLOOR_FRAMES	300u
#define NLP_FLOOR_TALK_FRAMES	100u

/* A NULL talk_state means no talker, while the floor is always on the line. */
static void nlp_floor_step(struct udsp_ec *ec, struct echo_path *path,
			   bool far, uint32_t *state, uint32_t *floor_state,
			   uint32_t *talk_state, struct udsp_ec_nlms_info *info)
{
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t echo[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];

	if (far)
		noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, state);
	else
		memset(ref, 0, sizeof(ref));
	path_run(path, ref, echo);
	floor_add(mic, echo, NLP_FLOOR_DBM0, floor_state);
	if (talk_state)
		floor_add(mic, mic, NLP_FLOOR_TALK_DBM0, talk_state);
	udsp_ec_nlms.run(ec, mic, ref);
	udsp_ec_nlms_info_get(ec, info);
}

/*
 * The processor stays engaged over a near-end floor 8 dB under the echo. A
 * converged filter on a 30 dB hybrid gets far-end bursts of 100 ms, 30 ms gaps
 * and a floor at -48 dBm0. The last two frames of each gap carry the floor
 * alone, and the echo-likeness test counts the tracked floor with its
 * headroom as their echo. The headroom covers the floor's excursion and a
 * seed less than 1 dB under the true level. The processor must act on 29 of
 * 32 gap frames: all 32 on this seed, and about 78 percent without the
 * headroom. A near talker 8 dB over the floor must still release it, and the
 * detector must stay clear.
 */
static void nlp_floor_test(void)
{
	struct udsp_ec_nlms_info converged;
	struct udsp_ec_nlms_info seeded;
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x31415926u;
	uint32_t floor_state = 0x0badcafeu;
	uint32_t talk_state = 0xa5a5c3c3u;
	double want = floor_dbov_get(NLP_FLOOR_DBM0);
	unsigned gap_frames = 0;
	unsigned gap_engaged = 0;
	unsigned talk_frames = 0;
	unsigned talk_nlp = 0;
	unsigned talk_dt = 0;
	unsigned f;

	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 30.0);
	(void)erle_run(ec, &path, &state, -10.0, 150u, 20);
	udsp_ec_nlms_info_get(ec, &converged);
	udsp_ec_nlms.freeze(ec, true);
	for (f = 0; f < NLP_FLOOR_DRAIN; f++)
		nlp_floor_step(ec, &path, false, &state, &floor_state, NULL,
			       &seeded);
	for (f = 0; f < NLP_FLOOR_FRAMES; f++) {
		unsigned pos = f % NLP_FLOOR_CYCLE;

		nlp_floor_step(ec, &path, pos < NLP_FLOOR_BURST, &state,
			       &floor_state, NULL, &info);
		/* The first gap frame still carries the echo of the burst. */
		if (f < NLP_FLOOR_SETTLE || pos <= NLP_FLOOR_BURST)
			continue;
		gap_frames++;
		if (info.nlp_active)
			gap_engaged++;
	}

	CASE("lec nlp engages over a near-end floor close to the echo");
	printf("# %u of %u gap frames engaged (%.1f%%), converged erle %.1f dB, "
	       "erle %.1f dB, near-end floor %.2f dBov, seed %.2f dBov, "
	       "tracked %.2f dBov\n", gap_engaged, gap_frames,
	       gap_frames ? 100.0 * gap_engaged / gap_frames : 0.0,
	       (double)converged.erle_db, (double)info.erle_db, want,
	       (double)seeded.noise_dbov, (double)info.noise_dbov);
	CHECK(converged.erle_db >= 18.0f);
	CHECK(fabs(seeded.noise_dbov - want) <= 1.0);
	CHECK(gap_frames >= 20u);
	CHECK(gap_engaged * 32u >= gap_frames * 29u);
	CHECK(info.erle_db >= 6.0f);
	CHECK(info.erle_db < 18.0f);

	for (f = NLP_FLOOR_FRAMES; f < NLP_FLOOR_FRAMES + NLP_FLOOR_TALK_FRAMES;
	     f++) {
		unsigned pos = f % NLP_FLOOR_CYCLE;
		bool talk = pos == NLP_FLOOR_BURST + 1u;

		nlp_floor_step(ec, &path, pos < NLP_FLOOR_BURST, &state,
			       &floor_state, talk ? &talk_state : NULL, &info);
		if (!talk)
			continue;
		talk_frames++;
		if (info.nlp_active)
			talk_nlp++;
		if (info.double_talk)
			talk_dt++;
	}

	CASE("lec nlp still releases for a near talker over the floor");
	printf("# %u talk frames, processor %u, double talk %u, erle %.1f dB, "
	       "tracked floor %.2f dBov\n", talk_frames, talk_nlp, talk_dt,
	       (double)info.erle_db, (double)info.noise_dbov);
	CHECK(talk_frames >= 5u);
	CHECK_UINT(talk_nlp, 0);
	CHECK_UINT(talk_dt, 0);
	udsp_ec_nlms.destroy(ec);
}

/* 1 when dt_fired disagrees with the rule of the detector, in either
 * direction: the detector fires when the window is active and the microphone
 * reaches the bar. window_run reads 0 exactly on an inactive window. */
static unsigned reading_bar_miss(const struct udsp_ec_nlms_frame *reading)
{
	return reading->dt_fired != (reading->window_run != 0u &&
				     reading->mic_power >= reading->bar);
}

#define READING_FLOOR_DBM0	-40.0

/* One frame of far end or of silence through the hybrid, and its reading.
 * floor_state, when given, adds the 8 kHz near-end floor of floor_add at
 * READING_FLOOR_DBM0. */
static void reading_frame_run(struct udsp_ec *ec, struct echo_path *path,
			      uint32_t *state, uint32_t *floor_state, bool loud,
			      struct udsp_ec_nlms_frame *reading)
{
	int16_t ref[UDSP_FRAME_MAX];
	int16_t mic[UDSP_FRAME_MAX];

	feed_frame_make(path, state, loud, ref, mic);
	if (floor_state)
		floor_add(mic, mic, READING_FLOOR_DBM0, floor_state);
	udsp_ec_nlms.run(ec, mic, ref);
	udsp_ec_nlms_frame_get(ec, reading);
}

/*
 * The frame reading on the fixture of nlp_dt_count_test. The far end alone
 * raises no verdict in one second, at an ERLE under the 18 dB trust line. One
 * near-end frame at -15 dBm0 then fires with a hold of 0. The hangover keeps
 * double talk for the next 23 far-end frames, so the lone verdict counts 24
 * double-talk frames and 1 fired frame. The reading fires on exactly the
 * frames where the microphone reaches the bar. A late hold read, a fired
 * count of hold frames, a stale fired flag or an unset verdict word fails
 * here.
 */
static void reading_verdict_test(void)
{
	struct udsp_ec_nlms_info start, before, after;
	struct udsp_ec_nlms_frame reading;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t near[UDSP_FRAME_SAMPLES];
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state = 0x31415926u;
	uint32_t near_state = 0xfeedfaceu;
	unsigned bar_miss = 0;
	unsigned acted = 0;
	unsigned f;

	CASE("lec frame reading names a fresh verdict and the hold behind it");
	ec = gate_fixture_create(&path, 23.0, 36u, &state, &acted);
	udsp_ec_nlms_info_get(ec, &start);
	for (f = 0; f < FRAMES_PER_SECOND; f++) {
		verdict_far_run(ec, &path, &state, 1u, &acted);
		udsp_ec_nlms_frame_get(ec, &reading);
		bar_miss += reading_bar_miss(&reading);
	}
	udsp_ec_nlms_info_get(ec, &before);
	printf("# before the near frame: erle %.1f dB, dt %u over the second\n",
	       (double)before.erle_db,
	       before.nlp_dt_frames - start.nlp_dt_frames);
	CHECK_UINT(before.nlp_dt_frames - start.nlp_dt_frames, 0);
	CHECK(before.erle_db < 18.0f);
	noise_fill(ref, UDSP_FRAME_SAMPLES, -10.0, &state);
	noise_fill(near, UDSP_FRAME_SAMPLES, -15.0, &near_state);
	verdict_step(ec, &path, ref, near, &acted);
	udsp_ec_nlms_frame_get(ec, &reading);
	udsp_ec_nlms_info_get(ec, &after);
	bar_miss += reading_bar_miss(&reading);
	CHECK(reading.dt_fired);
	CHECK_UINT(reading.hold_left, 0);
	CHECK(reading.mic_power >= reading.bar);
	CHECK(!strcmp(udsp_ec_nlms_verdict_name(reading.nlp_verdict), "dt"));
	CHECK(after.double_talk);
	for (f = 1; f <= 24u; f++) {
		verdict_far_run(ec, &path, &state, 1u, &acted);
		udsp_ec_nlms_frame_get(ec, &reading);
		udsp_ec_nlms_info_get(ec, &after);
		bar_miss += reading_bar_miss(&reading);
		if (f == 1u) {
			CHECK(!reading.dt_fired);
			CHECK_UINT(reading.hold_left, 24);
			CHECK(after.double_talk);
			CHECK(reading.mic_power < reading.bar);
		}
		if (f == 23u)
			CHECK(after.double_talk);
	}
	printf("# after 24 far frames: fired %u dt %u, bar misses %u\n",
	       after.nlp_dt_fired_frames - before.nlp_dt_fired_frames,
	       after.nlp_dt_frames - before.nlp_dt_frames, bar_miss);
	CHECK(!after.double_talk);
	CHECK_UINT(bar_miss, 0);
	CHECK_UINT(after.nlp_dt_fired_frames - before.nlp_dt_fired_frames, 1);
	CHECK_UINT(after.nlp_dt_frames - before.nlp_dt_frames, 24);
	CHECK_UINT((after.nlp_dt_frames - after.nlp_dt_fired_frames) -
		   (before.nlp_dt_frames - before.nlp_dt_fired_frames), 23);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The window count of the frame reading on a 48 ms instance from silence.
 * The window covers 5 frames at both rates, so 3 loud frames keep it active
 * for 4 zero frames after them. The count reads 1 to 3 on the loud frames and
 * 4 on the first zero frame. That frame is the tail: the window is active and
 * the frame's own reference is quiet. The fifth zero frame empties the window
 * and ends both. A count that is never reset and a tail read from the window
 * alone fail here.
 */
static void reading_window_test(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
	uint32_t state = 0x2718281du;
	unsigned f;

	case_rate(rate, "lec frame reading counts the frames since the window "
		  "woke and marks the tail");
	path_flat_init(&path, rate, 8, 12.0);
	for (f = 0; f < 10u; f++)
		reading_frame_run(ec, &path, &state, NULL, false, &reading);
	CHECK_UINT(reading.window_run, 0);
	for (f = 0; f < 3u; f++) {
		reading_frame_run(ec, &path, &state, NULL, true, &reading);
		CHECK_UINT(reading.window_run, f + 1u);
		CHECK(!reading.ref_quiet);
	}
	reading_frame_run(ec, &path, &state, NULL, false, &reading);
	CHECK_UINT(reading.window_run, 4);
	CHECK(reading.ref_quiet);
	for (f = 1; f < 5u; f++)
		reading_frame_run(ec, &path, &state, NULL, false, &reading);
	CHECK_UINT(reading.window_run, 0);
	CHECK(!reading.ref_quiet);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The reset inside an active window. Three loud frames leave the count at 3,
 * and the reset clears the whole reading. A read right after it returns
 * zeros, and the next loud frame reads a count of 1 and a frame count of 1. A
 * reset that keeps the count reads 4 on that frame. A reset that keeps the
 * reading reads 3 frames before it.
 */
static void reading_reset_test(void)
{
	struct udsp_ec_nlms_frame reading;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x16180339u;
	unsigned f;

	CASE("lec reset clears the frame reading inside an active window");
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 12.0);
	for (f = 0; f < 3u; f++)
		reading_frame_run(ec, &path, &state, NULL, true, &reading);
	CHECK_UINT(reading.window_run, 3);
	CHECK(reading.bar > 0.0f);
	udsp_ec_nlms_reset(ec);
	udsp_ec_nlms_frame_get(ec, &reading);
	CHECK_UINT(reading.frames, 0);
	CHECK_UINT(reading.window_run, 0);
	CHECK(!reading.dt_fired);
	CHECK(reading.bar == 0.0f);
	reading_frame_run(ec, &path, &state, NULL, true, &reading);
	CHECK_UINT(reading.window_run, 1);
	CHECK_UINT(reading.frames, 1);
	udsp_ec_nlms.destroy(ec);
}

#define READING_PAUSE_FRAMES	10u
#define READING_FAR_FRAMES	20u
/*
 * The far-end training of the floor fixture, and the longer training after
 * which the trust count of dsp/lec.c is complete. A case that reads a bar of
 * the trusted estimate takes the longer one. The training carries no near
 * floor, so the tracked ERLE stays over the trust line while the count runs.
 */
#define READING_TRAIN_FRAMES	150u
#define READING_TRUST_TRAIN_FRAMES	250u

/* A run of far end or of silence over the near-end floor. It returns the count
 * of frames whose dt_fired disagrees with the bar (reading_bar_miss). */
static unsigned reading_floor_frames_run(struct udsp_ec *ec,
					 struct echo_path *path,
					 uint32_t *state, uint32_t *floor_state,
					 bool loud, unsigned frames,
					 struct udsp_ec_nlms_frame *reading)
{
	unsigned miss = 0;
	unsigned f;

	for (f = 0; f < frames; f++) {
		reading_frame_run(ec, path, state, floor_state, loud, reading);
		miss += reading_bar_miss(reading);
	}
	return miss;
}

/*
 * The floor term of the bar, on the fixture of reset_count_test with a
 * near-end floor at -40 dBm0. The fixture trains until the trust count of
 * dsp/lec.c completes, so the estimate sets the bar. The tracker needs 5
 * frames of a far-end pause to measure the floor, and the pause lasts 10.
 * Far-end frames and a second pause follow, and its first tail frame still
 * carries the echo of the 8 ms hybrid. On the first tail frame with an
 * estimate under half the floor, the bar must reach the margin times the
 * headroom times the floor. A bar taken before the floor term, a bar without
 * the headroom, or a fired flag that disagrees with the bar, fails here.
 */
static void reading_floor_test(void)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_frame first = { 0 };
	struct udsp_ec_nlms_frame tail = { 0 };
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state = 0x77665544u;
	uint32_t floor_state = 0x0f100f10u;
	unsigned bar_miss;
	unsigned acted = 0;
	unsigned f;

	CASE("lec frame reading carries the floor in the bar");
	ec = gate_fixture_create(&path, 12.0, READING_TRUST_TRAIN_FRAMES,
				 &state, &acted);
	bar_miss = reading_floor_frames_run(ec, &path, &state, &floor_state,
					    false, READING_PAUSE_FRAMES,
					    &reading);
	printf("# floor after the first pause %.1f\n",
	       (double)reading.noise_power);
	CHECK(reading.noise_power > 0.0f);
	bar_miss += reading_floor_frames_run(ec, &path, &state, &floor_state,
					     true, READING_FAR_FRAMES,
					     &reading);
	for (f = 0; f < READING_PAUSE_FRAMES; f++) {
		reading_frame_run(ec, &path, &state, &floor_state, false,
				  &reading);
		bar_miss += reading_bar_miss(&reading);
		if (!first.ref_quiet && reading.ref_quiet)
			first = reading;
		if (!tail.ref_quiet && reading.ref_quiet &&
		    reading.estimate_power * 2.0f < reading.noise_power)
			tail = reading;
	}
	printf("# first tail frame %u: estimate %.1f bar %.1f floor %.1f\n",
	       first.frames, (double)first.estimate_power, (double)first.bar,
	       (double)first.noise_power);
	printf("# first tail frame under half the floor %u: estimate %.1f "
	       "bar %.1f floor %.1f\n", tail.frames,
	       (double)tail.estimate_power, (double)tail.bar,
	       (double)tail.noise_power);
	CHECK(tail.ref_quiet);
	/* 1.41 is LEC_GEIGEL_MARGIN and 2.0 LEC_GEIGEL_FLOOR_HEADROOM of
	 * dsp/lec.c. The bar is every term plus the floor with its headroom,
	 * times the margin. It therefore never sits under the margin times the
	 * headroom times the floor. A bar without the floor term, or without
	 * the headroom, can. */
	CHECK(tail.bar >= 1.41f * 2.0f * tail.noise_power);
	CHECK_UINT(bar_miss, 0);
	udsp_ec_nlms.destroy(ec);
}

#define HANGOVER_MARGINAL_DBM0		-29.0
#define HANGOVER_SOFT_DBM0		-45.0
#define HANGOVER_EXCURSION_DBM0		-36.0
#define HANGOVER_LOUD_NEAR_DBM0		-22.0
#define HANGOVER_UNDER_TWICE_DBM0	-34.5
#define HANGOVER_OVER_TWICE_DBM0	-33.0
#define HANGOVER_UNDER_SOFT_FAR_DBM0	-26.0
#define HANGOVER_UNDER_SOFT_DBM0	-31.5
#define HANGOVER_OVER_SOFT_FAR_DBM0	-21.0
#define HANGOVER_OVER_SOFT_DBM0		-27.5
#define HANGOVER_SETTLE_FRAMES		30u
#define HANGOVER_FAR_LIMIT		40u
/* LEC_GEIGEL_MARGINAL_RATIO and LEC_GEIGEL_SOFT_RATIO of dsp/lec.c: a verdict
 * under both ratios, with no hangover running, sets the short hangover. */
#define HANGOVER_MARGINAL_RATIO		2.0f
#define HANGOVER_SOFT_RATIO		16.0f

/*
 * The fixture of reading_verdict_test, or with floor set the fixture of
 * reading_floor_test. The far end runs at far_dbm0 or, with pause set, stays
 * silent. A floor line carries a near-end floor at floor_dbm0 on every frame.
 */
struct hangover_line {
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state;
	uint32_t near_state;
	uint32_t floor_state;
	double far_dbm0;
	double floor_dbm0;
	bool floor;
	bool pause;
};

/* One frame, with a near-end burst on top when near is given. */
static void hangover_frame_run(struct hangover_line *line, const int16_t *near,
			       struct udsp_ec_nlms_frame *reading,
			       struct udsp_ec_nlms_info *info)
{
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t echo[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	unsigned i;

	if (line->pause)
		memset(ref, 0, sizeof(ref));
	else
		noise_fill(ref, UDSP_FRAME_SAMPLES, line->far_dbm0,
			   &line->state);
	path_run(&line->path, ref, echo);
	floor_add(mic, echo, line->floor_dbm0,
		  line->floor ? &line->floor_state : NULL);
	for (i = 0; near && i < UDSP_FRAME_SAMPLES; i++)
		mic[i] = sample_clamp((double)mic[i] + near[i]);
	udsp_ec_nlms.run(line->ec, mic, ref);
	udsp_ec_nlms_frame_get(line->ec, reading);
	udsp_ec_nlms_info_get(line->ec, info);
}

static void hangover_near_run(struct hangover_line *line, double near_dbm0,
			      struct udsp_ec_nlms_frame *reading,
			      struct udsp_ec_nlms_info *info)
{
	int16_t near[UDSP_FRAME_SAMPLES];

	noise_fill(near, UDSP_FRAME_SAMPLES, near_dbm0, &line->near_state);
	hangover_frame_run(line, near, reading, info);
}

/* The 23 dB hybrid of reading_verdict_test, after its second of far end
 * alone, with no near-end floor. */
static void hangover_line_create(struct hangover_line *line)
{
	unsigned acted = 0;

	memset(line, 0, sizeof(*line));
	line->state = 0x31415926u;
	line->near_state = 0xfeedfaceu;
	line->far_dbm0 = -10.0;
	line->ec = gate_fixture_create(&line->path, 23.0, 36u, &line->state,
				       &acted);
	verdict_far_run(line->ec, &line->path, &line->state,
			FRAMES_PER_SECOND, &acted);
}

/* The 12 dB hybrid of reading_floor_test after train frames of far end. A
 * far-end pause then measures its floor, and HANGOVER_SETTLE_FRAMES of far
 * end at far_dbm0 follow. */
static void hangover_floor_line_train(struct hangover_line *line,
				      double far_dbm0, unsigned train)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	unsigned acted = 0;
	unsigned f;

	memset(line, 0, sizeof(*line));
	line->state = 0x77665544u;
	line->near_state = 0xfeedfaceu;
	line->floor_state = 0x0f100f10u;
	line->far_dbm0 = far_dbm0;
	line->floor_dbm0 = READING_FLOOR_DBM0;
	line->floor = true;
	line->ec = gate_fixture_create(&line->path, 12.0, train, &line->state,
				       &acted);
	(void)reading_floor_frames_run(line->ec, &line->path, &line->state,
				       &line->floor_state, false,
				       READING_PAUSE_FRAMES, &reading);
	for (f = 0; f < HANGOVER_SETTLE_FRAMES; f++)
		hangover_frame_run(line, NULL, &reading, &info);
}

static void hangover_floor_line_create(struct hangover_line *line,
				       double far_dbm0)
{
	hangover_floor_line_train(line, far_dbm0, READING_TRAIN_FRAMES);
}

/* Far-end frames up to the first one without double talk, at most limit of
 * them. It returns the count of the frames that still read double talk. */
static unsigned hangover_far_run(struct hangover_line *line, unsigned limit)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	unsigned f;

	for (f = 0; f < limit; f++) {
		hangover_frame_run(line, NULL, &reading, &info);
		if (!info.double_talk)
			return f;
	}
	return limit;
}

static double ratio_db_get(float power, float reference)
{
	return 10.0 * log10((double)power / (double)reference);
}

static void hangover_verdict_print(const char *what, double near_dbm0,
				   const struct udsp_ec_nlms_frame *reading)
{
	printf("# %s at %.1f dBm0: %.2f dB over the bar, ", what, near_dbm0,
	       ratio_db_get(reading->mic_power, reading->bar));
	if (reading->noise_power > 0.0f)
		printf("%.2f dB over the floor", ratio_db_get(
		       reading->mic_power, reading->noise_power));
	else
		printf("no floor");
	printf(", hangover %u\n", reading->hold_left);
}

struct hangover_result {
	struct udsp_ec_nlms_frame verdict;
	unsigned lasted;
	uint32_t fired;
	uint32_t dt;
};

/* One near-end frame and then far-end frames until double talk ends. */
static void hangover_verdict_run(struct hangover_line *line, const char *what,
				 double near_dbm0,
				 struct hangover_result *result)
{
	struct udsp_ec_nlms_info before, after;

	udsp_ec_nlms_info_get(line->ec, &before);
	hangover_near_run(line, near_dbm0, &result->verdict, &after);
	hangover_verdict_print(what, near_dbm0, &result->verdict);
	result->lasted = hangover_far_run(line, HANGOVER_FAR_LIMIT);
	udsp_ec_nlms_info_get(line->ec, &after);
	result->fired = after.nlp_dt_fired_frames - before.nlp_dt_fired_frames;
	result->dt = after.nlp_dt_frames - before.nlp_dt_frames;
	printf("# double talk for %u far frames: fired %u dt %u\n",
	       result->lasted, result->fired, result->dt);
}

/* The verdict fired with no hangover running, under twice the bar and within
 * 12 dB of a measured floor. The reading shows every condition of the short
 * hangover but the repeat limit. */
static void hangover_short_check(const struct udsp_ec_nlms_frame *verdict)
{
	CHECK(verdict->dt_fired);
	CHECK_UINT(verdict->hold_left, 0);
	CHECK(verdict->mic_power < HANGOVER_MARGINAL_RATIO * verdict->bar);
	CHECK(verdict->noise_power > 0.0f);
	CHECK(verdict->mic_power < HANGOVER_SOFT_RATIO * verdict->noise_power);
}

/*
 * A floor excursion on the fixture of reading_floor_test. A far end at
 * -45 dBm0 keeps the window active and puts the estimate under the floor. One
 * near-end frame then reaches the bar by less than 3 dB, within 12 dB of the
 * floor. It sets a hangover of 4 frames, so double talk lasts for 3 far-end
 * frames and ends on the fourth. A detector that gives every verdict a
 * hangover of 24 frames fails here.
 */
static void hangover_floor_test(void)
{
	struct hangover_result result;
	struct hangover_line line;

	CASE("lec hangover lasts four frames on a floor excursion");
	hangover_floor_line_create(&line, HANGOVER_SOFT_DBM0);
	hangover_verdict_run(&line, "excursion frame", HANGOVER_EXCURSION_DBM0,
			     &result);
	hangover_short_check(&result.verdict);
	CHECK_UINT(result.lasted, 3);
	CHECK_UINT(result.fired, 1);
	CHECK_UINT(result.dt, 4);
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * A marginal verdict on a line without a measured floor, on the fixture of
 * reading_verdict_test. The near-end frame reaches the bar by less than 3 dB.
 * The line reads a floor of 0, so the frame does not stand within 12 dB of it
 * and sets the long hangover. A detector that gives the short hangover to a
 * line without a floor fails here.
 */
static void hangover_no_floor_test(void)
{
	struct hangover_result result;
	struct hangover_line line;

	CASE("lec hangover lasts 24 frames on a marginal verdict without a "
	     "floor");
	hangover_line_create(&line);
	hangover_verdict_run(&line, "marginal frame", HANGOVER_MARGINAL_DBM0,
			     &result);
	CHECK(result.verdict.dt_fired);
	CHECK_UINT(result.verdict.hold_left, 0);
	CHECK(result.verdict.mic_power <
	      HANGOVER_MARGINAL_RATIO * result.verdict.bar);
	CHECK(result.verdict.noise_power == 0.0f);
	CHECK_UINT(result.lasted, 23);
	CHECK_UINT(result.fired, 1);
	CHECK_UINT(result.dt, 24);
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * A marginal verdict far over the floor. The fixture of reading_floor_test
 * carries a far end at -10 dBm0, so the echo stands near -22 dBm0. It trains
 * until the trust count completes. The near-end frame reaches the bar by less
 * than 3 dB, more than 12 dB over the floor. A frame that loud is a talker, so
 * it sets the long hangover.
 */
static void hangover_loud_test(void)
{
	struct hangover_result result;
	struct hangover_line line;

	CASE("lec hangover lasts 24 frames on a marginal verdict far over the "
	     "floor");
	hangover_floor_line_train(&line, -10.0, READING_TRUST_TRAIN_FRAMES);
	hangover_verdict_run(&line, "marginal frame", HANGOVER_LOUD_NEAR_DBM0,
			     &result);
	CHECK(result.verdict.dt_fired);
	CHECK_UINT(result.verdict.hold_left, 0);
	CHECK(result.verdict.mic_power <
	      HANGOVER_MARGINAL_RATIO * result.verdict.bar);
	CHECK(result.verdict.noise_power > 0.0f);
	CHECK(result.verdict.mic_power >=
	      HANGOVER_SOFT_RATIO * result.verdict.noise_power);
	CHECK_UINT(result.lasted, 23);
	CHECK_UINT(result.fired, 1);
	CHECK_UINT(result.dt, 24);
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * A verdict inside a running hangover, on the fixture of hangover_floor_test.
 * A floor excursion sets a hangover of 4 frames. On the next frame a second
 * excursion meets that hangover and the repeat limit, and sets the long
 * hangover. It stands under twice the bar and within 12 dB of the floor. A
 * detector with neither the hangover check nor the repeat limit gives it the
 * short hangover and fails here.
 */
static void hangover_rearm_test(void)
{
	struct udsp_ec_nlms_info before, after;
	struct udsp_ec_nlms_frame reading;
	struct hangover_line line;
	unsigned lasted;

	CASE("lec hangover lasts 24 frames on a verdict inside a hangover");
	hangover_floor_line_create(&line, HANGOVER_SOFT_DBM0);
	udsp_ec_nlms_info_get(line.ec, &before);
	hangover_near_run(&line, HANGOVER_EXCURSION_DBM0, &reading, &after);
	hangover_verdict_print("first excursion", HANGOVER_EXCURSION_DBM0,
			       &reading);
	hangover_short_check(&reading);
	hangover_near_run(&line, HANGOVER_EXCURSION_DBM0, &reading, &after);
	hangover_verdict_print("second excursion", HANGOVER_EXCURSION_DBM0,
			       &reading);
	CHECK(reading.dt_fired);
	CHECK_UINT(reading.hold_left, 4);
	CHECK(reading.mic_power < HANGOVER_MARGINAL_RATIO * reading.bar);
	CHECK(reading.mic_power < HANGOVER_SOFT_RATIO * reading.noise_power);
	lasted = hangover_far_run(&line, HANGOVER_FAR_LIMIT);
	udsp_ec_nlms_info_get(line.ec, &after);
	printf("# double talk for %u far frames: fired %u\n", lasted,
	       after.nlp_dt_fired_frames - before.nlp_dt_fired_frames);
	CHECK_UINT(lasted, 23);
	CHECK_UINT(after.nlp_dt_fired_frames - before.nlp_dt_fired_frames, 2);
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * The edge of the short hangover at twice the bar, on the fixture of
 * hangover_floor_test. A frame about 2.5 dB over the bar takes the short
 * hangover. A frame about 3.5 dB over the bar takes the long hangover. Both
 * stand within 12 dB of the floor. A marginal ratio of 1.5 or 2.5 fails one of
 * the two.
 */
static void hangover_marginal_edge_test(void)
{
	struct hangover_result result;
	struct hangover_line line;

	CASE("lec double talk takes the short hangover just under twice the "
	     "bar");
	hangover_floor_line_create(&line, HANGOVER_SOFT_DBM0);
	hangover_verdict_run(&line, "frame under twice the bar",
			     HANGOVER_UNDER_TWICE_DBM0, &result);
	hangover_short_check(&result.verdict);
	CHECK_UINT(result.lasted, 3);
	CHECK_UINT(result.fired, 1);
	udsp_ec_nlms.destroy(line.ec);

	CASE("lec double talk takes the long hangover just over twice the bar");
	hangover_floor_line_create(&line, HANGOVER_SOFT_DBM0);
	hangover_verdict_run(&line, "frame over twice the bar",
			     HANGOVER_OVER_TWICE_DBM0, &result);
	CHECK(result.verdict.dt_fired);
	CHECK_UINT(result.verdict.hold_left, 0);
	CHECK(result.verdict.mic_power >=
	      HANGOVER_MARGINAL_RATIO * result.verdict.bar);
	CHECK(result.verdict.mic_power <
	      HANGOVER_SOFT_RATIO * result.verdict.noise_power);
	CHECK_UINT(result.lasted, 23);
	CHECK_UINT(result.fired, 1);
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * The edge of the short hangover at 12 dB over the floor, on the fixture of
 * hangover_floor_test. A louder far end raises the estimate, so each frame
 * stays within 3 dB of the bar. A frame about 10 dB over the floor takes the
 * short hangover, and one about 14 dB over it takes the long hangover. A soft
 * ratio of 8 or 32 fails one of the two.
 */
static void hangover_soft_edge_test(void)
{
	struct hangover_result result;
	struct hangover_line line;

	CASE("lec double talk takes the short hangover just under 12 dB over "
	     "the floor");
	hangover_floor_line_create(&line, HANGOVER_UNDER_SOFT_FAR_DBM0);
	hangover_verdict_run(&line, "frame under 12 dB over the floor",
			     HANGOVER_UNDER_SOFT_DBM0, &result);
	hangover_short_check(&result.verdict);
	CHECK_UINT(result.lasted, 3);
	CHECK_UINT(result.fired, 1);
	udsp_ec_nlms.destroy(line.ec);

	CASE("lec double talk takes the long hangover just over 12 dB over "
	     "the floor");
	hangover_floor_line_create(&line, HANGOVER_OVER_SOFT_FAR_DBM0);
	hangover_verdict_run(&line, "frame over 12 dB over the floor",
			     HANGOVER_OVER_SOFT_DBM0, &result);
	CHECK(result.verdict.dt_fired);
	CHECK_UINT(result.verdict.hold_left, 0);
	CHECK(result.verdict.mic_power <
	      HANGOVER_MARGINAL_RATIO * result.verdict.bar);
	CHECK(result.verdict.noise_power > 0.0f);
	CHECK(result.verdict.mic_power >=
	      HANGOVER_SOFT_RATIO * result.verdict.noise_power);
	CHECK_UINT(result.lasted, 23);
	CHECK_UINT(result.fired, 1);
	udsp_ec_nlms.destroy(line.ec);
}

/* A fresh instance with the processor off, on a flat 8 ms hybrid. The filter
 * converges for 150 frames with the near-end floor on, and a far-end pause
 * measures the floor. */
static void hangover_talk_line_create(struct hangover_line *line,
				      double erl_db, double far_dbm0,
				      double floor_dbm0)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	unsigned f;

	memset(line, 0, sizeof(*line));
	line->state = 0x0badc0deu;
	line->near_state = 0xfeedfaceu;
	line->floor_state = 0x0f100f10u;
	line->far_dbm0 = far_dbm0;
	line->floor_dbm0 = floor_dbm0;
	line->floor = true;
	line->ec = udsp_ec_nlms.create(48);
	udsp_ec_nlms.nlp(line->ec, false);
	path_flat_init(&line->path, UDSP_SAMPLE_RATE, 8, erl_db);
	for (f = 0; f < FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2; f++)
		hangover_frame_run(line, NULL, &reading, &info);
	line->pause = true;
	for (f = 0; f < READING_PAUSE_FRAMES; f++)
		hangover_frame_run(line, NULL, &reading, &info);
	line->pause = false;
}

/* One talk frame at near_dbm0. It returns true when the filter adapted on
 * the frame. */
static bool hangover_talk_step(struct hangover_line *line, double near_dbm0,
			       struct udsp_ec_nlms_frame *reading,
			       struct udsp_ec_nlms_info *info)
{
	uint32_t held;

	udsp_ec_nlms_info_get(line->ec, info);
	held = info->hold_frames;
	hangover_near_run(line, near_dbm0, reading, info);
	return info->hold_frames == held;
}

#define HANGOVER_TALK_FLOOR_DBM0	-34.0
#define HANGOVER_TALK_DBM0		-19.0
#define HANGOVER_TALK_FRAMES		300u
#define HANGOVER_TALK_ADAPT_MAX		8u

/*
 * The talker of doubletalk_probe stands 3 dB over the echo of a 12 dB hybrid.
 * The filter converges with a near-end floor 12 dB under the echo on the
 * line, and a far-end pause measures that floor. The floor keeps the tracked
 * ERLE under 18 dB, so the detector uses the loose classic bar. The first
 * talk frames stand under that bar and adapt: 5 on this seed, all before the
 * first verdict. Every verdict stands 16 to 18 dB over the floor, so the soft
 * ratio alone gives it the long hangover. The case fails when more than 8 of
 * the 300 talk frames adapt.
 */
static void hangover_talker_test(void)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info start, info;
	struct hangover_line line;
	unsigned first = HANGOVER_TALK_FRAMES;
	unsigned adapted = 0;
	unsigned early = 0;
	double lowest = 1e9;
	unsigned f;

	hangover_talk_line_create(&line, 12.0, -10.0,
				  HANGOVER_TALK_FLOOR_DBM0);
	udsp_ec_nlms_info_get(line.ec, &start);
	for (f = 0; f < HANGOVER_TALK_FRAMES; f++) {
		bool adapt = hangover_talk_step(&line, HANGOVER_TALK_DBM0,
						&reading, &info);
		double over;

		adapted += adapt;
		if (first == HANGOVER_TALK_FRAMES)
			early += adapt;
		if (!reading.dt_fired)
			continue;
		if (first == HANGOVER_TALK_FRAMES)
			first = f;
		over = ratio_db_get(reading.mic_power, reading.noise_power);
		if (over < lowest)
			lowest = over;
	}

	CASE("lec double talk keeps a talker 3 dB over the echo out of the "
	     "filter on a line with a floor");
	printf("# floor %.2f dBov, tracked %.2f dBov, %u of %u talk frames "
	       "adapted, %u before the first verdict on frame %u, lowest "
	       "verdict %.2f dB over the floor, probes %u, tracked erle "
	       "%.1f dB before the talk and %.1f dB after it\n",
	       floor_dbov_get(HANGOVER_TALK_FLOOR_DBM0),
	       (double)start.noise_dbov, adapted, HANGOVER_TALK_FRAMES, early,
	       first, lowest, info.probes - start.probes,
	       (double)start.erle_db, (double)info.erle_db);
	CHECK(fabs(start.noise_dbov -
		   floor_dbov_get(HANGOVER_TALK_FLOOR_DBM0)) <= 1.0);
	CHECK(adapted <= HANGOVER_TALK_ADAPT_MAX);
	udsp_ec_nlms.destroy(line.ec);
}

#define HANGOVER_HEADROOM_DBM0	-37.0

/*
 * The upper bound of the floor headroom, on the fixture of
 * reading_floor_test, trained until the trust count completes. The far end
 * stops, and the second frame of the pause is a tail frame whose estimate is
 * under half the floor. The bar there stands 4.5 dB over the floor. A near-end
 * frame 5.5 dB over the floor must fire. A headroom of 3 puts the bar 6.3 dB
 * over the floor and fails here.
 */
static void hangover_headroom_test(void)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct hangover_line line;

	CASE("lec double talk fires on a frame 5.5 dB over the floor");
	hangover_floor_line_train(&line, -10.0, READING_TRUST_TRAIN_FRAMES);
	line.pause = true;
	hangover_frame_run(&line, NULL, &reading, &info);
	hangover_near_run(&line, HANGOVER_HEADROOM_DBM0, &reading, &info);
	printf("# tail frame at %.1f dBm0: %.2f dB over the floor, estimate "
	       "%.2f dB under it, bar %.2f dB over it\n",
	       HANGOVER_HEADROOM_DBM0,
	       ratio_db_get(reading.mic_power, reading.noise_power),
	       ratio_db_get(reading.noise_power, reading.estimate_power),
	       ratio_db_get(reading.bar, reading.noise_power));
	CHECK(reading.ref_quiet);
	CHECK(reading.estimate_power * 2.0f < reading.noise_power);
	/* 4.0 is 6 dB: the frame stands under the bar of a headroom of 3. */
	CHECK(reading.mic_power < 4.0f * reading.noise_power);
	CHECK(reading.dt_fired);
	udsp_ec_nlms.destroy(line.ec);
}

/* A floor excursion on the fixture of hangover_floor_test, far-end frames,
 * and a second excursion on frame n after the first. */
static void hangover_repeat_run(unsigned n, struct hangover_result *result)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct hangover_line line;
	unsigned fired = 0;
	unsigned f;

	hangover_floor_line_create(&line, HANGOVER_SOFT_DBM0);
	hangover_near_run(&line, HANGOVER_EXCURSION_DBM0, &reading, &info);
	hangover_short_check(&reading);
	for (f = 1; f < n; f++) {
		hangover_frame_run(&line, NULL, &reading, &info);
		fired += reading.dt_fired;
	}
	printf("# %u frames between the excursions, %u verdicts among them\n",
	       n - 1u, fired);
	CHECK_UINT(fired, 0);
	hangover_verdict_run(&line, "second excursion",
			     HANGOVER_EXCURSION_DBM0, result);
	hangover_short_check(&result->verdict);
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * The repeat limit, on the fixture of hangover_floor_test. A floor excursion
 * sets a hangover of 4 frames, and far-end frames with no verdict follow. A
 * second excursion 24 frames after the first counts 23 frames without a
 * verdict, under the limit of 24, and takes the long hangover. One 25 frames
 * after the first takes the short hangover again. A talker whose marginal
 * frames fire every few frames therefore meets the long hangover.
 */
static void hangover_repeat_test(void)
{
	struct hangover_result result;

	CASE("lec double talk takes the long hangover on a verdict 24 frames "
	     "after the last");
	hangover_repeat_run(24u, &result);
	CHECK_UINT(result.lasted, 23);
	CHECK_UINT(result.fired, 1);

	CASE("lec double talk takes the short hangover on a verdict 25 frames "
	     "after the last");
	hangover_repeat_run(25u, &result);
	CHECK_UINT(result.lasted, 3);
	CHECK_UINT(result.fired, 1);
}

/*
 * The start of the repeat count. A reset keeps the measured floor and puts
 * the count back at the limit. An excursion on the frame after a reset
 * therefore takes the short hangover. A fresh instance starts at the limit
 * too. A far-end pause of 10 frames measures its floor, and an excursion on
 * frame 14 takes the short hangover. A count that starts at 0 gives either
 * excursion the long hangover.
 */
static void hangover_repeat_start_test(void)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct hangover_result result;
	struct hangover_line line;
	unsigned f;

	CASE("lec double talk takes the short hangover on the first excursion "
	     "after a reset");
	hangover_floor_line_create(&line, HANGOVER_SOFT_DBM0);
	hangover_near_run(&line, HANGOVER_EXCURSION_DBM0, &reading, &info);
	CHECK(reading.dt_fired);
	udsp_ec_nlms_reset(line.ec);
	hangover_verdict_run(&line, "excursion after the reset",
			     HANGOVER_EXCURSION_DBM0, &result);
	hangover_short_check(&result.verdict);
	CHECK_UINT(result.lasted, 3);
	udsp_ec_nlms.destroy(line.ec);

	CASE("lec double talk takes the short hangover on the first excursion "
	     "of a fresh instance");
	memset(&line, 0, sizeof(line));
	line.state = 0x77665544u;
	line.near_state = 0xfeedfaceu;
	line.floor_state = 0x0f100f10u;
	line.far_dbm0 = HANGOVER_SOFT_DBM0;
	line.floor_dbm0 = READING_FLOOR_DBM0;
	line.floor = true;
	line.ec = udsp_ec_nlms.create(48);
	path_flat_init(&line.path, UDSP_SAMPLE_RATE, 8, 12.0);
	line.pause = true;
	for (f = 0; f < READING_PAUSE_FRAMES; f++)
		hangover_frame_run(&line, NULL, &reading, &info);
	line.pause = false;
	for (f = 0; f < 3u; f++)
		hangover_frame_run(&line, NULL, &reading, &info);
	hangover_verdict_run(&line, "first excursion", HANGOVER_EXCURSION_DBM0,
			     &result);
	CHECK_UINT(result.verdict.frames, 14);
	hangover_short_check(&result.verdict);
	CHECK_UINT(result.lasted, 3);
	udsp_ec_nlms.destroy(line.ec);
}

#define HANGOVER_SPARSE_FRAMES		3000u
/* 415 talk frames adapt on this seed; the bound adds 20 percent. */
#define HANGOVER_SPARSE_ADAPT_MAX	498u

/*
 * A talker who fires every fifth frame, which the repeat limit must cover.
 * A 24 dB hybrid carries a far end at -20 dBm0 over a floor at -45 dBm0 that
 * a far-end pause measures. Every fifth near-end frame is at -36 dBm0 and the
 * frames between are at -50 dBm0. A verdict within 24 frames of the last one
 * takes the long hangover. On this seed 400 of the 415 adapted frames fall in
 * the 4 trials of the latch escape. Without the repeat limit, 2930 of the 3000
 * talk frames adapt.
 */
static void hangover_sparse_test(void)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info start, info;
	struct hangover_line line;
	unsigned adapted = 0;
	unsigned in_trial = 0;
	unsigned fired = 0;
	double erle;
	unsigned f;

	hangover_talk_line_create(&line, 24.0, -20.0, -45.0);
	udsp_ec_nlms_info_get(line.ec, &start);
	for (f = 0; f < HANGOVER_SPARSE_FRAMES; f++) {
		bool adapt = hangover_talk_step(&line,
						f % 5u ? -50.0 : -36.0,
						&reading, &info);

		adapted += adapt;
		in_trial += adapt && reading.probing;
		fired += reading.dt_fired;
	}
	udsp_ec_nlms.freeze(line.ec, true);
	erle = erle_run(line.ec, &line.path, &line.state, -20.0, 20u, 20u);

	CASE("lec repeat limit keeps a talker who fires every fifth frame out "
	     "of the filter");
	printf("# %u of %u talk frames adapted, %u of them in %u trials, %u "
	       "verdicts, resets %u, true erle %.1f dB after the talk\n",
	       adapted, HANGOVER_SPARSE_FRAMES, in_trial,
	       info.probes - start.probes, fired,
	       info.diverge_resets - start.diverge_resets, erle);
	CHECK(adapted <= HANGOVER_SPARSE_ADAPT_MAX);
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * The call of the replay cases. Far-end bursts of noise pass through the
 * composite hybrid, a near-end floor is on every frame, and near-end bursts
 * over the far end fire the detector. The first instance has one reset, one
 * freeze stretch, one stretch with the processor off and one stretch of FEED
 * records for an open loop. It also has one skip of the engine: the tick
 * jumps, the instance sees nothing, and the next record carries SKIPPED and
 * the count. The second instance starts on FEED records for an inactive
 * stream.
 */
#define REPLAY_PERIOD		70u
#define REPLAY_FAR_ON		40u
#define REPLAY_FAR_DBM0		-10.0
#define REPLAY_NEAR_DBM0	-15.0
#define REPLAY_FLOOR_DBM0	-55.0
#define REPLAY_ERL_DB		12.0
#define REPLAY_NEAR_FRAMES	6u
#define REPLAY_FIRST_FRAMES	1000u
#define REPLAY_SECOND_FRAMES	200u
#define REPLAY_SECOND_FEEDS	5u
#define REPLAY_RESET_AT		500u
#define REPLAY_FREEZE_FROM	380u
#define REPLAY_NLP_OFF_FROM	700u
#define REPLAY_STRETCH		50u
#define REPLAY_FEED_FROM	800u
#define REPLAY_FEED_FRAMES	20u
#define REPLAY_TICK_GAP		500u
#define REPLAY_LOSS_AT		600u
#define REPLAY_SKIP_AT		650u
#define REPLAY_SKIP_TICKS	3u
#define REPLAY_SEGMENTS		3u
#define REPLAY_RECORDS		(REPLAY_FIRST_FRAMES + REPLAY_SECOND_FRAMES + 2u)
#define REPLAY_TICK_NS		10000000ull

struct replay_call {
	unsigned rate;
	struct echo_path path;
	uint32_t far_state;
	uint32_t floor_state;
	uint32_t near_state;
	struct udsp_ec *ec;
	bool nlp;
	bool failed;
	uint16_t gen;
	uint64_t tick;
	uint16_t skip_next;
	struct lecr_record *recs;
	unsigned count;
	struct lecr_tally seg_info[REPLAY_SEGMENTS];
	unsigned segments;
	unsigned runs;
	unsigned feeds;
	unsigned off_frames;
	unsigned freeze_frames;
	unsigned fired;
};

static bool replay_talk_get(unsigned g)
{
	static const unsigned from[] = { 300u, 630u, 910u, 1140u };
	unsigned i;

	for (i = 0; i < sizeof(from) / sizeof(from[0]); i++)
		if (g >= from[i] && g < from[i] + REPLAY_NEAR_FRAMES)
			return true;
	return false;
}

static void replay_frame_make(struct replay_call *c, unsigned g,
			      struct lecr_record *rec)
{
	int16_t echo[UDSP_FRAME_MAX];
	int16_t hiss[UDSP_FRAME_MAX];
	int16_t near[UDSP_FRAME_MAX];
	unsigned n = c->path.frame;
	bool talk = replay_talk_get(g);
	unsigned i;

	if (g % REPLAY_PERIOD < REPLAY_FAR_ON)
		noise_fill(rec->ref, n, REPLAY_FAR_DBM0, &c->far_state);
	path_run(&c->path, rec->ref, echo);
	noise_fill(hiss, n, REPLAY_FLOOR_DBM0, &c->floor_state);
	if (talk)
		noise_fill(near, n, REPLAY_NEAR_DBM0, &c->near_state);
	for (i = 0; i < n; i++)
		rec->mic[i] = sample_clamp((double)echo[i] + hiss[i] +
					   (talk ? near[i] : 0));
}

/* The counters of the instance at the end of a segment: before a reset and
 * before the instance goes. */
static void replay_segment_end(struct replay_call *c)
{
	struct udsp_ec_nlms_info info;

	udsp_ec_nlms_info_get(c->ec, &info);
	if (c->segments < REPLAY_SEGMENTS)
		lecr_tally_set(&c->seg_info[c->segments], &info);
	c->segments++;
}

/* One tick of the line in the order of chan_lec_run of core/chan.c: the
 * feed on a skip, else the processor switch, the reset, the freeze and the
 * run. feed is the SKIP flag of a FEED, or 0 for a RUN. */
static void replay_tick_run(struct replay_call *c, unsigned g, bool fresh,
			    uint16_t feed, bool reset, bool freeze, bool nlp)
{
	struct lecr_record *rec;

	if (c->failed)
		return;
	rec = &c->recs[c->count++];
	memset(rec, 0, sizeof(*rec));
	replay_frame_make(c, g, rec);
	rec->samples = (uint16_t)c->path.frame;
	rec->tick = c->tick++;
	rec->stamp_ns = rec->tick * REPLAY_TICK_NS;
	if (c->skip_next) {
		rec->flags |= LECR_FLAG_SKIPPED;
		rec->skipped = c->skip_next;
		c->skip_next = 0;
	}
	if (fresh) {
		c->ec = udsp_ec_nlms.create_rate(0, c->rate);
		c->nlp = true;
		c->gen++;
		rec->flags |= LECR_FLAG_NEW;
		if (!c->ec) {
			c->failed = true;
			return;
		}
	}
	if (feed) {
		udsp_ec_nlms_ref_feed(c->ec, rec->ref);
		rec->kind = LECR_KIND_FEED;
		rec->flags |= feed;
		c->feeds++;
	} else {
		if (c->nlp != nlp) {
			c->nlp = nlp;
			udsp_ec_nlms.nlp(c->ec, nlp);
		}
		if (reset) {
			replay_segment_end(c);
			udsp_ec_nlms_reset(c->ec);
			c->gen++;
			rec->flags |= LECR_FLAG_RESET;
		}
		udsp_ec_nlms.freeze(c->ec, freeze);
		memcpy(rec->out, rec->mic, rec->samples * sizeof(*rec->out));
		udsp_ec_nlms.run(c->ec, rec->out, rec->ref);
		rec->kind = LECR_KIND_RUN;
		rec->flags |= (nlp ? LECR_FLAG_NLP : 0u) |
			      (freeze ? LECR_FLAG_FREEZE : 0u);
		c->runs++;
		c->off_frames += !nlp;
		c->freeze_frames += freeze;
	}
	rec->gen = c->gen;
	lecr_record_fill(rec, c->ec);
	c->fired += (rec->flags & LECR_FLAG_FIRED) != 0;
}

static void replay_end_push(struct replay_call *c)
{
	struct lecr_record *rec;

	if (c->failed)
		return;
	replay_segment_end(c);
	rec = &c->recs[c->count++];
	memset(rec, 0, sizeof(*rec));
	rec->kind = LECR_KIND_END;
	rec->samples = (uint16_t)c->path.frame;
	rec->tick = c->tick;
	rec->stamp_ns = rec->tick * REPLAY_TICK_NS;
	rec->gen = c->gen;
	lecr_record_fill(rec, c->ec);
	udsp_ec_nlms.destroy(c->ec);
	c->ec = NULL;
}

static bool replay_stretch_get(unsigned f, unsigned from, unsigned frames)
{
	return f >= from && f < from + frames;
}

static bool replay_call_build(struct replay_call *c, unsigned rate)
{
	unsigned f;

	memset(c, 0, sizeof(*c));
	c->recs = calloc(REPLAY_RECORDS, sizeof(*c->recs));
	if (!c->recs)
		return false;
	c->rate = rate;
	c->far_state = 0x13579bdfu;
	c->floor_state = 0x2468ace0u;
	c->near_state = 0x0badcafeu;
	path_composite_init(&c->path, rate, REPLAY_ERL_DB);
	for (f = 0; f < REPLAY_FIRST_FRAMES; f++) {
		if (f == REPLAY_SKIP_AT) {
			c->tick += REPLAY_SKIP_TICKS;
			c->skip_next = REPLAY_SKIP_TICKS;
		}
		replay_tick_run(c, f, f == 0u,
				replay_stretch_get(f, REPLAY_FEED_FROM,
						   REPLAY_FEED_FRAMES) ?
				LECR_FLAG_SKIP_OPEN : 0u,
				f == REPLAY_RESET_AT,
				replay_stretch_get(f, REPLAY_FREEZE_FROM,
						   REPLAY_STRETCH),
				!replay_stretch_get(f, REPLAY_NLP_OFF_FROM,
						    REPLAY_STRETCH));
	}
	replay_end_push(c);
	c->tick += REPLAY_TICK_GAP;
	for (f = 0; f < REPLAY_SECOND_FRAMES; f++)
		replay_tick_run(c, REPLAY_FIRST_FRAMES + f, f == 0u,
				f < REPLAY_SECOND_FEEDS ?
				LECR_FLAG_SKIP_INACTIVE : 0u, false, false,
				true);
	replay_end_push(c);
	return !c->failed;
}

/* The records through the writer into memory. The caller frees *buf. */
static bool replay_file_write(const struct lecr_record *recs, unsigned count,
			      unsigned rate, char **buf, size_t *size)
{
	struct lecr_file file;
	FILE *f = open_memstream(buf, size);
	bool ok;
	unsigned i;

	if (!f)
		return false;
	ok = !lecr_write_open(f, &file, 0, rate);
	for (i = 0; ok && i < count; i++)
		ok = !lecr_write_record(f, &file, &recs[i]);
	ok = ok && !lecr_write_close(f, &file);
	if (fclose(f))
		ok = false;
	return ok;
}

/*
 * What the replay of a file saw. twin_frames counts the RUN frames on which
 * neither this frame nor the one before read ACTIVE. On them the processor
 * left the residual alone, so the shadow output must equal the replay
 * output.
 */
struct replay_seen {
	struct lecr_segment seg[REPLAY_SEGMENTS];
	unsigned segments;
	unsigned runs;
	unsigned feeds;
	unsigned diffs;
	unsigned twin_frames;
	unsigned twin_same;
	bool active_before;
};

static void replay_seen_add(void *ctx, const struct lecr_record *rec,
			    const struct lecr_replay *r,
			    const struct lecr_step *st,
			    const struct lecr_agree *a, bool closed)
{
	struct replay_seen *seen = ctx;
	bool active;

	if (closed && seen->segments < REPLAY_SEGMENTS)
		seen->seg[seen->segments] = a->done;
	seen->segments += closed;
	if (!rec || !st->replayed)
		return;
	if (rec->kind == LECR_KIND_FEED) {
		seen->feeds++;
		return;
	}
	seen->runs++;
	seen->diffs += st->diff != 0u;
	active = (rec->flags & LECR_FLAG_ACTIVE) != 0;
	if (r->shadow && !active && !seen->active_before && !st->fresh &&
	    !(rec->flags & LECR_FLAG_RESET)) {
		seen->twin_frames++;
		seen->twin_same += !memcmp(r->out, r->residual,
					   rec->samples * sizeof(*r->out));
	}
	seen->active_before = active;
}

/* The steady-state figures leave out the first skip_s of each segment. */
static int replay_buffer_skip_run(char *buf, size_t size, bool shadow,
				  unsigned skip_s, struct lecr_agree *a,
				  struct replay_seen *seen)
{
	struct lecr_replay r;
	struct lecr_file file;
	FILE *f = fmemopen(buf, size, "rb");
	int err;

	memset(seen, 0, sizeof(*seen));
	lecr_agree_init(a, skip_s);
	if (!f)
		return -ENOMEM;
	lecr_replay_init(&r, shadow);
	err = lecr_stream_replay(f, &file, &r, a, replay_seen_add, seen);
	fclose(f);
	return err;
}

static int replay_buffer_run(char *buf, size_t size, bool shadow,
			     struct lecr_agree *a, struct replay_seen *seen)
{
	return replay_buffer_skip_run(buf, size, shadow, 0, a, seen);
}

/* A copy of the records of a call that the caller edited, through the
 * writer and the replay. The answer is false when the file was not
 * written. */
static bool replay_recs_run(const struct replay_call *c,
			    const struct lecr_record *recs, unsigned skip_s,
			    struct lecr_agree *a, struct replay_seen *seen)
{
	char *buf = NULL;
	size_t size = 0;
	bool ok = replay_file_write(recs, c->count, c->rate, &buf, &size);

	if (ok)
		replay_buffer_skip_run(buf, size, false, skip_s, a, seen);
	free(buf);
	return ok;
}

/* The call without record drop, and with lost_before set on the record
 * that follows it, through the writer and the replay. */
static bool replay_drop_run(const struct replay_call *c, unsigned drop,
			    uint32_t lost, struct lecr_agree *a,
			    struct replay_seen *seen)
{
	struct lecr_record *recs = malloc(c->count * sizeof(*recs));
	char *buf = NULL;
	size_t size = 0;
	bool ok;

	if (!recs)
		return false;
	memcpy(recs, c->recs, drop * sizeof(*recs));
	memcpy(recs + drop, c->recs + drop + 1u,
	       (c->count - drop - 1u) * sizeof(*recs));
	recs[drop].lost_before = lost;
	ok = replay_file_write(recs, c->count - 1u, c->rate, &buf, &size) &&
	     !replay_buffer_run(buf, size, false, a, seen);
	free(buf);
	free(recs);
	return ok;
}

/* UDSP_TEST_LECR_OUT=<prefix> keeps a copy of the file of each rate, as
 * <prefix>-8k.lecr and <prefix>-16k.lecr, for a run of lec_replay. */
static void replay_file_keep(const char *buf, size_t size, unsigned rate)
{
	const char *prefix = getenv("UDSP_TEST_LECR_OUT");
	char path[512];
	FILE *f;

	if (!prefix)
		return;
	snprintf(path, sizeof(path), "%s-%uk.lecr", prefix, rate / 1000u);
	f = fopen(path, "wb");
	CHECK(f != NULL);
	if (!f)
		return;
	CHECK_UINT(fwrite(buf, 1, size, f), size);
	CHECK(!fclose(f));
	printf("# kept %s\n", path);
}

/* The raw bytes of a record against the offsets of the lec_replay.h table:
 * frames at 20, flags at 46, kind at 59, the verdict at 60 and the planes
 * from 64, all little-endian. */
static void replay_bytes_check(const uint8_t *b, const struct lecr_record *rec)
{
	unsigned n = rec->samples;

	CHECK_UINT(b[20] | b[21] << 8 | b[22] << 16 | (uint32_t)b[23] << 24,
		   rec->frames);
	CHECK_UINT(b[46] | b[47] << 8, rec->flags);
	CHECK_UINT(b[48] | b[49] << 8, n);
	CHECK_UINT(b[59], rec->kind);
	CHECK_UINT(b[60], rec->nlp_verdict);
	CHECK_UINT(b[62] | b[63], 0);
	CHECK_UINT((uint16_t)(b[64] | b[65] << 8), (uint16_t)rec->ref[0]);
	CHECK_UINT((uint16_t)(b[64 + 2 * n] | b[65 + 2 * n] << 8),
		   (uint16_t)rec->mic[0]);
	CHECK_UINT((uint16_t)(b[64 + 4 * n] | b[65 + 4 * n] << 8),
		   (uint16_t)rec->out[0]);
}

/*
 * The file of the synthetic call through the writer and the reader. The
 * file header carries the magic, version 2, the rate, the samples, the count
 * and the NEW flag at the offsets of the table. Every record reads back equal
 * to the record written. A writer that swaps two planes or writes big-endian
 * fails the byte checks.
 */
static void replay_file_check(const struct replay_call *c, char *buf,
			      size_t size)
{
	unsigned n = c->path.frame;
	size_t record = LECR_HEAD_BYTES + 6u * n;
	unsigned probe = REPLAY_FEED_FROM - 1u;
	struct lecr_record rec;
	struct lecr_file file;
	unsigned same = 0;
	unsigned i;
	FILE *f;

	CHECK_UINT(size, LECR_FILE_BYTES + c->count * record);
	if (size != LECR_FILE_BYTES + c->count * record)
		return;
	CHECK(!memcmp(buf, "UDSPLECR", 8));
	CHECK_UINT(buf[8] | buf[9] << 8, 2);
	CHECK_UINT((uint8_t)buf[12] | (uint8_t)buf[13] << 8, c->rate);
	CHECK_UINT((uint8_t)buf[16], n);
	CHECK_UINT((uint8_t)buf[20] | (uint8_t)buf[21] << 8, c->count);
	CHECK_UINT(buf[24], LECR_FILE_FIRST_NEW);
	replay_bytes_check((const uint8_t *)buf + LECR_FILE_BYTES +
			   probe * record, &c->recs[probe]);
	f = fmemopen(buf, size, "rb");
	CHECK(f != NULL);
	if (!f)
		return;
	if (lecr_read_open(f, &file)) {
		CHECK(!"the reader refuses the file header");
		fclose(f);
		return;
	}
	CHECK_UINT(file.rate, c->rate);
	CHECK_UINT(file.samples, n);
	CHECK_UINT(file.records, c->count);
	for (i = 0; i < c->count && lecr_read_record(f, &file, &rec) == 1; i++)
		same += !memcmp(&rec, &c->recs[i], sizeof(rec));
	CHECK_UINT(same, c->count);
	CHECK(!lecr_read_record(f, &file, &rec));
	fclose(f);
}

/*
 * The replay of a synthetic call (bench/lec_replay.h) at the rate given. A
 * live instance runs the call in the order of chan_lec_run, writes one record
 * per tick, and the file keeps every field. Each RUN replays bit for bit, the
 * shadow output matches where the processor did not act, and each segment
 * tally equals the live counters. A file without its first NEW reads
 * INCOMPLETE, and a frames mismatch or a gap reads FAIL. A gap is a jump
 * without a loss that the skip count does not explain, and a loss drops its
 * segment. A replay that ignores RESET, FREEZE, NLP, FEED, NEW or SKIPPED
 * fails, and so does a tally that counts a pass without ACTIVE as active.
 */
static void replay_call_test(unsigned rate)
{
	struct replay_seen seen;
	struct replay_call call;
	struct lecr_agree a;
	char *buf = NULL;
	size_t size = 0;
	unsigned total[LECR_COUNTS] = { 0 };
	unsigned probe;
	unsigned i, k;
	bool built = replay_call_build(&call, rate);
	bool written = built && replay_file_write(call.recs, call.count, rate,
						  &buf, &size);

	case_rate(rate,
		  "lec replay file keeps every field of a synthetic call");
	CHECK(built);
	CHECK(written);
	CHECK_UINT(call.count, REPLAY_RECORDS);
	if (!written) {
		free(buf);
		free(call.recs);
		return;
	}
	replay_file_check(&call, buf, size);
	replay_file_keep(buf, size, rate);

	case_rate(rate, "lec replay reproduces a synthetic call bit for bit");
	CHECK(!replay_buffer_run(buf, size, true, &a, &seen));
	printf("# %u runs, %u feeds replayed, %u differ; %u twin frames, %u "
	       "equal\n", seen.runs, seen.feeds, seen.diffs, seen.twin_frames,
	       seen.twin_same);
	CHECK_UINT(seen.runs, call.runs);
	CHECK_UINT(seen.feeds, call.feeds);
	CHECK_UINT(seen.diffs, 0);
	CHECK(seen.twin_frames > 0u);
	CHECK_UINT(seen.twin_same, seen.twin_frames);
	CHECK_UINT(a.instances, 2);
	CHECK_UINT(a.resets, 1);
	CHECK_UINT(a.frames_bad, 0);
	CHECK_UINT(a.evaluated, REPLAY_SEGMENTS);
	/* The skip of the engine is a jump with SKIPPED, not a gap. */
	CHECK_UINT(a.skip_marks, 1);
	CHECK_UINT(a.skip_periods, REPLAY_SKIP_TICKS);
	CHECK_UINT(a.gaps, 0);
	CHECK_UINT(lecr_agree_result(&a), LECR_PASS);

	case_rate(rate, "lec replay tally equals the canceller counters of "
		  "each segment");
	CHECK_UINT(seen.segments, REPLAY_SEGMENTS);
	CHECK_UINT(call.segments, REPLAY_SEGMENTS);
	CHECK_UINT(a.losses + a.gaps + a.skipped, 0);
	for (i = 0; i < REPLAY_SEGMENTS && i < seen.segments; i++) {
		for (k = 0; k < LECR_COUNTS; k++) {
			CHECK_UINT(seen.seg[i].live.n[k],
				   call.seg_info[i].n[k]);
			CHECK_UINT(seen.seg[i].replay.n[k],
				   call.seg_info[i].n[k]);
			total[k] += seen.seg[i].live.n[k];
		}
	}
	printf("# active %u wait %u erle %u unlike %u dt %u quiet %u; fired "
	       "%u, off %u, frozen %u, feeds %u\n", total[LECR_COUNT_ACTIVE],
	       total[LECR_COUNT_WAIT], total[LECR_COUNT_ERLE],
	       total[LECR_COUNT_UNLIKE], total[LECR_COUNT_DT],
	       total[LECR_COUNT_QUIET], call.fired, call.off_frames,
	       call.freeze_frames, call.feeds);
	CHECK(total[LECR_COUNT_ACTIVE] > 0u);
	CHECK(total[LECR_COUNT_WAIT] > 0u);
	CHECK(total[LECR_COUNT_DT] > 0u);
	CHECK(total[LECR_COUNT_QUIET] > 0u);
	CHECK(call.fired > 0u);
	CHECK_UINT(call.off_frames, REPLAY_STRETCH);
	CHECK_UINT(call.freeze_frames, REPLAY_STRETCH);
	free(buf);
	buf = NULL;

	case_rate(rate, "lec replay reads a capture without its first NEW as "
		  "incomplete");
	call.recs[0].flags &= (uint16_t)~LECR_FLAG_NEW;
	CHECK(replay_file_write(call.recs, call.count, rate, &buf, &size));
	CHECK(buf && !replay_buffer_run(buf, size, false, &a, &seen));
	CHECK_UINT(a.evaluated, 1);
	CHECK_UINT(a.skipped, REPLAY_FIRST_FRAMES);
	CHECK_UINT(lecr_agree_result(&a), LECR_INCOMPLETE);
	call.recs[0].flags |= LECR_FLAG_NEW;
	free(buf);
	buf = NULL;

	case_rate(rate, "lec replay reads a frames mismatch as a failure");
	probe = REPLAY_RESET_AT / 2u;
	call.recs[probe].frames++;
	CHECK(replay_file_write(call.recs, call.count, rate, &buf, &size));
	CHECK(buf && !replay_buffer_run(buf, size, false, &a, &seen));
	CHECK_UINT(a.frames_bad, 1);
	CHECK_UINT(a.failed, 0);
	CHECK_UINT(lecr_agree_result(&a), LECR_FAIL);
	call.recs[probe].frames--;
	free(buf);

	case_rate(rate, "lec replay drops a segment after a loss and fails a "
		  "tick gap");
	CHECK(replay_drop_run(&call, REPLAY_LOSS_AT, 0, &a, &seen));
	CHECK_UINT(a.gaps, 1);
	CHECK_UINT(a.losses, 0);
	CHECK_UINT(a.evaluated, REPLAY_SEGMENTS - 1u);
	CHECK_UINT(lecr_agree_result(&a), LECR_FAIL);
	CHECK(replay_drop_run(&call, REPLAY_LOSS_AT, 1, &a, &seen));
	CHECK_UINT(a.gaps, 0);
	CHECK_UINT(a.losses, 1);
	CHECK_UINT(a.lost, 1);
	CHECK_UINT(a.skipped, REPLAY_FIRST_FRAMES - 1u - REPLAY_LOSS_AT);
	CHECK_UINT(seen.segments, REPLAY_SEGMENTS);
	CHECK(seen.seg[1].lossy);
	CHECK_UINT(a.evaluated, REPLAY_SEGMENTS - 1u);
	CHECK_UINT(lecr_agree_result(&a), LECR_PASS);
	CHECK(replay_drop_run(&call, REPLAY_FIRST_FRAMES, 1, &a, &seen));
	CHECK_UINT(a.gaps, 0);
	CHECK_UINT(a.losses, 1);
	CHECK_UINT(a.instances, 2);
	CHECK_UINT(seen.segments, REPLAY_SEGMENTS);
	CHECK(seen.seg[1].lossy);
	CHECK_UINT(seen.diffs, 0);
	CHECK_UINT(a.evaluated, REPLAY_SEGMENTS - 1u);
	CHECK_UINT(lecr_agree_result(&a), LECR_PASS);

	case_rate(rate, "lec replay reads a skip that does not match its jump "
		  "as a gap, and a skip beside a loss as a loss");
	/* The record before the skip is lost from the archive, so the jump is
	 * one tick longer than the count explains. */
	CHECK(replay_drop_run(&call, REPLAY_SKIP_AT - 1u, 0, &a, &seen));
	CHECK_UINT(a.gaps, 1);
	CHECK_UINT(a.skip_marks, 0);
	CHECK_UINT(lecr_agree_result(&a), LECR_FAIL);
	CHECK(replay_drop_run(&call, REPLAY_SKIP_AT - 1u, 1, &a, &seen));
	CHECK_UINT(a.gaps, 0);
	CHECK_UINT(a.losses, 1);
	CHECK_UINT(a.skip_marks, 0);
	CHECK_UINT(a.evaluated, REPLAY_SEGMENTS - 1u);
	CHECK_UINT(lecr_agree_result(&a), LECR_PASS);
	buf = NULL;
	call.recs[REPLAY_SKIP_AT].skipped--;
	CHECK(replay_file_write(call.recs, call.count, rate, &buf, &size));
	CHECK(buf && !replay_buffer_run(buf, size, false, &a, &seen));
	CHECK_UINT(a.gaps, 1);
	CHECK_UINT(a.skip_marks, 0);
	CHECK_UINT(lecr_agree_result(&a), LECR_FAIL);
	call.recs[REPLAY_SKIP_AT].skipped++;
	free(buf);
	free(call.recs);
}

/* Sets the live record and the replay record of frame f of a driven
 * segment. */
typedef void (*replay_frame_edit)(unsigned f, struct lecr_record *rec,
				  struct lecr_record *got, const void *arg);

/* One segment of frames RUN records from a NEW, through the agreement alone
 * and without a canceller. The steady-state figures leave out skip_s. */
static void replay_seg_drive(struct lecr_agree *a, unsigned skip_s,
			     unsigned frames, replay_frame_edit edit,
			     const void *arg)
{
	struct lecr_record rec, got;
	struct lecr_step st;
	unsigned f;

	lecr_agree_init(a, skip_s);
	for (f = 0; f < frames; f++) {
		memset(&rec, 0, sizeof(rec));
		rec.kind = LECR_KIND_RUN;
		rec.samples = UDSP_FRAME_SAMPLES;
		rec.tick = f;
		rec.frames = f + 1u;
		rec.flags = f ? 0u : LECR_FLAG_NEW;
		got = rec;
		edit(f, &rec, &got, arg);
		memset(&st, 0, sizeof(st));
		st.fresh = f == 0u;
		st.replayed = true;
		st.diff = lecr_diff_get(&rec, &got, got.out, rec.samples,
					&st.out_delta);
		lecr_agree_add(a, &rec, &st, &got);
	}
	lecr_agree_finish(a);
}

static void replay_flip_edit(unsigned f, struct lecr_record *rec,
			     struct lecr_record *got, const void *arg)
{
	unsigned flipped = *(const unsigned *)arg;

	rec->nlp_verdict = f % 2u ? LECR_VERDICT_QUIET : LECR_VERDICT_ERLE;
	got->nlp_verdict = rec->nlp_verdict;
	if (f < flipped)
		got->nlp_verdict = f % 2u ? LECR_VERDICT_ERLE :
					    LECR_VERDICT_QUIET;
}

/* 1000 RUN frames that alternate two verdicts. The replay swaps the
 * verdicts of the first flipped frames in pairs, so the six counts and the
 * fired frames agree and only the verdict agreement can fail. */
static void replay_flip_run(unsigned flipped, struct lecr_agree *a)
{
	replay_seg_drive(a, 0, 1000u, replay_flip_edit, &flipped);
}

/*
 * The verdict agreement of lec_replay.h needs 95 percent or more of the
 * frames of a segment. 60 swapped frames in 1000 fail on the agreement
 * alone. 40 and 50 swapped frames pass. An agreement bar that is not
 * checked, or checked above 95 percent, fails here.
 */
static void replay_agree_test(void)
{
	struct lecr_agree a;
	unsigned i;

	CASE("lec replay agreement flags 6 percent of flipped verdicts");
	replay_flip_run(60u, &a);
	for (i = 0; i < LECR_COUNTS; i++)
		CHECK_UINT(a.done.live.n[i], a.done.replay.n[i]);
	CHECK_UINT(a.done.verdict_same, 940);
	CHECK_UINT(lecr_segment_faults(&a.done), LECR_FAULT_VERDICT);
	CHECK_UINT(lecr_agree_result(&a), LECR_FAIL);

	CASE("lec replay agreement passes 4 percent of flipped verdicts");
	replay_flip_run(40u, &a);
	CHECK_UINT(a.done.verdict_same, 960);
	CHECK_UINT(lecr_segment_faults(&a.done), 0);
	CHECK_UINT(lecr_agree_result(&a), LECR_PASS);
	replay_flip_run(50u, &a);
	CHECK_UINT(a.done.verdict_same, 950);
	CHECK_UINT(lecr_agree_result(&a), LECR_PASS);
}

/*
 * The verdict of the capture record has a stable order, and dsp/lec.c
 * writes its private order into the frame reading. The words of
 * udsp_ec_nlms_verdict_name for 0 to 5 must be the words of that stable
 * order. A reordered private enum fails here.
 */
static void replay_verdict_word_test(void)
{
	static const char *const words[] = {
		"off", "quiet", "dt", "unlike", "erle", "pass",
	};
	unsigned i;

	CASE("lec replay verdict order gives the words of the canceller");
	for (i = 0; i < sizeof(words) / sizeof(words[0]); i++)
		CHECK(!strcmp(udsp_ec_nlms_verdict_name(i), words[i]));
	CHECK(!strcmp(udsp_ec_nlms_verdict_name(LECR_VERDICT_OFF), "off"));
	CHECK(!strcmp(udsp_ec_nlms_verdict_name(LECR_VERDICT_QUIET), "quiet"));
	CHECK(!strcmp(udsp_ec_nlms_verdict_name(LECR_VERDICT_DT), "dt"));
	CHECK(!strcmp(udsp_ec_nlms_verdict_name(LECR_VERDICT_UNLIKE),
		      "unlike"));
	CHECK(!strcmp(udsp_ec_nlms_verdict_name(LECR_VERDICT_ERLE), "erle"));
	CHECK(!strcmp(udsp_ec_nlms_verdict_name(LECR_VERDICT_PASS), "pass"));
	CHECK(!strcmp(udsp_ec_nlms_verdict_name(6), "?"));
}

/* Sample k of record i, as fixture_records() of src/tools/lec-capture.py
 * writes it: ref 100 x i + k, mic i, and out -(100 x i + k) on a RUN. A
 * FEED has zeros in out, and an END has zeros in the three planes. */
static bool capture_fixture_planes_ok(const struct lecr_record *rec,
				      unsigned i)
{
	bool run = rec->kind == LECR_KIND_RUN;
	bool end = rec->kind == LECR_KIND_END;
	int base = (int)(100u * i);
	unsigned k;

	for (k = 0; k < 80u; k++) {
		if (rec->ref[k] != (end ? 0 : base + (int)k) ||
		    rec->mic[k] != (end ? 0 : (int)i) ||
		    rec->out[k] != (run ? -(base + (int)k) : 0))
			return false;
	}
	return true;
}

/*
 * The replay file of the ctest fixture test_lec_capture_fixture, through the
 * reader of bench/lec_replay.h. lec-capture.py writes it and ctest names it
 * in UDSP_LEC_CAPTURE_FIXTURE; the case skips when the variable is unset.
 * The expected values are literals here, and the docstring of
 * fixture_records() in the tool documents the same values. A writer that
 * swaps two planes, moves a field or writes big-endian fails here.
 */
static void capture_fixture_test(void)
{
	static const uint8_t kinds[] = {
		LECR_KIND_RUN, LECR_KIND_RUN, LECR_KIND_FEED,
		LECR_KIND_RUN, LECR_KIND_RUN, LECR_KIND_END,
	};
	static const uint16_t flags[] = {
		0x0009, 0x0048, 0x0100, 0x000e, 0x10b8, 0x0000,
	};
	static const uint32_t frames[] = { 1, 2, 2, 1, 2, 2 };
	static const uint8_t verdicts[] = { 5, 5, 0, 1, 2, 0 };
	const char *path = getenv("UDSP_LEC_CAPTURE_FIXTURE");
	struct lecr_record rec;
	struct lecr_file file;
	unsigned i;
	FILE *f;

	CASE("lec capture fixture reads back the documented values");
	if (!path) {
		tu_skip("UDSP_LEC_CAPTURE_FIXTURE not set");
		return;
	}
	f = fopen(path, "rb");
	CHECK(f != NULL);
	if (!f)
		return;
	if (lecr_read_open(f, &file)) {
		CHECK(!"the reader refuses the fixture header");
		fclose(f);
		return;
	}
	CHECK_UINT(file.line, 1);
	CHECK_UINT(file.rate, UDSP_SAMPLE_RATE);
	CHECK_UINT(file.samples, 80);
	CHECK_UINT(file.records, 6);
	CHECK_UINT(file.flags, LECR_FILE_FIRST_NEW | LECR_FILE_REF_LOSS);
	CHECK_UINT(file.ref_loss_half_db, 18);
	CHECK_UINT(lecr_file_ref_loss_get(&file), 18);
	for (i = 0; i < 6u && lecr_read_record(f, &file, &rec) == 1; i++) {
		CHECK_UINT(rec.stamp_ns, 1000000000ull + 10000000ull * i);
		CHECK_UINT(rec.tick, 1000u + i);
		CHECK_UINT(rec.lost_before, i == 4u ? 7 : 0);
		CHECK_UINT(rec.frames, frames[i]);
		CHECK_UINT(rec.hold_frames, i);
		CHECK(rec.erle_mdb == -1500 * (int32_t)i);
		CHECK_UINT(rec.echo_gain_micro, 250000);
		CHECK_UINT(rec.coef_energy_micro, 1000u * i);
		CHECK_UINT(rec.ref_delay_samples, 80u + i);
		CHECK_UINT(rec.gen, 7);
		CHECK_UINT(rec.flags, flags[i]);
		CHECK_UINT(rec.samples, 80);
		CHECK_UINT(rec.tail_ms, 48);
		CHECK_UINT(rec.noise_ddbov, i ? 653u : LECR_NOISE_UNMEASURED);
		CHECK_UINT(rec.probes, i);
		CHECK_UINT(rec.diverge_resets, 0);
		CHECK_UINT(rec.line, 1);
		CHECK_UINT(rec.kind, kinds[i]);
		CHECK_UINT(rec.nlp_verdict, verdicts[i]);
		CHECK_UINT(rec.hold_left, i == 4u ? 3 : 0);
		CHECK_UINT(rec.skipped, i == 4u ? 5 : 0);
		CHECK(capture_fixture_planes_ok(&rec, i));
	}
	CHECK_UINT(i, 6);
	CHECK(!lecr_read_record(f, &file, &rec));
	fclose(f);
}

/*
 * The verdict of udsp.h struct udsp_lec_capture is a stable enum, and the
 * engine copies the private value of dsp/lec.c into it unchanged. Each public
 * value must therefore name the verdict of its word. A reordered public or
 * private enum fails here.
 */
static void capture_verdict_word_test(void)
{
	static const struct {
		unsigned verdict;
		const char *word;
	} map[] = {
		{ UDSP_LEC_CAPTURE_VERDICT_OFF, "off" },
		{ UDSP_LEC_CAPTURE_VERDICT_QUIET, "quiet" },
		{ UDSP_LEC_CAPTURE_VERDICT_DT, "dt" },
		{ UDSP_LEC_CAPTURE_VERDICT_UNLIKE, "unlike" },
		{ UDSP_LEC_CAPTURE_VERDICT_ERLE, "erle" },
		{ UDSP_LEC_CAPTURE_VERDICT_PASS, "pass" },
	};
	unsigned i;

	CASE("lec capture verdict values give the words of the canceller");
	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		CHECK(!strcmp(udsp_ec_nlms_verdict_name(map[i].verdict),
			      map[i].word));
}

/* Three records of the synthetic call, by their index. They are a FEED of the
 * open-loop stretch, the NEW of the second instance and the last RUN before
 * its END. */
#define REPLAY_FEED_PROBE	(REPLAY_FEED_FROM + 5u)
#define REPLAY_SECOND_NEW	(REPLAY_FIRST_FRAMES + 1u)
#define REPLAY_SECOND_LAST	(REPLAY_RECORDS - 2u)

/*
 * One edit of the synthetic call for each rule of one record in the header
 * comment of lec_replay.h. samples 0 keeps the samples of the record, and
 * line is added to its line.
 */
static const struct replay_refusal {
	unsigned index;
	uint16_t set;
	uint16_t clear;
	uint16_t skipped;
	uint8_t line;
	uint16_t samples;
} replay_refusals[] = {
	{ REPLAY_FEED_PROBE, 0, LECR_FLAG_SKIP_OPEN, 0, 0, 0 },
	{ REPLAY_FEED_PROBE, LECR_FLAG_SKIP_MODEM, 0, 0, 0, 0 },
	{ REPLAY_FEED_FROM - 1u, LECR_FLAG_SKIP_OFF, 0, 0, 0, 0 },
	{ REPLAY_FIRST_FRAMES, LECR_FLAG_SKIP_INACTIVE, 0, 0, 0, 0 },
	{ REPLAY_FEED_PROBE, 0, 0, 7, 0, 0 },
	{ REPLAY_FEED_PROBE, LECR_FLAG_SKIPPED, 0, 0, 0, 0 },
	{ 0, LECR_FLAG_SKIPPED, 0, 2, 0, 0 },
	{ REPLAY_FIRST_FRAMES, LECR_FLAG_SKIPPED, 0, 2, 0, 0 },
	{ REPLAY_FEED_PROBE, 0, 0, 0, 1, 0 },
	{ REPLAY_FIRST_FRAMES, 0, 0, 0, 0, 100 },
};

static void replay_refusal_apply(struct lecr_record *recs,
				 const struct replay_refusal *e)
{
	struct lecr_record *rec = &recs[e->index];

	rec->flags = (uint16_t)((rec->flags | e->set) & ~e->clear);
	if (e->skipped)
		rec->skipped = e->skipped;
	rec->line = (uint8_t)(rec->line + e->line);
	if (e->samples)
		rec->samples = e->samples;
}

/*
 * The rules of a lineage and of one record (bench/lec_replay.h) on the
 * synthetic call at 8 kHz. A lost final RUN, a lost NEW, a lost END and a
 * FEED whose frames moved each read as a gap and fail. The same record lost
 * in the ring reads as a loss and passes. A record that breaks a rule of one
 * record stops the file there, which reads INCOMPLETE, and names the rule.
 */
static void replay_rule_test(void)
{
	struct replay_seen seen;
	struct replay_call call;
	struct lecr_record *recs = NULL;
	struct lecr_agree a;
	unsigned i;

	if (replay_call_build(&call, UDSP_SAMPLE_RATE))
		recs = malloc(call.count * sizeof(*recs));

	CASE("lec replay reads an END after a lost final RUN as a gap");
	if (!recs) {
		CHECK(false);
		free(call.recs);
		return;
	}
	CHECK(replay_drop_run(&call, REPLAY_SECOND_LAST, 0, &a, &seen));
	CHECK_UINT(a.gaps, 1);
	CHECK_UINT(a.losses, 0);
	CHECK_UINT(lecr_agree_result(&a), LECR_FAIL);
	CHECK(replay_drop_run(&call, REPLAY_SECOND_LAST, 1, &a, &seen));
	CHECK_UINT(a.gaps, 0);
	CHECK_UINT(a.losses, 1);
	CHECK_UINT(lecr_agree_result(&a), LECR_PASS);

	CASE("lec replay reads records after an END without NEW as a gap");
	CHECK(replay_drop_run(&call, REPLAY_SECOND_NEW, 0, &a, &seen));
	CHECK_UINT(a.gaps, 1);
	CHECK_UINT(a.instances, 1);
	CHECK_UINT(lecr_agree_result(&a), LECR_FAIL);
	CHECK(replay_drop_run(&call, REPLAY_SECOND_NEW, 1, &a, &seen));
	CHECK_UINT(a.gaps, 0);
	CHECK_UINT(a.losses, 1);
	CHECK_UINT(lecr_agree_result(&a), LECR_PASS);

	CASE("lec replay reads a NEW without an END before it as a gap");
	CHECK(replay_drop_run(&call, REPLAY_FIRST_FRAMES, 0, &a, &seen));
	CHECK_UINT(a.gaps, 1);
	CHECK_UINT(a.losses, 0);
	CHECK_UINT(a.instances, 2);
	CHECK_UINT(seen.segments, REPLAY_SEGMENTS);
	CHECK(seen.seg[1].lossy);
	CHECK_UINT(lecr_agree_result(&a), LECR_FAIL);

	CASE("lec replay reads a FEED whose frames moved as a gap");
	memcpy(recs, call.recs, call.count * sizeof(*recs));
	recs[REPLAY_FEED_PROBE].frames = 777u;
	CHECK(replay_recs_run(&call, recs, 0, &a, &seen));
	CHECK_UINT(a.gaps, 1);
	CHECK_UINT(lecr_agree_result(&a), LECR_FAIL);
	memcpy(recs, call.recs, call.count * sizeof(*recs));
	recs[REPLAY_SECOND_NEW].frames = 1u;
	CHECK(replay_recs_run(&call, recs, 0, &a, &seen));
	CHECK_UINT(a.gaps, 1);
	CHECK_UINT(lecr_agree_result(&a), LECR_FAIL);

	CASE("lec replay refuses a record that breaks a rule of one record");
	for (i = 0; i < sizeof(replay_refusals) / sizeof(replay_refusals[0]);
	     i++) {
		const struct replay_refusal *e = &replay_refusals[i];

		memcpy(recs, call.recs, call.count * sizeof(*recs));
		replay_refusal_apply(recs, e);
		CHECK(replay_recs_run(&call, recs, 0, &a, &seen));
		printf("# edit %u of record %u: %s\n", i, e->index,
		       a.refusal ? a.refusal : "not refused");
		CHECK(a.parse_error);
		CHECK(a.refusal != NULL);
		CHECK_UINT(a.records, e->index);
		CHECK_UINT(lecr_agree_result(&a), LECR_INCOMPLETE);
	}

	CASE("lec replay reads version 1 and 2 files, and a synthetic FEED in "
	     "version 2 alone");
	for (i = 0; i < 4u; i++) {
		bool synthetic = i & 1u;
		uint16_t version = i < 2u ? 2u : 1u;
		char *buf = NULL;
		size_t size = 0;

		memcpy(recs, call.recs, call.count * sizeof(*recs));
		if (synthetic)
			recs[REPLAY_FEED_PROBE].flags = (uint16_t)
				((recs[REPLAY_FEED_PROBE].flags &
				  ~LECR_FLAGS_REASON) |
				 LECR_FLAG_SKIP_SYNTHETIC);
		CHECK(replay_file_write(recs, call.count, call.rate, &buf,
					&size));
		if (buf && size > 9u) {
			buf[8] = (char)version;
			replay_buffer_run(buf, size, false, &a, &seen);
			printf("# version %u, synthetic FEED %u: %s\n",
			       version, synthetic,
			       a.refusal ? a.refusal : "read");
			CHECK_UINT(a.parse_error, version == 1u && synthetic);
			CHECK_UINT(lecr_agree_result(&a),
				   version == 1u && synthetic ?
				   LECR_INCOMPLETE : LECR_PASS);
		}
		free(buf);
	}
	free(recs);
	free(call.recs);
}

/* The first 1500 frames, 15 s, read the other verdict and the other FIRED in
 * the replay. The 500 frames after them agree. */
static void replay_early_edit(unsigned f, struct lecr_record *rec,
			      struct lecr_record *got, const void *arg)
{
	(void)arg;
	rec->nlp_verdict = LECR_VERDICT_QUIET;
	got->nlp_verdict = LECR_VERDICT_QUIET;
	if (f >= 15u * LECR_TICKS_PER_SECOND)
		return;
	got->nlp_verdict = LECR_VERDICT_ERLE;
	rec->flags |= f % 2u ? LECR_FLAG_FIRED : 0u;
	got->flags |= f % 2u ? 0u : LECR_FLAG_FIRED;
}

/*
 * The default exclusion of lec_replay leaves 15 s of each segment out of the
 * steady-state figures. The control and continuity checks count from the
 * first record. A segment whose first 15 s disagree passes on the 500 frames
 * after them, and fails without the exclusion. The synthetic call is 12 s
 * long, so no segment has a body under the default. A frames mismatch and a
 * gap in that time still read FAIL.
 */
static void replay_skip_test(void)
{
	struct replay_seen seen;
	struct replay_call call;
	struct lecr_record *recs = NULL;
	struct lecr_agree a;

	CASE("lec replay leaves the first 15 s of a segment out of the "
	     "steady-state figures by default");
	CHECK_UINT(LECR_SKIP_S_DEFAULT, 15);
	replay_seg_drive(&a, LECR_SKIP_S_DEFAULT, 2000u, replay_early_edit,
			 NULL);
	CHECK_UINT(a.done.runs, 2000);
	CHECK_UINT(a.done.body, 500);
	CHECK_UINT(a.done.verdict_same, 500);
	CHECK_UINT(a.done.fired_diff, 0);
	CHECK_UINT(lecr_segment_faults(&a.done), 0);
	CHECK_UINT(lecr_agree_result(&a), LECR_PASS);
	replay_seg_drive(&a, 0, 2000u, replay_early_edit, NULL);
	CHECK_UINT(a.done.body, 2000);
	CHECK_UINT(lecr_agree_result(&a), LECR_FAIL);

	CASE("lec replay counts the frames and the gaps from the first record "
	     "under the default exclusion");
	if (replay_call_build(&call, UDSP_SAMPLE_RATE))
		recs = malloc(call.count * sizeof(*recs));
	if (!recs) {
		CHECK(false);
		free(call.recs);
		return;
	}
	memcpy(recs, call.recs, call.count * sizeof(*recs));
	CHECK(replay_recs_run(&call, recs, LECR_SKIP_S_DEFAULT, &a, &seen));
	CHECK_UINT(a.evaluated, 0);
	CHECK_UINT(a.frames_bad + a.gaps, 0);
	CHECK_UINT(lecr_agree_result(&a), LECR_INCOMPLETE);
	recs[REPLAY_RESET_AT / 2u].frames++;
	CHECK(replay_recs_run(&call, recs, LECR_SKIP_S_DEFAULT, &a, &seen));
	CHECK_UINT(a.frames_bad, 1);
	CHECK_UINT(lecr_agree_result(&a), LECR_FAIL);
	recs[REPLAY_RESET_AT / 2u].frames--;
	memmove(recs + REPLAY_LOSS_AT, recs + REPLAY_LOSS_AT + 1u,
		(call.count - REPLAY_LOSS_AT - 1u) * sizeof(*recs));
	call.count--;
	CHECK(replay_recs_run(&call, recs, LECR_SKIP_S_DEFAULT, &a, &seen));
	CHECK_UINT(a.gaps, 1);
	CHECK_UINT(lecr_agree_result(&a), LECR_FAIL);
	free(recs);
	free(call.recs);
}

/* The live and the replay figures of one segment of 1000 frames. The first
 * quiet frames read quiet and the rest pass with ACTIVE. The replay reads
 * the first moved of them as a pass with ACTIVE. The last fired frames carry
 * FIRED, and the replay clears it on the first flipped of them. */
struct replay_bound {
	unsigned quiet;
	unsigned moved;
	unsigned fired;
	unsigned flipped;
};

#define REPLAY_BOUND_FRAMES	1000u

static void replay_bound_edit(unsigned f, struct lecr_record *rec,
			      struct lecr_record *got, const void *arg)
{
	const struct replay_bound *b = arg;
	unsigned fire_from = REPLAY_BOUND_FRAMES - b->fired;

	rec->nlp_verdict = f < b->quiet ? LECR_VERDICT_QUIET :
					  LECR_VERDICT_PASS;
	rec->flags |= f < b->quiet ? 0u : LECR_FLAG_ACTIVE;
	rec->flags |= f >= fire_from ? LECR_FLAG_FIRED : 0u;
	got->nlp_verdict = rec->nlp_verdict;
	got->flags = rec->flags;
	if (f < b->moved) {
		got->nlp_verdict = LECR_VERDICT_PASS;
		got->flags |= LECR_FLAG_ACTIVE;
	}
	if (f >= fire_from && f < fire_from + b->flipped)
		got->flags &= (uint16_t)~LECR_FLAG_FIRED;
}

static unsigned replay_bound_faults(unsigned quiet, unsigned moved,
				    unsigned fired, unsigned flipped)
{
	struct replay_bound b = { quiet, moved, fired, flipped };
	struct lecr_agree a;

	replay_seg_drive(&a, 0, REPLAY_BOUND_FRAMES, replay_bound_edit, &b);
	CHECK_UINT(a.done.body, REPLAY_BOUND_FRAMES);
	CHECK_UINT(a.failed, lecr_segment_faults(&a.done) ? 1u : 0u);
	return lecr_segment_faults(&a.done);
}

/*
 * The bound of a count and of the FIRED symmetric difference is max(5,
 * ceil(0.05 x live)). 210 live frames give 11 and 40 give 5. A difference on
 * the bound passes, and one over it fails. A bound rounded down, a bound
 * without its floor of 5 or a test with >= fails here.
 */
static void replay_bound_test(void)
{
	CASE("lec replay passes a count on its bound and fails it one over");
	CHECK_UINT(lecr_bound_get(210u), 11);
	CHECK_UINT(lecr_bound_get(40u), 5);
	CHECK_UINT(replay_bound_faults(210u, 11u, 0, 0), 0);
	CHECK_UINT(replay_bound_faults(210u, 12u, 0, 0), LECR_FAULT_COUNTS);
	CHECK_UINT(replay_bound_faults(40u, 5u, 0, 0), 0);
	CHECK_UINT(replay_bound_faults(40u, 6u, 0, 0), LECR_FAULT_COUNTS);

	CASE("lec replay passes the fired difference on its bound and fails "
	     "it one over");
	CHECK_UINT(replay_bound_faults(0, 0, 210u, 11u), 0);
	CHECK_UINT(replay_bound_faults(0, 0, 210u, 12u), LECR_FAULT_FIRED);
	CHECK_UINT(replay_bound_faults(0, 0, 40u, 5u), 0);
	CHECK_UINT(replay_bound_faults(0, 0, 40u, 6u), LECR_FAULT_FIRED);
}

#define PAUSE_ERL_DB		24.0
#define PAUSE_FLOOR_DBM0	-60.0
#define PAUSE_FAR_DBM0		-10.0
#define PAUSE_NEAR_DBM0		-25.0
#define PAUSE_FAR_FRAMES	800u
#define PAUSE_WORDS_FRAMES	300u
#define PAUSE_WORDS_DELAY	50u
#define PAUSE_SPEECH_FRAMES	500u
#define PAUSE_RISE_MAX_DB	1.0

/*
 * The line of the pause cases: 8 kHz, a flat 24 dB hybrid at 8 ms, and a
 * floor at -60 dBm0 on every frame. Far-end words at -10 dBm0 run for 8 s,
 * and their pauses measure the floor.
 */
struct pause_line {
	struct echo_path path;
	struct udsp_ec *ec;
	struct word_source far;
	struct word_source near;
	uint32_t floor_state;
	uint32_t near_state;
};

/* One frame of far-end words, or of a far-end pause, with near on top when
 * it is given. It returns the tracked floor. */
static float pause_frame_run(struct pause_line *line, bool far,
			     const int16_t *near)
{
	struct udsp_ec_nlms_info info;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t echo[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	unsigned i;

	if (far)
		word_fill(&line->far, ref, UDSP_FRAME_SAMPLES);
	else
		memset(ref, 0, sizeof(ref));
	path_run(&line->path, ref, echo);
	floor_add(mic, echo, PAUSE_FLOOR_DBM0, &line->floor_state);
	for (i = 0; near && i < UDSP_FRAME_SAMPLES; i++)
		mic[i] = sample_clamp((double)mic[i] + near[i]);
	udsp_ec_nlms.run(line->ec, mic, ref);
	udsp_ec_nlms_info_get(line->ec, &info);
	return info.noise_dbov;
}

/* The line after its 8 s of far-end words. It returns the tracked floor. */
static float pause_line_create(struct pause_line *line)
{
	float floor = 127.0f;
	unsigned f;

	memset(line, 0, sizeof(*line));
	line->floor_state = 0x0f100f10u;
	line->near_state = 0x600dcafeu;
	line->ec = udsp_ec_nlms.create(48);
	path_flat_init(&line->path, UDSP_SAMPLE_RATE, 8, PAUSE_ERL_DB);
	word_source_init(&line->far, UDSP_SAMPLE_RATE, PAUSE_FAR_DBM0,
			 0x13572468u);
	word_source_init(&line->near, UDSP_SAMPLE_RATE, PAUSE_NEAR_DBM0,
			 0xfeedfaceu);
	for (f = 0; f < PAUSE_FAR_FRAMES; f++)
		floor = pause_frame_run(line, true, NULL);
	return floor;
}

/*
 * Near-end words in a far-end pause. After 8 s of far-end words, a 3 s pause
 * carries 4 near-end words at -25 dBm0 from its first 0.5 s on. The floor
 * must not rise by more than 1 dB on any frame of the pause. A tracker
 * without the rise test rises by 17 dB on the first word.
 */
static void floor_pause_words_test(void)
{
	int16_t near[UDSP_FRAME_SAMPLES];
	struct pause_line line;
	float start = pause_line_create(&line);
	float loudest = start;
	unsigned words = 0;
	unsigned f;

	for (f = 0; f < PAUSE_WORDS_FRAMES; f++) {
		float floor;

		memset(near, 0, sizeof(near));
		if (f >= PAUSE_WORDS_DELAY) {
			word_fill(&line.near, near, UDSP_FRAME_SAMPLES);
			words += line.near.offset;
		}
		floor = pause_frame_run(&line, false, near);
		if (floor < loudest)
			loudest = floor;
	}

	CASE("lec keeps near-end words in a far-end pause out of the floor");
	printf("# near-end floor %.2f dBov, tracked %.2f dBov before the "
	       "pause, loudest %.2f dBov in it, %u near words\n",
	       floor_dbov_get(PAUSE_FLOOR_DBM0), (double)start,
	       (double)loudest, words);
	CHECK(fabs(start - floor_dbov_get(PAUSE_FLOOR_DBM0)) <= 1.5);
	CHECK_UINT(words, 4);
	CHECK(loudest >= start - PAUSE_RISE_MAX_DB);
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * Continuous speech in a far-end pause. The pause carries 5 s of syllabic
 * near-end speech at a mean of -25 dBm0, with the 14 dB swing of the
 * syllabic envelope. The floor must not rise by more than 1 dB. A tracker
 * without the rise test rises by 30 dB within about 1 s.
 */
static void floor_pause_speech_test(void)
{
	int16_t near[UDSP_FRAME_SAMPLES];
	struct pause_line line;
	float start = pause_line_create(&line);
	float loudest = start;
	unsigned f;

	for (f = 0; f < PAUSE_SPEECH_FRAMES; f++) {
		float floor;

		noise_fill(near, UDSP_FRAME_SAMPLES,
			   syllabic_dbm0_get(f, PAUSE_NEAR_DBM0),
			   &line.near_state);
		floor = pause_frame_run(&line, false, near);
		if (floor < loudest)
			loudest = floor;
	}

	CASE("lec keeps continuous speech in a far-end pause out of the floor");
	printf("# tracked floor %.2f dBov before the pause, loudest %.2f dBov "
	       "in it\n", (double)start, (double)loudest);
	CHECK(fabs(start - floor_dbov_get(PAUSE_FLOOR_DBM0)) <= 1.5);
	CHECK(loudest >= start - PAUSE_RISE_MAX_DB);
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * A near-end floor that rises by 15 dB and stays, then falls back, with the
 * far end silent. After 60 frames the floor must be within 1.5 dB of
 * -45 dBm0. It must be within 1 dB of -60 dBm0 20 frames after the fall. A
 * tracker that never follows a rise, or that follows a fall slowly, fails
 * here.
 */
static void floor_rise_test(void)
{
	struct udsp_ec_nlms_info info;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);

	silence_run(ec, -60.0, 50u);
	silence_run(ec, -45.0, 60u);
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec follows a near-end floor that rises and stays");
	printf("# near-end floor %.2f dBov, tracked %.2f dBov after 60 "
	       "frames\n", floor_dbov_get(-45.0), (double)info.noise_dbov);
	CHECK(fabs(info.noise_dbov - floor_dbov_get(-45.0)) <= 1.5);

	silence_run(ec, -60.0, 20u);
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec follows a near-end floor that falls");
	printf("# near-end floor %.2f dBov, tracked %.2f dBov after 20 "
	       "frames\n", floor_dbov_get(-60.0), (double)info.noise_dbov);
	CHECK(fabs(info.noise_dbov - floor_dbov_get(-60.0)) <= 1.0);
	udsp_ec_nlms.destroy(ec);
}

/*
 * The first measurable floor after digital silence. A fresh instance sees
 * 50 frames of digital silence and then 10 frames of a floor at -50 dBm0.
 * The floor must read 127, no floor, after the silence, and the level of the
 * floor within 1 dB after the 10 frames. A silence that seeds the floor at
 * 127 leaves 127 there, and the case fails.
 */
static void floor_silence_test(void)
{
	struct udsp_ec_nlms_info info;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	float silent;

	silence_run(ec, -200.0, 50u);
	udsp_ec_nlms_info_get(ec, &info);
	silent = info.noise_dbov;
	silence_run(ec, -50.0, 10u);
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec takes the first measurable floor whole after digital "
	     "silence");
	printf("# tracked floor %.2f dBov after the silence, %.2f dBov after "
	       "10 frames of a floor at %.2f dBov\n", (double)silent,
	       (double)info.noise_dbov, floor_dbov_get(-50.0));
	CHECK(silent == 127.0f);
	CHECK(fabs(info.noise_dbov - floor_dbov_get(-50.0)) <= 1.0);
	udsp_ec_nlms.destroy(ec);
}

#define SHORT_PAUSE_WORD	25u
#define SHORT_PAUSE_CYCLE	55u
#define SHORT_PAUSE_LOW_DBM0	-60.0
#define SHORT_PAUSE_HIGH_DBM0	-45.0
#define SHORT_PAUSE_LOW_FRAMES	300u
#define SHORT_PAUSE_HIGH_FRAMES	500u

/*
 * A near-end floor rises from -60 to -45 dBm0. The far end talks in syllabic
 * bursts of 25 frames with pauses of 30 frames. A pause gives 26
 * quiet-window readings, fewer than a full rise run. The floor must be
 * within 1.5 dB of -45 dBm0 5 s after the rise. A rise run that the far-end
 * frames clear never fills, and fails here.
 */
static void floor_short_pause_test(void)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x2468ace1u;
	uint32_t floor_state = 0x0f100f10u;
	double high = floor_dbov_get(SHORT_PAUSE_HIGH_DBM0);
	float low = 127.0f;
	unsigned followed = 0;
	unsigned f;

	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, PAUSE_ERL_DB);
	for (f = 0; f < SHORT_PAUSE_LOW_FRAMES + SHORT_PAUSE_HIGH_FRAMES; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t echo[UDSP_FRAME_SAMPLES];
		int16_t mic[UDSP_FRAME_SAMPLES];
		bool rose = f >= SHORT_PAUSE_LOW_FRAMES;

		memset(ref, 0, sizeof(ref));
		if (f % SHORT_PAUSE_CYCLE < SHORT_PAUSE_WORD)
			noise_fill(ref, UDSP_FRAME_SAMPLES,
				   syllabic_dbm0_get(f, PAUSE_FAR_DBM0),
				   &state);
		path_run(&path, ref, echo);
		floor_add(mic, echo, rose ? SHORT_PAUSE_HIGH_DBM0 :
			  SHORT_PAUSE_LOW_DBM0, &floor_state);
		udsp_ec_nlms.run(ec, mic, ref);
		udsp_ec_nlms_info_get(ec, &info);
		if (!rose)
			low = info.noise_dbov;
		else if (!followed && fabs(info.noise_dbov - high) <= 1.5)
			followed = f + 1u - SHORT_PAUSE_LOW_FRAMES;
	}

	CASE("lec follows a floor that rises through short far-end pauses");
	printf("# tracked floor %.2f dBov before the rise, %.2f dBov 5 s "
	       "after it against %.2f dBov, within 1.5 dB after %u frames\n",
	       (double)low, (double)info.noise_dbov, high, followed);
	CHECK(fabs(low - floor_dbov_get(SHORT_PAUSE_LOW_DBM0)) <= 1.5);
	CHECK(fabs(info.noise_dbov - high) <= 1.5);
	udsp_ec_nlms.destroy(ec);
}

/*
 * One frame of a frozen instance whose microphone reads dbov exactly: a
 * square wave at the amplitude of that level. With far set, far-end noise
 * makes the frame active. The filter holds no energy, so the residual is the
 * microphone. It returns the tracked floor, and sets *quiet when the whole
 * window of the frame was quiet.
 */
static float exact_frame_run(struct udsp_ec *ec, int dbov, bool far,
			     uint32_t *state, bool *quiet)
{
	int16_t amplitude = (int16_t)lrint(32767.0 * pow(10.0, -dbov / 20.0));
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	unsigned i;

	for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
		mic[i] = i % 2u ? (int16_t)-amplitude : amplitude;
	CHECK(udsp_cn_level_get(mic, UDSP_FRAME_SAMPLES) == dbov);
	memset(ref, 0, sizeof(ref));
	if (far)
		noise_fill(ref, UDSP_FRAME_SAMPLES, PAUSE_FAR_DBM0, state);
	udsp_ec_nlms.run(ec, mic, ref);
	udsp_ec_nlms_info_get(ec, &info);
	udsp_ec_nlms_frame_get(ec, &reading);
	*quiet = reading.window_run == 0u;
	return info.noise_dbov;
}

/* n active frames at dbov. */
static void exact_far_run(struct udsp_ec *ec, int dbov, unsigned n,
			  uint32_t *state)
{
	bool quiet;
	unsigned i;

	for (i = 0; i < n; i++)
		(void)exact_frame_run(ec, dbov, true, state, &quiet);
}

/*
 * Far-end pause frames at dbov until n of them are quiet-window frames, the
 * readings of the tracker. *tail counts the frames before them whose window
 * was still active. It returns the tracked floor.
 */
static float exact_quiet_run(struct udsp_ec *ec, int dbov, unsigned n,
			     uint32_t *state, unsigned *tail)
{
	float floor = 127.0f;
	unsigned read = 0;

	*tail = 0;
	while (read < n) {
		bool quiet;

		floor = exact_frame_run(ec, dbov, false, state, &quiet);
		if (quiet)
			read++;
		else
			(*tail)++;
	}
	return floor;
}

/* exact_quiet_run on a window that is already quiet. */
static float exact_read_run(struct udsp_ec *ec, int dbov, unsigned n,
			    uint32_t *state)
{
	unsigned tail;
	float floor = exact_quiet_run(ec, dbov, n, state, &tail);

	CHECK_UINT(tail, 0);
	return floor;
}

/* A frozen 48 ms instance whose floor reads dbov, taken whole from its first
 * frame. */
static struct udsp_ec *exact_line_create(int dbov, uint32_t *state)
{
	struct udsp_ec *ec = udsp_ec_nlms.create(48);

	udsp_ec_nlms.freeze(ec, true);
	(void)exact_read_run(ec, dbov, 1u, state);
	return ec;
}

/*
 * The edge of the rise test at LEC_NOISE_RISE_DB, 3 dB. A floor at 60.90 dBov
 * reads a frame at 58 dBov, 2.90 dB over it, and moves by 2 percent of the
 * gap. A floor at 64.10 dBov reads a frame at 61 dBov, 3.10 dB over it, and
 * holds. That reading is the first of a run of 50 at 61 dBov, which sets the
 * floor to 61. A bound of 2 or 4 dB fails one of the two.
 */
static void floor_bound_test(void)
{
	uint32_t state = 0x31415927u;
	struct udsp_ec *ec;
	float before;
	float after;

	CASE("lec moves the floor slowly on a reading 2.9 dB over it");
	ec = exact_line_create(60, &state);
	before = exact_read_run(ec, 61, 8u, &state);
	after = exact_read_run(ec, 58, 1u, &state);
	printf("# floor %.4f dBov, reading 2.9 dB over it, floor %.4f dBov "
	       "after\n", (double)before, (double)after);
	CHECK(before - 58.0f > 2.85f && before - 58.0f < 2.95f);
	CHECK(fabsf(after - (before - (before - 58.0f) * 0.02f)) < 0.001f);
	udsp_ec_nlms.destroy(ec);

	CASE("lec holds the floor on a reading 3.1 dB over it");
	ec = exact_line_create(60, &state);
	before = exact_read_run(ec, 66, 4u, &state);
	after = exact_read_run(ec, 61, 1u, &state);
	printf("# floor %.4f dBov, reading 3.1 dB over it, floor %.4f dBov "
	       "after\n", (double)before, (double)after);
	CHECK(before - 61.0f > 3.05f && before - 61.0f < 3.15f);
	CHECK(after == before);
	CHECK(exact_read_run(ec, 61, 48u, &state) == before);
	CHECK(exact_read_run(ec, 61, 1u, &state) == 61.0f);
	udsp_ec_nlms.destroy(ec);
}

/* A floor at 60 dBov reads 49 frames at 50 dBov, one frame at dbov, 49 more
 * at 50 dBov, and then one more. */
static void floor_break_check(int dbov)
{
	uint32_t state = 0x27182819u;
	struct udsp_ec *ec = exact_line_create(60, &state);
	float broken;
	float held;

	CHECK(exact_read_run(ec, 50, 49u, &state) == 60.0f);
	broken = exact_read_run(ec, dbov, 1u, &state);
	held = exact_read_run(ec, 50, 49u, &state);
	printf("# floor %.2f dBov after the reading at %d dBov, %.2f dBov 49 "
	       "readings later\n", (double)broken, dbov, (double)held);
	CHECK(held == broken);
	CHECK(exact_read_run(ec, 50, 1u, &state) == 50.0f);
	udsp_ec_nlms.destroy(ec);
}

/*
 * An in-band or a quieter reading ends a rise run. A floor at 60 dBov reads
 * 49 frames at 50 dBov, one at 60 or 62 dBov and 49 more at 50 dBov. It
 * holds. The next frame at 50 dBov completes a run of 50 and sets the floor
 * to 50. A run that the reading keeps, or a shorter run, fails here.
 */
static void floor_run_break_test(void)
{
	CASE("lec starts the rise run again after an in-band reading");
	floor_break_check(60);

	CASE("lec starts the rise run again after a quieter reading");
	floor_break_check(62);
}

#define QUIET3_READINGS		3u
#define QUIET3_ACTIVE		10u
#define QUIET3_CYCLES		17u

/*
 * Quiet windows of 3 frames between far-end bursts of 10 frames, with a floor
 * that rose from 60 to 50 dBov. Each window adds 3 readings to the rise run,
 * and the run holds across the bursts. After 16 windows, 48 readings, the
 * floor holds. The 17th window completes the run and sets the floor to 50. A
 * run that the bursts clear fails here.
 */
static void floor_quiet3_test(void)
{
	uint32_t state = 0x16180340u;
	struct udsp_ec *ec;
	float before_last = 127.0f;
	float floor = 127.0f;
	unsigned tails = 0;
	unsigned c;

	CASE("lec adds up the rise readings of quiet windows of 3 frames");
	ec = exact_line_create(60, &state);
	for (c = 0; c < QUIET3_CYCLES; c++) {
		unsigned tail;

		exact_far_run(ec, 50, QUIET3_ACTIVE, &state);
		before_last = floor;
		floor = exact_quiet_run(ec, 50, QUIET3_READINGS, &state,
					&tail);
		tails += tail;
	}
	printf("# floor %.2f dBov after 16 windows, %.2f dBov after 17, %u "
	       "tail frames\n", (double)before_last, (double)floor, tails);
	CHECK_UINT(tails, 4u * QUIET3_CYCLES);
	CHECK(before_last == 60.0f);
	CHECK(floor == 50.0f);
	udsp_ec_nlms.destroy(ec);
}

#define AGE_TAIL_FRAMES		4u

/* The floor after the 50th reading, distance frames after the first. The
 * pause before the second 25 readings starts with AGE_TAIL_FRAMES frames of
 * an active window. */
static float floor_age_run(unsigned distance)
{
	uint32_t state = 0x57721567u;
	struct udsp_ec *ec = exact_line_create(60, &state);
	unsigned tail;
	float floor;

	(void)exact_read_run(ec, 50, 25u, &state);
	exact_far_run(ec, 50, distance - 49u - AGE_TAIL_FRAMES, &state);
	floor = exact_quiet_run(ec, 50, 25u, &state, &tail);
	CHECK_UINT(tail, AGE_TAIL_FRAMES);
	udsp_ec_nlms.destroy(ec);
	return floor;
}

/*
 * The age limit of a rise run, LEC_NOISE_RISE_AGE, 500 frames. A floor at
 * 60 dBov reads 25 frames at 50 dBov, then far-end frames and a pause, then
 * 25 more readings. The run takes the 50th reading 500 frames after the
 * first, and sets the floor to 50. At 501 frames the run starts again, and
 * the floor holds. A run without the limit, or with a shorter one, fails.
 */
static void floor_age_test(void)
{
	float floor;

	CASE("lec keeps a rise run whose first reading is 500 frames old");
	floor = floor_age_run(500u);
	printf("# floor %.2f dBov at 500 frames\n", (double)floor);
	CHECK(floor == 50.0f);

	CASE("lec starts a rise run again when its first reading is 501 "
	     "frames old");
	floor = floor_age_run(501u);
	printf("# floor %.2f dBov at 501 frames\n", (double)floor);
	CHECK(floor == 60.0f);
}

/*
 * The reset and the rise run. A floor at 60 dBov reads 30 frames at 50 dBov,
 * and the reset follows. The reset keeps the floor. Another 30 readings
 * leave it at 60, and 20 after them complete a run of 50 and set it to 50. A
 * reset that keeps the run sets the floor 20 readings after the reset.
 */
static void floor_reset_test(void)
{
	uint32_t state = 0x14142136u;
	struct udsp_ec_nlms_info info;
	struct udsp_ec *ec;
	float held;

	CASE("lec reset starts the rise run again");
	ec = exact_line_create(60, &state);
	(void)exact_read_run(ec, 50, 30u, &state);
	udsp_ec_nlms_reset(ec);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK(info.noise_dbov == 60.0f);
	held = exact_read_run(ec, 50, 30u, &state);
	printf("# floor %.2f dBov 30 readings after the reset\n",
	       (double)held);
	CHECK(held == 60.0f);
	CHECK(exact_read_run(ec, 50, 20u, &state) == 50.0f);
	udsp_ec_nlms.destroy(ec);
}

#define FAN_DBM0		-38.0
#define FAN_WORDS_DBM0		-20.0
#define FAN_FLOOR_DBM0		-72.0
#define FAN_ERL_DB		24.0
#define FAN_FRAMES		(30u * FRAMES_PER_SECOND)
#define FAN_SEED_BOUND		(10u * FRAMES_PER_SECOND)
#define FAN_FLOOR_TOL_DB	1.5
#define FAN_DT_SHARE_MAX	0.10
#define FAN_LOWPASS_HZ		300.0

/*
 * The near speech of a line over every frame that holds it. A frame holds near
 * speech when the near plane alone crosses the activity bar (frame_loud). The
 * energies give the regression gain of the output on the near speech and the
 * output against the microphone. An exposure frame is a frame with near
 * speech whose echo estimate (estimate_power of the frame reading) stands at
 * or over the power of the near plane less 10 dB.
 */
struct near_stat {
	unsigned frames;
	unsigned exposure;
	double near;
	double product;
	double mic;
	double out;
};

static void near_stat_add(struct near_stat *st, const struct udsp_ec *ec,
			  const int16_t *near, const int16_t *mic,
			  const int16_t *out, unsigned frame)
{
	struct udsp_ec_nlms_frame reading;
	unsigned i;

	if (!frame_loud(near, frame))
		return;
	udsp_ec_nlms_frame_get(ec, &reading);
	st->frames++;
	if (reading.estimate_power >=
	    udsp_ec_nlms_frame_power_get(near, frame) / 10.0f)
		st->exposure++;
	for (i = 0; i < frame; i++) {
		st->near += (double)near[i] * near[i];
		st->product += (double)out[i] * near[i];
		st->mic += (double)mic[i] * mic[i];
		st->out += (double)out[i] * out[i];
	}
}

/*
 * The regression gain of an output on the near speech it carries, in dB. 0
 * dB means that the output does not scale the speech, and a line with no near
 * energy reads 0 dB. An output that carries none of the near speech, or its
 * opposite, reads the sentinel -200 dB, so a bound on the loss fails there.
 */
static double near_gain_db_get(double product, double near)
{
	if (!(near > 0.0))
		return 0.0;
	if (!(product > 0.0))
		return -200.0;
	return 20.0 * log10(product / near);
}

/*
 * The fan line of the seed cases: a flat 24 dB hybrid at 8 ms, the default
 * tail, the processor off and a near-end floor at -72 dBm0. A far-end fan at
 * -38 dBm0 never pauses. Its echo returns at -62 dBm0 by the bound of the
 * line at 8 kHz, and at -63 dBm0 at 16 kHz. That is over the -65 dBm0 of the
 * cap of dsp/lec.c, so the far-end floor keeps the fan frames active and the
 * seed reads them. The echo stands 10 dB over the floor. Far-end words at
 * -20 dBm0 go on top of the fan. fan_line_set gives the line another fan
 * level and another hybrid. The filter ERLE counts the frames without near
 * speech whose reference crosses the fixed bar, from measure on: the echo
 * over the residual less the floor. With measure_words set it counts the
 * frames whose far words cross the fixed bar instead. word_loud says that
 * the far words of the last frame crossed it, and out holds the output of
 * the last frame. speech takes the near speech of every frame.
 */
struct fan_line {
	struct echo_path path;
	struct word_source far;
	struct udsp_ec *ec;
	unsigned frame;
	uint32_t fan_state;
	uint32_t floor_state;
	double fan_dbm0;
	double erl_db;
	double floor_dbm0;
	double pole;
	double gain;
	double memory;
	bool measure;
	bool measure_words;
	bool word_loud;
	double echo_energy;
	double filter_energy;
	struct near_stat speech;
	int16_t out[UDSP_FRAME_MAX];
};

/* A first-order low-pass fan at lowpass_hz when it is over 0, with the gain
 * that keeps its level at fan_dbm0. */
static void fan_line_init(struct fan_line *line, unsigned rate,
			  double lowpass_hz)
{
	memset(line, 0, sizeof(*line));
	line->frame = rate / 100u;
	line->ec = udsp_ec_nlms.create_rate(0, rate);
	udsp_ec_nlms.nlp(line->ec, false);
	line->fan_dbm0 = FAN_DBM0;
	line->erl_db = FAN_ERL_DB;
	path_flat_init(&line->path, rate, 8, line->erl_db);
	word_source_init(&line->far, rate, FAN_WORDS_DBM0, 0x13572468u);
	line->fan_state = 0x0f00f00du;
	line->floor_state = 0x2468acefu;
	line->floor_dbm0 = FAN_FLOOR_DBM0;
	line->gain = 1.0;
	if (lowpass_hz > 0.0) {
		line->pole = exp(-2.0 * M_PI * lowpass_hz / rate);
		line->gain = sqrt((1.0 + line->pole) / (1.0 - line->pole));
	}
}

/* Another fan level and another hybrid, before the line runs. */
static void fan_line_set(struct fan_line *line, double fan_dbm0, double erl_db)
{
	line->fan_dbm0 = fan_dbm0;
	line->erl_db = erl_db;
	path_erl_set(&line->path, erl_db);
}

static void fan_fill(struct fan_line *line, int16_t *dst)
{
	unsigned i;

	noise_fill(dst, line->frame, line->fan_dbm0, &line->fan_state);
	if (line->pole == 0.0)
		return;
	for (i = 0; i < line->frame; i++) {
		line->memory = line->pole * line->memory +
			       (1.0 - line->pole) * dst[i];
		dst[i] = sample_clamp(line->memory * line->gain);
	}
}

/* One frame of the fan line, with near on top when it is given. It returns
 * the state after the frame. */
static struct udsp_ec_nlms_info fan_frame_run(struct fan_line *line, bool fan,
					      bool words, const int16_t *near)
{
	struct udsp_ec_nlms_info info;
	int16_t ref[UDSP_FRAME_MAX];
	int16_t part[UDSP_FRAME_MAX];
	int16_t echo[UDSP_FRAME_MAX];
	int16_t hiss[UDSP_FRAME_MAX];
	int16_t in[UDSP_FRAME_MAX];
	int16_t mic[UDSP_FRAME_MAX];
	unsigned i;

	memset(ref, 0, sizeof(ref));
	if (fan)
		fan_fill(line, ref);
	if (words) {
		word_fill(&line->far, part, line->frame);
		for (i = 0; i < line->frame; i++)
			ref[i] = sample_clamp((double)ref[i] + part[i]);
	}
	path_run(&line->path, ref, echo);
	noise_fill(hiss, line->frame, line->floor_dbm0, &line->floor_state);
	for (i = 0; i < line->frame; i++)
		mic[i] = sample_clamp((double)echo[i] + hiss[i] +
				      (near ? near[i] : 0));
	memcpy(in, mic, line->frame * sizeof(*mic));
	udsp_ec_nlms.run(line->ec, mic, ref);
	memcpy(line->out, mic, line->frame * sizeof(*mic));
	udsp_ec_nlms_info_get(line->ec, &info);
	if (near)
		near_stat_add(&line->speech, line->ec, near, in, mic,
			      line->frame);
	line->word_loud = words && frame_loud(part, line->frame);
	if (!line->measure || near)
		return info;
	if (!(line->measure_words ? line->word_loud :
				    frame_loud(ref, line->frame)))
		return info;
	for (i = 0; i < line->frame; i++) {
		double d = (double)mic[i] - hiss[i];

		line->echo_energy += (double)echo[i] * echo[i];
		line->filter_energy += d * d;
	}
	return info;
}

static double fan_filter_erle_get(const struct fan_line *line)
{
	if (!(line->filter_energy > 0.0))
		return 200.0;
	return 10.0 * log10(line->echo_energy / line->filter_energy);
}

/* True when the frame just run carried a window-active double talk
 * verdict or hangover. */
static bool fan_dt_get(const struct fan_line *line,
		       const struct udsp_ec_nlms_info *info)
{
	struct udsp_ec_nlms_frame reading;

	udsp_ec_nlms_frame_get(line->ec, &reading);
	return reading.window_run && info->double_talk;
}

static bool fan_window_get(const struct fan_line *line)
{
	struct udsp_ec_nlms_frame reading;

	udsp_ec_nlms_frame_get(line->ec, &reading);
	return reading.window_run != 0u;
}

/*
 * A far end that never pauses, at the line rate. The fan line runs for 30 s.
 * The seed must measure the floor within 10 s (FAN_SEED_BOUND), within 1.5 dB
 * of -72 dBm0, on a frame of an active window. After 10 s the window must stay
 * active, with no trial, no reset and double talk on under 10 percent of the
 * frames. The seed comes at 1.8 s at 8 kHz and 4.5 s at 16 kHz. Without the
 * seed, the line reads no floor and no double talk, because the fan echo
 * stands 10 dB over the floor.
 */
static void fan_seed_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct udsp_ec_nlms_info at;
	struct fan_line line;
	double floor = floor_dbov_get(FAN_FLOOR_DBM0);
	float seed = 127.0f;
	unsigned seeded = 0;
	unsigned window = 0;
	unsigned dt = 0;
	bool seed_window = false;
	unsigned f;

	fan_line_init(&line, rate, 0.0);
	memset(&at, 0, sizeof(at));
	for (f = 0; f < FAN_FRAMES; f++) {
		info = fan_frame_run(&line, true, true, NULL);
		if (!seeded && info.noise_dbov < 127.0f) {
			seeded = f + 1u;
			seed = info.noise_dbov;
			seed_window = fan_window_get(&line);
		}
		if (f + 1u == FAN_SEED_BOUND)
			at = info;
		if (f < FAN_SEED_BOUND || !fan_window_get(&line))
			continue;
		window++;
		if (fan_dt_get(&line, &info))
			dt++;
	}

	case_rate(rate, "lec measures the near-end floor under a far end that "
		  "never pauses");
	printf("# seed %.2f dBov at %u ms against %.2f dBov, window active %u, "
	       "floor %.2f dBov at the end; after %u ms: %u trials, %u resets, "
	       "double talk on %u of %u window-active frames\n", (double)seed,
	       10u * seeded, floor, seed_window, (double)info.noise_dbov,
	       10u * FAN_SEED_BOUND, info.probes - at.probes,
	       info.diverge_resets - at.diverge_resets, dt, window);
	CHECK(seeded > 0u && seeded <= FAN_SEED_BOUND);
	CHECK(seed_window);
	CHECK(fabs(at.noise_dbov - floor) <= FAN_FLOOR_TOL_DB);
	CHECK(fabs(info.noise_dbov - floor) <= FAN_FLOOR_TOL_DB);
	CHECK_UINT(info.probes - at.probes, 0);
	CHECK_UINT(info.diverge_resets - at.diverge_resets, 0);
	CHECK_UINT(window, FAN_FRAMES - FAN_SEED_BOUND);
	CHECK(dt < FAN_DT_SHARE_MAX * window);
	udsp_ec_nlms.destroy(line.ec);
}

#define TURNS_CYCLE		500u
#define TURNS_FRAMES		300u
#define TURNS_NEAR_START	250u
#define TURNS_NEAR_DBM0		-20.0
#define TURNS_SEED_BOUND	(15u * FRAMES_PER_SECOND)
/*
 * The fan of fan_turns_test, with the far-end floor of dsp/lec.c switched off.
 * With the fan at FAN_DBM0 the seed comes at 0.9 s, before the first near
 * turn. The line then tests no seed under the turns, and a build without the
 * talker guard runs no trial on it.
 */
#define TURNS_FAN_DBM0		-48.0

/*
 * The fan line at 16 kHz carries a talker in turns, with the fan at
 * -48 dBm0 and the far-end floor switched off. The far words and a near
 * talker at -20 dBm0 take turns of 3 s in a cycle of 5 s. The seed must come
 * within 15 s, and the floor must end within 1.5 dB of -72 dBm0. The line
 * must run no trial and no reset, before the seed and after it. The near
 * turn stands 28 dB over the fan, so the talker guard of dsp/lec.c clears the
 * latch run before the seed.
 */
static void fan_turns_test(void)
{
	int16_t near[UDSP_FRAME_MAX];
	struct udsp_ec_nlms_info info;
	struct udsp_ec_nlms_info at;
	struct word_source talker;
	struct fan_line line;
	double floor = floor_dbov_get(FAN_FLOOR_DBM0);
	unsigned seeded = 0;
	unsigned f;

	fan_line_init(&line, UDSP_SAMPLE_RATE_WB, 0.0);
	udsp_ec_nlms_ref_floor_set(line.ec, false);
	fan_line_set(&line, TURNS_FAN_DBM0, FAN_ERL_DB);
	word_source_init(&talker, UDSP_SAMPLE_RATE_WB, TURNS_NEAR_DBM0,
			 0x0badcafeu);
	memset(&at, 0, sizeof(at));
	for (f = 0; f < FAN_FRAMES; f++) {
		unsigned p = f % TURNS_CYCLE;
		bool far = p < TURNS_FRAMES;
		bool talk = p >= TURNS_NEAR_START ||
			    (f >= TURNS_CYCLE &&
			     p < TURNS_NEAR_START + TURNS_FRAMES - TURNS_CYCLE);

		if (p == 0u)
			word_source_restart(&line.far);
		if (p == TURNS_NEAR_START)
			word_source_restart(&talker);
		if (talk)
			word_fill(&talker, near, line.frame);
		info = fan_frame_run(&line, true, far, talk ? near : NULL);
		if (!seeded && info.noise_dbov < 127.0f) {
			seeded = f + 1u;
			at = info;
		}
	}

	CASE("lec runs no trial for a talker in turns under a far end that "
	     "never pauses");
	printf("# seed at %u ms, %u trials and %u resets before it, %u and %u "
	       "after it, floor %.2f dBov against %.2f dBov\n", 10u * seeded,
	       at.probes, at.diverge_resets, info.probes - at.probes,
	       info.diverge_resets - at.diverge_resets,
	       (double)info.noise_dbov, floor);
	CHECK(seeded > 0u && seeded <= TURNS_SEED_BOUND);
	CHECK_UINT(at.probes, 0);
	CHECK_UINT(at.diverge_resets, 0);
	CHECK_UINT(info.probes - at.probes, 0);
	CHECK_UINT(info.diverge_resets - at.diverge_resets, 0);
	CHECK(fabs(info.noise_dbov - floor) <= FAN_FLOOR_TOL_DB);
	udsp_ec_nlms.destroy(line.ec);
}

#define MEASURED_PAUSE_FRAMES	100u
#define MEASURED_NOISE_FROM	500u
#define MEASURED_FRAMES		1500u
#define MEASURED_NOISE_DBM0	-60.0

/*
 * A measured floor under a far end that never pauses. A far-end pause of 1 s
 * measures the floor at -72 dBm0. The fan at -38 dBm0 and the words follow,
 * and the fan frames stay active. From 5 s a stationary near-end noise at
 * -60 dBm0 joins the microphone. For the 14 s after the pause the floor must
 * stay within 1.5 dB of -72 dBm0. A seed that also runs on a measured floor
 * takes the noise in as the floor.
 */
static void fan_measured_test(void)
{
	int16_t noise[UDSP_FRAME_SAMPLES];
	struct udsp_ec_nlms_info info;
	struct fan_line line;
	double floor = floor_dbov_get(FAN_FLOOR_DBM0);
	uint32_t noise_state = 0x600dcafeu;
	double worst = 0.0;
	float start = 127.0f;
	unsigned f;

	fan_line_init(&line, UDSP_SAMPLE_RATE, 0.0);
	for (f = 0; f < MEASURED_PAUSE_FRAMES; f++)
		info = fan_frame_run(&line, false, false, NULL);
	start = info.noise_dbov;
	for (f = 0; f < MEASURED_FRAMES; f++) {
		bool noisy = f >= MEASURED_NOISE_FROM;

		if (noisy)
			noise_fill(noise, UDSP_FRAME_SAMPLES,
				   MEASURED_NOISE_DBM0, &noise_state);
		info = fan_frame_run(&line, true, true, noisy ? noise : NULL);
		if (fabs(info.noise_dbov - floor) > worst)
			worst = fabs(info.noise_dbov - floor);
	}

	CASE("lec never raises a measured floor from an active frame");
	printf("# floor %.2f dBov after the pause against %.2f dBov, %.2f dBov "
	       "at the end, largest distance %.2f dB\n", (double)start, floor,
	       (double)info.noise_dbov, worst);
	CHECK(fabs(start - floor) <= FAN_FLOOR_TOL_DB);
	CHECK(worst <= FAN_FLOOR_TOL_DB);
	udsp_ec_nlms.destroy(line.ec);
}

#define RESEED_FLOOR_DBM0	-82.0
#define RESEED_FRAMES		(2u * FRAMES_PER_SECOND)

/*
 * A seeded floor that the filter output falsifies. The fan line at 8 kHz
 * runs until the seed and then 1 s more. The near-end floor then drops to
 * -82 dBm0. The stale rule drops the old floor. Within 2 s the seed must
 * measure the new one within 1.5 dB, on a frame of an active window. A seed
 * that opens once per instance leaves the line without a floor.
 */
static void fan_reseed_test(void)
{
	struct udsp_ec_nlms_info info;
	struct fan_line line;
	double low = floor_dbov_get(RESEED_FLOOR_DBM0);
	unsigned seeded = 0;
	unsigned found = 0;
	unsigned dropped = 0;
	unsigned reseeded = 0;
	bool reseed_window = false;
	float before;
	unsigned f;

	fan_line_init(&line, UDSP_SAMPLE_RATE, 0.0);
	for (f = 0; f < FAN_FRAMES && (!seeded || f < seeded + 100u); f++) {
		info = fan_frame_run(&line, true, true, NULL);
		if (!seeded && info.noise_dbov < 127.0f)
			seeded = f + 1u;
	}
	before = info.noise_dbov;
	line.floor_dbm0 = RESEED_FLOOR_DBM0;
	for (f = 0; f < RESEED_FRAMES; f++) {
		info = fan_frame_run(&line, true, true, NULL);
		if (!dropped && info.noise_dbov >= 127.0f)
			dropped = f + 1u;
		if (dropped && !reseeded && info.noise_dbov < 127.0f) {
			reseeded = f + 1u;
			reseed_window = fan_window_get(&line);
		}
		if (!found && fabs(info.noise_dbov - low) <= FAN_FLOOR_TOL_DB)
			found = f + 1u;
	}

	CASE("lec re-seeds a floor that the residual falsified");
	printf("# seed at %u ms, floor %.2f dBov before the drop; dropped "
	       "after %u ms, re-seeded after %u ms with the window active %u, "
	       "%.2f dBov after 2 s against %.2f dBov, within 1.5 dB after %u "
	       "ms\n", 10u * seeded, (double)before, 10u * dropped,
	       10u * reseeded, reseed_window, (double)info.noise_dbov, low,
	       10u * found);
	CHECK(seeded > 0u);
	CHECK(dropped > 0u);
	CHECK(reseed_window);
	CHECK(found > 0u);
	CHECK(fabs(info.noise_dbov - low) <= FAN_FLOOR_TOL_DB);
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * A fan in a narrow band. The fan of the line at 8 kHz is low-passed by a
 * first-order filter at 300 Hz and keeps its level of -38 dBm0. A band that
 * the far words do not teach leaves more fan echo in the filter output. The
 * seed must read the floor within 1.5 dB of -72 dBm0, on a frame of an active
 * window. The far words of this line teach the band under 300 Hz. The seed
 * therefore reads as it does under a white fan, with or without gate (c) of
 * dsp/lec.c.
 */
static void fan_lowpass_test(void)
{
	struct udsp_ec_nlms_info info;
	struct fan_line line;
	double floor = floor_dbov_get(FAN_FLOOR_DBM0);
	float seed = 127.0f;
	unsigned seeded = 0;
	bool seed_window = false;
	unsigned f;

	fan_line_init(&line, UDSP_SAMPLE_RATE, FAN_LOWPASS_HZ);
	for (f = 0; f < FAN_FRAMES; f++) {
		info = fan_frame_run(&line, true, true, NULL);
		if (!seeded && info.noise_dbov < 127.0f) {
			seeded = f + 1u;
			seed = info.noise_dbov;
			seed_window = fan_window_get(&line);
		}
	}

	CASE("lec keeps the echo of a fan outside the learned band out of the "
	     "seed");
	printf("# seed %.2f dBov at %u ms against %.2f dBov, window active %u, "
	       "%.2f dBov at the end\n", (double)seed, 10u * seeded, floor,
	       seed_window, (double)info.noise_dbov);
	CHECK(seeded > 0u);
	CHECK(seed_window);
	CHECK(fabs(seed - floor) <= FAN_FLOOR_TOL_DB);
	CHECK(fabs(info.noise_dbov - floor) <= FAN_FLOOR_TOL_DB);
	udsp_ec_nlms.destroy(line.ec);
}

#define STEP_MEASURE_FROM	(10u * FRAMES_PER_SECOND)
/* A canceller without the seed reads 23.87 dB on the fan line. */
#define STEP_HEAD_ERLE_DB	23.9
#define STEP_ERLE_TOL_DB	1.0

/*
 * A seeded floor and the step control. The fan line at 8 kHz runs for 30 s,
 * and the filter ERLE counts the far-end frames of the last 20 s. The step
 * control of dsp/lec.c reads the seeded floor. The ERLE must come within 1 dB
 * of the figure of a canceller without the seed, which measures no floor on
 * this line. A step that a measured floor freezes fails here. A seed louder
 * than the floor does not: it closes the step on the fan-only frames, and
 * the ERLE rises.
 */
static void fan_step_test(void)
{
	struct udsp_ec_nlms_info info;
	struct fan_line line;
	unsigned f;

	fan_line_init(&line, UDSP_SAMPLE_RATE, 0.0);
	for (f = 0; f < FAN_FRAMES; f++) {
		line.measure = f >= STEP_MEASURE_FROM;
		info = fan_frame_run(&line, true, true, NULL);
	}

	CASE("lec keeps a seeded floor from freezing the step");
	printf("# filter ERLE %.2f dB over the last 20 s against %.2f dB, "
	       "floor %.2f dBov\n", fan_filter_erle_get(&line),
	       STEP_HEAD_ERLE_DB, (double)info.noise_dbov);
	CHECK(fan_filter_erle_get(&line) >= STEP_HEAD_ERLE_DB -
	      STEP_ERLE_TOL_DB);
	udsp_ec_nlms.destroy(line.ec);
}

#define BLOCK_NEAR_DBM0		-19.0

/*
 * Gate (f) of dsp/lec.c on a line without a floor. A flat 12 dB hybrid
 * converges for 1.5 s on a white far end at -10 dBm0. A steady white talker
 * 3 dB over the echo then speaks for 1 s. In a second instance the hybrid
 * gets 6 dB louder for 3 s. The tracked ERLE reads over 50 dB, so gates (b)
 * to (e) pass both. The floor must stay unmeasured in both; without gate (f)
 * the seed takes the talker and the misaligned echo in as the floor.
 */
static void seed_block_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t state = 0x0badc0deu;
	uint32_t near_state = 0xfeedfaceu;
	float loudest = 127.0f;
	unsigned f;

	ec = udsp_ec_nlms.create_rate(48, rate);
	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, rate, 8, 12.0);
	(void)erle_run(ec, &path, &state, -10.0,
		       FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20);
	for (f = 0; f < FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t near[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		unsigned i;

		noise_fill(ref, path.frame, -10.0, &state);
		path_run(&path, ref, echo);
		noise_fill(near, path.frame, BLOCK_NEAR_DBM0, &near_state);
		for (i = 0; i < path.frame; i++)
			mic[i] = sample_clamp((double)echo[i] + near[i]);
		udsp_ec_nlms.run(ec, mic, ref);
		udsp_ec_nlms_info_get(ec, &info);
		if (info.noise_dbov < loudest)
			loudest = info.noise_dbov;
	}
	udsp_ec_nlms.destroy(ec);

	case_rate(rate, "lec keeps a steady near talker out of the seed");
	printf("# floor %.2f dBov through 1 s of a talker 3 dB over the echo\n",
	       (double)loudest);
	CHECK(loudest == 127.0f);

	ec = udsp_ec_nlms.create_rate(48, rate);
	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, rate, 8, 12.0);
	(void)erle_run(ec, &path, &state, -10.0,
		       FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2, 20);
	path_erl_set(&path, 6.0);
	loudest = 127.0f;
	for (f = 0; f < 3u * FRAMES_PER_SECOND; f++) {
		(void)erle_run(ec, &path, &state, -10.0, 1u, 1u);
		udsp_ec_nlms_info_get(ec, &info);
		if (info.noise_dbov < loudest)
			loudest = info.noise_dbov;
	}

	case_rate(rate, "lec keeps the echo of a changed hybrid out of the "
		  "seed");
	printf("# floor %.2f dBov through 3 s after the hybrid got 6 dB "
	       "louder, %u trials\n", (double)loudest, info.probes);
	CHECK(loudest == 127.0f);
	udsp_ec_nlms.destroy(ec);
}

#define TRIAL_SEED_LOUD_DBM0	-20.0
#define TRIAL_SEED_TALK_DBM0	-10.0
#define TRIAL_SEED_CONVERGE	300u
#define TRIAL_SEED_RUN_AT	485u
#define TRIAL_SEED_LIMIT	1000u

/* One 8 kHz frame of the trial line: white far end at far_dbm0, the floor,
 * and a white talker at talk_dbm0 when talk is set. */
static void seed_trial_frame_run(struct udsp_ec *ec, struct echo_path *path,
				 double far_dbm0, bool talk, uint32_t *state,
				 uint32_t *floor_state)
{
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t echo[UDSP_FRAME_SAMPLES];
	int16_t near[UDSP_FRAME_SAMPLES];
	int16_t mic[UDSP_FRAME_SAMPLES];
	unsigned i;

	noise_fill(ref, UDSP_FRAME_SAMPLES, far_dbm0, state);
	path_run(path, ref, echo);
	floor_add(mic, echo, FAN_FLOOR_DBM0, floor_state);
	if (talk) {
		noise_fill(near, UDSP_FRAME_SAMPLES, TRIAL_SEED_TALK_DBM0,
			   state);
		for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
			mic[i] = sample_clamp((double)mic[i] + near[i]);
	}
	udsp_ec_nlms.run(ec, mic, ref);
}

/*
 * A flat 24 dB hybrid with a floor at -72 dBm0 converges on a white far end
 * at -20 dBm0. Gate (c) of dsp/lec.c keeps the seed shut there. A talker at
 * -10 dBm0 then builds the latch run to 485 frames.
 * At the fan level of -38 dBm0 the frames qualify, and the latch starts a
 * trial before the seed run is full. The trial frames clear the run, so the
 * seed must come 20 frames after the last trial frame. A run that the trial
 * keeps seeds earlier.
 */
static void seed_trial_test(void)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x31415926u;
	uint32_t floor_state = 0x2468acefu;
	unsigned before = 0;
	unsigned trial_end = 0;
	unsigned seeded = 0;
	float floor_at_start = 0.0f;
	bool started = false;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, FAN_ERL_DB);
	for (f = 0; f < TRIAL_SEED_CONVERGE; f++)
		seed_trial_frame_run(ec, &path, TRIAL_SEED_LOUD_DBM0, false,
				     &state, &floor_state);
	for (f = 0; f < TRIAL_SEED_LIMIT; f++) {
		seed_trial_frame_run(ec, &path, TRIAL_SEED_LOUD_DBM0, true,
				     &state, &floor_state);
		udsp_ec_nlms_frame_get(ec, &reading);
		if (reading.dt_run >= TRIAL_SEED_RUN_AT)
			break;
	}
	for (f = 0; f < TRIAL_SEED_LIMIT && !seeded; f++) {
		seed_trial_frame_run(ec, &path, FAN_DBM0, false, &state,
				     &floor_state);
		udsp_ec_nlms_info_get(ec, &info);
		udsp_ec_nlms_frame_get(ec, &reading);
		if (!info.probes)
			before++;
		else if (!before || info.probes > 1u)
			break;
		if (info.probes && !started) {
			started = true;
			floor_at_start = info.noise_dbov;
		}
		if (reading.probing)
			trial_end = f + 1u;
		if (info.noise_dbov < 127.0f)
			seeded = f + 1u;
	}
	udsp_ec_nlms_info_get(ec, &info);

	CASE("lec starts the seed run again after a trial");
	printf("# %u fan frames before the trial, floor %.2f dBov as it "
	       "started, trial ended at frame %u, seed at frame %u, %u "
	       "trials\n", before, (double)floor_at_start, trial_end, seeded,
	       info.probes);
	CHECK_UINT(info.probes, 1);
	CHECK(floor_at_start == 127.0f);
	CHECK(trial_end > before);
	CHECK_UINT(seeded - trial_end + 1u, 20);
	udsp_ec_nlms.destroy(ec);
}

/* LEC_GEIGEL_LATCH_MAX of dsp/lec.c, and the latch run at which the talk of
 * the snapshot case stops, short of it by less than one hangover. */
#define SNAP_LATCH_FRAMES	500u
#define SNAP_TALK_RUN		481u
#define SNAP_LIMIT_FRAMES	(10u * FRAMES_PER_SECOND)
#define SNAP_COMPARE_FRAMES	20u
#define SNAP_FAR_DBM0		-10.0
/* The talker under the far end: 3 dB over the echo of the 12 dB hybrid. */
#define SNAP_TALK_UNDER_DB	9.0

/*
 * The snapshot line: two instances that take the same frames on a flat 12 dB
 * hybrid at 8 kHz, under a white far end at far_dbm0. near holds the talker
 * of the next frame, and the two outputs hold the outputs of the last frame.
 */
struct snap_line {
	struct echo_path path;
	struct udsp_ec *lead;
	struct udsp_ec *twin;
	double far_dbm0;
	uint32_t state;
	uint32_t near_state;
	unsigned run_before;
	bool clear_before;
	int16_t near[UDSP_FRAME_SAMPLES];
	int16_t lead_out[UDSP_FRAME_SAMPLES];
	int16_t twin_out[UDSP_FRAME_SAMPLES];
};

/* One frame of the far end to both instances, with near on top when it is
 * given. */
static void snap_frame_run(struct snap_line *s, const int16_t *near)
{
	int16_t ref[UDSP_FRAME_SAMPLES];

	noise_fill(ref, UDSP_FRAME_SAMPLES, s->far_dbm0, &s->state);
	twin_step(s->lead, s->twin, &s->path, ref, near, s->lead_out,
		  s->twin_out, NULL);
}

/* A white talker at talk_dbm0 for the next frame. */
static const int16_t *snap_talk_get(struct snap_line *s, double talk_dbm0)
{
	noise_fill(s->near, UDSP_FRAME_SAMPLES, talk_dbm0, &s->near_state);
	return s->near;
}

/*
 * The run up to a latch that is due. After 1.5 s of far end, a near talker
 * 3 dB over the echo talks until the latch run nears its limit. A far-end
 * pause then lets the hangover run out, so the latch comes due on the next
 * frame of the far end. clear_before and run_before hold the double talk and
 * the latch run at the end of the pause.
 */
static void snap_line_build(struct snap_line *s, double far_dbm0)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	int16_t ref[UDSP_FRAME_SAMPLES];
	unsigned f;

	memset(s, 0, sizeof(*s));
	memset(&info, 0, sizeof(info));
	s->lead = udsp_ec_nlms.create(48);
	s->twin = udsp_ec_nlms.create(48);
	s->far_dbm0 = far_dbm0;
	s->state = 0x0badc0deu;
	s->near_state = 0xfeedfaceu;
	udsp_ec_nlms.nlp(s->lead, false);
	udsp_ec_nlms.nlp(s->twin, false);
	path_flat_init(&s->path, UDSP_SAMPLE_RATE, 8, 12.0);
	for (f = 0; f < FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2; f++)
		snap_frame_run(s, NULL);
	for (f = 0; f < SNAP_LIMIT_FRAMES; f++) {
		snap_frame_run(s, snap_talk_get(s, far_dbm0 -
						SNAP_TALK_UNDER_DB));
		udsp_ec_nlms_frame_get(s->lead, &reading);
		if (reading.dt_fired && reading.dt_run >= SNAP_TALK_RUN)
			break;
	}
	memset(ref, 0, sizeof(ref));
	for (f = 0; f < SNAP_LIMIT_FRAMES; f++) {
		twin_step(s->lead, s->twin, &s->path, ref, NULL, s->lead_out,
			  s->twin_out, NULL);
		udsp_ec_nlms_info_get(s->lead, &info);
		if (!info.double_talk)
			break;
	}
	udsp_ec_nlms_frame_get(s->lead, &reading);
	s->clear_before = !info.double_talk;
	s->run_before = reading.dt_run;
}

/*
 * The snapshot of a trial that starts on a frame that adapted and then fired.
 * The snapshot line comes to a due latch, and the latch comes due on the next
 * frame, which adapts and fires. The twin freezes before that frame, and the
 * talker refuses the trial of the first instance. Both filters then freeze,
 * and their energies and outputs must be equal to the bit.
 */
static void probe_snapshot_test(void)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info lead_info;
	struct udsp_ec_nlms_info twin_info;
	struct snap_line s;
	unsigned differ = 0;
	unsigned f;

	snap_line_build(&s, SNAP_FAR_DBM0);
	udsp_ec_nlms.freeze(s.twin, true);
	CASE("lec trial restores the filter that its start frame ended with");
	for (f = 0; f <= TRIAL_BUDGET; f++) {
		snap_frame_run(&s, snap_talk_get(&s, SNAP_FAR_DBM0 -
						 SNAP_TALK_UNDER_DB));
		if (f)
			continue;
		udsp_ec_nlms_info_get(s.lead, &lead_info);
		udsp_ec_nlms_frame_get(s.lead, &reading);
		CHECK(s.clear_before);
		CHECK(s.run_before >= SNAP_LATCH_FRAMES);
		CHECK_UINT(lead_info.probes, 1);
		CHECK(reading.dt_fired);
	}
	udsp_ec_nlms.freeze(s.lead, true);
	for (f = 0; f < SNAP_COMPARE_FRAMES; f++) {
		snap_frame_run(&s, NULL);
		if (memcmp(s.lead_out, s.twin_out, sizeof(s.lead_out)))
			differ++;
	}
	udsp_ec_nlms_info_get(s.lead, &lead_info);
	udsp_ec_nlms_info_get(s.twin, &twin_info);
	printf("# latch run %u after the pause; coefficient energy %.9f "
	       "against the twin's %.9f; %u of %u frames differ\n",
	       s.run_before, (double)lead_info.coef_energy,
	       (double)twin_info.coef_energy, differ, SNAP_COMPARE_FRAMES);
	CHECK_UINT(lead_info.probes, 1);
	CHECK_UINT(lead_info.diverge_resets, 0);
	CHECK(lead_info.coef_energy == twin_info.coef_energy);
	CHECK_UINT(differ, 0);
	udsp_ec_nlms.destroy(s.lead);
	udsp_ec_nlms.destroy(s.twin);
}

#define RING_DBM0		-20.0
#define RING_ON_FRAMES		200u
#define RING_CYCLE_FRAMES	600u
#define RING_FRAMES		(2u * RING_CYCLE_FRAMES)
#define RING_ERL_DB		24.0
#define RING_FLOOR_DBM0		-70.0
#define RING_WORDS_DBM0		-20.0
#define RING_SPEECH_FRAMES	(20u * FRAMES_PER_SECOND)
#define RING_FIRST_FRAMES	(2u * FRAMES_PER_SECOND)
#define RING_LAST_FRAMES	(8u * FRAMES_PER_SECOND)
/* LEC_GEIGEL_TRUST_DB and LEC_GEIGEL_TRUST_COEF_RATIO of dsp/lec.c. */
#define RING_TRUST_DB		18.0f
#define RING_COEF_RATIO		10.0f

/*
 * The ringback line: a flat 24 dB hybrid at 8 ms, the default tail, the
 * processor off and a near-end floor at -70 dBm0. The ringback is 440 plus
 * 480 Hz at -20 dBm0 each, 2 s on and 4 s off, for two cycles. Far-end words
 * at -20 dBm0 follow it. A line of the lockout cases (struct ring_spec) sets
 * another floor level, and clears floor on the frames before its floor.
 */
struct ring_line {
	struct echo_path path;
	struct word_source far;
	struct udsp_ec *ec;
	unsigned rate;
	unsigned frame;
	unsigned long phase;
	uint32_t far_state;
	uint32_t near_state;
	uint32_t floor_state;
	uint32_t held;
	double floor_dbm0;
	bool floor;
};

/*
 * The frames of one stretch whose own reference is active: those whose
 * update did not stand, those in double talk, and those that fired on a bar
 * that the estimate set. The energies of the same frames give the filter
 * ERLE (weak_filter_erle_get).
 */
struct ring_stat {
	unsigned active;
	unsigned held;
	unsigned dt;
	unsigned est_fired;
	double echo;
	double residual;
	double floor;
};

static void ring_line_init(struct ring_line *line, unsigned rate)
{
	memset(line, 0, sizeof(*line));
	line->rate = rate;
	line->frame = rate / 100u;
	line->ec = udsp_ec_nlms.create_rate(0, rate);
	udsp_ec_nlms.nlp(line->ec, false);
	path_flat_init(&line->path, rate, 8, RING_ERL_DB);
	word_source_init(&line->far, rate, RING_WORDS_DBM0, 0x13572468u);
	line->far_state = 0x0badc0deu;
	line->near_state = 0xfeedfaceu;
	line->floor_state = 0x2468acefu;
	line->floor_dbm0 = RING_FLOOR_DBM0;
	line->floor = true;
}

/* The two tones of the ringback, at RING_DBM0 each. */
static void ring_tones_fill(struct ring_line *line, int16_t *ref)
{
	int16_t high[UDSP_FRAME_MAX];
	unsigned long phase = line->phase;
	unsigned i;

	tone_fill(ref, line->frame, RING_DBM0, 440.0, &line->phase,
		  line->rate);
	tone_fill(high, line->frame, RING_DBM0, 480.0, &phase, line->rate);
	for (i = 0; i < line->frame; i++)
		ref[i] = sample_clamp((double)ref[i] + high[i]);
}

static void ring_fill(struct ring_line *line, int16_t *ref, unsigned f)
{
	if (f % RING_CYCLE_FRAMES >= RING_ON_FRAMES) {
		memset(ref, 0, line->frame * sizeof(*ref));
		return;
	}
	ring_tones_fill(line, ref);
}

/* The planes of the last frame of a ring line. They hold the reference, the
 * near speech, the echo, the floor, the microphone and the output. */
struct ring_planes {
	int16_t ref[UDSP_FRAME_MAX];
	int16_t near[UDSP_FRAME_MAX];
	int16_t echo[UDSP_FRAME_MAX];
	int16_t hiss[UDSP_FRAME_MAX];
	int16_t mic[UDSP_FRAME_MAX];
	int16_t out[UDSP_FRAME_MAX];
};

/* One frame of the line, with near on top when it is given. st takes the
 * frame when it is given and the frame's own reference is active. */
static void ring_planes_run(struct ring_line *line, const int16_t *ref,
			    const int16_t *near, struct ring_stat *st,
			    struct ring_planes *p)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	int16_t *echo = p->echo;
	int16_t *hiss = p->hiss;
	int16_t *mic = p->out;
	uint32_t held = line->held;
	unsigned i;

	memcpy(p->ref, ref, line->frame * sizeof(*ref));
	memset(p->near, 0, line->frame * sizeof(*p->near));
	if (near)
		memcpy(p->near, near, line->frame * sizeof(*near));
	path_run(&line->path, ref, echo);
	memset(hiss, 0, line->frame * sizeof(*hiss));
	if (line->floor)
		noise_fill(hiss, line->frame, line->floor_dbm0,
			   &line->floor_state);
	for (i = 0; i < line->frame; i++)
		mic[i] = sample_clamp((double)echo[i] + hiss[i] + p->near[i]);
	memcpy(p->mic, mic, line->frame * sizeof(*mic));
	udsp_ec_nlms.run(line->ec, mic, ref);
	udsp_ec_nlms_info_get(line->ec, &info);
	udsp_ec_nlms_frame_get(line->ec, &reading);
	line->held = info.hold_frames;
	if (!st || !frame_loud(ref, line->frame))
		return;
	st->active++;
	st->held += info.hold_frames != held;
	st->dt += info.double_talk;
	st->est_fired += reading.dt_fired && !reading.classic_arm;
	for (i = 0; i < line->frame; i++) {
		st->echo += (double)echo[i] * echo[i];
		st->residual += (double)mic[i] * mic[i];
		st->floor += (double)hiss[i] * hiss[i];
	}
}

static void ring_frame_run(struct ring_line *line, const int16_t *ref,
			   const int16_t *near, struct ring_stat *st)
{
	struct ring_planes p;

	ring_planes_run(line, ref, near, st, &p);
}

static void ring_tone_run(struct ring_line *line)
{
	int16_t ref[UDSP_FRAME_MAX];
	unsigned f;

	for (f = 0; f < RING_FRAMES; f++) {
		ring_fill(line, ref, f);
		ring_frame_run(line, ref, NULL, NULL);
	}
}

static void ring_words_run(struct ring_line *line, struct ring_stat *st)
{
	int16_t ref[UDSP_FRAME_MAX];

	word_fill(&line->far, ref, line->frame);
	ring_frame_run(line, ref, NULL, st);
}

static double ring_gap_db_get(const struct udsp_ec_nlms_info *info)
{
	if (!(info->coef_energy > 0.0f) || !(info->echo_gain > 0.0f))
		return -200.0;
	return 10.0 * log10((double)info->coef_energy / info->echo_gain);
}

/* LEC_GEIGEL_TRUST_FRAMES and LEC_GEIGEL_TRUST_PAUSE_FRAMES of dsp/lec.c,
 * and the second burst of the ringback of ring_trust_test. */
#define TRUST_FRAMES		200u
#define TRUST_PAUSE_FRAMES	200u
#define RING_TRUST_ON_FRAMES	300u

/* The ringback of ring_tone_run with a second burst of RING_TRUST_ON_FRAMES
 * frames. The far end returns after a pause of 4 s, so the trust count of
 * dsp/lec.c restarts at the second burst and completes on it. */
static void ring_trust_tone_run(struct ring_line *line)
{
	int16_t ref[UDSP_FRAME_MAX];
	unsigned f;

	for (f = 0; f < RING_FRAMES; f++) {
		unsigned on = f < RING_CYCLE_FRAMES ? RING_ON_FRAMES :
						      RING_TRUST_ON_FRAMES;

		if (f % RING_CYCLE_FRAMES < on)
			ring_tones_fill(line, ref);
		else
			memset(ref, 0, line->frame * sizeof(*ref));
		ring_frame_run(line, ref, NULL, NULL);
	}
}

/*
 * The trust rule on a filter that learned a ringback. The tone earns the
 * filter a tracked ERLE over the 18 dB trust line. Its coefficient energy
 * stands more than 10 dB under the learned gain. The second burst runs for
 * 3 s, so the trust count of dsp/lec.c is complete at the end of the
 * ringback. The energy rule alone must then refuse the trust. No frame of the
 * first second of far-end words must fire on a bar that the estimate set.
 */
static void ring_trust_test(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info at;
	struct ring_stat first;
	struct ring_line line;
	unsigned f;

	memset(&first, 0, sizeof(first));
	ring_line_init(&line, rate);
	ring_trust_tone_run(&line);
	udsp_ec_nlms_info_get(line.ec, &at);
	udsp_ec_nlms_frame_get(line.ec, &reading);
	for (f = 0; f < FRAMES_PER_SECOND; f++)
		ring_words_run(&line, &first);
	udsp_ec_nlms.destroy(line.ec);

	case_rate(rate, "lec refuses the trust of a filter trained on a "
		  "ringback");
	printf("# after the ringback: tracked ERLE %.2f dB, coefficient "
	       "energy %.2f dB against the learned gain, trust count %u, "
	       "trusted %u; first second of words: %u of %u active frames in "
	       "double talk, %u fired on the estimate\n", (double)at.erle_db,
	       ring_gap_db_get(&at), reading.trust_run, reading.trusted,
	       first.dt, first.active, first.est_fired);
	CHECK(at.erle_db >= RING_TRUST_DB);
	CHECK(at.coef_energy * RING_COEF_RATIO < at.echo_gain);
	CHECK(reading.trust_run >= TRUST_FRAMES);
	CHECK(!reading.trusted);
	CHECK(first.active > 0u);
	CHECK_UINT(first.est_fired, 0);
}

/*
 * The ringback line, then 20 s of far-end words, reproduces the hold of a
 * stale trusted estimate. The ringback must earn the filter a tracked ERLE of
 * 18 dB or more. The trust rules of dsp/lec.c refuse that trust at the end of
 * the ringback. The words must then run no trial and hold double talk on 20
 * percent or less of the first 2 s. The line must run no reset and hold at
 * most 20 percent of the active frames of the last 8 s. The filter ERLE of
 * those frames must reach 15 dB, where a canceller without the stale run
 * reads 0.7 dB.
 */
static void stale_hold_test(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct udsp_ec_nlms_info at;
	struct ring_stat first;
	struct ring_stat last;
	struct ring_line line;
	unsigned trial = 0;
	double erle;
	unsigned f;

	memset(&first, 0, sizeof(first));
	memset(&last, 0, sizeof(last));
	ring_line_init(&line, rate);
	ring_tone_run(&line);
	udsp_ec_nlms_info_get(line.ec, &at);
	udsp_ec_nlms_frame_get(line.ec, &reading);
	for (f = 0; f < RING_SPEECH_FRAMES; f++) {
		struct ring_stat *st = NULL;

		if (f < RING_FIRST_FRAMES)
			st = &first;
		else if (f >= RING_SPEECH_FRAMES - RING_LAST_FRAMES)
			st = &last;
		ring_words_run(&line, st);
		udsp_ec_nlms_info_get(line.ec, &info);
		if (!trial && info.probes > at.probes)
			trial = f + 1u;
	}
	udsp_ec_nlms.destroy(line.ec);
	erle = weak_filter_erle_get(last.echo, last.residual, last.floor);

	case_rate(rate, "lec ends the hold of a stale trusted estimate within "
		  "seconds");
	printf("# tracked ERLE %.2f dB at the first word, trusted %u; first "
	       "2 s: double talk on %u of %u active frames; %u trials, the "
	       "first at %u ms, %u resets; last 8 s: %u of %u active frames "
	       "held, filter ERLE %.2f dB\n", (double)at.erle_db,
	       reading.trusted, first.dt, first.active,
	       info.probes - at.probes, 10u * trial, info.diverge_resets,
	       last.held, last.active, erle);
	CHECK(at.erle_db >= RING_TRUST_DB);
	CHECK(!reading.trusted);
	CHECK(first.dt * 10u <= first.active * 2u);
	CHECK_UINT(info.probes - at.probes, 0);
	CHECK_UINT(info.diverge_resets, 0);
	CHECK(last.active > 0u);
	CHECK(last.held * 10u <= last.active * 2u);
	CHECK(erle >= 15.0);
}

#define TALKER_CONVERGE_FRAMES	(3u * FRAMES_PER_SECOND)
#define TALKER_FRAMES		(20u * FRAMES_PER_SECOND)
/* The echo of the far end through the hybrid of the ringback line. */
#define TALKER_ECHO_DBM0	(RING_WORDS_DBM0 - RING_ERL_DB)
/* LEC_GEIGEL_STALE_FRAMES and LEC_GEIGEL_LATCH_MAX of dsp/lec.c. */
#define TALKER_STALE_FRAMES	200u
#define TALKER_LATCH_FRAMES	500u

/* Runs the ringback line on white noise at the level of the words. */
static void ring_white_run(struct ring_line *line, unsigned frames)
{
	int16_t ref[UDSP_FRAME_MAX];
	unsigned f;

	for (f = 0; f < frames; f++) {
		noise_fill(ref, line->frame, RING_WORDS_DBM0, &line->far_state);
		ring_frame_run(line, ref, NULL, NULL);
	}
}

/*
 * A talker that the far end cannot explain starts no trial of the stale run.
 * The ringback line converges on white noise for 3 s. Far-end words then run
 * for 20 s with a near talker at the same level, 24 dB over the echo. The
 * detector fires on the trusted estimate on more frames than the stale run
 * needs, and each of those frames stands over the classic bar. The stale run
 * must stay under its trigger, and the line must run no trial and no reset.
 * A stale run that counts every trusted verdict starts a trial.
 */
static void stale_talker_test(void)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct word_source talker;
	struct ring_line line;
	unsigned fired = 0;
	unsigned longest = 0;
	unsigned f;

	ring_line_init(&line, UDSP_SAMPLE_RATE);
	word_source_init(&talker, UDSP_SAMPLE_RATE, RING_WORDS_DBM0,
			 0x0badcafeu);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES);
	for (f = 0; f < TALKER_FRAMES; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t near[UDSP_FRAME_SAMPLES];

		word_fill(&line.far, ref, line.frame);
		word_fill(&talker, near, line.frame);
		ring_frame_run(&line, ref, near, NULL);
		udsp_ec_nlms_frame_get(line.ec, &reading);
		fired += reading.dt_fired && reading.trusted;
		if (reading.stale_run > longest)
			longest = reading.stale_run;
	}
	udsp_ec_nlms_info_get(line.ec, &info);
	udsp_ec_nlms.destroy(line.ec);

	CASE("lec starts no stale-estimate trial for a talker the far end "
	     "cannot explain");
	printf("# %u frames fired on a trusted estimate, longest stale run "
	       "%u, %u trials, %u resets\n", fired, longest, info.probes,
	       info.diverge_resets);
	CHECK(fired >= TALKER_STALE_FRAMES);
	CHECK(longest < TALKER_STALE_FRAMES);
	CHECK_UINT(info.probes, 0);
	CHECK_UINT(info.diverge_resets, 0);
}

/* LEC_TRIAL_STANDDOWN_FRAMES plus LEC_PROBE_FRAMES of dsp/lec.c: the
 * shortest distance between the starts of two trials of the stale run, in
 * active frames. LEC_TRIAL_STANDDOWN_FRAMES alone is the shortest distance
 * between two trials of any arm under a talker who never pauses. */
#define EXPOSE_CADENCE		700u
#define EXPOSE_ANY_CADENCE	600u
#define EXPOSE_FRAMES		(30u * FRAMES_PER_SECOND)
#define EXPOSE_RESTORE		0.01
#define EXPOSE_NEAR_DB		2.0
#define EXPOSE_LEAK_DB		3.0

/*
 * The trials of one exposure run. A trial is the stale run's when the frame
 * before it read a stale run at the trigger and a latch run under its own.
 * A trial counts as kept when the coefficient energy it ends with stands
 * more than 1 percent from the energy it started with. The gaps count active
 * frames, the frames whose own reference is active. The energies cover the
 * trial frames: the microphone, the output, the echo, the echo that the
 * output leaves (the output less the near speech and the floor), and the near
 * speech against the output.
 */
struct expose_stat {
	unsigned trials;
	unsigned stale;
	unsigned kept;
	unsigned gap_min;
	unsigned stale_gap_min;
	unsigned active;
	unsigned trial_at;
	unsigned stale_at;
	float start_energy;
	bool open;
	double mic;
	double out;
	double echo;
	double leak;
	double near;
	double product;
};

static void expose_gap_note(unsigned *gap_min, unsigned *at, unsigned count,
			    unsigned active)
{
	if (count && (!*gap_min || active - *at < *gap_min))
		*gap_min = active - *at;
	*at = active;
}

static void expose_trial_end(struct expose_stat *st,
			     const struct udsp_ec_nlms_info *info)
{
	double miss = fabs((double)info->coef_energy - st->start_energy);

	st->open = false;
	if (miss > EXPOSE_RESTORE * st->start_energy)
		st->kept++;
}

static void expose_trial_step(struct expose_stat *st,
			      const struct udsp_ec_nlms_frame *before,
			      const struct udsp_ec_nlms_frame *reading,
			      const struct udsp_ec_nlms_info *info,
			      unsigned probes)
{
	bool stale = before->stale_run >= TALKER_STALE_FRAMES &&
		     before->dt_run < TALKER_LATCH_FRAMES;

	if (st->open && !reading->probing)
		expose_trial_end(st, info);
	if (info->probes == probes)
		return;
	expose_gap_note(&st->gap_min, &st->trial_at, st->trials, st->active);
	st->trials++;
	st->open = true;
	st->start_energy = info->coef_energy;
	if (!stale)
		return;
	expose_gap_note(&st->stale_gap_min, &st->stale_at, st->stale,
			st->active);
	st->stale++;
}

static void expose_energy_add(struct expose_stat *st, const int16_t *near,
			      const struct ring_planes *p, unsigned frame)
{
	unsigned i;

	for (i = 0; i < frame; i++) {
		double talk = near ? near[i] : 0.0;
		double leak = (double)p->out[i] - talk - p->hiss[i];

		st->mic += (double)p->mic[i] * p->mic[i];
		st->out += (double)p->out[i] * p->out[i];
		st->echo += (double)p->echo[i] * p->echo[i];
		st->leak += leak * leak;
		st->near += talk * talk;
		st->product += (double)p->out[i] * talk;
	}
}

/* One frame of an exposure run, with near on top when it is given. */
static void expose_frame_run(struct ring_line *line, const int16_t *ref,
			     const int16_t *near, struct expose_stat *st)
{
	struct udsp_ec_nlms_frame before;
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct ring_planes p;
	uint32_t probes;

	udsp_ec_nlms_info_get(line->ec, &info);
	udsp_ec_nlms_frame_get(line->ec, &before);
	probes = info.probes;
	ring_planes_run(line, ref, near, NULL, &p);
	udsp_ec_nlms_info_get(line->ec, &info);
	udsp_ec_nlms_frame_get(line->ec, &reading);
	if (frame_loud(ref, line->frame))
		st->active++;
	expose_trial_step(st, &before, &reading, &info, probes);
	if (st->open)
		expose_energy_add(st, near, &p, line->frame);
}

/* An exposure frame on a white far end at the level of the words. */
static void expose_white_run(struct ring_line *line, const int16_t *near,
			     struct expose_stat *st)
{
	int16_t ref[UDSP_FRAME_MAX];

	noise_fill(ref, line->frame, RING_WORDS_DBM0, &line->far_state);
	expose_frame_run(line, ref, near, st);
}

static double expose_db_get(double num, double den)
{
	if (!(num > 0.0) || !(den > 0.0))
		return 200.0;
	return 10.0 * log10(num / den);
}

static double expose_near_db_get(const struct expose_stat *st)
{
	return near_gain_db_get(st->product, st->near);
}

static void expose_print(const char *what, const struct expose_stat *st)
{
	printf("# %s: %u trials over %u active frames, %u of the stale run, "
	       "%u kept; shortest gaps %u, and %u between two of the stale "
	       "run; in the trials: near speech %.2f dB through, output %.2f "
	       "dB against the microphone, echo %.2f dB under its level\n",
	       what, st->trials, st->active, st->stale, st->kept, st->gap_min,
	       st->stale_gap_min, expose_near_db_get(st),
	       expose_db_get(st->out, st->mic),
	       expose_db_get(st->echo, st->leak));
}

/* The bounds of the trial frames that every exposure case shares. */
static void expose_bounds_check(const struct expose_stat *st)
{
	CHECK(fabs(expose_near_db_get(st)) <= EXPOSE_NEAR_DB);
	CHECK(expose_db_get(st->out, st->mic) <= TALK_BURST_DB);
	CHECK(expose_db_get(st->echo, st->leak) >= EXPOSE_LEAK_DB);
}

#define QUIET_FRAMES		(60u * FRAMES_PER_SECOND)
#define QUIET_FIRST_FRAMES	(20u * FRAMES_PER_SECOND)
#define QUIET_UNDER_DB		2.0

/*
 * The ringback line converges on white noise, then far-end words run for
 * 60 s with near words 2 dB under the echo. The talker fires on the estimate
 * under the classic bar, so the stale run starts trials, and the talker fails
 * each one. The line must run one trial in the first 20 s and no reset, and
 * each trial must restore the coefficient energy within 1 percent. Two trials
 * must stand 700 active frames apart or more, because a rejected trial stands
 * the stale run down. Without that stand-down, the next trial follows after
 * 330 to 530 active frames. The trial frames keep the bounds of the steady
 * talker, and the talker passes them 1.5 dB quieter.
 */
static void stale_quiet_test(void)
{
	struct udsp_ec_nlms_info info;
	struct word_source talker;
	struct expose_stat st;
	struct ring_line line;
	unsigned first = 0;
	unsigned f;

	memset(&st, 0, sizeof(st));
	ring_line_init(&line, UDSP_SAMPLE_RATE);
	word_source_init(&talker, UDSP_SAMPLE_RATE,
			 TALKER_ECHO_DBM0 - QUIET_UNDER_DB, 0x31415926u);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES);
	for (f = 0; f < QUIET_FRAMES; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t near[UDSP_FRAME_SAMPLES];

		word_fill(&line.far, ref, line.frame);
		word_fill(&talker, near, line.frame);
		expose_frame_run(&line, ref, near, &st);
		if (f + 1u == QUIET_FIRST_FRAMES)
			first = st.trials;
	}
	udsp_ec_nlms_info_get(line.ec, &info);
	udsp_ec_nlms.destroy(line.ec);

	CASE("lec rolls back a stale-estimate trial on a quiet talker and "
	     "stands down");
	expose_print("quiet talker", &st);
	printf("# %u trials in the first 20 s, %u resets\n", first,
	       info.diverge_resets);
	CHECK_UINT(first, 1);
	CHECK(st.stale >= 2u);
	CHECK_UINT(st.stale, st.trials);
	CHECK_UINT(st.kept, 0);
	CHECK(st.stale_gap_min >= EXPOSE_CADENCE);
	CHECK_UINT(info.diverge_resets, 0);
	expose_bounds_check(&st);
}

/*
 * A white near talker at the echo level joins the white far end of the
 * converged ringback line for 30 s. Each frame fires on the trusted estimate
 * under the classic bar, and the stale run starts the first trial. The latch
 * arm starts the others, and each of them carries the stand-down of the stale
 * run on. Each trial must be rejected, and two trials must stand 600 active
 * frames apart or more, the cadence of the latch arm alone. In the trial
 * frames the near speech must pass within 2 dB, and the output must stay
 * under the microphone plus 3 dB. The echo that the output leaves must stay
 * 3 dB or more under the echo.
 */
static void expose_steady_test(void)
{
	struct expose_stat st;
	struct ring_line line;
	unsigned f;

	memset(&st, 0, sizeof(st));
	ring_line_init(&line, UDSP_SAMPLE_RATE);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES);
	for (f = 0; f < EXPOSE_FRAMES; f++) {
		int16_t near[UDSP_FRAME_SAMPLES];

		noise_fill(near, line.frame, TALKER_ECHO_DBM0,
			   &line.near_state);
		expose_white_run(&line, near, &st);
	}
	udsp_ec_nlms.destroy(line.ec);

	CASE("lec bounds the stale-estimate trials of a steady talker between "
	     "the two bars");
	expose_print("steady talker", &st);
	CHECK(st.stale >= 1u);
	CHECK(st.trials >= 2u);
	CHECK_UINT(st.kept, 0);
	CHECK(st.gap_min >= EXPOSE_ANY_CADENCE);
	expose_bounds_check(&st);
}

#define TURNS_EXPOSE_CYCLE	500u
#define TURNS_EXPOSE_FAR	300u

/* One turn pattern of expose_turns_test: the near turn starts near_from
 * frames into the cycle. */
static void expose_turns_run(unsigned near_from, struct expose_stat *st)
{
	struct word_source talker;
	struct ring_line line;
	unsigned f;

	memset(st, 0, sizeof(*st));
	ring_line_init(&line, UDSP_SAMPLE_RATE);
	word_source_init(&talker, UDSP_SAMPLE_RATE,
			 TALKER_ECHO_DBM0 - QUIET_UNDER_DB, 0x31415926u);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES);
	for (f = 0; f < QUIET_FRAMES; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t near[UDSP_FRAME_SAMPLES];
		unsigned p = f % TURNS_EXPOSE_CYCLE;
		bool talk = p >= near_from &&
			    p < near_from + TURNS_EXPOSE_FAR;

		if (p == 0u)
			word_source_restart(&line.far);
		if (p == near_from)
			word_source_restart(&talker);
		memset(ref, 0, sizeof(ref));
		memset(near, 0, sizeof(near));
		if (p < TURNS_EXPOSE_FAR)
			word_fill(&line.far, ref, line.frame);
		if (talk)
			word_fill(&talker, near, line.frame);
		expose_frame_run(&line, ref, talk ? near : NULL, st);
	}
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * Turns of a talker between the two bars, on the converged ringback line.
 * Far-end words and near words 2 dB under the echo take turns for 60 s, in a
 * cycle of 5 s. Each talker talks for 3 s of the cycle. The near turn
 * overlaps the end of the far turn by 0.5, 1.5 or 3 s. The clear frames of
 * each far turn repay the stale run, so the turns must cost no trial.
 */
static void expose_turns_test(void)
{
	static const unsigned overlap[] = { 50u, 150u, 300u };
	unsigned i;

	CASE("lec bounds the stale-estimate trials of a talker in turns "
	     "between the two bars");
	for (i = 0; i < sizeof(overlap) / sizeof(overlap[0]); i++) {
		struct expose_stat st;
		char what[40];

		expose_turns_run(TURNS_EXPOSE_FAR - overlap[i], &st);
		snprintf(what, sizeof(what), "overlap of %u ms",
			 10u * overlap[i]);
		expose_print(what, &st);
		CHECK(st.active > 0u);
		CHECK_UINT(st.trials, 0);
	}
}

#define STOP_TALK_FRAMES	50u
#define STOP_AFTER_FRAMES	(3u * FRAMES_PER_SECOND)
#define STOP_ERLE_LOSS_DB	3.0

/*
 * A talker between the two bars who stops during the trial. The ringback
 * line converges on white noise and measures its filter ERLE over the last
 * second. A white near talker at the echo level then joins the white far end.
 * The stale run starts a trial, the talker stops 50 frames into it, and the
 * far end talks on alone for 3 s. The trial ends without the talker, so its
 * verdict can keep a filter that adapted on the talker. That costs at most
 * 3 dB of filter ERLE, and the trial frames keep the bounds of the steady
 * talker.
 */
static void expose_stop_test(void)
{
	struct ring_stat before;
	struct ring_stat after;
	struct expose_stat st;
	struct ring_line line;
	unsigned talked = 0;
	double erle_before;
	double erle_after;
	unsigned f;

	memset(&before, 0, sizeof(before));
	memset(&after, 0, sizeof(after));
	memset(&st, 0, sizeof(st));
	ring_line_init(&line, UDSP_SAMPLE_RATE);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES - FRAMES_PER_SECOND);
	for (f = 0; f < FRAMES_PER_SECOND; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];

		noise_fill(ref, line.frame, RING_WORDS_DBM0, &line.far_state);
		ring_frame_run(&line, ref, NULL, &before);
	}
	for (f = 0; f < EXPOSE_FRAMES && talked < STOP_TALK_FRAMES; f++) {
		int16_t near[UDSP_FRAME_SAMPLES];

		noise_fill(near, line.frame, TALKER_ECHO_DBM0,
			   &line.near_state);
		expose_white_run(&line, near, &st);
		if (st.trials)
			talked++;
	}
	for (f = 0; f < STOP_AFTER_FRAMES; f++) {
		int16_t ref[UDSP_FRAME_SAMPLES];

		if (f + FRAMES_PER_SECOND < STOP_AFTER_FRAMES) {
			expose_white_run(&line, NULL, &st);
			continue;
		}
		noise_fill(ref, line.frame, RING_WORDS_DBM0, &line.far_state);
		ring_frame_run(&line, ref, NULL, &after);
	}
	udsp_ec_nlms.destroy(line.ec);
	erle_before = weak_filter_erle_get(before.echo, before.residual,
					   before.floor);
	erle_after = weak_filter_erle_get(after.echo, after.residual,
					  after.floor);

	CASE("lec bounds a stale-estimate trial whose talker stops during the "
	     "trial");
	expose_print("talker who stops", &st);
	printf("# filter ERLE %.2f dB before the talker, %.2f dB in the last "
	       "second\n", erle_before, erle_after);
	CHECK_UINT(st.stale, 1);
	CHECK_UINT(st.trials, 1);
	CHECK(erle_after >= erle_before - STOP_ERLE_LOSS_DB);
	expose_bounds_check(&st);
}

#define TWIN_COMPARE_FRAMES	20u

/* One frame of white far end to two ringback lines, with near on top when it
 * is given. Both lines start from the same seeds, so they take the same
 * frames, and out of each holds its output. */
static void twin_ring_run(struct ring_line *lead, struct ring_line *twin,
			  const int16_t *near, struct ring_planes *lead_p,
			  struct ring_planes *twin_p)
{
	int16_t ref[UDSP_FRAME_MAX];

	noise_fill(ref, lead->frame, RING_WORDS_DBM0, &lead->far_state);
	noise_fill(ref, twin->frame, RING_WORDS_DBM0, &twin->far_state);
	ring_planes_run(lead, ref, near, NULL, lead_p);
	ring_planes_run(twin, ref, near, NULL, twin_p);
}

/*
 * The filter after a rejected trial of the stale run, against a frozen twin.
 * Two ringback lines take the same frames: white noise, then a white near
 * talker at the echo level until the first line starts a trial. The twin
 * freezes on that frame, so its filter holds the snapshot of the trial. The
 * talker refuses the trial, and both lines then freeze and take 20 frames of
 * the far end alone. Two frozen filters give one output on one frame only
 * when they hold the same coefficients. The outputs must be equal sample for
 * sample.
 */
static void expose_twin_test(void)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info lead_info;
	struct udsp_ec_nlms_info twin_info;
	struct ring_planes lead_p;
	struct ring_planes twin_p;
	struct ring_line lead;
	struct ring_line twin;
	unsigned started = 0;
	unsigned ended = 0;
	unsigned differ = 0;
	unsigned f;

	ring_line_init(&lead, UDSP_SAMPLE_RATE);
	ring_line_init(&twin, UDSP_SAMPLE_RATE);
	for (f = 0; f < TALKER_CONVERGE_FRAMES; f++)
		twin_ring_run(&lead, &twin, NULL, &lead_p, &twin_p);
	for (f = 0; f < EXPOSE_FRAMES && !ended; f++) {
		int16_t near[UDSP_FRAME_SAMPLES];

		noise_fill(near, lead.frame, TALKER_ECHO_DBM0,
			   &lead.near_state);
		twin_ring_run(&lead, &twin, near, &lead_p, &twin_p);
		udsp_ec_nlms_info_get(lead.ec, &lead_info);
		udsp_ec_nlms_frame_get(lead.ec, &reading);
		if (!started && lead_info.probes) {
			started = f + 1u;
			udsp_ec_nlms.freeze(twin.ec, true);
		} else if (started && !reading.probing) {
			ended = f + 1u;
		}
	}
	udsp_ec_nlms.freeze(lead.ec, true);
	for (f = 0; f < TWIN_COMPARE_FRAMES; f++) {
		twin_ring_run(&lead, &twin, NULL, &lead_p, &twin_p);
		if (memcmp(lead_p.out, twin_p.out,
			   lead.frame * sizeof(*lead_p.out)))
			differ++;
	}
	udsp_ec_nlms_info_get(lead.ec, &lead_info);
	udsp_ec_nlms_info_get(twin.ec, &twin_info);
	udsp_ec_nlms.destroy(lead.ec);
	udsp_ec_nlms.destroy(twin.ec);

	CASE("lec restores the filter exactly after a rejected stale-estimate "
	     "trial");
	printf("# trial from frame %u to frame %u, %u trials; coefficient "
	       "energy %.9f against the twin's %.9f; %u of %u frames "
	       "differ\n", started, ended, lead_info.probes,
	       (double)lead_info.coef_energy, (double)twin_info.coef_energy,
	       differ, TWIN_COMPARE_FRAMES);
	CHECK(started > 0u);
	CHECK(ended > started);
	CHECK_UINT(lead_info.probes, 1);
	CHECK(lead_info.coef_energy == twin_info.coef_energy);
	CHECK_UINT(differ, 0);
}

#define MARK_TRIALS_MAX		16u

/*
 * The trial fields of the frame reading over a run of frames. A trial starts
 * on the frame that moves the probe count. It ends on the last frame that
 * reads probing before a frame that does not. arm and end hold the fields of
 * those frames in order, up to MARK_TRIALS_MAX trials. The stray counts take
 * the other frames that read an arm or an end.
 */
struct trial_marks {
	struct udsp_ec_nlms_frame last;
	uint32_t probes;
	bool have_last;
	unsigned starts;
	unsigned ends;
	uint8_t arm[MARK_TRIALS_MAX];
	uint8_t end[MARK_TRIALS_MAX];
	unsigned start_probing;
	unsigned stray_arms;
	unsigned stray_ends;
};

/* The frame in last ends a trial when ended is set. */
static void trial_marks_end_note(struct trial_marks *m, bool ended)
{
	if (!ended) {
		if (m->last.trial_end != UDSP_EC_NLMS_TRIAL_END_NONE)
			m->stray_ends++;
		return;
	}
	if (m->ends < MARK_TRIALS_MAX)
		m->end[m->ends] = m->last.trial_end;
	m->ends++;
}

/* A fixture calls it after each frame. A NULL m takes nothing, so a fixture
 * serves a case that reads no trial field. */
static void trial_marks_add(struct trial_marks *m, const struct udsp_ec *ec)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;

	if (!m)
		return;
	udsp_ec_nlms_frame_get(ec, &reading);
	udsp_ec_nlms_info_get(ec, &info);
	if (m->have_last)
		trial_marks_end_note(m, m->last.probing && !reading.probing);
	m->last = reading;
	m->have_last = true;
	if (info.probes == m->probes) {
		if (reading.trial_arm != UDSP_EC_NLMS_TRIAL_ARM_NONE)
			m->stray_arms++;
		return;
	}
	m->probes = info.probes;
	m->start_probing += reading.probing;
	if (m->starts < MARK_TRIALS_MAX)
		m->arm[m->starts] = reading.trial_arm;
	m->starts++;
}

/* No frame follows the last frame of a run, so it counts as a frame that
 * ends no trial. Each leg therefore runs on after its last trial. */
static void trial_marks_finish(struct trial_marks *m)
{
	if (m->have_last)
		trial_marks_end_note(m, false);
	m->have_last = false;
}

static void trial_marks_print(const char *what, const struct trial_marks *m)
{
	unsigned i;

	printf("# %s: %u trials, arms", what, m->starts);
	for (i = 0; i < m->starts && i < MARK_TRIALS_MAX; i++)
		printf(" %s", lecr_trial_arm_name(m->arm[i]));
	printf("; %u ends", m->ends);
	for (i = 0; i < m->ends && i < MARK_TRIALS_MAX; i++)
		printf(" %s", lecr_trial_end_name(m->end[i]));
	printf("; %u start frames read probing, %u other frames read an arm, "
	       "%u an end\n", m->start_probing, m->stray_arms, m->stray_ends);
}

/* The checks of every leg: a start frame reads probing 0, and no other
 * frame reads an arm or an end. */
static void trial_marks_check(const struct trial_marks *m)
{
	CHECK_UINT(m->start_probing, 0);
	CHECK_UINT(m->stray_arms, 0);
	CHECK_UINT(m->stray_ends, 0);
}

#define CHANGE_FAR_DBM0		-10.0
#define CHANGE_ERL_DB		12.0
#define CHANGE_FLOOR_DBM0	-70.0
/* 4 s, so the trust count of dsp/lec.c completes before the talker. */
#define CHANGE_CONVERGE_FRAMES	(4u * FRAMES_PER_SECOND)
#define CHANGE_TALK_FRAMES	400u
#define CHANGE_FRAMES		(3u * FRAMES_PER_SECOND)
#define CHANGE_BLOCK_FRAMES	25u
#define CHANGE_BAR_DB		20.0
#define CHANGE_ECHO_DBM0	(CHANGE_FAR_DBM0 - CHANGE_ERL_DB)

/* The trials and the settle of one hybrid change: the frames after the
 * change to the end of the first block of 25 frames whose ERLE reaches
 * 20 dB. */
struct change_result {
	unsigned talk_trials;
	unsigned trials;
	unsigned settled;
	unsigned diverge_resets;
};

/* One frame of the change line, with a white near talker at talk_dbm0 when
 * talk is set. It returns the echo and the output energies. */
static void change_frame_run(struct udsp_ec *ec, struct echo_path *path,
			     uint32_t state[3], bool talk, double talk_dbm0,
			     double *echo_energy, double *res_energy)
{
	int16_t ref[UDSP_FRAME_MAX];
	int16_t echo[UDSP_FRAME_MAX];
	int16_t near[UDSP_FRAME_MAX];
	int16_t hiss[UDSP_FRAME_MAX];
	int16_t mic[UDSP_FRAME_MAX];
	unsigned i;

	noise_fill(ref, path->frame, CHANGE_FAR_DBM0, &state[0]);
	path_run(path, ref, echo);
	memset(near, 0, sizeof(near));
	if (talk)
		noise_fill(near, path->frame, talk_dbm0, &state[1]);
	noise_fill(hiss, path->frame, CHANGE_FLOOR_DBM0, &state[2]);
	for (i = 0; i < path->frame; i++)
		mic[i] = sample_clamp((double)echo[i] + near[i] + hiss[i]);
	udsp_ec_nlms.run(ec, mic, ref);
	for (i = 0; i < path->frame; i++) {
		*echo_energy += (double)echo[i] * echo[i];
		*res_energy += (double)mic[i] * mic[i];
	}
}

/*
 * A talker between the two bars during talk-over, then a hybrid change. A
 * flat 12 dB hybrid at 8 ms converges for 4 s on a white far end at
 * -10 dBm0. The near-end floor stands at -70 dBm0. A white near talker at the
 * level of the echo then joins for 400 frames. The stale run starts a trial
 * after 200 of them, and the talker refuses it. The talker stops, and the
 * hybrid changes to the late one at 28 ms. marks takes each frame when it is
 * not NULL.
 */
static void change_run(unsigned rate, struct change_result *out,
		       struct trial_marks *marks)
{
	struct udsp_ec_nlms_info info;
	struct echo_path first;
	struct echo_path second;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
	uint32_t state[3] = { 0x13572468u, 0xfeedfaceu, 0x2468acefu };
	double echo_energy = 0.0;
	double res_energy = 0.0;
	unsigned f;

	memset(out, 0, sizeof(*out));
	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&first, rate, 8, CHANGE_ERL_DB);
	path_late_init(&second, rate, CHANGE_ERL_DB);
	for (f = 0; f < CHANGE_CONVERGE_FRAMES + CHANGE_TALK_FRAMES; f++) {
		change_frame_run(ec, &first, state,
				 f >= CHANGE_CONVERGE_FRAMES, CHANGE_ECHO_DBM0,
				 &echo_energy, &res_energy);
		trial_marks_add(marks, ec);
	}
	udsp_ec_nlms_info_get(ec, &info);
	out->talk_trials = info.probes;
	path_history_carry(&second, &first);
	echo_energy = res_energy = 0.0;
	for (f = 0; f < CHANGE_FRAMES; f++) {
		change_frame_run(ec, &second, state, false, CHANGE_ECHO_DBM0,
				 &echo_energy, &res_energy);
		trial_marks_add(marks, ec);
		if ((f + 1u) % CHANGE_BLOCK_FRAMES)
			continue;
		if (!out->settled && res_energy > 0.0 &&
		    10.0 * log10(echo_energy / res_energy) >= CHANGE_BAR_DB)
			out->settled = f + 1u;
		echo_energy = res_energy = 0.0;
	}
	udsp_ec_nlms_info_get(ec, &info);
	out->trials = info.probes;
	out->diverge_resets = info.diverge_resets;
	udsp_ec_nlms.destroy(ec);
}

/* The figures of change_run on a canceller without the stale run, at both
 * rates, with the convergence of CHANGE_CONVERGE_FRAMES. */
#define CHANGE_HEAD_TRIALS	1u
#define CHANGE_HEAD_SETTLED	125u

/*
 * A talker between the two bars during talk-over, then a hybrid change. The
 * talker refuses a trial of the stale run and stops, and the flat 12 dB
 * hybrid changes to the late one. The rejected trial stands the stale run
 * down and not the path-change escape, so the escape must answer the change.
 * Without the stale run, the line runs one trial, for the change, and reaches
 * 20 dB of ERLE 125 frames after it (CHANGE_HEAD_SETTLED). The line must
 * run one trial more, settle within those 125 frames and run no reset. A
 * stale-run trial that stands the escape down keeps the change in lockout.
 */
static void change_test(unsigned rate)
{
	struct change_result r;

	change_run(rate, &r, NULL);

	case_rate(rate, "lec leaves the path-change escape free after a "
		  "rejected stale-estimate trial");
	printf("# %u trials in the talk-over, %u in all, %u resets; ERLE "
	       "%.0f dB after %u ms, %u ms before the stale run\n",
	       r.talk_trials, r.trials, r.diverge_resets, CHANGE_BAR_DB,
	       10u * r.settled, 10u * CHANGE_HEAD_SETTLED);
	CHECK_UINT(r.talk_trials, 1);
	CHECK_UINT(r.trials, CHANGE_HEAD_TRIALS + 1u);
	CHECK(r.settled > 0u);
	CHECK(r.settled <= CHANGE_HEAD_SETTLED);
	CHECK_UINT(r.diverge_resets, 0);
}

#define RETURN_PAUSE_FRAMES	50u
#define RETURN_TALK_DBM0	RING_WORDS_DBM0
#define RETURN_OVER_DB		15.0
/* LEC_GEIGEL_GAIN_HEADROOM, LEC_GEIGEL_GAIN_MIN, LEC_GEIGEL_FLOOR_HEADROOM and
 * LEC_GEIGEL_MARGIN of dsp/lec.c. */
#define RETURN_GAIN_HEADROOM	2.0
#define RETURN_GAIN_MIN		0.25
#define RETURN_FLOOR_HEADROOM	2.0
#define RETURN_MARGIN		1.41

/* The microphone of the last frame over the bar that the classic bound of
 * dsp/lec.c alone sets (echo_bound_get), in dB. */
static double return_over_db_get(const struct udsp_ec_nlms_frame *reading,
				 const struct udsp_ec_nlms_info *info)
{
	double bound = info->echo_gain * RETURN_GAIN_HEADROOM;
	double bar;

	if (bound > RETURN_GAIN_MIN)
		bound = RETURN_GAIN_MIN;
	bar = (reading->window_power * bound +
	       reading->noise_power * RETURN_FLOOR_HEADROOM) * RETURN_MARGIN;
	return 10.0 * log10(reading->mic_power / bar);
}

/* One frame of a white far end at the level of the words, with a white near
 * talker at talk_dbm0 on top, and the frame reading after it. */
static void return_white_run(struct ring_line *line, double talk_dbm0,
			     struct udsp_ec_nlms_frame *reading)
{
	int16_t ref[UDSP_FRAME_MAX];
	int16_t near[UDSP_FRAME_MAX];

	noise_fill(ref, line->frame, RING_WORDS_DBM0, &line->far_state);
	noise_fill(near, line->frame, talk_dbm0, &line->near_state);
	ring_frame_run(line, ref, near, NULL);
	udsp_ec_nlms_frame_get(line->ec, reading);
}

/*
 * A talker on the frame that the far end returns on starts no trial of the
 * stale run. On the converged ringback line, a white near talker at the echo
 * level joins the white far end until the run reaches 200. Both pause for
 * 500 ms: the hangover runs out, and the run stays at 200 or more. On the
 * first frame of the return, a talker at -20 dBm0 speaks, 15 dB or more over
 * the classic bar. The run must read 0 after that frame, and the line must
 * run no trial on it or on the next 100 active frames. A build that reads the
 * run before the clear starts a trial on that frame.
 */
static void stale_return_test(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct ring_line line;
	int16_t ref[UDSP_FRAME_MAX];
	unsigned built;
	unsigned kept;
	bool held;
	bool trusted;
	unsigned run;
	unsigned at_return;
	double over;
	unsigned f;

	memset(&reading, 0, sizeof(reading));
	ring_line_init(&line, rate);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES);
	for (f = 0; f < EXPOSE_FRAMES &&
		    reading.stale_run < TALKER_STALE_FRAMES; f++)
		return_white_run(&line, TALKER_ECHO_DBM0, &reading);
	built = reading.stale_run;
	memset(ref, 0, sizeof(ref));
	for (f = 0; f < RETURN_PAUSE_FRAMES; f++)
		ring_frame_run(&line, ref, NULL, NULL);
	udsp_ec_nlms_frame_get(line.ec, &reading);
	udsp_ec_nlms_info_get(line.ec, &info);
	kept = reading.stale_run;
	held = info.double_talk;
	return_white_run(&line, RETURN_TALK_DBM0, &reading);
	udsp_ec_nlms_info_get(line.ec, &info);
	over = return_over_db_get(&reading, &info);
	trusted = reading.trusted;
	run = reading.stale_run;
	at_return = info.probes;
	for (f = 0; f < TRIAL_BUDGET; f++)
		return_white_run(&line, RETURN_TALK_DBM0, &reading);
	udsp_ec_nlms_info_get(line.ec, &info);
	udsp_ec_nlms.destroy(line.ec);

	case_rate(rate, "lec starts no stale-estimate trial on the frame a "
		  "loud talker returns");
	printf("# stale run %u under the far end, %u after the pause, double "
	       "talk %u at its end; return frame: talker %.2f dB over the "
	       "classic bar, trusted %u, stale run %u, %u trials; %u trials "
	       "after %u more active frames\n", built, kept, held, over,
	       trusted, run, at_return, info.probes, TRIAL_BUDGET);
	CHECK(built >= TALKER_STALE_FRAMES);
	CHECK(kept >= TALKER_STALE_FRAMES);
	CHECK(!held);
	CHECK(trusted);
	CHECK(over >= RETURN_OVER_DB);
	CHECK_UINT(run, 0);
	CHECK_UINT(at_return, 0);
	CHECK_UINT(info.probes, 0);
}

/* LEC_TRIAL_STANDDOWN_FRAMES of dsp/lec.c. */
#define CARRY_COOLDOWN		600u
#define CARRY_LOUD_DBM0		(CHANGE_ECHO_DBM0 + 3.0)
#define CARRY_CLEAR_FRAMES	150u
#define CARRY_LIMIT_FRAMES	(10u * FRAMES_PER_SECOND)

/*
 * One trial of the carry case. start counts the frames to the frame the trial
 * starts on, and end the frames to the first frame after the trial. The arm
 * follows from the runs of the frame before the start. The stale run at its
 * trigger with the latch run under its own is the stale run's. Both runs under
 * their triggers is the path-change escape's. A trial counts as kept by the
 * rule of struct expose_stat.
 */
struct carry_trial {
	unsigned start;
	unsigned end;
	bool path;
	bool stale;
	bool kept;
};

/* Runs frames of the change line with a near talker at talk_dbm0 until a
 * trial starts and ends, or for limit frames. */
static void carry_trial_run(struct udsp_ec *ec, struct echo_path *path,
			    uint32_t state[3], double talk_dbm0, unsigned limit,
			    struct carry_trial *t)
{
	struct udsp_ec_nlms_frame before;
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	double echo_energy = 0.0;
	double res_energy = 0.0;
	float energy = 0.0f;
	uint32_t probes;
	unsigned f;

	memset(t, 0, sizeof(*t));
	for (f = 0; f < limit && !t->end; f++) {
		udsp_ec_nlms_info_get(ec, &info);
		udsp_ec_nlms_frame_get(ec, &before);
		probes = info.probes;
		change_frame_run(ec, path, state, true, talk_dbm0,
				 &echo_energy, &res_energy);
		udsp_ec_nlms_info_get(ec, &info);
		udsp_ec_nlms_frame_get(ec, &reading);
		if (info.probes != probes) {
			t->start = f + 1u;
			t->stale = before.stale_run >= TALKER_STALE_FRAMES &&
				   before.dt_run < TALKER_LATCH_FRAMES;
			t->path = before.stale_run < TALKER_STALE_FRAMES &&
				  before.dt_run < TALKER_LATCH_FRAMES;
			energy = info.coef_energy;
		} else if (t->start && !reading.probing) {
			t->end = f + 1u;
			t->kept = fabs((double)info.coef_energy - energy) >
				  EXPOSE_RESTORE * energy;
		}
	}
}

/*
 * A refused trial of the stale run inside the stand-down of the path-change
 * escape carries that stand-down on. On the converged change line, a talker
 * 3 dB over the echo refuses the trial of the escape after a change to the
 * late hybrid. The flat hybrid returns, and a talker at the echo level
 * refuses a trial of the stale run inside the stand-down. After 150 frames of
 * the far end alone, the late hybrid returns under the louder talker. The
 * next trial must be the escape's, more than 600 frames after the second
 * refusal. Without the carry, the escape comes back with the rest of the
 * first stand-down.
 */
static void stale_carry_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct carry_trial change;
	struct carry_trial stale;
	struct carry_trial next;
	struct echo_path first;
	struct echo_path second;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
	uint32_t state[3] = { 0x13572468u, 0xfeedfaceu, 0x2468acefu };
	double echo_energy = 0.0;
	double res_energy = 0.0;
	unsigned gap;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&first, rate, 8, CHANGE_ERL_DB);
	path_late_init(&second, rate, CHANGE_ERL_DB);
	for (f = 0; f < CHANGE_CONVERGE_FRAMES; f++)
		change_frame_run(ec, &first, state, false, CHANGE_ECHO_DBM0,
				 &echo_energy, &res_energy);
	path_history_carry(&second, &first);
	carry_trial_run(ec, &second, state, CARRY_LOUD_DBM0, CARRY_LIMIT_FRAMES,
			&change);
	path_history_carry(&first, &second);
	carry_trial_run(ec, &first, state, CHANGE_ECHO_DBM0, CARRY_LIMIT_FRAMES,
			&stale);
	for (f = 0; f < CARRY_CLEAR_FRAMES; f++)
		change_frame_run(ec, &first, state, false, CHANGE_ECHO_DBM0,
				 &echo_energy, &res_energy);
	path_history_carry(&second, &first);
	carry_trial_run(ec, &second, state, CARRY_LOUD_DBM0, CARRY_LIMIT_FRAMES,
			&next);
	udsp_ec_nlms_info_get(ec, &info);
	udsp_ec_nlms.destroy(ec);
	/* The refusal falls on the frame before end, and the frame at end and
	 * the clear frames follow it. */
	gap = 1u + CARRY_CLEAR_FRAMES + next.start;

	case_rate(rate, "lec carries the stand-down of the path-change escape "
		  "through a refused stale-estimate trial");
	printf("# escape trial %u frames after the change, kept %u; stale-run "
	       "trial %u frames after the refusal of the escape trial, stale "
	       "%u, kept %u; next trial %u frames after the refusal of the "
	       "stale-run trial, escape %u; %u trials, %u resets\n",
	       change.start, change.kept, 1u + stale.start,
	       stale.stale, stale.kept, gap, next.path, info.probes,
	       info.diverge_resets);
	CHECK(change.end > 0u);
	CHECK(change.path);
	CHECK(!change.kept);
	CHECK(stale.end > 0u);
	CHECK(stale.stale);
	CHECK(!stale.kept);
	CHECK(1u + stale.start < CARRY_COOLDOWN);
	CHECK(next.start > 0u);
	CHECK(next.path);
	CHECK(gap > CARRY_COOLDOWN);
	CHECK_UINT(info.diverge_resets, 0);
}

#define REPAY_BUILD		160u
#define REPAY_CLEAR_FRAMES	48u
#define REPAY_RESUME_FRAMES	100u
/* LEC_GEIGEL_RUN_DECAY of dsp/lec.c. */
#define REPAY_DECAY		4u

/*
 * The clear frames of an active window repay the stale run. A white talker at
 * the echo level joins the white far end of the converged ringback line until
 * the run reaches 160. The far end then talks alone for 48 frames, and each
 * clear frame after the hangover repays 4. The run must read 160 less 4 per
 * clear frame. The talker then joins again for 100 frames, and the line must
 * run no trial. A run that the clear frames do not repay reaches the trigger
 * of 200 and starts a trial.
 */
static void stale_repay_test(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct ring_line line;
	unsigned built;
	unsigned clear = 0;
	unsigned fired = 0;
	unsigned repaid;
	unsigned longest = 0;
	unsigned f;

	memset(&reading, 0, sizeof(reading));
	ring_line_init(&line, rate);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES);
	for (f = 0; f < EXPOSE_FRAMES && reading.stale_run < REPAY_BUILD; f++)
		return_white_run(&line, TALKER_ECHO_DBM0, &reading);
	built = reading.stale_run;
	for (f = 0; f < REPAY_CLEAR_FRAMES; f++) {
		ring_white_run(&line, 1u);
		udsp_ec_nlms_frame_get(line.ec, &reading);
		udsp_ec_nlms_info_get(line.ec, &info);
		fired += reading.dt_fired;
		clear += !info.double_talk;
	}
	repaid = reading.stale_run;
	for (f = 0; f < REPAY_RESUME_FRAMES; f++) {
		return_white_run(&line, TALKER_ECHO_DBM0, &reading);
		if (reading.stale_run > longest)
			longest = reading.stale_run;
	}
	udsp_ec_nlms_info_get(line.ec, &info);
	udsp_ec_nlms.destroy(line.ec);

	case_rate(rate, "lec repays the stale run on the clear frames of an "
		  "active window");
	printf("# stale run %u under the talker; far end alone: %u clear "
	       "frames, %u fired, stale run %u against %u; talker again: "
	       "longest stale run %u, %u trials\n", built, clear, fired,
	       repaid, built - REPAY_DECAY * clear, longest, info.probes);
	CHECK_UINT(built, REPAY_BUILD);
	CHECK_UINT(fired, 0);
	CHECK(clear > 0u && REPAY_DECAY * clear < built);
	CHECK_UINT(repaid, built - REPAY_DECAY * clear);
	CHECK(longest < TALKER_STALE_FRAMES);
	CHECK_UINT(info.probes, 0);
}

#define MARK_LATCH_DBM0		-19.0
#define MARK_LATCH_FRAMES	650u
#define MARK_PAUSE_PHASE	21u
#define MARK_PAUSE_FRAMES	(20u * FRAMES_PER_SECOND)

/* The step line of the talker cases: a white far end at TALKER_FAR_DBM0 on a
 * flat hybrid that steps from 12 dB to 3 dB, and a white talker 16 dB over
 * the far end on trial frame 10. */
#define TALKER_FAR_DBM0		-20.0
#define TALKER_OVER_DB		16.0
#define TALKER_TRIAL_FRAME	10u
#define TALKER_STEP_FROM_DB	12.0
#define TALKER_STEP_TO_DB	3.0

/* One frame of the step line to one instance, with the talker when talk is
 * set. */
static void talker_step_frame_run(struct udsp_ec *ec, struct echo_path *path,
				  uint32_t *state, uint32_t *near_state,
				  bool talk)
{
	int16_t ref[UDSP_FRAME_MAX];
	int16_t near[UDSP_FRAME_MAX];
	int16_t mic[UDSP_FRAME_MAX];
	unsigned i;

	noise_fill(ref, path->frame, TALKER_FAR_DBM0, state);
	path_run(path, ref, mic);
	if (talk) {
		noise_fill(near, path->frame, TALKER_FAR_DBM0 + TALKER_OVER_DB,
			   near_state);
		for (i = 0; i < path->frame; i++)
			mic[i] = sample_clamp((double)mic[i] + near[i]);
	}
	udsp_ec_nlms.run(ec, mic, ref);
}

static void trial_word_check(void)
{
	static const struct {
		unsigned value;
		const char *word;
	} arms[] = {
		{ UDSP_EC_NLMS_TRIAL_ARM_NONE, "" },
		{ UDSP_EC_NLMS_TRIAL_ARM_LATCH, "latch" },
		{ UDSP_EC_NLMS_TRIAL_ARM_PATH, "path" },
		{ UDSP_EC_NLMS_TRIAL_ARM_STALE, "stale" },
		{ UDSP_EC_NLMS_TRIAL_ARM_STALE + 1u, "?" },
	}, ends[] = {
		{ UDSP_EC_NLMS_TRIAL_END_NONE, "" },
		{ UDSP_EC_NLMS_TRIAL_END_KEPT, "kept" },
		{ UDSP_EC_NLMS_TRIAL_END_REJECTED, "rejected" },
		{ UDSP_EC_NLMS_TRIAL_END_DIVERGED, "diverged" },
		{ UDSP_EC_NLMS_TRIAL_END_TALKER, "talker" },
		{ UDSP_EC_NLMS_TRIAL_END_TALKER + 1u, "?" },
	};
	unsigned i;

	for (i = 0; i < sizeof(arms) / sizeof(arms[0]); i++)
		CHECK(!strcmp(lecr_trial_arm_name(arms[i].value),
			      arms[i].word));
	for (i = 0; i < sizeof(ends) / sizeof(ends[0]); i++)
		CHECK(!strcmp(lecr_trial_end_name(ends[i].value),
			      ends[i].word));
}

/* The reset of the instance on a frame that reads the arm and the end given.
 * The reading after the reset must read NONE in both fields. */
static void trial_reset_check(struct udsp_ec *ec, unsigned arm, unsigned end)
{
	struct udsp_ec_nlms_frame reading;

	udsp_ec_nlms_frame_get(ec, &reading);
	CHECK_UINT(reading.trial_arm, arm);
	CHECK_UINT(reading.trial_end, end);
	udsp_ec_nlms_reset(ec);
	udsp_ec_nlms_frame_get(ec, &reading);
	CHECK_UINT(reading.trial_arm, UDSP_EC_NLMS_TRIAL_ARM_NONE);
	CHECK_UINT(reading.trial_end, UDSP_EC_NLMS_TRIAL_END_NONE);
}

/*
 * The fixture of latch_refusal_test on the change line: a talker 3 dB over
 * the echo of a converged late hybrid refuses the latch trial. The instance
 * resets on the end frame, and the frame after the reset closes the trial.
 */
static void trial_latch_leg(void)
{
	struct trial_marks m = { .probes = 0 };
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state[3] = { 0x13572468u, 0xfeedfaceu, 0x2468acefu };
	double echo = 0.0;
	double res = 0.0;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_late_init(&path, UDSP_SAMPLE_RATE, CHANGE_ERL_DB);
	for (f = 0; f < CHANGE_CONVERGE_FRAMES + MARK_LATCH_FRAMES; f++) {
		change_frame_run(ec, &path, state, f >= CHANGE_CONVERGE_FRAMES,
				 MARK_LATCH_DBM0, &echo, &res);
		trial_marks_add(&m, ec);
		if (m.last.trial_end != UDSP_EC_NLMS_TRIAL_END_NONE)
			break;
	}
	trial_reset_check(ec, UDSP_EC_NLMS_TRIAL_ARM_NONE,
			  UDSP_EC_NLMS_TRIAL_END_REJECTED);
	change_frame_run(ec, &path, state, true, MARK_LATCH_DBM0, &echo, &res);
	trial_marks_add(&m, ec);
	trial_marks_finish(&m);
	udsp_ec_nlms.destroy(ec);
	trial_marks_print("latch leg", &m);
	CHECK_UINT(m.starts, 1);
	CHECK_UINT(m.arm[0], UDSP_EC_NLMS_TRIAL_ARM_LATCH);
	CHECK_UINT(m.ends, 1);
	CHECK_UINT(m.end[0], UDSP_EC_NLMS_TRIAL_END_REJECTED);
	trial_marks_check(&m);
}

/* The fixture of change_run. A talker at the echo level refuses a trial of
 * the stale run. The change to the late hybrid then starts a trial of the
 * path-change escape, and the line keeps it. */
static void trial_change_leg(void)
{
	struct trial_marks m = { .probes = 0 };
	struct change_result r;

	change_run(UDSP_SAMPLE_RATE, &r, &m);
	trial_marks_finish(&m);
	trial_marks_print("change leg", &m);
	CHECK_UINT(m.starts, 2);
	CHECK_UINT(m.arm[0], UDSP_EC_NLMS_TRIAL_ARM_STALE);
	CHECK_UINT(m.arm[1], UDSP_EC_NLMS_TRIAL_ARM_PATH);
	CHECK_UINT(m.ends, 2);
	CHECK_UINT(m.end[0], UDSP_EC_NLMS_TRIAL_END_REJECTED);
	CHECK_UINT(m.end[1], UDSP_EC_NLMS_TRIAL_END_KEPT);
	trial_marks_check(&m);
}

/*
 * The fixture of probe_pause_test: the DTX far end on the flat 0 dB hybrid,
 * from word phase MARK_PAUSE_PHASE. At that phase the latch run comes due
 * inside a far-end pause, so the trial waits for the next active frame. At
 * phase 0 the run comes due on an active frame, and no frame waits. The trial
 * frames inside the pauses do not adapt. The arm must read on the start frame
 * only, and the end on the end frame only.
 */
static void trial_pause_leg(void)
{
	struct trial_marks m = { .probes = 0 };
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(0);
	struct dtx_far far = { .state = 0x0c0ffee1u, .pos = MARK_PAUSE_PHASE };
	unsigned waits = 0;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, 0.0);
	for (f = 0; f < MARK_PAUSE_FRAMES; f++) {
		struct udsp_ec_nlms_frame before = m.last;
		int16_t ref[UDSP_FRAME_SAMPLES];
		int16_t mic[UDSP_FRAME_SAMPLES];

		dtx_far_fill(&far, ref);
		path_run(&path, ref, mic);
		udsp_ec_nlms.run(ec, mic, ref);
		trial_marks_add(&m, ec);
		if (!frame_loud(ref, UDSP_FRAME_SAMPLES) && !m.last.probing &&
		    before.dt_run >= TALKER_LATCH_FRAMES)
			waits++;
	}
	trial_marks_finish(&m);
	udsp_ec_nlms.destroy(ec);
	trial_marks_print("pause leg", &m);
	printf("# pause leg: the latch was due on %u frames of a far-end "
	       "pause\n", waits);
	CHECK_UINT(m.starts, 1);
	CHECK(waits > 0u);
	CHECK_UINT(m.arm[0], UDSP_EC_NLMS_TRIAL_ARM_LATCH);
	CHECK_UINT(m.ends, 1);
	CHECK_UINT(m.end[0], UDSP_EC_NLMS_TRIAL_END_KEPT);
	trial_marks_check(&m);
}

/*
 * The fixture of the first run of trial_talker_test: a talker 16 dB over the
 * far end ends a latch trial on trial frame 10. The latch then starts the
 * next trial, and the instance resets on its start frame, so that trial has
 * no end frame.
 */
static void trial_talker_leg(void)
{
	struct trial_marks m = { .probes = 0 };
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create(48);
	uint32_t state = 0x13572468u;
	uint32_t near_state = 0xfeedfaceu;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, TALKER_STEP_FROM_DB);
	for (f = 0; f < FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2; f++) {
		talker_step_frame_run(ec, &path, &state, &near_state, false);
		trial_marks_add(&m, ec);
	}
	path_erl_set(&path, TALKER_STEP_TO_DB);
	for (f = 0; f < SNAP_LIMIT_FRAMES && !m.starts; f++) {
		talker_step_frame_run(ec, &path, &state, &near_state, false);
		trial_marks_add(&m, ec);
	}
	for (f = 1; f <= TALKER_TRIAL_FRAME; f++) {
		talker_step_frame_run(ec, &path, &state, &near_state,
				      f == TALKER_TRIAL_FRAME);
		trial_marks_add(&m, ec);
	}
	for (f = 0; f < SNAP_LIMIT_FRAMES && m.starts < 2u; f++) {
		talker_step_frame_run(ec, &path, &state, &near_state, false);
		trial_marks_add(&m, ec);
	}
	trial_reset_check(ec, UDSP_EC_NLMS_TRIAL_ARM_LATCH,
			  UDSP_EC_NLMS_TRIAL_END_NONE);
	talker_step_frame_run(ec, &path, &state, &near_state, false);
	trial_marks_add(&m, ec);
	trial_marks_finish(&m);
	udsp_ec_nlms.destroy(ec);
	trial_marks_print("talker leg", &m);
	CHECK_UINT(m.starts, 2);
	CHECK_UINT(m.arm[0], UDSP_EC_NLMS_TRIAL_ARM_LATCH);
	CHECK_UINT(m.arm[1], UDSP_EC_NLMS_TRIAL_ARM_LATCH);
	CHECK_UINT(m.ends, 1);
	CHECK_UINT(m.end[0], UDSP_EC_NLMS_TRIAL_END_TALKER);
	trial_marks_check(&m);
}

/* The fixture of probe_divergence_test: the hybrid that amplifies drives
 * latch trials through the watchdog bound. */
static void trial_diverge_leg(void)
{
	struct udsp_ec_nlms_info info;
	struct trial_marks m = { .probes = 0 };
	struct divergence_line d;
	unsigned latch = 0;
	unsigned diverged = 0;
	unsigned i;
	unsigned f;

	divergence_line_init(&d, UDSP_SAMPLE_RATE);
	for (f = 0; f < DIVERGE_WORDS_FRAMES + DIVERGE_AMP_FRAMES; f++) {
		divergence_frame_run(&d, f);
		trial_marks_add(&m, d.ec);
	}
	trial_marks_finish(&m);
	udsp_ec_nlms_info_get(d.ec, &info);
	udsp_ec_nlms.destroy(d.ec);
	for (i = 0; i < m.starts && i < MARK_TRIALS_MAX; i++)
		latch += m.arm[i] == UDSP_EC_NLMS_TRIAL_ARM_LATCH;
	for (i = 0; i < m.ends && i < MARK_TRIALS_MAX; i++)
		diverged += m.end[i] == UDSP_EC_NLMS_TRIAL_END_DIVERGED;
	trial_marks_print("divergence leg", &m);
	CHECK(m.starts <= MARK_TRIALS_MAX);
	CHECK_UINT(m.starts, info.probes);
	CHECK_UINT(latch, m.starts);
	CHECK(info.diverge_resets >= 1u);
	CHECK_UINT(diverged, info.diverge_resets);
	trial_marks_check(&m);
}

/*
 * lec_replay prints the trial fields of the frame reading for each frame. A
 * start frame must read its arm and probing 0, an end frame its end and
 * probing 1, and every other frame NONE. A reset on a start frame or an end
 * frame clears both fields. At 8 kHz, talkers refuse a latch trial and a
 * stale-run trial, and the line keeps a path-change trial. A latch comes due
 * in a far-end pause, a talker ends a latch trial, and the watchdog stops
 * latch trials on the hybrid that amplifies. The words of lecr_trial_arm_name
 * and lecr_trial_end_name must match the enum values.
 */
static void trial_mark_test(void)
{
	CASE("lec frame reading names the arm and the end of each trial");
	trial_word_check();
	trial_latch_leg();
	trial_change_leg();
	trial_pause_leg();
	trial_talker_leg();
	trial_diverge_leg();
}

/* LEC_TALKER_RATIO of dsp/lec.c. */
#define TALKER_RATIO		16.0f
#define DENSE_PAUSE_FRAMES	15u

/*
 * A dense talker: a word source that starts its next word 15 frames after
 * each word ends (word_source_restart). The pauses are shorter than the long
 * hangover of 24 frames, so one verdict bridges a pause. The restart comes
 * before the shortest pause of the word source, so every word has the length
 * and the color of the first.
 */
struct dense_talker {
	struct word_source words;
	unsigned silent;
};

static void dense_talker_init(struct dense_talker *t, unsigned rate,
			      double level_dbm0, uint32_t seed)
{
	word_source_init(&t->words, rate, level_dbm0, seed);
	t->silent = 0;
}

static void dense_talker_fill(struct dense_talker *t, int16_t *dst,
			      unsigned frame)
{
	if (!t->words.talking && t->silent == DENSE_PAUSE_FRAMES)
		word_source_restart(&t->words);
	word_fill(&t->words, dst, frame);
	if (t->words.talking || t->words.offset)
		t->silent = 0;
	else
		t->silent++;
}

#define LATCH_FAN_DBM0		-26.0
#define LATCH_NEAR_DBM0		-10.0
#define LATCH_TRAIN_FRAMES	(3u * FRAMES_PER_SECOND)
#define LATCH_TALK_FRAMES	(30u * FRAMES_PER_SECOND)

/*
 * A near talker over a far end that never pauses: a fan at -26 dBm0 on the
 * 24 dB hybrid of the fan line. The filter trains on the fan alone for 3 s,
 * and a dense talker at -10 dBm0 then talks for 30 s. More than half of the
 * frames of each word stand more than 12 dB over the fan. The talker guard
 * of dsp/lec.c therefore clears the latch run on each word. The line must run
 * no trial, no reset and no exposure frame, and the near speech must pass
 * within 2 dB. A canceller without the guard runs 5 trials at each rate.
 */
static void latch_talker_test(unsigned rate)
{
	int16_t near[UDSP_FRAME_MAX];
	struct udsp_ec_nlms_info info;
	struct dense_talker talker;
	struct fan_line line;
	const struct near_stat *sp = &line.speech;
	unsigned f;

	fan_line_init(&line, rate, 0.0);
	fan_line_set(&line, LATCH_FAN_DBM0, FAN_ERL_DB);
	dense_talker_init(&talker, rate, LATCH_NEAR_DBM0, 0x0badcafeu);
	for (f = 0; f < LATCH_TRAIN_FRAMES; f++)
		info = fan_frame_run(&line, true, false, NULL);
	for (f = 0; f < LATCH_TALK_FRAMES; f++) {
		dense_talker_fill(&talker, near, line.frame);
		info = fan_frame_run(&line, true, false, near);
	}
	udsp_ec_nlms.destroy(line.ec);

	case_rate(rate, "lec keeps a near talker over a far-end fan out of "
		  "the latch");
	printf("# fan %.0f dBm0 on a %.0f dB hybrid, talker %.0f dBm0: %u "
	       "trials, %u resets; near speech %.2f dB through on %u frames, "
	       "output %.2f dB against the microphone, %u exposure frames\n",
	       line.fan_dbm0, line.erl_db, LATCH_NEAR_DBM0, info.probes,
	       info.diverge_resets, near_gain_db_get(sp->product, sp->near),
	       sp->frames, expose_db_get(sp->out, sp->mic), sp->exposure);
	CHECK_UINT(info.probes, 0);
	CHECK_UINT(info.diverge_resets, 0);
	CHECK(sp->frames > 0u);
	CHECK(fabs(near_gain_db_get(sp->product, sp->near)) <= EXPOSE_NEAR_DB);
	CHECK_UINT(sp->exposure, 0);
}

/*
 * The first run of trial_talker_test. A flat 12 dB hybrid converges for
 * 1.5 s on a white far end at -20 dBm0 and steps to 3 dB. The detector holds
 * the louder echo, and the latch starts a trial on a held frame. The twin
 * freezes after that frame, so its filter is the snapshot. On trial frame 10
 * a white talker 16 dB over the far end joins.
 */
static void trial_talker_step_check(void)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info before;
	struct udsp_ec_nlms_info info;
	struct udsp_ec_nlms_info twin_info;
	struct echo_path path;
	struct udsp_ec *lead = udsp_ec_nlms.create(48);
	struct udsp_ec *twin = udsp_ec_nlms.create(48);
	uint32_t state = 0x13572468u;
	uint32_t near_state = 0xfeedfaceu;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t near[UDSP_FRAME_SAMPLES];
	int16_t lead_out[UDSP_FRAME_SAMPLES];
	int16_t twin_out[UDSP_FRAME_SAMPLES];
	unsigned started = 0;
	unsigned f;

	memset(&info, 0, sizeof(info));
	udsp_ec_nlms.nlp(lead, false);
	udsp_ec_nlms.nlp(twin, false);
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, TALKER_STEP_FROM_DB);
	for (f = 0; f < FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2; f++) {
		noise_fill(ref, UDSP_FRAME_SAMPLES, TALKER_FAR_DBM0, &state);
		twin_step(lead, twin, &path, ref, NULL, lead_out, twin_out,
			  NULL);
	}
	path_erl_set(&path, TALKER_STEP_TO_DB);
	for (f = 0; f < SNAP_LIMIT_FRAMES && !started; f++) {
		noise_fill(ref, UDSP_FRAME_SAMPLES, TALKER_FAR_DBM0, &state);
		twin_step(lead, twin, &path, ref, NULL, lead_out, twin_out,
			  NULL);
		udsp_ec_nlms_info_get(lead, &info);
		if (info.probes)
			started = f + 1u;
	}
	udsp_ec_nlms.freeze(twin, true);
	for (f = 1; f < TALKER_TRIAL_FRAME; f++) {
		noise_fill(ref, UDSP_FRAME_SAMPLES, TALKER_FAR_DBM0, &state);
		twin_step(lead, twin, &path, ref, NULL, lead_out, twin_out,
			  NULL);
	}
	udsp_ec_nlms_info_get(lead, &before);
	noise_fill(ref, UDSP_FRAME_SAMPLES, TALKER_FAR_DBM0, &state);
	noise_fill(near, UDSP_FRAME_SAMPLES, TALKER_FAR_DBM0 + TALKER_OVER_DB,
		   &near_state);
	twin_step(lead, twin, &path, ref, near, lead_out, twin_out, NULL);
	udsp_ec_nlms_frame_get(lead, &reading);
	udsp_ec_nlms_info_get(lead, &info);
	udsp_ec_nlms_info_get(twin, &twin_info);
	CHECK(started > 0u);
	CHECK(reading.talker);
	CHECK(reading.probing);
	CHECK_UINT(reading.trial_end, UDSP_EC_NLMS_TRIAL_END_TALKER);
	CHECK_UINT(info.hold_frames - before.hold_frames, 1);
	CHECK(info.coef_energy == twin_info.coef_energy);
	printf("# step trial from frame %u after the step; talker frame: "
	       "mic %.2f dB over the window, held %u; coefficient energy %.9f "
	       "against the twin's %.9f\n", started,
	       10.0 * log10((double)reading.mic_power / reading.window_power),
	       info.hold_frames - before.hold_frames,
	       (double)info.coef_energy, (double)twin_info.coef_energy);
	noise_fill(ref, UDSP_FRAME_SAMPLES, TALKER_FAR_DBM0, &state);
	twin_step(lead, twin, &path, ref, NULL, lead_out, twin_out, NULL);
	udsp_ec_nlms_frame_get(lead, &reading);
	udsp_ec_nlms_info_get(lead, &info);
	CHECK(!reading.probing);
	CHECK_UINT(info.probes, before.probes);
	CHECK_UINT(info.diverge_resets, before.diverge_resets);
	udsp_ec_nlms.destroy(lead);
	udsp_ec_nlms.destroy(twin);
}

/*
 * The second run of trial_talker_test. The snapshot line at -20 dBm0 comes
 * to a due latch, and the twin freezes. The latch starts a trial on the next
 * frame, after the hangover ran out, so the snapshot holds no double talk.
 * The talker 3 dB over the echo talks on, and on trial frame 10 a white
 * talker 16 dB over the far end replaces it. The output of that frame must
 * equal the output of the twin, sample for sample.
 */
static void trial_talker_snap_check(void)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct snap_line s;
	unsigned started;
	unsigned f;

	snap_line_build(&s, TALKER_FAR_DBM0);
	udsp_ec_nlms.freeze(s.twin, true);
	for (f = 0; f < TALKER_TRIAL_FRAME; f++)
		snap_frame_run(&s, snap_talk_get(&s, TALKER_FAR_DBM0 -
						 SNAP_TALK_UNDER_DB));
	udsp_ec_nlms_info_get(s.lead, &info);
	started = info.probes;
	snap_frame_run(&s, snap_talk_get(&s, TALKER_FAR_DBM0 + TALKER_OVER_DB));
	udsp_ec_nlms_frame_get(s.lead, &reading);
	udsp_ec_nlms_info_get(s.lead, &info);
	printf("# snapshot trial: latch run %u after the pause; talker frame: "
	       "end %s, output %s the twin's\n", s.run_before,
	       lecr_trial_end_name(reading.trial_end),
	       memcmp(s.lead_out, s.twin_out, sizeof(s.lead_out)) ?
	       "differs from" : "equals");
	CHECK(s.clear_before);
	CHECK(s.run_before >= SNAP_LATCH_FRAMES);
	CHECK_UINT(started, 1);
	CHECK(reading.talker);
	CHECK_UINT(reading.trial_end, UDSP_EC_NLMS_TRIAL_END_TALKER);
	CHECK(!memcmp(s.lead_out, s.twin_out, sizeof(s.lead_out)));
	CHECK_UINT(info.probes, 1);
	CHECK_UINT(info.diverge_resets, 0);
	udsp_ec_nlms.destroy(s.lead);
	udsp_ec_nlms.destroy(s.twin);
}

/*
 * A talker 16 dB over the far end on frame 10 of a running trial. The frame
 * must read the talker, read probing, read the end talker and count one held
 * frame. The frame after it must read no trial, and the line must count no
 * new trial and no reset. The coefficient energy must equal that of a twin
 * frozen on the frame the trial started. A second run starts the trial after
 * the hangover ran out, so the snapshot holds no double talk. The output of
 * the frame that ends the trial must equal the output of the frozen twin.
 */
static void trial_talker_test(void)
{
	CASE("lec ends a trial on the first frame of a near talker");
	trial_talker_step_check();
	trial_talker_snap_check();
}

/* The far end after the pause of latch_return_test, under its level before
 * the pause. */
#define RETURN_QUIET_DB		8.0

/*
 * The latch comes due on a talker frame: the snapshot line at -20 dBm0 comes
 * to a due latch over a far-end pause. The far end returns 8 dB quieter, with
 * a white talker 16 dB over it. The talker clears the latch run before the
 * arms read it, so the line must start no trial then or in the next 100
 * frames. A build that clears the run after the arms read it starts a trial
 * on the return frame. The guard window must follow the silent frames of the
 * pause. A guard window that keeps the louder far end reads the talker only
 * 8 dB over it, and the latch starts a trial.
 */
static void latch_return_test(void)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct snap_line s;
	unsigned at_return;
	unsigned f;

	snap_line_build(&s, TALKER_FAR_DBM0);
	s.far_dbm0 = TALKER_FAR_DBM0 - RETURN_QUIET_DB;
	snap_frame_run(&s, snap_talk_get(&s, s.far_dbm0 + TALKER_OVER_DB));
	udsp_ec_nlms_frame_get(s.lead, &reading);
	udsp_ec_nlms_info_get(s.lead, &info);
	at_return = info.probes;
	for (f = 0; f < TRIAL_BUDGET; f++)
		snap_frame_run(&s, snap_talk_get(&s, s.far_dbm0 -
						 SNAP_TALK_UNDER_DB));
	udsp_ec_nlms_info_get(s.lead, &info);
	udsp_ec_nlms.destroy(s.lead);
	udsp_ec_nlms.destroy(s.twin);

	CASE("lec starts no latch trial on the frame a loud talker returns");
	printf("# latch run %u after the pause, double talk %u at its end; "
	       "return frame %.0f dB quieter: talker %u, mic %.2f dB over the "
	       "window, %u trials; %u trials after %u more frames\n",
	       s.run_before, !s.clear_before, RETURN_QUIET_DB, reading.talker,
	       10.0 * log10((double)reading.mic_power / reading.window_power),
	       at_return, info.probes, TRIAL_BUDGET);
	CHECK(s.clear_before);
	CHECK(s.run_before >= SNAP_LATCH_FRAMES);
	CHECK(reading.talker);
	CHECK_UINT(at_return, 0);
	CHECK_UINT(info.probes, 0);
}

#define TAIL_ERL_DB		6.0
#define TAIL_WORDS_DBM0		-20.0
#define TAIL_NOISE_DBM0		-48.0
#define TAIL_FRAMES		(10u * FRAMES_PER_SECOND)
#define TAIL_FEED_FRAMES	8u

/* One frame of the tail line: far-end words on a white noise at
 * TAIL_NOISE_DBM0, through the hybrid, with the near end silent. */
static void tail_frame_run(struct udsp_ec *ec, struct echo_path *path,
			   struct word_source *far, uint32_t *state,
			   struct udsp_ec_nlms_frame *reading)
{
	int16_t ref[UDSP_FRAME_MAX];
	int16_t words[UDSP_FRAME_MAX];
	int16_t mic[UDSP_FRAME_MAX];
	unsigned i;

	noise_fill(ref, path->frame, TAIL_NOISE_DBM0, state);
	word_fill(far, words, path->frame);
	for (i = 0; i < path->frame; i++)
		ref[i] = sample_clamp((double)ref[i] + words[i]);
	path_run(path, ref, mic);
	udsp_ec_nlms.run(ec, mic, ref);
	udsp_ec_nlms_frame_get(ec, reading);
}

/* Runs the tail line to a frame inside the body of a word. */
static void tail_word_run(struct udsp_ec *ec, struct echo_path *path,
			  struct word_source *far, uint32_t *state)
{
	struct udsp_ec_nlms_frame reading;

	do {
		tail_frame_run(ec, path, far, state, &reading);
	} while (!far->talking || far->pos <= WORD_ONSET_FRAMES);
}

/* One frame of the noise alone, and a microphone of a white talker 16 dB
 * over it and nothing else. It returns the talker verdict of the frame. */
static bool tail_talker_get(struct udsp_ec *ec, unsigned frame,
			    uint32_t *state)
{
	struct udsp_ec_nlms_frame reading;
	int16_t ref[UDSP_FRAME_MAX];
	int16_t mic[UDSP_FRAME_MAX];

	noise_fill(ref, frame, TAIL_NOISE_DBM0, state);
	noise_fill(mic, frame, TAIL_NOISE_DBM0 + TALKER_OVER_DB, state);
	udsp_ec_nlms.run(ec, mic, ref);
	udsp_ec_nlms_frame_get(ec, &reading);
	return reading.talker;
}

/*
 * A late echo is not a talker: a flat 6 dB hybrid puts its delay 1 ms short
 * of the default tail. Far-end words at -20 dBm0 stop into a noise at
 * -48 dBm0 that keeps the window active, and the near end is silent. The echo
 * of each word end outlasts the window of the frame, and the frame before
 * covers it: every frame must read no talker. The guard window must start
 * empty after a reset and must follow the reference feed. The line resets
 * inside a word, and later feeds 8 noise frames after a word. Each time, the
 * next frame carries a white talker 16 dB over the noise, and it must read
 * the talker.
 */
static void talker_tail_test(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct word_source far;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(0, rate);
	uint32_t state = 0x2718281du;
	int16_t ref[UDSP_FRAME_MAX];
	unsigned talker = 0;
	unsigned window = 0;
	bool after_reset;
	bool after_feed;
	unsigned f;

	udsp_ec_nlms.nlp(ec, false);
	udsp_ec_nlms_info_get(ec, &info);
	path_flat_init(&path, rate, info.tail_ms - 1u, TAIL_ERL_DB);
	word_source_init(&far, rate, TAIL_WORDS_DBM0, 0x13572468u);
	for (f = 0; f < TAIL_FRAMES; f++) {
		tail_frame_run(ec, &path, &far, &state, &reading);
		talker += reading.talker;
		if (reading.window_run &&
		    reading.mic_power > TALKER_RATIO * reading.window_power)
			window++;
	}
	tail_word_run(ec, &path, &far, &state);
	udsp_ec_nlms_reset(ec);
	after_reset = tail_talker_get(ec, path.frame, &state);
	tail_word_run(ec, &path, &far, &state);
	for (f = 0; f < TAIL_FEED_FRAMES; f++) {
		noise_fill(ref, path.frame, TAIL_NOISE_DBM0, &state);
		udsp_ec_nlms_ref_feed(ec, ref);
	}
	after_feed = tail_talker_get(ec, path.frame, &state);
	udsp_ec_nlms.destroy(ec);

	case_rate(rate, "lec reads no talker in the late echo of the tail");
	printf("# %u ms hybrid delay: %u of %u frames read a talker, %u read "
	       "one by the window of the frame alone; talker after a reset "
	       "%u, after a feed %u\n", info.tail_ms - 1u, talker, TAIL_FRAMES,
	       window, after_reset, after_feed);
	CHECK(window > 0u);
	CHECK_UINT(talker, 0);
	CHECK(after_reset);
	CHECK(after_feed);
}

/* LEC_REF_ACTIVE_POWER and LEC_REF_FLOOR_MARGIN of dsp/lec.c, and the mean
 * square of the identity, the fixed bar over the margin. */
#define REF_BAR_FIXED		2500.0f
#define REF_FLOOR_MARGIN	6.3
#define REF_FLOOR_IDENTITY	396.8f

/*
 * The cap of the activity bar by the formula of dsp/lec.c, from the learned
 * gain that the introspection read after the frame before, in the float
 * arithmetic of ref_bar_max_get: LEC_GEIGEL_GAIN_HEADROOM, LEC_GEIGEL_GAIN_MIN
 * and LEC_REF_RETURN_MAX.
 */
static float cap_bar_max_get(float gain)
{
	float bound = gain * 2.0f;
	float bar_max;

	if (bound > 0.25f)
		bound = 0.25f;
	bar_max = 82.4f / bound;
	return bar_max > REF_BAR_FIXED ? bar_max : REF_BAR_FIXED;
}

/*
 * The frames of one line of the far-floor cases: those whose bar stands over
 * the cap, those whose cap is not the formula of the gain before the frame,
 * those whose bar is not the fixed bar and those whose gain rose. restores
 * counts the trials whose end frame, a rejection or a talker end, left
 * another gain than the frame started with. On such a frame the cap must
 * still read the gain before the frame.
 */
struct cap_stat {
	unsigned frames;
	unsigned over;
	unsigned wrong;
	unsigned raised;
	unsigned rises;
	unsigned restores;
};

/* A fixture calls it after each frame, with the gain before the frame. */
static void cap_check(struct cap_stat *st, const struct udsp_ec *ec,
		      float gain)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	bool restore;

	udsp_ec_nlms_frame_get(ec, &reading);
	udsp_ec_nlms_info_get(ec, &info);
	restore = reading.trial_end == UDSP_EC_NLMS_TRIAL_END_REJECTED ||
		  reading.trial_end == UDSP_EC_NLMS_TRIAL_END_TALKER;
	st->frames++;
	st->over += reading.ref_bar > reading.ref_bar_max;
	st->wrong += reading.ref_bar_max != cap_bar_max_get(gain);
	st->raised += reading.ref_bar != REF_BAR_FIXED;
	st->rises += info.echo_gain > gain;
	st->restores += restore && info.echo_gain != gain;
}

static void cap_print(const char *what, const struct cap_stat *st)
{
	printf("# %s: %u frames, %u bars over the cap, %u caps off the "
	       "formula, %u raised bars, %u frames with a rising gain, %u "
	       "trial ends that restored another gain\n", what, st->frames,
	       st->over, st->wrong, st->raised, st->rises, st->restores);
}

/* The bounds of the cap that every far-floor line keeps. */
static void cap_bounds_check(const struct cap_stat *st)
{
	CHECK(st->frames > 0u);
	CHECK_UINT(st->over, 0);
	CHECK_UINT(st->wrong, 0);
}

/* One frame of the fan line, and the cap check of the frame. */
static struct udsp_ec_nlms_info fan_cap_frame_run(struct fan_line *line,
						  bool fan, bool words,
						  const int16_t *near,
						  struct cap_stat *cap)
{
	struct udsp_ec_nlms_info info;
	float gain;

	udsp_ec_nlms_info_get(line->ec, &info);
	gain = info.echo_gain;
	info = fan_frame_run(line, fan, words, near);
	cap_check(cap, line->ec, gain);
	return info;
}

/*
 * Far words in phrases of 1.2 s in each cycle of 1.8 s, so a pause of 600 ms
 * follows each phrase. A line that decays the latch run on the frames of a
 * pause loses more of the run in the pause than a phrase adds to it. The
 * native word source completes the run of far_floor_latch_test 12.2 to 12.4 s
 * after the step, too near the bound of 12 s that the case needs. The longer
 * pauses are the harder test of the rule, and a build without the rule runs
 * no trial in 20 s on them.
 */
#define PHRASE_FRAMES		120u
#define PHRASE_CYCLE_FRAMES	180u

/* index counts the frames of the schedule from 0, and silent the frames since
 * the last far word. */
struct phrase_words {
	unsigned index;
	unsigned silent;
};

/*
 * One frame of the fan line with the far words of the phrase schedule. Inside
 * a phrase the next word starts 15 frames after a word ends
 * (DENSE_PAUSE_FRAMES), the restart of the dense talker.
 */
static struct udsp_ec_nlms_info phrase_frame_run(struct fan_line *line,
						 struct phrase_words *pw,
						 const int16_t *near,
						 struct cap_stat *cap)
{
	struct udsp_ec_nlms_info info;
	unsigned p = pw->index++ % PHRASE_CYCLE_FRAMES;
	bool words = p < PHRASE_FRAMES;

	if (p == 0u || (words && !line->far.talking &&
			pw->silent == DENSE_PAUSE_FRAMES))
		word_source_restart(&line->far);
	info = fan_cap_frame_run(line, true, words, near, cap);
	pw->silent = line->word_loud ? 0u : pw->silent + 1u;
	return info;
}

/*
 * The line of far_floor_talker_test, the fan line with a flat 30 dB hybrid.
 * The hybrid holds the learned gain at its floor, so the cap of the bar
 * stands at -38 dBm0. After 2 s of silence a fan at -45 dBm0 runs for 60 s,
 * with far words in its first 5 s. A dense talker at -42 dBm0 speaks from
 * 15 s to 55 s. Its word peaks stand 9.5 dB over the fan, under the talker
 * guard of dsp/lec.c.
 */
#define FLOOR_FAN_DBM0		-45.0
#define FLOOR_ERL_DB		30.0
#define FLOOR_TALK_DBM0		-42.0
#define FLOOR_SILENT_FRAMES	(2u * FRAMES_PER_SECOND)
#define FLOOR_WORDS_TO		(FLOOR_SILENT_FRAMES + 5u * FRAMES_PER_SECOND)
#define FLOOR_TALK_FROM		(15u * FRAMES_PER_SECOND)
#define FLOOR_TALK_TO		(55u * FRAMES_PER_SECOND)
#define FLOOR_FRAMES		(FLOOR_SILENT_FRAMES + 60u * FRAMES_PER_SECOND)
#define FLOOR_QUIET_SHARE	0.90

/*
 * One leg of far_floor_talker_test. from and to hold the state at the start
 * and at the end of the talk, and end the state at the end of the line. The
 * counts cover the fan frames after 15 s. With words set, far words in the
 * phrases of phrase_frame_run go on through the talk. A talker under the guard
 * who talks over far words then forms double-talk verdicts on the far-word
 * frames, and the latch run holds through the background pauses. That leg can
 * therefore still latch, and its trials are reported with no bar. This build
 * runs 1 latch trial at 8 kHz and 2 at 16 kHz, each rejected, with 688 and 742
 * exposure frames. The build that decays the run on background frames runs no
 * trial, with 656 and 693 exposure frames. Both pass the near speech within
 * 0.01 dB with no reset.
 */
struct floor_talk {
	struct udsp_ec_nlms_info from;
	struct udsp_ec_nlms_info to;
	struct udsp_ec_nlms_info end;
	struct near_stat speech;
	struct trial_marks marks;
	struct cap_stat cap;
	unsigned fan_frames;
	unsigned quiet;
	unsigned background;
};

static void floor_talk_run(unsigned rate, bool words, struct floor_talk *t)
{
	int16_t near[UDSP_FRAME_MAX];
	struct udsp_ec_nlms_frame reading;
	struct phrase_words pw = { .index = 0 };
	struct dense_talker talker;
	struct fan_line line;
	unsigned f;

	memset(t, 0, sizeof(*t));
	fan_line_init(&line, rate, 0.0);
	fan_line_set(&line, FLOOR_FAN_DBM0, FLOOR_ERL_DB);
	dense_talker_init(&talker, rate, FLOOR_TALK_DBM0, 0x0badcafeu);
	for (f = 0; f < FLOOR_FRAMES; f++) {
		bool fan = f >= FLOOR_SILENT_FRAMES;
		bool talk = f >= FLOOR_TALK_FROM && f < FLOOR_TALK_TO;

		if (talk)
			dense_talker_fill(&talker, near, line.frame);
		if (words && talk)
			t->end = phrase_frame_run(&line, &pw, near, &t->cap);
		else
			t->end = fan_cap_frame_run(&line, fan,
						   fan && f < FLOOR_WORDS_TO,
						   talk ? near : NULL, &t->cap);
		trial_marks_add(&t->marks, line.ec);
		if (f + 1u == FLOOR_TALK_FROM)
			t->from = t->end;
		if (f + 1u == FLOOR_TALK_TO)
			t->to = t->end;
		if (f < FLOOR_TALK_FROM)
			continue;
		udsp_ec_nlms_frame_get(line.ec, &reading);
		t->fan_frames++;
		t->quiet += !reading.window_run;
		t->background += reading.background;
	}
	trial_marks_finish(&t->marks);
	t->speech = line.speech;
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * A dense near talker under a stationary far end that the far-end floor
 * reads as quiet. While the talker speaks the line must run no trial and no
 * reset. 90 percent or more of the fan frames after 15 s must read a quiet
 * window, and each must read a background. The floor must end within 1.5 dB
 * of -72 dBm0, and the near speech must pass within 2 dB with no exposure
 * frame. A canceller without the far-end floor runs 6 trials at each rate
 * and reads no quiet window. A second leg keeps far words through the talk,
 * and it must run no reset and pass the speech within 2 dB.
 */
static void far_floor_talker_test(unsigned rate)
{
	double floor = floor_dbov_get(FAN_FLOOR_DBM0);
	const struct near_stat *sp;
	const struct near_stat *wp;
	struct floor_talk alone;
	struct floor_talk over;

	floor_talk_run(rate, false, &alone);
	floor_talk_run(rate, true, &over);
	sp = &alone.speech;
	wp = &over.speech;

	case_rate(rate, "lec keeps a near talker under a stationary far end "
		  "out of double talk");
	printf("# fan %.0f dBm0 on a %.0f dB hybrid, talker %.0f dBm0: %u "
	       "trials and %u resets while the talker speaks, %u and %u in the "
	       "run; %u of %u fan frames after 15 s in a quiet window; floor "
	       "%.2f dBov against %.2f dBov; near speech %.2f dB through on %u "
	       "frames, output %.2f dB against the microphone, %u exposure "
	       "frames\n", FLOOR_FAN_DBM0, FLOOR_ERL_DB, FLOOR_TALK_DBM0,
	       alone.to.probes - alone.from.probes,
	       alone.to.diverge_resets - alone.from.diverge_resets,
	       alone.end.probes, alone.end.diverge_resets, alone.quiet,
	       alone.fan_frames, (double)alone.end.noise_dbov, floor,
	       near_gain_db_get(sp->product, sp->near), sp->frames,
	       expose_db_get(sp->out, sp->mic), sp->exposure);
	printf("# %u of %u fan frames after 15 s read a background\n",
	       alone.background, alone.fan_frames);
	cap_print("talker alone", &alone.cap);
	trial_marks_print("talker over far words", &over.marks);
	printf("# talker over far words: %u resets; near speech %.2f dB "
	       "through on %u frames, output %.2f dB against the microphone, "
	       "%u exposure frames\n", over.end.diverge_resets,
	       near_gain_db_get(wp->product, wp->near), wp->frames,
	       expose_db_get(wp->out, wp->mic), wp->exposure);
	cap_print("talker over far words", &over.cap);
	CHECK_UINT(alone.to.probes - alone.from.probes, 0);
	CHECK_UINT(alone.to.diverge_resets - alone.from.diverge_resets, 0);
	CHECK(alone.quiet >= FLOOR_QUIET_SHARE * alone.fan_frames);
	CHECK_UINT(alone.background, alone.fan_frames);
	CHECK(fabs(alone.end.noise_dbov - floor) <= FAN_FLOOR_TOL_DB);
	CHECK(sp->frames > 0u);
	CHECK(fabs(near_gain_db_get(sp->product, sp->near)) <= EXPOSE_NEAR_DB);
	CHECK_UINT(sp->exposure, 0);
	cap_bounds_check(&alone.cap);
	CHECK_UINT(over.end.diverge_resets, 0);
	CHECK(wp->frames > 0u);
	CHECK(fabs(near_gain_db_get(wp->product, wp->near)) <= EXPOSE_NEAR_DB);
	cap_bounds_check(&over.cap);
}

#define IDENT_FRAMES		(30u * FRAMES_PER_SECOND)
#define IDENT_FAN_DBM0		-45.0

/* The far ends of far_floor_identity_test. The fan is the negative
 * control. */
enum ident_far {
	IDENT_FAR_WORDS,
	IDENT_FAR_RING,
	IDENT_FAR_SPEECH,
	IDENT_FAR_DTX,
	IDENT_FAR_FAN,
};

/*
 * The twins of far_floor_identity_test: the instance of a ringback line,
 * which runs with the far-end floor on, and a twin that runs with it off.
 * Both have the processor on. The counts take the frames whose outputs
 * differ, the frames whose introspection differs, and the frames on which
 * the bar of the first instance stands over the fixed bar.
 */
struct ident_line {
	struct ring_line line;
	struct udsp_ec *twin;
	unsigned differ;
	unsigned states;
	unsigned raised;
};

static void ident_line_init(struct ident_line *l, unsigned rate)
{
	memset(l, 0, sizeof(*l));
	ring_line_init(&l->line, rate);
	l->twin = udsp_ec_nlms.create_rate(0, rate);
	udsp_ec_nlms.nlp(l->line.ec, true);
	udsp_ec_nlms.nlp(l->twin, true);
	udsp_ec_nlms_ref_floor_set(l->line.ec, true);
	udsp_ec_nlms_ref_floor_set(l->twin, false);
}

/* One frame to both instances: the lead runs with the floor switch on, the
 * twin with it off. */
static void ident_step(struct ident_line *l, const int16_t *ref)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info lead_info;
	struct udsp_ec_nlms_info twin_info;
	struct ring_planes p;
	int16_t out[UDSP_FRAME_MAX];
	size_t bytes = l->line.frame * sizeof(*out);

	ring_planes_run(&l->line, ref, NULL, NULL, &p);
	memcpy(out, p.mic, bytes);
	udsp_ec_nlms.run(l->twin, out, ref);
	udsp_ec_nlms_frame_get(l->line.ec, &reading);
	udsp_ec_nlms_info_get(l->line.ec, &lead_info);
	udsp_ec_nlms_info_get(l->twin, &twin_info);
	l->differ += memcmp(out, p.out, bytes) != 0;
	/* zero_skip_info_same leaves skip_frames out, because its first
	 * callers compare a full form against a skip form. The twins here run
	 * one form, so they must also skip the same frames. */
	l->states += !zero_skip_info_same(&lead_info, &twin_info) ||
		     lead_info.skip_frames != twin_info.skip_frames;
	l->raised += reading.ref_bar != REF_BAR_FIXED;
}

/* 30 s of one far end through the twins. far_speech_fill and dtx_far_fill
 * exist at 8 kHz only. */
static void ident_run(enum ident_far far, unsigned rate, struct ident_line *l)
{
	struct dtx_far dtx = { .state = 0x0c0ffee1u };
	uint32_t state = 0x13572468u;
	int16_t ref[UDSP_FRAME_MAX];
	double m1 = 0.0;
	double m2 = 0.0;
	unsigned f;

	ident_line_init(l, rate);
	for (f = 0; f < IDENT_FRAMES; f++) {
		switch (far) {
		case IDENT_FAR_RING:
			if (f < RING_FRAMES) {
				ring_fill(&l->line, ref, f);
				break;
			}
			word_fill(&l->line.far, ref, l->line.frame);
			break;
		case IDENT_FAR_WORDS:
			word_fill(&l->line.far, ref, l->line.frame);
			break;
		case IDENT_FAR_SPEECH:
			far_speech_fill(ref, f, &state, &m1, &m2);
			break;
		case IDENT_FAR_DTX:
			dtx_far_fill(&dtx, ref);
			break;
		case IDENT_FAR_FAN:
			noise_fill(ref, l->line.frame, IDENT_FAN_DBM0, &state);
			break;
		}
		ident_step(l, ref);
	}
	udsp_ec_nlms.destroy(l->line.ec);
	udsp_ec_nlms.destroy(l->twin);
}

static void ident_print(const char *what, unsigned rate,
			const struct ident_line *l)
{
	printf("# %s at %u Hz: %u of %u frames differ, %u states differ, %u "
	       "frames read a raised bar\n", what, rate, l->differ,
	       IDENT_FRAMES, l->states, l->raised);
}

/*
 * The identity of the far-end floor. Twins take the same frames of a
 * ringback line, the first with the switch on and the second with it off.
 * The far ends, 30 s each, pause to silence. They are the word source at
 * -20 dBm0 and the ringback line at both rates, and the speech-shaped and DTX
 * far ends at 8 kHz. Every output frame and the introspection must be equal,
 * and the bar of the first must read the fixed bar on every frame. A fan at
 * -45 dBm0, the negative control, must give different outputs.
 */
static void far_floor_identity_test(void)
{
	static const struct {
		enum ident_far far;
		unsigned rate;
		const char *what;
	} runs[] = {
		{ IDENT_FAR_WORDS, UDSP_SAMPLE_RATE, "words" },
		{ IDENT_FAR_WORDS, UDSP_SAMPLE_RATE_WB, "words" },
		{ IDENT_FAR_RING, UDSP_SAMPLE_RATE, "ringback line" },
		{ IDENT_FAR_RING, UDSP_SAMPLE_RATE_WB, "ringback line" },
		{ IDENT_FAR_SPEECH, UDSP_SAMPLE_RATE, "speech-shaped far end" },
		{ IDENT_FAR_DTX, UDSP_SAMPLE_RATE, "DTX far end" },
	};
	struct ident_line l;
	unsigned i;

	CASE("lec runs the arithmetic of the fixed bar on a far end that "
	     "pauses to silence");
	for (i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
		ident_run(runs[i].far, runs[i].rate, &l);
		ident_print(runs[i].what, runs[i].rate, &l);
		CHECK_UINT(l.differ, 0);
		CHECK_UINT(l.states, 0);
		CHECK_UINT(l.raised, 0);
	}
	ident_run(IDENT_FAR_FAN, UDSP_SAMPLE_RATE, &l);
	ident_print("fan", UDSP_SAMPLE_RATE, &l);
	CHECK(l.differ > 0u);
	CHECK(l.raised > 0u);
}

#define BAR_WORDS_FRAMES	(5u * FRAMES_PER_SECOND)
#define BAR_SILENT_FRAMES	100u
#define BAR_STEP_FRAMES		500u
#define BAR_LOW_DBM0		-52.0
#define BAR_HIGH_DBM0		-46.0
#define BAR_WHITE_DBM0		-20.0
/* The history of the far-end floor, 300 to 349 frames. */
#define BAR_HELD_FRAMES		300u
#define BAR_SETTLED_FRAMES	350u
#define BAR_TOL_DB		3.0

/* What the bar of a frame of far_floor_bar_test must read: any bar, the
 * fixed bar, the level of a white fan at dbm0 times the margin within 3 dB,
 * or the cap of the frame. */
enum bar_want {
	BAR_WANT_ANY,
	BAR_WANT_FIXED,
	BAR_WANT_LEVEL,
	BAR_WANT_CAP,
};

/* One part of a step: the bar it wants, the frames, the frames that miss the
 * bar and those that read a background, and the largest distance from the
 * level in dB. */
struct bar_part {
	enum bar_want want;
	double dbm0;
	unsigned frames;
	unsigned miss;
	unsigned background;
	double worst_db;
};

static void bar_part_check(struct bar_part *p,
			   const struct udsp_ec_nlms_frame *reading)
{
	double rms = dbm0_rms_get(p->dbm0);
	double db;

	p->frames++;
	p->background += reading->background;
	if (p->want == BAR_WANT_ANY)
		return;
	if (p->want == BAR_WANT_FIXED) {
		p->miss += reading->ref_bar != REF_BAR_FIXED;
		return;
	}
	if (p->want == BAR_WANT_CAP) {
		p->miss += reading->ref_bar != reading->ref_bar_max;
		return;
	}
	db = 10.0 * log10((double)reading->ref_bar /
			  (rms * rms * REF_FLOOR_MARGIN));
	if (fabs(db) > fabs(p->worst_db))
		p->worst_db = db;
	p->miss += fabs(db) > BAR_TOL_DB;
}

/* One step of the fan. held takes frames 1 to 300 of the step, and settled
 * the frames from 350 on. */
static void bar_step_run(struct fan_line *line, double fan_dbm0,
			 struct bar_part *held, struct bar_part *settled,
			 struct cap_stat *cap)
{
	struct udsp_ec_nlms_frame reading;
	unsigned k;

	line->fan_dbm0 = fan_dbm0;
	for (k = 1; k <= BAR_STEP_FRAMES; k++) {
		(void)fan_cap_frame_run(line, true, false, NULL, cap);
		udsp_ec_nlms_frame_get(line->ec, &reading);
		if (k <= BAR_HELD_FRAMES)
			bar_part_check(held, &reading);
		if (k >= BAR_SETTLED_FRAMES)
			bar_part_check(settled, &reading);
	}
}

/*
 * A flat 30 dB hybrid at 8 kHz takes 5 s of far words. The fan then steps
 * through silence, -52 dBm0, -46 dBm0, white noise at -20 dBm0 and silence.
 * After silence the bar must read the fixed bar for 300 frames, and from frame
 * 350 on the level times the margin within 3 dB. After the step to -46 dBm0
 * it must keep the value of -52 dBm0 for 300 frames, then follow. The white
 * noise must read the cap, and the bar must read the fixed bar on the second
 * silent frame, not on the first. From frame 350 on, only the fan at -46 dBm0,
 * over the fixed bar, must read a background.
 */
static void far_floor_bar_test(void)
{
	struct bar_part low_held = { .want = BAR_WANT_FIXED };
	struct bar_part low = { .want = BAR_WANT_LEVEL, .dbm0 = BAR_LOW_DBM0 };
	struct bar_part high_held = {
		.want = BAR_WANT_LEVEL, .dbm0 = BAR_LOW_DBM0,
	};
	struct bar_part high = {
		.want = BAR_WANT_LEVEL, .dbm0 = BAR_HIGH_DBM0,
	};
	struct bar_part white_held = { .want = BAR_WANT_ANY };
	struct bar_part white = { .want = BAR_WANT_CAP };
	struct udsp_ec_nlms_frame first;
	struct udsp_ec_nlms_frame second;
	struct cap_stat cap;
	struct fan_line line;
	unsigned f;

	memset(&cap, 0, sizeof(cap));
	fan_line_init(&line, UDSP_SAMPLE_RATE, 0.0);
	fan_line_set(&line, BAR_LOW_DBM0, FLOOR_ERL_DB);
	for (f = 0; f < BAR_WORDS_FRAMES; f++)
		(void)fan_cap_frame_run(&line, false, true, NULL, &cap);
	for (f = 0; f < BAR_SILENT_FRAMES; f++)
		(void)fan_cap_frame_run(&line, false, false, NULL, &cap);
	bar_step_run(&line, BAR_LOW_DBM0, &low_held, &low, &cap);
	bar_step_run(&line, BAR_HIGH_DBM0, &high_held, &high, &cap);
	bar_step_run(&line, BAR_WHITE_DBM0, &white_held, &white, &cap);
	(void)fan_cap_frame_run(&line, false, false, NULL, &cap);
	udsp_ec_nlms_frame_get(line.ec, &first);
	(void)fan_cap_frame_run(&line, false, false, NULL, &cap);
	udsp_ec_nlms_frame_get(line.ec, &second);
	udsp_ec_nlms.destroy(line.ec);

	CASE("lec raises the activity bar over the stationary level of the "
	     "reference");
	printf("# -52 dBm0 after silence: %u frames of the first 300 off the "
	       "fixed bar, %u from frame 350 off the level times the margin, "
	       "largest %.2f dB; -46 dBm0: %u and %u, largest %.2f and %.2f "
	       "dB; white: %u frames from 350 off the cap; silence: bar %.0f "
	       "then %.0f\n", low_held.miss, low.miss, low.worst_db,
	       high_held.miss, high.miss, high_held.worst_db, high.worst_db,
	       white.miss, (double)first.ref_bar, (double)second.ref_bar);
	printf("# background from frame 350: -52 dBm0 %u of %u, -46 dBm0 %u of "
	       "%u, white %u of %u and %u of the first 300; silence %u then "
	       "%u\n", low.background, low.frames, high.background,
	       high.frames, white.background, white.frames,
	       white_held.background, first.background, second.background);
	cap_print("bar line", &cap);
	CHECK_UINT(low_held.miss, 0);
	CHECK_UINT(low.miss, 0);
	CHECK_UINT(high_held.miss, 0);
	CHECK_UINT(high.miss, 0);
	CHECK_UINT(white.miss, 0);
	CHECK(first.ref_bar == first.ref_bar_max);
	CHECK(first.ref_bar > REF_BAR_FIXED);
	CHECK(second.ref_bar == REF_BAR_FIXED);
	CHECK_UINT(low.background, 0);
	CHECK(high.frames > 0u && high.background == high.frames);
	CHECK_UINT(white.background, 0);
	CHECK_UINT(white_held.background, 0);
	CHECK(!first.background && !second.background);
	cap_bounds_check(&cap);
}

#define CAP_FAN_DBM0		-48.0
#define CAP_ERL_DB		6.0
#define CAP_FAN_FRAMES		(10u * FRAMES_PER_SECOND)
#define CAP_TALK_FRAMES		(20u * FRAMES_PER_SECOND)

/* The fan at -48 dBm0 on the 6 dB hybrid, with the far words on it. */
static void cap_fan_run(struct cap_stat *st)
{
	struct udsp_ec_nlms_info info;
	struct fan_line line;
	unsigned f;

	memset(st, 0, sizeof(*st));
	fan_line_init(&line, UDSP_SAMPLE_RATE, 0.0);
	fan_line_set(&line, CAP_FAN_DBM0, CAP_ERL_DB);
	for (f = 0; f < CAP_FAN_FRAMES; f++) {
		udsp_ec_nlms_info_get(line.ec, &info);
		(void)fan_frame_run(&line, true, true, NULL);
		cap_check(st, line.ec, info.echo_gain);
	}
	udsp_ec_nlms.destroy(line.ec);
}

/* The fixture of stale_quiet_test for 20 s: the stale run starts trials
 * that a word talker 2 dB under the echo refuses. */
static void cap_trial_run(struct cap_stat *st)
{
	struct udsp_ec_nlms_info info;
	struct word_source talker;
	struct ring_line line;
	int16_t ref[UDSP_FRAME_MAX];
	int16_t near[UDSP_FRAME_MAX];
	unsigned f;

	memset(st, 0, sizeof(*st));
	ring_line_init(&line, UDSP_SAMPLE_RATE);
	word_source_init(&talker, UDSP_SAMPLE_RATE,
			 TALKER_ECHO_DBM0 - QUIET_UNDER_DB, 0x31415926u);
	for (f = 0; f < TALKER_CONVERGE_FRAMES + CAP_TALK_FRAMES; f++) {
		bool talk = f >= TALKER_CONVERGE_FRAMES;

		if (talk) {
			word_fill(&line.far, ref, line.frame);
			word_fill(&talker, near, line.frame);
		} else {
			noise_fill(ref, line.frame, RING_WORDS_DBM0,
				   &line.far_state);
		}
		udsp_ec_nlms_info_get(line.ec, &info);
		ring_frame_run(&line, ref, talk ? near : NULL, NULL);
		cap_check(st, line.ec, info.echo_gain);
	}
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * The cap of the activity bar follows the learned gain of the frame. A fan
 * at -48 dBm0 on a 6 dB hybrid must keep the fixed bar on every frame. On
 * every frame of that line and of the line of stale_quiet_test, the bar must
 * stand at or under the cap. The cap must equal the larger of 2500 and 82.4
 * over the bound of the gain that the introspection read after the frame
 * before. The second line must hold a rising gain and a rejected trial that
 * restores another gain.
 */
static void far_floor_cap_test(void)
{
	struct cap_stat fan;
	struct cap_stat trial;

	cap_fan_run(&fan);
	cap_trial_run(&trial);

	CASE("lec caps the activity bar by the learned gain of the frame");
	cap_print("fan on the 6 dB hybrid", &fan);
	cap_print("stale-estimate trials", &trial);
	CHECK_UINT(fan.raised, 0);
	CHECK_UINT(fan.over, 0);
	CHECK_UINT(fan.wrong, 0);
	CHECK_UINT(trial.over, 0);
	CHECK_UINT(trial.wrong, 0);
	CHECK(trial.raised > 0u);
	CHECK(trial.rises > 0u);
	CHECK(trial.restores > 0u);
}

#define FEED_FAN_DBM0		-45.0
#define FEED_TRAIN_FRAMES	(3u * FRAMES_PER_SECOND)
#define FEED_FAN_FRAMES		400u

/*
 * The far-end floor takes the frames of the reference feed and survives the
 * reset. Twins train on the same far words for 3 s on a flat 24 dB hybrid.
 * The first then takes 400 frames of a fan at -45 dBm0 through run(), the
 * second through udsp_ec_nlms_ref_feed. Their floors must be equal and over
 * 396.8. The reset of the first must keep its floor. The case compares no
 * bar, because the learned gain moves on the frames of run() only.
 */
static void far_floor_feed_test(void)
{
	struct udsp_ec_nlms_info lead_info;
	struct udsp_ec_nlms_info twin_info;
	struct udsp_ec_nlms_info reset_info;
	struct word_source far;
	struct echo_path path;
	struct udsp_ec *lead = udsp_ec_nlms.create(0);
	struct udsp_ec *twin = udsp_ec_nlms.create(0);
	uint32_t state = 0x0f00f00du;
	int16_t ref[UDSP_FRAME_SAMPLES];
	int16_t lead_out[UDSP_FRAME_SAMPLES];
	int16_t twin_out[UDSP_FRAME_SAMPLES];
	struct cap_stat cap;
	unsigned f;

	memset(&cap, 0, sizeof(cap));
	path_flat_init(&path, UDSP_SAMPLE_RATE, 8, FAN_ERL_DB);
	word_source_init(&far, UDSP_SAMPLE_RATE, FAN_WORDS_DBM0, 0x13572468u);
	for (f = 0; f < FEED_TRAIN_FRAMES; f++) {
		word_fill(&far, ref, UDSP_FRAME_SAMPLES);
		udsp_ec_nlms_info_get(lead, &lead_info);
		twin_step(lead, twin, &path, ref, NULL, lead_out, twin_out,
			  NULL);
		cap_check(&cap, lead, lead_info.echo_gain);
	}
	for (f = 0; f < FEED_FAN_FRAMES; f++) {
		noise_fill(ref, UDSP_FRAME_SAMPLES, FEED_FAN_DBM0, &state);
		path_run(&path, ref, lead_out);
		udsp_ec_nlms_info_get(lead, &lead_info);
		udsp_ec_nlms.run(lead, lead_out, ref);
		cap_check(&cap, lead, lead_info.echo_gain);
		udsp_ec_nlms_ref_feed(twin, ref);
	}
	udsp_ec_nlms_info_get(lead, &lead_info);
	udsp_ec_nlms_info_get(twin, &twin_info);
	udsp_ec_nlms_reset(lead);
	udsp_ec_nlms_info_get(lead, &reset_info);
	udsp_ec_nlms.destroy(lead);
	udsp_ec_nlms.destroy(twin);

	CASE("lec feeds the far-end floor from the reference feed and keeps it "
	     "through a reset");
	printf("# far-end floor %.1f after run(), %.1f after the feed, %.1f "
	       "after the reset\n", (double)lead_info.ref_floor,
	       (double)twin_info.ref_floor, (double)reset_info.ref_floor);
	cap_print("feed line, first twin", &cap);
	CHECK(lead_info.ref_floor == twin_info.ref_floor);
	CHECK(lead_info.ref_floor > REF_FLOOR_IDENTITY);
	CHECK(reset_info.ref_floor == lead_info.ref_floor);
	cap_bounds_check(&cap);
}

#define FLOOR_TRIAL_LOUD_DBM0	-35.0
#define FLOOR_TRIAL_QUIET_DBM0	-45.0
#define FLOOR_TRIAL_STEP_DB	10.0
#define FLOOR_TRIAL_TRAIN	(5u * FRAMES_PER_SECOND)
#define FLOOR_TRIAL_DROP	20u
#define FLOOR_TRIAL_TALKER	40u
/* A trial frame after the hangover of the trial ran out. The learned gain
 * rises on the clear frames before it, so the talker end restores another
 * gain. */
#define FLOOR_TRIAL_LATE_TALKER	80u
#define FLOOR_TRIAL_LIMIT	200u

/*
 * The trial under a raised bar. talker_at 0 runs no talker. start holds the
 * frame of the step at which the trial starts, end the trial frame that ends
 * it, and miss the trial frames before the talker whose bar is not the fixed
 * bar. reading holds the reading of the frame that ended the trial, and cap
 * the cap check of every frame of the line.
 */
struct floor_trial {
	unsigned start;
	unsigned end;
	unsigned miss;
	struct udsp_ec_nlms_frame reading;
	struct cap_stat cap;
};

/*
 * The fan line at 8 kHz, a 30 dB hybrid and the fan at -35 dBm0 with far
 * words for 5 s, so the bar reads the cap. The hybrid then steps to 10 dB,
 * and the latch starts a trial. The fan drops to -45 dBm0 on trial frame 20.
 * A talker 16 dB over the fan joins on trial frame talker_at.
 */
static void floor_trial_run(unsigned talker_at, struct floor_trial *t)
{
	struct udsp_ec_nlms_info info;
	struct udsp_ec_nlms_frame reading;
	struct fan_line line;
	uint32_t near_state = 0xfeedfaceu;
	int16_t near[UDSP_FRAME_MAX];
	unsigned k;
	unsigned f;

	memset(t, 0, sizeof(*t));
	fan_line_init(&line, UDSP_SAMPLE_RATE, 0.0);
	fan_line_set(&line, FLOOR_TRIAL_LOUD_DBM0, FLOOR_ERL_DB);
	for (f = 0; f < FLOOR_TRIAL_TRAIN; f++)
		(void)fan_cap_frame_run(&line, true, true, NULL, &t->cap);
	path_erl_set(&line.path, FLOOR_TRIAL_STEP_DB);
	for (f = 0; f < SNAP_LIMIT_FRAMES && !t->start; f++) {
		info = fan_cap_frame_run(&line, true, false, NULL, &t->cap);
		if (info.probes)
			t->start = f + 1u;
	}
	for (k = 1; t->start && k <= FLOOR_TRIAL_LIMIT && !t->end; k++) {
		bool talk = k == talker_at;

		if (k == FLOOR_TRIAL_DROP)
			line.fan_dbm0 = FLOOR_TRIAL_QUIET_DBM0;
		if (talk)
			noise_fill(near, line.frame,
				   FLOOR_TRIAL_QUIET_DBM0 + TALKER_OVER_DB,
				   &near_state);
		(void)fan_cap_frame_run(&line, true, false, talk ? near : NULL,
					&t->cap);
		udsp_ec_nlms_frame_get(line.ec, &reading);
		if (!talk && reading.probing)
			t->miss += reading.ref_bar != REF_BAR_FIXED;
		if (reading.trial_end == UDSP_EC_NLMS_TRIAL_END_NONE)
			continue;
		t->end = k;
		t->reading = reading;
	}
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * A hybrid step under a fan at -35 dBm0 over a raised bar starts a latch
 * trial. The fan drops to -45 dBm0 on trial frame 20. Every trial frame must
 * read the fixed bar, and the trial must end within 100 frames of its start.
 * A trial that kept the raised bar would wait on the quieter fan. In a second
 * run a talker 16 dB over the fan ends the trial on trial frame 40, and that
 * frame reads the raised bar. A third run puts the talker on trial frame 80,
 * and the cap of that frame must read the gain before the frame.
 */
static void far_floor_trial_test(void)
{
	struct floor_trial quiet;
	struct floor_trial talk;
	struct floor_trial late;

	floor_trial_run(0, &quiet);
	floor_trial_run(FLOOR_TRIAL_TALKER, &talk);
	floor_trial_run(FLOOR_TRIAL_LATE_TALKER, &late);

	CASE("lec reads the fixed bar during a trial under a raised bar");
	printf("# trial from frame %u after the step, ended on trial frame %u "
	       "(%s), %u trial frames off the fixed bar; with the talker: "
	       "ended on trial frame %u (%s), talker %u, bar %.0f, %u frames "
	       "before it off the fixed bar\n", quiet.start, quiet.end,
	       lecr_trial_end_name(quiet.reading.trial_end), quiet.miss,
	       talk.end, lecr_trial_end_name(talk.reading.trial_end),
	       talk.reading.talker, (double)talk.reading.ref_bar, talk.miss);
	CHECK(quiet.start > 0u);
	CHECK(quiet.end > 0u && quiet.end <= TRIAL_BUDGET);
	CHECK_UINT(quiet.miss, 0);
	CHECK_UINT(talk.end, FLOOR_TRIAL_TALKER);
	CHECK_UINT(talk.reading.trial_end, UDSP_EC_NLMS_TRIAL_END_TALKER);
	CHECK(talk.reading.talker);
	CHECK(talk.reading.probing);
	CHECK(talk.reading.ref_bar > REF_BAR_FIXED);
	CHECK_UINT(talk.miss, 0);
	cap_print("trial line", &quiet.cap);
	cap_print("trial line with the talker", &talk.cap);
	printf("# with the talker on trial frame %u: ended on trial frame %u "
	       "(%s)\n", FLOOR_TRIAL_LATE_TALKER, late.end,
	       lecr_trial_end_name(late.reading.trial_end));
	cap_print("trial line with the late talker", &late.cap);
	cap_bounds_check(&quiet.cap);
	cap_bounds_check(&talk.cap);
	CHECK_UINT(late.end, FLOOR_TRIAL_LATE_TALKER);
	CHECK_UINT(late.reading.trial_end, UDSP_EC_NLMS_TRIAL_END_TALKER);
	CHECK_UINT(late.cap.restores, 1);
	cap_bounds_check(&late.cap);
}

#define LOUDER_STEP_DB		18.0
#define LOUDER_TRAIN_FRAMES	(5u * FRAMES_PER_SECOND)
#define LOUDER_LIMIT_FRAMES	(12u * FRAMES_PER_SECOND)
#define LOUDER_MEASURE_FRAMES	(5u * FRAMES_PER_SECOND)
#define LOUDER_ERLE_TOL_DB	3.0
/* A pause frame is one whose window holds no far word: the window of 7
 * frames at 16 kHz and one frame more. */
#define LOUDER_PAUSE_FRAMES	8u

/*
 * The louder path of far_floor_latch_test. start and end are the frames after
 * the step that start and end the first trial, with its arm, its end and
 * whether the start frame was active. pause counts the pause frames from
 * frame 350 to the start of the trial, and background those of them that
 * read a background. The ERLE figures cover the far-word frames of the 5 s
 * before the step and of the 5 s after the trial. They read 8.21 and
 * 27.23 dB at 8 kHz, and 7.83 and 24.72 dB at 16 kHz. cap holds the cap
 * check of every frame of the line.
 */
struct louder_stat {
	unsigned start;
	unsigned end;
	unsigned arm;
	unsigned end_kind;
	bool start_active;
	unsigned pause;
	unsigned background;
	double before_db;
	double after_db;
	struct cap_stat cap;
};

/* One frame of the louder path, and the pause count of the frames before the
 * trial. pw->index is the frame count of the line after the frame. */
static void louder_frame_run(struct fan_line *line, struct louder_stat *st,
			     struct phrase_words *pw)
{
	struct udsp_ec_nlms_frame reading;

	(void)phrase_frame_run(line, pw, NULL, &st->cap);
	udsp_ec_nlms_frame_get(line->ec, &reading);
	if (st->start || pw->index < BAR_SETTLED_FRAMES ||
	    pw->silent < LOUDER_PAUSE_FRAMES)
		return;
	st->pause++;
	st->background += reading.background;
}

/* The ERLE of the far-word frames over the next frames of the line. */
static double louder_erle_run(struct fan_line *line, struct louder_stat *st,
			      struct phrase_words *pw)
{
	unsigned f;

	line->echo_energy = 0.0;
	line->filter_energy = 0.0;
	line->measure = true;
	for (f = 0; f < LOUDER_MEASURE_FRAMES; f++)
		louder_frame_run(line, st, pw);
	line->measure = false;
	return fan_filter_erle_get(line);
}

/*
 * The fan line with a flat 30 dB hybrid, a far-end background at -45 dBm0
 * from the first frame and far words at -20 dBm0 over it, in the phrases of
 * phrase_frame_run. After 5 s the hybrid steps to 18 dB. The run looks for
 * the first trial within limit frames of the step.
 */
static void louder_run(unsigned rate, unsigned limit, struct louder_stat *st)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct phrase_words pw = { .index = 0 };
	struct fan_line line;
	unsigned f;

	memset(st, 0, sizeof(*st));
	fan_line_init(&line, rate, 0.0);
	fan_line_set(&line, FLOOR_FAN_DBM0, FLOOR_ERL_DB);
	line.measure_words = true;
	line.measure = true;
	for (f = 0; f < LOUDER_TRAIN_FRAMES; f++)
		louder_frame_run(&line, st, &pw);
	line.measure = false;
	st->before_db = fan_filter_erle_get(&line);
	path_erl_set(&line.path, LOUDER_STEP_DB);
	for (f = 1; f <= limit && !st->end; f++) {
		louder_frame_run(&line, st, &pw);
		udsp_ec_nlms_frame_get(line.ec, &reading);
		udsp_ec_nlms_info_get(line.ec, &info);
		if (!st->start && info.probes) {
			st->start = f;
			st->arm = reading.trial_arm;
			st->start_active = reading.ref_power >= reading.ref_bar;
		}
		if (!st->start ||
		    reading.trial_end == UDSP_EC_NLMS_TRIAL_END_NONE)
			continue;
		st->end = f;
		st->end_kind = reading.trial_end;
	}
	if (st->end)
		st->after_db = louder_erle_run(&line, st, &pw);
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * The hybrid steps from 30 to 18 dB under a far-end background at -45 dBm0
 * and phrases of far words. A latch trial must start within 12 s of the step
 * on an active frame, and the line must keep it. The pause frames before the
 * trial must read a background. After the trial the filter ERLE on the far
 * words must reach that of the 5 s before the step, less 3 dB. The check is
 * one-sided, because those 5 s hold the cold start of the filter. A canceller
 * that decays the run in a pause runs no trial in 20 s at either rate.
 */
static void far_floor_latch_test(unsigned rate)
{
	struct louder_stat st;

	louder_run(rate, LOUDER_LIMIT_FRAMES, &st);

	case_rate(rate, "lec reaches the latch trial of a louder path under a "
		  "far-end background");
	printf("# step from %.0f to %.0f dB: trial %s from frame %u after the "
	       "step, active %u, ended %s on frame %u; %u of %u pause frames "
	       "before it read a background; filter ERLE %.2f dB before the "
	       "step, %.2f dB after the trial\n", FLOOR_ERL_DB, LOUDER_STEP_DB,
	       lecr_trial_arm_name(st.arm), st.start, st.start_active,
	       lecr_trial_end_name(st.end_kind), st.end, st.background,
	       st.pause, st.before_db, st.after_db);
	cap_print("louder path", &st.cap);
	CHECK(st.start > 0u && st.start <= LOUDER_LIMIT_FRAMES);
	CHECK_UINT(st.arm, UDSP_EC_NLMS_TRIAL_ARM_LATCH);
	CHECK(st.start_active);
	CHECK_UINT(st.end_kind, UDSP_EC_NLMS_TRIAL_END_KEPT);
	CHECK(st.after_db >= st.before_db - LOUDER_ERLE_TOL_DB);
	CHECK(st.pause > 0u);
	CHECK_UINT(st.background, st.pause);
	cap_bounds_check(&st.cap);
}

#define WORDS_ERL_DB		27.0
#define WORDS_FAN_DBM0		-45.0
/*
 * A far word every 45 frames, 450 ms. The restart comes before the shortest
 * pause of the word source, so every far word is the first word of the
 * source, 18 frames long. The window of 5 frames at 8 kHz holds the last
 * word frame until frame 21 of the cycle, and the window of 7 frames at
 * 16 kHz until frame 23 (words_pause_from_get). From the next frame on, the
 * window holds the fan alone, or nothing on a line with the fan off.
 */
#define WORDS_CYCLE		45u
#define WORDS_LENGTH		18u
#define WORDS_TRAIN_FRAMES	(5u * FRAMES_PER_SECOND)
#define WORDS_FRAMES		(25u * FRAMES_PER_SECOND)
#define WORDS_COUNT		((WORDS_FRAMES + WORDS_CYCLE - 1u) / \
				 WORDS_CYCLE)
/* The first word after the training, and the word of the near-end legs. */
#define WORDS_FIRST		((WORDS_TRAIN_FRAMES + WORDS_CYCLE - 1u) / \
				 WORDS_CYCLE)
#define WORDS_MARK		(WORDS_FIRST + 4u)
#define WORDS_ACT_MAX		2u
/* A run that starts on the first frame of a word engages on its 8th frame
 * (LEC_NLP_ON_FRAMES of dsp/lec.c). */
#define WORDS_ENGAGE_FRAME	7u
#define WORDS_BURST_DBM0	-30.0
#define WORDS_BURST_FRAMES	10u
#define WORDS_BURST_LEAD	5u
#define WORDS_UNDER_DB		6.0
#define WORDS_NEAR_FRAMES	6u
/*
 * The near word through the processor, on a build that zeroes the engage run
 * on every quiet frame and runs this case. The word on the first frame of the
 * far word reads -0.0226 dB there. The word from the second frame, under the
 * echo, reads -0.53 dB there, the figure of the filter alone.
 */
#define WORDS_NEAR_ZEROED_DB	-0.0226
#define WORDS_LATE_ZEROED_DB	-0.53

/*
 * One run of far_floor_words_test, or of the cases of a line with the fan
 * off. rate is the line rate, and fan says whether the fan runs. far_dbm0 is
 * the level of the far words after the training. A burst_word n puts the
 * near burst in the pause before word n. A near_word n puts the near word of
 * near_frames frames at near_dbm0 on word n, from frame near_from of the
 * word. A negative near_from starts the near word in the pause before word
 * n. 0 runs neither.
 *
 * latency holds, per word, the first frame of the word that reads nlp_active,
 * or WORDS_CYCLE when none does. quiet, quiet_acted and acted cover every
 * frame. lead counts the frames from the start of the burst to the word, and
 * lead_background those of them that read a background. The other counts
 * cover the frames after the training:
 * - loud and loud_idle: the frames whose far words cross the fixed bar, and
 *   those of them that read inactive, on the line with the processor off;
 * - pause and pause_background: the frames from words_pause_from_get on, and
 *   those of them that read a background, on that line;
 * - far_frames, far_acted and far_out: the frames of the far words, those
 *   that read nlp_active, and the output energy over them, on the line with
 *   the processor on.
 *
 * near is the energy of the near word, and near_on and near_off its products
 * with the output of each line. near_acted counts the frames of the near word
 * that read nlp_active, and near_later those of them after its first frame.
 * near_pause counts the frames of the near word before the far word, and
 * near_pause_lo and near_pause_hi hold the range of their microphone over the
 * floor that the detector read, in dB, on the line with the processor on.
 */
struct words_run {
	unsigned rate;
	bool fan;
	double far_dbm0;
	unsigned burst_word;
	unsigned near_word;
	int near_from;
	unsigned near_frames;
	double near_dbm0;
	unsigned latency[WORDS_COUNT];
	unsigned quiet;
	unsigned quiet_acted;
	unsigned acted;
	unsigned lead;
	unsigned lead_background;
	unsigned loud;
	unsigned loud_idle;
	unsigned pause;
	unsigned pause_background;
	unsigned far_frames;
	unsigned far_acted;
	unsigned near_acted;
	unsigned near_later;
	unsigned near_pause;
	double near_pause_lo;
	double near_pause_hi;
	double far_out;
	double near;
	double near_on;
	double near_off;
	double erle;
};

/*
 * The lines of a run: the fan line with the processor off, and a twin fan
 * line with it on. Both take the same frames, because a fan line builds its
 * planes from its own seeded sources. fan_frame_run measures the filter ERLE
 * on the output, so only the line with the processor off gives it.
 * pause_from is the first pause frame of a cycle (words_pause_from_get).
 */
struct words_lines {
	struct fan_line off;
	struct fan_line on;
	struct word_source talker;
	uint32_t burst_state;
	unsigned pause_from;
	bool fan;
	bool waiting;
};

/* The fan line of far_floor_words_test at the line rate, with the processor
 * off. */
static void words_line_init(struct fan_line *line, unsigned rate)
{
	fan_line_init(line, rate, 0.0);
	fan_line_set(line, WORDS_FAN_DBM0, WORDS_ERL_DB);
}

/* One frame of the far words of far_floor_words_test on one line, with the
 * fan when fan is set: a word starts on every frame f that is a multiple of
 * WORDS_CYCLE. */
static struct udsp_ec_nlms_info words_frame_run(struct fan_line *line,
						bool fan, unsigned f,
						const int16_t *near)
{
	if (f % WORDS_CYCLE == 0u)
		word_source_restart(&line->far);
	return fan_frame_run(line, fan, true, near);
}

/* The first frame of a cycle after its window stops holding the far word. */
static unsigned words_pause_from_get(const struct udsp_ec *ec)
{
	return WORDS_LENGTH + window_frames_get(ec) - 1u;
}

static void words_lines_init(struct words_lines *l, const struct words_run *r)
{
	words_line_init(&l->off, r->rate);
	words_line_init(&l->on, r->rate);
	udsp_ec_nlms.nlp(l->on.ec, true);
	l->off.measure_words = true;
	word_source_init(&l->talker, r->rate, r->near_dbm0, 0x0badcafeu);
	l->burst_state = 0xfeedfaceu;
	l->pause_from = words_pause_from_get(l->on.ec);
	l->fan = r->fan;
	l->waiting = false;
}

/* The frame of the near word that frame f carries, counted from 0, or -1
 * when f carries none. The near word holds near_frames frames from frame
 * near_from of word near_word. */
static long words_near_index_get(const struct words_run *r, unsigned f)
{
	long index = (long)f - (long)r->near_word * WORDS_CYCLE -
		     r->near_from;

	if (!r->near_word || index < 0 || index >= (long)r->near_frames)
		return -1;
	return index;
}

/* The near plane of frame f, or NULL. The near word is a word source at
 * near_dbm0. On far_floor_words_test it stands 6 dB under the echo of the far
 * word, so the two follow one envelope. */
static const int16_t *words_near_get(struct words_lines *l,
				     const struct words_run *r, unsigned f,
				     int16_t *near)
{
	unsigned p = f % WORDS_CYCLE;
	unsigned n = f / WORDS_CYCLE;

	if (n + 1u == r->burst_word &&
	    p + WORDS_BURST_LEAD + WORDS_BURST_FRAMES >= WORDS_CYCLE &&
	    p + WORDS_BURST_LEAD < WORDS_CYCLE) {
		noise_fill(near, l->on.frame, WORDS_BURST_DBM0,
			   &l->burst_state);
		return near;
	}
	if (words_near_index_get(r, f) < 0)
		return NULL;
	word_fill(&l->talker, near, l->on.frame);
	return near;
}

/* The line with the processor on: the first frame of each word that reads
 * nlp_active, the quiet frames, the frames before the word of the burst, and
 * the far frames after the training. */
static void words_on_note(struct words_lines *l, struct words_run *r,
			  unsigned f, const struct udsp_ec_nlms_info *info)
{
	struct udsp_ec_nlms_frame reading;
	unsigned p = f % WORDS_CYCLE;
	unsigned n = f / WORDS_CYCLE;
	unsigned i;

	udsp_ec_nlms_frame_get(l->on.ec, &reading);
	r->acted += info->nlp_active;
	if (reading.nlp_verdict == LECR_VERDICT_QUIET) {
		r->quiet++;
		r->quiet_acted += info->nlp_active;
	}
	if (p == 0u) {
		r->latency[n] = WORDS_CYCLE;
		l->waiting = true;
	}
	if (l->waiting && info->nlp_active) {
		r->latency[n] = p;
		l->waiting = false;
	}
	if (n + 1u == r->burst_word &&
	    p + WORDS_BURST_LEAD + WORDS_BURST_FRAMES >= WORDS_CYCLE) {
		r->lead++;
		r->lead_background += reading.background;
	}
	if (f < WORDS_TRAIN_FRAMES || p >= WORDS_LENGTH)
		return;
	r->far_frames++;
	r->far_acted += info->nlp_active;
	for (i = 0; i < l->on.frame; i++)
		r->far_out += (double)l->on.out[i] * l->on.out[i];
}

/* The line with the processor off: the activity of the word frames and the
 * background of the pause frames after the training. */
static void words_off_note(const struct words_lines *l, struct words_run *r,
			   unsigned f)
{
	struct udsp_ec_nlms_frame reading;
	unsigned p = f % WORDS_CYCLE;

	if (f < WORDS_TRAIN_FRAMES)
		return;
	udsp_ec_nlms_frame_get(l->off.ec, &reading);
	if (l->off.word_loud) {
		r->loud++;
		r->loud_idle += reading.ref_power < reading.ref_bar;
	}
	if (p < l->pause_from)
		return;
	r->pause++;
	r->pause_background += reading.background;
}

/* A frame of the near word before the far word, into the range of
 * near_pause_lo and near_pause_hi. */
static void words_pause_note(const struct words_lines *l, struct words_run *r)
{
	struct udsp_ec_nlms_frame reading;
	double db;

	udsp_ec_nlms_frame_get(l->on.ec, &reading);
	db = ratio_db_get(reading.mic_power, reading.noise_power);
	if (!r->near_pause || db < r->near_pause_lo)
		r->near_pause_lo = db;
	if (!r->near_pause || db > r->near_pause_hi)
		r->near_pause_hi = db;
	r->near_pause++;
}

/* The near word against the output of each line: the regression products of
 * near_gain_db_get, and the frames of the word that read nlp_active. index
 * counts the frames of the word from 0. */
static void words_near_note(const struct words_lines *l, struct words_run *r,
			    const int16_t *near, unsigned index, bool acted)
{
	unsigned i;

	for (i = 0; i < l->on.frame; i++) {
		r->near += (double)near[i] * near[i];
		r->near_on += (double)l->on.out[i] * near[i];
		r->near_off += (double)l->off.out[i] * near[i];
	}
	r->near_acted += acted;
	if (index)
		r->near_later += acted;
	if ((long)index < -(long)r->near_from)
		words_pause_note(l, r);
}

/* The run checks two figures of the line with the processor on: the six
 * counts add up to frames, and nlp_quiet_frames counts every frame that reads
 * quiet. It therefore runs inside the case. */
static void words_run(struct words_run *r)
{
	struct udsp_ec_nlms_info info;
	struct words_lines l;
	int16_t near[UDSP_FRAME_MAX];
	unsigned f;

	words_lines_init(&l, r);
	for (f = 0; f < WORDS_FRAMES; f++) {
		const int16_t *plane = words_near_get(&l, r, f, near);
		long index = words_near_index_get(r, f);

		if (f == WORDS_TRAIN_FRAMES) {
			l.off.far.level_dbm0 = r->far_dbm0;
			l.on.far.level_dbm0 = r->far_dbm0;
			l.off.measure = true;
		}
		(void)words_frame_run(&l.off, l.fan, f, plane);
		info = words_frame_run(&l.on, l.fan, f, plane);
		words_on_note(&l, r, f, &info);
		words_off_note(&l, r, f);
		if (plane && index >= 0)
			words_near_note(&l, r, plane, (unsigned)index,
					info.nlp_active);
	}
	r->erle = fan_filter_erle_get(&l.off);
	verdict_identity_check(l.on.ec, r->acted);
	udsp_ec_nlms_info_get(l.on.ec, &info);
	CHECK_UINT(info.nlp_quiet_frames, r->quiet);
	udsp_ec_nlms.destroy(l.off.ec);
	udsp_ec_nlms.destroy(l.on.ec);
}

/* A run of far_floor_words_test: 8 kHz, the fan on, and the near word of
 * WORDS_NEAR_FRAMES frames 6 dB under the echo. */
static void words_run_set(struct words_run *r, double far_dbm0)
{
	memset(r, 0, sizeof(*r));
	r->rate = UDSP_SAMPLE_RATE;
	r->fan = true;
	r->far_dbm0 = far_dbm0;
	r->near_frames = WORDS_NEAR_FRAMES;
	r->near_dbm0 = FAN_WORDS_DBM0 - WORDS_ERL_DB - WORDS_UNDER_DB;
}

/*
 * The words after the training whose processor acts more than WORDS_ACT_MAX
 * frames into the word. worst takes the largest latency. The build that
 * zeroes the engage run on every quiet frame engages on frame 7 of each of
 * these words, and acts on 495 of their 805 frames.
 */
static unsigned words_slow_count(const struct words_run *r, unsigned *worst)
{
	unsigned slow = 0;
	unsigned n;

	*worst = 0;
	for (n = WORDS_FIRST; n < WORDS_COUNT; n++) {
		slow += r->latency[n] > WORDS_ACT_MAX;
		if (r->latency[n] > *worst)
			*worst = r->latency[n];
	}
	return slow;
}

static void words_far_print(const char *what, const struct words_run *r)
{
	printf("# %s: %u of %u loud word frames inactive; the processor acts "
	       "on %u of %u far frames after the training, output %.2f dBm0 "
	       "over them\n", what, r->loud_idle, r->loud, r->far_acted,
	       r->far_frames, energy_dbm0_get(r->far_out, r->far_frames *
					      (r->rate / 100u)));
}

/*
 * Softer far words after the training, printed with no bound. The raised bar
 * holds part of the word frames at -44 dBm0 inactive. The build that zeroes
 * the engage run on every quiet frame acts on 2, 278 and 495 of the 805 far
 * frames at -44, -38 and -32 dBm0, with an output of -71.69, -72.14 and
 * -72.27 dBm0. The processor must not act on a quiet frame of these runs
 * either.
 */
static void words_levels_run(void)
{
	static const double levels[3] = { -44.0, -38.0, -32.0 };
	struct words_run r;
	char what[48];
	unsigned i;

	for (i = 0; i < 3u; i++) {
		words_run_set(&r, levels[i]);
		words_run(&r);
		snprintf(what, sizeof(what), "far words at %.0f dBm0",
			 levels[i]);
		words_far_print(what, &r);
		CHECK(r.quiet > 0u);
		CHECK_UINT(r.quiet_acted, 0);
	}
}

/*
 * Twin lines, with the processor off and on, take a fan at -45 dBm0 on a
 * 27 dB hybrid and far words at -20 dBm0. After 5 s, the first must hold the
 * words active with the filter ERLE of fan_step_test. On the second, each
 * word must engage the processor within 2 frames, and the processor must not
 * act on a quiet frame. A near burst at -30 dBm0 that ends 5 frames before a
 * word must hold that word to its 8th frame. A near word from the second frame
 * of a far word stands 6 dB under its echo. The processor replaces its first
 * frame and must act on no later frame of the word.
 */
static void far_floor_words_test(void)
{
	struct words_run run;
	struct words_run burst;
	struct words_run start;
	struct words_run late;
	unsigned worst;
	unsigned slow;

	CASE("lec keeps the processor ready for far words over a far-end "
	     "background");
	words_run_set(&run, FAN_WORDS_DBM0);
	words_run(&run);
	words_run_set(&burst, FAN_WORDS_DBM0);
	burst.burst_word = WORDS_MARK;
	words_run(&burst);
	words_run_set(&start, FAN_WORDS_DBM0);
	start.near_word = WORDS_MARK;
	words_run(&start);
	words_run_set(&late, FAN_WORDS_DBM0);
	late.near_word = WORDS_MARK;
	late.near_from = 1u;
	words_run(&late);
	slow = words_slow_count(&run, &worst);

	printf("# fan %.0f dBm0 on a %.0f dB hybrid, far words %.0f dBm0, "
	       "processor off: %u of %u pause frames read a background, filter "
	       "ERLE %.2f dB against %.2f dB\n", WORDS_FAN_DBM0, WORDS_ERL_DB,
	       FAN_WORDS_DBM0, run.pause_background, run.pause, run.erle,
	       STEP_HEAD_ERLE_DB);
	printf("# processor on: %u of %u words after the training engage "
	       "later than frame %u, the latest on frame %u; %u of %u quiet "
	       "frames read nlp_active\n", slow, WORDS_COUNT - WORDS_FIRST,
	       WORDS_ACT_MAX, worst, run.quiet_acted, run.quiet);
	words_far_print("far words at -20 dBm0", &run);
	printf("# near burst at %.0f dBm0: %u of %u frames before the word "
	       "read a background, the word engages on its frame %u\n",
	       WORDS_BURST_DBM0, burst.lead_background, burst.lead,
	       burst.latency[WORDS_MARK]);
	printf("# near word %.0f dB under the echo for %u frames, from the "
	       "second frame of the far word: %.2f dB through, filter alone "
	       "%.2f dB (%.2f dB with the run zeroed); the processor acts on "
	       "%u of its frames, %u after the first\n", WORDS_UNDER_DB,
	       WORDS_NEAR_FRAMES, near_gain_db_get(late.near_on, late.near),
	       near_gain_db_get(late.near_off, late.near),
	       WORDS_LATE_ZEROED_DB, late.near_acted, late.near_later);
	printf("# the same word from the first frame of the far word, printed "
	       "with no bound: %.4f dB through (%.4f dB with the run zeroed)\n",
	       near_gain_db_get(start.near_on, start.near),
	       WORDS_NEAR_ZEROED_DB);
	words_levels_run();
	CHECK(run.loud > 0u);
	CHECK_UINT(run.loud_idle, 0);
	CHECK(run.pause > 0u);
	CHECK_UINT(run.pause_background, run.pause);
	CHECK(run.erle >= STEP_HEAD_ERLE_DB - STEP_ERLE_TOL_DB);
	CHECK_UINT(slow, 0);
	CHECK(run.quiet > 0u);
	CHECK_UINT(run.quiet_acted + burst.quiet_acted + start.quiet_acted +
		   late.quiet_acted, 0);
	CHECK_UINT(burst.lead, WORDS_BURST_FRAMES + WORDS_BURST_LEAD);
	CHECK_UINT(burst.lead_background, burst.lead);
	CHECK_UINT(burst.latency[WORDS_MARK], WORDS_ENGAGE_FRAME);
	CHECK(late.near > 0.0);
	CHECK_UINT(late.near_later, 0);
}

#define TOGGLE_TRAIN_FRAMES	900u
#define TOGGLE_FRAMES		300u
#define TOGGLE_SETTLE_FRAMES	30u
/* The window of 5 frames at 8 kHz plus one frame: the shortest period of a
 * release and an engage over a background. */
#define TOGGLE_PERIOD		6u
/* The engage events of this case on a build that zeroes the engage run on
 * every quiet frame. A period holds 5 pass frames, so its run never
 * completes. */
#define TOGGLE_ZEROED_ENGAGES	0u

/*
 * The processor of far_floor_toggle_test. all_acted and stray cover every
 * frame of the line: the frames that read nlp_active, and those of them whose
 * verdict is not pass. The other counts cover the frames after the settling:
 * the frames, the engage events (nlp_active from 0 to 1), the acting frames
 * and the gap frames, and the gap frames that read quiet with a background.
 * act_out and gap_out hold the output energy of the acting and the gap
 * frames. last is nlp_active of the frame before.
 */
struct toggle_stat {
	unsigned all_acted;
	unsigned stray;
	unsigned frames;
	unsigned engages;
	unsigned acted;
	unsigned gaps;
	unsigned gap_quiet;
	double act_out;
	double gap_out;
	bool last;
};

static void toggle_note(struct toggle_stat *st, const struct fan_line *line,
			bool active, bool measured)
{
	struct udsp_ec_nlms_frame reading;
	double energy = 0.0;
	bool rose = active && !st->last;
	unsigned i;

	udsp_ec_nlms_frame_get(line->ec, &reading);
	st->all_acted += active;
	st->stray += active && reading.nlp_verdict != LECR_VERDICT_PASS;
	st->last = active;
	if (!measured)
		return;
	for (i = 0; i < line->frame; i++)
		energy += (double)line->out[i] * line->out[i];
	st->frames++;
	st->engages += rose;
	if (active) {
		st->acted++;
		st->act_out += energy;
		return;
	}
	st->gaps++;
	st->gap_quiet += reading.nlp_verdict == LECR_VERDICT_QUIET &&
			 reading.background;
	st->gap_out += energy;
}

/*
 * The fan line of far_floor_words_test, with the processor on, trains on its
 * far words for 9 s. A loud far frame then comes every 6 frames, so the window
 * at 8 kHz holds 5 active frames and one background frame per period. The
 * background frame releases the processor, and the next pass frame engages it
 * again. The case records the engage events, the acting frames and the output
 * of the gap frames against the acting frames. There must be at least one
 * engage event and at most one per period. The processor must act on pass
 * frames only, and the six counts must add up to frames.
 */
static void far_floor_toggle_test(void)
{
	struct udsp_ec_nlms_info info;
	struct toggle_stat st;
	struct fan_line line;
	unsigned f;

	CASE("lec engages the processor again at most once per window over a "
	     "far-end background");
	memset(&st, 0, sizeof(st));
	words_line_init(&line, UDSP_SAMPLE_RATE);
	udsp_ec_nlms.nlp(line.ec, true);
	for (f = 0; f < TOGGLE_TRAIN_FRAMES; f++) {
		info = words_frame_run(&line, true, f, NULL);
		toggle_note(&st, &line, info.nlp_active, false);
	}
	for (f = 0; f < TOGGLE_FRAMES; f++) {
		bool loud = f % TOGGLE_PERIOD == 0u;

		if (loud)
			word_source_restart(&line.far);
		info = fan_frame_run(&line, true, loud, NULL);
		toggle_note(&st, &line, info.nlp_active,
			    f >= TOGGLE_SETTLE_FRAMES);
	}
	verdict_identity_check(line.ec, st.all_acted);
	udsp_ec_nlms.destroy(line.ec);

	printf("# a loud far frame every %u frames over the fan: %u engage "
	       "events in %u frames (%u with the run zeroed), %u acting "
	       "frames, %.2f per period; %u of %u gap frames read quiet with a "
	       "background; gap frames %.2f dB against the acting frames\n",
	       TOGGLE_PERIOD, st.engages, st.frames, TOGGLE_ZEROED_ENGAGES,
	       st.acted, (double)st.acted * TOGGLE_PERIOD / st.frames,
	       st.gap_quiet, st.gaps,
	       expose_db_get(st.gap_out * st.acted, st.act_out * st.gaps));
	CHECK(st.engages > 0u);
	CHECK(st.engages <= st.frames / TOGGLE_PERIOD);
	CHECK_UINT(st.stray, 0);
}

/*
 * A run of the cases of a line with the fan off, at the line rate: the far
 * words of far_floor_words_test pause to digital silence, and the near end
 * carries its floor at -72 dBm0 alone. Every pause frame reads quiet with no
 * background. The floor that the pauses measure stands under the
 * echo-likeness bar of a quiet frame, so a pause keeps the engage run.
 */
static void words_quiet_set(struct words_run *r, unsigned rate)
{
	words_run_set(r, FAN_WORDS_DBM0);
	r->rate = rate;
	r->fan = false;
}

/*
 * Far words that pause to silence (words_quiet_set). Each word after the
 * training must engage the processor on its first frame, because the pause
 * before it keeps a complete run. A near burst at -30 dBm0 that ends 5 frames
 * before a word zeroes the run, so that word must engage on its 8th frame. The
 * processor must not act on a quiet frame. A rule that keeps the run through
 * the burst fails here, and so does a rule that zeroes it in such a pause.
 */
static void pause_burst_test(unsigned rate)
{
	struct words_run run;
	unsigned late = 0;
	unsigned n;

	case_rate(rate, "lec nlp acts on the first frame of a far word after a "
		  "pause to silence and waits again after a near burst");
	words_quiet_set(&run, rate);
	run.burst_word = WORDS_MARK;
	words_run(&run);
	for (n = WORDS_FIRST; n < WORDS_COUNT; n++)
		late += n != WORDS_MARK && run.latency[n] != 0u;
	printf("# far words at %.0f dBm0 with pauses to silence: %u of %u "
	       "pause frames read a background; %u of %u words after the "
	       "training and not after the burst engage after their first "
	       "frame; the "
	       "word after a near burst at %.0f dBm0 engages on its frame %u; "
	       "%u of %u quiet frames read nlp_active\n", FAN_WORDS_DBM0,
	       run.pause_background, run.pause, late,
	       WORDS_COUNT - WORDS_FIRST - 1u, WORDS_BURST_DBM0,
	       run.latency[WORDS_MARK], run.quiet_acted, run.quiet);
	words_far_print("far words at -20 dBm0 with pauses to silence", &run);
	CHECK(run.pause > 0u);
	CHECK_UINT(run.pause_background, 0);
	CHECK_UINT(late, 0);
	CHECK_UINT(run.latency[WORDS_MARK], WORDS_ENGAGE_FRAME);
	CHECK(run.quiet > 0u);
	CHECK_UINT(run.quiet_acted, 0);
}

#define PAUSE_LEAD_UNDER_DB	3.0
#define PAUSE_LEAD_FRAMES	4u
/*
 * The mid word. PAUSE_MID_OVER_DB is the nominal level of its source over the
 * floor level, not the level that the bars see. The microphone carries the
 * unvoiced onset of the word, 8 dB under its body on a 7 dB envelope, plus
 * the floor, and the tracked floor reads under the floor level. With a silent
 * reference, the bars of a quiet frame stand about 4.5 and 6.0 dB over the
 * tracked floor under the two floor headrooms of dsp/lec.c. The four onset
 * frames of a word spread over about 1.4 dB at the microphone, so the word
 * starts one frame before the far word. That frame stands between the bars at
 * both rates, 0.4 dB or more from each, and the leg prints its level.
 */
#define PAUSE_MID_OVER_DB	4.5
#define PAUSE_MID_FRAMES	1u

/* The figure that a rule which keeps the run over a background only reads on
 * a leg of pause_expose_test. */
#define BACKGROUND_ONLY_NOTE	", the figure with the run kept over a " \
				"background only"

/* The late near word of a run against the output of each line. note follows
 * the figure through the processor when through is set, else the figure of
 * the filter alone. */
static void words_late_print(const char *what, const struct words_run *r,
			     bool through)
{
	printf("# %s: near word %.0f dB under the echo for %u frames, from the "
	       "second frame of the far word: %.2f dB through%s; filter alone "
	       "%.2f dB%s; the processor acts on %u of its frames, %u after "
	       "the first\n", what, WORDS_UNDER_DB, r->near_frames,
	       near_gain_db_get(r->near_on, r->near),
	       through ? BACKGROUND_ONLY_NOTE : "",
	       near_gain_db_get(r->near_off, r->near),
	       through ? "" : BACKGROUND_ONLY_NOTE, r->near_acted,
	       r->near_later);
}

/* A run of a near word whose source stands at over_db against the floor
 * level, on the line of pause_burst_test, from lead frames before the far
 * word. */
static void words_lead_run(struct words_run *r, unsigned rate, double over_db,
			   unsigned lead)
{
	words_quiet_set(r, rate);
	r->near_word = WORDS_MARK;
	r->near_from = -(int)lead;
	r->near_frames = lead + WORDS_NEAR_FRAMES;
	r->near_dbm0 = FAN_FLOOR_DBM0 + over_db;
	words_run(r);
}

static void words_lead_print(const char *what, const struct words_run *r,
			     double over_db)
{
	printf("# %s, pauses to silence: near word source %.1f dB %s the "
	       "floor level for %u frames, %u of them in the pause, whose "
	       "microphone stands %.2f to %.2f dB over the tracked floor: "
	       "%.2f dB through; filter alone %.2f dB%s; the processor acts on "
	       "%u of its frames; the far word engages on its frame %u\n",
	       what, fabs(over_db), over_db < 0.0 ? "under" : "over",
	       r->near_frames, r->near_pause, r->near_pause_lo,
	       r->near_pause_hi, near_gain_db_get(r->near_on, r->near),
	       near_gain_db_get(r->near_off, r->near), BACKGROUND_ONLY_NOTE,
	       r->near_acted, r->latency[WORDS_MARK]);
}

/*
 * Each leg prints a near word through the processor and through the filter
 * alone, with no bound. On pause_burst_test's line, the late leg runs the
 * near word of far_floor_words_test, 6 dB under the echo from the second far
 * frame. The lead and mid legs start a near word 4 frames and 1 frame before
 * the far word. They print the level of its pause frames over the floor. The
 * background leg runs the late word on the fan line, which
 * far_floor_words_test checks at 8 kHz alone. With the run kept over a
 * background only, no complete run survives a pause to silence, and the
 * first three legs read the filter figure.
 */
static void pause_expose_test(unsigned rate)
{
	struct words_run late;
	struct words_run lead;
	struct words_run mid;
	struct words_run fan;

	case_rate(rate, "lec nlp exposure of a near word to a kept engage run");
	words_quiet_set(&late, rate);
	late.near_word = WORDS_MARK;
	late.near_from = 1;
	words_run(&late);
	words_lead_run(&lead, rate, -PAUSE_LEAD_UNDER_DB, PAUSE_LEAD_FRAMES);
	words_lead_run(&mid, rate, PAUSE_MID_OVER_DB, PAUSE_MID_FRAMES);
	words_run_set(&fan, FAN_WORDS_DBM0);
	fan.rate = rate;
	fan.near_word = WORDS_MARK;
	fan.near_from = 1;
	words_run(&fan);
	words_late_print("late, pauses to silence", &late, false);
	words_lead_print("lead", &lead, -PAUSE_LEAD_UNDER_DB);
	words_lead_print("mid", &mid, PAUSE_MID_OVER_DB);
	words_late_print("late, background", &fan, true);
	CHECK(late.near > 0.0);
	CHECK(lead.near > 0.0);
	CHECK(mid.near > 0.0);
	CHECK(fan.near > 0.0);
}

#define STATE_ROOM_DBM0		-60.0
#define STATE_PAUSE_FRAMES	12u
#define STATE_FAR_FRAMES	20u
/* Longer than a frame, so the first frame of a far return carries no echo. */
#define STATE_LATE_MS		20u
/* The 4 tail frames of the window, then more than the
 * LEC_GEIGEL_TRUST_PAUSE_FRAMES quiet frames of dsp/lec.c. */
#define STATE_TRUST_PAUSE	210u

/*
 * The line of the state cases of the engage run: the fixture of
 * nlp_wait_count_test at the line rate on a flat 12 dB hybrid at delay_ms,
 * with the processor engaged and no measured floor. Room noise at
 * STATE_ROOM_DBM0 joins the near end on the frames that ask for it. room and
 * out hold the near plane and the output of the last frame.
 */
struct state_line {
	struct echo_path path;
	struct udsp_ec *ec;
	uint32_t far_state;
	uint32_t room_state;
	unsigned acted;
	int16_t room[UDSP_FRAME_MAX];
	int16_t out[UDSP_FRAME_MAX];
};

static void state_line_init(struct state_line *s, unsigned rate,
			    unsigned delay_ms)
{
	memset(s, 0, sizeof(*s));
	s->far_state = 0x77665544u;
	s->room_state = 0x600dcafeu;
	s->ec = gate_fixture_rate_create(&s->path, rate, delay_ms, 12.0, 150u,
					 &s->far_state, &s->acted);
}

/* One frame: far-end noise at -10 dBm0 when far is set, else digital silence,
 * with the room noise on top when room is set. It returns true when the frame
 * reads nlp_active. */
static bool state_frame_run(struct state_line *s, bool far, bool room)
{
	int16_t ref[UDSP_FRAME_MAX];
	unsigned was = s->acted;

	memset(ref, 0, sizeof(ref));
	memset(s->room, 0, sizeof(s->room));
	if (far)
		noise_fill(ref, s->path.frame, -10.0, &s->far_state);
	if (room)
		noise_fill(s->room, s->path.frame, STATE_ROOM_DBM0,
			   &s->room_state);
	verdict_out_step(s->ec, &s->path, ref, room ? s->room : NULL, s->out,
			 &s->acted);
	return s->acted != was;
}

/* frames frames of far-end digital silence, with the room noise from frame
 * from on when room is set. */
static void state_pause_run(struct state_line *s, bool room, unsigned from,
			    unsigned frames)
{
	unsigned f;

	for (f = 0; f < frames; f++)
		(void)state_frame_run(s, false, room && f >= from);
}

/* A far return, with the room noise when room is set. It returns the first
 * frame of the return that reads nlp_active, counted from 0, or
 * STATE_FAR_FRAMES when none does. */
static unsigned state_far_run(struct state_line *s, bool room)
{
	unsigned f;

	for (f = 0; f < STATE_FAR_FRAMES; f++) {
		if (state_frame_run(s, true, room))
			return f;
	}
	return STATE_FAR_FRAMES;
}

/*
 * A pause of pause frames, then a far return (state_far_run). The frames
 * whose window still holds the far end carry digital silence. With room set,
 * the room noise starts on the first quiet frame and runs through the return.
 */
static unsigned state_return_run(struct state_line *s, bool room,
				 unsigned pause)
{
	state_pause_run(s, room, window_frames_get(s->ec) - 1u, pause);
	return state_far_run(s, room);
}

/*
 * A line with no measured floor gives a quiet frame a floor term of 0. The
 * echo-likeness bar of that frame is then the estimate times the margin, and
 * the estimate over a silent far end is 0. Room noise therefore stands over
 * the bar and zeroes the run on the first quiet frame, and the far return
 * engages on its 8th frame. Digital silence, the common case under the bar,
 * keeps the run (state_silence_test).
 */
static void state_room_test(unsigned rate)
{
	struct state_line s;
	unsigned at;

	state_line_init(&s, rate, 8u);
	at = state_return_run(&s, true, STATE_PAUSE_FRAMES);
	case_rate(rate, "lec nlp zeroes the engage run on a pause with room "
		  "noise and no measured floor");
	printf("# the far return acts from its frame %u\n", at);
	CHECK_UINT(at, WORDS_ENGAGE_FRAME);
	udsp_ec_nlms.destroy(s.ec);
}

/* On a line with no measured floor, the echo-likeness bar of a quiet frame
 * is the estimate times the margin. Digital silence at the near end never
 * stands over that bar, because 0 stands at or under any bar. A pause of
 * digital silence keeps the run, and the far return acts on its first frame. */
static void state_silence_test(unsigned rate)
{
	struct state_line s;
	unsigned at;

	state_line_init(&s, rate, 8u);
	at = state_return_run(&s, false, STATE_PAUSE_FRAMES);
	case_rate(rate, "lec nlp keeps the engage run through a pause of "
		  "digital silence");
	printf("# the far return acts from its frame %u\n", at);
	CHECK_UINT(at, 0);
	udsp_ec_nlms.destroy(s.ec);
}

/*
 * The bounds of the return frame of state_early_test. That frame is the
 * engage frame, so nlp_apply cross-fades it from the residual to the comfort
 * noise. The two are uncorrelated, and each keeps about a third of its power
 * over the frame. With both at the floor, the frame reads about 1.8 dB under
 * the floor, inside EARLY_FLOOR_TOL_DB. The regression gain of the output on
 * the room noise is the mean weight of the residual in the fade, about -6 dB.
 * A frame that passes the residual reads 0 dB, over EARLY_GAIN_MAX_DB.
 */
#define EARLY_FLOOR_TOL_DB	3.0
#define EARLY_GAIN_MAX_DB	-3.0

/*
 * The first pass frame of a far return can come before its echo reaches the
 * microphone, as on this 20 ms hybrid. A first pause measures the floor, and
 * the far end engages the processor again. After a second pause the
 * processor acts on the first frame of the return, and comfort noise must
 * replace the room noise there. That frame must stand within 3 dB of the
 * floor and not over the microphone. Its regression gain on the room noise
 * must stay under -3 dB.
 */
static void state_early_test(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct state_line s;
	double product = 0.0;
	double room = 0.0;
	double out_dbov;
	double gain_db;
	float out_power;
	unsigned first;
	unsigned at;
	unsigned i;

	state_line_init(&s, rate, STATE_LATE_MS);
	first = state_return_run(&s, true, STATE_PAUSE_FRAMES);
	at = state_return_run(&s, true, STATE_PAUSE_FRAMES);
	udsp_ec_nlms_frame_get(s.ec, &reading);
	udsp_ec_nlms_info_get(s.ec, &info);
	out_power = udsp_ec_nlms_frame_power_get(s.out, s.path.frame);
	out_dbov = floor_dbov_get(energy_dbm0_get(out_power, 1u));
	for (i = 0; i < s.path.frame; i++) {
		product += (double)s.out[i] * s.room[i];
		room += (double)s.room[i] * s.room[i];
	}
	gain_db = near_gain_db_get(product, room);
	case_rate(rate, "lec nlp replaces the first return frame before its "
		  "echo with comfort noise at the floor");
	printf("# the return after the pause that measures the floor acts from "
	       "its frame %u, the next return from its frame %u; that frame: "
	       "microphone %.2f dBm0, estimate %.2f dBm0, output %.2f dBm0, "
	       "floor %.2f dBov, output %.2f dBov, gain on the room noise "
	       "%.2f dB\n", first, at,
	       energy_dbm0_get(reading.mic_power, 1u),
	       energy_dbm0_get(reading.estimate_power, 1u),
	       energy_dbm0_get(out_power, 1u), (double)info.noise_dbov,
	       out_dbov, gain_db);
	CHECK_UINT(at, 0);
	CHECK(fabs(out_dbov - info.noise_dbov) <= EARLY_FLOOR_TOL_DB);
	CHECK(out_power <= reading.mic_power);
	CHECK(gain_db <= EARLY_GAIN_MAX_DB);
	udsp_ec_nlms.destroy(s.ec);
}

/*
 * A far return after LEC_GEIGEL_TRUST_PAUSE_FRAMES quiet-window frames
 * restarts a trust count that never held (quiet_run_update). The fixture
 * trains on 150 frames, under the 200 of the count, so the trust never held.
 * A pause of digital silence keeps the run, and the restart on the return
 * frame leaves the run alone. The return therefore acts on its first frame,
 * and its reading shows the count started over.
 */
static void state_trust_test(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct state_line s;
	unsigned before;
	unsigned at;

	state_line_init(&s, rate, 8u);
	udsp_ec_nlms_frame_get(s.ec, &reading);
	before = reading.trust_run;
	at = state_return_run(&s, false, STATE_TRUST_PAUSE);
	udsp_ec_nlms_frame_get(s.ec, &reading);
	case_rate(rate, "lec nlp keeps the engage run through a restart of the "
		  "trust count");
	printf("# trust count %u before the pause, %u after the return frame; "
	       "the return acts from its frame %u\n", before, reading.trust_run,
	       at);
	CHECK_UINT(at, 0);
	CHECK(reading.trust_run < before);
	udsp_ec_nlms.destroy(s.ec);
}

/*
 * A tone of whole periods in a frame has the same mean square on every frame,
 * within the rounding to 16 bits, so its level against the floor is set to a
 * few hundredths of a dB. White noise varies by about 0.6 dB from frame to
 * frame at 8 kHz. 1000 Hz holds 10 periods in a frame at both
 * rates. With a silent reference, the bar of a quiet frame stands about
 * 4.5 dB over the tracked floor under LEC_GEIGEL_FLOOR_HEADROOM and about
 * 6.0 dB over it under LEC_GEIGEL_PAUSE_HEADROOM. At both rates the tone of
 * HEADROOM_KEEP_DB stands 0.6 dB or more from each bar, and the tone of
 * HEADROOM_ZERO_DB about 1 dB over the second bar.
 */
#define STATE_TONE_HZ		1000.0
#define STATE_TONE_AT		4u
#define HEADROOM_KEEP_DB	5.3
#define HEADROOM_ZERO_DB	7.0

/* One frame of far-end digital silence with the tone at over_db over the
 * floor that the detector reads on the frame, in place of the room noise. */
static void state_tone_run(struct state_line *s, double over_db)
{
	struct udsp_ec_nlms_info info;
	int16_t ref[UDSP_FRAME_MAX];
	unsigned long phase = 0;
	double floor;

	udsp_ec_nlms_info_get(s->ec, &info);
	floor = 32767.0 * 32767.0 * pow(10.0, -info.noise_dbov / 10.0);
	memset(ref, 0, sizeof(ref));
	memset(s->room, 0, sizeof(s->room));
	tone_fill(s->room, s->path.frame,
		  energy_dbm0_get(floor * pow(10.0, over_db / 10.0), 1u),
		  STATE_TONE_HZ, &phase, s->path.frame * 100u);
	verdict_out_step(s->ec, &s->path, ref, s->room, s->out, &s->acted);
}

/*
 * A pause with room noise and one tone frame in its middle, then a far return
 * with room noise (state_far_run). reading takes the reading of the tone
 * frame. A single frame cannot move the floor: a reading more than
 * LEC_NOISE_RISE_DB over it joins a rise run of LEC_NOISE_RISE_FRAMES.
 */
static unsigned state_tone_return_run(struct state_line *s, double over_db,
				      struct udsp_ec_nlms_frame *reading)
{
	unsigned tail = window_frames_get(s->ec) - 1u;

	state_pause_run(s, true, tail, tail + STATE_TONE_AT);
	state_tone_run(s, over_db);
	udsp_ec_nlms_frame_get(s->ec, reading);
	state_pause_run(s, true, 0u, STATE_TONE_AT);
	return state_far_run(s, true);
}

/*
 * The two floor headrooms of the echo-likeness test, on the state line, after
 * a first pause that measures the floor. In each later pause, a tone frame
 * stands at a set level over the tracked floor. A tone 5.3 dB over it stands
 * between the bars of the two headrooms and keeps the run. The far return
 * then acts on its first frame. A tone 7 dB over it stands over both bars, so
 * the return acts on its 8th frame. A pause headroom equal to the active one
 * fails here.
 */
static void pause_headroom_test(unsigned rate)
{
	struct udsp_ec_nlms_frame keep;
	struct udsp_ec_nlms_frame zero;
	struct state_line s;
	unsigned first;
	unsigned kept;
	unsigned zeroed;

	state_line_init(&s, rate, 8u);
	first = state_return_run(&s, true, STATE_PAUSE_FRAMES);
	kept = state_tone_return_run(&s, HEADROOM_KEEP_DB, &keep);
	zeroed = state_tone_return_run(&s, HEADROOM_ZERO_DB, &zero);
	case_rate(rate, "lec nlp keeps the engage run through a floor "
		  "excursion in a pause and zeroes it over the pause headroom");
	printf("# the return after the pause that measures the floor acts from "
	       "its frame %u; a tone %.2f dB over the floor: the next return "
	       "acts from its frame %u; a tone %.2f dB over the floor: the "
	       "next return acts from its frame %u\n", first,
	       ratio_db_get(keep.mic_power, keep.noise_power), kept,
	       ratio_db_get(zero.mic_power, zero.noise_power), zeroed);
	CHECK(keep.noise_power > 0.0f);
	CHECK_UINT(keep.nlp_verdict, LECR_VERDICT_QUIET);
	CHECK_UINT(zero.nlp_verdict, LECR_VERDICT_QUIET);
	CHECK(keep.estimate_power == 0.0f);
	CHECK(zero.estimate_power == 0.0f);
	CHECK_UINT(kept, 0);
	CHECK_UINT(zeroed, WORDS_ENGAGE_FRAME);
	udsp_ec_nlms.destroy(s.ec);
}

/*
 * The active headroom of the echo-likeness test, on the state line, after a
 * first pause that measures the floor. The far end stops for 2 frames, so the
 * window stays active. On the second of them the 8 ms hybrid returns no echo,
 * and the estimate stands 30 dB under the floor. A tone 5.3 dB over the
 * tracked floor then stands between the bars of the two headrooms. It must
 * read unlike and cut the complete run to LEC_NLP_DECAY_FRAMES, so the far
 * return acts on its 4th frame. A test that takes the pause headroom on an
 * active window fails here.
 */
static void active_headroom_test(unsigned rate)
{
	struct udsp_ec_nlms_frame gap;
	struct udsp_ec_nlms_frame tone;
	struct state_line s;
	unsigned first;
	unsigned at;

	state_line_init(&s, rate, 8u);
	first = state_return_run(&s, true, STATE_PAUSE_FRAMES);
	(void)state_frame_run(&s, false, true);
	udsp_ec_nlms_frame_get(s.ec, &gap);
	state_tone_run(&s, HEADROOM_KEEP_DB);
	udsp_ec_nlms_frame_get(s.ec, &tone);
	at = state_far_run(&s, true);
	case_rate(rate, "lec nlp cuts the engage run on a tone between the "
		  "bars of an active window");
	printf("# the return after the pause that measures the floor acts from "
	       "its frame %u; first gap frame: window run %u, estimate %.3g, "
	       "floor %.3g; a tone %.2f dB over the floor on the second: "
	       "window run %u, estimate %.3g, verdict %s; the far return acts "
	       "from its frame %u\n", first, gap.window_run,
	       (double)gap.estimate_power, (double)gap.noise_power,
	       ratio_db_get(tone.mic_power, tone.noise_power), tone.window_run,
	       (double)tone.estimate_power,
	       udsp_ec_nlms_verdict_name(tone.nlp_verdict), at);
	CHECK(tone.noise_power > 0.0f);
	CHECK(tone.window_run > 0u);
	CHECK(tone.estimate_power * 100.0f < tone.noise_power);
	CHECK_UINT(tone.nlp_verdict, LECR_VERDICT_UNLIKE);
	CHECK_UINT(at, CUT_WAIT);
	udsp_ec_nlms.destroy(s.ec);
}

/*
 * A ringback line with a form of its own, for the lockout cases. Frame f
 * counts from the creation of the instance, 1 on the first run() call, like
 * the frame index of lec_replay:
 * - the ringback runs on frames 1 to ring_frames, with the two tones of the
 *   ring line. From frame noise_from (0 for none), white noise stands
 *   noise_db under each tone. Digital silence replaces it on cut_frames
 *   frames from frame cut_from;
 * - the far words of the word source at words_dbm0 start on frame answer,
 *   from word index first_word. word_fill takes the color of a word from its
 *   index;
 * - a white far-end background at background_dbm0 runs from frame
 *   background_from (0 for none);
 * - the hybrid is flat at erl_db and 8 ms, or the captured path field;
 * - the near-end floor at floor_dbm0 starts on frame floor_from;
 * - a dense talker at talk_dbm0 speaks on frames talk_from up to talk_to;
 * - nlp sets the processor.
 */
struct ring_spec {
	unsigned rate;
	unsigned ring_frames;
	unsigned noise_from;
	double noise_db;
	unsigned cut_from;
	unsigned cut_frames;
	unsigned answer;
	unsigned first_word;
	double words_dbm0;
	unsigned background_from;
	double background_dbm0;
	double erl_db;
	const struct field_path *field;
	unsigned floor_from;
	double floor_dbm0;
	unsigned talk_from;
	unsigned talk_to;
	double talk_dbm0;
	bool nlp;
	struct dense_talker talker;
};

static void ring_spec_line_init(struct ring_line *line, struct ring_spec *s)
{
	ring_line_init(line, s->rate);
	udsp_ec_nlms.nlp(line->ec, s->nlp);
	if (s->field)
		path_field_init(&line->path, s->rate, s->field);
	else
		path_flat_init(&line->path, s->rate, 8, s->erl_db);
	line->far.level_dbm0 = s->words_dbm0;
	line->far.word = s->first_word;
	line->floor_dbm0 = s->floor_dbm0;
	dense_talker_init(&s->talker, s->rate, s->talk_dbm0, 0x0badcafeu);
}

static void ring_part_add(int16_t *dst, const int16_t *part, unsigned n)
{
	unsigned i;

	for (i = 0; i < n; i++)
		dst[i] = sample_clamp((double)dst[i] + part[i]);
}

/* The reference of frame f. */
static void ring_spec_fill(struct ring_line *line, const struct ring_spec *s,
			   unsigned f, int16_t *ref)
{
	int16_t part[UDSP_FRAME_MAX];
	bool cut = s->cut_frames && f >= s->cut_from &&
		   f < s->cut_from + s->cut_frames;

	memset(ref, 0, line->frame * sizeof(*ref));
	if (f <= s->ring_frames && !cut)
		ring_tones_fill(line, ref);
	if (s->noise_from && f >= s->noise_from && f <= s->ring_frames) {
		noise_fill(part, line->frame, RING_DBM0 - s->noise_db,
			   &line->far_state);
		ring_part_add(ref, part, line->frame);
	}
	if (f >= s->answer) {
		word_fill(&line->far, part, line->frame);
		ring_part_add(ref, part, line->frame);
	}
	if (s->background_from && f >= s->background_from) {
		noise_fill(part, line->frame, s->background_dbm0,
			   &line->far_state);
		ring_part_add(ref, part, line->frame);
	}
}

/* Frame f of the line, with the talker and the floor of the spec. */
static void ring_spec_frame_run(struct ring_line *line, struct ring_spec *s,
				unsigned f, struct ring_stat *st,
				struct ring_planes *p)
{
	int16_t ref[UDSP_FRAME_MAX];
	int16_t near[UDSP_FRAME_MAX];
	bool talk = f >= s->talk_from && f < s->talk_to;

	ring_spec_fill(line, s, f, ref);
	if (talk)
		dense_talker_fill(&s->talker, near, line->frame);
	line->floor = f >= s->floor_from;
	ring_planes_run(line, ref, talk ? near : NULL, st, p);
}

/* The three forms of ring_spec_set. */
enum ring_form {
	RING_FORM_INSIDE,
	RING_FORM_REFUSED,
	RING_FORM_BACKGROUND,
};

#define SPEC_ERL_DB		28.0
#define SPEC_TALK_DBM0		-25.0
#define INSIDE_RING_FRAMES	300u
#define INSIDE_NOISE_FROM	101u
#define INSIDE_NOISE_DB		14.0
#define INSIDE_FIELD_NOISE_DB	25.0
#define INSIDE_ANSWER		633u
#define INSIDE_FLOOR_DBM0	-75.0
#define REFUSED_RING_FRAMES	430u
#define REFUSED_CUT_FROM	201u
#define REFUSED_CUT_FRAMES	20u
#define REFUSED_ANSWER		688u
#define REFUSED_WORDS_DBM0	-28.0
#define REFUSED_FLOOR_DBM0	-77.6
#define BACKGROUND_RING_FRAMES	200u
#define BACKGROUND_FROM		241u
#define BACKGROUND_DBM0		-45.0
#define BACKGROUND_ANSWER	291u
#define BACKGROUND_FLOOR_DBM0	-76.5
#define BACKGROUND_TALK_FROM	341u
#define BACKGROUND_TALK_TO	841u

static const struct field_path *field_path_get(const char *name)
{
	unsigned i;

	for (i = 0; i < sizeof(field_paths) / sizeof(field_paths[0]); i++) {
		if (!strcmp(field_paths[i].name, name))
			return &field_paths[i];
	}
	return NULL;
}

/*
 * The forms of the lockout cases, with the levels of the lab captures that
 * each form names. The tones are those of the ring line, at -20 dBm0 each.
 * The hybrid returns them 28 dB down. The measured figures come from a
 * canceller without the trust count. A pair of figures gives 8 kHz, then
 * 16 kHz.
 * - INSIDE, the levels of C5: one burst of 300 frames, with white noise
 *   14 dB under each tone from frame 101. The near floor starts on frame
 *   301, and the first word comes on frame 633. The tracked ERLE ends the
 *   ringback at 18.30 dB (8 kHz). The coefficient energy then stands 9.16 dB
 *   under the learned gain at its floor. At 16 kHz no noise level gives both
 *   figures on the flat hybrid. The form there takes the AN7581 path with the
 *   noise 25 dB under each tone (26.69 dB and 8.89 dB). The energy rule then
 *   holds from the first tones.
 * - REFUSED, the levels of C13: 200 frames of tones, 20 silent frames and
 *   210 frames of tones. The floor stands 32.3 and 32.2 dB under the tone
 *   echo from frame 1, and words at -28 dBm0 start on frame 688. The ringback
 *   ends with the rule refused: the energy stands 11.10 and 13.14 dB under the
 *   gain, with an ERLE of 32.36 and 32.17 dB. The rule turns held on frame
 *   690, with an ERLE of 21.26 and 21.37 dB. Words at -24 dBm0 take the ERLE
 *   under 18 dB first.
 * - BACKGROUND, the form of call T6: 200 frames of tones and 40 silent
 *   frames. A background at -45 dBm0 then runs to the end, with words from
 *   frame 291 and a dense talker from frame 341 to 840. The floor stands 31.3
 *   and 31.5 dB under the tone echo, and 3.3 and 3.5 dB under the echo of the
 *   background. The rule turns held on frame 244 and 245.
 */
static void ring_spec_set(struct ring_spec *s, enum ring_form form,
			  unsigned rate)
{
	memset(s, 0, sizeof(*s));
	s->rate = rate;
	s->words_dbm0 = RING_WORDS_DBM0;
	s->erl_db = SPEC_ERL_DB;
	s->talk_dbm0 = SPEC_TALK_DBM0;
	s->floor_from = 1u;
	s->nlp = true;
	switch (form) {
	case RING_FORM_INSIDE:
		s->ring_frames = INSIDE_RING_FRAMES;
		s->noise_from = INSIDE_NOISE_FROM;
		s->noise_db = INSIDE_NOISE_DB;
		s->answer = INSIDE_ANSWER;
		s->floor_from = INSIDE_RING_FRAMES + 1u;
		s->floor_dbm0 = INSIDE_FLOOR_DBM0;
		if (rate == UDSP_SAMPLE_RATE)
			break;
		s->field = field_path_get("an7581 raw");
		s->noise_db = INSIDE_FIELD_NOISE_DB;
		break;
	case RING_FORM_REFUSED:
		s->ring_frames = REFUSED_RING_FRAMES;
		s->cut_from = REFUSED_CUT_FROM;
		s->cut_frames = REFUSED_CUT_FRAMES;
		s->answer = REFUSED_ANSWER;
		s->words_dbm0 = REFUSED_WORDS_DBM0;
		s->floor_dbm0 = REFUSED_FLOOR_DBM0;
		break;
	case RING_FORM_BACKGROUND:
		s->ring_frames = BACKGROUND_RING_FRAMES;
		s->background_from = BACKGROUND_FROM;
		s->background_dbm0 = BACKGROUND_DBM0;
		s->answer = BACKGROUND_ANSWER;
		s->floor_dbm0 = BACKGROUND_FLOOR_DBM0;
		s->talk_from = BACKGROUND_TALK_FROM;
		s->talk_to = BACKGROUND_TALK_TO;
		break;
	}
}

/*
 * The census of lec_replay (the header comment of test/bench/lec_replay.c),
 * over the frames RING_CENSUS_FROM up to RING_CENSUS_TO, seconds 3 to 29.
 * Its definitions:
 * - a far frame: the largest ref_power of the frame and of the six frames
 *   before it stands at or over -40 dBFS;
 * - a leak frame: a far frame whose output stands over -65 dBFS;
 * - a near frame: mic_power stands at or over 16 times the guard window times
 *   the bound factor, min(0.25, 2 x echo_gain) after the frame. The guard
 *   window is the larger window_power of the frame and of the frame before;
 * - a lockout second: 10 or more far frames, the processor verdict dt on 30
 *   or more of its frames, and a loss under 8 dB. The loss is the microphone
 *   over the output, summed over the far frames of the second.
 * dBFS is 10 log10 of a power over 32768 squared. The echo loss of a second is
 * the power of the echo plane over the power of the leak, on the same far
 * frames. The leak is the output less the near speech and the floor.
 */
#define RING_CENSUS_FROM	300u
#define RING_CENSUS_TO		3000u
#define RING_CENSUS_HISTORY	6u
#define RING_CENSUS_FAR_DBFS	-40.0
#define RING_CENSUS_LEAK_DBFS	-65.0
#define RING_CENSUS_NEAR_RATIO	16.0
#define RING_CENSUS_SECOND_FAR	10u
#define RING_CENSUS_LOCKOUT_DT	30u
#define RING_CENSUS_LOCKOUT_DB	8.0
#define RING_CENSUS_SECONDS	(RING_CENSUS_TO / FRAMES_PER_SECOND)

struct ring_census_second {
	unsigned far;
	unsigned dt;
	double mic;
	double out;
	double echo;
	double leak;
};

/* A census selection: the far frames, or the far frames that are not near
 * frames. leak_max is the largest power of the echo leak, the output less the
 * near plane and the floor plane. */
struct ring_census_sel {
	unsigned frames;
	unsigned leak;
	double out_sum;
	double out_max;
	double leak_max;
};

/* ref_plane and out_plane keep the powers of the planes of each frame for
 * ring_census_leak_count. */
struct ring_census {
	float ref_last[RING_CENSUS_HISTORY];
	float window_last;
	struct ring_census_sel far;
	struct ring_census_sel far_not_near;
	unsigned near;
	struct ring_census_second second[RING_CENSUS_SECONDS];
	float ref_plane[RING_CENSUS_TO];
	float out_plane[RING_CENSUS_TO];
};

static double census_power_get(double dbfs)
{
	return 32768.0 * 32768.0 * pow(10.0, dbfs / 10.0);
}

static void ring_census_sel_add(struct ring_census_sel *s, bool leak,
				float out, double leak_power)
{
	s->frames++;
	s->leak += leak;
	s->out_sum += out;
	if (out > s->out_max)
		s->out_max = out;
	if (leak_power > s->leak_max)
		s->leak_max = leak_power;
}

static double ring_leak_power_get(const struct ring_planes *p, unsigned frame)
{
	double leak = 0.0;
	unsigned i;

	for (i = 0; i < frame; i++) {
		double l = (double)p->out[i] - p->near[i] - p->hiss[i];

		leak += l * l;
	}
	return leak / frame;
}

/* The second of far frame f takes the frame. */
static void ring_census_second_add(struct ring_census *c,
				   const struct udsp_ec_nlms_frame *reading,
				   const struct ring_planes *p, unsigned frame,
				   unsigned f, double leak_power)
{
	struct ring_census_second *sec = &c->second[f / FRAMES_PER_SECOND];

	sec->far++;
	sec->mic += reading->mic_power;
	sec->out += udsp_ec_nlms_frame_power_get(p->out, frame);
	sec->echo += udsp_ec_nlms_frame_power_get(p->echo, frame);
	sec->leak += leak_power;
}

/* Frame f of a line into the census, after the frame ran. */
static void ring_census_add(struct ring_census *c, const struct udsp_ec *ec,
			    const struct ring_planes *p, unsigned frame,
			    unsigned f)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	double leak_power;
	double guard;
	double factor;
	float ref;
	float out = udsp_ec_nlms_frame_power_get(p->out, frame);
	bool far;
	bool leak;
	bool near;
	unsigned i;

	udsp_ec_nlms_frame_get(ec, &reading);
	udsp_ec_nlms_info_get(ec, &info);
	guard = reading.window_power > c->window_last ? reading.window_power :
							  c->window_last;
	factor = 2.0 * info.echo_gain;
	if (factor > 0.25)
		factor = 0.25;
	ref = reading.ref_power;
	for (i = 0; i < RING_CENSUS_HISTORY; i++) {
		if (c->ref_last[i] > ref)
			ref = c->ref_last[i];
	}
	far = ref >= census_power_get(RING_CENSUS_FAR_DBFS);
	leak = far && out > census_power_get(RING_CENSUS_LEAK_DBFS);
	near = reading.mic_power >= RING_CENSUS_NEAR_RATIO * guard * factor;
	memmove(c->ref_last + 1, c->ref_last,
		sizeof(c->ref_last) - sizeof(c->ref_last[0]));
	c->ref_last[0] = reading.ref_power;
	c->window_last = reading.window_power;
	if (f < RING_CENSUS_TO) {
		c->ref_plane[f] = udsp_ec_nlms_frame_power_get(p->ref, frame);
		c->out_plane[f] = out;
	}
	if (f < RING_CENSUS_FROM || f >= RING_CENSUS_TO)
		return;
	c->near += near;
	c->second[f / FRAMES_PER_SECOND].dt +=
		reading.nlp_verdict == LECR_VERDICT_DT;
	if (!far)
		return;
	leak_power = ring_leak_power_get(p, frame);
	ring_census_second_add(c, &reading, p, frame, f, leak_power);
	ring_census_sel_add(&c->far, leak, out, leak_power);
	if (!near)
		ring_census_sel_add(&c->far_not_near, leak, out, leak_power);
}

/* The lockout seconds by the loss of the census, or by the echo loss when
 * echo is set. list takes the seconds, comma separated. */
static unsigned ring_census_lockout_get(const struct ring_census *c, bool echo,
					char *list, size_t size)
{
	size_t used = 0;
	unsigned count = 0;
	unsigned k;

	list[0] = '\0';
	for (k = RING_CENSUS_FROM / FRAMES_PER_SECOND;
	     k < RING_CENSUS_SECONDS; k++) {
		const struct ring_census_second *s = &c->second[k];
		double num = echo ? s->echo : s->mic;
		double den = echo ? s->leak : s->out;

		if (s->far < RING_CENSUS_SECOND_FAR ||
		    s->dt < RING_CENSUS_LOCKOUT_DT)
			continue;
		if (!(10.0 * log10(num / den) < RING_CENSUS_LOCKOUT_DB))
			continue;
		count++;
		if (used < size)
			used += (size_t)snprintf(list + used, size - used,
						 "%s%u", used ? "," : "", k);
	}
	if (!count)
		snprintf(list, size, "-");
	return count;
}

/* The leak frames again, from the powers of the reference and output planes
 * that the census kept. */
static unsigned ring_census_leak_count(const struct ring_census *c)
{
	unsigned count = 0;
	unsigned f;

	for (f = RING_CENSUS_FROM; f < RING_CENSUS_TO; f++) {
		float ref = c->ref_plane[f];
		unsigned k;

		for (k = 1; k <= RING_CENSUS_HISTORY; k++) {
			if (c->ref_plane[f - k] > ref)
				ref = c->ref_plane[f - k];
		}
		count += ref >= census_power_get(RING_CENSUS_FAR_DBFS) &&
			 c->out_plane[f] >
			 census_power_get(RING_CENSUS_LEAK_DBFS);
	}
	return count;
}

#define INSIDE_WORDS_FRAMES	(20u * FRAMES_PER_SECOND)
#define INSIDE_GAP_MIN_DB	-10.0
#define INSIDE_GAP_MAX_DB	-7.0
/* LEC_GEIGEL_GAIN_FLOOR of dsp/lec.c. */
#define INSIDE_GAIN_FLOOR	0.001f

/* The figures of ring_inside_test. at holds the state at the end of the
 * ringback, and answer the reading of the first word frame. */
struct inside_run {
	struct udsp_ec_nlms_info at;
	struct udsp_ec_nlms_frame answer;
	struct udsp_ec_nlms_info end;
	struct ring_stat first;
	struct ring_stat last;
	uint32_t first_probes;
	unsigned last_trusted;
	unsigned first_trust;
};

static void inside_run(struct inside_run *r)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct ring_planes p;
	struct ring_spec s;
	struct ring_line line;
	unsigned end;
	unsigned f;

	memset(r, 0, sizeof(*r));
	ring_spec_set(&s, RING_FORM_INSIDE, UDSP_SAMPLE_RATE);
	s.nlp = false;
	ring_spec_line_init(&line, &s);
	end = s.answer + INSIDE_WORDS_FRAMES;
	for (f = 1; f < end; f++) {
		bool first = f >= s.answer && f < s.answer + RING_FIRST_FRAMES;
		bool last = f + RING_LAST_FRAMES >= end;

		ring_spec_frame_run(&line, &s, f, first ? &r->first :
				    last ? &r->last : NULL, &p);
		udsp_ec_nlms_frame_get(line.ec, &reading);
		udsp_ec_nlms_info_get(line.ec, &info);
		if (f == s.ring_frames)
			r->at = info;
		if (f == s.answer)
			r->answer = reading;
		if (f + 1u == s.answer + RING_FIRST_FRAMES)
			r->first_probes = info.probes;
		if (f >= s.answer && reading.trusted && !r->first_trust)
			r->first_trust = f - s.answer + 1u;
		r->last_trusted += last && reading.trusted;
	}
	r->end = info;
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * A tone-trained filter inside the energy rule: the INSIDE form of
 * ring_spec_set at 8 kHz, with the processor off. After the ringback the
 * tracked ERLE must reach 18 dB, with the coefficient energy 7 to 10 dB under
 * the learned gain at its floor. The first word must meet no trust, and the
 * first 2 s must hold double talk on 20 percent or less, with no trial. The
 * last 8 s of 20 s of words must regain the trust. They must hold 20 percent
 * or less, with a filter ERLE of 15 dB. Without the trust count, the line
 * holds all 77 active frames of the first 2 s.
 */
static void ring_inside_test(void)
{
	struct inside_run r;
	double gap;
	double erle;

	inside_run(&r);
	gap = ring_gap_db_get(&r.at);
	erle = weak_filter_erle_get(r.last.echo, r.last.residual,
				    r.last.floor);

	CASE("lec withholds the trust of a tone-trained filter inside the "
	     "energy rule");
	printf("# after the ringback: tracked ERLE %.2f dB, coefficient energy "
	       "%.2f dB against the learned gain of %.2f dB, floor %.0f dBov; "
	       "first word: trusted %u, trust count %u\n",
	       (double)r.at.erle_db, gap, 10.0 * log10(r.at.echo_gain),
	       (double)r.at.noise_dbov, r.answer.trusted, r.answer.trust_run);
	printf("# first 2 s: double talk on %u of %u active frames, %u trials; "
	       "last 8 s: %u frames trusted, %u of %u active frames held, "
	       "filter ERLE %.2f dB; first trusted frame %u ms after the first "
	       "word; %u trials, %u resets\n", r.first.dt, r.first.active,
	       r.first_probes, r.last_trusted, r.last.held, r.last.active, erle,
	       10u * r.first_trust, r.end.probes, r.end.diverge_resets);
	CHECK(r.at.erle_db >= RING_TRUST_DB);
	CHECK(gap >= INSIDE_GAP_MIN_DB && gap <= INSIDE_GAP_MAX_DB);
	CHECK(r.at.echo_gain == INSIDE_GAIN_FLOOR);
	CHECK(r.at.noise_dbov == 127.0f);
	CHECK(!r.answer.trusted);
	CHECK(r.answer.trust_run < TRUST_FRAMES);
	CHECK(r.first.active > 0u);
	CHECK(r.first.dt * 10u <= r.first.active * 2u);
	CHECK_UINT(r.first_probes, 0);
	CHECK(r.last_trusted > 0u);
	CHECK(r.last.active > 0u);
	CHECK(r.last.held * 10u <= r.last.active * 2u);
	CHECK(erle >= 15.0);
}

#define LOCKOUT_LEAK_MAX	100u
#define LOCKOUT_SECONDS_MAX	1u
#define LOCKOUT_TALK_AFTER	(5u * FRAMES_PER_SECOND)
#define LOCKOUT_TALK_FRAMES	(10u * FRAMES_PER_SECOND)
#define LOCKOUT_SHORT_GAP	100u

/*
 * The census of the legs of ring_lockout_test on a canceller without the
 * trust count, frames 300 to 2999: the far out_sum and out_max. For the
 * BACKGROUND form, the figures are those of the far frames that are not near
 * frames. leak_max is the largest power of the echo leak on those frames.
 * Each figure reads back as the double that canceller measures, so it passes
 * its own bars.
 *
 * The same canceller reads these leak frames and lockout seconds:
 * - INSIDE at 8 kHz: 212 leak frames, 5 lockout seconds;
 * - REFUSED: 83 and 90 leak frames, 6 lockout seconds at each rate;
 * - BACKGROUND: 328 and 331 leak frames, 11 lockout seconds at each rate.
 *
 * The out_max bar of the BACKGROUND form reads leak_max. Reason: a far frame
 * under the near mark can carry near speech. The out_max of the output then
 * measures the phase of the near speech against the leak, not the canceller.
 * At 8 kHz the canceller with the trust count reads an output out_max of
 * 436861.25 against 414770.188 on frame 449, where the leak reads 649 against
 * 11788.
 */
struct lockout_head {
	unsigned rate;
	enum ring_form form;
	double out_sum;
	double out_max;
	double leak_max;
};

static const struct lockout_head lockout_heads[] = {
	{ UDSP_SAMPLE_RATE, RING_FORM_INSIDE, 535924.44934463501,
	  14975.474609375, 0.0 },
	{ UDSP_SAMPLE_RATE, RING_FORM_REFUSED, 102090.23774576187,
	  2728.550048828125, 0.0 },
	{ UDSP_SAMPLE_RATE_WB, RING_FORM_REFUSED, 117826.65641379356,
	  2597.143798828125, 0.0 },
	{ UDSP_SAMPLE_RATE, RING_FORM_BACKGROUND, 4363741.2377746105,
	  414770.1875, 17708.9875 },
	{ UDSP_SAMPLE_RATE_WB, RING_FORM_BACKGROUND, 4208286.3980493546,
	  346661.46875, 16424.05 },
};

static const struct lockout_head *lockout_head_get(unsigned rate,
						   enum ring_form form)
{
	unsigned i;

	for (i = 0; i < sizeof(lockout_heads) / sizeof(lockout_heads[0]); i++) {
		if (lockout_heads[i].rate == rate &&
		    lockout_heads[i].form == form)
			return &lockout_heads[i];
	}
	return NULL;
}

/*
 * One leg of ring_lockout_test, frames 1 to 2999. end holds the state after
 * frame 2999. first_trust is the first frame after the ringback that reads
 * trusted. restart is the first frame after the ringback whose trust count
 * reads 0 or 1 after a larger value. complete is the first frame after it
 * whose count reads TRUST_FRAMES. early counts the frames from restart
 * through complete that read trusted.
 */
struct lockout_leg {
	struct ring_census c;
	struct near_stat speech;
	struct trial_marks marks;
	struct udsp_ec_nlms_info end;
	unsigned first_trust;
	unsigned restart;
	unsigned complete;
	unsigned early;
};

static void lockout_trust_note(struct lockout_leg *g,
			       const struct ring_spec *s,
			       const struct udsp_ec_nlms_frame *reading,
			       unsigned f, unsigned last)
{
	if (f <= s->ring_frames)
		return;
	if (!g->first_trust && reading->trusted)
		g->first_trust = f;
	if (!g->restart && reading->trust_run <= 1u &&
	    last > reading->trust_run)
		g->restart = f;
	if (!g->restart || g->complete)
		return;
	g->early += reading->trusted;
	if (reading->trust_run >= TRUST_FRAMES)
		g->complete = f;
}

static void lockout_leg_run(struct ring_spec *s, struct lockout_leg *g)
{
	struct udsp_ec_nlms_frame reading;
	struct ring_planes p;
	struct ring_line line;
	unsigned last = 0;
	unsigned f;

	memset(g, 0, sizeof(*g));
	ring_spec_line_init(&line, s);
	for (f = 1; f < RING_CENSUS_TO; f++) {
		ring_spec_frame_run(&line, s, f, NULL, &p);
		ring_census_add(&g->c, line.ec, &p, line.frame, f);
		trial_marks_add(&g->marks, line.ec);
		near_stat_add(&g->speech, line.ec, p.near, p.mic, p.out,
			      line.frame);
		udsp_ec_nlms_frame_get(line.ec, &reading);
		lockout_trust_note(g, s, &reading, f, last);
		last = reading.trust_run;
	}
	trial_marks_finish(&g->marks);
	udsp_ec_nlms_info_get(line.ec, &g->end);
	udsp_ec_nlms.destroy(line.ec);
}

static void lockout_speech_print(const char *what,
				 const struct near_stat *sp)
{
	printf("# %s, near speech: %.2f dB through on %u frames, output %.2f "
	       "dB against the microphone, %u exposure frames\n", what,
	       near_gain_db_get(sp->product, sp->near), sp->frames,
	       expose_db_get(sp->out, sp->mic), sp->exposure);
}

static void lockout_speech_check(const struct near_stat *sp)
{
	CHECK(sp->frames > 0u);
	CHECK(fabs(near_gain_db_get(sp->product, sp->near)) <= EXPOSE_NEAR_DB);
	CHECK_UINT(sp->exposure, 0);
}

/*
 * The bars of a leg over frames 300 to 2999:
 * - 100 leak frames or fewer, and one lockout second or fewer;
 * - the out_sum and the out_max at or under those of a canceller without the
 *   trust count (lockout_heads);
 * - no reset and no trial.
 * A BACKGROUND leg reads the far frames that are not near frames and the echo
 * loss, because its talker stands in the output. Its out_max bar reads the
 * echo leak, and the out_max of the output is printed with no bar.
 */
static void lockout_leg_check(const char *what, const struct ring_spec *s,
			      enum ring_form form, const struct lockout_leg *g)
{
	const struct lockout_head *h = lockout_head_get(s->rate, form);
	bool echo = form == RING_FORM_BACKGROUND;
	const struct ring_census_sel *sel = echo ? &g->c.far_not_near :
						   &g->c.far;
	double max = echo ? sel->leak_max : sel->out_max;
	char list[160];
	unsigned lockout = ring_census_lockout_get(&g->c, echo, list,
						   sizeof(list));

	printf("# %s: %u frames, %u leak frames, out_sum %.9g against %.9g, "
	       "out_max %.9g against %.9g, %u lockout seconds %s; first "
	       "trusted frame %u\n", what, sel->frames, sel->leak,
	       sel->out_sum, h ? h->out_sum : 0.0, sel->out_max,
	       h ? h->out_max : 0.0, lockout, list, g->first_trust);
	if (echo)
		printf("# %s: echo leak max %.9g against %.9g\n", what,
		       sel->leak_max, h ? h->leak_max : 0.0);
	trial_marks_print(what, &g->marks);
	CHECK(h != NULL);
	CHECK_UINT(ring_census_leak_count(&g->c), g->c.far.leak);
	CHECK(sel->leak <= LOCKOUT_LEAK_MAX);
	CHECK(lockout <= LOCKOUT_SECONDS_MAX);
	CHECK(h && sel->out_sum <= h->out_sum);
	CHECK(h && max <= (echo ? h->leak_max : h->out_max));
	CHECK_UINT(g->end.diverge_resets, 0);
	CHECK_UINT(g->end.probes, 0);
	trial_marks_check(&g->marks);
}

/* The census figures of a leg with no bar, from frame 300 to 2999. */
static void lockout_leg_print(const char *what, const struct lockout_leg *g)
{
	char list[160];
	unsigned lockout = ring_census_lockout_get(&g->c, false, list,
						   sizeof(list));

	printf("# %s: %u far frames, %u leak frames, out_sum %.9g, out_max "
	       "%.9g, %u lockout seconds %s; first trusted frame %u, %u "
	       "trials, %u resets\n", what, g->c.far.frames, g->c.far.leak,
	       g->c.far.out_sum, g->c.far.out_max, lockout, list,
	       g->first_trust, g->end.probes, g->end.diverge_resets);
	trial_marks_print(what, &g->marks);
}

/*
 * The INSIDE or the REFUSED leg, then the same leg with a dense talker at
 * -25 dBm0. The talker starts 5 s after the first word and speaks for 10 s.
 * A leg on a field path has no bar: the INSIDE form at 16 kHz. There the
 * filter holds the energy rule through 200 or more clear ringback frames, so
 * the count completes inside the ringback. The trust survives the answer gap,
 * and no restart comes before the first word. The census there is the same
 * with and without the trust count: 802 leak frames, 24 lockout seconds and
 * the first trusted frame 301.
 */
static void lockout_words_leg(unsigned rate, enum ring_form form,
			      const char *what, struct lockout_leg *g)
{
	struct ring_spec s;
	char talk[64];

	ring_spec_set(&s, form, rate);
	lockout_leg_run(&s, g);
	if (s.field)
		lockout_leg_print(what, g);
	else
		lockout_leg_check(what, &s, form, g);
	s.talk_from = s.answer + LOCKOUT_TALK_AFTER;
	s.talk_to = s.talk_from + LOCKOUT_TALK_FRAMES;
	lockout_leg_run(&s, g);
	snprintf(talk, sizeof(talk), "%s with a talker", what);
	lockout_speech_print(talk, &g->speech);
	lockout_speech_check(&g->speech);
}

/* The BACKGROUND leg. The count restarts after the ringback, and the trust
 * must stay withheld from that frame through the frame that completes the
 * count. The census of the output is printed beside the bars. */
static void lockout_background_leg(unsigned rate, struct lockout_leg *g)
{
	struct ring_spec s;
	char list[160];
	unsigned lockout;

	ring_spec_set(&s, RING_FORM_BACKGROUND, rate);
	lockout_leg_run(&s, g);
	lockout = ring_census_lockout_get(&g->c, false, list, sizeof(list));
	lockout_leg_check("background leg", &s, RING_FORM_BACKGROUND, g);
	printf("# background leg, census of the output: %u far frames, %u "
	       "leak frames, out_sum %.9g, out_max %.9g, %u lockout seconds "
	       "%s; count restart on frame %u, complete on frame %u, %u "
	       "trusted frames between\n", g->c.far.frames, g->c.far.leak,
	       g->c.far.out_sum, g->c.far.out_max, lockout, list, g->restart,
	       g->complete, g->early);
	lockout_speech_print("background leg", &g->speech);
	CHECK(g->restart > 0u);
	CHECK(g->complete > g->restart);
	CHECK_UINT(g->early, 0);
	lockout_speech_check(&g->speech);
}

/* The INSIDE form with its first word 100 frames after the ringback, printed
 * with no bar. */
static void lockout_short_leg(unsigned rate, struct lockout_leg *g)
{
	struct ring_spec s;

	ring_spec_set(&s, RING_FORM_INSIDE, rate);
	s.answer = s.ring_frames + LOCKOUT_SHORT_GAP;
	lockout_leg_run(&s, g);
	lockout_leg_print("short gap leg", g);
}

/*
 * The lockout after the ringback ends without a trial, on the three forms of
 * ring_spec_set, each run for 30 s with the processor on. Each leg must keep
 * the bars of lockout_leg_check. The INSIDE leg at 16 kHz runs on the field
 * path and is printed with no bar (lockout_words_leg). The INSIDE and REFUSED
 * legs run again with a near talker, whose speech must pass within 2 dB with
 * no exposure frame. The INSIDE form with a gap of 1 s is printed with no
 * bar. A canceller without the trust count fails each leg that has a bar.
 */
static void ring_lockout_test(unsigned rate)
{
	static struct lockout_leg g;

	case_rate(rate, "lec ends the lockout after the ringback without a "
		  "trial");
	lockout_words_leg(rate, RING_FORM_INSIDE, "inside leg", &g);
	lockout_words_leg(rate, RING_FORM_REFUSED, "refused leg", &g);
	lockout_background_leg(rate, &g);
	lockout_short_leg(rate, &g);
}

#define RESTART_EXTRA_FRAMES	300u
#define RESTART_TALK_FROM	100u
#define RESTART_TALK_FRAMES	10u
#define RESTART_TALK_OVER_DB	10.0
#define RESTART_SILENT_FROM	200u
#define RESTART_SILENT_FRAMES	50u
#define RESTART_LIMIT_FRAMES	(10u * FRAMES_PER_SECOND)
#define RESTART_BEFORE_STEP	50u
#define RESTART_BEFORE_PAUSE	50u
#define RESTART_STEP_DB		12.0
#define RESTART_TRIAL_FRAMES	30u
#define RESTART_RETURN_TALK	10u

/* One frame of the ring line. far adds the white far end, and talk adds a
 * white talker talk_db over the echo. */
static void restart_frame_run(struct ring_line *line, bool far, bool talk,
			      double talk_db,
			      struct udsp_ec_nlms_frame *reading,
			      struct udsp_ec_nlms_info *info)
{
	int16_t ref[UDSP_FRAME_MAX];
	int16_t near[UDSP_FRAME_MAX];

	memset(ref, 0, sizeof(ref));
	if (far)
		noise_fill(ref, line->frame, RING_WORDS_DBM0, &line->far_state);
	if (talk)
		noise_fill(near, line->frame, TALKER_ECHO_DBM0 + talk_db,
			   &line->near_state);
	ring_frame_run(line, ref, talk ? near : NULL, NULL);
	udsp_ec_nlms_frame_get(line->ec, reading);
	udsp_ec_nlms_info_get(line->ec, info);
}

/*
 * A far-end pause whose quiet-window run reaches quiet frames, then the frame
 * that the far end returns on. The window turns quiet when the last loud
 * frame leaves it, so the pause holds quiet plus the window less 1 frames.
 */
static void restart_pause_run(struct ring_line *line, unsigned quiet,
			      struct udsp_ec_nlms_frame *ret)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	unsigned silent = quiet + window_frames_get(line->ec) - 1u;
	unsigned f;

	for (f = 0; f < silent; f++)
		restart_frame_run(line, false, false, 0.0, &reading, &info);
	restart_frame_run(line, true, false, 0.0, ret, &info);
}

/*
 * The white far end until the energy rule of dsp/lec.c turns held, at most
 * limit frames. The rule of frame k compares the coefficient energy of info
 * after frame k-1, times 10, with the learned gain after frame k. It returns
 * the frames that ran, or 0 when the rule did not turn, and at the reading of
 * the frame on which it turned.
 */
static unsigned restart_turn_run(struct ring_line *line, unsigned limit,
				 struct udsp_ec_nlms_frame *at)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct udsp_ec_nlms_info before;
	bool held = false;
	unsigned f;

	udsp_ec_nlms_info_get(line->ec, &before);
	for (f = 1; f <= limit; f++) {
		bool rule;

		restart_frame_run(line, true, false, 0.0, &reading, &info);
		rule = before.coef_energy * RING_COEF_RATIO >= info.echo_gain;
		before = info;
		if (rule && !held) {
			*at = reading;
			return f;
		}
		held = rule;
	}
	return 0;
}

/*
 * The white far end until the first trusted frame, at most limit frames. The
 * frames count from 1:
 * - count_at takes the first frame that reads a count of TRUST_FRAMES;
 * - first takes the first frame that reads trusted;
 * - early counts the trusted frames with a count of TRUST_FRAMES or less.
 */
struct restart_complete {
	unsigned count_at;
	unsigned first;
	unsigned early;
};

static void restart_complete_run(struct ring_line *line, unsigned limit,
				 struct restart_complete *c)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	unsigned f;

	memset(c, 0, sizeof(*c));
	for (f = 1; f <= limit && !c->first; f++) {
		restart_frame_run(line, true, false, 0.0, &reading, &info);
		if (!c->count_at && reading.trust_run == TRUST_FRAMES)
			c->count_at = f;
		if (reading.trusted && reading.trust_run <= TRUST_FRAMES)
			c->early++;
		if (reading.trusted)
			c->first = f;
	}
}

/* Part 1: the count moves by 1 on a clear active frame and stays on every
 * other frame. A talker burst and a far-end pause give the other frames. */
static void trust_count_part(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;
	struct ring_line line;
	unsigned miss = 0;
	unsigned fired = 0;
	unsigned held = 0;
	unsigned quiet = 0;
	unsigned before;
	unsigned f;

	ring_line_init(&line, rate);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES);
	udsp_ec_nlms_frame_get(line.ec, &reading);
	before = reading.trust_run;
	for (f = 0; f < RESTART_EXTRA_FRAMES; f++) {
		bool talk = f >= RESTART_TALK_FROM &&
			    f < RESTART_TALK_FROM + RESTART_TALK_FRAMES;
		bool far = f < RESTART_SILENT_FROM ||
			   f >= RESTART_SILENT_FROM + RESTART_SILENT_FRAMES;
		bool active;

		restart_frame_run(&line, far, talk, RESTART_TALK_OVER_DB,
				  &reading, &info);
		active = reading.ref_power >= reading.ref_bar;
		miss += reading.trust_run !=
			before + (active && !info.double_talk);
		fired += reading.dt_fired;
		held += active && info.double_talk;
		quiet += !active;
		before = reading.trust_run;
	}
	udsp_ec_nlms.destroy(line.ec);
	printf("# count: %u frames off the rule, %u fired, %u held active "
	       "frames, %u inactive frames, count %u at the end\n", miss, fired,
	       held, quiet, before);
	CHECK_UINT(miss, 0);
	CHECK(fired > 0u);
	CHECK(held > 0u);
	CHECK(quiet > 0u);
	CHECK(before > TRUST_FRAMES);
}

/*
 * Parts 2 and 3: a pause of quiet quiet-window frames, RESTART_BEFORE_PAUSE
 * frames after the turn of the rule and before the first trust. A restart
 * then shows as a fall of the count. Part 3 then completes the count.
 */
static void trust_pause_part(unsigned rate, unsigned quiet)
{
	struct udsp_ec_nlms_frame turn;
	struct udsp_ec_nlms_frame before;
	struct udsp_ec_nlms_frame ret;
	struct udsp_ec_nlms_info info;
	struct restart_complete c;
	struct ring_line line;
	unsigned turned;
	unsigned f;

	memset(&turn, 0, sizeof(turn));
	memset(&before, 0, sizeof(before));
	ring_line_init(&line, rate);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES);
	udsp_ec_nlms_reset(line.ec);
	turned = restart_turn_run(&line, RESTART_LIMIT_FRAMES, &turn);
	for (f = 0; f < RESTART_BEFORE_PAUSE; f++)
		restart_frame_run(&line, true, false, 0.0, &before, &info);
	restart_pause_run(&line, quiet, &ret);
	printf("# pause of %u quiet frames: rule turned on frame %u, count %u "
	       "before the pause and %u on the return\n", quiet, turned,
	       before.trust_run, ret.trust_run);
	CHECK(turned > 0u);
	CHECK(before.trust_run > 1u && before.trust_run < TRUST_FRAMES);
	CHECK(!before.trusted);
	if (quiet < TRUST_PAUSE_FRAMES) {
		CHECK(ret.trust_run >= before.trust_run &&
		      ret.trust_run <= before.trust_run + 1u);
		udsp_ec_nlms.destroy(line.ec);
		return;
	}
	CHECK(ret.trust_run <= 1u);
	CHECK(!ret.trusted);
	restart_complete_run(&line, RESTART_LIMIT_FRAMES, &c);
	printf("# count of %u on frame %u after the return, first trusted "
	       "frame %u, %u trusted frames with the count incomplete\n",
	       TRUST_FRAMES, c.count_at, c.first, c.early);
	CHECK(c.count_at > 0u);
	CHECK_UINT(c.first, c.count_at + 1u);
	CHECK_UINT(c.early, 0);
	udsp_ec_nlms.destroy(line.ec);
}

/* Part 4: a pause after the trust held restarts nothing. */
static void trust_held_part(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_frame ret;
	struct ring_line line;

	ring_line_init(&line, rate);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES);
	udsp_ec_nlms_frame_get(line.ec, &reading);
	restart_pause_run(&line, TRUST_PAUSE_FRAMES, &ret);
	udsp_ec_nlms.destroy(line.ec);
	printf("# pause after the trust: count %u before the pause, trusted "
	       "%u; return frame: count %u, trusted %u\n", reading.trust_run,
	       reading.trusted, ret.trust_run, ret.trusted);
	CHECK(reading.trusted);
	CHECK(reading.trust_run > TRUST_FRAMES);
	CHECK(ret.trust_run >= reading.trust_run &&
	      ret.trust_run <= reading.trust_run + 1u);
	CHECK(ret.trusted);
}

/* Part 5: the reset and the turn of the rule restart the count. */
static void trust_reset_part(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_frame turn;
	struct udsp_ec_nlms_info info;
	struct restart_complete c;
	struct ring_line line;
	unsigned turned;

	memset(&turn, 0, sizeof(turn));
	ring_line_init(&line, rate);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES);
	udsp_ec_nlms_reset(line.ec);
	restart_frame_run(&line, true, false, 0.0, &reading, &info);
	turned = restart_turn_run(&line, RESTART_LIMIT_FRAMES, &turn);
	restart_complete_run(&line, RESTART_LIMIT_FRAMES, &c);
	udsp_ec_nlms.destroy(line.ec);
	printf("# reset: count %u on the first frame; rule turned on frame %u "
	       "after it with a count of %u; count of %u on frame %u after the "
	       "turn, first trusted frame %u\n", reading.trust_run, turned + 1u,
	       turn.trust_run, TRUST_FRAMES, c.count_at, c.first);
	CHECK(reading.trust_run <= 1u);
	CHECK(turned > 0u);
	CHECK(turn.trust_run <= 1u);
	CHECK(c.count_at > 0u);
	CHECK_UINT(c.first, c.count_at + 1u);
	CHECK_UINT(c.early, 0);
}

/*
 * Part 6: a restart inside a refused trial. After the turn of the rule, the
 * hybrid steps up by 12 dB, and the classic bound holds the louder echo until
 * a trial starts. A pause of 2 s falls inside the trial. On the tenth frame
 * of the return, a talker 16 dB over the far end ends the trial.
 */
static void trust_reject_part(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_frame turn;
	struct udsp_ec_nlms_frame ret;
	struct udsp_ec_nlms_info info;
	struct ring_line line;
	unsigned trusted = 0;
	unsigned start = 0;
	unsigned arm = 0;
	unsigned snap;
	unsigned f;

	memset(&turn, 0, sizeof(turn));
	ring_line_init(&line, rate);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES);
	udsp_ec_nlms_reset(line.ec);
	(void)restart_turn_run(&line, RESTART_LIMIT_FRAMES, &turn);
	for (f = 0; f < RESTART_BEFORE_STEP; f++)
		restart_frame_run(&line, true, false, 0.0, &reading, &info);
	snap = reading.trust_run;
	path_erl_set(&line.path, RING_ERL_DB - RESTART_STEP_DB);
	for (f = 1; f <= RESTART_LIMIT_FRAMES && !start; f++) {
		restart_frame_run(&line, true, false, 0.0, &reading, &info);
		trusted += reading.trusted;
		if (!info.probes)
			continue;
		start = f;
		arm = reading.trial_arm;
		snap = reading.trust_run;
	}
	for (f = 0; f < RESTART_TRIAL_FRAMES; f++)
		restart_frame_run(&line, true, false, 0.0, &reading, &info);
	restart_pause_run(&line, TRUST_PAUSE_FRAMES, &ret);
	for (f = 2; f < RESTART_RETURN_TALK; f++)
		restart_frame_run(&line, true, false, 0.0, &reading, &info);
	restart_frame_run(&line, true, true, RING_ERL_DB + TALKER_OVER_DB,
			  &reading, &info);
	restart_frame_run(&line, true, false, 0.0, &ret, &info);
	udsp_ec_nlms.destroy(line.ec);
	printf("# trial after the step: %s trial on frame %u, count %u at its "
	       "start, %u trusted frames before it; talker end %s, count %u "
	       "on the next frame\n", lecr_trial_arm_name(arm), start, snap,
	       trusted, lecr_trial_end_name(reading.trial_end), ret.trust_run);
	CHECK(start > 0u);
	CHECK_UINT(trusted, 0);
	CHECK(snap > RESTART_RETURN_TALK);
	CHECK_UINT(reading.trial_end, UDSP_EC_NLMS_TRIAL_END_TALKER);
	CHECK(ret.trust_run <= RESTART_RETURN_TALK);
}

/* A far-end pause of TRUST_PAUSE_FRAMES quiet-window frames on the hybrid
 * that amplifies, then the frame that the far end returns on. */
static void trust_refused_pause_run(struct boundary_line *b,
				    struct udsp_ec_nlms_frame *ret)
{
	struct udsp_ec_nlms_info info;
	int16_t ref[UDSP_FRAME_MAX];
	unsigned window;
	unsigned f;

	udsp_ec_nlms_info_get(b->lead, &info);
	window = (info.taps + b->frame - 1u) / b->frame;
	memset(ref, 0, sizeof(ref));
	for (f = 0; f < TRUST_PAUSE_FRAMES + window; f++)
		twin_step(b->lead, b->twin, &b->amp, ref, NULL, b->lead_out,
			  b->twin_out, NULL);
	(void)boundary_amp_run(b, NULL);
	udsp_ec_nlms_frame_get(b->lead, ret);
}

/*
 * Part 7: a refused trial with clear frames, on the line of
 * probe_boundary_test. The trial spends its budget on the far end alone, and
 * the watchdog refuses it on its last frame. That frame must read the count of
 * the frame the trial started on. The trust holds inside the trial only, so a
 * pause of 2 s after the refusal must restart the count.
 */
static void trust_refused_part(unsigned rate)
{
	struct udsp_ec_nlms_frame start;
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_frame ret;
	struct udsp_ec_nlms_info info;
	struct boundary_line b;
	unsigned amp_frames = boundary_amp_count(rate);
	unsigned trusted = 0;
	unsigned spent = 0;
	unsigned most = 0;
	unsigned f;

	boundary_line_init(&b, rate);
	boundary_trial_start(&b, &info);
	udsp_ec_nlms_frame_get(b.lead, &start);
	while (spent + amp_frames < TRIAL_BUDGET) {
		boundary_weak_run(&b, false);
		udsp_ec_nlms_frame_get(b.lead, &reading);
		if (reading.trust_run > most)
			most = reading.trust_run;
		trusted += reading.trusted;
		spent++;
	}
	boundary_flush_run(&b);
	for (f = 1; f <= amp_frames; f++)
		(void)boundary_amp_run(&b, NULL);
	udsp_ec_nlms_frame_get(b.lead, &reading);
	trust_refused_pause_run(&b, &ret);
	udsp_ec_nlms.destroy(b.lead);
	udsp_ec_nlms.destroy(b.twin);
	printf("# refused trial: count %u on its start frame, %u at most on "
	       "its frames, %u on the frame the watchdog ends it; %u trusted "
	       "frames in the trial; count %u on the return after a pause\n",
	       start.trust_run, most, reading.trust_run, trusted,
	       ret.trust_run);
	CHECK(amp_frames > 1u);
	CHECK_UINT(reading.trial_end, UDSP_EC_NLMS_TRIAL_END_DIVERGED);
	CHECK(most > start.trust_run);
	CHECK_UINT(reading.trust_run, start.trust_run);
	CHECK(!start.trusted);
	CHECK(trusted > 0u);
	CHECK(ret.trust_run <= 1u);
}

#define RESTART_FREEZE_FRAMES	50u

/* Part 8: a frozen frame counts. The freeze holds the filter, and a clear
 * active frame under it still moves the tracker and the count. */
static void trust_freeze_part(unsigned rate)
{
	struct udsp_ec_nlms_frame before;
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info start;
	struct udsp_ec_nlms_info info;
	struct ring_line line;
	unsigned clear = 0;
	unsigned f;

	ring_line_init(&line, rate);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES);
	udsp_ec_nlms_frame_get(line.ec, &before);
	udsp_ec_nlms_info_get(line.ec, &start);
	udsp_ec_nlms.freeze(line.ec, true);
	for (f = 0; f < RESTART_FREEZE_FRAMES; f++) {
		restart_frame_run(&line, true, false, 0.0, &reading, &info);
		clear += reading.ref_power >= reading.ref_bar &&
			 !info.double_talk;
	}
	udsp_ec_nlms.destroy(line.ec);
	printf("# freeze: count %u before and %u after %u frozen frames, %u of "
	       "them clear, %u held\n", before.trust_run, reading.trust_run,
	       RESTART_FREEZE_FRAMES, clear,
	       info.hold_frames - start.hold_frames);
	CHECK(clear > 0u);
	CHECK_UINT(info.hold_frames - start.hold_frames, RESTART_FREEZE_FRAMES);
	CHECK_UINT(reading.trust_run, before.trust_run + clear);
}

#define RESTART_BG_DBM0		-48.0
#define RESTART_BG_FLOOR_DBM0	-85.0
#define RESTART_BG_SEED		0x5eed1234u
#define RESTART_BG_LEAD		400u
#define RESTART_BG_TALK_DBM0	-30.0
#define RESTART_BG_RUN		497u
#define RESTART_BG_TAIL		6u
#define RESTART_BG_PAUSE	260u

/* One frame of the ring line over a white far-end background. far adds the
 * white far end, and talk adds a white talker at talk_dbm0. */
static void restart_bg_frame_run(struct ring_line *line, uint32_t *bg_state,
				 bool far, bool talk, double talk_dbm0,
				 struct udsp_ec_nlms_frame *reading,
				 struct udsp_ec_nlms_info *info)
{
	int16_t ref[UDSP_FRAME_MAX];
	int16_t part[UDSP_FRAME_MAX];
	int16_t near[UDSP_FRAME_MAX];

	noise_fill(ref, line->frame, RESTART_BG_DBM0, bg_state);
	if (far) {
		noise_fill(part, line->frame, RING_WORDS_DBM0,
			   &line->far_state);
		ring_part_add(ref, part, line->frame);
	}
	if (talk)
		noise_fill(near, line->frame, talk_dbm0, &line->near_state);
	ring_frame_run(line, ref, talk ? near : NULL, NULL);
	udsp_ec_nlms_frame_get(line->ec, reading);
	udsp_ec_nlms_info_get(line->ec, info);
}

/*
 * Part 9: a restart and a trial on the same frame, before the first trust. A
 * talker between the bars drives the latch run to the latch on the last
 * frames of a far phrase, where no arm runs. A far background under the
 * raised bar then holds the run through a pause of 2.6 s. The far return
 * restarts the count, and the latch starts a trial on that frame. A talker
 * 16 dB over the far end ends the trial on its tenth frame. The count must
 * then read 0, because the trial keeps no count from before the pause.
 */
static void trust_return_trial_part(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_frame ret;
	struct udsp_ec_nlms_info info;
	struct ring_line line;
	uint32_t bg_state = RESTART_BG_SEED;
	uint32_t probes;
	unsigned trusted = 0;
	unsigned before;
	unsigned run;
	unsigned end;
	unsigned f;

	ring_line_init(&line, rate);
	line.floor_dbm0 = RESTART_BG_FLOOR_DBM0;
	for (f = 0; f < RESTART_BG_LEAD; f++) {
		restart_bg_frame_run(&line, &bg_state, false, false, 0.0,
				     &reading, &info);
		trusted += reading.trusted;
	}
	for (f = 0; f < RESTART_LIMIT_FRAMES &&
		    reading.dt_run < RESTART_BG_RUN; f++) {
		restart_bg_frame_run(&line, &bg_state, true, true,
				     RESTART_BG_TALK_DBM0, &reading, &info);
		trusted += reading.trusted;
	}
	for (f = 0; f < RESTART_BG_TAIL; f++)
		restart_bg_frame_run(&line, &bg_state, false, true,
				     RESTART_BG_TALK_DBM0, &reading, &info);
	for (f = 0; f < RESTART_BG_PAUSE; f++)
		restart_bg_frame_run(&line, &bg_state, false, false, 0.0,
				     &reading, &info);
	before = reading.trust_run;
	run = reading.dt_run;
	probes = info.probes;
	restart_bg_frame_run(&line, &bg_state, true, false, 0.0, &ret, &info);
	for (f = 2; f < RESTART_RETURN_TALK; f++)
		restart_bg_frame_run(&line, &bg_state, true, false, 0.0,
				     &reading, &info);
	restart_bg_frame_run(&line, &bg_state, true, true,
			     RING_WORDS_DBM0 + TALKER_OVER_DB, &reading, &info);
	end = reading.trial_end;
	restart_bg_frame_run(&line, &bg_state, true, false, 0.0, &reading,
			     &info);
	udsp_ec_nlms.destroy(line.ec);
	printf("# trial on the return: count %u and latch run %u before the "
	       "return, %u trusted frames before it; return frame: %s trial, "
	       "count %u; talker end %s, count %u on the next frame\n", before,
	       run, trusted, lecr_trial_arm_name(ret.trial_arm), ret.trust_run,
	       lecr_trial_end_name(end), reading.trust_run);
	CHECK_UINT(trusted, 0);
	CHECK(before > RESTART_RETURN_TALK);
	CHECK(run >= STEP_LATCH_FRAMES);
	CHECK_UINT(probes, 0);
	CHECK_UINT(ret.trial_arm, UDSP_EC_NLMS_TRIAL_ARM_LATCH);
	CHECK(ret.trust_run <= 1u);
	CHECK_UINT(end, UDSP_EC_NLMS_TRIAL_END_TALKER);
	CHECK_UINT(reading.trust_run, 0);
}

#define RESTART_LATCH_TALK_DB	14.0
#define RESTART_DROP_DB		30.0
#define RESTART_DROP_FRAMES	40u

/*
 * Part 10: a refused trial in which the energy rule turns. On the converged
 * ring line, a talker between the bars drives the latch to a trial. On the
 * start frame the hybrid falls by 30 dB and the talker stops. The trial
 * shrinks the filter, and the rule refuses on its last frame. A talker 16 dB
 * over the far end then ends the trial. The refusal gives back the filter,
 * the gain and the rule of the start frame, so the count must not restart.
 */
static void trust_rule_part(unsigned rate)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_frame start;
	struct udsp_ec_nlms_info info;
	struct udsp_ec_nlms_info prev;
	struct ring_line line;
	bool rule = true;
	unsigned f;

	memset(&start, 0, sizeof(start));
	ring_line_init(&line, rate);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES);
	udsp_ec_nlms_info_get(line.ec, &info);
	for (f = 0; f < RESTART_LIMIT_FRAMES && !info.probes; f++)
		restart_frame_run(&line, true, true, RESTART_LATCH_TALK_DB,
				  &start, &info);
	path_erl_set(&line.path, RING_ERL_DB + RESTART_DROP_DB);
	prev = info;
	for (f = 0; f < RESTART_DROP_FRAMES; f++) {
		restart_frame_run(&line, true, false, 0.0, &reading, &info);
		rule = prev.coef_energy * RING_COEF_RATIO >= info.echo_gain;
		prev = info;
	}
	restart_frame_run(&line, true, true, RING_ERL_DB + TALKER_OVER_DB,
			  &reading, &info);
	udsp_ec_nlms.destroy(line.ec);
	printf("# rule turn in a trial: %s trial with count %u, trusted %u; "
	       "rule %u on its last frame; talker end %s, count %u, trusted "
	       "%u\n", lecr_trial_arm_name(start.trial_arm), start.trust_run,
	       start.trusted, rule, lecr_trial_end_name(reading.trial_end),
	       reading.trust_run, reading.trusted);
	CHECK_UINT(start.trial_arm, UDSP_EC_NLMS_TRIAL_ARM_LATCH);
	CHECK(start.trusted);
	CHECK(!rule);
	CHECK_UINT(reading.trial_end, UDSP_EC_NLMS_TRIAL_END_TALKER);
	CHECK_UINT(reading.trust_run, start.trust_run);
	CHECK(reading.trusted);
}

/*
 * The trust count of dsp/lec.c on the ring line, in ten parts. A frame must
 * count when its own reference is active and it ends without double talk,
 * also under a freeze. A pause of 200 quiet-window frames before the first
 * trust must restart the count, and one of 199 must not. A pause after the
 * trust held, the reset and the turn of the energy rule are checked too. A
 * refused trial must keep the smaller of its start count and the count of the
 * frame, also on the frame of a restart. It must also give back the energy
 * rule and the held trust of its start frame.
 */
static void trust_restart_test(unsigned rate)
{
	case_rate(rate, "lec restarts the trust count at a far return, a rule "
		  "turn and a reset");
	trust_count_part(rate);
	trust_pause_part(rate, TRUST_PAUSE_FRAMES - 1u);
	trust_pause_part(rate, TRUST_PAUSE_FRAMES);
	trust_held_part(rate);
	trust_reset_part(rate);
	trust_reject_part(rate);
	trust_refused_part(rate);
	trust_freeze_part(rate);
	trust_return_trial_part(rate);
	trust_rule_part(rate);
}

/* The line of the snapshot cases: a flat 12 dB hybrid at 8 ms, a white far
 * end at SNAP_FAR_DBM0 and no near floor. It trains for 1.5 s. */
#define WINDOW_ERL_DB		12.0
#define WINDOW_TRAIN_FRAMES	(FRAMES_PER_SECOND + FRAMES_PER_SECOND / 2)
#define WINDOW_TALK_FRAMES	150u
#define WINDOW_AFTER_FRAMES	(2u * FRAMES_PER_SECOND)
#define WINDOW_ERLE_FRAMES	FRAMES_PER_SECOND
/* The talker frames that adapt on a canceller without the trust count. */
#define WINDOW_HEAD_ADAPTED	0u

/*
 * The figures of trust_window_talker_test. The energies give the filter
 * ERLE over the last second of the training and over the second that ends
 * 2 s after the talker stops. first_trust counts the frames from the return
 * to the first trusted one. early counts the trusted frames from the return
 * through the frame that completes the count.
 */
struct window_talk {
	struct near_stat speech;
	unsigned adapted;
	unsigned trained_trust;
	unsigned count_at_pause;
	unsigned return_count;
	unsigned first_trust;
	unsigned early;
	bool complete;
	double echo_dbm0;
	double before_echo;
	double before_out;
	double after_echo;
	double after_out;
};

/* One frame of the snapshot line, with near on top when it is given. */
static void window_frame_run(struct ring_line *line, bool far,
			     const int16_t *near, struct ring_planes *p)
{
	int16_t ref[UDSP_FRAME_MAX];

	memset(ref, 0, sizeof(ref));
	if (far)
		noise_fill(ref, line->frame, SNAP_FAR_DBM0, &line->far_state);
	ring_planes_run(line, ref, near, NULL, p);
}

static void window_energy_add(const struct ring_planes *p, unsigned frame,
			      double *echo, double *out)
{
	unsigned i;

	for (i = 0; i < frame; i++) {
		*echo += (double)p->echo[i] * p->echo[i];
		*out += (double)p->out[i] * p->out[i];
	}
}

/* The trust of frame k after the return, which reads the count of the frame
 * before it. */
static void window_trust_note(struct window_talk *w,
			      const struct ring_line *line, unsigned k)
{
	struct udsp_ec_nlms_frame reading;

	udsp_ec_nlms_frame_get(line->ec, &reading);
	if (k == 1u)
		w->return_count = reading.trust_run;
	if (!w->first_trust && reading.trusted)
		w->first_trust = k;
	if (w->complete)
		return;
	w->early += reading.trusted;
	w->complete = reading.trust_run >= TRUST_FRAMES;
}

/* The talker frames of the return, at the level of the echo. */
static void window_talk_run(struct window_talk *w, struct ring_line *line,
			    unsigned *k)
{
	struct udsp_ec_nlms_info info;
	int16_t near[UDSP_FRAME_MAX];
	struct ring_planes p;
	unsigned f;

	for (f = 0; f < WINDOW_TALK_FRAMES; f++) {
		uint32_t held = line->held;

		noise_fill(near, line->frame, w->echo_dbm0, &line->near_state);
		window_frame_run(line, true, near, &p);
		near_stat_add(&w->speech, line->ec, p.near, p.mic, p.out,
			      line->frame);
		udsp_ec_nlms_info_get(line->ec, &info);
		w->adapted += info.hold_frames == held;
		window_trust_note(w, line, ++*k);
	}
}

static void window_run(unsigned rate, struct window_talk *w)
{
	struct udsp_ec_nlms_frame reading;
	struct ring_planes p;
	struct ring_line line;
	unsigned window;
	unsigned k = 0;
	unsigned f;

	memset(w, 0, sizeof(*w));
	ring_line_init(&line, rate);
	path_flat_init(&line.path, rate, 8, WINDOW_ERL_DB);
	line.floor = false;
	window = window_frames_get(line.ec);
	for (f = 0; f < WINDOW_TRAIN_FRAMES; f++) {
		window_frame_run(&line, true, NULL, &p);
		udsp_ec_nlms_frame_get(line.ec, &reading);
		w->trained_trust += reading.trusted;
		if (f + WINDOW_ERLE_FRAMES >= WINDOW_TRAIN_FRAMES)
			window_energy_add(&p, line.frame, &w->before_echo,
					  &w->before_out);
	}
	w->count_at_pause = reading.trust_run;
	w->echo_dbm0 = energy_dbm0_get(w->before_echo,
				       WINDOW_ERLE_FRAMES * line.frame);
	for (f = 0; f < TRUST_PAUSE_FRAMES + window - 1u; f++)
		window_frame_run(&line, false, NULL, &p);
	window_talk_run(w, &line, &k);
	for (f = 0; f < WINDOW_AFTER_FRAMES; f++) {
		window_frame_run(&line, true, NULL, &p);
		window_trust_note(w, &line, ++k);
		if (f + WINDOW_ERLE_FRAMES >= WINDOW_AFTER_FRAMES)
			window_energy_add(&p, line.frame, &w->after_echo,
					  &w->after_out);
	}
	udsp_ec_nlms.destroy(line.ec);
}

/*
 * A near talker at the echo level while the trust count is open. The line of
 * the snapshot cases trains for 1.5 s, under the count, and pauses for 200
 * quiet-window frames. A white talker at the level of the echo joins the
 * return for 150 frames. The trust must stay withheld from the return through
 * the frame that completes the count. Without the count, the line holds every
 * talker frame (WINDOW_HEAD_ADAPTED).
 */
static void trust_window_talker_test(unsigned rate)
{
	struct window_talk w;
	const struct near_stat *sp = &w.speech;

	window_run(rate, &w);

	case_rate(rate, "lec reports a near talker at the echo level while the "
		  "trust count is open");
	printf("# count %u at the pause, %u trusted frames before it; talker "
	       "at %.2f dBm0: %u of %u frames adapted, against %u before the "
	       "trust count; near speech %.2f dB through; filter ERLE %.2f dB "
	       "before the pause and %.2f dB 2 s after the talker; first "
	       "trusted frame %u after the return\n", w.count_at_pause,
	       w.trained_trust, w.echo_dbm0, w.adapted, WINDOW_TALK_FRAMES,
	       WINDOW_HEAD_ADAPTED, near_gain_db_get(sp->product, sp->near),
	       expose_db_get(w.before_echo, w.before_out),
	       expose_db_get(w.after_echo, w.after_out), w.first_trust);
	CHECK(w.count_at_pause < TRUST_FRAMES);
	CHECK_UINT(w.trained_trust, 0);
	CHECK(w.return_count <= 1u);
	CHECK(w.complete);
	CHECK_UINT(w.early, 0);
}

/*
 * The form of trust_turn_test. A white talker in a far pause is stationary,
 * so the tracked floor rises to it. The bar then covers that talker at the
 * return, and the trust lapses there with or without the trust count. A word
 * talker leaves the floor. With far words of 3 s, a canceller without the
 * trust count loses the trust at 8 kHz from the sixth return on.
 */
#define TURN_CYCLES		10u
#define TURN_CYCLE_FRAMES	700u
#define TURN_FAR_FRAMES		400u
#define TURN_OVERLAP_FRAMES	150u
#define TURN_OVER_DB		6.0
#define TURN_ERLE_TOL_DB	1.0
#define TURN_TALKER_SEED	0x31415926u

/*
 * The figures of a canceller without the trust count for trust_turn_test. Each
 * row holds one rate and one talker level: the echo level, or 6 dB over it.
 * They give the mean filter ERLE and the adapted talker frames of the far
 * returns.
 */
struct turn_head {
	unsigned rate;
	double over_db;
	double erle_db;
	unsigned adapted;
};

static const struct turn_head turn_heads[] = {
	{ UDSP_SAMPLE_RATE, 0.0, 28.38, 174u },
	{ UDSP_SAMPLE_RATE, TURN_OVER_DB, 33.15, 125u },
	{ UDSP_SAMPLE_RATE_WB, 0.0, 28.47, 200u },
	{ UDSP_SAMPLE_RATE_WB, TURN_OVER_DB, 32.79, 125u },
};

static const struct turn_head *turn_head_get(unsigned rate, double over_db)
{
	unsigned i;

	for (i = 0; i < sizeof(turn_heads) / sizeof(turn_heads[0]); i++) {
		if (turn_heads[i].rate == rate &&
		    turn_heads[i].over_db == over_db)
			return &turn_heads[i];
	}
	return NULL;
}

/*
 * The figures of trust_turn_test: the mean filter ERLE, and the lapses of
 * the count after it first passed TRUST_FRAMES. talk counts the talker
 * frames of the far returns whose own reference is active. trusted and
 * adapted count those of them that read trusted, and those that adapted.
 */
struct turn_result {
	double erle;
	unsigned lapses;
	unsigned talk;
	unsigned trusted;
	unsigned adapted;
	bool passed;
	uint32_t probes;
	uint32_t resets;
};

/* The talker frames of a far return, after the frame ran. */
static void turn_return_note(const struct ring_line *line, uint32_t held,
			     struct turn_result *r)
{
	struct udsp_ec_nlms_frame reading;
	struct udsp_ec_nlms_info info;

	udsp_ec_nlms_frame_get(line->ec, &reading);
	udsp_ec_nlms_info_get(line->ec, &info);
	if (!(reading.ref_power >= reading.ref_bar))
		return;
	r->talk++;
	r->trusted += reading.trusted;
	r->adapted += info.hold_frames == held;
}

/*
 * One cycle of trust_turn_test: far words for 4 s and a pause of 3 s. A word
 * talker over_db over the echo speaks for 1.5 s on each side of the return.
 */
static double turn_cycle_run(struct ring_line *line, struct word_source *talker,
			     struct turn_result *r)
{
	struct udsp_ec_nlms_frame reading;
	struct ring_stat st;
	unsigned f;

	memset(&st, 0, sizeof(st));
	for (f = 0; f < TURN_CYCLE_FRAMES; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t near[UDSP_FRAME_MAX];
		bool talk = f < TURN_OVERLAP_FRAMES ||
			    f >= TURN_CYCLE_FRAMES - TURN_OVERLAP_FRAMES;
		bool alone = f >= TURN_OVERLAP_FRAMES && f < TURN_FAR_FRAMES;
		uint32_t held = line->held;

		if (f == 0u)
			word_source_restart(&line->far);
		if (f == TURN_CYCLE_FRAMES - TURN_OVERLAP_FRAMES)
			word_source_restart(talker);
		memset(ref, 0, sizeof(ref));
		if (f < TURN_FAR_FRAMES)
			word_fill(&line->far, ref, line->frame);
		if (talk)
			word_fill(talker, near, line->frame);
		ring_frame_run(line, ref, talk ? near : NULL,
			       alone ? &st : NULL);
		if (f < TURN_OVERLAP_FRAMES)
			turn_return_note(line, held, r);
		udsp_ec_nlms_frame_get(line->ec, &reading);
		if (reading.trust_run > TRUST_FRAMES)
			r->passed = true;
		r->lapses += r->passed && reading.trust_run <= 1u;
	}
	return weak_filter_erle_get(st.echo, st.residual, st.floor);
}

static void turn_run(unsigned rate, double over_db, struct turn_result *r)
{
	struct udsp_ec_nlms_info info;
	struct word_source talker;
	struct ring_line line;
	double sum = 0.0;
	unsigned c;

	memset(r, 0, sizeof(*r));
	ring_line_init(&line, rate);
	word_source_init(&talker, rate, TALKER_ECHO_DBM0 + over_db,
			 TURN_TALKER_SEED);
	ring_white_run(&line, TALKER_CONVERGE_FRAMES);
	for (c = 0; c < TURN_CYCLES; c++)
		sum += turn_cycle_run(&line, &talker, r);
	udsp_ec_nlms_info_get(line.ec, &info);
	udsp_ec_nlms.destroy(line.ec);
	r->erle = sum / TURN_CYCLES;
	r->probes = info.probes;
	r->resets = info.diverge_resets;
}

/*
 * After 3 s of white noise, ten cycles of far words for 4 s and a far pause
 * of 3 s follow. A word talker overlaps each far return by 1.5 s, at the echo
 * level and 6 dB over it. On a canceller without the trust count, the trust
 * holds on most talker frames of the returns. Those frames must adapt no more
 * often than on that canceller (turn_heads). The ERLE on the far words alone
 * must stay within 1 dB of that canceller. After the count first passes 200,
 * it must never read 0 or 1.
 */
static void trust_turn_test(unsigned rate)
{
	static const double over[2] = { 0.0, TURN_OVER_DB };
	unsigned i;

	case_rate(rate, "lec keeps its trust through the far pauses of a "
		  "two-way call");
	for (i = 0; i < 2u; i++) {
		const struct turn_head *h = turn_head_get(rate, over[i]);
		struct turn_result r;

		turn_run(rate, over[i], &r);
		printf("# talker %.0f dB over the echo: filter ERLE %.2f dB "
		       "against %.2f dB; talker frames of the returns: %u "
		       "trusted and %u adapted of %u, against %u adapted; %u "
		       "lapses of the count, %u trials, %u resets\n", over[i],
		       r.erle, h ? h->erle_db : 0.0, r.trusted, r.adapted,
		       r.talk, h ? h->adapted : 0u, r.lapses, r.probes,
		       r.resets);
		CHECK(h != NULL);
		CHECK(r.passed);
		CHECK(r.talk > 0u);
		CHECK(h && fabs(r.erle - h->erle_db) <= TURN_ERLE_TOL_DB);
		CHECK(h && r.adapted <= h->adapted);
		CHECK_UINT(r.lapses, 0);
	}
}

/*
 * The reference loss of udsp_ec_nlms_ref_loss_set (dsp/lec.h). The cases
 * below run the loss of 18 half-dB, the 9 dB of the default SLIC receive
 * gain of uvoice. REF_LOSS_SEED is the learned gain that an instance starts
 * from under that loss: LEC_GEIGEL_GAIN_MIN of dsp/lec.c, 9 dB higher.
 */
#define REF_LOSS_HALF_DB	18u
#define REF_LOSS_DB		9.0
#define REF_LOSS_SEED		(0.25 * pow(10.0, REF_LOSS_DB / 10.0))
#define REF_LOSS_GATE_DB	33.0
#define REF_LOSS_ERLE_TOL_DB	1.0
#define REF_LOSS_SHARE_TOL	5.0
#define REF_LOSS_WORD_FRAMES	(30u * FRAMES_PER_SECOND)
#define REF_LOSS_WORD_ERL_DB	20.0
#define REF_LOSS_WORD_FLOOR_DBM0	-65.0

static bool ref_loss_seed_ok(float echo_gain, double seed)
{
	return fabs((double)echo_gain - seed) <= 1e-5 * seed;
}

/*
 * A fresh instance reads 0. The loss survives the reset, and the same loss
 * again leaves a running instance alone. A changed loss on an instance that
 * ran resets it, and each reset seeds the learned gain of the loss. A loss
 * set before the first frame seeds it with no reset.
 */
static void ref_loss_setter_test(unsigned rate)
{
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(0, rate);
	uint32_t state = 0x13572468u;

	case_rate(rate, "lec reference loss reads 0 fresh and survives the "
		  "reset");
	CHECK(ec != NULL);
	if (!ec)
		return;
	CHECK_UINT(udsp_ec_nlms_ref_loss_get(ec), 0);
	CHECK_UINT(udsp_ec_nlms_ref_loss_get(NULL), 0);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK(info.echo_gain == 0.25f);
	udsp_ec_nlms.nlp(ec, false);
	udsp_ec_nlms_ref_loss_set(ec, REF_LOSS_HALF_DB);
	path_flat_init(&path, rate, 8, 12.0);
	erle_offset_run(ec, &path, &state, -10.0, 0, REF_LOSS_HALF_DB,
			FRAMES_PER_SECOND, 1u);
	udsp_ec_nlms_ref_loss_set(ec, REF_LOSS_HALF_DB);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(info.frames, FRAMES_PER_SECOND);
	CHECK(info.coef_energy > 0.0f);
	udsp_ec_nlms_reset(ec);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(udsp_ec_nlms_ref_loss_get(ec), REF_LOSS_HALF_DB);
	CHECK_UINT(info.frames, 0);
	CHECK(ref_loss_seed_ok(info.echo_gain, REF_LOSS_SEED));
	erle_offset_run(ec, &path, &state, -10.0, 0, REF_LOSS_HALF_DB,
			4u * FRAMES_PER_SECOND, 1u);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK(info.ref_floor > 0.0f);
	/* The far-end floor stands in the old scale, so it starts over too. */
	udsp_ec_nlms_ref_loss_set(ec, 12u);
	udsp_ec_nlms_info_get(ec, &info);
	CHECK_UINT(udsp_ec_nlms_ref_loss_get(ec), 12);
	CHECK_UINT(info.frames, 0);
	CHECK(info.coef_energy == 0.0f);
	CHECK(info.ref_floor == 0.0f);
	CHECK(ref_loss_seed_ok(info.echo_gain, 0.25 * pow(10.0, 0.6)));
	udsp_ec_nlms.destroy(ec);

	case_rate(rate, "lec reference loss before the first frame seeds the "
		  "learned gain");
	ec = udsp_ec_nlms.create_rate(0, rate);
	CHECK(ec != NULL);
	if (!ec)
		return;
	udsp_ec_nlms_ref_loss_set(ec, REF_LOSS_HALF_DB);
	udsp_ec_nlms_info_get(ec, &info);
	printf("# echo_gain %.6f, the seed %.6f\n", (double)info.echo_gain,
	       REF_LOSS_SEED);
	CHECK_UINT(info.frames, 0);
	CHECK(ref_loss_seed_ok(info.echo_gain, REF_LOSS_SEED));
	udsp_ec_nlms.destroy(ec);
}

/*
 * The combined-loss arm of nlp_gate_get with the loss alone moved: the probe
 * of nlp_combined_loss_test on the 12 dB hybrid, at 0 and at 18. The ERLE
 * stays between the 6 dB floor and the 18 dB fast path, so the combined arm
 * decides. The combined loss reads under the gate at 0, and the 9 dB of the
 * loss take it over the gate.
 */
static void ref_loss_gate_test(unsigned rate)
{
	struct gate_probe plain;
	struct gate_probe lossy;
	double plain_db;
	double lossy_db;

	nlp_gate_probe(rate, 0, 12.0, 36, &plain);
	nlp_gate_probe(rate, REF_LOSS_HALF_DB, 12.0, 36, &lossy);
	plain_db = plain.erle - 10.0 * log10(2.0 * plain.echo_gain);
	lossy_db = lossy.erle - 10.0 * log10(2.0 * lossy.echo_gain) +
		   REF_LOSS_DB;

	case_rate(rate, "lec reference loss carries the combined loss over "
		  "the gate");
	printf("# erle %.2f and %.2f dB, combined loss %.2f dB at 0 and "
	       "%.2f dB at %u half-dB\n", (double)plain.erle,
	       (double)lossy.erle, plain_db, lossy_db, REF_LOSS_HALF_DB);
	CHECK(plain.erle >= 6.0f && plain.erle < 18.0f);
	CHECK(lossy.erle >= 6.0f && lossy.erle < 18.0f);
	CHECK(plain_db < REF_LOSS_GATE_DB);
	CHECK(lossy_db >= REF_LOSS_GATE_DB);
	CHECK(!plain.nlp_far);
	CHECK(lossy.nlp_far);
	CHECK(!lossy.nlp_doubletalk);
}

/* The balance shape of path_weak_init at 20 dB under the word source, with a
 * near-end floor and no talker. On that line the processor acts on most
 * window-active frames and the ERLE gate refuses some, so both shares carry
 * a figure. The loss stands ahead of the reference alone, so the microphone
 * is the same at every loss. */
static void ref_loss_words_run(unsigned rate, unsigned half_db,
			       struct udsp_ec_nlms_info *info)
{
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(0, rate);
	struct word_source far;
	struct echo_path path;
	uint32_t floor_state = 0x0badcafeu;
	unsigned frame = rate / 100u;
	unsigned f;

	udsp_ec_nlms_ref_loss_set(ec, half_db);
	path_weak_init(&path, rate, REF_LOSS_WORD_ERL_DB);
	word_source_init(&far, rate, WEAK_FAR_DBM0, 0x13572468u);
	for (f = 0; f < REF_LOSS_WORD_FRAMES; f++) {
		int16_t line[UDSP_FRAME_MAX];
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t hiss[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		unsigned i;

		word_fill(&far, line, frame);
		path_run(&path, line, echo);
		noise_fill(hiss, frame, REF_LOSS_WORD_FLOOR_DBM0,
			   &floor_state);
		for (i = 0; i < frame; i++)
			mic[i] = sample_clamp((double)echo[i] + hiss[i]);
		level_loss_apply(ref, line, frame, half_db);
		udsp_ec_nlms.run(ec, mic, ref);
	}
	udsp_ec_nlms_info_get(ec, info);
	udsp_ec_nlms.destroy(ec);
}

/* A count over the window-active frames, the five counts other than
 * nlp_quiet_frames, in percent. */
static double ref_loss_share_get(const struct udsp_ec_nlms_info *info,
				 uint32_t count)
{
	uint32_t judged = info->nlp_active_frames + info->nlp_wait_frames +
			  info->nlp_erle_frames + info->nlp_unlike_frames +
			  info->nlp_dt_frames;

	return judged ? 100.0 * count / judged : 0.0;
}

/*
 * A reference 9 dB lower over an echo path 9 dB stronger, with the loss of
 * 18, against the line at 0. The path reads the line, and the canceller reads
 * the line less the loss, so the microphone of the two arms is the same. The
 * first case adapts one second on the composite hybrid of convergence_test
 * and reads the ERLE of the last 20 frames. The second runs the word source
 * over the weak hybrid and compares the verdict shares and the resets.
 */
static void ref_loss_equiv_test(unsigned rate)
{
	struct udsp_ec_nlms_info plain;
	struct udsp_ec_nlms_info lossy;
	struct echo_path path;
	double erle[2];
	double share[2][2];
	unsigned i;

	for (i = 0; i < 2u; i++) {
		struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, rate);
		uint32_t state = 0x0f1e2d3cu;
		unsigned half_db = i ? REF_LOSS_HALF_DB : 0u;

		udsp_ec_nlms.nlp(ec, false);
		udsp_ec_nlms_ref_loss_set(ec, half_db);
		path_composite_init(&path, rate, 12.0);
		erle[i] = erle_offset_run(ec, &path, &state, -10.0, 0, half_db,
					  FRAMES_PER_SECOND, 20);
		udsp_ec_nlms.destroy(ec);
	}
	case_rate(rate, "lec reference loss converges as the line at 0");
	printf("# erle %.2f dB at 0, %.2f dB at %u half-dB\n", erle[0],
	       erle[1], REF_LOSS_HALF_DB);
	CHECK(erle[0] >= 25.0);
	CHECK(fabs(erle[1] - erle[0]) <= REF_LOSS_ERLE_TOL_DB);

	ref_loss_words_run(rate, 0, &plain);
	ref_loss_words_run(rate, REF_LOSS_HALF_DB, &lossy);
	share[0][0] = ref_loss_share_get(&plain, plain.nlp_active_frames);
	share[0][1] = ref_loss_share_get(&plain, plain.nlp_erle_frames);
	share[1][0] = ref_loss_share_get(&lossy, lossy.nlp_active_frames);
	share[1][1] = ref_loss_share_get(&lossy, lossy.nlp_erle_frames);
	case_rate(rate, "lec reference loss keeps the verdict shares of the "
		  "line at 0");
	printf("# active %.1f%% and %.1f%%, erle %.1f%% and %.1f%%, resets "
	       "%u and %u, trials %u and %u\n", share[0][0], share[1][0],
	       share[0][1], share[1][1], plain.diverge_resets,
	       lossy.diverge_resets, plain.probes, lossy.probes);
	CHECK(share[0][0] > 0.0);
	CHECK(share[0][1] > 0.0);
	CHECK(fabs(share[1][0] - share[0][0]) <= REF_LOSS_SHARE_TOL);
	CHECK(fabs(share[1][1] - share[0][1]) <= REF_LOSS_SHARE_TOL);
	CHECK_UINT(lossy.diverge_resets, plain.diverge_resets);
}

/* The loss of each instance that a NEW record of the fixture created. */
struct fixture_loss {
	unsigned fresh;
	unsigned ec_loss;
	unsigned shadow_loss;
	bool shadow;
};

static void fixture_loss_seen(void *ctx, const struct lecr_record *rec,
			      const struct lecr_replay *r,
			      const struct lecr_step *st,
			      const struct lecr_agree *a, bool closed)
{
	struct fixture_loss *seen = ctx;

	(void)a;
	(void)closed;
	if (!rec || !st->fresh)
		return;
	seen->fresh++;
	seen->ec_loss = udsp_ec_nlms_ref_loss_get(r->ec);
	seen->shadow = r->shadow != NULL;
	seen->shadow_loss = udsp_ec_nlms_ref_loss_get(r->shadow);
}

static void fixture_loss_replay(char *buf, size_t size,
				struct fixture_loss *seen)
{
	struct lecr_replay r;
	struct lecr_agree a;
	struct lecr_file file;
	FILE *f = fmemopen(buf, size, "rb");

	memset(seen, 0, sizeof(*seen));
	if (!f)
		return;
	lecr_replay_init(&r, true);
	lecr_agree_init(&a, 0);
	lecr_stream_replay(f, &file, &r, &a, fixture_loss_seen, seen);
	fclose(f);
}

/* The bytes of the file that path names, or NULL. The caller frees them. */
static char *fixture_bytes_get(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	char *buf = NULL;
	long end;

	if (!f)
		return NULL;
	if (!fseek(f, 0, SEEK_END) && (end = ftell(f)) > 0 &&
	    !fseek(f, 0, SEEK_SET)) {
		buf = malloc((size_t)end);
		*size = (size_t)end;
	}
	if (buf && fread(buf, 1, *size, f) != *size) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	return buf;
}

/*
 * The replay of the fixture of capture_fixture_test gives the loss of its
 * header to the instance and to its shadow. The same bytes with flag bit 1
 * clear replay at 0, although offset 18 still holds the loss.
 */
static void capture_fixture_loss_test(void)
{
	const char *path = getenv("UDSP_LEC_CAPTURE_FIXTURE");
	struct fixture_loss seen;
	size_t size = 0;
	char *buf;

	CASE("lec capture fixture gives its reference loss to both instances");
	if (!path) {
		tu_skip("UDSP_LEC_CAPTURE_FIXTURE not set");
		CASE("lec capture fixture with the loss flag clear replays at 0");
		tu_skip("UDSP_LEC_CAPTURE_FIXTURE not set");
		return;
	}
	buf = fixture_bytes_get(path, &size);
	CHECK(buf != NULL && size > LECR_FILE_BYTES);
	if (!buf || size <= LECR_FILE_BYTES) {
		free(buf);
		return;
	}
	fixture_loss_replay(buf, size, &seen);
	CHECK_UINT(seen.fresh, 1);
	CHECK(seen.shadow);
	CHECK_UINT(seen.ec_loss, REF_LOSS_HALF_DB);
	CHECK_UINT(seen.shadow_loss, REF_LOSS_HALF_DB);

	CASE("lec capture fixture with the loss flag clear replays at 0");
	buf[24] = (char)(buf[24] & ~LECR_FILE_REF_LOSS);
	fixture_loss_replay(buf, size, &seen);
	CHECK_UINT(seen.fresh, 1);
	CHECK(seen.shadow);
	CHECK_UINT(seen.ec_loss, 0);
	CHECK_UINT(seen.shadow_loss, 0);
	free(buf);
}

/* A header loss is read with a bound of 30 dB: the reach of the engine, and
 * the point past which the scaled rules leave their range. */
static int header_loss_open(uint16_t half_db)
{
	struct lecr_file file;
	uint8_t head[LECR_FILE_BYTES];
	FILE *f;
	int rc;

	memset(&file, 0, sizeof(file));
	file.rate = UDSP_SAMPLE_RATE;
	file.samples = UDSP_FRAME_SAMPLES;
	lecr_file_ref_loss_set(&file, half_db);
	lecr_file_encode(head, &file);
	f = fmemopen(head, sizeof(head), "rb");
	if (!f)
		return -ENOMEM;
	rc = lecr_read_open(f, &file);
	fclose(f);
	return rc;
}

static void ref_loss_bound_test(void)
{
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(48, UDSP_SAMPLE_RATE);

	CASE("lec reference loss over the reach of the engine clamps to it");
	udsp_ec_nlms_ref_loss_set(ec, UDSP_EC_NLMS_REF_LOSS_MAX + 1u);
	CHECK_UINT(udsp_ec_nlms_ref_loss_get(ec), UDSP_EC_NLMS_REF_LOSS_MAX);
	udsp_ec_nlms.destroy(ec);

	CASE("lec replay reader refuses a header loss over the reach");
	CHECK(header_loss_open(UDSP_EC_NLMS_REF_LOSS_MAX) == 0);
	CHECK(header_loss_open(UDSP_EC_NLMS_REF_LOSS_MAX + 1u) == -EINVAL);
}

int main(int argc, char **argv)
{
	int failed;

	if (!path_select(argc, argv))
		return 2;
	tail_clamp_test();
	convergence_test(UDSP_SAMPLE_RATE);
	tail_bound_test();
	doubletalk_test(UDSP_SAMPLE_RATE);
	freeze_test();
	nlp_test();
	saturation_test();
	determinism_test();
	detector_latch_test();
	nlp_near_only_test();
	path_step_test(UDSP_SAMPLE_RATE);
	sustained_doubletalk_test();
	bootstrap_test();
	gain_bound_test();
	lockout_escape_test();
	probe_pause_test();
	probe_rearm_test();
	nlp_combined_loss_test();
	nlp_echo_like_test();
	nlp_hysteresis_test(UDSP_SAMPLE_RATE);
	reset_test();
	reconverge_test(UDSP_SAMPLE_RATE);
	lockout_release_test(UDSP_SAMPLE_RATE);
	near_end_preserve_test(UDSP_SAMPLE_RATE);
	bootstrap_hold_test();
	probe_reject_test();
	probe_freeze_test();
	probe_cooldown_test(UDSP_SAMPLE_RATE);
	latch_refusal_test(UDSP_SAMPLE_RATE);
	watchdog_test();
	/*
	 * Each group of legs runs after the groups above it, so a case keeps
	 * its case number when a group joins the suite. The 16 kHz leg is one
	 * such group.
	 */
	create_rate_test();
	convergence_test(UDSP_SAMPLE_RATE_WB);
	doubletalk_test(UDSP_SAMPLE_RATE_WB);
	path_step_test(UDSP_SAMPLE_RATE_WB);
	reconverge_test(UDSP_SAMPLE_RATE_WB);
	lockout_release_test(UDSP_SAMPLE_RATE_WB);
	near_end_preserve_test(UDSP_SAMPLE_RATE_WB);
	probe_cooldown_test(UDSP_SAMPLE_RATE_WB);
	latch_refusal_test(UDSP_SAMPLE_RATE_WB);
	upper_band_test();
	/* The vector-path legs run last for the same reason. */
	last_tap_test(UDSP_SAMPLE_RATE);
	last_tap_test(UDSP_SAMPLE_RATE_WB);
	last_sample_test(UDSP_SAMPLE_RATE);
	last_sample_test(UDSP_SAMPLE_RATE_WB);
	equivalence_test();
	/* The weak-hybrid legs run after them for the same reason. */
	noise_floor_stale_test();
	weak_hybrid_test(UDSP_SAMPLE_RATE);
	weak_hybrid_test(UDSP_SAMPLE_RATE_WB);
	probe_divergence_test(UDSP_SAMPLE_RATE);
	probe_divergence_test(UDSP_SAMPLE_RATE_WB);
	talkover_test(UDSP_SAMPLE_RATE);
	talkover_test(UDSP_SAMPLE_RATE_WB);
	louder_hybrid_test(UDSP_SAMPLE_RATE);
	louder_hybrid_test(UDSP_SAMPLE_RATE_WB);
	probe_boundary_test(UDSP_SAMPLE_RATE);
	probe_boundary_test(UDSP_SAMPLE_RATE_WB);
	noise_floor_outlier_test(UDSP_SAMPLE_RATE);
	noise_floor_outlier_test(UDSP_SAMPLE_RATE_WB);
	watchdog_recovery_test();
	/* The zero-skip legs run last for the numbering reason above. */
	zero_skip_test(UDSP_SAMPLE_RATE);
	zero_skip_test(UDSP_SAMPLE_RATE_WB);
	/* The verdict and peak legs run last for the numbering reason above. */
	nlp_quiet_count_test();
	nlp_erle_count_test();
	nlp_unlike_count_test();
	nlp_dt_count_test();
	nlp_wait_count_test(UDSP_SAMPLE_RATE);
	peak_tap_test(UDSP_SAMPLE_RATE);
	peak_tap_test(UDSP_SAMPLE_RATE_WB);
	reset_count_test();
	divergence_peak_test();
	/* The feed leg runs last for the numbering reason above. */
	ref_feed_test();
	/* The reference offset and peak legs run last for the numbering reason
	 * above. */
	ref_offset_test(UDSP_SAMPLE_RATE);
	ref_offset_test(UDSP_SAMPLE_RATE_WB);
	peak_zero_test();
	peak_tie_test();
	/* The trial verdict legs run last for the numbering reason above. */
	latch_pause_test();
	probe_syllable_test();
	/* The onset leg runs last for the numbering reason above. */
	probe_onset_test();
	/* The floor window leg runs last for the numbering reason above. */
	noise_floor_window_test();
	/* The processor floor legs run last for the numbering reason above. */
	nlp_floor_test();
	/* The frame reading legs run last for the numbering reason above. */
	reading_verdict_test();
	reading_window_test(UDSP_SAMPLE_RATE);
	reading_window_test(UDSP_SAMPLE_RATE_WB);
	reading_reset_test();
	reading_floor_test();
	/* The hangover legs run last for the numbering reason above. */
	hangover_floor_test();
	hangover_no_floor_test();
	hangover_loud_test();
	hangover_rearm_test();
	hangover_marginal_edge_test();
	hangover_soft_edge_test();
	hangover_talker_test();
	hangover_headroom_test();
	hangover_repeat_test();
	hangover_repeat_start_test();
	hangover_sparse_test();
	/* The replay legs run last for the numbering reason above. */
	replay_call_test(UDSP_SAMPLE_RATE);
	replay_call_test(UDSP_SAMPLE_RATE_WB);
	replay_agree_test();
	replay_verdict_word_test();
	capture_verdict_word_test();
	/* The capture fixture leg runs last for the numbering reason above. */
	capture_fixture_test();
	/* The reader rule, exclusion and bound legs run last for the numbering
	 * reason above. */
	replay_rule_test();
	replay_skip_test();
	replay_bound_test();
	/* The floor tracker legs follow, for the numbering reason above. */
	floor_pause_words_test();
	floor_pause_speech_test();
	floor_rise_test();
	floor_silence_test();
	floor_short_pause_test();
	floor_bound_test();
	floor_run_break_test();
	floor_quiet3_test();
	floor_age_test();
	floor_reset_test();
	/*
	 * The seed legs follow, for the numbering reason above. No case covers
	 * a reset that keeps the seed run, and none can: the reset sets the
	 * tracked ERLE to 0, so the first frame after it fails gate (b) of
	 * dsp/lec.c and clears the run anyway.
	 */
	fan_seed_test(UDSP_SAMPLE_RATE);
	fan_seed_test(UDSP_SAMPLE_RATE_WB);
	fan_turns_test();
	fan_measured_test();
	fan_reseed_test();
	fan_lowpass_test();
	fan_step_test();
	seed_block_test(UDSP_SAMPLE_RATE);
	seed_block_test(UDSP_SAMPLE_RATE_WB);
	seed_trial_test();
	/* The snapshot leg follows, for the numbering reason above. */
	probe_snapshot_test();
	/* The stale-estimate legs follow, for the numbering reason above. */
	ring_trust_test(UDSP_SAMPLE_RATE);
	ring_trust_test(UDSP_SAMPLE_RATE_WB);
	stale_hold_test(UDSP_SAMPLE_RATE);
	stale_hold_test(UDSP_SAMPLE_RATE_WB);
	stale_talker_test();
	stale_quiet_test();
	expose_steady_test();
	expose_turns_test();
	expose_stop_test();
	expose_twin_test();
	change_test(UDSP_SAMPLE_RATE);
	change_test(UDSP_SAMPLE_RATE_WB);
	stale_return_test(UDSP_SAMPLE_RATE);
	stale_return_test(UDSP_SAMPLE_RATE_WB);
	stale_carry_test(UDSP_SAMPLE_RATE);
	stale_carry_test(UDSP_SAMPLE_RATE_WB);
	stale_repay_test(UDSP_SAMPLE_RATE);
	stale_repay_test(UDSP_SAMPLE_RATE_WB);
	/* The trial field leg follows, for the numbering reason above. */
	trial_mark_test();
	/* The talker guard legs follow, for the numbering reason above. */
	latch_talker_test(UDSP_SAMPLE_RATE);
	latch_talker_test(UDSP_SAMPLE_RATE_WB);
	trial_talker_test();
	latch_return_test();
	talker_tail_test(UDSP_SAMPLE_RATE);
	talker_tail_test(UDSP_SAMPLE_RATE_WB);
	/* The far-end floor legs follow, for the numbering reason above. */
	far_floor_talker_test(UDSP_SAMPLE_RATE);
	far_floor_talker_test(UDSP_SAMPLE_RATE_WB);
	far_floor_identity_test();
	far_floor_bar_test();
	far_floor_cap_test();
	far_floor_feed_test();
	far_floor_trial_test();
	far_floor_latch_test(UDSP_SAMPLE_RATE);
	far_floor_latch_test(UDSP_SAMPLE_RATE_WB);
	far_floor_words_test();
	far_floor_toggle_test();
	/* The trust count legs follow, for the numbering reason above. */
	ring_inside_test();
	ring_lockout_test(UDSP_SAMPLE_RATE);
	ring_lockout_test(UDSP_SAMPLE_RATE_WB);
	trust_restart_test(UDSP_SAMPLE_RATE);
	trust_restart_test(UDSP_SAMPLE_RATE_WB);
	trust_window_talker_test(UDSP_SAMPLE_RATE);
	trust_window_talker_test(UDSP_SAMPLE_RATE_WB);
	trust_turn_test(UDSP_SAMPLE_RATE);
	trust_turn_test(UDSP_SAMPLE_RATE_WB);
	/* The reference loss legs follow, for the numbering reason above. */
	ref_loss_setter_test(UDSP_SAMPLE_RATE);
	ref_loss_setter_test(UDSP_SAMPLE_RATE_WB);
	ref_loss_gate_test(UDSP_SAMPLE_RATE);
	ref_loss_gate_test(UDSP_SAMPLE_RATE_WB);
	ref_loss_equiv_test(UDSP_SAMPLE_RATE);
	ref_loss_equiv_test(UDSP_SAMPLE_RATE_WB);
	capture_fixture_loss_test();
	ref_loss_bound_test();
	/* The legs of the engage run through a pause follow, for the numbering
	 * reason above. */
	nlp_hysteresis_test(UDSP_SAMPLE_RATE_WB);
	nlp_spurt_test(UDSP_SAMPLE_RATE);
	nlp_spurt_test(UDSP_SAMPLE_RATE_WB);
	nlp_wait_count_test(UDSP_SAMPLE_RATE_WB);
	state_room_test(UDSP_SAMPLE_RATE);
	state_room_test(UDSP_SAMPLE_RATE_WB);
	state_silence_test(UDSP_SAMPLE_RATE);
	state_silence_test(UDSP_SAMPLE_RATE_WB);
	state_early_test(UDSP_SAMPLE_RATE);
	state_early_test(UDSP_SAMPLE_RATE_WB);
	state_trust_test(UDSP_SAMPLE_RATE);
	state_trust_test(UDSP_SAMPLE_RATE_WB);
	pause_burst_test(UDSP_SAMPLE_RATE);
	pause_burst_test(UDSP_SAMPLE_RATE_WB);
	pause_expose_test(UDSP_SAMPLE_RATE);
	pause_expose_test(UDSP_SAMPLE_RATE_WB);
	nlp_dt_wait_test(UDSP_SAMPLE_RATE);
	nlp_dt_wait_test(UDSP_SAMPLE_RATE_WB);
	nlp_erle_wait_test(UDSP_SAMPLE_RATE);
	nlp_erle_wait_test(UDSP_SAMPLE_RATE_WB);
	/* The pause headroom legs follow, for the numbering reason above. */
	pause_headroom_test(UDSP_SAMPLE_RATE);
	pause_headroom_test(UDSP_SAMPLE_RATE_WB);
	active_headroom_test(UDSP_SAMPLE_RATE);
	active_headroom_test(UDSP_SAMPLE_RATE_WB);
	/* The legs of the cut run follow, for the numbering reason above. */
	nlp_flap_cost_test(UDSP_SAMPLE_RATE);
	nlp_flap_cost_test(UDSP_SAMPLE_RATE_WB);
	nlp_cut_test(UDSP_SAMPLE_RATE);
	nlp_cut_test(UDSP_SAMPLE_RATE_WB);
	failed = tu_summary("lec");
	printf("# lec per-tap path: %s\n",
	       udsp_ec_nlms_path_name(udsp_ec_nlms_path_get()));
	return failed;
}
