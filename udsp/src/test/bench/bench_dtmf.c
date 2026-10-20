// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <complex.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dsp.h"

#define WINDOW_SAMPLES UDSP_DTMF_WINDOW_SAMPLES
#define WARM_SAMPLES (5u * UDSP_FRAME_SAMPLES)
#define TAIL_SAMPLES (6u * UDSP_FRAME_SAMPLES)
#define HOLD_SAMPLES (300u * UDSP_SAMPLE_RATE / 1000u)
#define WALK_SAMPLES (300u * UDSP_SAMPLE_RATE / 1000u)
#define PI 3.14159265358979323846
#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const double frequency[16] = {
	697.0, 770.0, 852.0, 941.0, 1209.0, 1336.0, 1477.0, 1633.0,
	1394.0, 1540.0, 1704.0, 1882.0, 2418.0, 2672.0, 2954.0, 3266.0,
};
static const char digits[] = "123A456B789C*0#D";
static const unsigned quarter_start[4] = { 0, 51, 102, 153 };
static const unsigned quarter_length[4] = { 51, 51, 51, 52 };
static const unsigned quarter_end[4] = { 50, 101, 152, 204 };
static const double quarter_centroid[4] = { 25.0, 76.0, 127.0, 178.5 };

enum offset_kind {
	OFFSET_NOMINAL,
	OFFSET_PLUS_TOLERANCE,
	OFFSET_MINUS_TOLERANCE,
	OFFSET_PLUS_EDGE,
	OFFSET_MINUS_EDGE,
	OFFSET_PLUS_REJECT,
	OFFSET_MINUS_REJECT,
	OFFSET_COUNT,
};

static const char *const offset_name[] = {
	"nominal", "+1.5pct", "-1.5pct", "+edge", "-edge",
	"+3pct", "-3pct",
};

enum pair_method {
	PAIR_CANCEL,
	PAIR_CANCEL2,
	PAIR_UNMIX,
	PAIR_HALF,
};

struct tone_pair {
	double amplitude[2];
	double nominal[2];
	double delta[2];
	double omega[2];
};

struct event_count {
	unsigned total;
	unsigned digit[16];
	unsigned other;
};

struct estimate {
	double coarse;
	double delta;
	double complex sum;
	double hops;
	unsigned adjacent;
	unsigned mask;
	unsigned loaded;
	unsigned fill;
};

struct load_result {
	double loaded_at;
	double estimated_at;
	unsigned mask;
};

struct hold_result {
	double first;
	double previous;
	double anchor;
	double step;
	unsigned estimates;
	unsigned missing;
	bool previous_valid;
};

struct run_result {
	double complex sum;
	double magnitude;
	double hops;
	double coarse;
	unsigned adjacent;
};

struct run_snapshot {
	double delta[2];
	double coherence[2];
};

struct hold2_result {
	struct hold_result instant[2];
	struct run_snapshot at3;
	struct run_snapshot at7;
	struct run_snapshot end;
	unsigned decisions;
};

static double amplitude_get(double level_dbm0)
{
	return 32767.0 * pow(10.0, (level_dbm0 - 3.14) / 20.0);
}

static double offset_get(double nominal, enum offset_kind offset)
{
	switch (offset) {
	case OFFSET_PLUS_TOLERANCE:
		return 0.015 * nominal;
	case OFFSET_MINUS_TOLERANCE:
		return -0.015 * nominal;
	case OFFSET_PLUS_EDGE:
		return 0.015 * nominal + 2.0;
	case OFFSET_MINUS_EDGE:
		return -(0.015 * nominal + 2.0);
	case OFFSET_PLUS_REJECT:
		return 0.03 * nominal;
	case OFFSET_MINUS_REJECT:
		return -0.03 * nominal;
	default:
		return 0.0;
	}
}

static void pair_offsets_init(struct tone_pair *pair, unsigned digit,
			      double level, double twist,
			      const enum offset_kind offsets[2])
{
	unsigned i;

	pair->nominal[0] = frequency[digit / 4u];
	pair->nominal[1] = frequency[4u + digit % 4u];
	pair->amplitude[0] = amplitude_get(twist > 0.0 ? level - twist : level);
	pair->amplitude[1] = amplitude_get(twist > 0.0 ? level : level + twist);
	for (i = 0; i < 2; i++) {
		pair->delta[i] = offset_get(pair->nominal[i], offsets[i]);
		pair->omega[i] = 2.0 * PI * (pair->nominal[i] + pair->delta[i]) /
			UDSP_SAMPLE_RATE;
	}
}

static void pair_init(struct tone_pair *pair, unsigned digit, double level,
		      double twist, enum offset_kind offset)
{
	const enum offset_kind offsets[2] = { offset, offset };

	pair_offsets_init(pair, digit, level, twist, offsets);
}

static double pair_sample_get(const struct tone_pair *pair, unsigned sample)
{
	return pair->amplitude[0] * sin(pair->omega[0] * sample) +
		pair->amplitude[1] * sin(pair->omega[1] * sample);
}

static void burst_frame_fill(int16_t *samples, const struct tone_pair *pair,
			     unsigned fed, unsigned start, unsigned length)
{
	unsigned i;

	for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
		unsigned n = fed + i;
		double value = 0.0;

		if (n >= start && n - start < length)
			value = pair_sample_get(pair, n - start);
		samples[i] = udsp_sat16_round(value);
	}
}

static void pair_window_fill(double *samples, const struct tone_pair *pair,
			     unsigned fill)
{
	unsigned start = WINDOW_SAMPLES - fill;
	unsigned i;

	for (i = 0; i < WINDOW_SAMPLES; i++)
		samples[i] = i < start ? 0.0 : pair_sample_get(pair, i - start);
}

static double energy_get(const int16_t *samples, unsigned count)
{
	double energy = 0.0;
	unsigned i;

	for (i = 0; i < count; i++)
		energy += (double)samples[i] * samples[i];
	return energy;
}

static void event_add(struct event_count *count, const struct udsp_event *event)
{
	const char *digit = strchr(digits, event->u.digit.digit);

	count->total++;
	if (!digit || !*digit) {
		count->other++;
		return;
	}
	count->digit[digit - digits]++;
}

static struct event_count burst_events_get(const struct tone_pair *pair,
					   unsigned duration, unsigned offset,
					   bool warm)
{
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	struct event_count count = { 0 };
	unsigned start = (warm ? WARM_SAMPLES : 0u) + offset;
	unsigned length = duration * UDSP_SAMPLE_RATE / 1000u;
	unsigned end = start + length + TAIL_SAMPLES;
	unsigned fed;

	udsp_dtmf_detector_init(&detector, false);
	for (fed = 0; fed < end; fed += UDSP_FRAME_SAMPLES) {
		int16_t samples[UDSP_FRAME_SAMPLES];

		burst_frame_fill(samples, pair, fed, start, length);
		if (udsp_dtmf_detector_process(&detector, samples, &event))
			event_add(&count, &event);
	}
	if (udsp_dtmf_detector_flush(&detector, &event))
		event_add(&count, &event);
	return count;
}

static bool burst_accepted(unsigned digit, unsigned duration, unsigned offset,
			   bool warm)
{
	struct tone_pair pair;
	struct event_count count;

	pair_init(&pair, digit, -10.0, 0.0, OFFSET_NOMINAL);
	count = burst_events_get(&pair, duration, offset, warm);
	return count.total == 1 && count.digit[digit] == 1;
}

static void accept_run(bool quick)
{
	static const unsigned durations[] = { 30, 35, 40, 45, 50, 60 };
	unsigned warm_count[ARRAY_COUNT(durations)][UDSP_FRAME_SAMPLES] = { 0 };
	unsigned warm_mask[ARRAY_COUNT(durations)][UDSP_FRAME_SAMPLES] = { 0 };
	unsigned stride = quick ? 10u : 1u;
	unsigned d;

	if (quick)
		fprintf(stderr, "quick accept: all digits and durations; "
			"offsets 0..70 step 10\n");
	puts("table\tduration_ms\tcold_pct\twarm_pct\ttrials\toffset_step");
	for (d = 0; d < ARRAY_COUNT(durations); d++) {
		unsigned cold = 0, warm = 0, trials = 0;
		unsigned cell;

		for (cell = 0; cell < 16u * UDSP_FRAME_SAMPLES; cell += stride) {
			unsigned digit = cell / UDSP_FRAME_SAMPLES;
			unsigned offset = cell % UDSP_FRAME_SAMPLES;
			bool accepted = burst_accepted(digit, durations[d],
						      offset, true);

			cold += burst_accepted(digit, durations[d], offset, false);
			warm += accepted;
			warm_count[d][offset] += accepted;
			warm_mask[d][offset] |= (unsigned)accepted << digit;
			trials++;
		}
		printf("accept\t%u\t%.6f\t%.6f\t%u\t%u\n", durations[d],
		       100.0 * cold / trials, 100.0 * warm / trials, trials,
		       stride);
	}
	puts("table\tduration_ms\toffset_samples\twarm_count\tdigits\t"
	     "warm_mask_hex_123A456B789Cstar0hashD_lsb_first");
	for (d = 0; d < ARRAY_COUNT(durations); d++) {
		unsigned offset;

		for (offset = 0; offset < UDSP_FRAME_SAMPLES; offset += stride)
			printf("accept_offset\t%u\t%u\t%u\t16\t%04x\n",
			       durations[d], offset, warm_count[d][offset],
			       warm_mask[d][offset]);
	}
}

enum matrix_class {
	MATRIX_SHALL_ACCEPT,
	MATRIX_PRODUCT_ACCEPT,
	MATRIX_SHALL_REJECT,
	MATRIX_POLICY_REJECT,
	MATRIX_CLASS_COUNT,
};

static const char *const matrix_class_name[] = {
	"shall_accept", "product_accept", "shall_reject", "policy_reject",
};

struct matrix_cell {
	unsigned make;
	double twist;
	double level;
	enum offset_kind offsets[2];
};

struct matrix_count {
	unsigned accepted;
	unsigned total;
	unsigned event_cells;
	unsigned events;
	unsigned failures;
};

static enum matrix_class matrix_class_get(const struct matrix_cell *cell)
{
	if (cell->make == 20)
		return MATRIX_SHALL_REJECT;
	if (cell->make == 25 || cell->twist < -6.0 || cell->twist > 6.0 ||
	    cell->level == -40.0 || cell->offsets[0] >= OFFSET_PLUS_REJECT ||
	    cell->offsets[1] >= OFFSET_PLUS_REJECT)
		return MATRIX_POLICY_REJECT;
	return cell->make == 35 ? MATRIX_PRODUCT_ACCEPT : MATRIX_SHALL_ACCEPT;
}

static void matrix_prefix_print(const char *table, const struct matrix_cell *cell)
{
	printf("%s\t%s\t%u\t%+.0f\t%.0f\t%s\t%s\t", table,
	       cell->level == 0.0 ? "reported" :
	       matrix_class_name[matrix_class_get(cell)], cell->make,
	       cell->twist, cell->level, offset_name[cell->offsets[0]],
	       offset_name[cell->offsets[1]]);
}

static void matrix_detail_print(const char *table, const struct matrix_cell *cell,
				unsigned digit, unsigned lead,
				const struct tone_pair *pair,
				const struct event_count *events,
				bool accepted, bool failed)
{
	matrix_prefix_print(table, cell);
	printf("%c\t%u\t%.6f\t%.6f\t%.6f\t%.6f\t%u\t1\t%u\t%u\t%u\n",
	       digits[digit], lead, pair->nominal[0], pair->nominal[1],
	       pair->delta[0], pair->delta[1], (unsigned)accepted,
	       (unsigned)(events->total != 0), events->total, (unsigned)failed);
}

static void matrix_cell_run(const struct matrix_cell *cell, bool quick,
			    struct matrix_count *totals)
{
	static const unsigned leads[] = { 0, 11, 23, 34, 45, 57, 68, 79 };
	enum matrix_class contract = matrix_class_get(cell);
	bool reported = cell->level == 0.0;
	bool reject = !reported && (contract == MATRIX_SHALL_REJECT ||
		contract == MATRIX_POLICY_REJECT);
	struct matrix_count count = { 0 };
	unsigned digit, i;

	for (digit = 0; digit < 16; digit += quick ? 5u : 1u) {
		struct tone_pair pair;

		pair_offsets_init(&pair, digit, cell->level, cell->twist,
				  cell->offsets);
		for (i = 0; i < ARRAY_COUNT(leads); i += quick ? 4u : 1u) {
			struct event_count events = burst_events_get(&pair, cell->make,
								     leads[i], true);
			bool accepted = events.total == 1 && events.digit[digit] == 1;
			bool failed = !reported &&
				(reject ? events.total != 0 : !accepted);

			count.accepted += accepted;
			count.total++;
			count.event_cells += events.total != 0;
			count.events += events.total;
			count.failures += failed;
			if (reject)
				matrix_detail_print("matrixreject", cell, digit, leads[i],
						    &pair, &events, accepted, failed);
			if (failed)
				matrix_detail_print("matrixfail", cell, digit, leads[i],
						    &pair, &events, accepted, failed);
		}
	}
	matrix_prefix_print(reported ? "matrixreport" : "matrix", cell);
	printf("all\tall\tnan\tnan\tnan\tnan\t%u\t%u\t%u\t%u\t%u\n",
	       count.accepted, count.total, count.event_cells, count.events,
	       count.failures);
	if (reported)
		return;
	totals[contract].accepted += count.accepted;
	totals[contract].total += count.total;
	totals[contract].event_cells += count.event_cells;
	totals[contract].events += count.events;
	totals[contract].failures += count.failures;
}

static void matrix_run(bool quick)
{
	static const unsigned makes[] = { 35, 41, 50, 52, 20, 25 };
	static const double twists[] = { -6, 0, 6, -10, 10 };
	static const double levels[] = { -4, -28, -40 };
	static const enum offset_kind offsets[] = {
		OFFSET_MINUS_EDGE, OFFSET_NOMINAL, OFFSET_PLUS_EDGE,
		OFFSET_MINUS_REJECT, OFFSET_PLUS_REJECT,
	};
	struct matrix_count totals[MATRIX_CLASS_COUNT] = { 0 };
	unsigned cells = ARRAY_COUNT(makes) * ARRAY_COUNT(twists) *
		ARRAY_COUNT(levels) * ARRAY_COUNT(offsets) * ARRAY_COUNT(offsets);
	unsigned i;

	if (quick)
		fprintf(stderr, "quick matrix: digits 1,5,9,D; leads 0,45; "
			"all other dimensions retained\n");
	puts("table\tclass\tmake_ms\ttwist_db\tlevel_dbm0\trow_offset\t"
	     "column_offset\tdigit\tlead_samples\trow_nominal_hz\t"
	     "column_nominal_hz\trow_offset_hz\tcolumn_offset_hz\taccepted\t"
	     "total\tevent_cells\tevents\tfailures");
	for (i = 0; i < cells; i++) {
		unsigned axis = i;
		struct matrix_cell cell;

		cell.offsets[1] = offsets[axis % ARRAY_COUNT(offsets)];
		axis /= ARRAY_COUNT(offsets);
		cell.offsets[0] = offsets[axis % ARRAY_COUNT(offsets)];
		axis /= ARRAY_COUNT(offsets);
		cell.level = levels[axis % ARRAY_COUNT(levels)];
		axis /= ARRAY_COUNT(levels);
		cell.twist = twists[axis % ARRAY_COUNT(twists)];
		cell.make = makes[axis / ARRAY_COUNT(twists)];
		matrix_cell_run(&cell, quick, totals);
	}
	for (i = 0; i < ARRAY_COUNT(twists) * ARRAY_COUNT(offsets) *
	     ARRAY_COUNT(offsets); i++) {
		struct matrix_cell cell = {
			.make = 50,
			.twist = twists[i / ARRAY_COUNT(offsets) /
					ARRAY_COUNT(offsets)],
			.level = 0.0,
			.offsets = {
				offsets[i / ARRAY_COUNT(offsets) %
					ARRAY_COUNT(offsets)],
				offsets[i % ARRAY_COUNT(offsets)],
			},
		};

		matrix_cell_run(&cell, quick, totals);
	}
	puts("table\tclass\taccepted\ttotal\tevent_cells\tevents\tfailures");
	for (i = 0; i < MATRIX_CLASS_COUNT; i++)
		printf("matrixtotal\t%s\t%u\t%u\t%u\t%u\t%u\n",
		       matrix_class_name[i], totals[i].accepted, totals[i].total,
		       totals[i].event_cells, totals[i].events, totals[i].failures);
}

static double passing_fill_get(const struct tone_pair *pair, unsigned offset)
{
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	unsigned start = WARM_SAMPLES + offset;
	unsigned fed;

	udsp_dtmf_detector_init(&detector, false);
	for (fed = 0; fed < start + WALK_SAMPLES; fed += UDSP_FRAME_SAMPLES) {
		int16_t samples[UDSP_FRAME_SAMPLES];
		unsigned previous = detector.candidate_decisions;
		char active = detector.active;
		bool ended;

		burst_frame_fill(samples, pair, fed, start, WALK_SAMPLES);
		ended = udsp_dtmf_detector_process(&detector, samples, &event);
		if (detector.candidate_decisions <= previous &&
		    !(!active && detector.active) && !ended)
			continue;
		return fmin(WINDOW_SAMPLES, fed + UDSP_FRAME_SAMPLES - start);
	}
	return NAN;
}

static void walk_cell_print(unsigned digit, double level, double twist,
			    unsigned stride)
{
	struct tone_pair pair;
	double minimum = NAN, maximum = NAN;
	unsigned passes = 0, trials = 0;
	unsigned offset;

	pair_init(&pair, digit, level, twist, OFFSET_NOMINAL);
	for (offset = 0; offset < UDSP_FRAME_SAMPLES; offset += stride) {
		double fill = passing_fill_get(&pair, offset);

		trials++;
		if (!isfinite(fill))
			continue;
		minimum = fmin(minimum, fill);
		maximum = fmax(maximum, fill);
		passes++;
	}
	printf("owalk\t%c\t%.0f\t%+.0f\t%.0f\t%.0f\t%.3f\t%.3f\t"
	       "%u\t%u\t%u\t300\n", digits[digit], level, twist,
	       minimum, maximum, minimum * 1000.0 / UDSP_SAMPLE_RATE,
	       maximum * 1000.0 / UDSP_SAMPLE_RATE, passes, trials, stride);
}

static double energy_gate_fill_get(const struct tone_pair *pair)
{
	double gate = WINDOW_SAMPLES * 500.0 * 500.0;
	double energy = 0.0;
	unsigned fill;

	for (fill = 1; fill <= WINDOW_SAMPLES; fill++) {
		double value = udsp_sat16_round(pair_sample_get(pair, fill - 1));

		energy += value * value;
		if (energy >= gate)
			return fill;
	}
	return NAN;
}

static void energy_gate_print(void)
{
	double minimum = NAN, maximum = NAN;
	unsigned reached = 0;
	unsigned digit;

	for (digit = 0; digit < 16; digit++) {
		struct tone_pair pair;
		double fill;

		pair_init(&pair, digit, -28.0, -6.0, OFFSET_NOMINAL);
		fill = energy_gate_fill_get(&pair);
		if (!isfinite(fill))
			continue;
		minimum = fmin(minimum, fill);
		maximum = fmax(maximum, fill);
		reached++;
	}
	puts("table\tdigits\tlevel_dbm0\ttwist_db\tmin_fill_samples\t"
	     "max_fill_samples\tmin_fill_ms\tmax_fill_ms\treached\tgate");
	printf("energy_gate\tall16\t-28\t-6\t%.0f\t%.0f\t%.3f\t%.3f\t"
	       "%u\t%.0f\n", minimum, maximum,
	       minimum * 1000.0 / UDSP_SAMPLE_RATE,
	       maximum * 1000.0 / UDSP_SAMPLE_RATE, reached,
	       WINDOW_SAMPLES * 500.0 * 500.0);
}

static void owalk_run(bool quick)
{
	static const double levels[] = { -3, -13, -23, -28 };
	static const double twists[] = { -6, 0, 3, 6 };
	unsigned cell;

	if (quick)
		fprintf(stderr, "quick owalk: all digits, levels and twists; "
			"offsets 0..70 step 10; energy gate step 1\n");
	puts("table\tdigit\tlevel_dbm0\ttwist_db\tmin_fill_samples\t"
	     "max_fill_samples\tmin_fill_ms\tmax_fill_ms\tpassing_offsets\t"
	     "offsets\toffset_step\ttone_limit_ms");
	for (cell = 0; cell < 16u * 4u * 4u; cell++)
		walk_cell_print(cell / 16u, levels[cell / 4u % 4u],
				twists[cell % 4u], quick ? 10u : 1u);
	energy_gate_print();
}

static void harmonic_cell_print(unsigned digit, double level,
				enum offset_kind offset)
{
	struct udsp_goertzel_bank bank;
	struct tone_pair pair;
	int16_t samples[WINDOW_SAMPLES];
	double power[16];
	double fundamental, harmonic, energy;
	unsigned row = digit / 4u;
	unsigned column = digit % 4u;
	unsigned i;

	pair_init(&pair, digit, level, 0.0, offset);
	for (i = 0; i < WINDOW_SAMPLES; i++)
		samples[i] = udsp_sat16_round(pair_sample_get(&pair, i));
	energy = energy_get(samples, WINDOW_SAMPLES);
	udsp_goertzel_init(&bank, frequency, 16);
	udsp_goertzel_process(&bank, samples, WINDOW_SAMPLES);
	for (i = 0; i < 16; i++)
		power[i] = udsp_goertzel_power(&bank, i);
	fundamental = power[row] + power[4u + column];
	harmonic = power[8u + row] + power[12u + column];
	printf("harmonic\t%c\t%s\t%.0f\t%.6f\t%.6f\t%.9f\t%.9f\n",
	       digits[digit], offset_name[offset], level,
	       pair.delta[0], pair.delta[1], harmonic / fundamental,
	       fundamental / (energy * WINDOW_SAMPLES));
}

static void harmonic_run(bool quick)
{
	static const enum offset_kind offsets[] = {
		OFFSET_NOMINAL, OFFSET_PLUS_EDGE, OFFSET_MINUS_EDGE,
	};
	static const double levels[] = { -4, -10, -28 };
	unsigned cell;

	if (quick)
		fprintf(stderr, "quick harmonic: digits 1,5,9,D; "
			"all offsets and levels\n");
	puts("table\tdigit\toffset\tlevel_dbm0\trow_offset_hz\t"
	     "column_offset_hz\tharmonic_ratio\tpair_ratio");
	for (cell = 0; cell < 16u * 3u * 3u; cell++) {
		unsigned digit = cell / 9u;

		if (quick && digit % 5u)
			continue;
		harmonic_cell_print(digit, levels[cell % 3u],
				    offsets[cell / 3u % 3u]);
	}
}

static double complex phase_get(double angle)
{
	return cos(angle) + I * sin(angle);
}

static double power_get(double complex value)
{
	return creal(value) * creal(value) + cimag(value) * cimag(value);
}

static double dirichlet_get(unsigned length, double delta)
{
	double angle = PI * delta / UDSP_SAMPLE_RATE;
	double denominator = sin(angle);

	if (fabs(denominator) < 1e-12)
		return length;
	return sin(length * angle) / denominator;
}

static double loss_get(unsigned length, double delta)
{
	double ratio;

	if (!length)
		return NAN;
	ratio = dirichlet_get(length, delta) / length;
	return ratio * ratio;
}

static double complex slice_get(const double *samples, double nominal,
				unsigned start, unsigned end)
{
	double omega = 2.0 * PI * nominal / UDSP_SAMPLE_RATE;
	double coefficient = 2.0 * cos(omega);
	double q1 = 0.0, q2 = 0.0;
	unsigned i;

	for (i = start; i <= end; i++) {
		double q0 = samples[i] + coefficient * q1 - q2;

		q2 = q1;
		q1 = q0;
	}
	return q1 - q2 * cos(omega) + I * q2 * sin(omega);
}

static double complex quarter_get(const double *samples, double nominal,
				  unsigned quarter)
{
	return slice_get(samples, nominal, quarter_start[quarter],
			 quarter_end[quarter]);
}

static double complex slice_leakage_get(double winner, double partner,
					unsigned length, double centroid)
{
	double delta = partner - winner;
	double angle = 2.0 * PI * delta * centroid / UDSP_SAMPLE_RATE;

	return dirichlet_get(length, delta) * phase_get(angle) / length;
}

static double complex leakage_get(double winner, double partner,
				  unsigned quarter)
{
	return slice_leakage_get(winner, partner, quarter_length[quarter],
				 quarter_centroid[quarter]);
}

static double angle_wrap(double angle)
{
	angle = remainder(angle, 2.0 * PI);
	return angle <= -PI ? angle + 2.0 * PI : angle;
}

static struct estimate estimate_get(const double complex *value, double nominal)
{
	struct estimate result = { .coarse = NAN, .delta = NAN };
	double normalized[4];
	double maximum = 0.0;
	double omega = 2.0 * PI * nominal / UDSP_SAMPLE_RATE;
	double complex sum = 0.0;
	double hops = 0.0;
	unsigned adjacent = 0;
	unsigned first = 4, last = 0;
	unsigned k;

	for (k = 0; k < 4; k++) {
		normalized[k] = power_get(value[k]) /
			((double)quarter_length[k] * quarter_length[k]);
		maximum = fmax(maximum, normalized[k]);
	}
	for (k = 0; k < 4; k++) {
		if (normalized[k] < 0.5 * maximum)
			continue;
		result.mask |= 1u << k;
		result.loaded++;
		result.fill += quarter_length[k];
		if (first == 4)
			first = k;
		last = k;
	}
	if (result.loaded < 2)
		return result;
	for (k = 0; k < 3; k++) {
		unsigned mask = 3u << k;

		if ((result.mask & mask) != mask)
			continue;
		sum += value[k + 1] * conj(value[k]) *
			phase_get(-omega * (quarter_end[k + 1] - quarter_end[k]));
		hops += quarter_centroid[k + 1] - quarter_centroid[k];
		adjacent++;
	}
	if (!adjacent || power_get(sum) == 0.0)
		return result;
	result.sum = sum;
	result.hops = hops;
	result.adjacent = adjacent;
	result.coarse = carg(sum) * UDSP_SAMPLE_RATE /
		(2.0 * PI * hops / adjacent);
	result.delta = result.coarse;
	if (result.loaded >= 3) {
		double span = quarter_centroid[last] - quarter_centroid[first];
		double phi = angle_wrap(carg(value[last] * conj(value[first])) -
			omega * (quarter_end[last] - quarter_end[first]));
		double fine = phi * UDSP_SAMPLE_RATE / (2.0 * PI * span);
		double step = UDSP_SAMPLE_RATE / span;

		result.delta = fine + round((result.coarse - fine) / step) * step;
	}
	return result;
}

static void pair_raw_get(const double *samples, const struct tone_pair *pair,
			 double complex raw[2][4])
{
	unsigned k, tone;

	for (tone = 0; tone < 2; tone++) {
		for (k = 0; k < 4; k++)
			raw[tone][k] = quarter_get(samples, pair->nominal[tone], k);
	}
}

static void pair_cancel(const double complex raw[2][4],
			const struct tone_pair *pair, const double *partner,
			struct estimate *result)
{
	double complex cancelled[2][4];
	unsigned k, tone;

	for (tone = 0; tone < 2; tone++) {
		for (k = 0; k < 4; k++)
			cancelled[tone][k] = raw[tone][k] - raw[1u - tone][k] *
				leakage_get(pair->nominal[tone],
					    partner[1u - tone], k);
		result[tone] = estimate_get(cancelled[tone], pair->nominal[tone]);
	}
}

static void pair_estimate(const double *samples, const struct tone_pair *pair,
			  struct estimate *result)
{
	double complex raw[2][4];

	pair_raw_get(samples, pair, raw);
	pair_cancel(raw, pair, pair->nominal, result);
}

static void pair_estimate2(const double *samples, const struct tone_pair *pair,
			   struct estimate *result, struct estimate *first)
{
	double complex raw[2][4];
	double partner[2];
	unsigned tone;

	pair_estimate(samples, pair, first);
	for (tone = 0; tone < 2; tone++)
		partner[tone] = pair->nominal[tone] +
			(isfinite(first[tone].delta) ? first[tone].delta : 0.0);
	pair_raw_get(samples, pair, raw);
	pair_cancel(raw, pair, partner, result);
}

static void pair_values_unmix(double complex a, double complex b,
			      double complex ab, double complex ba,
			      double complex *out_a, double complex *out_b)
{
	double complex determinant = 1.0 - ab * ba;

	if (cabs(determinant) < 1e-6) {
		*out_a = a;
		*out_b = b;
		return;
	}
	*out_a = (a - ab * b) / determinant;
	*out_b = (b - ba * a) / determinant;
}

static void pair_unmix(const double complex raw[2][4],
		       const struct tone_pair *pair, const double *partner,
		       double complex unmixed[2][4], struct estimate *result)
{
	unsigned k, tone;

	for (k = 0; k < 4; k++) {
		double complex ab = leakage_get(pair->nominal[0],
						partner[1], k);
		double complex ba = leakage_get(pair->nominal[1],
						partner[0], k);

		pair_values_unmix(raw[0][k], raw[1][k], ab, ba,
				  &unmixed[0][k], &unmixed[1][k]);
	}
	for (tone = 0; tone < 2; tone++)
		result[tone] = estimate_get(unmixed[tone], pair->nominal[tone]);
}

static void pair_estimate3(const double complex raw[2][4],
			   const struct tone_pair *pair,
			   double complex unmixed[2][4],
			   struct estimate *result, struct estimate *first)
{
	double partner[2];
	unsigned tone;

	pair_unmix(raw, pair, pair->nominal, unmixed, first);
	for (tone = 0; tone < 2; tone++)
		partner[tone] = pair->nominal[tone] +
			(isfinite(first[tone].delta) ? first[tone].delta : 0.0);
	pair_unmix(raw, pair, partner, unmixed, result);
}

static void pair_half_estimate(const double *samples,
			       const struct tone_pair *pair,
			       struct estimate *result)
{
	static const unsigned starts[] = { 0, 102 };
	static const unsigned ends[] = { 101, 204 };
	static const double centroids[] = { 50.5, 153.0 };
	double complex unmixed[2][2];
	double partner[2];
	double span = centroids[1] - centroids[0];
	double step = UDSP_SAMPLE_RATE / span;
	unsigned k, tone;

	for (tone = 0; tone < 2; tone++)
		partner[tone] = pair->nominal[tone] +
			(isfinite(result[tone].delta) ? result[tone].delta : 0.0);
	for (k = 0; k < 2; k++) {
		unsigned length = ends[k] - starts[k] + 1;
		double complex a = slice_get(samples, pair->nominal[0],
					     starts[k], ends[k]);
		double complex b = slice_get(samples, pair->nominal[1],
					     starts[k], ends[k]);
		double complex ab = slice_leakage_get(pair->nominal[0],
					partner[1], length, centroids[k]);
		double complex ba = slice_leakage_get(pair->nominal[1],
					partner[0], length, centroids[k]);

		pair_values_unmix(a, b, ab, ba,
				  &unmixed[0][k], &unmixed[1][k]);
	}
	for (tone = 0; tone < 2; tone++) {
		double omega = 2.0 * PI * pair->nominal[tone] / UDSP_SAMPLE_RATE;
		double complex product = unmixed[tone][1] * conj(unmixed[tone][0]);
		double phi, fine;

		result[tone].delta = NAN;
		result[tone].sum = 0.0;
		result[tone].hops = 0.0;
		result[tone].adjacent = 0;
		if (!isfinite(result[tone].coarse) || power_get(product) == 0.0)
			continue;
		phi = angle_wrap(carg(product) - omega * (ends[1] - ends[0]));
		fine = phi * UDSP_SAMPLE_RATE / (2.0 * PI * span);
		result[tone].delta = fine +
			round((result[tone].coarse - fine) / step) * step;
		result[tone].sum = cabs(product) * phase_get(phi);
		result[tone].hops = span;
		result[tone].adjacent = 1;
	}
}

static void pair_result_get(const double *samples, const struct tone_pair *pair,
			    enum pair_method method, struct estimate *result,
			    struct estimate *first)
{
	if (method == PAIR_UNMIX || method == PAIR_HALF) {
		double complex raw[2][4];
		double complex unmixed[2][4];

		pair_raw_get(samples, pair, raw);
		pair_estimate3(raw, pair, unmixed, result, first);
		if (method == PAIR_HALF)
			pair_half_estimate(samples, pair, result);
		return;
	}
	if (method == PAIR_CANCEL2)
		pair_estimate2(samples, pair, result, first);
	else
		pair_estimate(samples, pair, result);
}

static void accuracy_cell_print(unsigned digit, double twist,
				enum offset_kind offset, unsigned fill,
				enum pair_method method)
{
	static const char *const tables[] = {
		"accuracy", "accuracy2", "accuracy3", "accuracy4",
	};
	struct tone_pair pair;
	struct estimate result[2];
	struct estimate first[2];
	double samples[WINDOW_SAMPLES];

	pair_init(&pair, digit, -10.0, twist, offset);
	pair_window_fill(samples, &pair, fill);
	pair_result_get(samples, &pair, method, result, first);
	printf("%s\t%c\t-10\t%+.0f\t%s\t%u\t%.6f\t%.6f\t"
	       "%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%x\t%x\t%u\t%u",
	       tables[method],
	       digits[digit], twist, offset_name[offset], fill,
	       pair.delta[0], pair.delta[1], result[0].coarse, result[1].coarse,
	       result[0].delta, result[1].delta,
	       result[0].delta - pair.delta[0], result[1].delta - pair.delta[1],
	       result[0].mask, result[1].mask, result[0].loaded, result[1].loaded);
	if (method != PAIR_CANCEL)
		printf("\t%.6f\t%.6f", first[0].delta - pair.delta[0],
		       first[1].delta - pair.delta[1]);
	putchar('\n');
}

static void accuracy_run(bool quick, enum pair_method method)
{
	static const double twists[] = { -8, -6, 0, 6 };
	static const unsigned fills[] = { 77, 88, 99, 120, 153, 205 };
	unsigned cell;

	printf("table\tdigit\tlevel_dbm0\ttwist_db\toffset\tfill_samples\t"
	     "row_true_hz\tcolumn_true_hz\trow_coarse_hz\tcolumn_coarse_hz\t"
	     "row_delta_hz\tcolumn_delta_hz\trow_error_hz\tcolumn_error_hz\t"
	     "row_mask_hex_lsb_oldest\tcolumn_mask_hex_lsb_oldest\t"
	     "row_loaded\tcolumn_loaded");
	if (method != PAIR_CANCEL)
		printf("\trow_pass1_error_hz\tcolumn_pass1_error_hz");
	putchar('\n');
	for (cell = 0; cell < 16u * 4u * OFFSET_COUNT * 6u; cell++) {
		unsigned digit = cell / (4u * OFFSET_COUNT * 6u);
		unsigned twist = cell / (OFFSET_COUNT * 6u) % 4u;
		enum offset_kind offset = cell / 6u % OFFSET_COUNT;

		if (quick && digit % 5u)
			continue;
		accuracy_cell_print(digit, twists[twist], offset,
				    fills[cell % 6u], method);
	}
}

static void load_result_add(struct load_result *load,
			    const struct estimate *estimate, unsigned fill)
{
	if (!isfinite(load->loaded_at) && estimate->loaded >= 2) {
		load->loaded_at = fill;
		load->mask = estimate->mask;
	}
	if (!isfinite(load->estimated_at) && isfinite(estimate->delta))
		load->estimated_at = fill;
}

static void load_cell_print(unsigned digit, enum pair_method method)
{
	static const char *const tables[] = { "load", "load2", "load3", "load4" };
	struct tone_pair pair;
	struct load_result load[2] = {
		{ .loaded_at = NAN, .estimated_at = NAN },
		{ .loaded_at = NAN, .estimated_at = NAN },
	};
	double together = NAN;
	double estimated_together = NAN;
	unsigned fill;

	pair_init(&pair, digit, -10.0, 0.0, OFFSET_NOMINAL);
	for (fill = 1; fill <= WINDOW_SAMPLES; fill++) {
		struct estimate result[2];
		struct estimate first[2];
		double samples[WINDOW_SAMPLES];
		unsigned tone;

		pair_window_fill(samples, &pair, fill);
		pair_result_get(samples, &pair, method, result, first);
		for (tone = 0; tone < 2; tone++)
			load_result_add(&load[tone], &result[tone], fill);
		if (!isfinite(together) && result[0].loaded >= 2 &&
		    result[1].loaded >= 2)
			together = fill;
		if (!isfinite(estimated_together) && isfinite(result[0].delta) &&
		    isfinite(result[1].delta))
			estimated_together = fill;
	}
	printf("%s\t%c\t-10\t0\t%.0f\t%.0f\t%.0f\t%x\t%x\t%.0f\t"
	       "%.0f\t%.0f\n", tables[method],
	       digits[digit], load[0].loaded_at,
	       load[1].loaded_at, together, load[0].mask, load[1].mask,
	       load[0].estimated_at, load[1].estimated_at, estimated_together);
}

static void load_run(bool quick, enum pair_method method)
{
	unsigned digit;

	puts("table\tdigit\tlevel_dbm0\ttwist_db\trow_load_samples\t"
	     "column_load_samples\tboth_load_samples\trow_mask_hex_lsb_oldest\t"
	     "column_mask_hex_lsb_oldest\trow_estimate_samples\t"
	     "column_estimate_samples\tboth_estimate_samples");
	for (digit = 0; digit < 16; digit += quick ? 5u : 1u)
		load_cell_print(digit, method);
}

static double uniform_get(uint32_t *state)
{
	*state ^= *state << 13;
	*state ^= *state >> 17;
	*state ^= *state << 5;
	return (*state + 0.5) / 4294967296.0;
}

static double noise_get(uint32_t *state)
{
	double u = uniform_get(state);
	double v = uniform_get(state);

	return sqrt(-2.0 * log(u)) * cos(2.0 * PI * v);
}

static void pair_hold_fill(double *samples, const struct tone_pair *pair,
			   double snr, uint32_t seed)
{
	double signal_energy = 0.0;
	double noise_rms = 0.0;
	uint32_t state = seed;
	unsigned i;

	for (i = 0; i < HOLD_SAMPLES; i++) {
		samples[i] = pair_sample_get(pair, i);
		signal_energy += samples[i] * samples[i];
	}
	if (isfinite(snr))
		noise_rms = sqrt(signal_energy / HOLD_SAMPLES) *
			pow(10.0, -snr / 20.0);
	for (i = 0; i < HOLD_SAMPLES; i++)
		samples[i] += noise_rms * noise_get(&state);
}

static void snr_print(double snr)
{
	if (isfinite(snr))
		printf("%.0f", snr);
	else
		printf("none");
}

static void hold_result_add(struct hold_result *result, double delta)
{
	if (!isfinite(delta)) {
		result->missing++;
		result->previous_valid = false;
		return;
	}
	if (!result->estimates) {
		result->first = delta;
		result->anchor = 0.0;
	}
	result->anchor = fmax(result->anchor, fabs(delta - result->first));
	if (result->previous_valid)
		result->step = fmax(result->step, fabs(delta - result->previous));
	result->previous = delta;
	result->previous_valid = true;
	result->estimates++;
}

static void hold_cell_print(unsigned digit, double twist,
			    enum offset_kind offset, double snr)
{
	struct tone_pair pair;
	struct hold_result result[2] = {
		{ .first = NAN, .anchor = NAN, .step = NAN },
		{ .first = NAN, .anchor = NAN, .step = NAN },
	};
	double samples[HOLD_SAMPLES];
	uint32_t seed = 0x5eed1234u + digit;
	unsigned i;

	pair_init(&pair, digit, -10.0, twist, offset);
	pair_hold_fill(samples, &pair, snr, seed);
	for (i = 0; i + WINDOW_SAMPLES <= HOLD_SAMPLES; i += 40) {
		struct estimate estimate[2];

		pair_estimate(samples + i, &pair, estimate);
		hold_result_add(&result[0], estimate[0].delta);
		hold_result_add(&result[1], estimate[1].delta);
	}
	printf("hold\t%c\t-10\t%+.0f\t%s\t", digits[digit], twist,
	       offset_name[offset]);
	snr_print(snr);
	printf("\t300\t40\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t"
	       "%u\t%u\t%u\t%u\t%u\n", result[0].first, result[1].first,
	       result[0].anchor, result[1].anchor, result[0].step, result[1].step,
	       result[0].estimates, result[1].estimates, result[0].missing,
	       result[1].missing, seed);
}

static void hold_run(bool quick)
{
	static const double twists[] = { -8, 0, 6 };
	static const double snrs[] = { INFINITY, 30, 20, 15 };
	unsigned cell;

	puts("table\tdigit\tlevel_dbm0\ttwist_db\toffset\tsnr_db\ton_ms\t"
	     "slide_samples\trow_first_hz\tcolumn_first_hz\trow_anchor_hz\t"
	     "column_anchor_hz\trow_step_hz\tcolumn_step_hz\trow_estimates\t"
	     "column_estimates\trow_missing\tcolumn_missing\tseed");
	for (cell = 0; cell < 16u * 3u * 2u * 4u; cell++) {
		unsigned digit = cell / 24u;
		enum offset_kind offset = cell / 4u % 2u ?
			OFFSET_PLUS_EDGE : OFFSET_NOMINAL;

		if (quick && digit % 5u)
			continue;
		hold_cell_print(digit, twists[cell / 8u % 3u], offset,
				snrs[cell % 4u]);
	}
}

static void bias_cell_print(unsigned digit, enum offset_kind offset,
			    unsigned fill, enum pair_method method)
{
	static const char *const tables[] = { "bias", "bias2", "bias3", "bias4" };
	struct tone_pair pair;
	struct estimate result[2];
	struct estimate first[2];
	double samples[WINDOW_SAMPLES];
	double relief[2];
	unsigned tone;

	pair_init(&pair, digit, -10.0, 0.0, offset);
	pair_window_fill(samples, &pair, fill);
	pair_result_get(samples, &pair, method, result, first);
	for (tone = 0; tone < 2; tone++)
		relief[tone] = loss_get(fill, pair.delta[tone]) /
			loss_get(result[tone].fill, pair.delta[tone]);
	printf("%s\t%c\t-10\t0\t%s\t%u\t%.6f\t%.6f\t%.6f\t%.6f\t"
	       "%x\t%x\t%u\t%u\t%.9f\t%.9f\n",
	       tables[method], digits[digit],
	       offset_name[offset], fill, pair.delta[0], pair.delta[1],
	       result[0].delta - pair.delta[0], result[1].delta - pair.delta[1],
	       result[0].mask, result[1].mask, result[0].fill, result[1].fill,
	       relief[0], relief[1]);
}

static void bias_run(bool quick, enum pair_method method)
{
	unsigned cell;

	puts("table\tdigit\tlevel_dbm0\ttwist_db\toffset\tfill_samples\t"
	     "row_true_hz\tcolumn_true_hz\trow_error_hz\tcolumn_error_hz\t"
	     "row_mask_hex_lsb_oldest\tcolumn_mask_hex_lsb_oldest\t"
	     "row_est_fill\tcolumn_est_fill\trow_relief_error\t"
	     "column_relief_error");
	for (cell = 0; cell < 16u * 2u * 13u; cell++) {
		unsigned digit = cell / 26u;
		unsigned fill = 51u + 13u * (cell % 13u);
		enum offset_kind offset = cell / 13u % 2u ?
			OFFSET_MINUS_EDGE : OFFSET_PLUS_EDGE;

		if (quick && digit % 5u)
			continue;
		bias_cell_print(digit, offset,
				fill > WINDOW_SAMPLES ? WINDOW_SAMPLES : fill,
				method);
	}
}

static void estimator_run(bool quick)
{
	if (quick)
		fprintf(stderr, "quick estimator: digits 1,5,9,D in all four "
			"tables; all other dimensions retained\n");
	accuracy_run(quick, PAIR_CANCEL);
	load_run(quick, PAIR_CANCEL);
	hold_run(quick);
	bias_run(quick, PAIR_CANCEL);
}

static void estimator2_run(bool quick)
{
	accuracy_run(quick, PAIR_CANCEL2);
}

static void load2_run(bool quick)
{
	load_run(quick, PAIR_CANCEL2);
}

static void bias2_run(bool quick)
{
	bias_run(quick, PAIR_CANCEL2);
}

static void accuracy3_run(bool quick)
{
	accuracy_run(quick, PAIR_UNMIX);
}

static void load3_run(bool quick)
{
	load_run(quick, PAIR_UNMIX);
}

static void bias3_run(bool quick)
{
	bias_run(quick, PAIR_UNMIX);
}

static void accuracy4_run(bool quick)
{
	accuracy_run(quick, PAIR_HALF);
}

static void load4_run(bool quick)
{
	load_run(quick, PAIR_HALF);
}

static void bias4_run(bool quick)
{
	bias_run(quick, PAIR_HALF);
}

static void run_result_add(struct run_result *run,
			   const struct estimate *estimate)
{
	run->sum += estimate->sum;
	run->magnitude += cabs(estimate->sum);
	run->hops += estimate->hops;
	run->adjacent += estimate->adjacent;
	if (estimate->adjacent)
		run->coarse += estimate->coarse * cabs(estimate->sum);
}

static struct run_snapshot run_snapshot_get(const struct run_result *run,
					    enum pair_method method)
{
	struct run_snapshot result = {
		.delta = { NAN, NAN }, .coherence = { NAN, NAN },
	};
	unsigned tone;

	for (tone = 0; tone < 2; tone++) {
		if (!run[tone].adjacent || run[tone].magnitude == 0.0)
			continue;
		result.coherence[tone] = cabs(run[tone].sum) /
			run[tone].magnitude;
		if (power_get(run[tone].sum) == 0.0)
			continue;
		result.delta[tone] = carg(run[tone].sum) * UDSP_SAMPLE_RATE /
			(2.0 * PI * run[tone].hops / run[tone].adjacent);
		if (method == PAIR_HALF) {
			double step = UDSP_SAMPLE_RATE / 102.5;
			double coarse = run[tone].coarse / run[tone].magnitude;

			result.delta[tone] +=
				round((coarse - result.delta[tone]) / step) * step;
		}
	}
	return result;
}

static struct hold2_result hold2_result_get(const double *samples,
					  const struct tone_pair *pair,
					  enum pair_method method)
{
	struct hold2_result result = {
		.instant = {
			{ .first = NAN, .anchor = NAN, .step = NAN },
			{ .first = NAN, .anchor = NAN, .step = NAN },
		},
	};
	struct run_result run[2] = { 0 };
	unsigned i;

	for (i = 0; i + WINDOW_SAMPLES <= HOLD_SAMPLES; i += 40) {
		struct estimate estimate[2];
		struct estimate first[2];
		unsigned tone;

		pair_result_get(samples + i, pair, method, estimate, first);
		for (tone = 0; tone < 2; tone++) {
			hold_result_add(&result.instant[tone],
					estimate[tone].delta);
			run_result_add(&run[tone], &estimate[tone]);
		}
		result.decisions++;
		if (result.decisions == 3)
			result.at3 = run_snapshot_get(run, method);
		if (result.decisions == 7)
			result.at7 = run_snapshot_get(run, method);
	}
	result.end = run_snapshot_get(run, method);
	return result;
}

static void run_error_print(const struct run_snapshot *result,
			    const struct tone_pair *pair)
{
	printf("\t%.6f\t%.6f\t%.6f\t%.6f", result->delta[0], result->delta[1],
	       result->delta[0] - pair->delta[0],
	       result->delta[1] - pair->delta[1]);
}

static void hold2_cell_print(unsigned digit, double twist,
			     enum offset_kind offset, double snr,
			     enum pair_method method)
{
	struct tone_pair pair;
	struct hold2_result result;
	double samples[HOLD_SAMPLES];
	uint32_t seed = 0x5eed1234u + digit;

	pair_init(&pair, digit, -10.0, twist, offset);
	pair_hold_fill(samples, &pair, snr, seed);
	result = hold2_result_get(samples, &pair, method);
	printf("%s\t%c\t-10\t%+.0f\t%s\t",
	       method == PAIR_HALF ? "hold4" :
	       method == PAIR_UNMIX ? "hold3" : "hold2",
	       digits[digit], twist, offset_name[offset]);
	snr_print(snr);
	printf("\t300\t40\t%.6f\t%.6f\t%.6f\t%.6f", pair.delta[0],
	       pair.delta[1], result.instant[0].step, result.instant[1].step);
	run_error_print(&result.at3, &pair);
	run_error_print(&result.at7, &pair);
	run_error_print(&result.end, &pair);
	printf("\t%.9f\t%.9f\t%.9f\t%.9f\t%u\t%u\t%u\t%u\t%u\t%u\n",
	       result.at7.coherence[0], result.at7.coherence[1],
	       result.end.coherence[0], result.end.coherence[1],
	       result.instant[0].estimates, result.instant[1].estimates,
	       result.instant[0].missing, result.instant[1].missing,
	       result.decisions, seed);
}

static void hold_accum_run(bool quick, enum pair_method method)
{
	static const double twists[] = { -8, -6, 0, 6 };
	static const double snrs[] = { INFINITY, 30, 20, 15 };
	unsigned cell;

	puts("table\tdigit\tlevel_dbm0\ttwist_db\toffset\tsnr_db\ton_ms\t"
	     "slide_samples\trow_true_hz\tcolumn_true_hz\trow_step_hz\t"
	     "column_step_hz\trow_accum3_hz\tcolumn_accum3_hz\trow_error3_hz\t"
	     "column_error3_hz\trow_accum7_hz\tcolumn_accum7_hz\t"
	     "row_error7_hz\t"
	     "column_error7_hz\trow_accum_end_hz\tcolumn_accum_end_hz\t"
	     "row_error_end_hz\tcolumn_error_end_hz\trow_coherence7\t"
	     "column_coherence7\trow_coherence_end\tcolumn_coherence_end\t"
	     "row_estimates\tcolumn_estimates\trow_missing\tcolumn_missing\t"
	     "decisions\tseed");
	for (cell = 0; cell < 16u * 4u * 2u * 4u; cell++) {
		unsigned digit = cell / 32u;
		enum offset_kind offset = cell / 4u % 2u ?
			OFFSET_PLUS_EDGE : OFFSET_NOMINAL;

		if (quick && digit % 5u)
			continue;
		hold2_cell_print(digit, twists[cell / 8u % 4u], offset,
				 snrs[cell % 4u], method);
	}
}

static void hold2_run(bool quick)
{
	hold_accum_run(quick, PAIR_CANCEL2);
}

static void hold3_run(bool quick)
{
	hold_accum_run(quick, PAIR_UNMIX);
}

static void hold4_run(bool quick)
{
	hold_accum_run(quick, PAIR_HALF);
}

static void pair_vary_fill(double *samples, const struct tone_pair *pair,
			   double drift_pct, double jitter_pct, uint32_t seed)
{
	double phase[2] = { 0.0, 0.0 };
	double scale = 1.0;
	double frame_scale = 1.0;
	uint32_t state = seed;
	unsigned i;

	for (i = 0; i < HOLD_SAMPLES; i++) {
		if (i % UDSP_FRAME_SAMPLES == 0) {
			frame_scale = scale *
				(1.0 + jitter_pct *
				 (2.0 * uniform_get(&state) - 1.0) / 100.0);
			scale *= 1.0 + drift_pct / 100.0;
		}
		samples[i] = pair->amplitude[0] * sin(phase[0]) +
			pair->amplitude[1] * sin(phase[1]);
		phase[0] = angle_wrap(phase[0] + pair->omega[0] * frame_scale);
		phase[1] = angle_wrap(phase[1] + pair->omega[1] * frame_scale);
	}
}

static void coherence_cell_print(unsigned digit, double twist,
				 enum offset_kind offset, double snr,
				 double drift_pct, double jitter_pct,
				 enum pair_method method)
{
	struct tone_pair pair;
	struct hold2_result result;
	double samples[HOLD_SAMPLES];
	uint32_t seed = 0x5eed1234u + digit;
	const char *kind = drift_pct ? "drift" :
		jitter_pct ? "jitter" : "steady";

	pair_init(&pair, digit, -10.0, twist, offset);
	if (drift_pct || jitter_pct)
		pair_vary_fill(samples, &pair, drift_pct, jitter_pct, seed);
	else
		pair_hold_fill(samples, &pair, snr, seed);
	result = hold2_result_get(samples, &pair, method);
	printf("%s\t%s\t%c\t-10\t%+.0f\t%s\t",
	       method == PAIR_HALF ? "coherence4" :
	       method == PAIR_UNMIX ? "coherence3" : "coherence",
	       kind, digits[digit], twist, offset_name[offset]);
	snr_print(snr);
	printf("\t300\t40\t%.1f\t%.1f\t%.6f\t%.6f\t%.9f\t%.9f\t"
	       "%.6f\t%.6f\t%.9f\t%.9f\t%.6f\t%.6f\t%u\t%u\t%u\t%u\t%u\t%u\n",
	       drift_pct, jitter_pct, pair.nominal[0], pair.nominal[1],
	       result.at7.coherence[0], result.at7.coherence[1],
	       result.at7.delta[0], result.at7.delta[1],
	       result.end.coherence[0], result.end.coherence[1],
	       result.end.delta[0], result.end.delta[1],
	       result.instant[0].estimates, result.instant[1].estimates,
	       result.instant[0].missing, result.instant[1].missing,
	       result.decisions, seed);
}

static void coherence_method_run(bool quick, enum pair_method method)
{
	static const double twists[] = { -8, 0, 6 };
	static const double snrs[] = { INFINITY, 20, 15 };
	static const double drifts[] = { 0.8, 1.4, 2.0 };
	unsigned cell;

	puts("table\tsignal_kind\tdigit\tlevel_dbm0\ttwist_db\toffset\tsnr_db\t"
	     "on_ms\tslide_samples\tdrift_pct_per_10ms\tjitter_pct_per_10ms\t"
	     "row_nominal_hz\tcolumn_nominal_hz\trow_coherence7\t"
	     "column_coherence7\trow_accum7_hz\tcolumn_accum7_hz\t"
	     "row_coherence_end\tcolumn_coherence_end\trow_accum_end_hz\t"
	     "column_accum_end_hz\trow_estimates\tcolumn_estimates\t"
	     "row_missing\t"
	     "column_missing\tdecisions\tseed");
	for (cell = 0; cell < 16u * 3u * 2u * 3u; cell++) {
		unsigned digit = cell / 18u;
		enum offset_kind offset = cell / 3u % 2u ?
			OFFSET_PLUS_EDGE : OFFSET_NOMINAL;

		if (quick && digit % 5u)
			continue;
		coherence_cell_print(digit, twists[cell / 6u % 3u], offset,
				     snrs[cell % 3u], 0.0, 0.0, method);
	}
	for (cell = 0; cell < ARRAY_COUNT(drifts); cell++)
		coherence_cell_print(0, 0.0, OFFSET_NOMINAL, INFINITY,
				     drifts[cell], 0.0, method);
	coherence_cell_print(0, 0.0, OFFSET_NOMINAL, INFINITY, 0.0, 0.4,
			     method);
}

static void coherence_run(bool quick)
{
	coherence_method_run(quick, PAIR_CANCEL2);
}

static void coherence3_run(bool quick)
{
	coherence_method_run(quick, PAIR_UNMIX);
}

static void coherence4_run(bool quick)
{
	coherence_method_run(quick, PAIR_HALF);
}

static double twist_get(const double complex value[2][4])
{
	double energy[2] = { 0.0, 0.0 };
	unsigned k;

	for (k = 0; k < 4; k++) {
		energy[0] += power_get(value[0][k]);
		energy[1] += power_get(value[1][k]);
	}
	return 10.0 * log10(energy[1] / energy[0]);
}

static void twist_cell_print(unsigned digit, double level, double twist,
			     enum offset_kind offset, double snr)
{
	struct tone_pair pair;
	struct hold_result result[2] = {
		{ .first = NAN, .anchor = NAN, .step = NAN },
		{ .first = NAN, .anchor = NAN, .step = NAN },
	};
	double error_sum[2] = { 0.0, 0.0 };
	double worst[2] = { 0.0, 0.0 };
	double samples[HOLD_SAMPLES];
	uint32_t seed = 0x5eed1234u + digit;
	unsigned i;

	pair_init(&pair, digit, level, twist, offset);
	pair_hold_fill(samples, &pair, snr, seed);
	for (i = 0; i + WINDOW_SAMPLES <= HOLD_SAMPLES; i += 40) {
		struct estimate estimate[2];
		struct estimate first[2];
		double complex raw[2][4];
		double complex unmixed[2][4];
		double measured[2];
		unsigned method;

		pair_raw_get(samples + i, &pair, raw);
		pair_estimate3(raw, &pair, unmixed, estimate, first);
		measured[0] = twist_get(raw);
		measured[1] = twist_get(unmixed);
		for (method = 0; method < 2; method++) {
			double error = measured[method] - twist;

			error_sum[method] += error;
			worst[method] = fmax(worst[method], fabs(error));
			hold_result_add(&result[method], measured[method]);
		}
	}
	printf("twist\t%c\t%.0f\t%+.0f\t%s\t", digits[digit], level, twist,
	       offset_name[offset]);
	snr_print(snr);
	printf("\t300\t40\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%u\t%u\n",
	       error_sum[0] / result[0].estimates, worst[0],
	       error_sum[1] / result[1].estimates, worst[1],
	       result[0].step, result[1].step, result[0].estimates, seed);
}

static void twist_run(bool quick)
{
	static const double levels[] = { -3, -13, -23, -28 };
	static const double twists[] = { -8, -6, -3, 0, 3, 6 };
	static const enum offset_kind offsets[] = {
		OFFSET_NOMINAL, OFFSET_PLUS_EDGE, OFFSET_MINUS_EDGE,
	};
	static const double snrs[] = { INFINITY, 20, 15 };
	unsigned cell;

	puts("table\tdigit\tlevel_dbm0\ttwist_db\toffset\tsnr_db\ton_ms\t"
	     "slide_samples\traw_mean_error_db\traw_worst_abs_error_db\t"
	     "unmixed_mean_error_db\tunmixed_worst_abs_error_db\t"
	     "raw_max_step_db\tunmixed_max_step_db\tdecisions\tseed");
	for (cell = 0; cell < 16u * 4u * 6u * 3u * 3u; cell++) {
		unsigned digit = cell / 216u;
		unsigned snr = cell % 3u;

		if (quick && (digit % 5u || snr == 1u))
			continue;
		twist_cell_print(digit, levels[cell / 54u % 4u],
				 twists[cell / 9u % 6u],
				 offsets[cell / 3u % 3u], snrs[snr]);
	}
}

struct cadence_echo {
	unsigned delay_ms;
	double attenuation_db;
};

static double cadence_sample_get(const struct tone_pair *pairs, unsigned n,
				 unsigned start, unsigned on, unsigned off)
{
	unsigned period = on + off;
	unsigned relative = n >= start ? n - start : 10u * period;
	unsigned digit = relative / period;
	unsigned phase = relative % period;

	if (digit < 10 && phase < on)
		return pair_sample_get(&pairs[digit], phase);
	return 0.0;
}

static void cadence_frame_fill(int16_t *samples, const struct tone_pair *pairs,
			       unsigned fed, unsigned start, unsigned on,
			       unsigned off, const struct cadence_echo *echo)
{
	unsigned delay = echo ? echo->delay_ms * UDSP_SAMPLE_RATE / 1000u : 0u;
	double gain = echo ? pow(10.0, -echo->attenuation_db / 20.0) : 0.0;
	unsigned i;

	for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
		unsigned n = fed + i;
		double value = cadence_sample_get(pairs, n, start, on, off);

		if (echo)
			value += gain * cadence_sample_get(pairs, n,
						  start + delay, on, off);
		samples[i] = udsp_sat16_round(value);
	}
}

static struct event_count cadence_events_get(const struct tone_pair *pairs,
					     unsigned on_ms, unsigned off_ms,
					     unsigned lead,
					     const struct cadence_echo *echo)
{
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	struct event_count count = { 0 };
	unsigned on = on_ms * UDSP_SAMPLE_RATE / 1000u;
	unsigned off = off_ms * UDSP_SAMPLE_RATE / 1000u;
	unsigned delay = echo ? echo->delay_ms * UDSP_SAMPLE_RATE / 1000u : 0u;
	unsigned start = WARM_SAMPLES + lead;
	unsigned end = start + 10u * (on + off) + delay + TAIL_SAMPLES;
	unsigned fed;

	udsp_dtmf_detector_init(&detector, false);
	for (fed = 0; fed < end; fed += UDSP_FRAME_SAMPLES) {
		int16_t samples[UDSP_FRAME_SAMPLES];

		cadence_frame_fill(samples, pairs, fed, start, on, off, echo);
		if (udsp_dtmf_detector_process(&detector, samples, &event))
			event_add(&count, &event);
	}
	if (udsp_dtmf_detector_flush(&detector, &event))
		event_add(&count, &event);
	return count;
}

static void cadence_cell_print(unsigned on_ms, unsigned off_ms,
			       enum offset_kind offset, unsigned stride,
			       const struct cadence_echo *echo)
{
	static const unsigned sequence[] = { 0, 1, 2, 4, 5, 6, 8, 9, 10, 13 };
	struct tone_pair pairs[10];
	unsigned histogram[11] = { 0 };
	unsigned misses[10] = { 0 };
	unsigned excess = 0, unexpected = 0, duplicate = 0, leads = 0;
	unsigned i, lead;

	for (i = 0; i < 10; i++)
		pair_init(&pairs[i], sequence[i], -10.0, 0.0, offset);
	for (lead = 0; lead < UDSP_FRAME_SAMPLES; lead += stride) {
		struct event_count count = cadence_events_get(
			pairs, on_ms, off_ms, lead, echo);

		leads++;
		if (count.total <= 10)
			histogram[count.total]++;
		else
			excess++;
		unexpected += count.other + count.digit[3] + count.digit[7] +
			count.digit[11] + count.digit[12] + count.digit[14] +
			count.digit[15];
		for (i = 0; i < 10; i++) {
			unsigned detected = count.digit[sequence[i]];

			misses[i] += !detected;
			duplicate += detected > 1 ? detected - 1 : 0;
		}
	}
	printf("%s\t%u\t%u\t%s\t%u\t%u", echo ? "echo" : "cadence",
	       on_ms, off_ms, offset_name[offset], leads, stride);
	for (i = 11; i > 0; i--)
		printf("\t%u", histogram[i - 1]);
	printf("\t%u\t%u\t%u", excess, unexpected, duplicate);
	for (i = 0; i < 10; i++)
		printf("\t%u", misses[i]);
	if (echo)
		printf("\t%ums/-%.0fdB", echo->delay_ms, echo->attenuation_db);
	putchar('\n');
}

static void cadence_header_print(bool echo)
{
	printf("table\ton_ms\toff_ms\toffset\tleads\tlead_step\tcount_10\t"
	     "count_9\tcount_8\tcount_7\tcount_6\tcount_5\tcount_4\tcount_3\t"
	     "count_2\tcount_1\tcount_0\tcount_over_10\tunexpected_events\t"
	     "duplicate_events\tmiss_1\tmiss_2\tmiss_3\tmiss_4\tmiss_5\t"
	     "miss_6\tmiss_7\tmiss_8\tmiss_9\tmiss_0");
	puts(echo ? "\techo" : "");
}

static void cadence_run(bool quick)
{
	static const unsigned durations[] = { 50, 40, 35, 30, 20 };
	unsigned cell;

	if (quick)
		fprintf(stderr, "quick cadence: all cadences and offsets; "
			"leads 0..70 step 10\n");
	cadence_header_print(false);
	for (cell = 0; cell < ARRAY_COUNT(durations) * 2u; cell++)
		cadence_cell_print(durations[cell / 2u], 60, cell % 2u ?
				   OFFSET_PLUS_EDGE : OFFSET_NOMINAL,
				   quick ? 10u : 1u, NULL);
}

static void echo_run(bool quick)
{
	static const struct cadence_echo echo = {
		.delay_ms = 20, .attenuation_db = 10.0,
	};
	static const unsigned durations[][2] = { { 50, 60 }, { 41, 40 } };
	unsigned cell;

	if (quick)
		fprintf(stderr, "quick echo: both cadences and offsets; "
			"leads 0..70 step 10\n");
	cadence_header_print(true);
	for (cell = 0; cell < ARRAY_COUNT(durations) * 2u; cell++)
		cadence_cell_print(durations[cell / 2u][0],
				   durations[cell / 2u][1], cell % 2u ?
				   OFFSET_PLUS_EDGE : OFFSET_NOMINAL,
				   quick ? 10u : 1u, &echo);
}

/*
 * The cadence table dials ten different digits, so it never meets the guard
 * that keeps an ended digit from validating again at once. The repeat rows
 * dial repeated digits and compare the events with the dial string in order,
 * which event_count cannot do. The bridge rows dial one digit ten times, so
 * every pause that splits adds an event. The pause walks in steps of one
 * sample near 40 ms, the split bound of the detector, and near 70 ms.
 */
#define REPEAT_EVENTS_MAX	16u

static const char repeat_dial[] = "3342001145";
static const char bridge_dial[] = "5555555555";

static const struct {
	const char *name;
	enum offset_kind offsets[2];
} repeat_offsets[] = {
	{ "nominal", { OFFSET_NOMINAL, OFFSET_NOMINAL } },
	{ "+edge", { OFFSET_PLUS_EDGE, OFFSET_PLUS_EDGE } },
	{ "-edge", { OFFSET_MINUS_EDGE, OFFSET_MINUS_EDGE } },
	{ "+row-col", { OFFSET_PLUS_EDGE, OFFSET_MINUS_EDGE } },
	{ "-row+col", { OFFSET_MINUS_EDGE, OFFSET_PLUS_EDGE } },
};

struct repeat_run {
	char seen[REPEAT_EVENTS_MAX];
	unsigned events;
	unsigned blocked;
	unsigned peak_min;
	unsigned peak_max;
};

static void repeat_event_add(struct repeat_run *run,
			     const struct udsp_event *event)
{
	if (run->events < REPEAT_EVENTS_MAX)
		run->seen[run->events] = event->u.digit.digit;
	run->events++;
}

/* A different digit clears the guard. A frame that ends no digit can still
 * take the run of decisions without a valid digit back to zero under the
 * guard. That frame met a repeat of the ended digit. The count is of such
 * frames, and the run each cut is read at frame resolution: the frame can
 * hold one more failing decision. */
static bool repeat_blocked(const struct udsp_dtmf_detector *detector,
			   unsigned before, bool emitted)
{
	return before && !emitted && !detector->no_vsc_run &&
		detector->ended_digit && !detector->ended_digit_clear &&
		!detector->active && !detector->candidate;
}

static void repeat_peak_add(struct repeat_run *run, unsigned peak)
{
	run->blocked++;
	if (peak < run->peak_min)
		run->peak_min = peak;
	if (peak > run->peak_max)
		run->peak_max = peak;
}

static struct repeat_run repeat_events_get(const struct tone_pair *pairs,
					   unsigned on, unsigned off,
					   unsigned lead)
{
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	struct repeat_run run = { .peak_min = UINT_MAX };
	unsigned start = WARM_SAMPLES + lead;
	unsigned end = start + 10u * (on + off) + TAIL_SAMPLES;
	unsigned fed;

	udsp_dtmf_detector_init(&detector, false);
	for (fed = 0; fed < end; fed += UDSP_FRAME_SAMPLES) {
		int16_t samples[UDSP_FRAME_SAMPLES];
		unsigned before = detector.no_vsc_run;
		bool emitted;

		cadence_frame_fill(samples, pairs, fed, start, on, off, NULL);
		emitted = udsp_dtmf_detector_process(&detector, samples,
						     &event);
		if (emitted)
			repeat_event_add(&run, &event);
		if (repeat_blocked(&detector, before, emitted))
			repeat_peak_add(&run, before);
	}
	if (udsp_dtmf_detector_flush(&detector, &event))
		repeat_event_add(&run, &event);
	return run;
}

static void repeat_cell_print(const char *table, const char *dial,
			      unsigned on, unsigned off, unsigned offset,
			      unsigned stride)
{
	struct tone_pair pairs[10];
	unsigned leads = 0, exact = 0, blocked = 0;
	unsigned events_min = UINT_MAX, events_max = 0;
	unsigned peak_min = UINT_MAX, peak_max = 0;
	unsigned i, lead;

	for (i = 0; i < 10; i++)
		pair_offsets_init(&pairs[i],
				  (unsigned)(strchr(digits, dial[i]) - digits),
				  -10.0, 0.0, repeat_offsets[offset].offsets);
	for (lead = 0; lead < UDSP_FRAME_SAMPLES; lead += stride) {
		struct repeat_run run = repeat_events_get(pairs, on, off, lead);

		leads++;
		exact += run.events == 10 && !memcmp(run.seen, dial, 10);
		events_min = run.events < events_min ? run.events : events_min;
		events_max = run.events > events_max ? run.events : events_max;
		blocked += run.blocked;
		peak_min = run.peak_min < peak_min ? run.peak_min : peak_min;
		peak_max = run.peak_max > peak_max ? run.peak_max : peak_max;
	}
	printf("%s\t%.3f\t%.3f\t%u\t%s\t%u\t%u\t%u\t%u\t%u", table,
	       on * 1000.0 / UDSP_SAMPLE_RATE, off * 1000.0 / UDSP_SAMPLE_RATE,
	       off, repeat_offsets[offset].name, leads, exact, events_min,
	       events_max, blocked);
	if (blocked)
		printf("\t%u\t%u\n", peak_min, peak_max);
	else
		puts("\t-\t-");
}

static unsigned repeat_off_next(unsigned off, bool quick)
{
	unsigned ms = UDSP_SAMPLE_RATE / 1000u;
	bool fine = (off >= 28u * ms && off < 45u * ms) ||
		(off >= 60u * ms && off < 75u * ms);

	if (quick || !fine)
		return (off / ms + 1u) * ms;
	return off + 1u;
}

static void repeat_run(bool quick)
{
	static const struct {
		unsigned on_ms;
		unsigned offsets;
	} tones[] = {
		{ 70, ARRAY_COUNT(repeat_offsets) },
		{ 50, ARRAY_COUNT(repeat_offsets) },
		{ 41, 1 },
	};
	unsigned ms = UDSP_SAMPLE_RATE / 1000u;
	unsigned stride = quick ? 10u : 1u;
	unsigned t, offset, off;

	if (quick)
		fprintf(stderr, "quick repeat: pause steps 1 ms; "
			"leads 0..70 step 10\n");
	printf("table\ton_ms\toff_ms\toff_samples\toffset\tleads\tleads_exact\t"
	       "events_min\tevents_max\tblocked_frames\tblocked_peak_min\t"
	       "blocked_peak_max\n");
	for (t = 0; t < ARRAY_COUNT(tones); t++)
		for (offset = 0; offset < tones[t].offsets; offset++)
			for (off = 25u * ms; off <= 80u * ms;
			     off = repeat_off_next(off, quick))
				repeat_cell_print("repeat", repeat_dial,
						  tones[t].on_ms * ms, off,
						  offset, stride);
	for (offset = 0; offset < ARRAY_COUNT(repeat_offsets); offset++)
		for (off = 15u * ms; off <= 40u * ms; off += quick ? ms : 1u)
			repeat_cell_print("bridge", bridge_dial, 70u * ms, off,
					  offset, stride);
}

static int usage_print(const char *program)
{
	fprintf(stderr, "usage: %s accept|owalk|harmonic|estimator|cadence|"
		"estimator2|hold2|coherence|load2|bias2|accuracy3|hold3|load3|"
		"bias3|coherence3|twist|accuracy4|hold4|coherence4|load4|bias4|"
		"matrix|echo|repeat [quick]\n", program);
	return 2;
}

int main(int argc, char **argv)
{
	static const struct {
		const char *name;
		void (*run)(bool quick);
	} commands[] = {
		{ "accept", accept_run },
		{ "owalk", owalk_run },
		{ "harmonic", harmonic_run },
		{ "estimator", estimator_run },
		{ "cadence", cadence_run },
		{ "estimator2", estimator2_run },
		{ "hold2", hold2_run },
		{ "coherence", coherence_run },
		{ "load2", load2_run },
		{ "bias2", bias2_run },
		{ "accuracy3", accuracy3_run },
		{ "hold3", hold3_run },
		{ "load3", load3_run },
		{ "bias3", bias3_run },
		{ "coherence3", coherence3_run },
		{ "twist", twist_run },
		{ "accuracy4", accuracy4_run },
		{ "hold4", hold4_run },
		{ "coherence4", coherence4_run },
		{ "load4", load4_run },
		{ "bias4", bias4_run },
		{ "matrix", matrix_run },
		{ "echo", echo_run },
		{ "repeat", repeat_run },
	};
	unsigned i;

	if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "quick")))
		return usage_print(argv[0]);
	for (i = 0; i < ARRAY_COUNT(commands); i++) {
		if (strcmp(argv[1], commands[i].name))
			continue;
		commands[i].run(argc == 3);
		return ferror(stdout) ? 1 : 0;
	}
	return usage_print(argv[0]);
}
