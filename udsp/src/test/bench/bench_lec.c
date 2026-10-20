// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * G.168-derived bench for the NLMS line echo canceller, with the speexdsp MDF
 * canceller (pkg-config speexdsp) as an independent oracle on identical
 * scenarios. Standalone binary, not a ctest case: it prints tables for the
 * record and never asserts.
 *
 * The bench is derived from G.168 and is not G.168-conformant. The
 * recommendation text is not in this tree. Each scenario names the numbered
 * G.168 test whose intent it models, the hybrid responses are built here from
 * the shape the Annex D models have (flat bulk delay, dispersive reflection,
 * second weaker reflection) and are not the Annex D coefficient tables, and
 * the composite source is built from the Annex C description (voiced burst,
 * pause, pseudo noise, pause) rather than the Annex C waveform. Every level
 * and duration below is a choice made here and printed with the result. The
 * scenario keys S1 to S8 are this bench's own labels, not G.168 numbers.
 *
 * The comparison is between two linear residuals: the NLP is off on the udsp
 * side, because speex_echo_cancellation returns the linear residual and its
 * suppressor lives in the separate preprocessor. The weak-hybrid rows, the
 * field rows, the talk-over rows and the far-end noise rows run a second
 * instance with the processor on. Their processor columns need it, and the
 * comparison needs the linear residual.
 *
 * --rate 8000|16000 selects the line rate. Every duration below is stated in
 * milliseconds or in 10 ms frames and therefore means the same at both rates;
 * the hybrids come from lec_paths.h, which samples one shape at the rate it is
 * given. Both cancellers follow the rate: speexdsp takes the frame in samples
 * and the tail in samples, and its sampling rate through
 * SPEEX_ECHO_SET_SAMPLING_RATE.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <speex/speex_echo.h>

#include "../../dsp/dsp.h"
#include "../../dsp/lec.h"
#include "../../dsp/resamp.h"
#include "lec_paths.h"
#include "lec_paths_field.h"
#include "lec_replay.h"

/* The engine tick is 10 ms at every line rate, so a frame count is a time. */
#define FRAMES_PER_SECOND	100u
#define BLOCK_FRAMES		(FRAMES_PER_SECOND / 2u)
#define BLOCKS_MAX		24u

enum source_kind {
	SOURCE_NOISE,
	SOURCE_CSS,
	SOURCE_TONE,
};

enum path_kind {
	PATH_FLAT,
	PATH_DISPERSIVE,
	PATH_COMPOSITE,
	PATH_LATE,
	PATH_UPPER,
	PATH_WEAK,
	PATH_KINDS,
};

static const char *const path_names[PATH_KINDS] = {
	"flat 8 ms", "dispersive", "composite", "late 28 ms", "upper 6k",
	"weak bal",
};

static void path_build(struct echo_path *path, enum path_kind kind,
		       unsigned rate, double erl_db)
{
	switch (kind) {
	case PATH_DISPERSIVE:
		path_dispersive_init(path, rate, erl_db);
		return;
	case PATH_COMPOSITE:
		path_composite_init(path, rate, erl_db);
		return;
	case PATH_LATE:
		path_late_init(path, rate, erl_db);
		return;
	case PATH_UPPER:
		path_upper_init(path, rate, erl_db);
		return;
	case PATH_WEAK:
		path_weak_init(path, rate, erl_db);
		return;
	default:
		path_flat_init(path, rate, 8, erl_db);
		return;
	}
}

struct run_cfg {
	const char *name;
	const char *models;
	unsigned tail_ms;
	unsigned frames;
	double far_dbm0;
	double near_dbm0;
	unsigned near_from;
	unsigned near_to;
	unsigned path_change;
	enum source_kind source;
	bool no_echo;
};

struct track {
	double echo[BLOCKS_MAX];
	double residual[BLOCKS_MAX];
	double product;
	double near_energy;
};

/*
 * --ref-loss N: the loss in half-dB steps that the engine applies ahead of
 * the reference of the canceller. Every instance takes the loss, and every
 * run() call reads the reference less the loss. The echo path keeps the line
 * reference, so the microphone of a row is the microphone of the row at 0. A
 * run at 0 passes the line reference itself, so it is the bench without the
 * option.
 *
 * BENCH_ACTIVE_POWER is the fixed activity bar of dsp/lec.c
 * (LEC_REF_ACTIVE_POWER). bench_ref_bar is that bar less the loss, in the
 * float arithmetic of ref_loss_scale_set, so a reference frame of the bench
 * meets the bar that the canceller reads.
 */
#define BENCH_ACTIVE_POWER	2500.0f

static unsigned bench_ref_loss;
static float bench_ref_bar = BENCH_ACTIVE_POWER;

static void bench_ref_loss_set(unsigned half_db)
{
	float db = (float)half_db / 2.0f;
	float g2 = powf(10.0f, db * -0.1f);

	bench_ref_loss = half_db;
	bench_ref_bar = BENCH_ACTIVE_POWER * g2;
}

static struct udsp_ec *bench_ec_create(unsigned tail_ms, unsigned line_rate)
{
	struct udsp_ec *ec = udsp_ec_nlms.create_rate(tail_ms, line_rate);

	udsp_ec_nlms_ref_loss_set(ec, bench_ref_loss);
	return ec;
}

/* The reference of the canceller: ref itself at 0, else ref less the loss in
 * buf. */
static const int16_t *bench_ref_get(const int16_t *ref, int16_t *buf,
				    unsigned frame)
{
	if (!bench_ref_loss)
		return ref;
	level_loss_apply(buf, ref, frame, bench_ref_loss);
	return buf;
}

static void bench_ec_run(struct udsp_ec *ec, int16_t *mic, const int16_t *ref,
			 unsigned frame)
{
	int16_t buf[UDSP_FRAME_MAX];

	udsp_ec_nlms.run(ec, mic, bench_ref_get(ref, buf, frame));
}



/*
 * Composite source derived from the G.168 Annex C description: a voiced burst
 * of 100 Hz harmonics (50 ms), a pause (150 ms), pseudo noise (200 ms) and a
 * second pause (100 ms), repeating. The activity factor and the alternation
 * between a periodic and a noise-like excitation are what the bench needs
 * from it.
 */
static void css_fill(int16_t *dst, unsigned count, double level_dbm0,
		     uint32_t *state, unsigned long *phase, unsigned rate)
{
	double amplitude = dbm0_rms_get(level_dbm0);
	unsigned long cycle = rate / 2u;
	unsigned long burst_to = rate / 20u;
	unsigned long noise_from = rate / 5u;
	unsigned long noise_to = 2ul * rate / 5u;
	unsigned i;

	for (i = 0; i < count; i++) {
		unsigned long position = (*phase + i) % cycle;
		double value = 0.0;
		unsigned harmonic;

		if (position < burst_to) {
			for (harmonic = 1; harmonic <= 12u; harmonic++)
				value += sin(2.0 * M_PI * 100.0 * harmonic *
					     (double)position /
					     rate) / harmonic;
			value *= amplitude * 0.8;
		} else if (position >= noise_from && position < noise_to) {
			double unit = rng_next(state) / 4294967295.0 * 2.0 -
				1.0;

			value = unit * amplitude * sqrt(3.0);
		}
		dst[i] = sample_clamp(value);
	}
	*phase = (*phase + count) % cycle;
}

static void track_add(struct track *track, unsigned block, const int16_t *echo,
		      const int16_t *residual, const int16_t *near, bool count,
		      unsigned frame)
{
	unsigned i;

	for (i = 0; i < frame; i++) {
		track->echo[block] += (double)echo[i] * echo[i];
		track->residual[block] += (double)residual[i] * residual[i];
		if (!count)
			continue;
		track->product += (double)residual[i] * near[i];
		track->near_energy += (double)near[i] * near[i];
	}
}

/*
 * With no echo injected there is no echo return loss to report: the ratio is
 * not a very good result, it is not a result at all. NAN says so, and the
 * 200 dB sentinel is kept for the case it belongs to, a real echo whose
 * residual fell below the int16 floor.
 */
static double erle_get(double echo, double residual)
{
	if (echo < 1e-6)
		return NAN;
	if (residual < 1e-6)
		return 200.0;
	return 10.0 * log10(echo / residual);
}

static double track_erle_get(const struct track *track, unsigned block)
{
	return erle_get(track->echo[block], track->residual[block]);
}

static void erle_print(double value)
{
	if (isnan(value))
		printf("            n/a");
	else
		printf(" %14.2f", value);
}

static void scenario_run(const struct run_cfg *cfg, unsigned line_rate)
{
	unsigned frame = line_rate / 100u;
	struct udsp_ec *ec = bench_ec_create(cfg->tail_ms, line_rate);
	/* The oracle gets the tail the canceller applied, never the one the
	 * scenario asked for: a clamped request would otherwise hand speexdsp
	 * a longer filter than the arm it is compared against. */
	SpeexEchoState *mdf = speex_echo_state_init((int)frame,
		(int)(udsp_ec_nlms_tail_get(ec) * (line_rate / 1000u)));
	struct udsp_ec_nlms_info info;
	struct echo_path path;
	struct echo_path late;
	struct track own;
	struct track oracle;
	uint32_t far_state = 0x13572468u;
	uint32_t near_state = 0xfeedfaceu;
	unsigned long css_phase = 0;
	unsigned long tone_phase = 0;
	int rate = (int)line_rate;
	unsigned blocks = (cfg->frames + BLOCK_FRAMES - 1u) / BLOCK_FRAMES;
	unsigned block;
	unsigned f;

	memset(&own, 0, sizeof(own));
	memset(&oracle, 0, sizeof(oracle));
	speex_echo_ctl(mdf, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
	udsp_ec_nlms.nlp(ec, false);
	path_composite_init(&path, line_rate, 12.0);
	path_late_init(&late, line_rate, 12.0);
	if (blocks > BLOCKS_MAX)
		blocks = BLOCKS_MAX;

	for (f = 0; f < cfg->frames; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t near[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		int16_t own_out[UDSP_FRAME_MAX];
		int16_t oracle_out[UDSP_FRAME_MAX];
		struct echo_path *active = &path;
		bool talking = cfg->near_dbm0 < 0.0 && f >= cfg->near_from &&
			f < cfg->near_to;
		unsigned i;

		block = f / BLOCK_FRAMES;
		if (block >= blocks)
			break;
		/* The line changes hybrid, it does not go quiet: the far-end
		 * delay line moves into the replacement at the switch frame,
		 * so the frames after it carry the echo of a different
		 * response and not the silence of an unfilled one. */
		if (cfg->path_change && f == cfg->path_change)
			path_history_carry(&late, &path);
		if (cfg->path_change && f >= cfg->path_change)
			active = &late;
		switch (cfg->source) {
		case SOURCE_CSS:
			css_fill(ref, frame, cfg->far_dbm0, &far_state,
				 &css_phase, line_rate);
			break;
		case SOURCE_TONE:
			tone_fill(ref, frame, cfg->far_dbm0, 1000.0,
				  &tone_phase, line_rate);
			break;
		default:
			noise_fill(ref, frame, cfg->far_dbm0, &far_state);
			break;
		}
		path_run(active, ref, echo);
		if (cfg->no_echo)
			memset(echo, 0, frame * sizeof(*echo));
		memset(near, 0, frame * sizeof(*near));
		if (talking)
			noise_fill(near, frame, cfg->near_dbm0, &near_state);
		for (i = 0; i < frame; i++)
			mic[i] = sample_clamp((double)echo[i] + near[i]);
		memcpy(own_out, mic, frame * sizeof(*own_out));
		bench_ec_run(ec, own_out, ref, frame);
		speex_echo_cancellation(mdf, mic, ref, oracle_out);
		track_add(&own, block, echo, own_out, near, talking, frame);
		track_add(&oracle, block, echo, oracle_out, near, talking,
			  frame);
	}

	printf("\n%s\n  models: %s\n", cfg->name, cfg->models);
	printf("  tail %u ms, far end %.1f dBm0, %u frames (%.1f s)",
	       udsp_ec_nlms_tail_get(ec), cfg->far_dbm0, cfg->frames,
	       (double)cfg->frames / FRAMES_PER_SECOND);
	if (cfg->near_dbm0 < 0.0)
		printf(", near end %.1f dBm0 over frames %u..%u",
		       cfg->near_dbm0, cfg->near_from, cfg->near_to);
	if (cfg->path_change)
		printf(", path change at frame %u", cfg->path_change);
	printf("\n  block(0.5 s) |   udsp ERLE dB | speexdsp ERLE dB |   gap dB\n");
	for (block = 0; block < blocks; block++) {
		double mine = track_erle_get(&own, block);
		double theirs = track_erle_get(&oracle, block);

		printf("  %12u |", block);
		erle_print(mine);
		printf(" |  ");
		erle_print(theirs);
		printf(" | ");
		if (isnan(mine) || isnan(theirs))
			printf("     n/a\n");
		else
			printf("%8.2f\n", mine - theirs);
	}
	if (own.near_energy > 0.0) {
		printf("  near-end passthrough: udsp %+.2f dB, speexdsp %+.2f dB (0 dB = unattenuated)\n",
		       20.0 * log10(own.product / own.near_energy),
		       20.0 * log10(oracle.product / oracle.near_energy));
	}
	udsp_ec_nlms_info_get(ec, &info);
	printf("  udsp state: %u of %u frames held, %u divergence resets, coefficient energy %.5f, tracked ERLE %.2f dB\n",
	       info.hold_frames, info.frames, info.diverge_resets,
	       (double)info.coef_energy, (double)info.erle_db);
	printf("  converged residual level: udsp %.2f dBm0, speexdsp %.2f dBm0\n",
	       rms_dbm0_get(sqrt(own.residual[blocks - 1u] /
				 (BLOCK_FRAMES * frame))),
	       rms_dbm0_get(sqrt(oracle.residual[blocks - 1u] /
				 (BLOCK_FRAMES * frame))));
	speex_echo_state_destroy(mdf);
	udsp_ec_nlms.destroy(ec);
}

static double elapsed_ns_get(const struct timespec *start,
			     const struct timespec *end, unsigned frames)
{
	double ns = (double)(end->tv_sec - start->tv_sec) * 1e9 +
		(double)(end->tv_nsec - start->tv_nsec);

	return ns / frames;
}

#define TIMING_FRAMES		500u
#define TIMING_ROUNDS		4u

/*
 * The stimulus is built once and kept: a hybrid convolution costs more than
 * the canceller and would be timed with it otherwise.
 *
 * One form of the per-tap loops per call: the caller names it, the selection
 * in force at entry is restored, and a form this build does not carry reports
 * NAN instead of a time. The info block is written only when the run happens,
 * so a caller that wants the row's tap count asks for the scalar form, the one
 * every target has. zero_skip sets the zero-reference skip switch of the
 * instance the call creates (dsp/lec.h).
 */
static double own_ns_get(unsigned tail_ms, unsigned line_rate,
			 enum udsp_ec_nlms_path path, bool zero_skip,
			 const int16_t *ref_track, const int16_t *mic_track,
			 struct udsp_ec_nlms_info *info)
{
	enum udsp_ec_nlms_path outer = udsp_ec_nlms_path_get();
	unsigned frame = line_rate / 100u;
	struct udsp_ec *ec;
	struct timespec start;
	struct timespec end;
	unsigned round;
	unsigned f;
	int16_t work[UDSP_FRAME_MAX];
	double ns;

	if (!udsp_ec_nlms_path_set(path))
		return NAN;
	ec = bench_ec_create(tail_ms, line_rate);
	udsp_ec_nlms_zero_skip_set(ec, zero_skip);
	for (f = 0; f < 100u; f++) {
		memcpy(work, mic_track + f * frame, frame * sizeof(*work));
		bench_ec_run(ec, work, ref_track + f * frame, frame);
	}
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (round = 0; round < TIMING_ROUNDS; round++) {
		for (f = 0; f < TIMING_FRAMES; f++) {
			memcpy(work, mic_track + f * frame,
			       frame * sizeof(*work));
			bench_ec_run(ec, work, ref_track + f * frame, frame);
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	ns = elapsed_ns_get(&start, &end, TIMING_FRAMES * TIMING_ROUNDS);
	udsp_ec_nlms_info_get(ec, info);
	udsp_ec_nlms.destroy(ec);
	udsp_ec_nlms_path_set(outer);
	return ns;
}

static double oracle_ns_get(unsigned tail_ms, unsigned line_rate,
			    const int16_t *ref_track, const int16_t *mic_track)
{
	unsigned frame = line_rate / 100u;
	struct udsp_ec *ec = bench_ec_create(tail_ms, line_rate);
	SpeexEchoState *mdf = speex_echo_state_init((int)frame,
		(int)(udsp_ec_nlms_tail_get(ec) * (line_rate / 1000u)));
	struct timespec start;
	struct timespec end;
	int rate = (int)line_rate;
	unsigned round;
	unsigned f;
	int16_t out[UDSP_FRAME_MAX];

	udsp_ec_nlms.destroy(ec);
	speex_echo_ctl(mdf, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (round = 0; round < TIMING_ROUNDS; round++) {
		for (f = 0; f < TIMING_FRAMES; f++) {
			speex_echo_cancellation(mdf, mic_track + f * frame,
						ref_track + f * frame, out);
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	speex_echo_state_destroy(mdf);
	return elapsed_ns_get(&start, &end, TIMING_FRAMES * TIMING_ROUNDS);
}

static void ns_print(double value)
{
	if (isnan(value))
		printf(" %14s", "n/a");
	else
		printf(" %14.0f", value);
}

static void timing_run(unsigned tail_ms, unsigned line_rate,
		       const int16_t *ref_track, const int16_t *mic_track)
{
	struct udsp_ec_nlms_info info;
	double scalar_ns = own_ns_get(tail_ms, line_rate,
				      UDSP_EC_NLMS_PATH_SCALAR, true,
				      ref_track, mic_track, &info);
	double simd_ns = own_ns_get(tail_ms, line_rate,
				    UDSP_EC_NLMS_PATH_SIMD, true, ref_track,
				    mic_track, &info);
	double oracle_ns = oracle_ns_get(tail_ms, line_rate, ref_track,
					 mic_track);

	printf("  %7u | %6u | %12.0f |", info.tail_ms, info.taps,
	       2.0 * info.taps * (line_rate / 100u));
	ns_print(scalar_ns);
	printf(" |");
	ns_print(simd_ns);
	printf(" | ");
	if (isnan(simd_ns))
		printf("%5s", "n/a");
	else
		printf("%4.1fx", scalar_ns / simd_ns);
	printf(" | %11.0f\n", oracle_ns);
}

/*
 * The same frame over a reference that is all zeros, in the form that walks
 * the tap loops over the zero window and in the form that skips them
 * (dsp/lec.c frame_zero_run). The first frames of the run still walk the
 * loops in both forms: the window drains one frame at a time.
 */
static void zero_timing_run(unsigned tail_ms, unsigned line_rate,
			    const int16_t *zero_track, const int16_t *mic_track)
{
	enum udsp_ec_nlms_path path = udsp_ec_nlms_path_get();
	struct udsp_ec_nlms_info info;
	double full_ns;
	double skip_ns;

	full_ns = own_ns_get(tail_ms, line_rate, path, false, zero_track,
			     mic_track, &info);
	skip_ns = own_ns_get(tail_ms, line_rate, path, true, zero_track,
			     mic_track, &info);
	printf("  %7u | %6u |", info.tail_ms, info.taps);
	ns_print(full_ns);
	printf(" |");
	ns_print(skip_ns);
	printf(" | %u of %u\n", info.skip_frames, info.frames);
}

static void stimulus_build(struct echo_path *path, unsigned line_rate,
			   int16_t *ref_track, int16_t *mic_track)
{
	unsigned frame = line_rate / 100u;
	uint32_t far_state = 0x2468ace0u;
	unsigned f;

	for (f = 0; f < TIMING_FRAMES; f++) {
		int16_t *ref = ref_track + f * frame;

		noise_fill(ref, frame, -10.0, &far_state);
		path_run(path, ref, mic_track + f * frame);
	}
}

/*
 * The per-path gate table. Each row converges one hybrid for three seconds
 * and reads the ERLE over the 200 ms that end at one second. It then changes
 * the line to the next hybrid of the list, with the filter converged, and
 * measures how long the recovery takes. The recovery bar is a fixed 20 dB and
 * the resolution is one 50 ms block. The same number therefore means the same
 * thing at both line rates, and the 16 kHz to 8 kHz ratio is the gate.
 */
/* Three seconds: a settle of two ends before the trust count of 200 clear
 * frames completes, and the table measures a trusted filter. */
#define SUMMARY_SETTLE_FRAMES	300u
#define SUMMARY_AFTER_FRAMES	800u
#define SUMMARY_BLOCK_FRAMES	5u
#define SUMMARY_BAR_DB		20.0
#define SUMMARY_TAIL_MS		48u
#define SUMMARY_ERL_DB		12.0
/*
 * One draw of this row is a sample of a bimodal distribution: a hybrid change
 * under continuous far-end speech either recovers in about 600 ms or holds for
 * the whole double-talk escape period, and which one a seed takes is not
 * predictable from the row. The table therefore aggregates over a list of
 * seeds and prints how many of them latched. The seeds walk the reference
 * generator's state by the golden-ratio stride, so the list is reproducible,
 * the first entry is the default seed, and --seeds 40 is the sweep the gate
 * numbers are quoted from.
 */
#define SUMMARY_SEEDS_DEFAULT	8u
/* A ceiling on the list, so a mistyped count cannot run the bench for hours. */
#define SUMMARY_SEEDS_MAX	1024u
#define SUMMARY_SEED_BASE	0x13572468u
#define SUMMARY_SEED_STRIDE	0x9e3779b9u
/*
 * A row-seed is latched when the hold after the path change runs unbroken for
 * this many frames. The two modes are 2.5 s apart, so the exact placement is
 * not delicate: a released instance holds at most 48 frames of the 800 and a
 * latched one holds 501 (measured at both rates).
 */
#define SUMMARY_LATCH_FRAMES	250u

/* The walk starts at the first seed of the run, not at the built-in base, so a
 * table printed with --seed reproduces from its own heading. */
static uint32_t summary_seed_get(uint32_t first, unsigned index)
{
	uint32_t seed = first + index * SUMMARY_SEED_STRIDE;

	/* The xorshift generator has no state 0 to walk out of. */
	return seed ? seed : 1u;
}

/*
 * One row per hybrid. The pairings are the same at both line rates, so the
 * 16 kHz to 8 kHz reconvergence ratio compares like with like; the upper-band
 * row exists at 16 kHz only, because 6 kHz has no narrowband form, and it
 * changes into the composite hybrid so the recovery crosses the 4 kHz band
 * split.
 */
struct summary_row {
	enum path_kind from;
	enum path_kind to;
	bool wideband_only;
};

static const struct summary_row summary_rows[] = {
	{ PATH_FLAT, PATH_DISPERSIVE, false },
	{ PATH_DISPERSIVE, PATH_COMPOSITE, false },
	{ PATH_COMPOSITE, PATH_LATE, false },
	{ PATH_LATE, PATH_FLAT, false },
	{ PATH_UPPER, PATH_COMPOSITE, true },
};

struct energy_pair {
	double echo;
	double own;
	double oracle;
};

struct summary_result {
	unsigned own_held;
	unsigned own_hold_run;
	double own_erle_1s;
	double oracle_erle_1s;
	double own_erle_end;
	double oracle_erle_end;
	double own_reconverge_ms;
	double oracle_reconverge_ms;
};

/* What one row aggregates to over the seed list. */
struct summary_stats {
	double own_erle_1s;
	double oracle_erle_1s;
	double own_erle_end;
	double oracle_erle_end;
	double own_reconverge_ms;
	double own_reconverge_max;
	double oracle_reconverge_ms;
	double held;
	unsigned hold_run_max;
	unsigned latched;
	unsigned own_never;
	unsigned oracle_never;
	unsigned seeds;
};

static void energy_add(struct energy_pair *e, const int16_t *echo,
		       const int16_t *own, const int16_t *oracle,
		       unsigned frame)
{
	unsigned i;

	for (i = 0; i < frame; i++) {
		e->echo += (double)echo[i] * echo[i];
		e->own += (double)own[i] * own[i];
		e->oracle += (double)oracle[i] * oracle[i];
	}
}

/* One frame through the hybrid and through both cancellers, on identical
 * input: the two arms never see different samples. */
static void pair_step(struct udsp_ec *ec, SpeexEchoState *mdf,
		      struct echo_path *path, uint32_t *state, unsigned frame,
		      int16_t *echo, int16_t *own, int16_t *oracle)
{
	int16_t ref[UDSP_FRAME_MAX];

	noise_fill(ref, frame, -10.0, state);
	path_run(path, ref, echo);
	memcpy(own, echo, frame * sizeof(*own));
	bench_ec_run(ec, own, ref, frame);
	speex_echo_cancellation(mdf, echo, ref, oracle);
}

static void summary_run(enum path_kind kind, enum path_kind next,
			unsigned line_rate, uint32_t seed,
			struct summary_result *out)
{
	unsigned frame = line_rate / 100u;
	struct udsp_ec *ec = bench_ec_create(SUMMARY_TAIL_MS,
						      line_rate);
	SpeexEchoState *mdf = speex_echo_state_init((int)frame,
		(int)(udsp_ec_nlms_tail_get(ec) * (line_rate / 1000u)));
	struct udsp_ec_nlms_info info;
	struct energy_pair window;
	struct energy_pair block;
	struct echo_path first;
	struct echo_path second;
	uint32_t state = seed;
	int16_t echo[UDSP_FRAME_MAX];
	int16_t own[UDSP_FRAME_MAX];
	int16_t oracle[UDSP_FRAME_MAX];
	int rate = (int)line_rate;
	unsigned held;
	unsigned run = 0;
	unsigned f;

	memset(out, 0, sizeof(*out));
	memset(&window, 0, sizeof(window));
	memset(&block, 0, sizeof(block));
	out->own_reconverge_ms = NAN;
	out->oracle_reconverge_ms = NAN;
	speex_echo_ctl(mdf, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
	udsp_ec_nlms.nlp(ec, false);
	path_build(&first, kind, line_rate, SUMMARY_ERL_DB);
	path_build(&second, next, line_rate, SUMMARY_ERL_DB);

	for (f = 0; f < SUMMARY_SETTLE_FRAMES; f++) {
		pair_step(ec, mdf, &first, &state, frame, echo, own, oracle);
		if (f + 20u < FRAMES_PER_SECOND || f >= FRAMES_PER_SECOND)
			continue;
		energy_add(&window, echo, own, oracle, frame);
	}
	out->own_erle_1s = erle_get(window.echo, window.own);
	out->oracle_erle_1s = erle_get(window.echo, window.oracle);
	/* The line changes hybrid, it does not go quiet: the far-end delay
	 * line moves into the replacement so the frames after the change
	 * carry the echo of a different response, not the silence of an
	 * unfilled one. */
	path_history_carry(&second, &first);
	/* Held frames over the three seconds of convergence. The double-talk
	 * detector reads a window maximum, and a longer frame makes that
	 * maximum a tighter bound. The count is therefore a rate-sensitive
	 * number that explains a slow first second where it is not zero. */
	udsp_ec_nlms_info_get(ec, &info);
	out->own_held = info.hold_frames;
	held = info.hold_frames;

	for (f = 0; f < SUMMARY_AFTER_FRAMES; f++) {
		pair_step(ec, mdf, &second, &state, frame, echo, own, oracle);
		/* The longest unbroken hold after the change, which is what
		 * separates a lockout from the handful of held frames a
		 * recovery costs. */
		udsp_ec_nlms_info_get(ec, &info);
		run = info.hold_frames != held ? run + 1u : 0u;
		if (run > out->own_hold_run)
			out->own_hold_run = run;
		held = info.hold_frames;
		energy_add(&block, echo, own, oracle, frame);
		if ((f + 1u) % SUMMARY_BLOCK_FRAMES)
			continue;
		out->own_erle_end = erle_get(block.echo, block.own);
		out->oracle_erle_end = erle_get(block.echo, block.oracle);
		if (isnan(out->own_reconverge_ms) &&
		    out->own_erle_end >= SUMMARY_BAR_DB)
			out->own_reconverge_ms = (double)(f + 1u) * 10.0;
		if (isnan(out->oracle_reconverge_ms) &&
		    out->oracle_erle_end >= SUMMARY_BAR_DB)
			out->oracle_reconverge_ms = (double)(f + 1u) * 10.0;
		memset(&block, 0, sizeof(block));
	}
	speex_echo_state_destroy(mdf);
	udsp_ec_nlms.destroy(ec);
}

static void ms_print(double value)
{
	if (isnan(value))
		printf(" %9s", "never");
	else
		printf(" %9.0f", value);
}

/* One row of the table: every seed of the list through the same path pair. */
static void summary_stats_get(enum path_kind from, enum path_kind to,
			      unsigned line_rate, unsigned seeds,
			      uint32_t seed_first, struct summary_stats *out)
{
	unsigned reached = 0;
	unsigned oracle_reached = 0;
	unsigned s;

	memset(out, 0, sizeof(*out));
	out->seeds = seeds;
	for (s = 0; s < seeds; s++) {
		struct summary_result result;
		uint32_t seed = summary_seed_get(seed_first, s);

		summary_run(from, to, line_rate, seed, &result);
		out->own_erle_1s += result.own_erle_1s;
		out->oracle_erle_1s += result.oracle_erle_1s;
		out->own_erle_end += result.own_erle_end;
		out->oracle_erle_end += result.oracle_erle_end;
		out->held += result.own_held;
		if (result.own_hold_run > out->hold_run_max)
			out->hold_run_max = result.own_hold_run;
		if (result.own_hold_run >= SUMMARY_LATCH_FRAMES)
			out->latched++;
		if (isnan(result.own_reconverge_ms)) {
			out->own_never++;
		} else {
			out->own_reconverge_ms += result.own_reconverge_ms;
			if (result.own_reconverge_ms > out->own_reconverge_max)
				out->own_reconverge_max =
					result.own_reconverge_ms;
			reached++;
		}
		if (isnan(result.oracle_reconverge_ms)) {
			out->oracle_never++;
		} else {
			out->oracle_reconverge_ms +=
				result.oracle_reconverge_ms;
			oracle_reached++;
		}
	}
	out->own_erle_1s /= seeds;
	out->oracle_erle_1s /= seeds;
	out->own_erle_end /= seeds;
	out->oracle_erle_end /= seeds;
	out->held /= seeds;
	/* A mean over the seeds that reached the bar at all; the never count
	 * is reported beside it rather than folded into a number. */
	out->own_reconverge_ms = reached ? out->own_reconverge_ms / reached :
					   NAN;
	out->oracle_reconverge_ms = oracle_reached ?
		out->oracle_reconverge_ms / oracle_reached : NAN;
	if (out->own_never)
		out->own_reconverge_max = NAN;
}

static void summary_print(unsigned line_rate, unsigned seeds,
			  uint32_t seed_first, int16_t *ref_track,
			  int16_t *mic_track)
{
	struct udsp_ec_nlms_info info;
	struct summary_stats stats;
	struct echo_path path;
	double own_ns;
	double oracle_ns;
	unsigned row;

	printf("\nper-path gate table at %u Hz, tail %u ms, far end -10.0 dBm0, hybrid %.0f dB ERL\n",
	       line_rate, SUMMARY_TAIL_MS, SUMMARY_ERL_DB);
	printf("every figure is the mean over %u seed%s from 0x%08x, stride 0x%08x\n",
	       seeds, seeds == 1u ? "" : "s", seed_first, SUMMARY_SEED_STRIDE);
	printf("ERLE at 1 s is measured over the 200 ms that end at one second;\n");
	printf("reconvergence is the time from the path change to the first %u ms block at %.0f dB,\n",
	       SUMMARY_BLOCK_FRAMES * 10u, SUMMARY_BAR_DB);
	printf("latch counts the seeds whose hold after the change ran %u frames unbroken;\n",
	       SUMMARY_LATCH_FRAMES);
	printf("settle is the mean hold count of the three seconds before the change and run is\n");
	printf("the longest unbroken hold after it, over the worst seed of the list;\n");
	printf("the udsp ns column runs the %s form of the per-tap loops\n",
	       udsp_ec_nlms_path_name(udsp_ec_nlms_path_get()));
	printf("  %-10s | %-10s | %18s | %31s | %17s | %17s | %5s | %14s\n",
	       "path", "change to", "ERLE at 1 s dB", "reconvergence ms",
	       "ERLE at the end", "ns per run()", "latch", "held frames");
	printf("  %-10s | %-10s | %7s | %8s | %9s | %9s | %9s | %6s | %8s | %6s | %8s | %5s | %6s | %5s\n",
	       "", "", "udsp", "speexdsp", "udsp", "udsp max", "speexdsp",
	       "udsp", "speexdsp", "udsp", "speexdsp", "n/N", "settle", "run");
	for (row = 0; row < sizeof(summary_rows) / sizeof(summary_rows[0]);
	     row++) {
		enum path_kind from = summary_rows[row].from;
		enum path_kind to = summary_rows[row].to;
		char latch[24];

		if (summary_rows[row].wideband_only &&
		    line_rate != UDSP_SAMPLE_RATE_WB)
			continue;
		summary_stats_get(from, to, line_rate, seeds, seed_first,
				  &stats);
		path_build(&path, from, line_rate, SUMMARY_ERL_DB);
		stimulus_build(&path, line_rate, ref_track, mic_track);
		own_ns = own_ns_get(SUMMARY_TAIL_MS, line_rate,
				    udsp_ec_nlms_path_get(), true, ref_track,
				    mic_track, &info);
		oracle_ns = oracle_ns_get(SUMMARY_TAIL_MS, line_rate, ref_track,
					  mic_track);
		snprintf(latch, sizeof(latch), "%u/%u", stats.latched,
			 stats.seeds);
		printf("  %-10s | %-10s | %7.2f | %8.2f |", path_names[from],
		       path_names[to], stats.own_erle_1s, stats.oracle_erle_1s);
		ms_print(stats.own_reconverge_ms);
		printf(" |");
		ms_print(stats.own_reconverge_max);
		printf(" |");
		ms_print(stats.oracle_reconverge_ms);
		printf(" | %6.2f | %8.2f | %6.0f | %8.0f | %5s | %6.0f | %5u\n",
		       stats.own_erle_end, stats.oracle_erle_end, own_ns,
		       oracle_ns, latch, stats.held, stats.hold_run_max);
		if (stats.own_never || stats.oracle_never)
			printf("    (never reached the bar in %.1f s: udsp %u of %u, speexdsp %u of %u)\n",
			       (double)SUMMARY_AFTER_FRAMES / FRAMES_PER_SECOND,
			       stats.own_never, stats.seeds, stats.oracle_never,
			       stats.seeds);
	}
}

/*
 * The weak-hybrid rows. The per-path table above validates on a 12 dB hybrid
 * driven by white noise, with a microphone that carries the echo and nothing
 * else. The field lines return 26 to 30 dB and their microphones carry the
 * near end's own noise floor as well.
 *
 * A row is specified by the echo-to-floor ratio, not by two absolute levels:
 * the fault is a function of how close the echo comes to the near end's own
 * noise. A live tracked ERLE of -5 to +10 dB puts a field line at about
 * 10 dB. The hybrid is the balance shape of lec_paths.h, calibrated against
 * the traces' own return-loss distribution, and the source is the word
 * source, which stops abruptly at the end of each word.
 *
 * What the filter removes is reported apart from what the far end hears. The
 * microphone of a noisy row carries more floor than echo, so the delivered
 * ratio is bounded by the floor whatever the canceller does; the filter
 * column takes the injected floor energy out of the residual, which is
 * unbiased because the floor and the misalignment are uncorrelated, and is
 * therefore the number that measures the canceller.
 *
 * Three arms see identical frames: the canceller with its NLP off, which is
 * the linear residual the speexdsp oracle compares against, the same
 * canceller with its NLP on, which is what the far caller hears, and the
 * oracle. nlp_apply runs after every tracker in dsp/lec.c and writes only the
 * output frame, so the two udsp arms take the same decisions and differ only
 * in the audio they emit; the row prints one decision column for both.
 *
 * The processor table after the weak-hybrid table reads the NLP arm of the
 * same runs: the verdict of each frame, the word onsets it engaged on and the
 * peak tap. The linear arm has its processor off and counts no verdict.
 */
#define WEAK_FRAMES		3000u
#define WEAK_WINDOW_FRAMES	10u
#define WEAK_OFFSET_FRAMES	10u
#define WEAK_ONSET_FRAMES	10u
#define WEAK_SETTLE_FRAMES	100u
/*
 * The shortest word of word_fill plus its pause is 48 frames, so at most 63
 * words start in a run. The engaged onsets are a subset of them.
 */
#define WEAK_ONSET_MAX		64u
_Static_assert(WEAK_FRAMES / 48u + 1u <= WEAK_ONSET_MAX,
	       "the engage array must hold one index per word of the run");
/* The detector's reference window is the tail rounded up to whole frames, so
 * the 48 ms tail spans five frames at both line rates; this bounds it. */
#define WEAK_RING_MAX		16u
/*
 * A block carrying less reference than this is a pause, and the loss it
 * reports is the ratio of two silences. -45 dBm0 is 20 dB under the word
 * level every row drives.
 */
#define WEAK_BLOCK_DBM0		-45.0
#define WEAK_FAR_SEED		0x13572468u

/*
 * field NULL builds the balance hybrid at erl_db. A field row builds the
 * captured path instead and sets its own far, echo and floor levels, so
 * erl_db, far_dbm0 and floor_dbm0 do not apply to it. ref_offset is in
 * samples at the line rate (struct ref_delay), and tail_ms 0 is
 * SUMMARY_TAIL_MS.
 */
struct weak_cfg {
	const char *name;
	double erl_db;
	double far_dbm0;
	double floor_dbm0;
	double near_dbm0;
	unsigned near_from;
	unsigned near_to;
	const struct field_path *field;
	int ref_offset;
	unsigned tail_ms;
};

/*
 * The energies of a field row on the frames the gate reads, and the levels
 * that the calibration of the row chose. reach is the energy of the floor
 * plus the echo of the taps outside the window of the filter.
 */
struct field_gate {
	double drive;
	double echo;
	double mic;
	double lin;
	double oracle;
	double floor;
	double reach;
	double word_dbm0;
	double h_scale;
};

struct weak_result {
	unsigned active;
	unsigned adapt;
	unsigned dt_single;
	unsigned near_active;
	unsigned near_held;
	unsigned near_flagged;
	unsigned windows;
	unsigned window_neg;
	unsigned offsets;
	unsigned probes;
	unsigned hold_run;
	double echo_floor_db;
	double erle_1s;
	double erle_end;
	double gain_end;
	double oracle_gain_end;
	double window_worst;
	double offset_echo;
	double offset_lin;
	double offset_nlp;
	double erl_p10;
	double erl_p50;
	float echo_gain;
	float coef_energy;
	float tracked_erle;
	unsigned far_active;
	unsigned far_acted;
	unsigned nlp_active;
	unsigned nlp_wait;
	unsigned nlp_erle;
	unsigned nlp_unlike;
	unsigned nlp_dt;
	unsigned onsets;
	unsigned onset_engaged;
	double engage_p50;
	double engage_max;
	double onset_echo;
	double onset_lin;
	double onset_nlp;
	/* 0 means never: the processor needs eight eligible frames to act, so
	 * no run acts on frame 0. */
	unsigned first_act;
	unsigned peak_tap;
	struct field_gate gate;
};

/*
 * The word bookkeeping of the processor table. An onset opens a window over
 * the first 100 ms of the word and opens the word; the far-end offset closes
 * the word. engage holds, for each engaged word, the frame of the word that
 * the processor first acted on; the onset frame is 1.
 */
struct weak_nlp {
	struct udsp_ec_nlms_info settled;
	unsigned window_left;
	unsigned word_frames;
	bool word_open;
	bool word_acted;
	double engage[WEAK_ONSET_MAX];
};

/* The fixed activity bar of dsp/lec.c, in the float arithmetic of the
 * canceller (dsp/lec.h), as a property of the signal. The bar of the canceller
 * equals it when the reference history holds a frame under the bar over
 * LEC_REF_FLOOR_MARGIN, and stands over it on a stationary far end. A
 * reference frame of the canceller meets bench_ref_bar, and a microphone or
 * a drive frame meets BENCH_ACTIVE_POWER, because the loss stands ahead of
 * the reference alone. A column that needs the view of the canceller reads
 * the frame reading instead. */
static bool weak_active_get(const int16_t *frame, unsigned count, float bar)
{
	return udsp_ec_nlms_frame_power_get(frame, count) >= bar;
}

static int weak_double_cmp(const void *first, const void *second)
{
	double a = *(const double *)first;
	double b = *(const double *)second;

	if (a < b)
		return -1;
	return a > b ? 1 : 0;
}

/*
 * What the filter removed: the residual with the injected floor energy taken
 * out. The floor and the misalignment are uncorrelated, so their energies
 * add and the subtraction is unbiased. A residual at or under the floor is
 * reported as the 200 dB sentinel erle_get already uses.
 *
 * The window column beside it needs no such correction: it asks whether the
 * canceller's output is louder than its own input, and the floor is in both
 * sides of it. A filter that adds energy is what the far caller hears as a
 * repeated syllable.
 */
static double weak_filter_erle_get(double echo, double residual, double floor)
{
	return erle_get(echo, residual - floor > 0.0 ? residual - floor : 0.0);
}

/*
 * One frame of the processor table, from the end of the settle second on.
 * onset is a word start with no near talker, the rule the offset window uses.
 */
static void weak_nlp_step(struct weak_nlp *t, struct weak_result *out,
			  bool active, bool onset, bool acted, bool offset,
			  const double *energy)
{
	if (active) {
		out->far_active++;
		if (acted)
			out->far_acted++;
	}
	if (onset) {
		out->onsets++;
		t->window_left = WEAK_ONSET_FRAMES;
		t->word_frames = 0;
		t->word_open = true;
		t->word_acted = false;
	}
	if (t->window_left) {
		t->window_left--;
		out->onset_echo += energy[0];
		out->onset_lin += energy[1];
		out->onset_nlp += energy[2];
	}
	if (!t->word_open)
		return;
	t->word_frames++;
	if (acted && !t->word_acted) {
		t->word_acted = true;
		t->engage[out->onset_engaged++] = t->word_frames;
	}
	t->word_open = !offset;
}

/* The verdict shares are deltas from the settle snapshot, so they describe
 * the converged filter only. */
static void weak_nlp_finish(struct weak_nlp *t,
			    const struct udsp_ec_nlms_info *end, unsigned frame,
			    struct weak_result *out)
{
	unsigned engaged = out->onset_engaged;
	unsigned samples = out->onsets * WEAK_ONSET_FRAMES * frame;

	out->nlp_active = end->nlp_active_frames -
			  t->settled.nlp_active_frames;
	out->nlp_wait = end->nlp_wait_frames - t->settled.nlp_wait_frames;
	out->nlp_erle = end->nlp_erle_frames - t->settled.nlp_erle_frames;
	out->nlp_unlike = end->nlp_unlike_frames -
			  t->settled.nlp_unlike_frames;
	out->nlp_dt = end->nlp_dt_frames - t->settled.nlp_dt_frames;
	out->peak_tap = end->coef_peak_tap;
	qsort(t->engage, engaged, sizeof(*t->engage), weak_double_cmp);
	out->engage_p50 = engaged ? t->engage[engaged / 2u] : NAN;
	out->engage_max = engaged ? t->engage[engaged - 1u] : NAN;
	if (!samples)
		return;
	out->onset_echo = rms_dbm0_get(sqrt(out->onset_echo / samples));
	out->onset_lin = rms_dbm0_get(sqrt(out->onset_lin / samples));
	out->onset_nlp = rms_dbm0_get(sqrt(out->onset_nlp / samples));
}

/*
 * The field rows. Each captured path of lec_paths_field.h runs with the
 * levels of its capture on far-active frames: the far rms of the table, the
 * echo that puts the uplink erle_64_db over the floor, and the residual rms
 * of the fit as the near-end floor. The gate reads the frames from
 * FIELD_GATE_FROM to the end of the run whose drive is active, and the
 * calibration reads the same frames, so the ceiling of a row equals the
 * erle_64_db of its capture.
 */
#define FIELD_GATE_FROM		1000u
#define FIELD_WORD_DBM0		-25.0
#define FIELD_HPF_HZ		300.0

typedef void far_fill_fn(void *source, int16_t *dst, unsigned frame);

struct field_hpf {
	double b0;
	double b1;
	double b2;
	double a1;
	double a2;
	double x[2];
	double y[2];
};

/*
 * The far end of a field row: the word source through a high-pass at 300 Hz,
 * ahead of the delay line and the path, so the reference and the path see one
 * signal. The far end of the capture came over the PSTN band, and the
 * response of a table under 300 Hz is fit noise, where the word source is
 * loud. At 16 kHz the word source runs at the 8 kHz of the wire and the 2:1
 * step of the engine takes it to the line rate, because the reference of the
 * capture carried nothing above 4 kHz.
 */
struct field_source {
	struct word_source word;
	struct udsp_resamp up;
	struct field_hpf hpf;
	unsigned rate;
};

/*
 * A second-order Butterworth high-pass by the bilinear transform, with the
 * corner prewarped: K = tan(pi fc / fs), n = 1 / (1 + sqrt(2) K + K^2),
 * b0 = b2 = n, b1 = -2 n, a1 = 2 (K^2 - 1) n, a2 = (1 - sqrt(2) K + K^2) n.
 * field_hpf_run() computes y[t] = b0 x[t] + b1 x[t-1] + b2 x[t-2] -
 * a1 y[t-1] - a2 y[t-2] from a zero state.
 */
static void field_hpf_init(struct field_hpf *f, unsigned rate)
{
	double k = tan(M_PI * FIELD_HPF_HZ / rate);
	double n = 1.0 / (1.0 + M_SQRT2 * k + k * k);

	memset(f, 0, sizeof(*f));
	f->b0 = n;
	f->b1 = -2.0 * n;
	f->b2 = n;
	f->a1 = 2.0 * (k * k - 1.0) * n;
	f->a2 = (1.0 - M_SQRT2 * k + k * k) * n;
}

static void field_hpf_run(struct field_hpf *f, int16_t *samples,
			  unsigned count)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		double x = samples[i];
		double y = f->b0 * x + f->b1 * f->x[0] + f->b2 * f->x[1] -
			   f->a1 * f->y[0] - f->a2 * f->y[1];

		f->x[1] = f->x[0];
		f->x[0] = x;
		f->y[1] = f->y[0];
		f->y[0] = y;
		samples[i] = sample_clamp(y);
	}
}

static void field_source_init(struct field_source *s, unsigned rate,
			      double word_dbm0)
{
	word_source_init(&s->word, FIELD_PATH_RATE, word_dbm0, WEAK_FAR_SEED);
	udsp_resamp_reset(&s->up);
	field_hpf_init(&s->hpf, rate);
	s->rate = rate;
}

static void field_source_fill(void *source, int16_t *dst, unsigned frame)
{
	struct field_source *s = source;
	int16_t narrow[UDSP_FRAME_MAX / 2u];

	if (s->rate == FIELD_PATH_RATE) {
		word_fill(&s->word, dst, frame);
	} else {
		word_fill(&s->word, narrow, frame / 2u);
		udsp_resamp_up2(&s->up, narrow, frame / 2u, dst);
	}
	field_hpf_run(&s->hpf, dst, frame);
}

static double frame_energy_get(const int16_t *samples, unsigned count)
{
	double energy = 0.0;
	unsigned i;

	for (i = 0; i < count; i++)
		energy += (double)samples[i] * samples[i];
	return energy;
}

/*
 * The fit reports the uplink b over the residual rms N, which holds the
 * floor, so the echo alone is sqrt(b^2 - N^2) with b = N * 10^(erle_64 / 20).
 */
static double field_echo_rms_get(const struct field_path *table)
{
	double floor = table->residual_rms;
	double uplink = floor * pow(10.0, table->erle_64_db / 20.0);

	return sqrt(uplink * uplink - floor * floor);
}

/*
 * A dry pass of the source through a scratch copy of the path, over the
 * frames the gate reads. The source enters at *word_dbm0. The drive and the
 * echo scale linearly with the word level and the taps, so the pass sets the
 * word level to the far rms of the table and scales the path to the echo rms
 * of the capture, on the frames that it read. The return value is the scale
 * of the taps.
 */
static double field_level_set(struct echo_path *path,
			      const struct field_path *table,
			      far_fill_fn *fill, void *source,
			      double *word_dbm0)
{
	static struct echo_path scratch;
	unsigned frame = path->frame;
	double drive_energy = 0.0;
	double echo_energy = 0.0;
	double samples = 0.0;
	double gain;
	double scale;
	unsigned f;
	unsigned k;

	scratch = *path;
	for (f = 0; f < WEAK_FRAMES; f++) {
		int16_t drive[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];

		fill(source, drive, frame);
		path_run(&scratch, drive, echo);
		if (f < FIELD_GATE_FROM ||
		    !weak_active_get(drive, frame, BENCH_ACTIVE_POWER))
			continue;
		drive_energy += frame_energy_get(drive, frame);
		echo_energy += frame_energy_get(echo, frame);
		samples += frame;
	}
	gain = table->far_rms / sqrt(drive_energy / samples);
	scale = field_echo_rms_get(table) /
		(gain * sqrt(echo_energy / samples));
	*word_dbm0 += 20.0 * log10(gain);
	for (k = 0; k < path->taps; k++)
		path->h[k] = (float)(path->h[k] * scale);
	return scale;
}

/*
 * A second path with the taps outside [offset, offset + taps): the taps that
 * a filter of that length cannot reach when its tap 0 sits offset samples
 * into the path. Its echo plus the floor is what an ideal filter leaves.
 */
static void field_reach_init(struct echo_path *reach,
			     const struct echo_path *path, int offset,
			     unsigned taps)
{
	unsigned k;

	*reach = *path;
	reach->taps = 0;
	for (k = 0; k < path->taps; k++) {
		int lag = (int)k - offset;

		if (lag >= 0 && lag < (int)taps)
			reach->h[k] = 0.0f;
		else
			reach->taps = k + 1u;
	}
}

/*
 * The frame after a word carries the tail of the high-pass, and whether it
 * crosses the activity bar depends on the level. The second pass reads the
 * frames that the new level makes active. Returns the near-end floor of the
 * row in dBm0.
 */
#define FIELD_LEVEL_PASSES	2u

static double field_row_setup(const struct weak_cfg *cfg, unsigned line_rate,
			      unsigned taps, struct echo_path *path,
			      struct echo_path *reach, struct field_source *far,
			      struct field_gate *gate)
{
	struct field_source probe;
	double word_dbm0 = FIELD_WORD_DBM0;
	unsigned pass;

	path_field_init(path, line_rate, cfg->field);
	gate->h_scale = 1.0;
	for (pass = 0; pass < FIELD_LEVEL_PASSES; pass++) {
		field_source_init(&probe, line_rate, word_dbm0);
		gate->h_scale *= field_level_set(path, cfg->field,
						 field_source_fill, &probe,
						 &word_dbm0);
	}
	gate->word_dbm0 = word_dbm0;
	field_reach_init(reach, path, cfg->ref_offset, taps);
	field_source_init(far, line_rate, word_dbm0);
	return rms_dbm0_get(cfg->field->residual_rms);
}

/* energy holds the echo, linear, NLP, speexdsp and floor energies of the
 * frame, in that order, as weak_run() fills it. */
static void field_gate_add(struct field_gate *g, const double *energy,
			   const int16_t *drive, const int16_t *mic,
			   const int16_t *hiss, const int16_t *reach,
			   unsigned frame)
{
	unsigned i;

	for (i = 0; i < frame; i++) {
		double left = (double)hiss[i] + reach[i];

		g->reach += left * left;
	}
	g->drive += frame_energy_get(drive, frame);
	g->mic += frame_energy_get(mic, frame);
	g->echo += energy[0];
	g->lin += energy[1];
	g->oracle += energy[3];
	g->floor += energy[4];
}

static void weak_run(const struct weak_cfg *cfg, unsigned line_rate,
		     struct weak_result *out)
{
	static double erl_samples[WEAK_FRAMES];
	static struct echo_path path;
	static struct echo_path reach;
	unsigned frame = line_rate / 100u;
	unsigned tail_ms = cfg->tail_ms ? cfg->tail_ms : SUMMARY_TAIL_MS;
	struct udsp_ec *lin = bench_ec_create(tail_ms, line_rate);
	struct udsp_ec *nlp = bench_ec_create(tail_ms, line_rate);
	unsigned taps = udsp_ec_nlms_tail_get(lin) * (line_rate / 1000u);
	SpeexEchoState *mdf = speex_echo_state_init((int)frame, (int)taps);
	struct udsp_ec_nlms_info info;
	struct udsp_ec_nlms_info ninfo;
	struct weak_nlp track;
	struct field_source far;
	struct word_source near;
	struct ref_delay delay;
	double floor_dbm0 = cfg->floor_dbm0;
	uint32_t floor_state = 0x0badcafeu;
	double ring[WEAK_RING_MAX];
	double first[3] = { 0.0, 0.0, 0.0 };
	double last[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
	double window[2] = { 0.0, 0.0 };
	double block[2] = { 0.0, 0.0 };
	double offset_floor = 0.0;
	double echo_peak = 0.0;
	unsigned erl_count = 0;
	unsigned offset_left = 0;
	unsigned hold_run = 0;
	bool window_clean = true;
	uint32_t held = 0;
	int rate = (int)line_rate;
	unsigned f;

	memset(out, 0, sizeof(*out));
	memset(ring, 0, sizeof(ring));
	memset(&track, 0, sizeof(track));
	out->window_worst = 200.0;
	speex_echo_ctl(mdf, SPEEX_ECHO_SET_SAMPLING_RATE, &rate);
	udsp_ec_nlms.nlp(lin, false);
	ref_delay_init(&delay, cfg->ref_offset);
	if (cfg->field) {
		floor_dbm0 = field_row_setup(cfg, line_rate, taps, &path,
					     &reach, &far, &out->gate);
	} else {
		path_weak_init(&path, line_rate, cfg->erl_db);
		path_geometry_set(&reach, line_rate);
		word_source_init(&far.word, line_rate, cfg->far_dbm0,
				 WEAK_FAR_SEED);
	}
	word_source_init(&near, line_rate, cfg->near_dbm0, 0xfeedfaceu);

	for (f = 0; f < WEAK_FRAMES; f++) {
		int16_t drive[UDSP_FRAME_MAX];
		int16_t path_in[UDSP_FRAME_MAX];
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t outside[UDSP_FRAME_MAX];
		int16_t talk[UDSP_FRAME_MAX];
		int16_t hiss[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		int16_t lin_out[UDSP_FRAME_MAX];
		int16_t nlp_out[UDSP_FRAME_MAX];
		int16_t oracle_out[UDSP_FRAME_MAX];
		int16_t ref_buf[UDSP_FRAME_MAX];
		const int16_t *ref_ec;
		double energy[5] = { 0.0, 0.0, 0.0, 0.0, 0.0 };
		double ref_power = 0.0;
		double window_power;
		bool talking = cfg->near_dbm0 < 0.0 && f >= cfg->near_from &&
			f < cfg->near_to;
		bool onset = far.word.talking && far.word.pos == 0;
		bool active;
		bool near_loud;
		unsigned i;

		if (cfg->field)
			field_source_fill(&far, drive, frame);
		else
			word_fill(&far.word, drive, frame);
		ref_delay_run(&delay, drive, frame, path_in, ref);
		path_run(&path, path_in, echo);
		path_run(&reach, path_in, outside);
		memset(talk, 0, frame * sizeof(*talk));
		memset(hiss, 0, frame * sizeof(*hiss));
		if (talking)
			word_fill(&near, talk, frame);
		near_loud = talking && weak_active_get(talk, frame,
						       BENCH_ACTIVE_POWER);
		if (floor_dbm0 < 0.0)
			noise_fill(hiss, frame, floor_dbm0, &floor_state);
		for (i = 0; i < frame; i++)
			mic[i] = sample_clamp((double)echo[i] + talk[i] +
					      hiss[i]);
		memcpy(lin_out, mic, frame * sizeof(*mic));
		memcpy(nlp_out, mic, frame * sizeof(*mic));
		ref_ec = bench_ref_get(ref, ref_buf, frame);
		udsp_ec_nlms.run(lin, lin_out, ref_ec);
		udsp_ec_nlms.run(nlp, nlp_out, ref_ec);
		speex_echo_cancellation(mdf, mic, ref, oracle_out);
		for (i = 0; i < frame; i++) {
			ref_power += (double)ref[i] * ref[i];
			energy[0] += (double)echo[i] * echo[i];
			energy[1] += (double)lin_out[i] * lin_out[i];
			energy[2] += (double)nlp_out[i] * nlp_out[i];
			energy[3] += (double)oracle_out[i] * oracle_out[i];
			energy[4] += (double)hiss[i] * hiss[i];
		}
		ref_power /= frame;
		ring[f % WEAK_RING_MAX] = ref_power;
		window_power = 0.0;
		for (i = 0; i < WEAK_RING_MAX; i++) {
			if (ring[i] > window_power)
				window_power = ring[i];
		}
		if (energy[0] / frame > echo_peak)
			echo_peak = energy[0] / frame;
		active = weak_active_get(ref_ec, frame, bench_ref_bar);
		if (cfg->field && f >= FIELD_GATE_FROM &&
		    weak_active_get(drive, frame, BENCH_ACTIVE_POWER))
			field_gate_add(&out->gate, energy, drive, mic, hiss,
				       outside, frame);

		udsp_ec_nlms_info_get(lin, &info);
		/* The escape spends active frames alone (path_change_get), so
		 * the run that matters to it skips the pauses. */
		if (active) {
			hold_run = info.hold_frames != held ? hold_run + 1u :
							      0u;
			if (hold_run > out->hold_run)
				out->hold_run = hold_run;
		}
		if (active && !near_loud) {
			out->active++;
			if (info.hold_frames == held)
				out->adapt++;
			if (info.double_talk)
				out->dt_single++;
		}
		if (active && near_loud) {
			out->near_active++;
			if (info.hold_frames != held)
				out->near_held++;
			if (info.double_talk)
				out->near_flagged++;
		}
		held = info.hold_frames;
		if (f == WEAK_SETTLE_FRAMES - 1u)
			out->tracked_erle = info.erle_db;

		if (f < WEAK_SETTLE_FRAMES) {
			first[0] += energy[0];
			first[1] += energy[1];
			first[2] += energy[4];
		}
		if (f + WEAK_SETTLE_FRAMES >= WEAK_FRAMES) {
			double mic_energy = 0.0;

			for (i = 0; i < frame; i++)
				mic_energy += (double)mic[i] * mic[i];
			last[0] += energy[0];
			last[1] += energy[1];
			last[2] += energy[3];
			last[3] += energy[4];
			last[4] += mic_energy;
		}
		if (offset_left) {
			offset_left--;
			out->offset_echo += energy[0];
			out->offset_lin += energy[1];
			out->offset_nlp += energy[2];
			offset_floor += energy[4];
		}
		/* The word stopped on this frame: in the 100 ms that follow,
		 * the far caller hears any echo of the word. */
		if (far.word.offset && !talking) {
			offset_left = WEAK_OFFSET_FRAMES;
			out->offsets++;
		}
		udsp_ec_nlms_info_get(nlp, &ninfo);
		if (ninfo.nlp_active && !out->first_act)
			out->first_act = f;
		if (f == WEAK_SETTLE_FRAMES - 1u)
			track.settled = ninfo;
		if (f >= WEAK_SETTLE_FRAMES)
			weak_nlp_step(&track, out, active, onset && !talking,
				      ninfo.nlp_active, far.word.offset,
				      energy);
		for (i = 0; i < frame; i++)
			window[0] += (double)mic[i] * mic[i];
		window[1] += energy[1];
		block[0] += ref_power * frame;
		block[1] += energy[0];
		if (talking)
			window_clean = false;
		if ((f + 1u) % WEAK_WINDOW_FRAMES)
			continue;
		if (rms_dbm0_get(sqrt(block[0] / (WEAK_WINDOW_FRAMES *
						  frame))) > WEAK_BLOCK_DBM0) {
			erl_samples[erl_count++] = 10.0 * log10(block[0] /
							        block[1]);
			if (window_clean) {
				double db = erle_get(window[0], window[1]);

				out->windows++;
				if (db < 0.0)
					out->window_neg++;
				if (db < out->window_worst)
					out->window_worst = db;
			}
		}
		window[0] = window[1] = 0.0;
		block[0] = block[1] = 0.0;
		window_clean = true;
	}
	qsort(erl_samples, erl_count, sizeof(*erl_samples), weak_double_cmp);
	out->erl_p10 = erl_count ? erl_samples[erl_count / 10u] : NAN;
	out->erl_p50 = erl_count ? erl_samples[erl_count / 2u] : NAN;
	out->erle_1s = weak_filter_erle_get(first[0], first[1], first[2]);
	out->erle_end = weak_filter_erle_get(last[0], last[1], last[3]);
	out->gain_end = erle_get(last[4], last[1]);
	out->oracle_gain_end = erle_get(last[4], last[2]);
	out->echo_floor_db = floor_dbm0 < 0.0 ?
		rms_dbm0_get(sqrt(echo_peak)) - floor_dbm0 : 200.0;
	if (out->offsets) {
		unsigned samples = out->offsets * WEAK_OFFSET_FRAMES * frame;

		out->offset_echo = rms_dbm0_get(sqrt(out->offset_echo /
						     samples));
		out->offset_lin = rms_dbm0_get(sqrt(out->offset_lin / samples));
		out->offset_nlp = rms_dbm0_get(sqrt(out->offset_nlp / samples));
		offset_floor = rms_dbm0_get(sqrt(offset_floor / samples));
	}
	udsp_ec_nlms_info_get(lin, &info);
	out->probes = info.probes;
	out->echo_gain = info.echo_gain;
	out->coef_energy = info.coef_energy;
	weak_nlp_finish(&track, &ninfo, frame, out);
	speex_echo_state_destroy(mdf);
	udsp_ec_nlms.destroy(lin);
	udsp_ec_nlms.destroy(nlp);
}

static double weak_pct_get(unsigned part, unsigned whole)
{
	return whole ? 100.0 * part / whole : 0.0;
}

static void weak_count_print(double value)
{
	if (isnan(value))
		printf(" %5s |", "-");
	else
		printf(" %5.0f |", value);
}

/* The column is 12 wide unless a row name is longer. */
static int weak_name_width_get(const struct weak_cfg *rows, unsigned count)
{
	size_t width = 12u;
	unsigned row;

	for (row = 0; row < count; row++) {
		if (strlen(rows[row].name) > width)
			width = strlen(rows[row].name);
	}
	return (int)width;
}

static void weak_nlp_print(unsigned line_rate, const struct weak_cfg *rows,
			   const struct weak_result *results, unsigned count)
{
	int width = weak_name_width_get(rows, count);
	unsigned row;

	printf("\nprocessor table at %u Hz: the NLP arm of the %s "
	       "rows above, the same runs\n", line_rate,
	       count && rows[0].field ? "field" : "weak-hybrid");
	printf("far, the shares, the onsets and the onset levels count from "
	       "frame %u, the end of\n", WEAK_SETTLE_FRAMES);
	printf("the settle second, because the table describes a converged "
	       "filter;\n");
	printf("far counts the far-active frames, and replaced is the share "
	       "of them the NLP replaced;\n");
	printf("W is the frames with an active reference window, the echo "
	       "tail of each word too;\n");
	printf("acted, wait, erle, unlike and dt split W: replaced, eligible "
	       "inside the engage\n");
	printf("count, refused by the ERLE gate, not echo-like, and double "
	       "talk;\n");
	printf("engaged is the share of the word onsets the NLP acted on "
	       "before the word stopped;\n");
	printf("engage is the frame of the word the NLP first acted on, the "
	       "onset frame is 1,\n");
	printf("the frame on which the engage run reaches 8 eligible frames; "
	       "a refused frame\n");
	printf("zeroes the run or cuts a complete run, so the frame is 1 when "
	       "a pause kept\n");
	printf("a complete run and the onset frame passes, and later when "
	       "onset frames fail\n");
	printf("the gate;\n");
	printf("p50 and max are over the engaged onsets;\n");
	printf("onset is the level in the first 100 ms of each word: echo, "
	       "linear, NLP out;\n");
	printf("first act is the first frame the NLP replaced, from frame 0 "
	       "of the run;\n");
	printf("peak is the tap of the largest coefficient at the end of the "
	       "run, and its delay;\n");
	printf("- means none\n");
	printf("  %-*s | %4s | %8s | %42s | %6s | %7s | %13s | %27s | %5s |"
	       " %12s\n", width, "row", "far", "replaced", "share of W",
	       "onsets", "engaged", "engage frame", "onset 100 ms dBm0",
	       "first", "peak tap");
	printf("  %-*s | %4s | %8s | %6s | %6s | %6s | %6s | %6s | %6s |"
	       " %7s | %5s | %5s | %7s | %7s | %7s | %5s | %4s | %5s\n", width,
	       "", "", "", "acted", "wait", "erle", "unlike", "dt", "", "",
	       "p50", "max", "echo", "linear", "nlp out", "act", "tap", "ms");
	for (row = 0; row < count; row++) {
		const struct weak_result *r = &results[row];
		unsigned judged = r->nlp_active + r->nlp_wait + r->nlp_erle +
				  r->nlp_unlike + r->nlp_dt;

		printf("  %-*s | %4u | %7.1f%% | %5.1f%% | %5.1f%% | %5.1f%% |"
		       " %5.1f%% | %5.1f%% | %6u | %6.0f%% |", width,
		       rows[row].name,
		       r->far_active, weak_pct_get(r->far_acted, r->far_active),
		       weak_pct_get(r->nlp_active, judged),
		       weak_pct_get(r->nlp_wait, judged),
		       weak_pct_get(r->nlp_erle, judged),
		       weak_pct_get(r->nlp_unlike, judged),
		       weak_pct_get(r->nlp_dt, judged), r->onsets,
		       weak_pct_get(r->onset_engaged, r->onsets));
		weak_count_print(r->engage_p50);
		weak_count_print(r->engage_max);
		printf(" %7.1f | %7.1f | %7.1f |", r->onset_echo, r->onset_lin,
		       r->onset_nlp);
		weak_count_print(r->first_act ? (double)r->first_act : NAN);
		printf(" %4u | %5.1f\n", r->peak_tap,
		       1000.0 * r->peak_tap / line_rate);
	}
}

static void weak_print(unsigned line_rate)
{
	static const struct weak_cfg rows[] = {
		{ .name = "quiet line", .erl_db = 28.0, .far_dbm0 = -25.0,
		  .floor_dbm0 = -70.0 },
		{ .name = "floor 13 dB", .erl_db = 28.0, .far_dbm0 = -25.0,
		  .floor_dbm0 = -57.0 },
		{ .name = "floor 11 dB", .erl_db = 28.0, .far_dbm0 = -25.0,
		  .floor_dbm0 = -55.0 },
		{ .name = "floor  9 dB", .erl_db = 28.0, .far_dbm0 = -25.0,
		  .floor_dbm0 = -53.0 },
		{ .name = "30 dB hybrid", .erl_db = 30.0, .far_dbm0 = -25.0,
		  .floor_dbm0 = -55.0 },
		{ .name = "double talk", .erl_db = 28.0, .far_dbm0 = -25.0,
		  .floor_dbm0 = -55.0, .near_dbm0 = -25.0, .near_from = 1000,
		  .near_to = 2000 },
	};
	struct weak_result results[sizeof(rows) / sizeof(rows[0])];
	unsigned count = sizeof(rows) / sizeof(rows[0]);
	unsigned row;

	printf("\nweak-hybrid rows at %u Hz, tail %u ms, balance hybrid, far end -25.0 dBm0 words\n",
	       line_rate, SUMMARY_TAIL_MS);
	printf("the source is speech-shaped words with unvoiced onsets that stop abruptly,\n");
	printf("with pauses of 300 ms and more; E/N is the echo peak over the near-end floor\n");
	printf("ERL p10/p50 is the hybrid's own return loss in 100 ms blocks (the reference\n");
	printf("traces read p10 26.0 and p50 30.4 dB);\n");
	printf("duty is the share of far-active single-talk frames the filter adapted on and dt\n");
	printf("the share it called double talk; the ERLE columns take the injected floor energy\n");
	printf("out of the residual, so they measure the FILTER; loss is the whole microphone\n");
	printf("over the whole output, which is what the line delivers, speexdsp beside it;\n");
	printf("neg counts the 100 ms single-talk windows whose output is LOUDER than their\n");
	printf("input, the state in which the canceller adds energy;\n");
	printf("offset is the level in the 100 ms after each word stop: echo, linear, NLP out\n");
	printf("  %-12s | %4s | %13s | %14s | %14s | %15s | %13s | %14s | %24s | %6s\n",
	       "row", "E/N", "hybrid ERL dB", "adaptation", "filter ERLE dB",
	       "delivered loss", "worst window", "escape",
	       "offset 100 ms dBm0", "state");
	printf("  %-12s | %4s | %6s | %6s | %5s | %6s | %6s | %6s | %6s | %8s | %6s | %6s | %6s | %6s | %7s | %7s | %7s | %6s\n",
	       "", "dB", "p10", "p50", "duty", "dt", "at 1 s", "at end",
	       "udsp", "speexdsp", "dB", "neg/N", "probes", "hold", "echo",
	       "linear", "nlp out", "gain");
	for (row = 0; row < count; row++) {
		const struct weak_result *r = &results[row];

		weak_run(&rows[row], line_rate, &results[row]);
		printf("  %-12s | %4.0f | %6.1f | %6.1f | %4.0f%% | %5.0f%% | %6.2f | %6.2f | %6.2f | %8.2f | %6.2f | %2u/%-4u| %6u | %6u | %7.1f | %7.1f | %7.1f | %6.5f\n",
		       rows[row].name, r->echo_floor_db, r->erl_p10, r->erl_p50,
		       weak_pct_get(r->adapt, r->active),
		       weak_pct_get(r->dt_single, r->active),
		       r->erle_1s, r->erle_end, r->gain_end, r->oracle_gain_end,
		       r->window_worst, r->window_neg, r->windows, r->probes,
		       r->hold_run, r->offset_echo, r->offset_lin,
		       r->offset_nlp, (double)r->echo_gain);
		if (!rows[row].near_from)
			continue;
		printf("    real double talk on the same hybrid: %u far-active frames with near speech, %.0f%% held, %.0f%% flagged\n",
		       r->near_active,
		       weak_pct_get(r->near_held, r->near_active),
		       weak_pct_get(r->near_flagged, r->near_active));
	}
	weak_nlp_print(line_rate, rows, results, count);
}

/*
 * Each table runs at both tails, with tap 0 of the filter 2 ms before the
 * onset and at the onset, in that order, so an onset row follows the row it
 * takes its cliff from. A table of an 8 kHz line has no 16 kHz rows.
 */
#define FIELD_ROWS_MAX		(4u * FIELD_PATHS_COUNT)
#define FIELD_NAME_MAX		17u
#define FIELD_LABEL_MAX		(FIELD_NAME_MAX + 24u)

static const unsigned field_tails[2] = { 48u, 64u };

static void field_name_get(const struct field_path *table, unsigned rate,
			   char *name, size_t size)
{
	snprintf(name, size, "%s%s", table->name,
		 rate == FIELD_PATH_RATE ? "" : " bl");
}

static unsigned field_rows_build(unsigned line_rate, struct weak_cfg *rows,
				 char (*labels)[FIELD_LABEL_MAX])
{
	unsigned count = 0;
	size_t i;

	for (i = 0; i < FIELD_ROWS_MAX; i++) {
		const struct field_path *table = &field_paths[i / 4u];
		struct weak_cfg *row = &rows[count];
		bool onset = i % 2u;
		char name[FIELD_NAME_MAX];

		if (table->line_rate < line_rate)
			continue;
		memset(row, 0, sizeof(*row));
		row->field = table;
		row->tail_ms = field_tails[i / 2u % 2u];
		row->ref_offset = onset ? (int)(table->onset_tap * line_rate /
						FIELD_PATH_RATE) : 0;
		field_name_get(table, line_rate, name, sizeof(name));
		snprintf(labels[count], FIELD_LABEL_MAX, "%s %u %s", name,
			 row->tail_ms, onset ? "onset" : "before");
		row->name = labels[count];
		count++;
	}
	return count;
}

static double field_fit_get(const struct weak_cfg *row)
{
	const struct field_path *table = row->field;

	if (row->tail_ms == field_tails[0])
		return row->ref_offset ? table->erle_48_onset_db :
					 table->erle_48_db;
	return row->ref_offset ? table->erle_64_onset_db : table->erle_64_db;
}

static void field_row_print(const struct weak_cfg *row,
			    const struct weak_result *r, unsigned line_rate,
			    double before_loss, double before_fit)
{
	const struct field_gate *g = &r->gate;
	double loss = erle_get(g->mic, g->lin);
	double fit = field_fit_get(row);
	char name[FIELD_NAME_MAX];

	field_name_get(row->field, line_rate, name, sizeof(name));
	printf("  %-14s | %4u | %-11s | %6.2f | %8.2f | %7.2f | %6.2f |"
	       " %6.2f | %6.1f | %6.1f | %8.2f |", name, row->tail_ms,
	       row->ref_offset ? "at onset" : "2 ms before", loss,
	       erle_get(g->mic, g->oracle), erle_get(g->mic, g->floor),
	       erle_get(g->mic, g->reach), erle_get(g->drive, g->echo),
	       row->field->uplink_db, fit, loss - fit);
	if (!row->ref_offset) {
		printf(" %6s | %6s\n", "-", "-");
		return;
	}
	printf(" %6.2f | %6.1f\n", before_loss - loss, before_fit - fit);
}

/*
 * The captured paths as weak-hybrid rows, one run each: a row is calibrated
 * and deterministic, so a seed list would repeat it.
 */
static void field_print(unsigned line_rate)
{
	static struct weak_cfg rows[FIELD_ROWS_MAX];
	static struct weak_result results[FIELD_ROWS_MAX];
	static char labels[FIELD_ROWS_MAX][FIELD_LABEL_MAX];
	unsigned count = field_rows_build(line_rate, rows, labels);
	double before_loss = NAN;
	double before_fit = NAN;
	unsigned row;

	printf("\nfield rows at %u Hz: the captured echo paths of real "
	       "lines, one run each\n", line_rate);
	printf("a row sets the far end, the echo and the near-end floor to "
	       "the levels of its\n");
	printf("capture on far-active frames; the far end is the word source "
	       "high-passed at\n");
	printf("%.0f Hz, because a table is fit noise under that frequency; "
	       "bl marks a\n", FIELD_HPF_HZ);
	printf("table interpolated to 16 kHz from the 8 kHz of the wire, with "
	       "the word source\n");
	printf("converted the same way;\n");
	printf("reference is the place of tap 0 of the filter against the "
	       "echo onset;\n");
	printf("the dB columns read the frames %u to %u whose drive is "
	       "active: loss is the\n", FIELD_GATE_FROM, WEAK_FRAMES - 1u);
	printf("microphone over the linear output, speexdsp the same for "
	       "that canceller,\n");
	printf("ceiling the microphone over the injected floor, reach the "
	       "microphone over the\n");
	printf("floor plus the echo of the taps outside the window of the "
	       "filter; ERL is the\n");
	printf("echo under the far end, header the uplink figure of the "
	       "capture; fit is the\n");
	printf("ideal ERLE of the capture at the tail and reference of the "
	       "row; cliff is the\n");
	printf("loss 2 ms before minus the loss at the onset, fit the same "
	       "for the fit\n");
	printf("  %-14s | %4s | %-11s | %36s | %15s | %17s | %15s\n", "row",
	       "tail", "reference", "dB on active frames", "ERL dB",
	       "fit dB", "cliff dB");
	printf("  %-14s | %4s | %-11s | %6s | %8s | %7s | %6s | %6s | %6s |"
	       " %6s | %8s | %6s | %6s\n", "", "ms", "", "loss", "speexdsp",
	       "ceiling", "reach", "bench", "header", "fit", "loss-fit",
	       "loss", "fit");
	for (row = 0; row < count; row++) {
		weak_run(&rows[row], line_rate, &results[row]);
		field_row_print(&rows[row], &results[row], line_rate,
				before_loss, before_fit);
		before_loss = erle_get(results[row].gate.mic,
				       results[row].gate.lin);
		before_fit = field_fit_get(&rows[row]);
	}
	printf("gate, printed and not asserted: a 64 ms row 2 ms before "
	       "reads a loss within\n");
	printf("3 dB of fit and a ceiling within 0.5 dB of fit; a 48 ms row "
	       "2 ms before reads a\n");
	printf("loss within 3 dB of reach; an onset row reads a cliff of "
	       "5 dB or more\n");
	weak_nlp_print(line_rate, rows, results, count);
}

/*
 * The talk-over rows. The weak-hybrid rows above drive one talker at a time.
 * On a field call the canceller held in single talk and did not hold in
 * talk-over: eight watchdog resets and four trials in 233 s, every reset
 * interval containing a talk-over second, and the strongest echo of the whole
 * call inside those intervals.
 *
 * Both sides run the word source here, at the levels a real call has: a
 * handset at its nominal -10 dBm0 against a far end that arrives quiet. The
 * near talker is skewed against the far one, because talk-over is a partial
 * overlap and two sources started together are a duet, not a conversation.
 * The skew sets how much of the call is talk-over, and the row prints it.
 *
 * The rows attribute each reset, which the 5 s field sampling could not do: a
 * reset is a trial abort or the plain zero branch, and dsp/lec.c tells them
 * apart in public state, because the abort restores the snapshot and the zero
 * branch leaves the coefficient energy at exactly zero. The row also counts
 * the frames the filter adapted on while only the far end talked, which is
 * what says whether a control that throttles talk-over also throttled the
 * learning.
 *
 * The echo column uses the field's own method: the regression gain of the
 * output on the injected echo over half-second windows, taken on the frames
 * the near end is silent, where the far caller hears their own echo in the
 * clear. A window above 0 dB is the canceller handing back more echo than the
 * hybrid returned, which the far caller hears as a burst of echo.
 *
 * The canceller of the rows runs with its processor off. It therefore cannot
 * show the near-end speech that the processor replaces with comfort noise. A
 * second instance with its processor on, the create default, runs the same
 * frames of the talk phase. It counts the frames it marks active on near-end
 * speech, and on the clean far-only frames. A second table repeats the rows
 * at TALK_QUIET_FLOOR_DBM0, where the processor can act.
 *
 * nlp_active marks the frames that the processor acted on. The release frame
 * of nlp_apply in dsp/lec.c still carries a one-frame cross-fade, with
 * nlp_active already false. The counts are therefore the frames marked
 * active, not every frame the processor modified.
 */
#define TALK_FRAMES		6000u
#define TALK_WINDOW		50u
#define TALK_WINDOW_MIN		10u
#define TALK_TAIL_MS		48u
#define TALK_FAR_DBM0		-25.0
#define TALK_NEAR_DBM0		-10.0
#define TALK_FLOOR_DBM0		-55.0
/*
 * The floor of the second table, that of the quiet line of the weak rows. At
 * TALK_FLOOR_DBM0 the tracked ERLE stays under the 6 dB minimum of the
 * processor gate (LEC_NLP_ERLE_MIN_DB in dsp/lec.c). The processor then never
 * acts, and the two replaced columns cannot move.
 */
#define TALK_QUIET_FLOOR_DBM0	-70.0
/* The hybrid the weak rows above are calibrated on (path_weak_init). */
#define TALK_ERL_DB		28.0
#define TALK_SETTLE_FRAMES	800u
/*
 * The bar the reconvergence after the talk-over is measured against: what the
 * filter removes, with the injected floor taken out of the residual, the way
 * the weak rows above measure it.
 */
#define TALK_SETTLE_DB		10.0
/*
 * A window this far above the echo the hybrid returned is a burst: the
 * canceller handed the far caller back more of their own voice than the line
 * did. The canceller reads +2.1 dB at 8 kHz and +0.6 dB at 16 kHz on the worst
 * window of these rows: a filter sitting at pass-through while the talk-over
 * denies it anything to learn from. Three decibels leaves that case room for
 * the regression estimate's own noise.
 */
#define TALK_BURST_DB		3.0

struct talk_result {
	unsigned overlap;
	unsigned far_active;
	unsigned clean;
	unsigned clean_adapt;
	unsigned near_replaced;
	unsigned clean_replaced;
	unsigned resets_trial;
	unsigned resets_zero;
	unsigned probes;
	unsigned windows;
	unsigned windows_hot;
	double worst_window;
	double settle_ms;
};

static void talk_run(unsigned line_rate, unsigned skew, double floor_dbm0,
		     struct talk_result *out)
{
	unsigned frame = line_rate / 100u;
	struct udsp_ec *ec = bench_ec_create(TALK_TAIL_MS, line_rate);
	struct udsp_ec *nlp = bench_ec_create(TALK_TAIL_MS, line_rate);
	struct udsp_ec_nlms_info info;
	struct udsp_ec_nlms_info ninfo;
	struct word_source far;
	struct word_source near;
	struct echo_path path;
	uint32_t floor_state = 0x0badcafeu;
	uint32_t held = 0;
	uint32_t resets = 0;
	double win[2] = { 0.0, 0.0 };
	double settle[3] = { 0.0, 0.0, 0.0 };
	unsigned win_n = 0;
	unsigned win_used = 0;
	unsigned f;

	memset(out, 0, sizeof(*out));
	out->worst_window = -200.0;
	out->settle_ms = NAN;
	udsp_ec_nlms.nlp(ec, false);
	path_weak_init(&path, line_rate, TALK_ERL_DB);
	word_source_init(&far, line_rate, TALK_FAR_DBM0, 0x13572468u);
	word_source_init(&near, line_rate, TALK_NEAR_DBM0, 0xfeedfaceu);
	for (f = 0; f < skew; f++) {
		int16_t skip[UDSP_FRAME_MAX];

		word_fill(&near, skip, frame);
	}
	for (f = 0; f < TALK_FRAMES; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t talk[UDSP_FRAME_MAX];
		int16_t hiss[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		int16_t nlp_out[UDSP_FRAME_MAX];
		int16_t ref_buf[UDSP_FRAME_MAX];
		const int16_t *ref_ec;
		double echo_energy = 0.0;
		double product = 0.0;
		bool far_on;
		bool near_on;
		unsigned i;

		word_fill(&far, ref, frame);
		word_fill(&near, talk, frame);
		path_run(&path, ref, echo);
		noise_fill(hiss, frame, floor_dbm0, &floor_state);
		for (i = 0; i < frame; i++)
			mic[i] = sample_clamp((double)echo[i] + talk[i] +
					      hiss[i]);
		ref_ec = bench_ref_get(ref, ref_buf, frame);
		far_on = weak_active_get(ref_ec, frame, bench_ref_bar);
		near_on = weak_active_get(talk, frame, BENCH_ACTIVE_POWER);
		memcpy(nlp_out, mic, frame * sizeof(*mic));
		udsp_ec_nlms.run(ec, mic, ref_ec);
		udsp_ec_nlms_info_get(ec, &info);
		udsp_ec_nlms.run(nlp, nlp_out, ref_ec);
		udsp_ec_nlms_info_get(nlp, &ninfo);
		if (near_on && ninfo.nlp_active)
			out->near_replaced++;
		for (i = 0; i < frame; i++) {
			echo_energy += (double)echo[i] * echo[i];
			product += (double)mic[i] * echo[i];
		}
		if (info.diverge_resets != resets) {
			resets = info.diverge_resets;
			/* The zero branch leaves nothing behind; the trial
			 * abort puts the snapshot back. */
			if (info.coef_energy > 0.0f)
				out->resets_trial++;
			else
				out->resets_zero++;
		}
		if (far_on) {
			out->far_active++;
			if (near_on)
				out->overlap++;
		}
		if (far_on && !near_on) {
			out->clean++;
			if (info.hold_frames == held)
				out->clean_adapt++;
			if (ninfo.nlp_active)
				out->clean_replaced++;
			win[0] += product;
			win[1] += echo_energy;
			win_used++;
		}
		held = info.hold_frames;
		if (++win_n < TALK_WINDOW)
			continue;
		if (win_used >= TALK_WINDOW_MIN && win[1] > 0.0) {
			double gain = 20.0 * log10(fabs(win[0] / win[1]) +
						   1e-12);

			out->windows++;
			if (gain > TALK_BURST_DB)
				out->windows_hot++;
			if (gain > out->worst_window)
				out->worst_window = gain;
		}
		win_n = 0;
		win_used = 0;
		win[0] = win[1] = 0.0;
	}
	udsp_ec_nlms.destroy(nlp);
	/* The talk-over ends and the far end is alone: how long until the
	 * filter is worth having again. */
	for (f = 0; f < TALK_SETTLE_FRAMES; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t hiss[UDSP_FRAME_MAX];
		int16_t mic[UDSP_FRAME_MAX];
		unsigned i;

		noise_fill(ref, frame, TALK_FAR_DBM0, &floor_state);
		path_run(&path, ref, echo);
		noise_fill(hiss, frame, floor_dbm0, &floor_state);
		for (i = 0; i < frame; i++)
			mic[i] = sample_clamp((double)echo[i] + hiss[i]);
		bench_ec_run(ec, mic, ref, frame);
		for (i = 0; i < frame; i++) {
			settle[0] += (double)echo[i] * echo[i];
			settle[1] += (double)mic[i] * mic[i];
			settle[2] += (double)hiss[i] * hiss[i];
		}
		if ((f + 1u) % 25u)
			continue;
		if (isnan(out->settle_ms) &&
		    weak_filter_erle_get(settle[0], settle[1], settle[2]) >=
		    TALK_SETTLE_DB)
			out->settle_ms = (double)(f + 1u) * 10.0;
		settle[0] = settle[1] = settle[2] = 0.0;
	}
	udsp_ec_nlms_info_get(ec, &info);
	out->probes = info.probes;
	udsp_ec_nlms.destroy(ec);
}

static void talk_table_print(unsigned line_rate, double floor_dbm0)
{
	static const unsigned skews[] = { 0u, 15u, 22u, 30u, 45u };
	unsigned row;

	printf("  %-8s | %7s | %23s | %6s | %15s | %6s | %9s | %9s | %10s\n",
	       "skew", "overlap", "resets", "probes", "echo windows",
	       "adapt", "reconverge", "near repl", "clean repl");
	printf("  %-8s | %7s | %6s | %6s | %5s | %6s | %6s | %6s | %6s | %9s |"
	       " %9s | %10s\n", "frames", "", "trial", "zero", "total", "",
	       "hot/N", "worst", "", "ms", "frames", "frames");
	for (row = 0; row < sizeof(skews) / sizeof(skews[0]); row++) {
		struct talk_result r;
		char hot[24];

		talk_run(line_rate, skews[row], floor_dbm0, &r);
		snprintf(hot, sizeof(hot), "%u/%u", r.windows_hot, r.windows);
		printf("  %8u | %6.0f%% | %6u | %6u | %5u | %6u | %6s | %6.1f | %5.0f%% | ",
		       skews[row],
		       r.far_active ? 100.0 * r.overlap / r.far_active : 0.0,
		       r.resets_trial, r.resets_zero,
		       r.resets_trial + r.resets_zero, r.probes, hot,
		       r.worst_window,
		       r.clean ? 100.0 * r.clean_adapt / r.clean : 0.0);
		if (isnan(r.settle_ms))
			printf("%9s", "never");
		else
			printf("%9.0f", r.settle_ms);
		printf(" | %9u | %10u\n", r.near_replaced, r.clean_replaced);
	}
}

static void talk_print(unsigned line_rate)
{
	printf("\ntalk-over rows at %u Hz, tail %u ms, balance hybrid, far end %.1f dBm0 words,\n",
	       line_rate, TALK_TAIL_MS, TALK_FAR_DBM0);
	printf("near end %.1f dBm0 words skewed against them, near-end floor %.1f dBm0\n",
	       TALK_NEAR_DBM0, TALK_FLOOR_DBM0);
	printf("overlap is the share of far-active frames the near end also talks on;\n");
	printf("resets are split into trial aborts and the plain zero branch, which the\n");
	printf("field sampling could not resolve; adapt is the share of the CLEAN far-only\n");
	printf("frames the filter still learned from; echo is the regression gain of the\n");
	printf("output on the injected echo over 0.5 s of near-end silence, the field method,\n");
	printf("and hot counts the windows above %.0f dB, where the canceller hands back MORE\n",
	       TALK_BURST_DB);
	printf("echo than the hybrid returned\n");
	printf("near repl counts the near-end speech frames the NLP arm marks "
	       "active;\n");
	printf("clean repl counts the CLEAN far-only frames the NLP arm marks "
	       "active\n");
	talk_table_print(line_rate, TALK_FLOOR_DBM0);
	printf("\ntalk-over rows at %u Hz, near-end floor %.1f dBm0, "
	       "the rest as above\n", line_rate, TALK_QUIET_FLOOR_DBM0);
	talk_table_print(line_rate, TALK_QUIET_FLOOR_DBM0);
}

/*
 * The far-end noise rows. A stationary far-end noise over the activity bar of
 * dsp/lec.c keeps the reference window active. The detector then reads near
 * speech as double talk for as long as the near party talks. After
 * LEC_GEIGEL_LATCH_MAX frames of such a run, the latch starts a trial that
 * adapts on the near speech. The rows measure what the near speech and the
 * filter lose to that.
 *
 * The talker rows step the noise across the bar and across -38 dBm0. With the
 * learned gain at its floor, the bound of echo_bound_get puts the echo of
 * -38 dBm0 at -65 dBm0. The hybrid itself returns that echo at -68 dBm0.
 *
 * The pauses of the dense talker are shorter than the long hangover of the
 * detector, so one verdict bridges a pause. The double-talk run therefore does
 * not decay between two words. The restart comes before the shortest pause of
 * the word source, where word_fill moves the word index itself. Every word of
 * the dense talker therefore has the same length and color.
 *
 * A gap of the noise alone separates the talker from the far-end words before
 * and after it. The gap is longer than the reference window of the detector.
 * On a noise under the bar, the window of a far-end word that the first word
 * phase cuts therefore closes before the talker starts. The gap is also
 * longer than the long hangover, so the last near word cannot hold the last
 * far-end words in double talk.
 *
 * The untrained row puts a near-end noise over the echo from the first frame,
 * so the learned gain does not fall to its floor. The louder-path row steps
 * the hybrid while the far end carries the noise alone. It shows how long the
 * louder echo returns before a trial or the far-end words teach the filter the
 * new path.
 *
 * Two instances run every frame of a row. The first has its processor on, the
 * create default, and gives the trials, the verdicts and the near speech. The
 * second has its processor off and gives the filter ERLE. Reason: the
 * processor of the first replaces the output on the frames that it acts on.
 */
#define FAR_NOISE_SILENT_FRAMES	300u
#define FAR_NOISE_WORD_FRAMES	1000u
#define FAR_NOISE_GAP_FRAMES	30u
#define FAR_NOISE_TALK_FRAMES	4000u
#define FAR_NOISE_LEAD_TO	(FAR_NOISE_SILENT_FRAMES + \
				 FAR_NOISE_WORD_FRAMES)
#define FAR_NOISE_TALK_FROM	(FAR_NOISE_LEAD_TO + FAR_NOISE_GAP_FRAMES)
#define FAR_NOISE_TALK_TO	(FAR_NOISE_TALK_FROM + FAR_NOISE_TALK_FRAMES)
#define FAR_NOISE_LAST_FROM	(FAR_NOISE_TALK_TO + FAR_NOISE_GAP_FRAMES)
#define FAR_NOISE_FRAMES	(FAR_NOISE_LAST_FROM + FAR_NOISE_WORD_FRAMES)
#define FAR_NOISE_PAUSE_FRAMES	15u
#define FAR_NOISE_ERL_DB	30.0
#define FAR_NOISE_FLOOR_DBM0	-72.0
#define FAR_NOISE_WORDS_DBM0	-20.0
#define FAR_NOISE_TALK_DBM0	-25.0
#define FAR_NOISE_NEAR_DBM0	-35.0
#define FAR_NOISE_STEP_FRAME	1500u
#define FAR_NOISE_STEP_ERL_DB	10.0
#define FAR_NOISE_RETURN_FRAME	2500u
#define FAR_NOISE_STEP_ERLE_FROM	3500u
#define FAR_NOISE_STEP_FRAMES	4500u
#define FAR_NOISE_SECOND_FIRST	13u
#define FAR_NOISE_SECONDS	32u
#define FAR_NOISE_LINE_SECONDS	16u

enum far_noise_kind {
	FAR_NOISE_TALKER,
	FAR_NOISE_UNTRAINED,
	FAR_NOISE_STEP,
};

struct far_noise_row {
	const char *name;
	double noise_dbm0;
	enum far_noise_kind kind;
};

/* What a frame of a row carries, by the timeline of its kind. erle marks the
 * frames whose far-end words the ERLE column reads. */
struct far_noise_phase {
	bool noise;
	bool words;
	bool talker;
	bool near_noise;
	bool erle;
};

/* silent counts the zero frames of the dense talker since its last word.
 * words says whether the frame before was in a word phase, so a pause inside
 * a word phase reads true. */
struct far_noise_line {
	struct udsp_ec *ec;
	struct udsp_ec *lin;
	struct echo_path path;
	struct word_source far;
	struct word_source near;
	double noise_dbm0;
	uint32_t noise_state;
	uint32_t near_state;
	uint32_t floor_state;
	unsigned frame;
	unsigned silent;
	bool words;
};

/* floor holds the near-end floor and, on the untrained row, the near-end
 * noise: everything the near end injects that is not speech. */
struct far_noise_frame {
	int16_t words[UDSP_FRAME_MAX];
	int16_t ref[UDSP_FRAME_MAX];
	int16_t echo[UDSP_FRAME_MAX];
	int16_t talk[UDSP_FRAME_MAX];
	int16_t floor[UDSP_FRAME_MAX];
	int16_t out[UDSP_FRAME_MAX];
	int16_t lin[UDSP_FRAME_MAX];
	struct udsp_ec_nlms_info info;
	struct udsp_ec_nlms_frame reading;
};

/*
 * gaps counts the talker frames whose talker plane is all zeros, and
 * near_frames the talker frames whose talker crosses the activity bar. A word
 * frame under the bar counts in neither. added_energy is the energy of the
 * output less the near speech and the floor plane over the near frames. erle
 * holds the echo, the output of the instance with its processor off and the
 * floor energies. The fields from trial_frame on belong to the louder-path
 * row. second_energy and second_samples cover the seconds from
 * FAR_NOISE_SECOND_FIRST on. floor_at holds the floor when
 * FAR_NOISE_STEP_FRAME and FAR_NOISE_RETURN_FRAME start.
 */
struct far_noise_result {
	uint32_t probes;
	uint32_t resets;
	unsigned talk_frames;
	unsigned talk_dt;
	unsigned gaps;
	unsigned gaps_quiet;
	unsigned near_frames;
	unsigned exposure;
	unsigned near_repl;
	double near_product;
	double near_energy;
	double added_energy;
	double erle[3];
	float noise_dbov;
	float echo_gain;
	unsigned trial_frame;
	unsigned trial_arm;
	double second_energy[FAR_NOISE_SECONDS];
	unsigned second_samples[FAR_NOISE_SECONDS];
	float floor_at[2];
};

static void far_noise_phase_get(enum far_noise_kind kind, unsigned f,
				struct far_noise_phase *p)
{
	memset(p, 0, sizeof(*p));
	if (kind == FAR_NOISE_STEP) {
		p->noise = true;
		p->words = f < FAR_NOISE_WORD_FRAMES ||
			   f >= FAR_NOISE_RETURN_FRAME;
		p->erle = f >= FAR_NOISE_STEP_ERLE_FROM;
		return;
	}
	p->near_noise = kind == FAR_NOISE_UNTRAINED;
	p->noise = p->near_noise || f >= FAR_NOISE_SILENT_FRAMES;
	p->talker = f >= FAR_NOISE_TALK_FROM && f < FAR_NOISE_TALK_TO;
	p->erle = f >= FAR_NOISE_LAST_FROM;
	p->words = p->erle || (kind == FAR_NOISE_TALKER && p->noise &&
			       f < FAR_NOISE_LEAD_TO);
}

static void far_noise_line_init(struct far_noise_line *line,
				const struct far_noise_row *row,
				unsigned line_rate)
{
	memset(line, 0, sizeof(*line));
	line->frame = line_rate / 100u;
	line->ec = bench_ec_create(0, line_rate);
	line->lin = bench_ec_create(0, line_rate);
	udsp_ec_nlms.nlp(line->lin, false);
	path_flat_init(&line->path, line_rate, 8, FAR_NOISE_ERL_DB);
	word_source_init(&line->far, line_rate, FAR_NOISE_WORDS_DBM0,
			 0x13572468u);
	word_source_init(&line->near, line_rate, FAR_NOISE_TALK_DBM0,
			 0xfeedfaceu);
	line->noise_dbm0 = row->noise_dbm0;
	line->noise_state = 0x0f00f00du;
	line->near_state = 0x600dcafeu;
	line->floor_state = 0x0badcafeu;
}

static void far_noise_talk_fill(struct far_noise_line *line, int16_t *dst)
{
	if (!line->near.talking && line->silent == FAR_NOISE_PAUSE_FRAMES)
		word_source_restart(&line->near);
	word_fill(&line->near, dst, line->frame);
	if (line->near.talking || line->near.offset)
		line->silent = 0;
	else
		line->silent++;
}

/* The far end of the frame: the noise plus the words, and a word phase
 * starts on a word. */
static void far_noise_ref_fill(struct far_noise_line *line,
			       const struct far_noise_phase *p,
			       struct far_noise_frame *fr)
{
	unsigned i;

	memset(fr->words, 0, sizeof(fr->words));
	memset(fr->ref, 0, sizeof(fr->ref));
	if (p->words && !line->words)
		word_source_restart(&line->far);
	line->words = p->words;
	if (p->words)
		word_fill(&line->far, fr->words, line->frame);
	if (p->noise)
		noise_fill(fr->ref, line->frame, line->noise_dbm0,
			   &line->noise_state);
	for (i = 0; i < line->frame; i++)
		fr->ref[i] = sample_clamp((double)fr->ref[i] + fr->words[i]);
}

static void far_noise_near_fill(struct far_noise_line *line,
				const struct far_noise_phase *p,
				struct far_noise_frame *fr)
{
	int16_t noise[UDSP_FRAME_MAX];
	unsigned i;

	memset(fr->talk, 0, sizeof(fr->talk));
	if (p->talker)
		far_noise_talk_fill(line, fr->talk);
	noise_fill(fr->floor, line->frame, FAR_NOISE_FLOOR_DBM0,
		   &line->floor_state);
	if (!p->near_noise)
		return;
	noise_fill(noise, line->frame, FAR_NOISE_NEAR_DBM0, &line->near_state);
	for (i = 0; i < line->frame; i++)
		fr->floor[i] = sample_clamp((double)fr->floor[i] + noise[i]);
}

static void far_noise_frame_run(struct far_noise_line *line,
				const struct far_noise_phase *p,
				struct far_noise_frame *fr)
{
	unsigned i;

	far_noise_ref_fill(line, p, fr);
	far_noise_near_fill(line, p, fr);
	path_run(&line->path, fr->ref, fr->echo);
	for (i = 0; i < line->frame; i++)
		fr->out[i] = sample_clamp((double)fr->echo[i] + fr->talk[i] +
					  fr->floor[i]);
	memcpy(fr->lin, fr->out, line->frame * sizeof(*fr->out));
	bench_ec_run(line->ec, fr->out, fr->ref, line->frame);
	bench_ec_run(line->lin, fr->lin, fr->ref, line->frame);
	udsp_ec_nlms_info_get(line->ec, &fr->info);
	udsp_ec_nlms_frame_get(line->ec, &fr->reading);
}

/* One frame of the dense talker: the verdict columns, the gap columns on a
 * frame without a near word, and the near-speech columns on a frame whose
 * talker crosses the activity bar. */
static void far_noise_talk_add(struct far_noise_result *r,
			       const struct far_noise_frame *fr, unsigned frame)
{
	double power = frame_energy_get(fr->talk, frame) / frame;
	unsigned i;

	r->talk_frames++;
	if (fr->info.double_talk)
		r->talk_dt++;
	if (power == 0.0) {
		r->gaps++;
		if (!fr->reading.window_run)
			r->gaps_quiet++;
		return;
	}
	if (!weak_active_get(fr->talk, frame, BENCH_ACTIVE_POWER))
		return;
	r->near_frames++;
	if (fr->reading.estimate_power >= power / 10.0)
		r->exposure++;
	if (fr->info.nlp_active)
		r->near_repl++;
	for (i = 0; i < frame; i++) {
		double added = (double)fr->out[i] - fr->talk[i] - fr->floor[i];

		r->near_product += (double)fr->out[i] * fr->talk[i];
		r->near_energy += (double)fr->talk[i] * fr->talk[i];
		r->added_energy += added * added;
	}
}

static void far_noise_erle_add(struct far_noise_result *r,
			       const struct far_noise_frame *fr, unsigned frame)
{
	r->erle[0] += frame_energy_get(fr->echo, frame);
	r->erle[1] += frame_energy_get(fr->lin, frame);
	r->erle[2] += frame_energy_get(fr->floor, frame);
}

static void far_noise_step_add(struct far_noise_result *r,
			       const struct far_noise_frame *fr, unsigned f,
			       unsigned frame)
{
	unsigned second = f / FRAMES_PER_SECOND;

	if (f + 1u == FAR_NOISE_STEP_FRAME)
		r->floor_at[0] = fr->info.noise_dbov;
	if (f + 1u == FAR_NOISE_RETURN_FRAME)
		r->floor_at[1] = fr->info.noise_dbov;
	if (f >= FAR_NOISE_STEP_FRAME &&
	    r->trial_arm == UDSP_EC_NLMS_TRIAL_ARM_NONE &&
	    fr->reading.trial_arm != UDSP_EC_NLMS_TRIAL_ARM_NONE) {
		r->trial_frame = f;
		r->trial_arm = fr->reading.trial_arm;
	}
	if (second < FAR_NOISE_SECOND_FIRST ||
	    second >= FAR_NOISE_SECOND_FIRST + FAR_NOISE_SECONDS ||
	    weak_active_get(fr->words, frame, BENCH_ACTIVE_POWER))
		return;
	second -= FAR_NOISE_SECOND_FIRST;
	r->second_energy[second] += frame_energy_get(fr->out, frame);
	r->second_samples[second] += frame;
}

static void far_noise_run(const struct far_noise_row *row, unsigned line_rate,
			  struct far_noise_result *out)
{
	static struct far_noise_line line;
	unsigned frames = row->kind == FAR_NOISE_STEP ? FAR_NOISE_STEP_FRAMES :
							FAR_NOISE_FRAMES;
	struct far_noise_phase p;
	struct far_noise_frame fr;
	unsigned f;

	memset(out, 0, sizeof(*out));
	far_noise_line_init(&line, row, line_rate);
	for (f = 0; f < frames; f++) {
		far_noise_phase_get(row->kind, f, &p);
		if (row->kind == FAR_NOISE_STEP && f == FAR_NOISE_STEP_FRAME)
			path_erl_set(&line.path, FAR_NOISE_STEP_ERL_DB);
		far_noise_frame_run(&line, &p, &fr);
		if (p.talker)
			far_noise_talk_add(out, &fr, line.frame);
		if (p.erle && weak_active_get(fr.words, line.frame,
					      BENCH_ACTIVE_POWER))
			far_noise_erle_add(out, &fr, line.frame);
		if (row->kind == FAR_NOISE_STEP)
			far_noise_step_add(out, &fr, f, line.frame);
	}
	out->probes = fr.info.probes;
	out->resets = fr.info.diverge_resets;
	out->noise_dbov = fr.info.noise_dbov;
	out->echo_gain = fr.info.echo_gain;
	udsp_ec_nlms.destroy(line.ec);
	udsp_ec_nlms.destroy(line.lin);
}

/* The dBov scale of dsp/cn.c reads 127 while no floor is measured. */
static void far_noise_dbov_print(float dbov, int width)
{
	if (dbov >= 127.0f)
		printf("%*s", width, "none");
	else
		printf("%*.1f", width, (double)dbov);
}

/* NAN is a figure with no frame behind it. */
static void far_noise_db_print(double db, int width)
{
	if (isnan(db))
		printf(" %*s |", width, "n/a");
	else
		printf(" %*.2f |", width, db);
}

static void far_noise_talk_print(const struct far_noise_result *r)
{
	double loss = NAN;
	double added = NAN;

	if (r->near_frames)
		loss = -20.0 * log10(fabs(r->near_product / r->near_energy));
	if (r->near_frames && r->added_energy > 0.0)
		added = 10.0 * log10(r->near_energy / r->added_energy);
	printf(" %5.1f%% |", weak_pct_get(r->talk_dt, r->talk_frames));
	if (r->gaps)
		printf(" %5.1f%% |", weak_pct_get(r->gaps_quiet, r->gaps));
	else
		printf(" %6s |", "n/a");
	printf(" %8u |", r->exposure);
	far_noise_db_print(loss, 9);
	far_noise_db_print(added, 7);
	printf(" %9u |", r->near_repl);
}

static void far_noise_row_print(const struct far_noise_row *row,
				const struct far_noise_result *r)
{
	printf("  %-11s | %5.0f | %6u | %6u |", row->name, row->noise_dbm0,
	       r->probes, r->resets);
	if (r->talk_frames)
		far_noise_talk_print(r);
	else
		printf(" %6s | %6s | %8s | %9s | %7s | %9s |", "-", "-", "-",
		       "-", "-", "-");
	far_noise_db_print(weak_filter_erle_get(r->erle[0], r->erle[1],
						r->erle[2]), 6);
	printf(" ");
	far_noise_dbov_print(r->noise_dbov, 7);
	printf(" | %6.1f\n", 10.0 * log10(r->echo_gain));
}

static void far_noise_step_print(const struct far_noise_result *r)
{
	unsigned s;

	printf("louder path: ");
	if (r->trial_arm == UDSP_EC_NLMS_TRIAL_ARM_NONE)
		printf("no trial after the step\n");
	else
		printf("the first trial after the step starts at frame %u, "
		       "arm %s\n", r->trial_frame,
		       lecr_trial_arm_name(r->trial_arm));
	printf("louder path: output dBm0 per second over the frames without "
	       "a far-end word\n");
	for (s = 0; s < FAR_NOISE_SECONDS; s++) {
		unsigned first = FAR_NOISE_SECOND_FIRST + s;
		double samples = r->second_samples[s];

		if (!(s % FAR_NOISE_LINE_SECONDS))
			printf("  %2u to %2u s:", first,
			       first + FAR_NOISE_LINE_SECONDS - 1u);
		if (samples > 0.0)
			printf(" %3.0f", rms_dbm0_get(sqrt(r->second_energy[s] /
							   samples)));
		else
			printf(" %3s", "-");
		if (s % FAR_NOISE_LINE_SECONDS == FAR_NOISE_LINE_SECONDS - 1u)
			printf("\n");
	}
	printf("louder path: floor dBov when frame %u starts ",
	       FAR_NOISE_STEP_FRAME);
	far_noise_dbov_print(r->floor_at[0], 0);
	printf(", when frame %u starts ", FAR_NOISE_RETURN_FRAME);
	far_noise_dbov_print(r->floor_at[1], 0);
	printf(", at the end ");
	far_noise_dbov_print(r->noise_dbov, 0);
	printf("\n");
}

static void far_noise_table_print(unsigned line_rate)
{
	static const struct far_noise_row rows[] = {
		{ "talker", -54.0, FAR_NOISE_TALKER },
		{ "talker", -48.0, FAR_NOISE_TALKER },
		{ "talker", -42.0, FAR_NOISE_TALKER },
		{ "talker", -36.0, FAR_NOISE_TALKER },
		{ "talker", -30.0, FAR_NOISE_TALKER },
		{ "talker", -24.0, FAR_NOISE_TALKER },
		{ "untrained", -45.0, FAR_NOISE_UNTRAINED },
		{ "louder path", -45.0, FAR_NOISE_STEP },
	};
	struct far_noise_result step;
	struct far_noise_result r;
	bool stepped = false;
	unsigned row;

	printf("  %-11s | %5s | %6s | %6s | %6s | %6s | %8s | %9s | %7s |"
	       " %9s | %6s | %7s | %6s\n", "row", "noise", "trials", "resets",
	       "dt", "quiet", "exposure", "near loss", "added", "near repl",
	       "ERLE", "floor", "gain");
	printf("  %-11s | %5s | %6s | %6s | %6s | %6s | %8s | %9s | %7s |"
	       " %9s | %6s | %7s | %6s\n", "", "dBm0", "", "", "", "", "frames",
	       "dB", "dB", "frames", "dB", "dBov", "dB");
	for (row = 0; row < sizeof(rows) / sizeof(rows[0]); row++) {
		far_noise_run(&rows[row], line_rate, &r);
		far_noise_row_print(&rows[row], &r);
		if (rows[row].kind != FAR_NOISE_STEP)
			continue;
		step = r;
		stepped = true;
	}
	if (stepped)
		far_noise_step_print(&step);
}

static void far_noise_print(unsigned line_rate)
{
	struct udsp_ec *ec = bench_ec_create(0, line_rate);
	unsigned tail_ms = udsp_ec_nlms_tail_get(ec);

	udsp_ec_nlms.destroy(ec);
	printf("\nfar-end noise rows at %u Hz, tail %u ms, flat hybrid of "
	       "%.0f dB ERL at 8 ms,\n", line_rate, tail_ms, FAR_NOISE_ERL_DB);
	printf("near-end floor %.1f dBm0; the far-end noise and the floor "
	       "are white\n", FAR_NOISE_FLOOR_DBM0);
	printf("the rows at -54, -48 and -42 dBm0 stand under -38 dBm0, the "
	       "rows at -36, -30\n");
	printf("and -24 dBm0 over it; the bound of echo_bound_get with the "
	       "learned gain at its\n");
	printf("floor puts the echo of -38 dBm0 at -65 dBm0, the hybrid "
	       "itself at -68 dBm0\n");
	printf("a talker row: %u s of far-end silence, then the noise to the "
	       "end of the row;\n",
	       FAR_NOISE_SILENT_FRAMES / FRAMES_PER_SECOND);
	printf("%u s of far-end words at %.1f dBm0 on the noise, the near "
	       "end silent;\n", FAR_NOISE_WORD_FRAMES / FRAMES_PER_SECOND,
	       FAR_NOISE_WORDS_DBM0);
	printf("a gap of %u frames of the noise alone;\n",
	       FAR_NOISE_GAP_FRAMES);
	printf("%u s of a dense near talker at %.1f dBm0 and no far-end word, "
	       "a word source\n", FAR_NOISE_TALK_FRAMES / FRAMES_PER_SECOND,
	       FAR_NOISE_TALK_DBM0);
	printf("that starts its next word %u frames after each word ends;\n",
	       FAR_NOISE_PAUSE_FRAMES);
	printf("a gap of %u frames of the noise alone; %u s of far-end words, "
	       "the near end silent\n", FAR_NOISE_GAP_FRAMES,
	       FAR_NOISE_WORD_FRAMES / FRAMES_PER_SECOND);
	printf("untrained: the noise and a white near-end noise at %.1f dBm0 "
	       "from the first frame,\n", FAR_NOISE_NEAR_DBM0);
	printf("%u s of the two noises, then the gap, the talker, the gap and "
	       "the words of a talker row\n",
	       FAR_NOISE_LEAD_TO / FRAMES_PER_SECOND);
	printf("louder path: the noise from the first frame and the near end "
	       "silent; far-end words\n");
	printf("in frames 0 to %u and %u to %u; the hybrid steps to %.0f dB "
	       "ERL at frame %u\n", FAR_NOISE_WORD_FRAMES - 1u,
	       FAR_NOISE_RETURN_FRAME, FAR_NOISE_STEP_FRAMES - 1u,
	       FAR_NOISE_STEP_ERL_DB, FAR_NOISE_STEP_FRAME);
	printf("two instances run the same frames: the processor on (the "
	       "create default) gives\n");
	printf("every column but ERLE, the processor off gives ERLE\n");
	printf("trials and resets count the trials and the divergence resets "
	       "of the row;\n");
	printf("dt is the share of the talker frames that end in double "
	       "talk;\n");
	printf("quiet is the share of the talker frames with a zero talker "
	       "plane whose window is\n");
	printf("inactive;\n");
	printf("a frame holds near speech when the talker alone crosses the "
	       "activity bar;\n");
	printf("exposure counts the near-speech frames whose echo estimate "
	       "stands at or over the\n");
	printf("near speech less 10 dB;\n");
	printf("near loss is the regression gain of the output on the near "
	       "speech over those\n");
	printf("frames, as a loss: 0 dB means the output does not scale the "
	       "speech;\n");
	printf("added is the near speech over the output less the near speech "
	       "and the injected\n");
	printf("near-end noise, over those frames: a larger figure is "
	       "better;\n");
	printf("near repl counts the near-speech frames that the processor "
	       "marks active;\n");
	printf("ERLE is the filter ERLE over the far-word frames of the last "
	       "10 s, from frame %u\n", FAR_NOISE_STEP_ERLE_FROM);
	printf("on the louder path, with the injected near-end noise taken "
	       "out of the output;\n");
	printf("floor is the near-end floor the instance tracks at the end, "
	       "none while it has none;\n");
	printf("gain is the learned echo gain at the end; - means the row "
	       "has no near talker,\n");
	printf("n/a that no frame of the row holds the figure\n");
	far_noise_table_print(line_rate);
}

/*
 * The per-path table. On aarch64 the per-tap loops of dsp/lec.c come in two
 * forms and the vector one accumulates the dot product in thirty-two partial
 * sums instead of one, so the two are equivalent and not identical. Each row
 * drives one hybrid and hands the same microphone frame to two instances, one
 * per form, so the ERLE the line delivers and the worst sample the two forms
 * disagree on are both measured against the same stimulus. The bar is 0.1 dB
 * of ERLE and four counts.
 *
 * The cost of the two forms is in the tail table at the end of the run, which
 * sweeps the tails; timing every hybrid here as well would double the bench
 * without adding a number.
 */
#define PATH_TABLE_FRAMES	300u
#define PATH_TABLE_MEASURE	20u

struct path_pair {
	double erle[2];
	unsigned max_delta;
};

static void path_pair_measure(enum path_kind kind, unsigned line_rate,
			      uint32_t seed, struct path_pair *out)
{
	unsigned frame = line_rate / 100u;
	struct udsp_ec *scalar = bench_ec_create(SUMMARY_TAIL_MS,
							  line_rate);
	struct udsp_ec *simd = bench_ec_create(SUMMARY_TAIL_MS,
							line_rate);
	struct echo_path path;
	uint32_t state = seed;
	double echo_energy = 0.0;
	double scalar_energy = 0.0;
	double simd_energy = 0.0;
	unsigned f;

	memset(out, 0, sizeof(*out));
	udsp_ec_nlms.nlp(scalar, false);
	udsp_ec_nlms.nlp(simd, false);
	path_build(&path, kind, line_rate, SUMMARY_ERL_DB);
	for (f = 0; f < PATH_TABLE_FRAMES; f++) {
		int16_t ref[UDSP_FRAME_MAX];
		int16_t echo[UDSP_FRAME_MAX];
		int16_t scalar_out[UDSP_FRAME_MAX];
		int16_t simd_out[UDSP_FRAME_MAX];
		unsigned i;

		noise_fill(ref, frame, -10.0, &state);
		path_run(&path, ref, echo);
		memcpy(scalar_out, echo, frame * sizeof(*echo));
		memcpy(simd_out, echo, frame * sizeof(*echo));
		udsp_ec_nlms_path_set(UDSP_EC_NLMS_PATH_SCALAR);
		bench_ec_run(scalar, scalar_out, ref, frame);
		udsp_ec_nlms_path_set(UDSP_EC_NLMS_PATH_SIMD);
		bench_ec_run(simd, simd_out, ref, frame);
		for (i = 0; i < frame; i++) {
			unsigned delta = (unsigned)abs(scalar_out[i] -
						       simd_out[i]);

			if (delta > out->max_delta)
				out->max_delta = delta;
		}
		if (f < PATH_TABLE_MEASURE)
			continue;
		for (i = 0; i < frame; i++) {
			echo_energy += (double)echo[i] * echo[i];
			scalar_energy += (double)scalar_out[i] * scalar_out[i];
			simd_energy += (double)simd_out[i] * simd_out[i];
		}
	}
	out->erle[UDSP_EC_NLMS_PATH_SCALAR] = erle_get(echo_energy,
						       scalar_energy);
	out->erle[UDSP_EC_NLMS_PATH_SIMD] = erle_get(echo_energy, simd_energy);
	udsp_ec_nlms.destroy(scalar);
	udsp_ec_nlms.destroy(simd);
}

static void path_table_print(unsigned line_rate, uint32_t seed)
{
	enum udsp_ec_nlms_path outer = udsp_ec_nlms_path_get();
	unsigned row;

	printf("\nper-path equivalence at %u Hz, tail %u ms, hybrid %.0f dB ERL, seed 0x%08x, %u frames\n",
	       line_rate, SUMMARY_TAIL_MS, SUMMARY_ERL_DB, seed,
	       PATH_TABLE_FRAMES);
	printf("the scalar form is canonical; both forms see the same microphone frame\n");
	if (!udsp_ec_nlms_path_set(UDSP_EC_NLMS_PATH_SIMD)) {
		printf("  this build carries the %s form only: nothing to compare\n",
		       udsp_ec_nlms_path_name(outer));
		return;
	}
	udsp_ec_nlms_path_set(outer);
	printf("  %-10s | %14s | %14s | %8s | %18s\n", "hybrid",
	       "scalar ERLE dB", "simd ERLE dB", "delta dB",
	       "worst sample delta");
	for (row = 0; row < PATH_KINDS; row++) {
		struct path_pair pair;

		if (row == PATH_UPPER && line_rate != UDSP_SAMPLE_RATE_WB)
			continue;
		path_pair_measure(row, line_rate, seed, &pair);
		printf("  %-10s |", path_names[row]);
		erle_print(pair.erle[UDSP_EC_NLMS_PATH_SCALAR]);
		printf(" |");
		erle_print(pair.erle[UDSP_EC_NLMS_PATH_SIMD]);
		printf(" | %8.4f | %18u\n",
		       pair.erle[UDSP_EC_NLMS_PATH_SCALAR] -
		       pair.erle[UDSP_EC_NLMS_PATH_SIMD], pair.max_delta);
	}
	udsp_ec_nlms_path_set(outer);
}

/*
 * The value must consume the whole token: "8000junk" is a mistake, not an
 * 8 kHz run, and a bench that guesses would print a table under the wrong
 * heading. Base 0 reads the seed in the 0x form the table prints, so a row
 * can be reproduced by pasting its own heading back.
 */
static bool value_parse(const char *token, unsigned long max,
			unsigned long *value)
{
	char *end;

	errno = 0;
	*value = strtoul(token, &end, 0);
	return end != token && !*end && errno != ERANGE && *value <= max;
}

/*
 * --rate picks the line rate, --seeds the length of the per-path seed list and
 * --seed its first entry. --seed alone means one run of that seed, which is
 * how a single row of the table is reproduced; given together, --seeds says
 * how many seeds follow that first one. --ref-loss sets the reference loss
 * in half-dB steps (bench_ref_loss_set), up to the 30 dB of the lowest SLIC
 * gain (UDSP_SLIC_GAIN_MIN in core/udsp.c). Anything else is a usage error.
 */
#define BENCH_REF_LOSS_MAX	60u

static bool args_parse(int argc, char **argv, unsigned *rate, unsigned *seeds,
		       uint32_t *seed_first)
{
	bool seed_given = false;
	int i;

	for (i = 1; i < argc; i++) {
		unsigned long value;

		if (i + 1 >= argc)
			return false;
		if (!strcmp(argv[i], "--rate")) {
			if (!value_parse(argv[++i], UDSP_SAMPLE_RATE_WB, &value))
				return false;
			*rate = (unsigned)value;
			continue;
		}
		if (!strcmp(argv[i], "--seeds")) {
			if (!value_parse(argv[++i], SUMMARY_SEEDS_MAX, &value) ||
			    !value)
				return false;
			*seeds = (unsigned)value;
			continue;
		}
		if (!strcmp(argv[i], "--ref-loss")) {
			if (!value_parse(argv[++i], BENCH_REF_LOSS_MAX, &value))
				return false;
			bench_ref_loss_set((unsigned)value);
			continue;
		}
		if (strcmp(argv[i], "--seed"))
			return false;
		/* The xorshift reference generator has no state 0 to walk out
		 * of, so 0 is a bad seed rather than a small one. */
		if (!value_parse(argv[++i], 0xffffffffu, &value) || !value)
			return false;
		*seed_first = (uint32_t)value;
		seed_given = true;
	}
	if (!*seeds)
		*seeds = seed_given ? 1u : SUMMARY_SEEDS_DEFAULT;
	return *rate == UDSP_SAMPLE_RATE || *rate == UDSP_SAMPLE_RATE_WB;
}

int main(int argc, char **argv)
{
	static const struct run_cfg scenarios[] = {
		{
			.name = "S1 steady state residual, composite source",
			.models = "G.168 test 1 (steady state residual and returned echo level), Annex C composite source, Annex D hybrid shape",
			.tail_ms = 48, .frames = 1000, .far_dbm0 = -10.0,
			.near_dbm0 = 0.0, .source = SOURCE_CSS,
		},
		{
			.name = "S2 convergence, white excitation",
			.models = "G.168 test 2A (convergence with NLP disabled)",
			.tail_ms = 48, .frames = 600, .far_dbm0 = -10.0,
			.near_dbm0 = 0.0, .source = SOURCE_NOISE,
		},
		{
			.name = "S3A double talk, near end 3 dB above the echo",
			.models = "G.168 test 3 (performance under double talk), low near-end level",
			.tail_ms = 48, .frames = 800, .far_dbm0 = -10.0,
			.near_dbm0 = -19.0, .near_from = 300, .near_to = 500,
			.source = SOURCE_NOISE,
		},
		{
			.name = "S3B double talk, near end at the far-end level",
			.models = "G.168 test 3 (performance under double talk), high near-end level",
			.tail_ms = 48, .frames = 800, .far_dbm0 = -10.0,
			.near_dbm0 = -10.0, .near_from = 300, .near_to = 500,
			.source = SOURCE_NOISE,
		},
		{
			.name = "S5 infinite return loss",
			.models = "G.168 test 5 (infinite return loss convergence): reference present, no echo",
			.tail_ms = 48, .frames = 600, .far_dbm0 = -10.0,
			.near_dbm0 = -30.0, .near_from = 0, .near_to = 600,
			.source = SOURCE_NOISE, .no_echo = true,
		},
		{
			.name = "S6 narrow-band reference, 1000 Hz",
			.models = "G.168 test 6 (non-divergence on narrow-band signals)",
			.tail_ms = 48, .frames = 600, .far_dbm0 = -10.0,
			.near_dbm0 = 0.0, .source = SOURCE_TONE,
		},
		{
			.name = "S7 path change to a late hybrid",
			.models = "G.168 test 7 (stability) and the reconvergence intent of test 2",
			.tail_ms = 48, .frames = 1000, .far_dbm0 = -10.0,
			.near_dbm0 = 0.0, .path_change = 500,
			.source = SOURCE_NOISE,
		},
		{
			.name = "S8 tail shorter than the dispersion",
			.models = "G.168 tail-length intent: a 16 ms tail against a 25 ms hybrid",
			.tail_ms = 16, .frames = 600, .far_dbm0 = -10.0,
			.near_dbm0 = 0.0, .source = SOURCE_NOISE,
		},
	};
	/* 128 ms is the narrowband ceiling; the wideband list stops at the
	 * 64 ms wideband cap, so no row is a clamped duplicate. */
	static const unsigned narrow_tails[] = { 32u, 48u, 64u, 128u };
	static const unsigned wide_tails[] = { 16u, 32u, 48u, 64u };
	const unsigned *tails = narrow_tails;
	unsigned tail_count = sizeof(narrow_tails) / sizeof(narrow_tails[0]);
	unsigned line_rate = UDSP_SAMPLE_RATE;
	unsigned seeds = 0;
	uint32_t seed_first = SUMMARY_SEED_BASE;
	int16_t *ref_track;
	int16_t *mic_track;
	struct echo_path path;
	unsigned i;

	if (!args_parse(argc, argv, &line_rate, &seeds, &seed_first)) {
		fprintf(stderr,
			"usage: %s [--rate 8000|16000] [--seeds N] [--seed VALUE] [--ref-loss N]\n",
			argv[0]);
		return 2;
	}
	if (line_rate == UDSP_SAMPLE_RATE_WB) {
		tails = wide_tails;
		tail_count = sizeof(wide_tails) / sizeof(wide_tails[0]);
	}
	ref_track = calloc(TIMING_FRAMES * UDSP_FRAME_MAX, sizeof(int16_t));
	mic_track = calloc(TIMING_FRAMES * UDSP_FRAME_MAX, sizeof(int16_t));
	if (!ref_track || !mic_track)
		return 1;
	printf("udsp_ec_nlms versus speexdsp MDF, G.168-derived scenarios\n");
	printf("line rate %u Hz, frame %u samples per 10 ms tick\n", line_rate,
	       line_rate / 100u);
	printf("both cancellers see identical frames; ERLE is measured against the injected echo\n");
	printf("udsp runs with the NLP off: speex_echo_cancellation returns the linear residual\n");
	printf("an ERLE of 200.00 dB is the sentinel for a residual below the int16 floor;\n");
	printf("n/a means the scenario injects no echo, so there is no return loss to report\n");
	if (bench_ref_loss)
		printf("reference loss %u half-dB: udsp reads the reference that "
		       "much lower,\nthe echo path and speexdsp read the line "
		       "reference\n", bench_ref_loss);
	for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
		scenario_run(&scenarios[i], line_rate);

	summary_print(line_rate, seeds, seed_first, ref_track, mic_track);
	weak_print(line_rate);
	field_print(line_rate);
	talk_print(line_rate);
	far_noise_print(line_rate);
	path_table_print(line_rate, seed_first);

	path_composite_init(&path, line_rate, 12.0);
	stimulus_build(&path, line_rate, ref_track, mic_track);
	printf("\ncycle cost per %u-sample frame, canceller only (this host, -Os)\n",
	       line_rate / 100u);
	printf("the two udsp columns are the two forms of the per-tap loops;\n");
	printf("n/a in the simd column means this build carries the scalar form only\n");
	printf("  tail ms |   taps | MACs / frame | udsp scalar ns |   udsp simd ns |  gain | speexdsp ns\n");
	for (i = 0; i < tail_count; i++)
		timing_run(tails[i], line_rate, ref_track, mic_track);

	memset(ref_track, 0, TIMING_FRAMES * UDSP_FRAME_MAX * sizeof(int16_t));
	printf("\nthe same frame over a zero reference, %s path: the full form walks the\n",
	       udsp_ec_nlms_path_name(udsp_ec_nlms_path_get()));
	printf("tap loops over the zero window, the skip form leaves them out\n");
	printf("  tail ms |   taps |   full form ns |   skip form ns | frames skipped\n");
	for (i = 0; i < tail_count; i++)
		zero_timing_run(tails[i], line_rate, ref_track, mic_track);
	free(ref_track);
	free(mic_track);
	return 0;
}
