// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../dsp/resamp.h"
#include "util.h"

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

static const int16_t frozen_input[64] = {
	-17655, -9635, -1413, 7011, 15637, 24465, -26506, -17274,
	-7840, 1796, 11634, 21674, -28085, -17641, -6995, 3853,
	14903, 26155, -22392, -10736, 1122, 13182, 25444, -22093,
	-9427, 3441, 16511, 29783, -16744, -3068, 10810, 24890,
	-20829, -6345, 8341, 23229, -21682, -6390, 9104, 24800,
	-19303, -3203, 13099, 29603, -13692, 3216, 20326, -22363,
	-4849, 12867, -29216, -11096, 7226, 25750, -15525, 3403,
	22533, -18136, 1398, 21134, -18929, 1211, 21553, -17904
};

static const int16_t frozen_down[32] = {
	8, -20, 17, -14, 9, -14, 20, -31,
	40, -96, 321, -10922, -4529, 19829, -11170, -12877,
	16404, -12595, -12804, 21146, -9436, 205, 14450, -18075,
	25557, -6983, 13965, -7541, 8883, -6140, 7392, -1147
};

static const int16_t frozen_up[128] = {
	29, 0, -37, 0, 73, 0, -129, 0,
	177, 0, -306, 0, 495, 0, -730, 0,
	1137, 0, -1809, 0, 3251, 0, -10387, -17654,
	-15460, -9635, -5850, -1413, 4651, 7011, 7382, 15637,
	28147, 24464, -1584, -26505, -28876, -17273, -10123, -7840,
	-2917, 1796, 3926, 11634, 23959, 21673, -3147, -28084,
	-30372, -17640, -9334, -6995, -2181, 3853, 7546, 14903,
	26498, 26154, 3790, -22391, -26733, -10736, 2025, 1122,
	661, 13182, 28272, 25443, 1786, -22092, -25124, -9427,
	3935, 3441, 2496, 16510, 32767, 29782, 3248, -16743,
	-13952, -3068, 2368, 10810, 24991, 24889, 1416, -20828,
	-19485, -6345, 773, 8341, 22027, 23228, 827, -21681,
	-20450, -6390, 1648, 9104, 22625, 24799, 3521, -19302,
	-18707, -3203, 6703, 13099, 24932, 29602, 11772, -13692,
	-17533, 3216, 22885, 20325, -1241, -22362, -24371, -4849,
	15748, 12867, -11260, -29215, -25043, -11096, -1878, 7226
};

static bool samples_equal(const int16_t *a, const int16_t *b, unsigned count)
{
	return memcmp(a, b, count * sizeof(*a)) == 0;
}

static void tone_fill(int16_t *samples, unsigned count, double frequency,
		      double rate, double amplitude)
{
	unsigned i;

	for (i = 0; i < count; i++)
		samples[i] = (int16_t)lrint(amplitude *
					     sin(2.0 * M_PI * frequency * i / rate));
}

static double snr_measure(const int16_t *samples, unsigned start, unsigned count,
			  double frequency, double rate, double amplitude,
			  double delay)
{
	double signal_energy = 0.0;
	double error_energy = 0.0;
	unsigned i;

	for (i = start; i < count; i++) {
		double ideal = amplitude * sin(2.0 * M_PI * frequency *
					       (i - delay) / rate);
		double error = samples[i] - ideal;

		signal_energy += ideal * ideal;
		error_energy += error * error;
	}
	return 10.0 * log10(signal_energy / error_energy);
}

static double rms_measure(const int16_t *samples, unsigned start, unsigned count)
{
	double energy = 0.0;
	unsigned i;

	for (i = start; i < count; i++)
		energy += (double)samples[i] * samples[i];
	return sqrt(energy / (count - start));
}

static double tone_component_measure(const int16_t *samples, unsigned start,
				     unsigned count, double frequency,
				     double rate)
{
	double real = 0.0;
	double imag = 0.0;
	unsigned i;

	for (i = 0; i < count; i++) {
		double angle = 2.0 * M_PI * frequency * i / rate;

		real += samples[start + i] * cos(angle);
		imag -= samples[start + i] * sin(angle);
	}
	return 2.0 * hypot(real, imag) / count;
}

static void test_table(void)
{
	unsigned i;

	CASE("coefficient table");
	CHECK_UINT(UDSP_RESAMP_TAPS, 47);
	CHECK_UINT(udsp_resamp_coefficient_get(23), 32767);
	for (i = 0; i < UDSP_RESAMP_TAPS; i++)
		CHECK(udsp_resamp_coefficient_get(i) ==
		      udsp_resamp_coefficient_get(UDSP_RESAMP_TAPS - 1U - i));
	for (i = 1; i < UDSP_RESAMP_TAPS; i += 2U) {
		if (i != UDSP_RESAMP_TAPS / 2U)
			CHECK(udsp_resamp_coefficient_get(i) == 0);
	}
}

static void test_response(void)
{
	double nominal = 0.0;
	unsigned frequency;
	unsigned tap;

	CASE("frequency response");
	for (tap = 0; tap < UDSP_RESAMP_TAPS; tap++)
		nominal += udsp_resamp_coefficient_get(tap);
	for (frequency = 100; frequency <= 8000; frequency += 100) {
		double real = 0.0;
		double imag = 0.0;
		double db;

		for (tap = 0; tap < UDSP_RESAMP_TAPS; tap++) {
			double angle = 2.0 * M_PI * frequency * tap / 16000.0;

			real += udsp_resamp_coefficient_get(tap) * cos(angle);
			imag -= udsp_resamp_coefficient_get(tap) * sin(angle);
		}
		db = 20.0 * log10(hypot(real, imag) / fabs(nominal));
		if (frequency <= 3400)
			CHECK(fabs(db) <= 0.1);
		if (frequency >= 4600)
			CHECK(db <= -58.0);
	}
}

static unsigned magnitude_peak_get(const int16_t *samples, unsigned count)
{
	int peak = -1;
	unsigned peak_index = 0;
	unsigned i;

	for (i = 0; i < count; i++) {
		int magnitude = samples[i] < 0 ? -samples[i] : samples[i];

		if (magnitude <= peak)
			continue;
		peak = magnitude;
		peak_index = i;
	}
	return peak_index;
}

static void test_impulse(void)
{
	struct udsp_resamp rs;
	int16_t down_input[96] = { 16384 };
	int16_t down_output[48];
	int16_t up_input[64] = { 16384 };
	int16_t up_output[128];
	unsigned peak;

	CASE("down2 impulse delay");
	udsp_resamp_reset(&rs);
	udsp_resamp_down2(&rs, down_input, ARRAY_SIZE(down_input), down_output);
	peak = magnitude_peak_get(down_output, ARRAY_SIZE(down_output));
	CHECK_UINT(2U * peak + 1U, 23);

	CASE("up2 impulse delay");
	udsp_resamp_reset(&rs);
	udsp_resamp_up2(&rs, up_input, ARRAY_SIZE(up_input), up_output);
	peak = magnitude_peak_get(up_output, ARRAY_SIZE(up_output));
	CHECK_UINT(peak, 23);
}

static void test_down_tones(void)
{
	const double amplitude = 32767.0 * 0.7079457843841379;
	struct udsp_resamp rs;
	int16_t input[320];
	int16_t output[160];
	double attenuation;
	double snr;

	CASE("down2 1 kHz SNR");
	tone_fill(input, ARRAY_SIZE(input), 1000.0, 16000.0, amplitude);
	udsp_resamp_reset(&rs);
	udsp_resamp_down2(&rs, input, ARRAY_SIZE(input), output);
	snr = snr_measure(output, 32, ARRAY_SIZE(output), 1000.0, 8000.0,
			  amplitude, 11.0);
	CHECK(snr > 50.0);

	CASE("down2 6 kHz attenuation");
	tone_fill(input, ARRAY_SIZE(input), 6000.0, 16000.0, amplitude);
	udsp_resamp_reset(&rs);
	udsp_resamp_down2(&rs, input, ARRAY_SIZE(input), output);
	attenuation = 20.0 * log10((amplitude / sqrt(2.0)) /
				   rms_measure(output, 32, ARRAY_SIZE(output)));
	CHECK(attenuation > 55.0);
}

static void test_real_path_response(void)
{
	const double amplitude = 32767.0 * 0.5;
	struct udsp_resamp rs;
	int16_t down_input[3200];
	int16_t down_output[1600];
	int16_t up_input[512];
	int16_t up_output[1024];
	double desired;
	double image;
	double level_db;
	double output_rms;

	CASE("down2 3.3 kHz level");
	tone_fill(down_input, ARRAY_SIZE(down_input), 3300.0, 16000.0, amplitude);
	udsp_resamp_reset(&rs);
	udsp_resamp_down2(&rs, down_input, ARRAY_SIZE(down_input), down_output);
	desired = tone_component_measure(down_output, 80, 1520, 3300.0, 8000.0);
	level_db = 20.0 * log10(desired / amplitude);
	CHECK(fabs(level_db) <= 0.1);

	CASE("up2 3 kHz image rejection");
	tone_fill(up_input, ARRAY_SIZE(up_input), 3000.0, 8000.0, amplitude);
	udsp_resamp_reset(&rs);
	udsp_resamp_up2(&rs, up_input, ARRAY_SIZE(up_input), up_output);
	desired = tone_component_measure(up_output, 128, 896, 3000.0, 16000.0);
	image = tone_component_measure(up_output, 128, 896, 5000.0, 16000.0);
	CHECK(20.0 * log10(image / desired) < -55.0);

	CASE("down2 7666 Hz rejection");
	tone_fill(down_input, ARRAY_SIZE(down_input), 7666.0, 16000.0, amplitude);
	udsp_resamp_reset(&rs);
	udsp_resamp_down2(&rs, down_input, ARRAY_SIZE(down_input), down_output);
	output_rms = rms_measure(down_output, 80, ARRAY_SIZE(down_output));
	CHECK(20.0 * log10(output_rms / (amplitude / sqrt(2.0))) < -58.0);
}

static void test_round_trip(void)
{
	const double amplitude = 32767.0 * 0.7079457843841379;
	struct udsp_resamp up_rs;
	struct udsp_resamp down_rs;
	int16_t input[160];
	int16_t wide[320];
	int16_t output[160];
	double input_rms;
	double level_db;
	double output_rms;
	double snr;

	CASE("up2 down2 1 kHz round trip");
	tone_fill(input, ARRAY_SIZE(input), 1000.0, 8000.0, amplitude);
	udsp_resamp_reset(&up_rs);
	udsp_resamp_reset(&down_rs);
	udsp_resamp_up2(&up_rs, input, ARRAY_SIZE(input), wide);
	udsp_resamp_down2(&down_rs, wide, ARRAY_SIZE(wide), output);
	snr = snr_measure(output, 48, ARRAY_SIZE(output), 1000.0, 8000.0,
			  amplitude, 22.5);
	input_rms = amplitude / sqrt(2.0);
	output_rms = rms_measure(output, 48, ARRAY_SIZE(output));
	level_db = 20.0 * log10(output_rms / input_rms);
	CHECK(snr > 45.0);
	CHECK(fabs(level_db) < 0.2);
}

static void test_saturation(void)
{
	struct udsp_resamp rs;
	int16_t down_input[96];
	int16_t down_output[48];
	int16_t up_input[64];
	int16_t up_output[128];
	unsigned i;

	CASE("positive DC saturation");
	for (i = 0; i < ARRAY_SIZE(down_input); i++)
		down_input[i] = INT16_MAX;
	for (i = 0; i < ARRAY_SIZE(up_input); i++)
		up_input[i] = INT16_MAX;
	udsp_resamp_reset(&rs);
	udsp_resamp_down2(&rs, down_input, ARRAY_SIZE(down_input), down_output);
	CHECK_UINT(down_output[12], INT16_MAX);
	for (i = 0; i < ARRAY_SIZE(down_output); i++)
		CHECK(down_output[i] > -5000);
	udsp_resamp_reset(&rs);
	udsp_resamp_up2(&rs, up_input, ARRAY_SIZE(up_input), up_output);
	CHECK_UINT(up_output[24], INT16_MAX);
	for (i = 0; i < ARRAY_SIZE(up_output); i++)
		CHECK(up_output[i] > -5000);

	CASE("negative DC saturation");
	for (i = 0; i < ARRAY_SIZE(down_input); i++)
		down_input[i] = INT16_MIN;
	for (i = 0; i < ARRAY_SIZE(up_input); i++)
		up_input[i] = INT16_MIN;
	udsp_resamp_reset(&rs);
	udsp_resamp_down2(&rs, down_input, ARRAY_SIZE(down_input), down_output);
	CHECK(down_output[12] == INT16_MIN);
	udsp_resamp_reset(&rs);
	udsp_resamp_up2(&rs, up_input, ARRAY_SIZE(up_input), up_output);
	CHECK(up_output[24] == INT16_MIN);
}

static void overflow_pattern_fill(int16_t *samples, unsigned count,
				  bool positive)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		unsigned delay = count - 1U - i;
		unsigned pair_delay = delay < count / 2U ? delay : count - 1U - delay;
		bool coefficient_positive =
			udsp_resamp_coefficient_get(2U * pair_delay) > 0;

		if (coefficient_positive == positive)
			samples[i] = INT16_MAX;
		else
			samples[i] = INT16_MIN;
	}
}

static void test_overflow_guards(void)
{
	struct udsp_resamp rs;
	int16_t positive_input[24];
	int16_t negative_input[24];
	int16_t down_output[12];
	int16_t up_output[48];

	CASE("full scale accumulator guards");
	overflow_pattern_fill(positive_input, ARRAY_SIZE(positive_input), true);
	overflow_pattern_fill(negative_input, ARRAY_SIZE(negative_input), false);
	udsp_resamp_reset(&rs);
	udsp_resamp_up2(&rs, positive_input, ARRAY_SIZE(positive_input), up_output);
	CHECK(up_output[46] == INT16_MAX);
	udsp_resamp_reset(&rs);
	udsp_resamp_up2(&rs, negative_input, ARRAY_SIZE(negative_input), up_output);
	CHECK(up_output[46] == INT16_MIN);
	udsp_resamp_reset(&rs);
	udsp_resamp_down2(&rs, positive_input, ARRAY_SIZE(positive_input), down_output);
	udsp_resamp_reset(&rs);
	udsp_resamp_down2(&rs, negative_input, ARRAY_SIZE(negative_input), down_output);
}

static void test_contract_edges(void)
{
	struct udsp_resamp whole_rs;
	struct udsp_resamp odd_rs;
	int16_t odd_input[5] = { 1000, -2000, 3000, -4000, 5000 };
	int16_t whole_output[2];
	int16_t odd_output[3] = { 0, 0, 12345 };
	int16_t clean_output[2];
	int16_t recovered_output[2];

	CASE("down2 trailing odd sample ignored");
	udsp_resamp_reset(&whole_rs);
	udsp_resamp_reset(&odd_rs);
	udsp_resamp_down2(&whole_rs, odd_input, 4, whole_output);
	udsp_resamp_down2(&odd_rs, odd_input, ARRAY_SIZE(odd_input), odd_output);
	CHECK(samples_equal(whole_output, odd_output, ARRAY_SIZE(whole_output)));
	CHECK(odd_output[2] == 12345);

	CASE("ring index recovery");
	udsp_resamp_reset(&whole_rs);
	udsp_resamp_up2(&whole_rs, odd_input, 1, clean_output);
	udsp_resamp_reset(&odd_rs);
	odd_rs.index = UINT_MAX;
	udsp_resamp_up2(&odd_rs, odd_input, 1, recovered_output);
	CHECK(samples_equal(clean_output, recovered_output, ARRAY_SIZE(clean_output)));
}

static void test_rounding_mode(void)
{
	struct udsp_resamp rs;
	int16_t positive_input[12];
	int16_t negative_input[12];
	int16_t output[24];
	unsigned i;

	CASE("round half away from zero");
	for (i = 0; i < ARRAY_SIZE(positive_input); i++) {
		positive_input[i] = 1000;
		negative_input[i] = -1000;
	}
	udsp_resamp_reset(&rs);
	udsp_resamp_up2(&rs, positive_input, ARRAY_SIZE(positive_input), output);
	CHECK(output[23] == 1000);
	udsp_resamp_reset(&rs);
	udsp_resamp_up2(&rs, negative_input, ARRAY_SIZE(negative_input), output);
	CHECK(output[23] == -1000);
}

static int16_t stream_sample_get(unsigned index)
{
	uint32_t value = index * 40507U + index * index * 7919U + 12345U;

	return (int16_t)(value & 0xffffU);
}

static void test_streaming(void)
{
	static const unsigned down_chunks[] = { 2, 3, 5, 7, 11, 13, 17, 19, 23, 31 };
	static const unsigned up_chunks[] = { 3, 5, 7, 11, 13, 17, 19, 23, 29 };
	struct udsp_resamp whole_rs;
	struct udsp_resamp split_rs;
	int16_t down_input[262];
	int16_t down_whole[131];
	int16_t down_split[131];
	int16_t up_input[127];
	int16_t up_whole[254];
	int16_t up_split[254];
	unsigned input_offset;
	unsigned output_offset;
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(down_input); i++)
		down_input[i] = stream_sample_get(i);
	for (i = 0; i < ARRAY_SIZE(up_input); i++)
		up_input[i] = stream_sample_get(i + 307U);

	CASE("down2 streaming prime chunks");
	udsp_resamp_reset(&whole_rs);
	udsp_resamp_reset(&split_rs);
	udsp_resamp_down2(&whole_rs, down_input, ARRAY_SIZE(down_input), down_whole);
	input_offset = 0;
	output_offset = 0;
	for (i = 0; i < ARRAY_SIZE(down_chunks); i++) {
		unsigned output_count = down_chunks[i];
		unsigned input_count = 2U * output_count;

		udsp_resamp_down2(&split_rs, down_input + input_offset, input_count,
				   down_split + output_offset);
		input_offset += input_count;
		output_offset += output_count;
	}
	CHECK_UINT(input_offset, ARRAY_SIZE(down_input));
	CHECK(samples_equal(down_whole, down_split, ARRAY_SIZE(down_whole)));

	CASE("up2 streaming prime chunks");
	udsp_resamp_reset(&whole_rs);
	udsp_resamp_reset(&split_rs);
	udsp_resamp_up2(&whole_rs, up_input, ARRAY_SIZE(up_input), up_whole);
	input_offset = 0;
	output_offset = 0;
	for (i = 0; i < ARRAY_SIZE(up_chunks); i++) {
		unsigned input_count = up_chunks[i];

		udsp_resamp_up2(&split_rs, up_input + input_offset, input_count,
				 up_split + output_offset);
		input_offset += input_count;
		output_offset += 2U * input_count;
	}
	CHECK_UINT(input_offset, ARRAY_SIZE(up_input));
	CHECK(samples_equal(up_whole, up_split, ARRAY_SIZE(up_whole)));
}

/*
 * The direct form of the two filters over a plain input array, the definition
 * in the resamp.c header restated without a window: every product and every
 * sum in the order the implementation takes them, so the comparison is exact
 * and not a bound. A stream of 2048 pseudo-random samples walks the window
 * through every position many times over, which the 64-sample frozen vectors
 * do once.
 */
static int16_t reference_round(int32_t acc_a, int32_t acc_b)
{
	const int32_t scale = 1 << 15;
	const int32_t half = scale >> 1;
	int32_t acc;

	if (acc_b > 0 && acc_a > INT32_MAX - acc_b)
		return INT16_MAX;
	if (acc_b < 0 && acc_a < INT32_MIN - acc_b)
		return INT16_MIN;
	acc = acc_a + acc_b;
	if (acc >= INT16_MAX * scale + half)
		return INT16_MAX;
	if (acc <= INT16_MIN * scale - half)
		return INT16_MIN;
	if (acc >= 0)
		return (int16_t)((acc + half) >> 15);
	return (int16_t)-((-acc + half) >> 15);
}

static int16_t reference_delayed(const int16_t *in, unsigned newest,
				 unsigned delay)
{
	return delay > newest ? 0 : in[newest - delay];
}

static int16_t reference_down(const int16_t *in, unsigned newest)
{
	int32_t acc = (int32_t)reference_delayed(in, newest, 23) * (1 << 14);
	unsigned tap;

	for (tap = 0; tap < UDSP_RESAMP_TAPS / 2U; tap += 2U)
		acc += ((int32_t)reference_delayed(in, newest, tap) +
			reference_delayed(in, newest, UDSP_RESAMP_TAPS - 1U - tap)) *
			(udsp_resamp_coefficient_get(tap) >> 1);
	return reference_round(acc, 0);
}

static int16_t reference_up_even(const int16_t *in, unsigned newest)
{
	int32_t positive = 0;
	int32_t negative = 0;
	unsigned tap;

	for (tap = 0; tap < UDSP_RESAMP_TAPS / 2U; tap += 2U) {
		int16_t coefficient = udsp_resamp_coefficient_get(tap);
		int32_t term = ((int32_t)reference_delayed(in, newest, tap / 2U) +
				reference_delayed(in, newest,
						  (UDSP_RESAMP_TAPS - 1U - tap) / 2U)) *
			coefficient;

		if (coefficient > 0)
			positive += term;
		else
			negative += term;
	}
	return reference_round(positive, negative);
}

static int16_t reference_up_odd(const int16_t *in, unsigned newest)
{
	return reference_round((int32_t)reference_delayed(in, newest, 11) *
			       udsp_resamp_coefficient_get(23), 0);
}

static void test_reference(void)
{
	static int16_t input[2048];
	static int16_t down[1024];
	static int16_t up[4096];
	struct udsp_resamp rs;
	unsigned mismatches = 0;
	unsigned i;

	for (i = 0; i < ARRAY_SIZE(input); i++)
		input[i] = stream_sample_get(i + 991U);

	CASE("down2 matches the direct form over 2048 samples");
	udsp_resamp_reset(&rs);
	udsp_resamp_down2(&rs, input, ARRAY_SIZE(input), down);
	for (i = 0; i < ARRAY_SIZE(down); i++) {
		if (down[i] != reference_down(input, 2U * i + 1U))
			mismatches++;
	}
	CHECK_UINT(mismatches, 0);

	CASE("up2 matches the direct form over 2048 samples");
	mismatches = 0;
	udsp_resamp_reset(&rs);
	udsp_resamp_up2(&rs, input, ARRAY_SIZE(input), up);
	for (i = 0; i < ARRAY_SIZE(input); i++) {
		if (up[2U * i] != reference_up_even(input, i))
			mismatches++;
		if (up[2U * i + 1U] != reference_up_odd(input, i))
			mismatches++;
	}
	CHECK_UINT(mismatches, 0);
}

static void test_vectors(void)
{
	struct udsp_resamp rs;
	int16_t down[ARRAY_SIZE(frozen_down)];
	int16_t up[ARRAY_SIZE(frozen_up)];

	CASE("down2 frozen vector");
	udsp_resamp_reset(&rs);
	udsp_resamp_down2(&rs, frozen_input, ARRAY_SIZE(frozen_input), down);
	CHECK(samples_equal(down, frozen_down, ARRAY_SIZE(down)));

	CASE("up2 frozen vector");
	udsp_resamp_reset(&rs);
	udsp_resamp_up2(&rs, frozen_input, ARRAY_SIZE(frozen_input), up);
	CHECK(samples_equal(up, frozen_up, ARRAY_SIZE(up)));
}

int main(void)
{
	test_table();
	test_response();
	test_impulse();
	test_down_tones();
	test_real_path_response();
	test_round_trip();
	test_saturation();
	test_overflow_guards();
	test_contract_edges();
	test_rounding_mode();
	test_streaming();
	test_vectors();
	test_reference();
	return tu_summary("resamp");
}
