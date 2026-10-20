// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <math.h>
#include <limits.h>
#include <string.h>

#include "dsp.h"

static const double dtmf_frequency[16] = {
	697.0, 770.0, 852.0, 941.0, 1209.0, 1336.0, 1477.0, 1633.0,
	1394.0, 1540.0, 1704.0, 1882.0, 2418.0, 2672.0, 2954.0, 3266.0,
};

static const char digit_map[4][4] = {
	{ '1', '2', '3', 'A' },
	{ '4', '5', '6', 'B' },
	{ '7', '8', '9', 'C' },
	{ '*', '0', '#', 'D' },
};

static const unsigned slice_start[6] = { 0, 51, 102, 153, 0, 102 };
static const unsigned slice_length[6] = { 51, 51, 51, 52, 102, 103 };
static const unsigned slice_end[6] = { 50, 101, 152, 204, 101, 204 };
static const double slice_centroid[6] = {
	25.0, 76.0, 127.0, 178.5, 50.5, 153.0,
};

struct offset_estimate {
	struct udsp_dtmf_offset_run run;
	double delta;
};

struct window_level {
	int8_t dbm0;
	bool steady;
};

/*
 * The talk-off bars. A field capture carries two false digits inside 233 s of
 * a real conversation, digit 5 at 60 ms and digit 1 at 40 ms, both from the
 * near-end talker's own speech. A detector that promotes a digit from a single
 * window that passes the guard turns any 25.6 ms accident in a formant pair
 * into an RFC 4733 event.
 *
 * Each bar below names the constraint it comes from: ETSI ES 201 235-3, the
 * ITU-T Q.24 national values, or a measurement where no
 * standard sets the number. All measurements come from the pair builders of
 * test_dsp.c, which set the level of each tone explicitly. They cover the 16
 * digits, the levels -3 to -23 dBm0, the twists -6 to +4 dB and every sample
 * alignment.
 */

/*
 * A candidate needs seven consecutive 5 ms decisions. At the 0.24 pair bar,
 * measured overlap O is about 12.0 to 12.75 ms in the 25.625 ms window W.
 * The bound h*n - W + 2*O, with hop h and count n, guarantees acceptance
 * at 35 ms and rejection below about 29 ms. ETSI ES 201 235-3 Table 3 requires
 * acceptance above 40 ms and rejection below 20 ms; the middle band permits
 * either result. Cold start needs a full window before decisions begin,
 * so its warm-up costs decisions outside these warm-start bounds.
 */
#define DTMF_HIT_DECISIONS_MIN	7u

/*
 * Stationary tones retain phase coherence; a glide rotates successive
 * half-window products. At validation, the 27060480-cell conformant sweep
 * reaches a minimum of 0.861039562. The test_dsp.c talk-off signals reach
 * at most 0.715055221; the 16 drift cells reach at most 0.628379201.
 * The 0.80 bar leaves 41.8 percent of the measured gap on the conformant side.
 * A failed check defers validation so further estimates can dilute onset error.
 */
#define DTMF_COHERENCE_MIN	0.80

/*
 * Four missed decisions preserve a digit; the fifth ends it. The caller sees
 * decisions per 10 ms frame, so five misses can delay cease about 43 ms
 * after a pause starts, beyond the ETSI Table 3 limit of 40 ms.
 * Four consecutive silent misses can safely end the digit one decision
 * earlier. Every window across an interruption below 20 ms retains at least
 * 5.7 ms of tone, about 430 RMS at -28 dBm0 per tone, above the 250 silence
 * line. Such an interruption cannot qualify for the earlier cease.
 */
#define DTMF_MISS_DECISIONS_MAX	4u

/*
 * An ended digit validates again only after this many consecutive decisions
 * without a valid digit. The run counts from the first miss, so the misses
 * that end the digit count too.
 *
 * O_on and O_off are the tone fills that one passing decision needs at the
 * onset and at the end of a tone. For a pause P, the run between two tones
 * spans P + O_on + O_off - W, about P - 1 ms at nominal frequency. With seven
 * decisions, a repeat splits at every alignment from a 36.625 ms pause
 * (bench_dtmf repeat). ETSI ES 201 235-3 4.2.2(a) requires the split above
 * 40 ms. One decision more misses that bound by 1.625 ms. A break below 20 ms
 * inside one key press never reports two digits (test_dsp.c
 * dtmf_break_test).
 */
#define DTMF_PAUSE_DECISIONS_MIN	7u

/*
 * The bar sets the position of the accept/reject band. Guaranteed warm
 * acceptance starts at 5*7 - 25.625 + 2*O ms, where O ~= 51.25*bar.
 * At 0.24, measured overlap O is 12.0 to 12.75 ms, so 35 ms holds at every
 * alignment. At 0.25 the 35 ms boundary has zero margin: the warm burst test
 * accepts 61 of 64 cases.
 * A 25 ms burst cannot supply the required 30 ms pass interval.
 * A 20 ms silent gap still bridges: its fail region stays below 20.9 ms.
 *
 * A pure pair puts 0.5 * energy * N into the two bins. The bar requires
 * 48 percent of that value. A clean pair reads 0.94 to 0.99 of the pure
 * value; a pair 1.5 percent off frequency reads 0.42 to 0.62.
 * The offset estimate relieves winner powers before this bar applies.
 * Relief uses the loaded span and stays clamped to the accept edge.
 * The bar retains the same tone-fill meaning at nominal frequency.
 */
#define DTMF_PAIR_ENERGY_MIN	0.24

/*
 * offset_relief_get() clamps delta to 0.015*f + 2 Hz, with length <= 205.
 * At 1633 Hz and 205 samples, maximum relief is 8.031 dB, bounded by
 * factor 6.357. Shorter spans and lower frequencies reduce relief.
 * The raw pair floor is 0.24/6.357 = 0.037753657...; rounding DOWN to
 * 0.0377 keeps borderline windows. Below this floor, even maximum relief
 * cannot reach DTMF_PAIR_ENERGY_MIN, so estimates cannot affect rejection.
 */
#define DTMF_RAW_PAIR_ENERGY_MIN	0.0377

/*
 * ETSI ES 201 235-3 requires 1.5 percent plus 2 Hz acceptance.
 * Q.24 national columns refuse from 3.0 to 3.5 percent, and the Australian
 * column at 7 percent. REFUSE_PCT takes the lowest, 3.0 percent.
 * The midpoint of the ETSI accept edge and that refusal, 0.0225*f + 1 Hz,
 * leaves room for estimator error.
 * The half-window bench measures 1.8 Hz error at validation without twist
 * and 3.9 Hz at the 6 dB corners.
 * A network with 1.8 percent senders needs ACCEPT_PCT = 0.018;
 * that moves the midpoint to 0.024*f + 1 Hz.
 * The Q.24 Australian +4 Hz allowance needs ACCEPT_HZ = 4.0.
 */
#define DTMF_OFFSET_ACCEPT_PCT	0.015
#define DTMF_OFFSET_ACCEPT_HZ	2.0
#define DTMF_OFFSET_REFUSE_PCT	0.030

/*
 * Fill is a loudness property. Partner leakage makes per-bin quarter powers
 * beat on close pairs at low levels. For digit *, that mask can count only
 * 102 of 205 tone samples and leave the row about 2 dB under-relieved.
 * The scan covers 2880 waveforms: all digits, nine combinations of nominal
 * and accept edges, five levels, and four twists. Levels are -3, -4, -13,
 * -23, and -28 dBm0; twists are -6, 0, 3, and +6 dB.
 * Across all sample positions through 300 ms, full tone quarters retain
 * at least 0.620734279 of the maximum quarter energy. Across onset fills
 * 1 through 205, full quarters retain at least 0.621888395; silent quarters
 * reach zero. The 0.25 threshold separates full tone quarters from silence.
 */
#define DTMF_QUARTER_ENERGY_MIN	0.25

/*
 * The row level reads true only on a window that the tone fills. A tone of T
 * samples reads (A * T / 2)^2 in the Goertzel power, so a partly filled window
 * reads 20 log10(T / 205) dB low: 1.7 dB for the 169 tone samples that a
 * 40 ms digit can validate on. A steady window has every quarter within 3 dB
 * of the loudest. Full tone quarters retain at least 0.62 of the loudest
 * (DTMF_QUARTER_ENERGY_MIN), so a full window is always steady.
 */
#define DTMF_LEVEL_QUARTER_MIN	0.5
/* The six bits of the RFC 4733 volume field hold 0 to 63. */
#define DTMF_LEVEL_MIN_DBM0	(-63)

/*
 * The winning bin of each group must beat the runner up by 8 dB. The row
 * frequencies are 73 Hz apart at their closest and the window is 205
 * samples, so the neighbour bin sees the tone 1.87 bin widths away, where
 * the rectangular window leaks about 20 dB down. Measured minimum on the
 * generator is 15.9 dB. A harmonic comb with two harmonics in one group
 * fails here.
 */
#define DTMF_GROUP_PEAK_MIN	6.3

/*
 * The 725760-cell clean sweep reaches 0.070410120 in full tone windows,
 * 0.437279246 at validation, and 0.588072875 across all measured windows.
 * Partial tone windows exceed 0.10, so the bar uses the allowed 0.12 cap.
 * The bar does not apply to a partly filled window that extends the active
 * digit (window_digit_get).
 * The test_dsp.c comb has harmonics 8.2 dB below nominal fundamentals.
 * The 0.12 bar requires 9.208188 dB attenuation, leaving 1.008188 dB margin.
 * Its measured minimum ratio, 0.136381489, leaves 0.555742 dB margin.
 */
#define DTMF_HARMONIC_MAX	0.12

/*
 * ETSI ES 201 235-3 4.2.1.3(c) requires acceptance through 6 dB of level
 * difference. It sets no rejection requirement above that difference.
 * The 725760-cell clean sweep reaches 7.883 dB in full tone windows.
 * Exact offsets still produce 7.375 dB with fresh estimates and a loaded
 * span of 205 samples, which exposes the loss model's close-pair residual.
 * The 8.2 dB bound adds 0.3 dB to the measured maximum, rounded upward.
 * This measurement-derived bound applies to the relieved reading;
 * it does not define a signal-level policy.
 * The reverse bound stays -8.0 dB, which includes ETSI -6 dB.
 */
#define DTMF_TWIST_MIN		(-8.0)
#define DTMF_TWIST_MAX		8.2

static unsigned strongest_get(const double *power, unsigned first,
			      unsigned count, double *second)
{
	unsigned strongest = first;
	unsigned i;

	*second = 0.0;
	for (i = first + 1; i < first + count; i++) {
		if (power[i] <= power[strongest]) {
			if (power[i] > *second)
				*second = power[i];
			continue;
		}
		*second = power[strongest];
		strongest = i;
	}
	return strongest;
}

static double complex phase_get(double angle)
{
	return cos(angle) + I * sin(angle);
}

static double complex_power_get(double complex value)
{
	return creal(value) * creal(value) + cimag(value) * cimag(value);
}

static double dirichlet_get(unsigned length, double delta)
{
	double angle = M_PI * delta / UDSP_SAMPLE_RATE;
	double denominator = sin(angle);

	if (fabs(denominator) < 1e-12)
		return length;
	return sin(length * angle) / denominator;
}

static double loss_get(unsigned length, double delta)
{
	double ratio = dirichlet_get(length, delta) / length;

	return ratio * ratio;
}

static unsigned window_energy_get(const int16_t *samples, double *energy,
				  unsigned *mask, bool *steady)
{
	double quarter_energy[4] = { 0.0 };
	double maximum = 0.0;
	unsigned length = 0;
	unsigned k, i;

	*energy = 0.0;
	*mask = 0;
	for (k = 0; k < 4; k++) {
		for (i = slice_start[k]; i <= slice_end[k]; i++)
			quarter_energy[k] += (double)samples[i] * samples[i];
		*energy += quarter_energy[k];
		maximum = fmax(maximum, quarter_energy[k]);
	}
	for (k = 0; k < 4; k++) {
		if (quarter_energy[k] <= 0.0 ||
		    quarter_energy[k] < DTMF_QUARTER_ENERGY_MIN * maximum)
			continue;
		*mask |= 1u << k;
		length += slice_length[k];
	}
	*steady = *mask == 0xfu;
	for (k = 0; k < 4; k++)
		*steady &= quarter_energy[k] >=
			DTMF_LEVEL_QUARTER_MIN * maximum;
	return length;
}

static int8_t row_level_get(double row_power)
{
	double amplitude = 2.0 * sqrt(row_power) / UDSP_DTMF_WINDOW_SAMPLES;
	double level;

	if (amplitude <= 0.0)
		return DTMF_LEVEL_MIN_DBM0;
	/* The inverse of dsp.h udsp_dbm0_amplitude. */
	level = round(20.0 * log10(amplitude / 32767.0) + 3.14);
	if (level > 0.0)
		return 0;
	if (level < DTMF_LEVEL_MIN_DBM0)
		return DTMF_LEVEL_MIN_DBM0;
	return (int8_t)level;
}

static void slices_get(struct udsp_dtmf_detector *detector, unsigned index,
		       double complex value[6])
{
	struct udsp_goertzel_bank *bank = &detector->sub_bank;
	unsigned i;

	for (i = 0; i < 6; i++) {
		struct udsp_goertzel_iq result;

		udsp_goertzel_reset_index(bank, index);
		udsp_goertzel_process_index(bank, index,
				detector->sample_window + slice_start[i],
				slice_length[i]);
		udsp_goertzel_result(bank, index, &result);
		value[i] = result.re + I * result.im;
	}
}

static double complex slice_kernel_get(double delta, unsigned k)
{
	return dirichlet_get(slice_length[k], delta) / slice_length[k] *
		phase_get(2.0 * M_PI * delta * slice_centroid[k] /
			  UDSP_SAMPLE_RATE);
}

static bool slice_unmix(const double complex raw[2][6],
			double complex value[2][6], unsigned k,
			double complex ab, double complex ba)
{
	double complex determinant = 1.0 - ab * ba;
	double complex a = raw[0][k];
	double complex b = raw[1][k];

	if (cabs(determinant) < 1e-6) {
		value[0][k] = a;
		value[1][k] = b;
		return false;
	}
	value[0][k] = (a - ab * b) / determinant;
	value[1][k] = (b - ba * a) / determinant;
	return true;
}

static struct offset_estimate offset_estimate_get(const double complex value[6],
						  double nominal, unsigned mask)
{
	struct offset_estimate result = { .delta = NAN };
	double omega = 2.0 * M_PI * nominal / UDSP_SAMPLE_RATE;
	unsigned loaded = 0;
	unsigned first = 4, last = 0;
	unsigned k;

	for (k = 0; k < 4; k++) {
		if (!(mask & (1u << k)))
			continue;
		loaded++;
		if (first == 4)
			first = k;
		last = k;
	}
	for (k = 0; k < 3; k++) {
		if ((mask & (3u << k)) != (3u << k))
			continue;
		result.run.quarter += value[k + 1] * conj(value[k]) *
			phase_get(-omega * (slice_end[k + 1] - slice_end[k]));
		result.run.centroid_hops_x2 +=
			2.0 * (slice_centroid[k + 1] - slice_centroid[k]);
		result.run.adjacent++;
	}
	if (!result.run.adjacent)
		return result;
	result.delta = carg(result.run.quarter) * UDSP_SAMPLE_RATE /
		(M_PI * result.run.centroid_hops_x2 / result.run.adjacent);
	if (loaded >= 3) {
		double span = slice_centroid[last] - slice_centroid[first];
		double phi = carg(value[last] * conj(value[first]) *
			phase_get(-omega * (slice_end[last] - slice_end[first])));
		double step = UDSP_SAMPLE_RATE / span;
		double fine;

		if (phi <= -M_PI)
			phi += 2.0 * M_PI;
		fine = phi * UDSP_SAMPLE_RATE / (2.0 * M_PI * span);
		result.delta = fine + round((result.delta - fine) / step) * step;
	}
	return result;
}

static void slices_unmix(const struct udsp_dtmf_detector *detector,
			 unsigned pair, unsigned mask, double complex value[2][6],
			 struct offset_estimate estimate[2])
{
	double complex raw[2][6];
	double nominal[2] = { dtmf_frequency[pair / 4],
			      dtmf_frequency[4 + pair % 4] };
	double partner[2];
	bool valid = true;
	unsigned k, side, pass;

	memcpy(raw, value, sizeof(raw));
	for (k = 0; k < 4; k++)
		valid &= slice_unmix(raw, value, k, detector->offset_kernel[pair][k],
			    conj(detector->offset_kernel[pair][k]));
	for (pass = 0; pass < 2; pass++) {
		for (side = 0; side < 2; side++) {
			estimate[side] = offset_estimate_get(value[side], nominal[side], mask);
			partner[side] = nominal[side] +
				(isfinite(estimate[side].delta) ? estimate[side].delta : 0.0);
		}
		for (k = pass ? 4 : 0; k < (pass ? 6u : 4u); k++)
			valid &= slice_unmix(raw, value, k,
				slice_kernel_get(partner[1] - nominal[0], k),
				slice_kernel_get(partner[0] - nominal[1], k));
	}
	for (side = 0; side < 2; side++) {
		double omega = 2.0 * M_PI * nominal[side] / UDSP_SAMPLE_RATE;

		if (!valid || !isfinite(estimate[side].delta))
			continue;
		estimate[side].run.half = value[side][5] * conj(value[side][4]) *
			phase_get(-omega * 103.0);
		estimate[side].run.half_magnitude = cabs(estimate[side].run.half);
		estimate[side].run.decisions = 1;
	}
}

static double offset_run_coarse_get(const struct udsp_dtmf_offset_run *run)
{
	double hop = 0.5 * run->centroid_hops_x2 / run->adjacent;

	return carg(run->quarter) * UDSP_SAMPLE_RATE / (2.0 * M_PI * hop);
}

static double offset_run_delta_get(const struct udsp_dtmf_offset_run *run)
{
	double coarse = offset_run_coarse_get(run);
	double fine = carg(run->half);
	double step = UDSP_SAMPLE_RATE / 102.5;

	if (fine <= -M_PI)
		fine += 2.0 * M_PI;
	fine *= UDSP_SAMPLE_RATE / (2.0 * M_PI * 102.5);
	return fine + round((coarse - fine) / step) * step;
}

static void offset_run_add(struct udsp_dtmf_offset_run *run,
			   const struct offset_estimate *estimate)
{
	if (!estimate->run.decisions)
		return;
	run->quarter += estimate->run.quarter;
	run->half += estimate->run.half;
	run->half_magnitude += estimate->run.half_magnitude;
	run->decisions++;
	run->adjacent += estimate->run.adjacent;
	run->centroid_hops_x2 += estimate->run.centroid_hops_x2;
}

static void offset_reset(struct udsp_dtmf_detector *detector)
{
	memset(detector->offset_run, 0, sizeof(detector->offset_run));
}

static void offset_pair_add(struct udsp_dtmf_detector *detector,
			    const struct offset_estimate estimate[2])
{
	unsigned side;

	for (side = 0; side < 2; side++)
		offset_run_add(&detector->offset_run[side], &estimate[side]);
}

static bool offset_coherence_pass(const struct udsp_dtmf_offset_run *run)
{
	return cabs(run->half) >= DTMF_COHERENCE_MIN * run->half_magnitude;
}

static bool offset_relief_get(const struct udsp_dtmf_offset_run *previous,
			      const struct offset_estimate *estimate,
			      double nominal, unsigned length,
			      double *scale, double *harmonic_scale,
			      double *quarter_sum,
			      bool validating)
{
	struct udsp_dtmf_offset_run run = *previous;
	double gate = 0.5 * ((DTMF_OFFSET_ACCEPT_PCT + DTMF_OFFSET_REFUSE_PCT) *
			     nominal + DTMF_OFFSET_ACCEPT_HZ);
	double delta, relief_delta;

	*scale = 1.0;
	*harmonic_scale = 1.0;
	offset_run_add(&run, estimate);
	if (!run.decisions)
		return true;
	delta = offset_run_delta_get(&run);
	relief_delta = fmin(fabs(delta), DTMF_OFFSET_ACCEPT_PCT * nominal +
			    DTMF_OFFSET_ACCEPT_HZ);
	*scale = 1.0 / loss_get(length, relief_delta);
	*harmonic_scale = 1.0 / loss_get(UDSP_DTMF_WINDOW_SAMPLES,
					relief_delta);
	/* The 51-sample approximation compensates the measured 0.42 dB skew. */
	*quarter_sum /= loss_get(51, relief_delta);
	/* A lone onset estimate is too noisy to refuse before validation. */
	if (!validating && (!estimate->run.decisions || run.decisions < 2))
		return true;
	/* The 725760-cell clean sweep leaves at least 1.448006 Hz of margin
	 * from valid estimate 2 before validation, so the gate needs no slack. */
	return fabs(delta) <= gate;
}

static bool dtmf_guard_pass(const double *power, unsigned row,
			    unsigned column, const double twist_power[2], double energy,
			    const double scale[2], const double harmonic_scale[2])
{
	double pair_power;
	double twist_db;
	double harmonic_power;

	pair_power = power[row] * scale[0] + power[column + 4] * scale[1];
	twist_db = 10.0 * log10(twist_power[1] / twist_power[0]);
	if (pair_power < energy * UDSP_DTMF_WINDOW_SAMPLES * DTMF_PAIR_ENERGY_MIN)
		return false;
	if (twist_db < DTMF_TWIST_MIN || twist_db > DTMF_TWIST_MAX)
		return false;
	harmonic_power = power[8 + row] * harmonic_scale[0] +
		power[12 + column] * harmonic_scale[1];
	if (harmonic_power >= pair_power * DTMF_HARMONIC_MAX)
		return false;
	return true;
}

static bool offset_guard_pass(struct udsp_dtmf_detector *detector,
			      const double power[16], unsigned row,
			      unsigned column, const double quarter_sum[2],
			      double energy, unsigned length,
			      const struct offset_estimate estimate[2],
			      bool continuing, double *row_power)
{
	const struct udsp_dtmf_offset_run empty = { 0 };
	double sum[2] = { quarter_sum[0], quarter_sum[1] };
	double scale[2], harmonic_scale[2];
	bool validating = continuing && !detector->active &&
		detector->candidate_decisions >= DTMF_HIT_DECISIONS_MIN - 1;
	bool estimates_valid = true;
	bool offset_pass = true;
	unsigned side;

	for (side = 0; side < 2; side++) {
		const struct udsp_dtmf_offset_run *run = continuing ?
			&detector->offset_run[side] : &empty;

		estimates_valid &= run->decisions || estimate[side].run.decisions;
		offset_pass &= offset_relief_get(run, &estimate[side],
			dtmf_frequency[side ? column + 4 : row], length,
			&scale[side], &harmonic_scale[side], &sum[side], validating);
	}
	if (estimates_valid) {
		sum[0] = power[row] * scale[0];
		sum[1] = power[column + 4] * scale[1];
	}
	*row_power = power[row] * scale[0];
	return dtmf_guard_pass(power, row, column, sum, energy,
			       scale, harmonic_scale) && offset_pass;
}

static double harmonic_power_get(struct udsp_dtmf_detector *detector,
				 unsigned index)
{
	struct udsp_goertzel_bank *bank = &detector->sub_bank;

	udsp_goertzel_reset_index(bank, index);
	udsp_goertzel_process_index(bank, index, detector->sample_window,
				    UDSP_DTMF_WINDOW_SAMPLES);
	return udsp_goertzel_power(bank, index);
}

static char window_digit_get(struct udsp_dtmf_detector *detector,
			     bool *silent, struct offset_estimate estimate[2],
			     struct window_level *level)
{
	const int16_t *samples = detector->sample_window;
	double power[16];
	double row_power;
	double energy = 0.0;
	double row_second;
	double column_second;
	double quarter_sum[2] = { 0.0, 0.0 };
	double complex value[2][6];
	bool continuing;
	bool pass;
	char digit;
	unsigned row;
	unsigned column;
	unsigned length;
	unsigned mask;
	unsigned i;

	length = window_energy_get(samples, &energy, &mask, &level->steady);
	*silent = energy < UDSP_DTMF_WINDOW_SAMPLES * 250.0 * 250.0;
	if (energy < UDSP_DTMF_WINDOW_SAMPLES * 500.0 * 500.0)
		return 0;
	udsp_goertzel_reset(&detector->bank);
	udsp_goertzel_process(&detector->bank, samples, UDSP_DTMF_WINDOW_SAMPLES);
	for (i = 0; i < 8; i++)
		power[i] = udsp_goertzel_power(&detector->bank, i);
	row = strongest_get(power, 0, 4, &row_second);
	column = strongest_get(power, 4, 4, &column_second) - 4;
	if (power[row] < row_second * DTMF_GROUP_PEAK_MIN ||
	    power[column + 4] < column_second * DTMF_GROUP_PEAK_MIN)
		return 0;
	if (power[row] + power[column + 4] <
	    energy * UDSP_DTMF_WINDOW_SAMPLES * DTMF_RAW_PAIR_ENERGY_MIN)
		return 0;
	slices_get(detector, row, value[0]);
	slices_get(detector, column + 4, value[1]);
	slices_unmix(detector, row * 4 + column, mask, value, estimate);
	for (i = 0; i < 4; i++) {
		quarter_sum[0] += complex_power_get(value[0][i]);
		quarter_sum[1] += complex_power_get(value[1][i]);
	}
	digit = digit_map[row][column];
	continuing = digit == (detector->candidate ?
		detector->candidate : detector->active);
	/* The harmonic bar refuses a voiced comb before a digit validates,
	 * and ends an active digit that turns into one. On a partly filled
	 * window of a close pair, the column tone leaks into the second
	 * harmonic bin of its row: 1336 Hz beside 1394 Hz, 1477 Hz beside
	 * 1540 Hz, 1633 Hz beside 1704 Hz. A partly filled window that extends
	 * the active digit therefore skips the bar. At an accept edge, the bar
	 * would end digit 2 inside a 10 ms break, and ETSI ES 201 235-3
	 * forbids an end below 20 ms. */
	if (digit == detector->active && mask != 0xfu) {
		power[8 + row] = 0.0;
		power[12 + column] = 0.0;
	} else {
		power[8 + row] = harmonic_power_get(detector, 8 + row);
		power[12 + column] = harmonic_power_get(detector, 12 + column);
	}
	pass = offset_guard_pass(detector, power, row, column, quarter_sum,
				 energy, length, estimate, continuing,
				 &row_power);
	if (!pass)
		return 0;
	level->dbm0 = row_level_get(row_power);
	return digit;
}

static bool sample_window_add(struct udsp_dtmf_detector *detector,
			      const int16_t *samples)
{
	const unsigned hop_samples = UDSP_DTMF_HOP_SAMPLES;
	const unsigned keep_samples = UDSP_DTMF_WINDOW_SAMPLES - hop_samples;
	unsigned discard;

	if (detector->sample_count + hop_samples <=
	    UDSP_DTMF_WINDOW_SAMPLES) {
		memcpy(detector->sample_window + detector->sample_count, samples,
		       hop_samples * sizeof(*samples));
		detector->sample_count += hop_samples;
		return detector->sample_count == UDSP_DTMF_WINDOW_SAMPLES;
	}
	discard = detector->sample_count - keep_samples;
	memmove(detector->sample_window, detector->sample_window + discard,
		keep_samples * sizeof(*samples));
	memcpy(detector->sample_window + keep_samples, samples,
	       hop_samples * sizeof(*samples));
	detector->sample_count = UDSP_DTMF_WINDOW_SAMPLES;
	return true;
}

static uint16_t duration_get(unsigned decisions)
{
	if (decisions > UINT16_MAX / 5U)
		return UINT16_MAX;
	return (uint16_t)(decisions * 5U);
}

/* A partly filled window can only read the level low, so of the steady windows
 * of a run the loudest is the fullest. A short digit can validate with no
 * steady window in its run. It then keeps the latest window, which reads the
 * level low by the part it lacks. */
static void candidate_level_add(struct udsp_dtmf_detector *detector,
				const struct window_level *level)
{
	if (detector->candidate_level_steady &&
	    (!level->steady ||
	     level->dbm0 <= detector->candidate_level_dbm0))
		return;
	detector->candidate_level_dbm0 = level->dbm0;
	detector->candidate_level_steady = level->steady;
}

static void candidate_add(struct udsp_dtmf_detector *detector, char digit,
			  const struct offset_estimate estimate[2],
			  const struct window_level *level)
{
	if (digit == detector->ended_digit && !detector->ended_digit_clear)
		digit = 0;
	if (digit != detector->candidate) {
		if (digit || !detector->active ||
		    detector->candidate != detector->active)
			offset_reset(detector);
		detector->candidate = digit;
		detector->candidate_decisions = digit ? 1 : 0;
		detector->candidate_level_dbm0 = 0;
		detector->candidate_level_steady = false;
		if (!digit)
			return;
		offset_pair_add(detector, estimate);
		candidate_level_add(detector, level);
		return;
	}
	if (!digit)
		return;
	detector->candidate_decisions++;
	offset_pair_add(detector, estimate);
	candidate_level_add(detector, level);
}

static bool digit_end_emit(struct udsp_dtmf_detector *detector,
			   struct udsp_event *event, unsigned extra_decisions)
{
	memset(event, 0, sizeof(*event));
	event->type = UDSP_EV_DIGIT;
	event->u.digit.digit = detector->active;
	event->u.digit.remote = detector->remote;
	event->u.digit.duration_ms = duration_get(detector->active_decisions +
						    extra_decisions);
	detector->ended_digit = detector->active;
	detector->ended_digit_clear = false;
	detector->active = 0;
	detector->active_level_dbm0 = 0;
	detector->active_decisions = 0;
	detector->missed_decisions = 0;
	detector->missed_silent = 0;
	offset_reset(detector);
	return true;
}

void udsp_dtmf_detector_init(struct udsp_dtmf_detector *detector, bool remote)
{
	unsigned pair, k;

	memset(detector, 0, sizeof(*detector));
	detector->remote = remote;
	udsp_goertzel_init(&detector->bank, dtmf_frequency, 8);
	udsp_goertzel_init(&detector->sub_bank, dtmf_frequency, 16);
	for (pair = 0; pair < 16; pair++) {
		double delta = dtmf_frequency[4 + pair % 4] -
			dtmf_frequency[pair / 4];

		/* The Dirichlet ratio is even, so the reverse kernel is conjugate. */
		for (k = 0; k < 4; k++)
			detector->offset_kernel[pair][k] = slice_kernel_get(delta, k);
	}
}

static bool decision_step(struct udsp_dtmf_detector *detector,
			  const int16_t *samples, struct udsp_event *event)
{
	struct offset_estimate estimate[2] = { 0 };
	struct window_level level = { 0 };
	bool silent;
	char digit;

	if (!sample_window_add(detector, samples))
		return false;
	digit = window_digit_get(detector, &silent, estimate, &level);
	if (!digit) {
		if (detector->no_vsc_run < DTMF_PAUSE_DECISIONS_MIN)
			detector->no_vsc_run++;
		if (detector->no_vsc_run == DTMF_PAUSE_DECISIONS_MIN)
			detector->ended_digit_clear = true;
	} else {
		detector->no_vsc_run = 0;
		if (digit != detector->ended_digit)
			detector->ended_digit_clear = true;
	}

	if (digit && digit == detector->active) {
		if (detector->candidate && digit != detector->candidate)
			offset_reset(detector);
		offset_pair_add(detector, estimate);
		detector->active_decisions++;
		detector->missed_decisions = 0;
		detector->missed_silent = 0;
		detector->candidate = 0;
		detector->candidate_decisions = 0;
		return false;
	}
	if (detector->active) {
		detector->missed_decisions++;
		detector->missed_silent = silent ?
			detector->missed_silent + 1u : 0u;
		candidate_add(detector, digit, estimate, &level);
		if (detector->missed_decisions <= DTMF_MISS_DECISIONS_MAX &&
		    detector->missed_silent < DTMF_MISS_DECISIONS_MAX) {
			detector->active_decisions++;
			return false;
		}
		return digit_end_emit(detector, event, 0);
	}
	candidate_add(detector, digit, estimate, &level);
	if (detector->candidate_decisions < DTMF_HIT_DECISIONS_MIN ||
	    !detector->offset_run[0].decisions ||
	    !detector->offset_run[1].decisions ||
	    !offset_coherence_pass(&detector->offset_run[0]) ||
	    !offset_coherence_pass(&detector->offset_run[1]))
		return false;
	/* The digit takes the whole run with it, so its duration still counts
	 * from the first decision that saw it, not from this one. */
	detector->active = digit;
	detector->active_level_dbm0 = detector->candidate_level_dbm0;
	detector->active_decisions = detector->candidate_decisions;
	return false;
}

bool udsp_dtmf_detector_process(struct udsp_dtmf_detector *detector,
				const int16_t *samples,
				struct udsp_event *event)
{
	bool emitted = false;
	unsigned i;

	/* Both decisions run; validation only sets active, so a call emits at
	 * most one event. */
	for (i = 0; i < UDSP_FRAME_SAMPLES; i += UDSP_DTMF_HOP_SAMPLES)
		emitted |= decision_step(detector, samples + i, event);
	return emitted;
}

bool udsp_dtmf_detector_flush(struct udsp_dtmf_detector *detector,
			      struct udsp_event *event)
{
	unsigned delay_decisions = (UDSP_DTMF_WINDOW_SAMPLES - 1) /
		UDSP_DTMF_HOP_SAMPLES;
	bool emitted = false;

	if (detector->active)
		emitted = digit_end_emit(detector, event, delay_decisions);
	detector->ended_digit = 0;
	detector->ended_digit_clear = false;
	detector->no_vsc_run = 0;
	offset_reset(detector);
	return emitted;
}
