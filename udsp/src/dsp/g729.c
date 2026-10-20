// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The G.729 codec: Annex A speech frames, and the Annex B voice activity
 * detector, DTX, SID frames and comfort noise. Clauses 2.4, A.2.1 and B.5 of
 * the Recommendation give its ANSI C code precedence over the prose. The
 * comments call that code "the normative C code" and name it only where no
 * clause states the rule.
 */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"
#include "g729.h"

#define G729_RATE 8000U
#define G729_FRAME 80U
#define G729_SUBFRAME 40U
#define G729_BYTES 10U
#define G729_SID_BYTES 2U
#define G729_PITCH_MIN 20
#define G729_PITCH_MAX 143
#define G729_INTERPOLATION 11
#define G729_EXCITATION_HISTORY (G729_PITCH_MAX + G729_INTERPOLATION)
#define G729_SPEECH_HISTORY 160
#define G729_DTX_ACF_FRAMES 2
#define G729_DTX_SUM_FRAMES 3
#define G729_DTX_GAIN_FRAMES 2
#define G729_DTX_SID_MINIMUM 3
#define G729_DTX_SEED_INITIAL 11111
#define G729_DTX_CHANGE_FRACTION 4855
#define G729_DTX_AVERAGE_FRACTION 3161

struct g729_parameters {
	uint16_t lsp_first;
	uint16_t lsp_second;
	uint16_t pitch[2];
	uint16_t fixed[2];
	uint16_t signs[2];
	uint16_t gain[2];
	uint16_t parity;
};

/* Annex B clause B.4.4 defines the decoder comfort noise this state serves.
 * The received SID parameters live here as the decoded LSP vector and the
 * gain; the target gain carries the equation (B.19) smoothing. The energy
 * pair snapshots the excitation of the last good frame for the clause
 * B.4.5 erasure recovery. The generator seed is apart from the clause 4.4.4
 * erasure seed, and every active frame resets it (clause B.4.5).
 * past_speech holds the Ftyp(t-1) = 1 test of equation (B.27), and sid_seen
 * gates the codec.h len-0 contract: no comfort noise before the first SID
 * since reset. The comfort-noise excitation writes into the shared
 * excitation history, because the clause B.4.4 adaptive part reads it. */
struct g729_cng {
	int16_t lsp[G729_LPC_ORDER];
	int16_t sid_gain;
	int16_t gain_current;
	int16_t energy;
	int16_t energy_shift;
	int16_t seed;
	bool past_speech;
	bool sid_seen;
};

struct g729_decoder {
	int16_t excitation[G729_EXCITATION_HISTORY + G729_FRAME];
	int16_t synthesis_memory[G729_LPC_ORDER];
	int16_t lsp_previous[G729_LPC_ORDER];
	int16_t lsf_previous[G729_LPC_ORDER];
	int16_t lsf_history[4][G729_LPC_ORDER];
	int16_t gain_energy[4];
	int16_t previous_ma;
	int16_t pitch_previous;
	int16_t gain_pitch;
	int16_t gain_code;
	int16_t sharp;
	int16_t post_speech_history[G729_LPC_ORDER];
	int16_t post_residual[G729_PITCH_MAX + G729_SUBFRAME];
	int16_t post_residual_scaled[G729_PITCH_MAX + G729_SUBFRAME];
	int16_t post_synthesis_memory[G729_LPC_ORDER];
	int16_t post_preemphasis_memory;
	int16_t post_gain;
	int16_t high_pass_x[2];
	int16_t high_pass_y_high[2];
	int16_t high_pass_y_low[2];
	uint16_t random_seed;
	struct g729_cng cng;
};

/* Annex B clause B.3 defines the voice activity detection this state
 * serves; the clause B.4 discontinuous transmission consumes the decision
 * on an instance created with dtx. */
struct g729_vad {
	int16_t noise_lsf[G729_LPC_ORDER];
	int16_t minimum_buffer[16];
	int16_t minimum_previous;
	int16_t minimum_next;
	int16_t minimum;
	int16_t energy_average;
	int16_t noise_energy;
	int16_t noise_energy_low;
	int16_t noise_zero_cross;
	int16_t energy_previous;
	int16_t silence_count;
	int16_t update_count;
	int16_t extend_count;
	int16_t quiet_count;
	int16_t extend_flag;
	int16_t hangover_flag;
	int16_t frame_count;
	bool decision_previous;
	bool decision_earlier;
	bool decision;
};

/* Annex B clause B.4 defines the discontinuous transmission this state
 * serves. The autocorrelation history holds the raw coefficients of clause
 * B.4.1.1, newest row first; a shift entry carries the exponent of its row.
 * The reference correlation pair describes the last SID filter for the
 * clause B.4.1.3 Itakura comparison. The comfort-noise seed is the
 * encoder-local mirror of the decoder's clause B.4.4 generator, distinct
 * from the decoder's erasure seed. */
struct g729_dtx {
	int16_t acf[G729_DTX_ACF_FRAMES][G729_LPC_ORDER + 1];
	int16_t acf_shift[G729_DTX_ACF_FRAMES];
	int16_t acf_sum[G729_DTX_SUM_FRAMES][G729_LPC_ORDER + 1];
	int16_t acf_sum_shift[G729_DTX_SUM_FRAMES];
	int16_t energy[G729_DTX_GAIN_FRAMES];
	int16_t energy_shift[G729_DTX_GAIN_FRAMES];
	int16_t sid_lsp[G729_LPC_ORDER];
	int16_t sid_correlation[G729_LPC_ORDER + 1];
	int16_t sid_correlation_shift;
	int16_t sid_gain;
	int16_t gain_current;
	int16_t energy_previous;
	int16_t energy_count;
	int16_t frame_phase;
	int16_t since_sid;
	int16_t seed;
	bool change_pending;
	bool enabled;
};

struct g729_encoder {
	int16_t speech[G729_SPEECH_HISTORY + G729_FRAME];
	int16_t weighted[G729_PITCH_MAX + G729_FRAME];
	int16_t excitation[G729_EXCITATION_HISTORY + G729_FRAME];
	int16_t lsp_previous[G729_LPC_ORDER];
	int16_t lsp_quantized_previous[G729_LPC_ORDER];
	int16_t lsf_history[4][G729_LPC_ORDER];
	int16_t gain_energy[4];
	int16_t lpc_previous[G729_LPC_ORDER + 1];
	int16_t reflection_previous[2];
	int16_t weighting_memory[G729_LPC_ORDER];
	int16_t target_memory[G729_LPC_ORDER];
	int16_t preprocess_x[2];
	int16_t preprocess_y_high[2];
	int16_t preprocess_y_low[2];
	int32_t excitation_error[4];
	int16_t sharp;
	struct g729_vad vad;
	struct g729_dtx dtx;
};

struct g729_state {
	struct g729_encoder encoder;
	struct g729_decoder decoder;
};

static void lsf_spacing_apply(int16_t *lsf, int16_t gap);
static void lsf_stability_apply(int16_t *lsf);
static void lsf_to_lsp(const int16_t *lsf, int16_t *lsp);

static int16_t value_saturate16(int64_t value)
{
	if (value > INT16_MAX)
		return INT16_MAX;
	if (value < INT16_MIN)
		return INT16_MIN;
	return (int16_t)value;
}

static int32_t value_saturate32(int64_t value)
{
	if (value > INT32_MAX)
		return INT32_MAX;
	if (value < INT32_MIN)
		return INT32_MIN;
	return (int32_t)value;
}

static int16_t word_low_get(int32_t value)
{
	uint16_t low = (uint16_t)(uint32_t)value;

	if (low <= INT16_MAX)
		return (int16_t)low;
	return (int16_t)((int32_t)low - 65536);
}

static int16_t word_high_get(int32_t value)
{
	return (int16_t)(value >> 16);
}

static int16_t word_add(int16_t first, int16_t second)
{
	return value_saturate16((int32_t)first + second);
}

static int16_t word_subtract(int16_t first, int16_t second)
{
	return value_saturate16((int32_t)first - second);
}

static int16_t word_add_track(int16_t first, int16_t second, bool *overflow)
{
	int32_t value = (int32_t)first + second;

	if (value > INT16_MAX || value < INT16_MIN)
		*overflow = true;
	return value_saturate16(value);
}

static int16_t word_subtract_track(int16_t first, int16_t second,
				   bool *overflow)
{
	int32_t value = (int32_t)first - second;

	if (value > INT16_MAX || value < INT16_MIN)
		*overflow = true;
	return value_saturate16(value);
}

static int16_t word_shift_right(int16_t value, int shift);

static int16_t word_shift_left(int16_t value, int shift)
{
	if (shift < 0)
		return word_shift_right(value, -shift);
	if (shift >= 15)
		return value ? (value > 0 ? INT16_MAX : INT16_MIN) : 0;
	return value_saturate16((int64_t)value * (INT64_C(1) << shift));
}

static int16_t word_shift_right(int16_t value, int shift)
{
	if (shift < 0)
		return word_shift_left(value, -shift);
	if (shift >= 15)
		return value < 0 ? -1 : 0;
	return (int16_t)(value >> shift);
}

static int16_t word_multiply(int16_t first, int16_t second)
{
	if (first == INT16_MIN && second == INT16_MIN)
		return INT16_MAX;
	return (int16_t)(((int32_t)first * second) >> 15);
}

static int16_t word_multiply_round(int16_t first, int16_t second)
{
	return value_saturate16(((int32_t)first * second + 16384) >> 15);
}

static int16_t word_shift_right_round(int16_t value, int shift)
{
	int16_t result;

	if (shift > 15)
		return 0;
	result = word_shift_right(value, shift);
	if (shift > 0 && (value & (1 << (shift - 1))))
		result++;
	return result;
}

static int32_t long_add(int32_t first, int32_t second)
{
	return value_saturate32((int64_t)first + second);
}

static int32_t long_subtract(int32_t first, int32_t second)
{
	return value_saturate32((int64_t)first - second);
}

static int32_t long_multiply(int16_t first, int16_t second)
{
	return value_saturate32((int64_t)first * second * 2);
}

static int32_t long_mac(int32_t accumulator, int16_t first, int16_t second)
{
	return long_add(accumulator, long_multiply(first, second));
}

static int32_t long_msu(int32_t accumulator, int16_t first, int16_t second)
{
	return long_subtract(accumulator, long_multiply(first, second));
}

static int32_t long_shift_right(int32_t value, int shift);

static int32_t long_saturate_track(int64_t value, bool *overflow)
{
	if (value > INT32_MAX || value < INT32_MIN)
		*overflow = true;
	return value_saturate32(value);
}

static int32_t long_multiply_track(int16_t first, int16_t second,
				   bool *overflow)
{
	return long_saturate_track((int64_t)first * second * 2, overflow);
}

static int32_t long_msu_track(int32_t accumulator, int16_t first,
			      int16_t second, bool *overflow)
{
	int32_t product = long_multiply_track(first, second, overflow);

	return long_saturate_track((int64_t)accumulator - product, overflow);
}

static int32_t long_mac_track(int32_t accumulator, int16_t first,
			      int16_t second, bool *overflow)
{
	int32_t product = long_multiply_track(first, second, overflow);

	return long_saturate_track((int64_t)accumulator + product, overflow);
}

static int32_t long_shift_left_track(int32_t value, int shift, bool *overflow)
{
	if (shift < 0)
		return long_shift_right(value, -shift);
	if (shift >= 31) {
		if (value)
			*overflow = true;
		return value ? (value > 0 ? INT32_MAX : INT32_MIN) : 0;
	}
	return long_saturate_track((int64_t)value * (INT64_C(1) << shift),
		overflow);
}

static int32_t long_shift_left(int32_t value, int shift)
{
	if (shift < 0)
		return long_shift_right(value, -shift);
	if (shift >= 31)
		return value ? (value > 0 ? INT32_MAX : INT32_MIN) : 0;
	return value_saturate32((int64_t)value * (INT64_C(1) << shift));
}

static int32_t long_shift_right(int32_t value, int shift)
{
	if (shift < 0)
		return long_shift_left(value, -shift);
	if (shift >= 31)
		return value < 0 ? -1 : 0;
	return value >> shift;
}

static int32_t long_shift_right_round(int32_t value, int shift)
{
	if (shift <= 0)
		return long_shift_left(value, -shift);
	if (shift > 31)
		return 0;
	return long_add(long_shift_right(value, shift),
		((uint32_t)value >> (shift - 1)) & 1U);
}

static int16_t long_round(int32_t value)
{
	return word_high_get(long_add(value, 32768));
}

static int long_normalize(int32_t value)
{
	uint32_t magnitude;

	if (!value)
		return 0;
	if (value == -1)
		return 31;
	magnitude = value < 0 ? ~(uint32_t)value : (uint32_t)value;
	return __builtin_clz(magnitude) - 1;
}

static int word_normalize(int16_t value)
{
	uint16_t magnitude;

	if (!value)
		return 0;
	if (value == -1)
		return 15;
	magnitude = value < 0 ? (uint16_t)~value : (uint16_t)value;
	return __builtin_clz((unsigned)magnitude) -
		((int)(sizeof(unsigned) * CHAR_BIT) - 15);
}

static int16_t word_absolute(int16_t value)
{
	if (value == INT16_MIN)
		return INT16_MAX;
	return value < 0 ? (int16_t)-value : value;
}

static int32_t long_absolute(int32_t value)
{
	if (value == INT32_MIN)
		return INT32_MAX;
	return value < 0 ? -value : value;
}

static int16_t fraction_divide(int16_t numerator, int16_t denominator)
{
	uint32_t remainder;
	uint32_t divisor;
	uint16_t quotient = 0;
	unsigned i;

	if (numerator <= 0)
		return 0;
	if (numerator >= denominator)
		return INT16_MAX;
	remainder = (uint16_t)numerator;
	divisor = (uint16_t)denominator;
	for (i = 0; i < 15; i++) {
		quotient <<= 1;
		remainder <<= 1;
		if (remainder < divisor)
			continue;
		remainder -= divisor;
		quotient++;
	}
	return (int16_t)quotient;
}

static void long_extract(int32_t value, int16_t *high, int16_t *low)
{
	*high = word_high_get(value);
	*low = word_low_get(long_msu(long_shift_right(value, 1), *high, 16384));
}

static int32_t long_multiply_dpf(int16_t high, int16_t low, int16_t value)
{
	int32_t product = long_multiply(high, value);

	return long_mac(product, word_multiply(low, value), 1);
}

static int32_t long_compose(int16_t high, int16_t low)
{
	return long_mac((int32_t)high * 65536, low, 1);
}

static int32_t long_multiply_dpf_pair(int16_t first_high, int16_t first_low,
				      int16_t second_high, int16_t second_low)
{
	int32_t result = long_multiply(first_high, second_high);

	result = long_mac(result, word_multiply(first_high, second_low), 1);
	return long_mac(result, word_multiply(first_low, second_high), 1);
}

static int32_t long_divide_dpf(int32_t numerator, int16_t high, int16_t low)
{
	int16_t approximation = fraction_divide(16383, high);
	int16_t reciprocal_high;
	int16_t reciprocal_low;
	int16_t numerator_high;
	int16_t numerator_low;
	int32_t reciprocal;

	reciprocal = long_multiply_dpf(high, low, approximation);
	reciprocal = long_subtract(INT32_MAX, reciprocal);
	long_extract(reciprocal, &reciprocal_high, &reciprocal_low);
	reciprocal = long_multiply_dpf(reciprocal_high, reciprocal_low,
		approximation);
	long_extract(reciprocal, &reciprocal_high, &reciprocal_low);
	long_extract(numerator, &numerator_high, &numerator_low);
	reciprocal = long_multiply_dpf_pair(numerator_high, numerator_low,
		reciprocal_high, reciprocal_low);
	return long_shift_left(reciprocal, 2);
}

/*
 * The tables that follow from a closed formula. g729_tables_init() computes
 * them once, in double precision, with rounding to the nearest integer and
 * 16-bit saturation. Each entry equals the constant of the normative C code.
 * g729_tables_verify() lets a target prove that its libm rounds each entry
 * as the host does.
 */
int16_t g729_power2_table[33];
int16_t g729_log2_table[33];
int16_t g729_inverse_sqrt_table[49];
int16_t g729_hamwindow[240];
int16_t g729_cos_table[65];
int16_t g729_cos_slope[64];
int16_t g729_acos_slope[64];

static pthread_once_t tables_once = PTHREAD_ONCE_INIT;

static int16_t table_round(double value)
{
	return value_saturate16(llround(value));
}

/* Equation (3) of clause 3.2.1: half a Hamming window over samples 0 to 199
 * and a quarter of a cosine cycle over samples 200 to 239. */
static double lp_window_get(unsigned sample)
{
	if (sample < 200)
		return 0.54 - 0.46 * cos(2.0 * M_PI * sample / 399.0);
	return cos(2.0 * M_PI * (sample - 200) / 159.0);
}

static void tables_compute(void)
{
	long long cosine[65];
	unsigned i;

	for (i = 0; i <= 32; i++) {
		g729_power2_table[i] = table_round(16384.0 * exp2(i / 32.0));
		g729_log2_table[i] = table_round(32767.0 *
			log2(1.0 + i / 32.0));
	}
	for (i = 0; i <= 48; i++)
		g729_inverse_sqrt_table[i] = table_round(32768.0 /
			sqrt(1.0 + i / 16.0));
	for (i = 0; i < 240; i++)
		g729_hamwindow[i] = table_round(32767.0 * lp_window_get(i));
	for (i = 0; i <= 64; i++) {
		cosine[i] = llround(32768.0 * cos(M_PI * i / 64.0));
		g729_cos_table[i] = value_saturate16(cosine[i]);
	}
	/* The arc cosine slope divides by the step of the rounded cosine
	 * before saturation, so its first step starts at 32768. */
	for (i = 0; i < 64; i++) {
		g729_cos_slope[i] = table_round(524288.0 *
			(cos(M_PI * (i + 1) / 64.0) - cos(M_PI * i / 64.0)));
		g729_acos_slope[i] = table_round(1048576.0 /
			(double)(cosine[i + 1] - cosine[i]));
	}
}

void g729_tables_init(void)
{
	pthread_once(&tables_once, tables_compute);
}

/* The CRC-32 of zlib over the little-endian bytes of the table. */
static uint32_t table_crc_get(const int16_t *table, unsigned count)
{
	uint32_t crc = UINT32_MAX;
	unsigned i;

	for (i = 0; i < 2 * count; i++) {
		uint16_t value = (uint16_t)table[i / 2];
		unsigned bit;

		crc ^= (i & 1) ? value >> 8 : value & 0xffU;
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1U));
	}
	return ~crc;
}

#define TABLE_CHECK(table, crc) \
	{ table, sizeof(table) / sizeof((table)[0]), crc }

bool g729_tables_verify(void)
{
	static const struct {
		const int16_t *table;
		unsigned count;
		uint32_t crc;
	} checks[] = {
		TABLE_CHECK(g729_power2_table, 0xe3e9fe51U),
		TABLE_CHECK(g729_log2_table, 0x5474b326U),
		TABLE_CHECK(g729_inverse_sqrt_table, 0xd97bb52fU),
		TABLE_CHECK(g729_hamwindow, 0xf5c54a53U),
		TABLE_CHECK(g729_cos_table, 0x1d828fa4U),
		TABLE_CHECK(g729_cos_slope, 0x9ed41b14U),
		TABLE_CHECK(g729_acos_slope, 0xc1f1383dU),
	};
	unsigned i;

	g729_tables_init();
	for (i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
		if (table_crc_get(checks[i].table, checks[i].count) !=
		    checks[i].crc)
			return false;
	}
	return true;
}

static void log2_get(int32_t value, int16_t *exponent, int16_t *fraction)
{
	int normalization;
	int index;
	int16_t offset;
	int16_t difference;
	int32_t interpolated;

	if (value <= 0) {
		*exponent = 0;
		*fraction = 0;
		return;
	}
	normalization = long_normalize(value);
	value = long_shift_left(value, normalization);
	*exponent = (int16_t)(30 - normalization);
	value = long_shift_right(value, 9);
	index = word_high_get(value) - 32;
	value = long_shift_right(value, 1);
	offset = word_low_get(value) & 0x7fff;
	interpolated = (int32_t)g729_log2_table[index] * 65536;
	difference = word_subtract(g729_log2_table[index],
		g729_log2_table[index + 1]);
	interpolated = long_msu(interpolated, difference, offset);
	*fraction = word_high_get(interpolated);
}

static int32_t power2_get(int16_t exponent, int16_t fraction)
{
	int32_t scaled = long_multiply(fraction, 32);
	int index = word_high_get(scaled);
	int16_t offset;
	int16_t difference;
	int shift;

	scaled = long_shift_right(scaled, 1);
	offset = word_low_get(scaled) & 0x7fff;
	scaled = (int32_t)g729_power2_table[index] * 65536;
	difference = word_subtract(g729_power2_table[index],
		g729_power2_table[index + 1]);
	scaled = long_msu(scaled, difference, offset);
	shift = 30 - exponent;
	return long_shift_right_round(scaled, shift);
}

static int32_t inverse_sqrt_get(int32_t value)
{
	int normalization;
	int exponent;
	int index;
	int16_t offset;
	int16_t difference;
	int32_t interpolated;

	if (value <= 0)
		return INT32_C(0x3fffffff);
	normalization = long_normalize(value);
	value = long_shift_left(value, normalization);
	exponent = 30 - normalization;
	if (!(exponent & 1))
		value = long_shift_right(value, 1);
	exponent = (exponent >> 1) + 1;
	value = long_shift_right(value, 9);
	index = word_high_get(value) - 16;
	value = long_shift_right(value, 1);
	offset = word_low_get(value) & 0x7fff;
	interpolated = (int32_t)g729_inverse_sqrt_table[index] * 65536;
	difference = word_subtract(g729_inverse_sqrt_table[index],
		g729_inverse_sqrt_table[index + 1]);
	interpolated = long_msu(interpolated, difference, offset);
	return long_shift_right(interpolated, exponent);
}

static void encoder_preprocess(struct g729_encoder *encoder, int16_t *signal)
{
	unsigned sample;

	for (sample = 0; sample < G729_FRAME; sample++) {
		int16_t oldest_x = encoder->preprocess_x[1];
		int32_t total;

		encoder->preprocess_x[1] = encoder->preprocess_x[0];
		encoder->preprocess_x[0] = signal[sample];
		total = long_multiply_dpf(encoder->preprocess_y_high[0],
			encoder->preprocess_y_low[0], g729_preprocess_a[1]);
		total = long_add(total, long_multiply_dpf(
			encoder->preprocess_y_high[1], encoder->preprocess_y_low[1],
			g729_preprocess_a[2]));
		total = long_mac(total, encoder->preprocess_x[0],
			g729_preprocess_b[0]);
		total = long_mac(total, encoder->preprocess_x[1],
			g729_preprocess_b[1]);
		total = long_mac(total, oldest_x, g729_preprocess_b[2]);
		total = long_shift_left(total, 3);
		signal[sample] = long_round(total);
		encoder->preprocess_y_high[1] = encoder->preprocess_y_high[0];
		encoder->preprocess_y_low[1] = encoder->preprocess_y_low[0];
		long_extract(total, &encoder->preprocess_y_high[0],
			&encoder->preprocess_y_low[0]);
	}
}

static void autocorrelation_get(const int16_t *speech, int16_t *high,
				int16_t *low, int16_t *exponent)
{
	int16_t windowed[240];
	int normalization;
	int scale = 1;
	int32_t total;
	unsigned lag;
	unsigned sample;

	for (sample = 0; sample < 240; sample++)
		windowed[sample] = long_round(long_multiply(speech[sample],
			g729_hamwindow[sample]));
	for (;;) {
		bool overflow = false;

		total = 1;
		for (sample = 0; sample < 240; sample++)
			total = long_mac_track(total, windowed[sample],
				windowed[sample], &overflow);
		if (!overflow)
			break;
		for (sample = 0; sample < 240; sample++)
			windowed[sample] = word_shift_right(windowed[sample], 2);
		scale += 4;
	}
	normalization = long_normalize(total);
	total = long_shift_left(total, normalization);
	long_extract(total, &high[0], &low[0]);
	*exponent = (int16_t)(scale - normalization);
	for (lag = 1; lag <= G729_VAD_ORDER; lag++) {
		total = 0;
		for (sample = 0; sample < 240 - lag; sample++)
			total = long_mac(total, windowed[sample],
				windowed[sample + lag]);
		total = long_shift_left(total, normalization);
		long_extract(total, &high[lag], &low[lag]);
	}
}

static void lag_window_apply(int16_t *high, int16_t *low)
{
	unsigned lag;

	for (lag = 1; lag <= G729_VAD_ORDER; lag++) {
		int32_t value = long_multiply_dpf_pair(high[lag], low[lag],
			g729_lag_h[lag - 1], g729_lag_l[lag - 1]);

		long_extract(value, &high[lag], &low[lag]);
	}
}

static void levinson_get(struct g729_encoder *encoder, const int16_t *r_high,
			 const int16_t *r_low, int16_t *lpc,
			 int16_t *residual_energy)
{
	int16_t a_high[G729_LPC_ORDER + 1] = { 0 };
	int16_t a_low[G729_LPC_ORDER + 1] = { 0 };
	int16_t next_high[G729_LPC_ORDER + 1];
	int16_t next_low[G729_LPC_ORDER + 1];
	int16_t alpha_high;
	int16_t alpha_low;
	int alpha_exponent;
	int16_t k_high;
	int16_t k_low;
	int16_t reflection[2] = { 0, 0 };
	int32_t value;
	int32_t ratio;
	unsigned order;

	value = long_compose(r_high[1], r_low[1]);
	ratio = long_divide_dpf(long_absolute(value), r_high[0], r_low[0]);
	if (value > 0)
		ratio = value_saturate32(-(int64_t)ratio);
	long_extract(ratio, &k_high, &k_low);
	value = long_shift_right(ratio, 4);
	long_extract(value, &a_high[1], &a_low[1]);
	value = long_multiply_dpf_pair(k_high, k_low, k_high, k_low);
	value = long_subtract(INT32_MAX, long_absolute(value));
	{
		int16_t factor_high;
		int16_t factor_low;

		long_extract(value, &factor_high, &factor_low);
		value = long_multiply_dpf_pair(r_high[0], r_low[0],
			factor_high, factor_low);
	}
	alpha_exponent = long_normalize(value);
	value = long_shift_left(value, alpha_exponent);
	long_extract(value, &alpha_high, &alpha_low);
	reflection[0] = k_high;
	for (order = 2; order <= G729_LPC_ORDER; order++) {
		unsigned coefficient;

		value = 0;
		for (coefficient = 1; coefficient < order; coefficient++)
			value = long_add(value, long_multiply_dpf_pair(
				r_high[coefficient], r_low[coefficient],
				a_high[order - coefficient], a_low[order - coefficient]));
		value = long_add(long_shift_left(value, 4),
			long_compose(r_high[order], r_low[order]));
		ratio = long_divide_dpf(long_absolute(value), alpha_high, alpha_low);
		if (value > 0)
			ratio = value_saturate32(-(int64_t)ratio);
		ratio = long_shift_left(ratio, alpha_exponent);
		long_extract(ratio, &k_high, &k_low);
		if (order == 2)
			reflection[1] = k_high;
		if (word_absolute(k_high) > 32750) {
			memcpy(lpc, encoder->lpc_previous,
				(G729_LPC_ORDER + 1) * sizeof(*lpc));
			return;
		}
		for (coefficient = 1; coefficient < order; coefficient++) {
			value = long_multiply_dpf_pair(k_high, k_low,
				a_high[order - coefficient], a_low[order - coefficient]);
			value = long_add(value,
				long_compose(a_high[coefficient], a_low[coefficient]));
			long_extract(value, &next_high[coefficient],
				&next_low[coefficient]);
		}
		value = long_shift_right(ratio, 4);
		long_extract(value, &next_high[order], &next_low[order]);
		value = long_multiply_dpf_pair(k_high, k_low, k_high, k_low);
		value = long_subtract(INT32_MAX, long_absolute(value));
		{
			int16_t factor_high;
			int16_t factor_low;

			long_extract(value, &factor_high, &factor_low);
			value = long_multiply_dpf_pair(alpha_high, alpha_low,
				factor_high, factor_low);
		}
		{
			int shift = long_normalize(value);

			value = long_shift_left(value, shift);
			long_extract(value, &alpha_high, &alpha_low);
			alpha_exponent += shift;
		}
		for (coefficient = 1; coefficient <= order; coefficient++) {
			a_high[coefficient] = next_high[coefficient];
			a_low[coefficient] = next_low[coefficient];
		}
	}
	/* The unstable early return above skips this write and leaves the
	 * caller's previous residual energy, as the normative C code does. */
	if (residual_energy)
		*residual_energy = word_shift_right(alpha_high, alpha_exponent);
	lpc[0] = 4096;
	for (order = 1; order <= G729_LPC_ORDER; order++) {
		value = long_compose(a_high[order], a_low[order]);
		lpc[order] = long_round(long_shift_left(value, 1));
	}
	memcpy(encoder->lpc_previous, lpc,
		(G729_LPC_ORDER + 1) * sizeof(*lpc));
	/* The VAD reads the order-2 reflection coefficient. The normative C
	 * code commits it only when all ten iterations succeed, so an unstable
	 * frame keeps the previous frame's pair, as the LPC fallback above
	 * keeps the previous filter. */
	memcpy(encoder->reflection_previous, reflection,
		sizeof(encoder->reflection_previous));
}

static int16_t chebyshev_get(int16_t x, const int16_t *coefficients,
			     bool reduced_scale)
{
	int16_t second_high = reduced_scale ? 128 : 256;
	int16_t second_low = 0;
	int16_t first_high;
	int16_t first_low;
	int16_t next_high;
	int16_t next_low;
	int32_t total;
	unsigned coefficient;

	total = long_multiply(x, reduced_scale ? 256 : 512);
	total = long_mac(total, coefficients[1], 4096);
	long_extract(total, &first_high, &first_low);
	for (coefficient = 2; coefficient < 5; coefficient++) {
		total = long_shift_left(long_multiply_dpf(first_high, first_low, x), 1);
		total = long_mac(total, second_high, INT16_MIN);
		total = long_msu(total, second_low, 1);
		total = long_mac(total, coefficients[coefficient], 4096);
		long_extract(total, &next_high, &next_low);
		second_high = first_high;
		second_low = first_low;
		first_high = next_high;
		first_low = next_low;
	}
	total = long_multiply_dpf(first_high, first_low, x);
	total = long_mac(total, second_high, INT16_MIN);
	total = long_msu(total, second_low, 1);
	total = long_mac(total, coefficients[5], 2048);
	return word_high_get(long_shift_left(total, reduced_scale ? 7 : 6));
}

static void lpc_to_lsp(const int16_t *lpc, const int16_t *fallback,
			int16_t *lsp)
{
	int16_t first[6];
	int16_t second[6];
	const int16_t *polynomial = first;
	bool reduced_scale = false;
	bool coefficient_overflow = false;
	int16_t low_x;
	int16_t low_y;
	unsigned root_count = 0;
	unsigned grid_index = 0;
	unsigned i;

	first[0] = 2048;
	second[0] = 2048;
	for (i = 0; i < 5; i++) {
		bool overflow = false;
		int32_t total = long_multiply(lpc[i + 1], 16384);
		int16_t sum;
		int16_t difference;

		total = long_mac_track(total, lpc[10 - i], 16384, &overflow);
		sum = word_high_get(total);
		first[i + 1] = word_subtract_track(sum, first[i], &overflow);
		total = long_multiply(lpc[i + 1], 16384);
		total = long_msu_track(total, lpc[10 - i], 16384, &overflow);
		difference = word_high_get(total);
		second[i + 1] = word_add_track(difference, second[i], &overflow);
		coefficient_overflow |= overflow;
	}
	if (coefficient_overflow) {
		reduced_scale = true;
		first[0] = 1024;
		second[0] = 1024;
		for (i = 0; i < 5; i++) {
			int32_t total = long_multiply(lpc[i + 1], 8192);
			int16_t sum;
			int16_t difference;

			total = long_mac(total, lpc[10 - i], 8192);
			sum = word_high_get(total);
			first[i + 1] = word_subtract(sum, first[i]);
			total = long_multiply(lpc[i + 1], 8192);
			total = long_msu(total, lpc[10 - i], 8192);
			difference = word_high_get(total);
			second[i + 1] = word_add(difference, second[i]);
		}
	}
	low_x = g729_lsp_grid[0];
	low_y = chebyshev_get(low_x, polynomial, reduced_scale);
	while (root_count < G729_LPC_ORDER && grid_index < 50) {
		int16_t high_x = low_x;
		int16_t high_y = low_y;

		grid_index++;
		low_x = g729_lsp_grid[grid_index];
		low_y = chebyshev_get(low_x, polynomial, reduced_scale);
		if (long_multiply(low_y, high_y) > 0)
			continue;
		for (i = 0; i < 2; i++) {
			int16_t middle_x = word_add(word_shift_right(low_x, 1),
				word_shift_right(high_x, 1));
			int16_t middle_y = chebyshev_get(middle_x, polynomial,
				reduced_scale);

			if (long_multiply(low_y, middle_y) <= 0) {
				high_x = middle_x;
				high_y = middle_y;
			} else {
				low_x = middle_x;
				low_y = middle_y;
			}
		}
		if (high_y == low_y) {
			lsp[root_count] = low_x;
		} else {
			int16_t width = word_subtract(high_x, low_x);
			int16_t delta = word_subtract(high_y, low_y);
			int sign = delta;
			int exponent;
			int16_t inverse;
			int32_t correction;

			delta = word_absolute(delta);
			exponent = word_normalize(delta);
			delta = word_shift_left(delta, exponent);
			inverse = fraction_divide(16383, delta);
			correction = long_multiply(width, inverse);
			correction = long_shift_right(correction, 20 - exponent);
			inverse = word_low_get(correction);
			if (sign < 0)
				inverse = word_subtract(0, inverse);
			correction = long_shift_right(long_multiply(low_y, inverse), 11);
			lsp[root_count] = word_subtract(low_x,
				word_low_get(correction));
		}
		low_x = lsp[root_count++];
		polynomial = polynomial == first ? second : first;
		low_y = chebyshev_get(low_x, polynomial, reduced_scale);
	}
	if (root_count != G729_LPC_ORDER)
		memcpy(lsp, fallback, G729_LPC_ORDER * sizeof(*lsp));
}

static void lsp_to_lsf(const int16_t *lsp, int16_t *lsf)
{
	int index = 63;
	int i;

	for (i = G729_LPC_ORDER - 1; i >= 0; i--) {
		int16_t offset;
		int16_t frequency;
		int32_t interpolation;

		while (index > 0 && g729_cos_table[index] < lsp[i])
			index--;
		offset = word_subtract(lsp[i], g729_cos_table[index]);
		interpolation = long_multiply(g729_acos_slope[index], offset);
		frequency = word_add(word_shift_left((int16_t)index, 9),
			word_low_get(long_shift_right(interpolation, 12)));
		lsf[i] = word_multiply(frequency, 25736);
	}
}

static void lsf_weights_get(const int16_t *lsf, int16_t *weights)
{
	int16_t spacing[G729_LPC_ORDER];
	int16_t maximum = 0;
	int shift;
	unsigned i;

	spacing[0] = word_subtract(lsf[1], 9221);
	for (i = 1; i + 1 < G729_LPC_ORDER; i++)
		spacing[i] = word_subtract(word_subtract(lsf[i + 1], lsf[i - 1]),
			8192);
	spacing[G729_LPC_ORDER - 1] = word_subtract(15485,
		lsf[G729_LPC_ORDER - 2]);
	for (i = 0; i < G729_LPC_ORDER; i++) {
		if (spacing[i] > 0) {
			weights[i] = 2048;
		} else {
			int32_t square = long_multiply(spacing[i], spacing[i]);
			int16_t scaled = word_high_get(long_shift_left(square, 2));

			square = long_multiply(scaled, 20480);
			weights[i] = word_add(word_high_get(long_shift_left(square, 2)),
				2048);
		}
	}
	weights[4] = word_high_get(long_shift_left(long_multiply(weights[4],
		19661), 1));
	weights[5] = word_high_get(long_shift_left(long_multiply(weights[5],
		19661), 1));
	for (i = 0; i < G729_LPC_ORDER; i++) {
		if (weights[i] > maximum)
			maximum = weights[i];
	}
	shift = word_normalize(maximum);
	for (i = 0; i < G729_LPC_ORDER; i++)
		weights[i] = word_shift_left(weights[i], shift);
}

static void encoder_lsf_history_update(struct g729_encoder *encoder,
				       const int16_t *element)
{
	unsigned history;

	for (history = 3; history > 0; history--)
		memcpy(encoder->lsf_history[history],
			encoder->lsf_history[history - 1],
			sizeof(encoder->lsf_history[history]));
	memcpy(encoder->lsf_history[0], element,
		sizeof(encoder->lsf_history[0]));
}

static void lsf_residual_get(int16_t history[4][G729_LPC_ORDER],
			     const int16_t *lsf,
			     const int16_t predictor[4][G729_LPC_ORDER],
			     const int16_t *present_inverse, int16_t *residual)
{
	unsigned i;
	unsigned past;

	for (i = 0; i < G729_LPC_ORDER; i++) {
		int32_t total = (int32_t)lsf[i] * 65536;
		int16_t difference;

		for (past = 0; past < 4; past++)
			total = long_msu(total, history[past][i],
				predictor[past][i]);
		difference = word_high_get(total);
		total = long_multiply(difference, present_inverse[i]);
		residual[i] = word_high_get(long_shift_left(total, 3));
	}
}

static void lsf_compose(int16_t history[4][G729_LPC_ORDER],
			const int16_t *element, const int16_t *present,
			const int16_t predictor[4][G729_LPC_ORDER],
			int16_t *lsf)
{
	unsigned i;
	unsigned past;

	for (i = 0; i < G729_LPC_ORDER; i++) {
		int32_t total = long_multiply(element[i], present[i]);

		for (past = 0; past < 4; past++)
			total = long_mac(total, history[past][i],
				predictor[past][i]);
		lsf[i] = word_high_get(total);
	}
}

static unsigned lsf_stage1_select(const int16_t *residual)
{
	int32_t best_distance = INT32_MAX;
	unsigned best = 0;
	unsigned candidate;

	for (candidate = 0; candidate < G729_LSP_STAGE1_SIZE; candidate++) {
		int32_t distance = 0;
		unsigned i;

		for (i = 0; i < G729_LPC_ORDER; i++) {
			int16_t difference = word_subtract(residual[i],
				g729_lsp_stage1[candidate][i]);

			distance = long_mac(distance, difference, difference);
		}
		if (distance >= best_distance)
			continue;
		best_distance = distance;
		best = candidate;
	}
	return best;
}

static unsigned lsf_stage2_select(const int16_t *residual,
				  const int16_t *weights, unsigned first,
				  unsigned start, unsigned end)
{
	int32_t best_distance = INT32_MAX;
	unsigned best = 0;
	unsigned candidate;

	for (candidate = 0; candidate < G729_LSP_STAGE2_SIZE; candidate++) {
		int32_t distance = 0;
		unsigned i;

		for (i = start; i < end; i++) {
			int16_t difference = word_subtract(word_subtract(residual[i],
				g729_lsp_stage1[first][i]),
				g729_lsp_stage2[candidate][i]);
			int16_t weighted = word_multiply(weights[i], difference);

			distance = long_mac(distance, weighted, difference);
		}
		if (distance >= best_distance)
			continue;
		best_distance = distance;
		best = candidate;
	}
	return best;
}

static void lsf_half_spacing_apply(int16_t *values, unsigned start,
				   unsigned end, int16_t gap)
{
	unsigned i;

	for (i = start + 1; i < end; i++) {
		int16_t difference = word_subtract(values[i - 1], values[i]);
		int16_t correction = word_shift_right(word_add(difference, gap), 1);

		if (correction <= 0)
			continue;
		values[i - 1] = word_subtract(values[i - 1], correction);
		values[i] = word_add(values[i], correction);
	}
}

static int32_t lsf_mode_distance(const int16_t *weights,
				 const int16_t *candidate,
				 const int16_t *residual, unsigned mode)
{
	int32_t distance = 0;
	unsigned i;

	for (i = 0; i < G729_LPC_ORDER; i++) {
		int16_t difference = word_subtract(candidate[i], residual[i]);
		int16_t predicted = word_multiply(difference,
			g729_lsp_present[mode][i]);
		int16_t weighted = word_high_get(long_shift_left(
			long_multiply(weights[i], predicted), 4));

		distance = long_mac(distance, weighted, predicted);
	}
	return distance;
}

static void lsf_quantize(struct g729_encoder *encoder, const int16_t *lsp,
			 struct g729_parameters *parameters, int16_t *quantized_lsp)
{
	int16_t lsf[G729_LPC_ORDER];
	int16_t weights[G729_LPC_ORDER];
	int16_t residual[2][G729_LPC_ORDER];
	int16_t candidate[2][G729_LPC_ORDER];
	int32_t distance[2];
	unsigned first[2];
	unsigned lower[2];
	unsigned upper[2];
	unsigned mode;
	unsigned i;

	lsp_to_lsf(lsp, lsf);
	lsf_weights_get(lsf, weights);
	for (mode = 0; mode < 2; mode++) {
		lsf_residual_get(encoder->lsf_history, lsf,
			g729_lsp_predictor[mode],
			g729_lsp_present_inverse[mode], residual[mode]);
		first[mode] = lsf_stage1_select(residual[mode]);
		lower[mode] = lsf_stage2_select(residual[mode], weights,
			first[mode], 0, 5);
		upper[mode] = lsf_stage2_select(residual[mode], weights,
			first[mode], 5, G729_LPC_ORDER);
		for (i = 0; i < 5; i++)
			candidate[mode][i] = word_add(
				g729_lsp_stage1[first[mode]][i],
				g729_lsp_stage2[lower[mode]][i]);
		lsf_half_spacing_apply(candidate[mode], 0, 5, 10);
		for (i = 5; i < G729_LPC_ORDER; i++)
			candidate[mode][i] = word_add(
				g729_lsp_stage1[first[mode]][i],
				g729_lsp_stage2[upper[mode]][i]);
		lsf_half_spacing_apply(candidate[mode], 4, G729_LPC_ORDER, 10);
		lsf_spacing_apply(candidate[mode], 5);
		distance[mode] = lsf_mode_distance(weights, candidate[mode],
			residual[mode], mode);
	}
	mode = distance[1] < distance[0] ? 1 : 0;
	parameters->lsp_first = (uint16_t)((mode << 7) | first[mode]);
	parameters->lsp_second = (uint16_t)((lower[mode] << 5) | upper[mode]);
	for (i = 0; i < 5; i++)
		candidate[mode][i] = word_add(g729_lsp_stage1[first[mode]][i],
			g729_lsp_stage2[lower[mode]][i]);
	for (i = 5; i < G729_LPC_ORDER; i++)
		candidate[mode][i] = word_add(g729_lsp_stage1[first[mode]][i],
			g729_lsp_stage2[upper[mode]][i]);
	lsf_spacing_apply(candidate[mode], 10);
	lsf_spacing_apply(candidate[mode], 5);
	lsf_compose(encoder->lsf_history, candidate[mode],
		g729_lsp_present[mode], g729_lsp_predictor[mode], lsf);
	encoder_lsf_history_update(encoder, candidate[mode]);
	lsf_stability_apply(lsf);
	lsf_to_lsp(lsf, quantized_lsp);
}

static unsigned bits_read(const uint8_t *data, unsigned *position,
			  unsigned count)
{
	unsigned value = 0;
	unsigned i;

	for (i = 0; i < count; i++) {
		unsigned bit = *position + i;

		value = (value << 1) | ((data[bit / 8] >> (7 - bit % 8)) & 1U);
	}
	*position += count;
	return value;
}

static void bits_write(uint8_t *data, unsigned *position, unsigned value,
		       unsigned count)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		unsigned bit = *position + i;
		unsigned source = (value >> (count - i - 1)) & 1U;

		data[bit / 8] |= (uint8_t)(source << (7 - bit % 8));
	}
	*position += count;
}

static void parameters_pack(const struct g729_parameters *parameters,
			    uint8_t payload[G729_BYTES])
{
	unsigned position = 0;

	memset(payload, 0, G729_BYTES);
	bits_write(payload, &position, parameters->lsp_first, 8);
	bits_write(payload, &position, parameters->lsp_second, 10);
	bits_write(payload, &position, parameters->pitch[0], 8);
	bits_write(payload, &position, parameters->parity, 1);
	bits_write(payload, &position, parameters->fixed[0], 13);
	bits_write(payload, &position, parameters->signs[0], 4);
	bits_write(payload, &position, parameters->gain[0], 7);
	bits_write(payload, &position, parameters->pitch[1], 5);
	bits_write(payload, &position, parameters->fixed[1], 13);
	bits_write(payload, &position, parameters->signs[1], 4);
	bits_write(payload, &position, parameters->gain[1], 7);
}

static void parameters_unpack(const uint8_t payload[G729_BYTES],
			      struct g729_parameters *parameters)
{
	unsigned position = 0;

	parameters->lsp_first = bits_read(payload, &position, 8);
	parameters->lsp_second = bits_read(payload, &position, 10);
	parameters->pitch[0] = bits_read(payload, &position, 8);
	parameters->parity = bits_read(payload, &position, 1);
	parameters->fixed[0] = bits_read(payload, &position, 13);
	parameters->signs[0] = bits_read(payload, &position, 4);
	parameters->gain[0] = bits_read(payload, &position, 7);
	parameters->pitch[1] = bits_read(payload, &position, 5);
	parameters->fixed[1] = bits_read(payload, &position, 13);
	parameters->signs[1] = bits_read(payload, &position, 4);
	parameters->gain[1] = bits_read(payload, &position, 7);
}

static unsigned pitch_parity_get(unsigned index)
{
	unsigned parity = 1;
	unsigned i;

	index >>= 2;
	for (i = 0; i < 6; i++) {
		parity ^= index & 1U;
		index >>= 1;
	}
	return parity;
}

static void lsf_spacing_apply(int16_t *lsf, int16_t gap)
{
	unsigned i;

	for (i = 1; i < G729_LPC_ORDER; i++) {
		int16_t distance = word_subtract(lsf[i - 1], lsf[i]);
		int16_t correction = word_shift_right(word_add(distance, gap), 1);

		if (correction <= 0)
			continue;
		lsf[i - 1] = word_subtract(lsf[i - 1], correction);
		lsf[i] = word_add(lsf[i], correction);
	}
}

static void lsf_history_update(struct g729_decoder *decoder,
			       const int16_t *element)
{
	unsigned i;

	for (i = 3; i > 0; i--)
		memcpy(decoder->lsf_history[i], decoder->lsf_history[i - 1],
		       sizeof(decoder->lsf_history[i]));
	memcpy(decoder->lsf_history[0], element,
	       sizeof(decoder->lsf_history[0]));
}

static void lsf_stability_apply(int16_t *lsf)
{
	unsigned i;

	for (i = 0; i + 1 < G729_LPC_ORDER; i++) {
		int16_t temporary;

		if (lsf[i + 1] >= lsf[i])
			continue;
		temporary = lsf[i];
		lsf[i] = lsf[i + 1];
		lsf[i + 1] = temporary;
	}
	if (lsf[0] < 40)
		lsf[0] = 40;
	for (i = 0; i + 1 < G729_LPC_ORDER; i++) {
		if ((int32_t)lsf[i + 1] - lsf[i] < 321)
			lsf[i + 1] = word_add(lsf[i], 321);
	}
	if (lsf[G729_LPC_ORDER - 1] > 25681)
		lsf[G729_LPC_ORDER - 1] = 25681;
}

static void lsf_good_decode(struct g729_decoder *decoder,
			    const struct g729_parameters *parameters,
			    int16_t *lsf)
{
	int mode = parameters->lsp_first >> 7;
	int first = parameters->lsp_first & 0x7f;
	int lower = parameters->lsp_second >> 5;
	int upper = parameters->lsp_second & 0x1f;
	int16_t element[G729_LPC_ORDER];
	unsigned i;
	unsigned history;

	for (i = 0; i < G729_LPC_ORDER; i++) {
		int second = i < 5 ? lower : upper;

		element[i] = word_add(g729_lsp_stage1[first][i],
			g729_lsp_stage2[second][i]);
	}
	lsf_spacing_apply(element, 10);
	lsf_spacing_apply(element, 5);
	for (i = 0; i < G729_LPC_ORDER; i++) {
		int32_t total = long_multiply(element[i], g729_lsp_present[mode][i]);

		for (history = 0; history < 4; history++)
			total = long_mac(total, decoder->lsf_history[history][i],
				g729_lsp_predictor[mode][history][i]);
		lsf[i] = word_high_get(total);
	}
	lsf_history_update(decoder, element);
	lsf_stability_apply(lsf);
	memcpy(decoder->lsf_previous, lsf, sizeof(decoder->lsf_previous));
	decoder->previous_ma = (int16_t)mode;
}

static void lsf_erasure_decode(struct g729_decoder *decoder, int16_t *lsf)
{
	int16_t element[G729_LPC_ORDER];
	int mode = decoder->previous_ma;
	unsigned i;
	unsigned history;

	memcpy(lsf, decoder->lsf_previous, sizeof(decoder->lsf_previous));
	for (i = 0; i < G729_LPC_ORDER; i++) {
		int32_t total = (int32_t)decoder->lsf_previous[i] * 65536;
		int16_t residual;

		for (history = 0; history < 4; history++)
			total = long_msu(total, decoder->lsf_history[history][i],
				g729_lsp_predictor[mode][history][i]);
		residual = word_high_get(total);
		total = long_multiply(residual,
			g729_lsp_present_inverse[mode][i]);
		element[i] = word_high_get(long_shift_left(total, 3));
	}
	lsf_history_update(decoder, element);
}

static void lsf_to_lsp(const int16_t *lsf, int16_t *lsp)
{
	unsigned i;

	for (i = 0; i < G729_LPC_ORDER; i++) {
		int16_t frequency = word_multiply(lsf[i], 20861);
		unsigned index = (uint16_t)frequency >> 8;
		int16_t offset = frequency & 0xff;
		int32_t interpolation;

		if (index > 63)
			index = 63;
		interpolation = long_multiply(g729_cos_slope[index], offset);
		lsp[i] = word_add(g729_cos_table[index],
			word_low_get(long_shift_right(interpolation, 13)));
	}
}

static void lsp_polynomial_get(const int16_t *lsp, int32_t *polynomial)
{
	unsigned order;

	polynomial[0] = long_multiply(4096, 2048);
	polynomial[1] = long_msu(0, lsp[0], 512);
	for (order = 2; order <= 5; order++) {
		int coefficient;
		int16_t root = lsp[2 * order - 2];

		polynomial[order] = polynomial[order - 2];
		for (coefficient = (int)order; coefficient > 1; coefficient--) {
			int16_t high;
			int16_t low;
			int32_t product;

			long_extract(polynomial[coefficient - 1], &high, &low);
			product = long_shift_left(long_multiply_dpf(high, low,
				root), 1);
			polynomial[coefficient] = long_subtract(
				long_add(polynomial[coefficient],
					 polynomial[coefficient - 2]), product);
		}
		polynomial[1] = long_msu(polynomial[1], root, 512);
	}
}

static void lsp_to_lpc(const int16_t *lsp, int16_t *lpc)
{
	int32_t first[6];
	int32_t second[6];
	int i;

	lsp_polynomial_get(lsp, first);
	lsp_polynomial_get(lsp + 1, second);
	for (i = 5; i > 0; i--) {
		first[i] = long_add(first[i], first[i - 1]);
		second[i] = long_subtract(second[i], second[i - 1]);
	}
	lpc[0] = 4096;
	for (i = 1; i <= 5; i++) {
		lpc[i] = word_low_get(long_shift_right_round(
			long_add(first[i], second[i]), 13));
		lpc[11 - i] = word_low_get(long_shift_right_round(
			long_subtract(first[i], second[i]), 13));
	}
}

static void lsp_pair_to_lpc(const int16_t *previous, const int16_t *current,
			    int16_t lpc[2][G729_LPC_ORDER + 1])
{
	int16_t interpolated[G729_LPC_ORDER];
	unsigned i;

	for (i = 0; i < G729_LPC_ORDER; i++)
		interpolated[i] = word_add(word_shift_right(previous[i], 1),
			word_shift_right(current[i], 1));
	lsp_to_lpc(interpolated, lpc[0]);
	lsp_to_lpc(current, lpc[1]);
}

static void lpc_frame_get(struct g729_decoder *decoder,
			  const struct g729_parameters *parameters, bool erased,
			  int16_t lpc[2][G729_LPC_ORDER + 1])
{
	int16_t lsf[G729_LPC_ORDER];
	int16_t lsp[G729_LPC_ORDER];

	if (erased)
		lsf_erasure_decode(decoder, lsf);
	else
		lsf_good_decode(decoder, parameters, lsf);
	lsf_to_lsp(lsf, lsp);
	lsp_pair_to_lpc(decoder->lsp_previous, lsp, lpc);
	memcpy(decoder->lsp_previous, lsp, sizeof(decoder->lsp_previous));
}

static void pitch_decode(unsigned index, bool second_subframe,
			 int16_t *pitch, int16_t *fraction)
{
	if (!second_subframe) {
		if (index < 197) {
			*pitch = (int16_t)((index + 2) / 3 + 19);
			*fraction = (int16_t)(index - 3 * *pitch + 58);
			return;
		}
		*pitch = (int16_t)index - 112;
		*fraction = 0;
		return;
	}
	{
		int minimum = *pitch - 5;
		int quotient;

		if (minimum < G729_PITCH_MIN)
			minimum = G729_PITCH_MIN;
		if (minimum + 9 > G729_PITCH_MAX)
			minimum = G729_PITCH_MAX - 9;
		quotient = (index + 2) / 3 - 1;
		*pitch = (int16_t)(minimum + quotient);
		*fraction = (int16_t)(index - 2 - 3 * quotient);
	}
}

static void adaptive_codebook_get(int16_t *excitation, int pitch,
				  int fraction)
{
	int16_t *base = excitation - pitch;
	unsigned sample;

	fraction = -fraction;
	if (fraction < 0) {
		fraction += 3;
		base--;
	}
	for (sample = 0; sample < G729_SUBFRAME; sample++) {
		int16_t *left = base++;
		int16_t *right = base;
		int32_t total = 0;
		unsigned tap;

		for (tap = 0; tap < 10; tap++) {
			total = long_mac(total, left[-(int)tap],
				g729_pitch_interpolation[fraction + 3 * tap]);
			total = long_mac(total, right[tap],
				g729_pitch_interpolation[3 - fraction + 3 * tap]);
		}
		excitation[sample] = long_round(total);
	}
}

static void fixed_codebook_get(unsigned index, unsigned signs, int16_t *code)
{
	unsigned position[4];
	unsigned pulse;

	position[0] = 5 * (index & 7U);
	index >>= 3;
	position[1] = 5 * (index & 7U) + 1;
	index >>= 3;
	position[2] = 5 * (index & 7U) + 2;
	index >>= 3;
	position[3] = 5 * ((index >> 1) & 7U) + 3 + (index & 1U);
	memset(code, 0, G729_SUBFRAME * sizeof(*code));
	for (pulse = 0; pulse < 4; pulse++) {
		code[position[pulse]] = signs & 1U ? 8191 : -8192;
		signs >>= 1;
	}
}

static uint16_t decoder_random_get(struct g729_decoder *decoder)
{
	decoder->random_seed = (uint16_t)(31821U * decoder->random_seed + 13849U);
	return decoder->random_seed;
}

static void gain_energy_erasure_update(struct g729_decoder *decoder)
{
	int32_t average = 0;
	unsigned i;

	for (i = 0; i < 4; i++)
		average += decoder->gain_energy[i];
	average = (average >> 2) - 4096;
	if (average < -14336)
		average = -14336;
	for (i = 3; i > 0; i--)
		decoder->gain_energy[i] = decoder->gain_energy[i - 1];
	decoder->gain_energy[0] = (int16_t)average;
}

static void gain_predict(struct g729_decoder *decoder, const int16_t *code,
			 int16_t *gain, int16_t *gain_exponent)
{
	int32_t energy = 0;
	int32_t prediction;
	int16_t exponent;
	int16_t fraction;
	int16_t high;
	int16_t low;
	unsigned i;

	for (i = 0; i < G729_SUBFRAME; i++)
		energy = long_mac(energy, code[i], code[i]);
	log2_get(energy, &exponent, &fraction);
	prediction = long_multiply_dpf(exponent, fraction, -24660);
	prediction = long_mac(prediction, 32588, 32);
	prediction = long_shift_left(prediction, 10);
	for (i = 0; i < 4; i++)
		prediction = long_mac(prediction, g729_gain_predictor[i],
			decoder->gain_energy[i]);
	*gain = word_high_get(prediction);
	prediction = long_shift_right(long_multiply(*gain, 5439), 8);
	long_extract(prediction, &high, &low);
	*gain = word_low_get(power2_get(14, low));
	*gain_exponent = word_subtract(14, high);
}

static void gain_energy_update(struct g729_decoder *decoder, int32_t factor)
{
	int16_t exponent;
	int16_t fraction;
	int16_t normalized;
	unsigned i;

	for (i = 3; i > 0; i--)
		decoder->gain_energy[i] = decoder->gain_energy[i - 1];
	log2_get(factor, &exponent, &fraction);
	normalized = word_high_get(long_shift_left(long_add(
		(int32_t)(exponent - 13) * 65536,
		(int32_t)fraction * 2), 13));
	decoder->gain_energy[0] = word_multiply(normalized, 24660);
}

static void gains_decode(struct g729_decoder *decoder, unsigned index,
			 const int16_t *code, bool erased)
{
	int first;
	int second;
	int16_t predicted;
	int16_t predicted_exponent;
	int16_t correction;
	int32_t correction_q13;
	int32_t product;

	if (erased) {
		decoder->gain_pitch = word_multiply(decoder->gain_pitch, 29491);
		if (decoder->gain_pitch > 29491)
			decoder->gain_pitch = 29491;
		decoder->gain_code = word_multiply(decoder->gain_code, 32111);
		gain_energy_erasure_update(decoder);
		return;
	}
	first = g729_gain_inverse1[index >> 4];
	second = g729_gain_inverse2[index & 15];
	decoder->gain_pitch = word_add(g729_gain_stage1[first][0],
		g729_gain_stage2[second][0]);
	gain_predict(decoder, code, &predicted, &predicted_exponent);
	correction_q13 = (int32_t)g729_gain_stage1[first][1] +
		g729_gain_stage2[second][1];
	correction = word_low_get(long_shift_right(correction_q13, 1));
	product = long_multiply(correction, predicted);
	product = long_shift_left(product, -predicted_exponent + 4);
	decoder->gain_code = word_high_get(product);
	gain_energy_update(decoder, correction_q13);
}

static bool synthesis_filter(const int16_t *lpc, const int16_t *excitation,
			     int16_t *output, int16_t *memory)
{
	int16_t history[G729_LPC_ORDER + G729_SUBFRAME];
	bool overflow = false;
	unsigned sample;

	memcpy(history, memory, G729_LPC_ORDER * sizeof(*history));
	for (sample = 0; sample < G729_SUBFRAME; sample++) {
		int32_t total = long_multiply_track(excitation[sample], lpc[0],
			&overflow);
		unsigned coefficient;

		for (coefficient = 1; coefficient <= G729_LPC_ORDER; coefficient++)
			total = long_msu_track(total, lpc[coefficient],
				history[G729_LPC_ORDER + sample - coefficient], &overflow);
		total = long_shift_left_track(total, 3, &overflow);
		total = long_saturate_track((int64_t)total + 32768, &overflow);
		history[G729_LPC_ORDER + sample] = word_high_get(total);
	}
	memcpy(output, history + G729_LPC_ORDER,
	       G729_SUBFRAME * sizeof(*output));
	if (!overflow)
		memcpy(memory, history + G729_SUBFRAME,
		       G729_LPC_ORDER * sizeof(*memory));
	return overflow;
}

static void lpc_weight(const int16_t *lpc, int16_t gamma, int16_t *weighted)
{
	int16_t factor = gamma;
	unsigned i;

	weighted[0] = lpc[0];
	for (i = 1; i <= G729_LPC_ORDER; i++) {
		weighted[i] = long_round(long_multiply(lpc[i], factor));
		factor = long_round(long_multiply(factor, gamma));
	}
}

static void residual_filter(const int16_t *lpc, const int16_t *speech,
			    int16_t *residual)
{
	unsigned sample;

	for (sample = 0; sample < G729_SUBFRAME; sample++) {
		int32_t total = long_multiply(speech[sample], lpc[0]);
		unsigned coefficient;

		for (coefficient = 1; coefficient <= G729_LPC_ORDER; coefficient++)
			total = long_mac(total, lpc[coefficient],
				speech[(int)sample - (int)coefficient]);
		residual[sample] = long_round(long_shift_left(total, 3));
	}
}

static void synthesis_filter_plain(const int16_t *lpc, const int16_t *input,
				   int16_t *output, unsigned count,
				   int16_t *memory)
{
	int16_t source[G729_SUBFRAME];
	int16_t history[G729_LPC_ORDER + G729_SUBFRAME];
	unsigned sample;

	memcpy(source, input, count * sizeof(*source));
	memcpy(history, memory, G729_LPC_ORDER * sizeof(*history));
	for (sample = 0; sample < count; sample++) {
		int32_t total = long_multiply(source[sample], lpc[0]);
		unsigned coefficient;

		for (coefficient = 1; coefficient <= G729_LPC_ORDER; coefficient++)
			total = long_msu(total, lpc[coefficient],
				history[G729_LPC_ORDER + sample - coefficient]);
		history[G729_LPC_ORDER + sample] =
			long_round(long_shift_left(total, 3));
	}
	memcpy(output, history + G729_LPC_ORDER, count * sizeof(*output));
	memcpy(memory, history + count, G729_LPC_ORDER * sizeof(*memory));
}

static int16_t open_pitch_region_get(const int16_t *signal, int start,
				     int end, int step, int *best_pitch)
{
	int32_t best_correlation = INT32_MIN;
	int32_t energy = 1;
	int32_t inverse;
	int16_t correlation_high;
	int16_t correlation_low;
	int16_t inverse_high;
	int16_t inverse_low;
	int pitch;
	unsigned sample;

	*best_pitch = start;
	for (pitch = start; pitch < end; pitch += step) {
		int32_t correlation = 0;

		for (sample = 0; sample < G729_FRAME; sample += 2)
			correlation = long_mac(correlation, signal[sample],
				signal[(int)sample - pitch]);
		if (correlation <= best_correlation)
			continue;
		best_correlation = correlation;
		*best_pitch = pitch;
	}
	for (sample = 0; sample < G729_FRAME; sample += 2)
		energy = long_mac(energy, signal[(int)sample - *best_pitch],
			signal[(int)sample - *best_pitch]);
	inverse = inverse_sqrt_get(energy);
	long_extract(best_correlation, &correlation_high, &correlation_low);
	long_extract(inverse, &inverse_high, &inverse_low);
	return word_low_get(long_multiply_dpf_pair(correlation_high,
		correlation_low, inverse_high, inverse_low));
}

static int open_pitch_get(const int16_t *signal)
{
	int16_t scaled_storage[G729_PITCH_MAX + G729_FRAME];
	int16_t *scaled = scaled_storage + G729_PITCH_MAX;
	int16_t first_value;
	int16_t second_value;
	int16_t third_value;
	int first_pitch;
	int second_pitch;
	int third_pitch;
	int32_t energy = 0;
	bool overflow = false;
	int sample;

	for (sample = -G729_PITCH_MAX; sample < (int)G729_FRAME; sample += 2)
		energy = long_mac_track(energy, signal[sample], signal[sample],
			&overflow);
	for (sample = -G729_PITCH_MAX; sample < (int)G729_FRAME; sample++) {
		if (overflow)
			scaled[sample] = word_shift_right(signal[sample], 3);
		else if (energy < 1048576)
			scaled[sample] = word_shift_left(signal[sample], 3);
		else
			scaled[sample] = signal[sample];
	}
	first_value = open_pitch_region_get(scaled, 20, 40, 1, &first_pitch);
	second_value = open_pitch_region_get(scaled, 40, 80, 1, &second_pitch);
	third_value = open_pitch_region_get(scaled, 80, 143, 2, &third_pitch);
	{
		int even_pitch = third_pitch;
		int candidate;

		for (candidate = even_pitch + 1; candidate >= even_pitch - 1;
		     candidate -= 2) {
			int32_t best = 0;
			int32_t selected = 0;
			unsigned i;

			for (i = 0; i < G729_FRAME; i += 2) {
				best = long_mac(best, scaled[i],
					scaled[(int)i - third_pitch]);
				selected = long_mac(selected, scaled[i],
					scaled[(int)i - candidate]);
			}
			if (selected > best)
				third_pitch = candidate;
		}
		third_value = open_pitch_region_get(scaled, third_pitch,
			third_pitch + 1, 1, &third_pitch);
	}
	if (word_absolute(word_subtract(word_shift_left((int16_t)second_pitch, 1),
		(int16_t)third_pitch)) < 5)
		second_value = word_add(second_value, word_shift_right(third_value, 2));
	if (word_absolute(word_subtract((int16_t)(3 * second_pitch),
		(int16_t)third_pitch)) < 7)
		second_value = word_add(second_value, word_shift_right(third_value, 2));
	if (word_absolute(word_subtract(word_shift_left((int16_t)first_pitch, 1),
		(int16_t)second_pitch)) < 5)
		first_value = word_add(first_value, word_multiply(second_value, 6554));
	if (word_absolute(word_subtract((int16_t)(3 * first_pitch),
		(int16_t)second_pitch)) < 7)
		first_value = word_add(first_value, word_multiply(second_value, 6554));
	if (first_value < second_value) {
		first_value = second_value;
		first_pitch = second_pitch;
	}
	if (first_value < third_value)
		first_pitch = third_pitch;
	return first_pitch;
}

static void backward_correlation_get(const int16_t *impulse,
				     const int16_t *target,
				     int16_t *correlation)
{
	int32_t full[G729_SUBFRAME];
	int32_t maximum = 0;
	int shift;
	unsigned i;

	for (i = 0; i < G729_SUBFRAME; i++) {
		int32_t total = 0;
		unsigned j;

		for (j = i; j < G729_SUBFRAME; j++)
			total = long_mac(total, target[j], impulse[j - i]);
		full[i] = total;
		if (long_absolute(total) > maximum)
			maximum = long_absolute(total);
	}
	shift = long_normalize(maximum);
	if (shift > 16)
		shift = 16;
	shift = 18 - shift;
	for (i = 0; i < G729_SUBFRAME; i++)
		correlation[i] = word_low_get(long_shift_right(full[i], shift));
}

static int closed_pitch_get(int16_t *excitation, const int16_t *target,
			    const int16_t *impulse, int minimum, int maximum,
			    bool second_subframe, int16_t *fraction)
{
	int16_t correlation[G729_SUBFRAME];
	int16_t best_vector[G729_SUBFRAME];
	int32_t best = INT32_MIN;
	int best_pitch = minimum;
	int pitch;

	backward_correlation_get(impulse, target, correlation);
	for (pitch = minimum; pitch <= maximum; pitch++) {
		int32_t value = 0;
		unsigned i;

		for (i = 0; i < G729_SUBFRAME; i++)
			value = long_mac(value, correlation[i],
				excitation[(int)i - pitch]);
		if (value <= best)
			continue;
		best = value;
		best_pitch = pitch;
	}
	adaptive_codebook_get(excitation, best_pitch, 0);
	best = 0;
	for (pitch = 0; pitch < (int)G729_SUBFRAME; pitch++)
		best = long_mac(best, correlation[pitch], excitation[pitch]);
	*fraction = 0;
	if (!second_subframe && best_pitch > 84)
		return best_pitch;
	memcpy(best_vector, excitation, sizeof(best_vector));
	for (pitch = -1; pitch <= 1; pitch += 2) {
		int32_t value = 0;
		unsigned i;

		adaptive_codebook_get(excitation, best_pitch, pitch);
		for (i = 0; i < G729_SUBFRAME; i++)
			value = long_mac(value, correlation[i], excitation[i]);
		if (value <= best)
			continue;
		best = value;
		*fraction = (int16_t)pitch;
		memcpy(best_vector, excitation, sizeof(best_vector));
	}
	memcpy(excitation, best_vector, sizeof(best_vector));
	return best_pitch;
}

static unsigned pitch_index_get(int pitch, int fraction, int *minimum,
				int *maximum, bool second_subframe)
{
	if (!second_subframe) {
		unsigned index = pitch <= 85 ? (unsigned)(3 * pitch - 58 + fraction) :
			(unsigned)(pitch + 112);

		*minimum = pitch - 5;
		if (*minimum < G729_PITCH_MIN)
			*minimum = G729_PITCH_MIN;
		*maximum = *minimum + 9;
		if (*maximum > G729_PITCH_MAX) {
			*maximum = G729_PITCH_MAX;
			*minimum = *maximum - 9;
		}
		return index;
	}
	return (unsigned)(3 * (pitch - *minimum) + 2 + fraction);
}

static int16_t pitch_gain_get(const int16_t *target, const int16_t *filtered,
			      int16_t *coefficients)
{
	int16_t scaled[G729_SUBFRAME];
	int16_t yy;
	int16_t xy;
	int yy_exponent;
	int xy_exponent;
	int32_t total;
	bool overflow = false;
	unsigned i;

	for (i = 0; i < G729_SUBFRAME; i++)
		scaled[i] = word_shift_right(filtered[i], 2);
	total = 1;
	for (i = 0; i < G729_SUBFRAME; i++)
		total = long_mac_track(total, filtered[i], filtered[i], &overflow);
	if (overflow) {
		total = 1;
		for (i = 0; i < G729_SUBFRAME; i++)
			total = long_mac(total, scaled[i], scaled[i]);
		yy_exponent = long_normalize(total) - 4;
		yy = long_round(long_shift_left(total, yy_exponent + 4));
	} else {
		yy_exponent = long_normalize(total);
		yy = long_round(long_shift_left(total, yy_exponent));
	}
	overflow = false;
	total = 0;
	for (i = 0; i < G729_SUBFRAME; i++)
		total = long_mac_track(total, target[i], filtered[i], &overflow);
	if (overflow) {
		total = 0;
		for (i = 0; i < G729_SUBFRAME; i++)
			total = long_mac(total, target[i], scaled[i]);
		xy_exponent = long_normalize(total) - 2;
		xy = long_round(long_shift_left(total, xy_exponent + 2));
	} else {
		xy_exponent = long_normalize(total);
		xy = long_round(long_shift_left(total, xy_exponent));
	}
	coefficients[0] = yy;
	coefficients[1] = (int16_t)(15 - yy_exponent);
	coefficients[2] = xy;
	coefficients[3] = (int16_t)(15 - xy_exponent);
	if (xy <= 0) {
		coefficients[3] = -15;
		return 0;
	}
	xy = word_shift_right(xy, 1);
	xy = fraction_divide(xy, yy);
	xy = word_shift_right(xy, xy_exponent - yy_exponent);
	return xy > 19661 ? 19661 : xy;
}

static void fixed_correlations_get(const int16_t *impulse,
				   int16_t correlation[G729_SUBFRAME][G729_SUBFRAME])
{
	int16_t scaled[G729_SUBFRAME];
	int32_t energy = 0;
	int shift;
	unsigned first;
	unsigned second;

	for (first = 0; first < G729_SUBFRAME; first++)
		energy = long_mac(energy, impulse[first], impulse[first]);
	if (word_high_get(energy) > 32000) {
		for (first = 0; first < G729_SUBFRAME; first++)
			scaled[first] = word_shift_right(impulse[first], 1);
	} else {
		shift = word_shift_right((int16_t)long_normalize(energy), 1);
		for (first = 0; first < G729_SUBFRAME; first++)
			scaled[first] = word_shift_left(impulse[first], shift);
	}
	for (first = 0; first < G729_SUBFRAME; first++) {
		for (second = first; second < G729_SUBFRAME; second++) {
			int32_t total = 0;
			unsigned sample;

			for (sample = second; sample < G729_SUBFRAME; sample++)
				total = long_mac(total, scaled[sample - first],
					scaled[sample - second]);
			correlation[first][second] = word_high_get(total);
			correlation[second][first] = word_high_get(total);
		}
	}
}

static int16_t fixed_pair_get(int16_t correlation[G729_SUBFRAME][G729_SUBFRAME],
			      const int16_t *signs, int first, int second)
{
	const int16_t *selected = signs;

	if (signs[first] < 0)
		return word_multiply(correlation[first][second],
			signs[second] < 0 ? INT16_MAX : INT16_MIN);
	return word_multiply(correlation[first][second], selected[second]);
}

static bool fixed_ratio_better(int16_t old_square, int16_t old_energy,
			       int16_t square, int16_t energy)
{
	int32_t comparison = long_multiply(old_energy, square);

	comparison = long_msu(comparison, old_square, energy);
	return comparison > 0;
}

static void fixed_phase_first(const int16_t *dn,
			      int16_t correlation[G729_SUBFRAME][G729_SUBFRAME],
			      const int16_t *signs, int first_track,
			      int second_track, int *first_position,
			      int *second_position, int16_t *sum_out,
			      int16_t *energy_out)
{
	int previous = -1;
	int16_t best_square = -1;
	int16_t best_energy = 1;
	unsigned pass;

	for (pass = 0; pass < 2; pass++) {
		int selected = first_track;
		int16_t maximum = -1;
		int position;

		for (position = first_track; position < (int)G729_SUBFRAME;
		     position += 5) {
			if (position == previous || dn[position] <= maximum)
				continue;
			maximum = dn[position];
			selected = position;
		}
		previous = selected;
		for (position = second_track; position < (int)G729_SUBFRAME;
		     position += 5) {
			int16_t sum = word_add(dn[selected], dn[position]);
			int32_t energy = long_multiply(correlation[selected][selected], 8192);
			int16_t square;
			int16_t energy16;

			energy = long_mac(energy,
				fixed_pair_get(correlation, signs, selected, position), 16384);
			energy = long_mac(energy, correlation[position][position], 8192);
			square = word_multiply(sum, sum);
			energy16 = long_round(energy);
			if (!fixed_ratio_better(best_square, best_energy,
				    square, energy16))
				continue;
			best_square = square;
			best_energy = energy16;
			*first_position = selected;
			*second_position = position;
			*sum_out = sum;
		}
	}
	*energy_out = best_energy;
}

static void fixed_phase_second(const int16_t *dn,
			       int16_t correlation[G729_SUBFRAME][G729_SUBFRAME],
			       const int16_t *signs, int fixed_first,
			       int fixed_second, int first_track,
			       int second_track, int16_t fixed_sum,
			       int16_t fixed_energy, int *first_position,
			       int *second_position, int16_t *square_out,
			       int16_t *energy_out)
{
	int16_t extra[G729_SUBFRAME];
	int16_t best_square = -1;
	int16_t best_energy = 1;
	int position;

	for (position = second_track; position < (int)G729_SUBFRAME;
	     position += 5) {
		int32_t total = long_multiply(
			fixed_pair_get(correlation, signs, fixed_first, position), 8192);

		total = long_mac(total,
			fixed_pair_get(correlation, signs, fixed_second, position), 8192);
		total = long_mac(total, correlation[position][position], 4096);
		extra[position] = long_round(total);
	}
	for (position = first_track; position < (int)G729_SUBFRAME;
	     position += 5) {
		int16_t partial_sum = word_add(fixed_sum, dn[position]);
		int32_t partial_energy = long_multiply(fixed_energy, 8192);
		int other;

		partial_energy = long_mac(partial_energy,
			fixed_pair_get(correlation, signs, fixed_first, position), 4096);
		partial_energy = long_mac(partial_energy,
			fixed_pair_get(correlation, signs, fixed_second, position), 4096);
		partial_energy = long_mac(partial_energy,
			correlation[position][position], 2048);
		for (other = second_track; other < (int)G729_SUBFRAME; other += 5) {
			int16_t sum = word_add(partial_sum, dn[other]);
			int32_t energy = long_mac(partial_energy,
				fixed_pair_get(correlation, signs, position, other), 4096);
			int16_t square;
			int16_t energy16;

			energy = long_mac(energy, extra[other], 16384);
			square = word_multiply(sum, sum);
			energy16 = long_round(energy);
			if (!fixed_ratio_better(best_square, best_energy,
				    square, energy16))
				continue;
			best_square = square;
			best_energy = energy16;
			*first_position = position;
			*second_position = other;
		}
	}
	*square_out = best_square;
	*energy_out = best_energy;
}

static unsigned fixed_codebook_search(const int16_t *target, int16_t *impulse,
				      int pitch, int16_t sharp, int16_t *code,
				      int16_t *filtered, uint16_t *sign_index)
{
	int16_t correlation[G729_SUBFRAME][G729_SUBFRAME];
	int16_t dn[G729_SUBFRAME];
	int16_t signs[G729_SUBFRAME];
	int best_position[4] = { 0, 1, 2, 3 };
	int16_t best_square = -1;
	int16_t best_energy = 1;
	int track;
	unsigned i;

	sharp = word_shift_left(sharp, 1);
	if (pitch < (int)G729_SUBFRAME) {
		for (i = (unsigned)pitch; i < G729_SUBFRAME; i++)
			impulse[i] = word_add(impulse[i],
				word_multiply(impulse[i - pitch], sharp));
	}
	fixed_correlations_get(impulse, correlation);
	backward_correlation_get(impulse, target, dn);
	for (i = 0; i < G729_SUBFRAME; i++) {
		if (dn[i] >= 0) {
			signs[i] = INT16_MAX;
		} else {
			signs[i] = INT16_MIN;
			dn[i] = word_subtract(0, dn[i]);
		}
	}
	for (track = 3; track <= 4; track++) {
		int fixed_first;
		int fixed_second;
		int free_first;
		int free_second;
		int16_t fixed_sum;
		int16_t fixed_energy;
		int16_t square;
		int16_t energy;

		fixed_phase_first(dn, correlation, signs, 2, track,
			&fixed_first, &fixed_second, &fixed_sum, &fixed_energy);
		fixed_phase_second(dn, correlation, signs, fixed_first, fixed_second,
			0, 1, fixed_sum, fixed_energy, &free_first, &free_second,
			&square, &energy);
		if (fixed_ratio_better(best_square, best_energy, square, energy)) {
			best_square = square;
			best_energy = energy;
			best_position[0] = free_first;
			best_position[1] = free_second;
			best_position[2] = fixed_first;
			best_position[3] = fixed_second;
		}
		fixed_phase_first(dn, correlation, signs, track, 0,
			&fixed_first, &fixed_second, &fixed_sum, &fixed_energy);
		fixed_phase_second(dn, correlation, signs, fixed_first, fixed_second,
			1, 2, fixed_sum, fixed_energy, &free_first, &free_second,
			&square, &energy);
		if (fixed_ratio_better(best_square, best_energy, square, energy)) {
			best_square = square;
			best_energy = energy;
			best_position[0] = fixed_second;
			best_position[1] = free_first;
			best_position[2] = free_second;
			best_position[3] = fixed_first;
		}
	}
	memset(code, 0, G729_SUBFRAME * sizeof(*code));
	memset(filtered, 0, G729_SUBFRAME * sizeof(*filtered));
	*sign_index = 0;
	for (i = 0; i < 4; i++) {
		int position = best_position[i];
		int16_t pulse = word_shift_right(signs[position], 2);
		unsigned sample;

		code[position] = pulse;
		if (pulse > 0)
			*sign_index |= (uint16_t)(1U << i);
		for (sample = (unsigned)position; sample < G729_SUBFRAME; sample++) {
			if (pulse > 0)
				filtered[sample] = word_add(filtered[sample],
					impulse[sample - position]);
			else
				filtered[sample] = word_subtract(filtered[sample],
					impulse[sample - position]);
		}
	}
	if (pitch < (int)G729_SUBFRAME) {
		for (i = (unsigned)pitch; i < G729_SUBFRAME; i++)
			code[i] = word_add(code[i], word_multiply(code[i - pitch], sharp));
	}
	{
		unsigned fourth = (unsigned)best_position[3] / 5;
		unsigned fourth_track = (unsigned)best_position[3] % 5 - 3;

		return (unsigned)best_position[0] / 5 |
			((unsigned)best_position[1] / 5 << 3) |
			((unsigned)best_position[2] / 5 << 6) |
			((fourth * 2 + fourth_track) << 9);
	}
}

static void gain_correlations_get(const int16_t *target,
				  const int16_t *adaptive,
				  const int16_t *fixed,
				  const int16_t *pitch_coefficients,
				  int16_t *coefficients, int16_t *exponents)
{
	int16_t scaled_fixed[G729_SUBFRAME];
	int32_t total;
	int normalization;
	unsigned i;

	coefficients[0] = pitch_coefficients[0];
	exponents[0] = word_subtract(0, pitch_coefficients[1]);
	coefficients[1] = word_subtract(0, pitch_coefficients[2]);
	exponents[1] = word_subtract(0, word_add(pitch_coefficients[3], 1));
	for (i = 0; i < G729_SUBFRAME; i++)
		scaled_fixed[i] = word_shift_right(fixed[i], 3);
	total = 1;
	for (i = 0; i < G729_SUBFRAME; i++)
		total = long_mac(total, scaled_fixed[i], scaled_fixed[i]);
	normalization = long_normalize(total);
	coefficients[2] = long_round(long_shift_left(total, normalization));
	exponents[2] = (int16_t)(normalization + 3);
	total = 1;
	for (i = 0; i < G729_SUBFRAME; i++)
		total = long_mac(total, target[i], scaled_fixed[i]);
	normalization = long_normalize(total);
	coefficients[3] = word_subtract(0,
		long_round(long_shift_left(total, normalization)));
	exponents[3] = (int16_t)(normalization - 7);
	total = 1;
	for (i = 0; i < G729_SUBFRAME; i++)
		total = long_mac(total, adaptive[i], scaled_fixed[i]);
	normalization = long_normalize(total);
	coefficients[4] = long_round(long_shift_left(total, normalization));
	exponents[4] = (int16_t)(normalization - 7);
}

static void encoder_gain_predict(const struct g729_encoder *encoder,
				 const int16_t *code, int16_t *gain,
				 int16_t *gain_exponent)
{
	int32_t energy = 0;
	int32_t prediction;
	int16_t exponent;
	int16_t fraction;
	int16_t high;
	int16_t low;
	unsigned i;

	for (i = 0; i < G729_SUBFRAME; i++)
		energy = long_mac(energy, code[i], code[i]);
	log2_get(energy, &exponent, &fraction);
	prediction = long_multiply_dpf(exponent, fraction, -24660);
	prediction = long_mac(prediction, 32588, 32);
	prediction = long_shift_left(prediction, 10);
	for (i = 0; i < 4; i++)
		prediction = long_mac(prediction, g729_gain_predictor[i],
			encoder->gain_energy[i]);
	*gain = word_high_get(prediction);
	prediction = long_shift_right(long_multiply(*gain, 5439), 8);
	long_extract(prediction, &high, &low);
	*gain = word_low_get(power2_get(14, low));
	*gain_exponent = word_subtract(14, high);
}

static void encoder_gain_energy_update(struct g729_encoder *encoder,
				       int32_t factor)
{
	int16_t exponent;
	int16_t fraction;
	int16_t normalized;
	unsigned i;

	for (i = 3; i > 0; i--)
		encoder->gain_energy[i] = encoder->gain_energy[i - 1];
	log2_get(factor, &exponent, &fraction);
	normalized = word_high_get(long_shift_left(long_add(
		(int32_t)(exponent - 13) * 65536,
		(int32_t)fraction * 2), 13));
	encoder->gain_energy[0] = word_multiply(normalized, 24660);
}

static void gain_candidates_get(const int16_t *coefficients,
				const int16_t *exponents, int16_t predicted,
				int predicted_exponent, bool tame,
				unsigned *first_start, unsigned *second_start)
{
	static const int16_t first_threshold[4] = {
		10808, 12374, 19778, 32567,
	};
	static const int16_t second_threshold[8] = {
		14087, 16188, 20274, 21321, 23525, 25232, 27873, 30542,
	};
	int16_t best_gain[2];
	int16_t denominator;
	int16_t inverse_denominator;
	int denominator_exponent;
	int inverse_exponent;
	int16_t numerator;
	int numerator_exponent;
	int exponent;
	int shift;
	int32_t first_value;
	int32_t second_value;
	int32_t value;
	int16_t predicted_q4;

	first_value = long_multiply(coefficients[0], coefficients[2]);
	exponent = exponents[0] + exponents[2] - 1;
	second_value = long_multiply(coefficients[4], coefficients[4]);
	{
		int second_exponent = 2 * exponents[4] + 1;

		if (exponent > second_exponent) {
			value = long_subtract(long_shift_right(first_value,
				exponent - second_exponent), second_value);
			exponent = second_exponent;
		} else {
			value = long_subtract(first_value, long_shift_right(second_value,
				second_exponent - exponent));
		}
	}
	shift = long_normalize(value);
	denominator = word_high_get(long_shift_left(value, shift));
	denominator_exponent = exponent + shift - 16;
	inverse_denominator = word_subtract(0,
		fraction_divide(16384, denominator));
	inverse_exponent = 29 - denominator_exponent;
	first_value = long_multiply(coefficients[2], coefficients[1]);
	{
		int first_exponent = exponents[2] + exponents[1];
		int second_exponent = exponents[3] + exponents[4] + 1;

		second_value = long_multiply(coefficients[3], coefficients[4]);
		if (first_exponent > second_exponent) {
			value = long_subtract(long_shift_right(first_value,
				first_exponent - second_exponent + 1),
				long_shift_right(second_value, 1));
			exponent = second_exponent - 1;
		} else {
			value = long_subtract(long_shift_right(first_value, 1),
				long_shift_right(second_value,
					second_exponent - first_exponent + 1));
			exponent = first_exponent - 1;
		}
	}
	shift = long_normalize(value);
	numerator = word_high_get(long_shift_left(value, shift));
	numerator_exponent = exponent + shift - 16;
	shift = numerator_exponent + inverse_exponent - 24;
	value = long_shift_right(long_multiply(numerator, inverse_denominator),
		shift);
	best_gain[0] = word_high_get(value);
	if (tame && best_gain[0] > 481)
		best_gain[0] = 481;
	first_value = long_multiply(coefficients[0], coefficients[3]);
	{
		int first_exponent = exponents[0] + exponents[3];
		int second_exponent = exponents[1] + exponents[4] + 1;

		second_value = long_multiply(coefficients[1], coefficients[4]);
		if (first_exponent > second_exponent) {
			value = long_subtract(long_shift_right(first_value,
				first_exponent - second_exponent + 1),
				long_shift_right(second_value, 1));
			exponent = second_exponent - 1;
		} else {
			value = long_subtract(long_shift_right(first_value, 1),
				long_shift_right(second_value,
					second_exponent - first_exponent + 1));
			exponent = first_exponent - 1;
		}
	}
	shift = long_normalize(value);
	numerator = word_high_get(long_shift_left(value, shift));
	numerator_exponent = exponent + shift - 16;
	shift = numerator_exponent + inverse_exponent - 17;
	value = long_shift_right(long_multiply(numerator, inverse_denominator),
		shift);
	best_gain[1] = word_high_get(value);
	if (predicted_exponent >= 4) {
		predicted_q4 = word_shift_right(predicted, predicted_exponent - 4);
	} else {
		value = long_shift_left((int32_t)predicted,
			20 - predicted_exponent);
		predicted_q4 = word_high_get(value);
	}
	{
		int16_t accumulator_high;
		int32_t code_x;
		int32_t code_y;
		int32_t combined = long_multiply(31881, best_gain[0]);

		combined = long_add(combined, long_shift_right(INT32_C(1822990272), 15));
		accumulator_high = word_high_get(combined);
		value = long_multiply(accumulator_high, predicted_q4);
		combined = long_shift_left((int32_t)best_gain[1], 7);
		combined = long_subtract(combined, value);
		accumulator_high = word_high_get(long_shift_left(combined, 2));
		code_x = long_multiply(accumulator_high, -17103);
		combined = long_subtract(long_multiply(31881, best_gain[0]),
			long_shift_right(INT32_C(1731217536), 10));
		accumulator_high = word_high_get(combined);
		accumulator_high = word_multiply(accumulator_high, predicted_q4);
		value = long_multiply(accumulator_high, 31548);
		combined = long_multiply(31881, best_gain[1]);
		value = long_subtract(value, long_shift_right(combined, 3));
		accumulator_high = word_high_get(long_shift_left(value, 2));
		code_y = long_multiply(accumulator_high, -17103);
		*first_start = 0;
		while (*first_start < 4 && code_y > long_shift_right(
			long_multiply(first_threshold[*first_start], predicted_q4), 3))
			(*first_start)++;
		*second_start = 0;
		while (*second_start < 8 && code_x > long_shift_right(
			long_multiply(second_threshold[*second_start], predicted_q4), 5))
			(*second_start)++;
	}
}

static unsigned gains_quantize(struct g729_encoder *encoder,
			       const int16_t *code, const int16_t *coefficients,
			       const int16_t *exponents, bool tame,
			       int16_t *pitch_gain, int16_t *code_gain)
{
	int16_t aligned_high[5];
	int16_t aligned_low[5];
	int16_t predicted;
	int16_t predicted_exponent;
	int minimum_exponent;
	int term_exponent[5];
	int32_t best_distance = INT32_MAX;
	unsigned best_first = 0;
	unsigned best_second = 0;
	unsigned first_start;
	unsigned second_start;
	unsigned first;

	encoder_gain_predict(encoder, code, &predicted, &predicted_exponent);
	gain_candidates_get(coefficients, exponents, predicted, predicted_exponent,
		tame, &first_start, &second_start);
	term_exponent[0] = exponents[0] + 13;
	term_exponent[1] = exponents[1] + 14;
	term_exponent[2] = exponents[2] + 2 * predicted_exponent - 21;
	term_exponent[3] = exponents[3] + predicted_exponent - 3;
	term_exponent[4] = exponents[4] + predicted_exponent - 4;
	minimum_exponent = term_exponent[0];
	for (first = 1; first < 5; first++) {
		if (term_exponent[first] < minimum_exponent)
			minimum_exponent = term_exponent[first];
	}
	for (first = 0; first < 5; first++) {
		int32_t aligned = (int32_t)coefficients[first] * 65536;

		aligned = long_shift_right(aligned,
			term_exponent[first] - minimum_exponent);
		long_extract(aligned, &aligned_high[first], &aligned_low[first]);
	}
	for (first = first_start; first < first_start + 4; first++) {
		unsigned second;

		for (second = second_start; second < second_start + 8; second++) {
			int16_t candidate_pitch = word_add(g729_gain_stage1[first][0],
				g729_gain_stage2[second][0]);
			int32_t correction_total = (int32_t)g729_gain_stage1[first][1] +
				g729_gain_stage2[second][1];
			int16_t correction = word_low_get(long_shift_right(
				correction_total, 1));
			int16_t candidate_code = word_multiply(predicted, correction);
			int16_t pitch_square;
			int16_t code_square;
			int16_t cross;
			int32_t distance;

			if (tame && candidate_pitch >= 16383)
				continue;
			pitch_square = word_multiply(candidate_pitch, candidate_pitch);
			code_square = word_multiply(candidate_code, candidate_code);
			cross = word_multiply(candidate_code, candidate_pitch);
			distance = long_multiply_dpf(aligned_high[0], aligned_low[0],
				pitch_square);
			distance = long_add(distance, long_multiply_dpf(aligned_high[1],
				aligned_low[1], candidate_pitch));
			distance = long_add(distance, long_multiply_dpf(aligned_high[2],
				aligned_low[2], code_square));
			distance = long_add(distance, long_multiply_dpf(aligned_high[3],
				aligned_low[3], candidate_code));
			distance = long_add(distance, long_multiply_dpf(aligned_high[4],
				aligned_low[4], cross));
			if (distance >= best_distance)
				continue;
			best_distance = distance;
			best_first = first;
			best_second = second;
		}
	}
	*pitch_gain = word_add(g729_gain_stage1[best_first][0],
		g729_gain_stage2[best_second][0]);
	{
		int32_t correction = (int32_t)g729_gain_stage1[best_first][1] +
			g729_gain_stage2[best_second][1];
		int16_t half = word_low_get(long_shift_right(correction, 1));
		int32_t product = long_multiply(half, predicted);

		product = long_shift_left(product, 4 - predicted_exponent);
		*code_gain = word_high_get(product);
		encoder_gain_energy_update(encoder, correction);
	}
	return (unsigned)(g729_gain_map1[best_first] * 16 +
		g729_gain_map2[best_second]);
}

static void pitch_postfilter(const int16_t *signal, const int16_t *scaled,
			     int minimum, int maximum, int16_t *output)
{
	int32_t best_correlation = INT32_MIN;
	int best_pitch = minimum;
	int pitch;
	int16_t correlation;
	int16_t delayed_energy;
	int16_t current_energy;
	int16_t direct_gain;
	int16_t delayed_gain;
	unsigned sample;

	for (pitch = minimum; pitch <= maximum; pitch++) {
		int32_t total = 0;

		for (sample = 0; sample < G729_SUBFRAME; sample++)
			total = long_mac(total, scaled[sample], scaled[(int)sample - pitch]);
		if (total <= best_correlation)
			continue;
		best_correlation = total;
		best_pitch = pitch;
	}
	{
		int32_t delayed_total = 1;
		int32_t current_total = 1;
		int32_t scale_source;
		int normalization;

		for (sample = 0; sample < G729_SUBFRAME; sample++) {
			delayed_total = long_mac(delayed_total,
				scaled[(int)sample - best_pitch],
				scaled[(int)sample - best_pitch]);
			current_total = long_mac(current_total, scaled[sample],
				scaled[sample]);
		}
		if (best_correlation < 0)
			best_correlation = 0;
		scale_source = best_correlation;
		if (delayed_total > scale_source)
			scale_source = delayed_total;
		if (current_total > scale_source)
			scale_source = current_total;
		normalization = long_normalize(scale_source);
		correlation = long_round(long_shift_left(best_correlation, normalization));
		delayed_energy = long_round(long_shift_left(delayed_total, normalization));
		current_energy = long_round(long_shift_left(current_total, normalization));
	}
	{
		int32_t threshold = long_subtract(long_multiply(correlation, correlation),
			long_shift_right(long_multiply(delayed_energy, current_energy), 1));

		if (threshold < 0) {
			memcpy(output, signal, G729_SUBFRAME * sizeof(*output));
			return;
		}
	}
	if (correlation > delayed_energy) {
		direct_gain = 21845;
		delayed_gain = 10923;
	} else {
		int16_t numerator = word_shift_right(word_multiply(correlation, 16384), 1);
		int16_t denominator = word_add(numerator,
			word_shift_right(delayed_energy, 1));

		if (denominator > 0) {
			delayed_gain = fraction_divide(numerator, denominator);
			direct_gain = word_subtract(32767, delayed_gain);
		} else {
			direct_gain = 32767;
			delayed_gain = 0;
		}
	}
	for (sample = 0; sample < G729_SUBFRAME; sample++)
		output[sample] = word_add(word_multiply(direct_gain, signal[sample]),
			word_multiply(delayed_gain, signal[(int)sample - best_pitch]));
}

static void preemphasis_apply(struct g729_decoder *decoder, int16_t *signal,
			      int16_t factor)
{
	int16_t last = signal[G729_SUBFRAME - 1];
	int sample;

	for (sample = G729_SUBFRAME - 1; sample > 0; sample--)
		signal[sample] = word_subtract(signal[sample],
			word_multiply(factor, signal[sample - 1]));
	signal[0] = word_subtract(signal[0],
		word_multiply(factor, decoder->post_preemphasis_memory));
	decoder->post_preemphasis_memory = last;
}

static void gain_control_apply(struct g729_decoder *decoder,
			       const int16_t *input, int16_t *output)
{
	int32_t output_energy = 0;
	int32_t input_energy = 0;
	int16_t target = 0;
	int output_exponent;
	unsigned sample;

	for (sample = 0; sample < G729_SUBFRAME; sample++) {
		int16_t scaled = word_shift_right(output[sample], 2);

		output_energy = long_mac(output_energy, scaled, scaled);
	}
	if (!output_energy) {
		decoder->post_gain = 0;
		return;
	}
	output_exponent = long_normalize(output_energy) - 1;
	{
		int16_t output_gain = long_round(long_shift_left(output_energy,
			output_exponent));

		for (sample = 0; sample < G729_SUBFRAME; sample++) {
			int16_t scaled = word_shift_right(input[sample], 2);

			input_energy = long_mac(input_energy, scaled, scaled);
		}
		if (input_energy) {
			int input_exponent = long_normalize(input_energy);
			int16_t input_gain = long_round(long_shift_left(input_energy,
				input_exponent));
			int32_t ratio = fraction_divide(output_gain, input_gain);

			ratio = long_shift_left(ratio, 7);
			ratio = long_shift_right(ratio, output_exponent - input_exponent);
			ratio = inverse_sqrt_get(ratio);
			target = word_multiply(long_round(long_shift_left(ratio, 9)), 3276);
		}
	}
	for (sample = 0; sample < G729_SUBFRAME; sample++) {
		decoder->post_gain = word_add(word_multiply(decoder->post_gain, 29491),
			target);
		output[sample] = word_high_get(long_shift_left(
			long_multiply(output[sample], decoder->post_gain), 3));
	}
}

static void high_pass_apply(struct g729_decoder *decoder, int16_t *signal)
{
	unsigned sample;

	for (sample = 0; sample < G729_FRAME; sample++) {
		int16_t oldest_x = decoder->high_pass_x[1];
		int32_t total;

		decoder->high_pass_x[1] = decoder->high_pass_x[0];
		decoder->high_pass_x[0] = signal[sample];
		total = long_multiply_dpf(decoder->high_pass_y_high[0],
			decoder->high_pass_y_low[0], g729_high_pass_a[1]);
		total = long_add(total, long_multiply_dpf(
			decoder->high_pass_y_high[1], decoder->high_pass_y_low[1],
			g729_high_pass_a[2]));
		total = long_mac(total, decoder->high_pass_x[0], g729_high_pass_b[0]);
		total = long_mac(total, decoder->high_pass_x[1], g729_high_pass_b[1]);
		total = long_mac(total, oldest_x, g729_high_pass_b[2]);
		total = long_shift_left(total, 2);
		signal[sample] = long_round(long_shift_left(total, 1));
		decoder->high_pass_y_high[1] = decoder->high_pass_y_high[0];
		decoder->high_pass_y_low[1] = decoder->high_pass_y_low[0];
		long_extract(total, &decoder->high_pass_y_high[0],
			&decoder->high_pass_y_low[0]);
	}
}

/* On a comfort-noise frame the normative C code bypasses the clause 4.2.1
 * long-term postfilter; active selects that path. */
static void postfilter_apply(struct g729_decoder *decoder, int16_t *speech,
			     int16_t lpc[2][G729_LPC_ORDER + 1],
			     const int16_t *pitch, bool active)
{
	int16_t speech_history[G729_LPC_ORDER + G729_FRAME];
	int16_t filtered[G729_FRAME];
	unsigned subframe;

	memcpy(speech_history, decoder->post_speech_history,
	       sizeof(decoder->post_speech_history));
	memcpy(speech_history + G729_LPC_ORDER, speech,
	       G729_FRAME * sizeof(*speech));
	for (subframe = 0; subframe < 2; subframe++) {
		int16_t numerator[G729_LPC_ORDER + 1];
		int16_t denominator[G729_LPC_ORDER + 1];
		int16_t impulse[22] = { 0 };
		int16_t impulse_memory[G729_LPC_ORDER] = { 0 };
		int16_t residual_filtered[G729_SUBFRAME];
		int16_t *residual = decoder->post_residual + G729_PITCH_MAX;
		int16_t *scaled = decoder->post_residual_scaled + G729_PITCH_MAX;
		int minimum = pitch[subframe] - 3;
		int maximum = minimum + 6;
		int16_t correlation_zero;
		int16_t correlation_one;
		int16_t tilt = 0;
		unsigned sample;

		if (maximum > G729_PITCH_MAX) {
			maximum = G729_PITCH_MAX;
			minimum = maximum - 6;
		}
		lpc_weight(lpc[subframe], 18022, numerator);
		lpc_weight(lpc[subframe], 22938, denominator);
		residual_filter(numerator,
			speech_history + G729_LPC_ORDER + subframe * G729_SUBFRAME,
			residual);
		for (sample = 0; sample < G729_SUBFRAME; sample++)
			scaled[sample] = word_shift_right(residual[sample], 2);
		if (active)
			pitch_postfilter(residual, scaled, minimum, maximum,
				residual_filtered);
		else
			memcpy(residual_filtered, residual,
				sizeof(residual_filtered));
		memcpy(impulse, numerator, sizeof(numerator));
		synthesis_filter_plain(denominator, impulse, impulse, 22,
			impulse_memory);
		{
			int32_t zero = long_multiply(impulse[0], impulse[0]);
			int32_t one = long_multiply(impulse[0], impulse[1]);

			for (sample = 1; sample < 22; sample++)
				zero = long_mac(zero, impulse[sample], impulse[sample]);
			for (sample = 1; sample + 1 < 22; sample++)
				one = long_mac(one, impulse[sample], impulse[sample + 1]);
			correlation_zero = word_high_get(zero);
			correlation_one = word_high_get(one);
		}
		if (correlation_one > 0) {
			correlation_one = word_multiply(correlation_one, 26214);
			tilt = fraction_divide(correlation_one, correlation_zero);
		}
		preemphasis_apply(decoder, residual_filtered, tilt);
		synthesis_filter_plain(denominator, residual_filtered,
			filtered + subframe * G729_SUBFRAME, G729_SUBFRAME,
			decoder->post_synthesis_memory);
		gain_control_apply(decoder,
			speech + subframe * G729_SUBFRAME,
			filtered + subframe * G729_SUBFRAME);
		memmove(decoder->post_residual, decoder->post_residual + G729_SUBFRAME,
			G729_PITCH_MAX * sizeof(decoder->post_residual[0]));
		memmove(decoder->post_residual_scaled,
			decoder->post_residual_scaled + G729_SUBFRAME,
			G729_PITCH_MAX * sizeof(decoder->post_residual_scaled[0]));
	}
	memcpy(decoder->post_speech_history,
	       speech + G729_FRAME - G729_LPC_ORDER,
	       sizeof(decoder->post_speech_history));
	memcpy(speech, filtered, sizeof(filtered));
	high_pass_apply(decoder, speech);
}

static bool excitation_tame_get(const struct g729_encoder *encoder, int pitch,
				int fraction)
{
	int adjusted = pitch + (fraction > 0 ? 1 : 0);
	int first_index = adjusted - 50;
	int first_zone;
	int last_zone;
	int32_t maximum = -1;
	int zone;

	if (first_index < 0)
		first_index = 0;
	first_zone = first_index / 40;
	last_zone = (adjusted + 8) / 40;
	if (last_zone > 3)
		last_zone = 3;
	for (zone = first_zone; zone <= last_zone; zone++) {
		if (encoder->excitation_error[zone] > maximum)
			maximum = encoder->excitation_error[zone];
	}
	return maximum > 983040000;
}

static void excitation_error_update(struct g729_encoder *encoder,
				    int16_t pitch_gain, int pitch)
{
	int32_t worst = -1;
	int start = pitch - G729_SUBFRAME;
	int end;
	int zone;

	if (start < 0) {
		int16_t high;
		int16_t low;
		int32_t value;

		long_extract(encoder->excitation_error[0], &high, &low);
		value = long_shift_left(long_multiply_dpf(high, low, pitch_gain), 1);
		value = long_add(16384, value);
		worst = value;
		long_extract(value, &high, &low);
		value = long_shift_left(long_multiply_dpf(high, low, pitch_gain), 1);
		value = long_add(16384, value);
		if (value > worst)
			worst = value;
	} else {
		start /= 40;
		end = (pitch - 1) / 40;
		for (zone = start; zone <= end; zone++) {
			int16_t high;
			int16_t low;
			int32_t value;

			long_extract(encoder->excitation_error[zone], &high, &low);
			value = long_shift_left(long_multiply_dpf(high, low, pitch_gain), 1);
			value = long_add(16384, value);
			if (value > worst)
				worst = value;
		}
	}
	for (zone = 3; zone > 0; zone--)
		encoder->excitation_error[zone] = encoder->excitation_error[zone - 1];
	encoder->excitation_error[0] = worst;
}

/*
 * Annex B clause B.3, the voice activity detection. The section follows the
 * fixed-point arithmetic of the normative C code operation by operation,
 * because the clause B.4 DTX it feeds is bit-exact. The decision is stored
 * in the encoder state; an instance created with dtx acts on it, and without
 * dtx the encoder ignores it.
 */

/* The Q15 conversion the VAD consumes, distinct from lsp_to_lsf(), which
 * yields the Q13 radian scale the quantizer consumes. */
static void vad_lsf_get(const int16_t *lsp, int16_t *lsf)
{
	int index = 63;
	int i;

	for (i = G729_LPC_ORDER - 1; i >= 0; i--) {
		int16_t offset;
		int16_t interpolated;
		int32_t product;

		while (g729_cos_table[index] < lsp[i])
			index--;
		offset = word_subtract(lsp[i], g729_cos_table[index]);
		product = long_multiply(g729_acos_slope[index], offset);
		interpolated = long_round(long_shift_left(product, 3));
		lsf[i] = word_add(interpolated,
			word_shift_left((int16_t)index, 8));
	}
}

static int16_t vad_log_energy_get(int32_t value, int16_t exponent)
{
	int16_t log_exponent;
	int16_t log_fraction;
	int32_t total;

	log2_get(value, &log_exponent, &log_fraction);
	total = long_multiply_dpf(log_exponent, log_fraction, 9864);
	total = long_mac(total, 9864, (int16_t)(exponent - 2));
	total = long_shift_left(total, 11);
	return word_subtract(word_high_get(total), 4875);
}

/* Clause B.3.3, the long-term minimum energy over segments of eight frames. */
static void vad_minimum_update(struct g729_vad *vad, int16_t energy)
{
	unsigned i;

	if (vad->frame_count < 129) {
		if (energy < vad->minimum) {
			vad->minimum = energy;
			vad->minimum_previous = energy;
		}
		if (!(vad->frame_count & 7)) {
			vad->minimum_buffer[(vad->frame_count >> 3) - 1] =
				vad->minimum;
			vad->minimum = INT16_MAX;
		}
	}
	if (!(vad->frame_count & 7)) {
		vad->minimum_previous = vad->minimum_buffer[0];
		for (i = 1; i < 16; i++) {
			if (vad->minimum_buffer[i] < vad->minimum_previous)
				vad->minimum_previous = vad->minimum_buffer[i];
		}
	}
	if (vad->frame_count < 129)
		return;
	if ((vad->frame_count & 7) == 1) {
		vad->minimum = vad->minimum_previous;
		vad->minimum_next = INT16_MAX;
	}
	if (energy < vad->minimum)
		vad->minimum = energy;
	if (energy < vad->minimum_next)
		vad->minimum_next = energy;
	if (vad->frame_count & 7)
		return;
	for (i = 0; i < 15; i++)
		vad->minimum_buffer[i] = vad->minimum_buffer[i + 1];
	vad->minimum_buffer[15] = vad->minimum_next;
	vad->minimum_previous = vad->minimum_buffer[0];
	for (i = 1; i < 16; i++) {
		if (vad->minimum_buffer[i] < vad->minimum_previous)
			vad->minimum_previous = vad->minimum_buffer[i];
	}
}

/* Clause B.3.2, accumulation of the noise averages over the first frames. */
static bool vad_average_init(struct g729_vad *vad, int16_t energy,
			     int16_t zero_cross, const int16_t *lsf)
{
	unsigned i;

	if (energy < 3072) {
		vad->quiet_count++;
		return false;
	}
	vad->energy_average = word_high_get(long_mac(
		(int32_t)vad->energy_average * 65536, energy, 1024));
	vad->noise_zero_cross = word_high_get(long_mac(
		(int32_t)vad->noise_zero_cross * 65536, zero_cross, 1024));
	for (i = 0; i < G729_LPC_ORDER; i++)
		vad->noise_lsf[i] = word_high_get(long_mac(
			(int32_t)vad->noise_lsf[i] * 65536, lsf[i], 1024));
	return true;
}

static void vad_average_scale(struct g729_vad *vad)
{
	int16_t factor = g729_vad_init_factor[vad->quiet_count];
	int16_t shift = g729_vad_init_shift[vad->quiet_count];
	unsigned i;

	vad->energy_average = word_high_get(long_shift_left(
		long_multiply(vad->energy_average, factor), shift));
	vad->noise_zero_cross = word_high_get(long_shift_left(
		long_multiply(vad->noise_zero_cross, factor), shift));
	for (i = 0; i < G729_LPC_ORDER; i++)
		vad->noise_lsf[i] = word_high_get(long_shift_left(
			long_multiply(vad->noise_lsf[i], factor), shift));
	/* Clause B.3.2 selects one of three K offset pairs by the T1 and T2
	 * energy thresholds. The normative C code sets both averages with one
	 * fixed pair, and clause B.5 gives that code precedence. */
	vad->noise_energy = word_subtract(vad->energy_average, 2048);
	vad->noise_energy_low = word_subtract(vad->energy_average, 2458);
}

/* Clause B.3.5, the fourteen boundary decisions, in the clause's order.
 * Boundaries 8 to 10 use the full-band difference, as the clause text and
 * the arithmetic of the normative C code do. A heading comment in that code
 * calls them low-band tests. */
static bool vad_boundaries_check(int16_t low_diff, int16_t full_diff,
				 int16_t distortion, int16_t cross_diff)
{
	int32_t value;

	value = long_multiply(cross_diff, -14680);
	value = long_mac(value, 8192, -28521);
	value = long_shift_right(value, 8);
	value = long_add(value, (int32_t)distortion * 65536);
	if (value > 0)
		return true;
	value = long_multiply(cross_diff, 19065);
	value = long_mac(value, 8192, -19446);
	value = long_shift_right(value, 7);
	value = long_add(value, (int32_t)distortion * 65536);
	if (value > 0)
		return true;
	value = long_multiply(cross_diff, 20480);
	value = long_mac(value, 8192, 16384);
	value = long_shift_right(value, 2);
	value = long_add(value, (int32_t)full_diff * 65536);
	if (value < 0)
		return true;
	value = long_multiply(cross_diff, -16384);
	value = long_mac(value, 8192, 19660);
	value = long_shift_right(value, 2);
	value = long_add(value, (int32_t)full_diff * 65536);
	if (value < 0)
		return true;
	value = long_multiply(full_diff, 32767);
	value = long_mac(value, 1024, 30802);
	if (value < 0)
		return true;
	value = long_multiply(distortion, -28160);
	value = long_mac(value, 64, 19988);
	value = long_mac(value, full_diff, 512);
	if (value < 0)
		return true;
	value = long_multiply(distortion, 32767);
	value = long_mac(value, 32, -30199);
	if (value > 0)
		return true;
	value = long_multiply(cross_diff, -20480);
	value = long_mac(value, 8192, 22938);
	value = long_shift_right(value, 2);
	value = long_add(value, (int32_t)full_diff * 65536);
	if (value < 0)
		return true;
	value = long_multiply(cross_diff, 23831);
	value = long_mac(value, 4096, 31576);
	value = long_shift_right(value, 2);
	value = long_add(value, (int32_t)full_diff * 65536);
	if (value < 0)
		return true;
	value = long_multiply(full_diff, 32767);
	value = long_mac(value, 2048, 17367);
	if (value < 0)
		return true;
	value = long_multiply(distortion, -22400);
	value = long_mac(value, 32, 25395);
	value = long_mac(value, low_diff, 256);
	if (value < 0)
		return true;
	value = long_multiply(full_diff, -30427);
	value = long_mac(value, 256, -29959);
	value = long_add(value, (int32_t)low_diff * 65536);
	if (value > 0)
		return true;
	value = long_multiply(full_diff, -23406);
	value = long_mac(value, 512, 28087);
	value = long_add(value, (int32_t)low_diff * 65536);
	if (value < 0)
		return true;
	value = long_multiply(full_diff, 24576);
	value = long_mac(value, 1024, 29491);
	value = long_mac(value, low_diff, 16384);
	if (value < 0)
		return true;
	return false;
}

/* Clause B.3.6 stage two, the bounded frame extension. */
static bool vad_extend_check(struct g729_vad *vad, bool decision,
			     int16_t energy)
{
	if (!vad->decision_earlier || !vad->decision_previous || decision)
		return decision;
	if (word_absolute(word_subtract(vad->energy_previous, energy)) > 614)
		return decision;
	vad->extend_count++;
	vad->hangover_flag = 1;
	if (vad->extend_count <= 4) {
		vad->extend_flag = 1;
	} else {
		vad->extend_count = 0;
		vad->extend_flag = 0;
	}
	return true;
}

/* Clause B.3.7, the auto-regressive update of the noise averages. */
static void vad_noise_update(struct g729_vad *vad, int16_t energy,
			     int16_t energy_low, int16_t zero_cross,
			     const int16_t *lsf)
{
	static const int16_t weights[6][6] = {
		{ 24576, 8192, 26214, 6554, 19661, 13017 },
		{ 31130, 1638, 30147, 2621, 21299, 11469 },
		{ 31785, 983, 30802, 1966, 22938, 9830 },
		{ 32440, 328, 31457, 1311, 24576, 8192 },
		{ 32604, 164, 32440, 328, 24576, 8192 },
		{ 32604, 164, 32702, 66, 24576, 8192 },
	};
	const int16_t *weight;
	unsigned tier;
	unsigned i;

	vad->update_count++;
	if (vad->update_count < 20)
		tier = 0;
	else if (vad->update_count < 30)
		tier = 1;
	else if (vad->update_count < 40)
		tier = 2;
	else if (vad->update_count < 50)
		tier = 3;
	else if (vad->update_count < 60)
		tier = 4;
	else
		tier = 5;
	weight = weights[tier];
	vad->noise_energy = word_high_get(long_mac(
		long_multiply(weight[0], vad->noise_energy), weight[1],
		energy));
	vad->noise_energy_low = word_high_get(long_mac(
		long_multiply(weight[0], vad->noise_energy_low), weight[1],
		energy_low));
	vad->noise_zero_cross = word_high_get(long_mac(
		long_multiply(weight[2], vad->noise_zero_cross), weight[3],
		zero_cross));
	for (i = 0; i < G729_LPC_ORDER; i++)
		vad->noise_lsf[i] = word_high_get(long_mac(
			long_multiply(weight[4], vad->noise_lsf[i]), weight[5],
			lsf[i]));
}

/* Clauses B.3.4 through B.3.7 for a frame past the initialization phase.
 * The difference variables carry noise average minus frame, the sign of the
 * normative C code, opposite to the clause's frame minus average; the
 * boundary inequalities therefore read flipped against the B.3.5 table. */
static bool vad_decide(struct g729_encoder *encoder, int16_t energy,
		       int16_t energy_low, int16_t distortion,
		       int16_t zero_cross, const int16_t *lsf)
{
	struct g729_vad *vad = &encoder->vad;
	int16_t full_diff;
	int16_t low_diff;
	int16_t cross_diff;
	bool decision;

	if (vad->frame_count == 32)
		vad_average_scale(vad);
	full_diff = word_subtract(vad->noise_energy, energy);
	low_diff = word_subtract(vad->noise_energy_low, energy_low);
	cross_diff = word_subtract(vad->noise_zero_cross, zero_cross);
	if (energy < 3072)
		decision = false;
	else
		decision = vad_boundaries_check(low_diff, full_diff,
			distortion, cross_diff);
	vad->hangover_flag = 0;
	if (vad->decision_previous && !decision &&
	    word_add(full_diff, 410) < 0 && energy > 3072) {
		decision = true;
		vad->hangover_flag = 1;
	}
	if (vad->extend_flag)
		decision = vad_extend_check(vad, decision, energy);
	else
		vad->extend_flag = 1;
	if (!decision)
		vad->silence_count++;
	if (decision && vad->silence_count > 10 &&
	    word_subtract(energy, vad->energy_previous) <= 614) {
		decision = false;
		vad->silence_count = 0;
	}
	if (decision)
		vad->silence_count = 0;
	if (word_subtract(word_subtract(energy, 614), vad->noise_energy) < 0 &&
	    vad->frame_count > 128 && !vad->hangover_flag &&
	    encoder->reflection_previous[1] < 19661)
		decision = false;
	if (word_subtract(word_subtract(energy, 614), vad->noise_energy) < 0 &&
	    encoder->reflection_previous[1] < 24576 && distortion < 83)
		vad_noise_update(vad, energy, energy_low, zero_cross, lsf);
	if (vad->frame_count > 128 &&
	    ((word_subtract(vad->noise_energy, vad->minimum) < 0 &&
	      distortion < 83) ||
	     word_subtract(vad->noise_energy, vad->minimum) > 2048)) {
		vad->noise_energy = vad->minimum;
		vad->update_count = 0;
	}
	return decision;
}

static void encoder_vad_update(struct g729_encoder *encoder,
			       const int16_t *high, const int16_t *low,
			       int16_t exponent, const int16_t *lsp)
{
	struct g729_vad *vad = &encoder->vad;
	int16_t lsf[G729_LPC_ORDER];
	int16_t energy;
	int16_t energy_low;
	int16_t distortion;
	int16_t zero_cross = 0;
	int32_t total;
	bool decision = false;
	unsigned i;

	/* The increment must precede vad_minimum_update(): its buffer index
	 * (frame_count >> 3) - 1 assumes the one-based frame number. */
	if (vad->frame_count == INT16_MAX)
		vad->frame_count = 256;
	else
		vad->frame_count++;
	vad_lsf_get(lsp, lsf);
	energy = vad_log_energy_get(long_compose(high[0], low[0]), exponent);
	total = 0;
	for (i = 1; i <= G729_VAD_ORDER; i++)
		total = long_mac(total, high[i], g729_vad_low_band[i]);
	total = long_shift_left(total, 1);
	total = long_mac(total, high[0], g729_vad_low_band[0]);
	energy_low = vad_log_energy_get(total, exponent);
	total = 0;
	for (i = 0; i < G729_LPC_ORDER; i++) {
		int16_t difference = word_subtract(lsf[i], vad->noise_lsf[i]);

		total = long_mac(total, difference, difference);
	}
	distortion = word_high_get(total);
	for (i = 121; i <= 200; i++) {
		if (word_multiply(encoder->speech[i - 1],
				  encoder->speech[i]) < 0)
			zero_cross = word_add(zero_cross, 410);
	}
	vad_minimum_update(vad, energy);
	if (vad->frame_count <= 32)
		decision = vad_average_init(vad, energy, zero_cross, lsf);
	if (vad->frame_count >= 32)
		decision = vad_decide(encoder, energy, energy_low, distortion,
			zero_cross, lsf);
	vad->energy_previous = energy;
	vad->decision_earlier = vad->decision_previous;
	vad->decision_previous = decision;
	vad->decision = decision;
}

/*
 * Annex B clause B.4, the DTX decision, the SID encoder and the
 * encoder-local comfort noise of clause B.4.4. Like the B.3 section, the
 * arithmetic follows the normative C code operation by operation, because
 * clause B.5 makes the bit stream normative down to each rounding.
 */

static int16_t dtx_random_get(int16_t *seed)
{
	*seed = (int16_t)(uint16_t)(31821U * (uint16_t)*seed + 13849U);
	return *seed;
}

static int16_t dtx_gauss_get(int16_t *seed)
{
	int32_t total = 0;
	unsigned i;

	for (i = 0; i < 12; i++)
		total = long_add(total, dtx_random_get(seed));
	return word_low_get(long_shift_right(total, 7));
}

static int16_t dtx_sqrt_get(int32_t value)
{
	int16_t root = 0;
	int16_t probe = 0x4000;
	unsigned i;

	for (i = 0; i < 14; i++) {
		int16_t candidate = word_add(root, probe);
		int32_t square = long_multiply(candidate, candidate);

		if (long_subtract(value, square) >= 0)
			root = candidate;
		probe = word_shift_right(probe, 1);
	}
	return root;
}

/* Equations (B.9) and (B.16): sum the stored autocorrelation rows into one
 * normalized set with a common exponent. */
static void dtx_acf_sum_get(const int16_t rows[][G729_LPC_ORDER + 1],
			    const int16_t *shifts, unsigned count,
			    int16_t *sum, int16_t *shift)
{
	int32_t table[G729_LPC_ORDER + 1] = { 0 };
	int16_t base = shifts[0];
	int normalization;
	unsigned row;
	unsigned i;

	for (row = 1; row < count; row++) {
		if (shifts[row] < base)
			base = shifts[row];
	}
	base = word_add(base, 14);
	for (row = 0; row < count; row++) {
		int amount = base - shifts[row];

		for (i = 0; i <= G729_LPC_ORDER; i++)
			table[i] = long_add(table[i],
				long_shift_left(rows[row][i], amount));
	}
	normalization = long_normalize(table[0]);
	for (i = 0; i <= G729_LPC_ORDER; i++)
		sum[i] = word_high_get(long_shift_left(table[i],
			normalization));
	*shift = word_add(base, (int16_t)(normalization - 16));
}

static void dtx_sum_commit(struct g729_dtx *dtx)
{
	memmove(dtx->acf_sum[1], dtx->acf_sum[0],
		(G729_DTX_SUM_FRAMES - 1) * sizeof(dtx->acf_sum[0]));
	memmove(dtx->acf_sum_shift + 1, dtx->acf_sum_shift,
		(G729_DTX_SUM_FRAMES - 1) * sizeof(dtx->acf_sum_shift[0]));
	dtx_acf_sum_get(dtx->acf, dtx->acf_shift, G729_DTX_ACF_FRAMES,
		dtx->acf_sum[0], &dtx->acf_sum_shift[0]);
}

/* Clause B.4.1.1: every frame, voiced or not, stores its raw
 * autocorrelation coefficients; a full two-frame window of voiced frames
 * commits into the clause B.4.2.2 past-filter sum. */
static void dtx_acf_update(struct g729_dtx *dtx, const int16_t *high,
			   int16_t exponent, bool voiced)
{
	memmove(dtx->acf[1], dtx->acf[0],
		(G729_DTX_ACF_FRAMES - 1) * sizeof(dtx->acf[0]));
	memmove(dtx->acf_shift + 1, dtx->acf_shift,
		(G729_DTX_ACF_FRAMES - 1) * sizeof(dtx->acf_shift[0]));
	dtx->acf_shift[0] = word_subtract(0, word_add(16, exponent));
	memcpy(dtx->acf[0], high, sizeof(dtx->acf[0]));
	dtx->frame_phase++;
	if (dtx->frame_phase != G729_DTX_ACF_FRAMES)
		return;
	dtx->frame_phase = 0;
	if (voiced)
		dtx_sum_commit(dtx);
}

/* Equation (B.13): the scaled autocorrelation of a filter's coefficients. */
static void dtx_filter_correlation_get(const int16_t *coefficients,
				       int16_t *correlation, int16_t *shift)
{
	int32_t total = 0;
	int normalization;
	unsigned i;
	unsigned j;

	for (j = 0; j <= G729_LPC_ORDER; j++)
		total = long_mac(total, coefficients[j], coefficients[j]);
	normalization = long_normalize(total);
	correlation[0] = long_round(long_shift_left(total, normalization));
	for (i = 1; i <= G729_LPC_ORDER; i++) {
		total = 0;
		for (j = 0; j + i <= G729_LPC_ORDER; j++)
			total = long_mac(total, coefficients[j],
				coefficients[j + i]);
		correlation[i] = long_round(long_shift_left(total,
			normalization));
	}
	*shift = (int16_t)normalization;
}

/* Equation (B.12): the Itakura distance test between the stored filter
 * correlation and a frame autocorrelation, against alpha times a
 * threshold. The retry loop narrows one side at a time until the sum fits
 * without overflow, in the shift order of the normative C code. */
static bool dtx_filter_distance_check(const int16_t *correlation,
				      int16_t correlation_shift,
				      const int16_t *acf, int16_t alpha,
				      int16_t threshold)
{
	int16_t shifts[2] = { 0, 0 };
	unsigned side = 1;
	int32_t distance;
	int32_t bound;
	int amount;
	unsigned i;

	for (;;) {
		bool overflow = false;

		distance = long_shift_right(long_multiply_track(
			word_shift_right(correlation[0], shifts[0]),
			word_shift_right(acf[0], shifts[1]), &overflow), 1);
		for (i = 1; i <= G729_LPC_ORDER; i++)
			distance = long_mac_track(distance,
				word_shift_right(correlation[i], shifts[0]),
				word_shift_right(acf[i], shifts[1]),
				&overflow);
		if (!overflow)
			break;
		shifts[side]++;
		side = 1 - side;
	}
	bound = long_add(word_multiply_round(alpha, threshold), alpha);
	amount = (correlation_shift + 9) - (shifts[0] + shifts[1]);
	bound = long_shift_left(bound, amount);
	return long_subtract(distance, bound) > 0;
}

/* Equation (B.16): the past average filter over the committed
 * autocorrelation sums. */
static void dtx_past_filter_get(struct g729_encoder *encoder,
				int16_t *coefficients)
{
	static const int16_t zero_low[G729_LPC_ORDER + 1] = { 0 };
	struct g729_dtx *dtx = &encoder->dtx;
	int16_t summed[G729_LPC_ORDER + 1];
	int16_t shift;

	dtx_acf_sum_get(dtx->acf_sum, dtx->acf_sum_shift,
		G729_DTX_SUM_FRAMES, summed, &shift);
	if (!summed[0]) {
		coefficients[0] = 4096;
		memset(coefficients + 1, 0,
			G729_LPC_ORDER * sizeof(*coefficients));
		return;
	}
	levinson_get(encoder, summed, zero_low, coefficients, NULL);
}

/* Clause B.4.2.1, the 5-bit logarithmic quantizer of the energy term. */
static int16_t dtx_energy_quantize(int32_t value, int16_t shift,
				   int16_t *decoded)
{
	int16_t exponent;
	int16_t fraction;
	int16_t log_energy;
	int16_t index;

	log2_get(value, &exponent, &fraction);
	log_energy = word_add(
		word_shift_left(word_subtract(exponent, shift), 10),
		word_multiply_round(fraction, 1024));
	if (word_subtract(log_energy, -2721) <= 0) {
		*decoded = -12;
		return 0;
	}
	if (word_subtract(log_energy, 22111) > 0) {
		*decoded = 66;
		return 31;
	}
	if (word_subtract(log_energy, 4762) <= 0) {
		log_energy = word_add(log_energy, 3401);
		index = word_multiply(log_energy, 24);
		if (index < 1)
			index = 1;
		*decoded = word_subtract(word_shift_left(index, 2), 8);
		return index;
	}
	log_energy = word_subtract(log_energy, 340);
	index = word_subtract(
		word_shift_right(word_multiply(log_energy, 193), 2), 1);
	if (index < 6)
		index = 6;
	*decoded = word_add(word_shift_left(index, 1), 4);
	return index;
}

/* Equation (B.15): scale the weighted energy sum into the quantizer. A
 * count of zero quantizes a single pre-scaled energy, the clause B.4.5
 * erasure recovery. */
static int16_t dtx_gain_quantize(const int16_t *energy,
				 const int16_t *shifts, int16_t count,
				 int16_t *decoded)
{
	int32_t value;
	int16_t shift;
	int16_t high;
	int16_t low;
	int16_t i;

	if (!count) {
		value = long_shift_left(*energy, *shifts);
		long_extract(value, &high, &low);
		value = long_multiply_dpf(high, low, g729_sid_gain_factor[0]);
		shift = 0;
	} else {
		shift = shifts[0];
		for (i = 1; i < count; i++) {
			if (shifts[i] < shift)
				shift = shifts[i];
		}
		shift = word_add(shift,
			(int16_t)(16 - g729_sid_gain_margin[count]));
		value = 0;
		for (i = 0; i < count; i++)
			value = long_add(value, long_shift_left(energy[i],
				shift - shifts[i]));
		long_extract(value, &high, &low);
		value = long_multiply_dpf(high, low,
			g729_sid_gain_factor[count]);
	}
	return dtx_energy_quantize(value, shift, decoded);
}

/* Clause B.4.2.2 item 2, the delayed-decision first stage: for each
 * predictor mode, weigh the distance to each mapped stage-1 codeword and
 * keep the four best mode-codeword pairs. */
static void sid_stage1_search(const int16_t residual[2][G729_LPC_ORDER],
			      int16_t candidate[4][G729_LPC_ORDER],
			      unsigned candidate_mode[4],
			      unsigned candidate_index[4])
{
	int16_t sum[2][32];
	unsigned mode;
	unsigned entry;
	unsigned pick;
	unsigned i;

	for (mode = 0; mode < 2; mode++) {
		for (entry = 0; entry < 32; entry++) {
			int32_t distance = 0;

			for (i = 0; i < G729_LPC_ORDER; i++) {
				int16_t difference =
					word_subtract(residual[mode][i],
					g729_lsp_stage1[
						g729_sid_stage1_map[entry]][i]);

				distance = long_mac(distance, difference,
					difference);
			}
			sum[mode][entry] = word_multiply(
				word_high_get(distance),
				g729_sid_mode_weight[mode]);
		}
	}
	for (pick = 0; pick < 4; pick++) {
		int16_t best = INT16_MAX;
		unsigned best_mode = 0;
		unsigned best_entry = 0;

		for (mode = 0; mode < 2; mode++) {
			for (entry = 0; entry < 32; entry++) {
				if (word_subtract(sum[mode][entry], best) >= 0)
					continue;
				best = sum[mode][entry];
				best_mode = mode;
				best_entry = entry;
			}
		}
		sum[best_mode][best_entry] = INT16_MAX;
		for (i = 0; i < G729_LPC_ORDER; i++)
			candidate[pick][i] = word_subtract(
				residual[best_mode][i],
				g729_lsp_stage1[
					g729_sid_stage1_map[best_entry]][i]);
		candidate_mode[pick] = best_mode;
		candidate_index[pick] = best_entry;
	}
}

/* Clause B.4.2.2 item 3, the full second-stage search over the four
 * stage-1 candidates, weighted by each candidate's present-frame factor. */
static void sid_stage2_search(const int16_t candidate[4][G729_LPC_ORDER],
			      const unsigned candidate_mode[4],
			      const int16_t *weights, unsigned *pick_out,
			      unsigned *entry_out)
{
	int16_t sum[4][16];
	int16_t best = INT16_MAX;
	unsigned best_pick = 0;
	unsigned best_entry = 0;
	unsigned pick;
	unsigned entry;
	unsigned i;

	for (pick = 0; pick < 4; pick++) {
		const int16_t *present = g729_sid_present[candidate_mode[pick]];

		for (entry = 0; entry < 16; entry++) {
			int32_t distance = 0;

			for (i = 0; i < G729_LPC_ORDER; i++) {
				unsigned half = i < 5 ? 0 : 1;
				int16_t weight = word_high_get(long_shift_left(
					long_multiply(present[i], present[i]),
					2));
				int16_t difference = word_subtract(
					candidate[pick][i],
					g729_lsp_stage2[g729_sid_stage2_map[
						half][entry]][i]);

				weight = word_multiply(weight, weights[i]);
				weight = word_high_get(long_shift_left(
					long_multiply(weight, difference), 3));
				distance = long_mac(distance, weight,
					difference);
			}
			sum[pick][entry] = word_high_get(distance);
		}
	}
	for (pick = 0; pick < 4; pick++) {
		for (entry = 0; entry < 16; entry++) {
			if (word_subtract(sum[pick][entry], best) >= 0)
				continue;
			best = sum[pick][entry];
			best_pick = pick;
			best_entry = entry;
		}
	}
	*pick_out = best_pick;
	*entry_out = best_entry;
}

/* Clause B.4.2.2: quantize the SID LSF vector with the switched MA
 * predictor and the two mapped codebook stages, sharing the speech
 * quantizer's predictor memory. */
static void sid_lsf_quantize(struct g729_encoder *encoder, const int16_t *lsp,
			     int16_t *quantized_lsp, uint16_t indices[3])
{
	int16_t lsf[G729_LPC_ORDER];
	int16_t weights[G729_LPC_ORDER];
	int16_t residual[2][G729_LPC_ORDER];
	int16_t candidate[4][G729_LPC_ORDER];
	int16_t selected[G729_LPC_ORDER];
	int16_t quantized_lsf[G729_LPC_ORDER];
	unsigned candidate_mode[4];
	unsigned candidate_index[4];
	unsigned mode;
	unsigned pick;
	unsigned stage2;
	unsigned i;

	lsp_to_lsf(lsp, lsf);
	if (lsf[0] < 40)
		lsf[0] = 40;
	for (i = 0; i + 1 < G729_LPC_ORDER; i++) {
		if (word_subtract(lsf[i + 1], lsf[i]) < 642)
			lsf[i + 1] = word_add(lsf[i], 642);
	}
	if (lsf[G729_LPC_ORDER - 1] > 25681)
		lsf[G729_LPC_ORDER - 1] = 25681;
	if (lsf[G729_LPC_ORDER - 1] < lsf[G729_LPC_ORDER - 2])
		lsf[G729_LPC_ORDER - 2] =
			word_subtract(lsf[G729_LPC_ORDER - 1], 321);
	lsf_weights_get(lsf, weights);
	for (mode = 0; mode < 2; mode++)
		lsf_residual_get(encoder->lsf_history, lsf,
			g729_sid_predictor[mode],
			g729_sid_present_inverse[mode], residual[mode]);
	sid_stage1_search(residual, candidate, candidate_mode,
		candidate_index);
	sid_stage2_search(candidate, candidate_mode, weights, &pick, &stage2);
	mode = candidate_mode[pick];
	indices[0] = (uint16_t)mode;
	indices[1] = (uint16_t)candidate_index[pick];
	indices[2] = (uint16_t)stage2;
	for (i = 0; i < G729_LPC_ORDER; i++) {
		unsigned half = i < 5 ? 0 : 1;

		selected[i] = word_add(
			g729_lsp_stage1[g729_sid_stage1_map[indices[1]]][i],
			g729_lsp_stage2[g729_sid_stage2_map[half][stage2]][i]);
	}
	lsf_spacing_apply(selected, 10);
	lsf_compose(encoder->lsf_history, selected, g729_sid_present[mode],
		g729_sid_predictor[mode], quantized_lsf);
	encoder_lsf_history_update(encoder, selected);
	lsf_stability_apply(quantized_lsf);
	lsf_to_lsp(quantized_lsf, quantized_lsp);
}

/* Clause B.4.3 and octet transmission mode: 15 SID bits, most significant
 * bit of each parameter first, and one zero pad bit closing the second
 * octet. */
static void sid_parameters_pack(const uint16_t indices[3], unsigned gain,
				uint8_t payload[G729_SID_BYTES])
{
	unsigned position = 0;

	memset(payload, 0, G729_SID_BYTES);
	bits_write(payload, &position, indices[0], 1);
	bits_write(payload, &position, indices[1], 5);
	bits_write(payload, &position, indices[2], 4);
	bits_write(payload, &position, gain, 5);
}

/* The clause B.4.4 random draw of one subframe's parameters: fractional
 * pitch, four pulse positions with their signs, and the adaptive gain. */
static void dtx_parameters_draw(int16_t *seed, int16_t *pitch,
				int16_t *fraction, int16_t position[4],
				int16_t sign[4], int16_t *pitch_gain)
{
	int16_t random;
	int16_t spread;

	random = dtx_random_get(seed);
	*fraction = word_subtract(random & 3, 1);
	if (*fraction == 2)
		*fraction = 0;
	random = word_shift_right(random, 2);
	*pitch = word_add(random & 0x3f, G729_SUBFRAME);
	random = word_shift_right(random, 6);
	spread = random & 7;
	position[0] = word_add(word_shift_left(spread, 2), spread);
	random = word_shift_right(random, 3);
	sign[0] = random & 1;
	random = word_shift_right(random, 1);
	spread = random & 7;
	spread = word_add(word_shift_left(spread, 2), spread);
	position[1] = word_add(spread, 1);
	random = word_shift_right(random, 3);
	sign[1] = random & 1;
	random = dtx_random_get(seed);
	spread = random & 7;
	spread = word_add(word_shift_left(spread, 2), spread);
	position[2] = word_add(spread, 2);
	random = word_shift_right(random, 3);
	sign[2] = random & 1;
	random = word_shift_right(random, 1);
	spread = random & 0xf;
	position[3] = word_add(spread & 1, 3);
	spread = word_shift_right(spread, 1) & 7;
	spread = word_add(word_shift_left(spread, 2), spread);
	position[3] = word_add(position[3], spread);
	random = word_shift_right(random, 4);
	sign[3] = random & 1;
	*pitch_gain = dtx_random_get(seed) & 0x1fff;
}

/* One subframe of gaussian excitation, scaled to alpha times the target
 * gain through the inverse square root of its own energy. */
static void dtx_gaussian_draw(int16_t *seed, int16_t gain,
			      int16_t gaussian[G729_SUBFRAME])
{
	int32_t total = 0;
	int16_t factor;
	int16_t shift;
	int16_t high;
	int16_t low;
	unsigned i;

	for (i = 0; i < G729_SUBFRAME; i++) {
		gaussian[i] = dtx_gauss_get(seed);
		total = long_mac(total, gaussian[i], gaussian[i]);
	}
	total = inverse_sqrt_get(long_shift_right(total, 1));
	long_extract(total, &high, &low);
	factor = word_add(gain, word_multiply_round(gain, 19043));
	total = long_multiply_dpf(high, low, factor);
	shift = (int16_t)long_normalize(total);
	factor = word_high_get(long_shift_left(total, shift));
	shift = word_subtract(shift, 14);
	for (i = 0; i < G729_SUBFRAME; i++)
		gaussian[i] = word_shift_right_round(
			word_multiply_round(gaussian[i], factor), shift);
}

static int16_t dtx_pulse_sum(const int16_t *samples,
			     const int16_t position[4],
			     const int16_t sign[4], int shift)
{
	int16_t sum = 0;
	unsigned i;

	for (i = 0; i < 4; i++) {
		int16_t pulse = word_shift_right(samples[position[i]], shift);

		if (sign[i])
			sum = word_add(sum, pulse);
		else
			sum = word_subtract(sum, pulse);
	}
	return sum;
}

/* Equations (B.20) through (B.22): the fixed gain that brings the mixed
 * excitation to the target energy. A negative discriminant drops the
 * adaptive part, current becomes the gaussian excitation alone and the
 * adaptive gain zeroes for the taming update. */
static int16_t dtx_fixed_gain_solve(int16_t *current,
				    const int16_t *gaussian,
				    const int16_t position[4],
				    const int16_t sign[4], int16_t gain,
				    int16_t maximum, int16_t *pitch_gain)
{
	int16_t scaled[G729_SUBFRAME];
	int16_t inter;
	int16_t shift = 0;
	int16_t high;
	int16_t low;
	int16_t first_root;
	int16_t second_root;
	int16_t negated;
	int16_t fixed_gain;
	int32_t total = 0;
	int32_t target;
	int32_t squared_target;
	unsigned i;

	if (maximum) {
		shift = word_subtract(3, (int16_t)word_normalize(maximum));
		if (shift < 0)
			shift = 0;
	}
	for (i = 0; i < G729_SUBFRAME; i++)
		scaled[i] = word_shift_right(current[i], shift);
	for (i = 0; i < G729_SUBFRAME; i++)
		total = long_mac(total, scaled[i], scaled[i]);
	inter = dtx_pulse_sum(scaled, position, sign, 0);
	squared_target = long_multiply(gain, word_low_get(
		long_shift_right(long_multiply(gain, G729_SUBFRAME), 6)));
	target = long_shift_right(squared_target,
		word_add(1, word_shift_left(shift, 1)));
	target = long_subtract(target, total);
	inter = word_shift_right(inter, 1);
	target = long_mac(target, inter, inter);
	shift = word_add(shift, 1);
	if (target < 0) {
		int16_t mask;

		memcpy(current, gaussian, G729_SUBFRAME * sizeof(*current));
		mask = word_absolute(gaussian[position[0]]) |
			word_absolute(gaussian[position[1]]) |
			word_absolute(gaussian[position[2]]) |
			word_absolute(gaussian[position[3]]);
		shift = (mask & 0x4000) ? 2 : 1;
		inter = dtx_pulse_sum(gaussian, position, sign, shift);
		long_extract(squared_target, &high, &low);
		target = long_multiply_dpf(high, low, 24576);
		target = long_shift_right(target,
			word_subtract(word_shift_left(shift, 1), 1));
		target = long_mac(target, inter, inter);
		*pitch_gain = 0;
	}
	second_root = dtx_sqrt_get(target);
	first_root = word_subtract(second_root, inter);
	negated = word_subtract(0, word_add(inter, second_root));
	if (word_subtract(word_absolute(negated),
		    word_absolute(first_root)) < 0)
		first_root = negated;
	fixed_gain = word_shift_right_round(first_root,
		word_subtract(2, shift));
	if (fixed_gain >= 0) {
		if (word_subtract(fixed_gain, 5000) > 0)
			fixed_gain = 5000;
	} else if (word_add(fixed_gain, 5000) < 0) {
		fixed_gain = -5000;
	}
	return fixed_gain;
}

/* Clause B.4.4, the comfort-noise excitation both sides synthesize: the
 * decoder for its output, the encoder locally so its filter memories track
 * the far decoder. Only the encoder updates the taming memory, because the
 * decoder has none. */
static void dtx_excitation_generate(int16_t *excitation, int16_t *seed,
				    int16_t gain,
				    struct g729_encoder *encoder)
{
	unsigned subframe;

	if (!gain) {
		memset(excitation, 0, G729_FRAME * sizeof(*excitation));
		if (!encoder)
			return;
		for (subframe = 0; subframe < 2; subframe++)
			excitation_error_update(encoder, 0,
				G729_SUBFRAME + 1);
		return;
	}
	for (subframe = 0; subframe < 2; subframe++) {
		int16_t *current = excitation + subframe * G729_SUBFRAME;
		int16_t gaussian[G729_SUBFRAME];
		int16_t position[4];
		int16_t sign[4];
		int16_t fraction;
		int16_t pitch;
		int16_t pitch_gain;
		int16_t pitch_gain_q15;
		int16_t maximum = 0;
		int16_t fixed_gain;
		unsigned i;

		dtx_parameters_draw(seed, &pitch, &fraction, position, sign,
			&pitch_gain);
		pitch_gain_q15 = word_shift_left(pitch_gain, 1);
		dtx_gaussian_draw(seed, gain, gaussian);
		adaptive_codebook_get(current, pitch, fraction);
		for (i = 0; i < G729_SUBFRAME; i++) {
			int16_t mixed = word_add(word_multiply_round(
				current[i], pitch_gain_q15), gaussian[i]);
			int16_t magnitude = word_absolute(mixed);

			current[i] = mixed;
			if (magnitude > maximum)
				maximum = magnitude;
		}
		fixed_gain = dtx_fixed_gain_solve(current, gaussian, position,
			sign, gain, maximum, &pitch_gain);
		for (i = 0; i < 4; i++) {
			if (sign[i])
				current[position[i]] = word_add(
					current[position[i]], fixed_gain);
			else
				current[position[i]] = word_subtract(
					current[position[i]], fixed_gain);
		}
		if (encoder)
			excitation_error_update(encoder, pitch_gain, pitch);
	}
}

/* Equation (B.19), the smoothed target gain shared by the encoder-local
 * comfort noise and the decoder comfort noise. */
static int16_t dtx_gain_smooth(int16_t current, int16_t sid_gain)
{
	return word_add(word_multiply_round(current, 28672),
		word_multiply_round(sid_gain, 4096));
}

static void encoder_buffers_shift(struct g729_encoder *encoder)
{
	memmove(encoder->speech, encoder->speech + G729_FRAME,
		G729_SPEECH_HISTORY * sizeof(encoder->speech[0]));
	memmove(encoder->weighted, encoder->weighted + G729_FRAME,
		G729_PITCH_MAX * sizeof(encoder->weighted[0]));
	memmove(encoder->excitation, encoder->excitation + G729_FRAME,
		G729_EXCITATION_HISTORY * sizeof(encoder->excitation[0]));
}

/* Clause B.4.4 generates the comfort-noise excitation on both sides, so the
 * encoder stays in step with the far decoder. The weighted-speech and target
 * memories therefore track each non-active frame: the weighting filter
 * consumes the speech residual through the comfort-noise filter, and the
 * target memory consumes that residual minus the local comfort-noise
 * excitation. */
static void encoder_cng_memories_update(struct g729_encoder *encoder,
					int16_t lpc[2][G729_LPC_ORDER + 1])
{
	int16_t *speech = encoder->speech + 120;
	int16_t *excitation = encoder->excitation + G729_EXCITATION_HISTORY;
	int16_t *weighted = encoder->weighted + G729_PITCH_MAX;
	unsigned subframe;

	for (subframe = 0; subframe < 2; subframe++) {
		int16_t weighted_lpc[G729_LPC_ORDER + 1];
		int16_t denominator[G729_LPC_ORDER + 1];
		int16_t residual[G729_SUBFRAME];
		unsigned i;

		residual_filter(lpc[subframe],
			speech + subframe * G729_SUBFRAME, residual);
		lpc_weight(lpc[subframe], 24576, weighted_lpc);
		denominator[0] = 4096;
		for (i = 1; i <= G729_LPC_ORDER; i++)
			denominator[i] = word_subtract(weighted_lpc[i],
				word_multiply(weighted_lpc[i - 1], 22938));
		synthesis_filter_plain(denominator, residual,
			weighted + subframe * G729_SUBFRAME, G729_SUBFRAME,
			encoder->weighting_memory);
		for (i = 0; i < G729_SUBFRAME; i++)
			residual[i] = word_subtract(residual[i],
				excitation[subframe * G729_SUBFRAME + i]);
		synthesis_filter_plain(weighted_lpc, residual, residual,
			G729_SUBFRAME, encoder->target_memory);
	}
}

/* Clauses B.4.1.2 through B.4.2.2, one non-active frame: decide SID
 * against untransmitted, quantize the SID parameters when one goes out,
 * and run the local comfort noise either way. Returns the encoded byte
 * count, G729_SID_BYTES or 0. */
static int encoder_cng_encode(struct g729_encoder *encoder, uint8_t *payload)
{
	static const int16_t zero_low[G729_LPC_ORDER + 1] = { 0 };
	struct g729_dtx *dtx = &encoder->dtx;
	bool past_voice = encoder->vad.decision_earlier;
	int16_t current_acf[G729_LPC_ORDER + 1];
	int16_t current_lpc[G729_LPC_ORDER + 1] = { 4096 };
	int16_t lpc[2][G729_LPC_ORDER + 1];
	int16_t quantized_energy;
	int16_t gain_index;
	uint16_t indices[3] = { 0 };
	bool sid;
	unsigned i;

	for (i = G729_DTX_GAIN_FRAMES - 1; i >= 1; i--) {
		dtx->energy[i] = dtx->energy[i - 1];
		dtx->energy_shift[i] = dtx->energy_shift[i - 1];
	}
	dtx_acf_sum_get(dtx->acf, dtx->acf_shift, G729_DTX_ACF_FRAMES,
		current_acf, &dtx->energy_shift[0]);
	if (!current_acf[0])
		dtx->energy[0] = 0;
	else
		levinson_get(encoder, current_acf, zero_low, current_lpc,
			&dtx->energy[0]);
	if (past_voice) {
		sid = true;
		dtx->since_sid = 0;
		dtx->energy_count = 1;
		gain_index = dtx_gain_quantize(dtx->energy, dtx->energy_shift,
			1, &quantized_energy);
	} else {
		dtx->energy_count = word_add(dtx->energy_count, 1);
		if (dtx->energy_count > G729_DTX_GAIN_FRAMES)
			dtx->energy_count = G729_DTX_GAIN_FRAMES;
		gain_index = dtx_gain_quantize(dtx->energy, dtx->energy_shift,
			dtx->energy_count, &quantized_energy);
		if (dtx_filter_distance_check(dtx->sid_correlation,
			    dtx->sid_correlation_shift, current_acf,
			    dtx->energy[0], G729_DTX_CHANGE_FRACTION))
			dtx->change_pending = true;
		/* Clause B.4.1.4: a decoded-energy step above 2 dB counts
		 * as a change. */
		if (word_subtract(word_absolute(word_subtract(
			    dtx->energy_previous, quantized_energy)), 2) > 0)
			dtx->change_pending = true;
		dtx->since_sid = word_add(dtx->since_sid, 1);
		if (dtx->since_sid < G729_DTX_SID_MINIMUM) {
			sid = false;
		} else {
			sid = dtx->change_pending;
			dtx->since_sid = G729_DTX_SID_MINIMUM;
		}
	}
	if (sid) {
		int16_t past_filter[G729_LPC_ORDER + 1];
		const int16_t *sid_filter = past_filter;
		int16_t sid_lsp_new[G729_LPC_ORDER];

		dtx->since_sid = 0;
		dtx->change_pending = false;
		dtx_past_filter_get(encoder, past_filter);
		dtx_filter_correlation_get(past_filter, dtx->sid_correlation,
			&dtx->sid_correlation_shift);
		/* Equation (B.17): a stationary neighborhood transmits the
		 * past average filter, otherwise the current one becomes the
		 * new reference. */
		if (dtx_filter_distance_check(dtx->sid_correlation,
			    dtx->sid_correlation_shift, current_acf,
			    dtx->energy[0], G729_DTX_AVERAGE_FRACTION)) {
			sid_filter = current_lpc;
			dtx_filter_correlation_get(current_lpc,
				dtx->sid_correlation,
				&dtx->sid_correlation_shift);
		}
		lpc_to_lsp(sid_filter, encoder->lsp_quantized_previous,
			sid_lsp_new);
		sid_lsf_quantize(encoder, sid_lsp_new, dtx->sid_lsp, indices);
		dtx->energy_previous = quantized_energy;
		dtx->sid_gain = g729_sid_gain[gain_index];
		sid_parameters_pack(indices, (unsigned)gain_index, payload);
	}
	if (past_voice)
		dtx->gain_current = dtx->sid_gain;
	else
		dtx->gain_current = dtx_gain_smooth(dtx->gain_current,
			dtx->sid_gain);
	dtx_excitation_generate(encoder->excitation + G729_EXCITATION_HISTORY,
		&dtx->seed, dtx->gain_current, encoder);
	lsp_pair_to_lpc(encoder->lsp_quantized_previous, dtx->sid_lsp, lpc);
	memcpy(encoder->lsp_quantized_previous, dtx->sid_lsp,
		sizeof(encoder->lsp_quantized_previous));
	if (!dtx->frame_phase)
		dtx_sum_commit(dtx);
	encoder_cng_memories_update(encoder, lpc);
	encoder->sharp = 3277;
	encoder_buffers_shift(encoder);
	return sid ? (int)G729_SID_BYTES : 0;
}

static void encoder_reset(struct g729_encoder *encoder)
{
	static const int16_t initial_lsp[G729_LPC_ORDER] = {
		30000, 26000, 21000, 15000, 8000,
		0, -8000, -15000, -21000, -26000,
	};
	static const int16_t initial_lsf[G729_LPC_ORDER] = {
		2339, 4679, 7018, 9358, 11698,
		14037, 16377, 18717, 21056, 23396,
	};
	bool dtx_enabled = encoder->dtx.enabled;
	unsigned i;

	memset(encoder, 0, sizeof(*encoder));
	memcpy(encoder->lsp_previous, initial_lsp, sizeof(initial_lsp));
	memcpy(encoder->lsp_quantized_previous, initial_lsp, sizeof(initial_lsp));
	encoder->lpc_previous[0] = 4096;
	for (i = 0; i < 4; i++) {
		memcpy(encoder->lsf_history[i], initial_lsf, sizeof(initial_lsf));
		encoder->gain_energy[i] = -14336;
		encoder->excitation_error[i] = 16384;
	}
	encoder->sharp = 3277;
	encoder->vad.minimum = INT16_MAX;
	encoder->vad.extend_flag = 1;
	encoder->vad.decision_previous = true;
	encoder->vad.decision_earlier = true;
	/* The dtx flag is a create parameter and survives reset per the
	 * codec contract; the DTX state proper starts over. */
	encoder->dtx.enabled = dtx_enabled;
	for (i = 0; i < G729_DTX_ACF_FRAMES; i++)
		encoder->dtx.acf_shift[i] = 40;
	for (i = 0; i < G729_DTX_SUM_FRAMES; i++)
		encoder->dtx.acf_sum_shift[i] = 40;
	for (i = 0; i < G729_DTX_GAIN_FRAMES; i++)
		encoder->dtx.energy_shift[i] = 40;
	encoder->dtx.seed = G729_DTX_SEED_INITIAL;
}

static void encoder_analysis_get(struct g729_encoder *encoder,
				 int16_t lsp[G729_LPC_ORDER])
{
	int16_t correlation_high[G729_VAD_ORDER + 1];
	int16_t correlation_low[G729_VAD_ORDER + 1];
	int16_t raw_high[G729_LPC_ORDER + 1];
	int16_t correlation_exponent;
	int16_t analysis_lpc[G729_LPC_ORDER + 1];

	autocorrelation_get(encoder->speech, correlation_high, correlation_low,
		&correlation_exponent);
	/* Clause B.4.1.1 stores the frame autocorrelation before the lag
	 * window. */
	memcpy(raw_high, correlation_high, sizeof(raw_high));
	lag_window_apply(correlation_high, correlation_low);
	levinson_get(encoder, correlation_high, correlation_low, analysis_lpc,
		NULL);
	lpc_to_lsp(analysis_lpc, encoder->lsp_previous, lsp);
	encoder_vad_update(encoder, correlation_high, correlation_low,
		correlation_exponent, lsp);
	if (encoder->dtx.enabled)
		dtx_acf_update(&encoder->dtx, raw_high, correlation_exponent,
			encoder->vad.decision);
}

static void encoder_lpc_get(struct g729_encoder *encoder,
			    struct g729_parameters *parameters,
			    const int16_t lsp[G729_LPC_ORDER],
			    int16_t lpc[2][G729_LPC_ORDER + 1],
			    int16_t weighted[2][G729_LPC_ORDER + 1])
{
	int16_t quantized_lsp[G729_LPC_ORDER];

	lsf_quantize(encoder, lsp, parameters, quantized_lsp);
	lsp_pair_to_lpc(encoder->lsp_quantized_previous, quantized_lsp, lpc);
	lpc_weight(lpc[0], 24576, weighted[0]);
	lpc_weight(lpc[1], 24576, weighted[1]);
	memcpy(encoder->lsp_previous, lsp, sizeof(encoder->lsp_previous));
	memcpy(encoder->lsp_quantized_previous, quantized_lsp,
		sizeof(encoder->lsp_quantized_previous));
}

static void encoder_weighted_speech_get(struct g729_encoder *encoder,
					const int16_t lpc[2][G729_LPC_ORDER + 1],
					const int16_t weighted[2][G729_LPC_ORDER + 1])
{
	int16_t denominator[G729_LPC_ORDER + 1];
	int16_t *speech = encoder->speech + 120;
	int16_t *excitation = encoder->excitation + G729_EXCITATION_HISTORY;
	int16_t *current_weighted = encoder->weighted + G729_PITCH_MAX;
	unsigned subframe;

	for (subframe = 0; subframe < 2; subframe++) {
		unsigned i;

		residual_filter(lpc[subframe], speech + subframe * G729_SUBFRAME,
			excitation + subframe * G729_SUBFRAME);
		denominator[0] = 4096;
		for (i = 1; i <= G729_LPC_ORDER; i++)
			denominator[i] = word_subtract(weighted[subframe][i],
				word_multiply(weighted[subframe][i - 1], 22938));
		synthesis_filter_plain(denominator,
			excitation + subframe * G729_SUBFRAME,
			current_weighted + subframe * G729_SUBFRAME,
			G729_SUBFRAME, encoder->weighting_memory);
	}
}

static int encoder_frame_encode(struct g729_encoder *encoder,
				const int16_t *pcm, uint8_t payload[G729_BYTES])
{
	struct g729_parameters parameters = { 0 };
	int16_t lpc[2][G729_LPC_ORDER + 1];
	int16_t weighted_lpc[2][G729_LPC_ORDER + 1];
	int16_t lsp[G729_LPC_ORDER];
	int16_t *new_speech = encoder->speech + G729_SPEECH_HISTORY;
	int16_t *excitation = encoder->excitation + G729_EXCITATION_HISTORY;
	int minimum;
	int maximum;
	int open_pitch;
	unsigned subframe;

	memcpy(new_speech, pcm, G729_FRAME * sizeof(*pcm));
	encoder_preprocess(encoder, new_speech);
	encoder_analysis_get(encoder, lsp);
	if (encoder->dtx.enabled) {
		if (!encoder->vad.decision)
			return encoder_cng_encode(encoder, payload);
		/* Clause B.4.5: every active frame resets the pseudo-random
		 * generator, so the encoder-local comfort noise and the far
		 * decoder stay aligned. */
		encoder->dtx.seed = G729_DTX_SEED_INITIAL;
	}
	encoder_lpc_get(encoder, &parameters, lsp, lpc, weighted_lpc);
	encoder_weighted_speech_get(encoder, lpc, weighted_lpc);
	open_pitch = open_pitch_get(encoder->weighted + G729_PITCH_MAX);
	minimum = open_pitch - 3;
	if (minimum < G729_PITCH_MIN)
		minimum = G729_PITCH_MIN;
	maximum = minimum + 6;
	if (maximum > G729_PITCH_MAX) {
		maximum = G729_PITCH_MAX;
		minimum = maximum - 6;
	}
	for (subframe = 0; subframe < 2; subframe++) {
		int16_t impulse[G729_SUBFRAME] = { 4096 };
		int16_t zero_memory[G729_LPC_ORDER] = { 0 };
		int16_t target_memory[G729_LPC_ORDER];
		int16_t target[G729_SUBFRAME];
		int16_t adaptive_filtered[G729_SUBFRAME];
		int16_t fixed_target[G729_SUBFRAME];
		int16_t fixed_code[G729_SUBFRAME];
		int16_t fixed_filtered[G729_SUBFRAME];
		int16_t pitch_coefficients[4];
		int16_t gain_coefficients[5];
		int16_t gain_exponents[5];
		int16_t fraction;
		int16_t pitch_gain;
		int16_t code_gain;
		bool tame;
		int pitch;
		unsigned sample;

		synthesis_filter_plain(weighted_lpc[subframe], impulse, impulse,
			G729_SUBFRAME, zero_memory);
		memcpy(target_memory, encoder->target_memory, sizeof(target_memory));
		synthesis_filter_plain(weighted_lpc[subframe],
			excitation + subframe * G729_SUBFRAME, target,
			G729_SUBFRAME, target_memory);
		pitch = closed_pitch_get(excitation + subframe * G729_SUBFRAME,
			target, impulse, minimum, maximum, subframe != 0, &fraction);
		parameters.pitch[subframe] = (uint16_t)pitch_index_get(pitch, fraction,
			&minimum, &maximum, subframe != 0);
		if (!subframe)
			parameters.parity = (uint16_t)pitch_parity_get(parameters.pitch[0]);
		memset(zero_memory, 0, sizeof(zero_memory));
		synthesis_filter_plain(weighted_lpc[subframe],
			excitation + subframe * G729_SUBFRAME, adaptive_filtered,
			G729_SUBFRAME, zero_memory);
		pitch_gain = pitch_gain_get(target, adaptive_filtered,
			pitch_coefficients);
		tame = excitation_tame_get(encoder, pitch, fraction);
		if (tame && pitch_gain > 15564)
			pitch_gain = 15564;
		for (sample = 0; sample < G729_SUBFRAME; sample++) {
			int32_t contribution = long_shift_left(
				long_multiply(adaptive_filtered[sample], pitch_gain), 1);

			fixed_target[sample] = word_subtract(target[sample],
				word_high_get(contribution));
		}
		parameters.fixed[subframe] = (uint16_t)fixed_codebook_search(
			fixed_target, impulse, pitch, encoder->sharp, fixed_code,
			fixed_filtered, &parameters.signs[subframe]);
		gain_correlations_get(target, adaptive_filtered, fixed_filtered,
			pitch_coefficients, gain_coefficients, gain_exponents);
		parameters.gain[subframe] = (uint16_t)gains_quantize(encoder,
			fixed_code, gain_coefficients, gain_exponents, tame,
			&pitch_gain, &code_gain);
		encoder->sharp = pitch_gain;
		if (encoder->sharp > 13017)
			encoder->sharp = 13017;
		if (encoder->sharp < 3277)
			encoder->sharp = 3277;
		for (sample = 0; sample < G729_SUBFRAME; sample++) {
			int32_t total = long_multiply(
				excitation[subframe * G729_SUBFRAME + sample], pitch_gain);

			total = long_mac(total, fixed_code[sample], code_gain);
			total = long_shift_left(total, 1);
			excitation[subframe * G729_SUBFRAME + sample] = long_round(total);
		}
		excitation_error_update(encoder, pitch_gain, pitch);
		for (sample = G729_SUBFRAME - G729_LPC_ORDER;
		     sample < G729_SUBFRAME; sample++) {
			int16_t adaptive = word_high_get(long_shift_left(
				long_multiply(adaptive_filtered[sample], pitch_gain), 1));
			int16_t fixed = word_high_get(long_shift_left(
				long_multiply(fixed_filtered[sample], code_gain), 2));

			encoder->target_memory[sample -
				(G729_SUBFRAME - G729_LPC_ORDER)] =
				word_subtract(target[sample], word_add(adaptive, fixed));
		}
	}
	parameters_pack(&parameters, payload);
	encoder_buffers_shift(encoder);
	return G729_BYTES;
}

static void decoder_reset(struct g729_decoder *decoder)
{
	static const int16_t initial_lsp[G729_LPC_ORDER] = {
		30000, 26000, 21000, 15000, 8000,
		0, -8000, -15000, -21000, -26000,
	};
	static const int16_t initial_lsf[G729_LPC_ORDER] = {
		2339, 4679, 7018, 9358, 11698,
		14037, 16377, 18717, 21056, 23396,
	};
	static const int16_t initial_sid_lsp[G729_LPC_ORDER] = {
		31441, 27566, 21458, 13612, 4663,
		-4663, -13612, -21458, -27566, -31441,
	};
	unsigned i;

	memset(decoder, 0, sizeof(*decoder));
	memcpy(decoder->lsp_previous, initial_lsp, sizeof(initial_lsp));
	memcpy(decoder->lsf_previous, initial_lsf, sizeof(initial_lsf));
	for (i = 0; i < 4; i++) {
		memcpy(decoder->lsf_history[i], initial_lsf, sizeof(initial_lsf));
		decoder->gain_energy[i] = -14336;
	}
	decoder->pitch_previous = 60;
	decoder->sharp = 3277;
	decoder->post_gain = 4096;
	decoder->random_seed = 21845;
	memcpy(decoder->cng.lsp, initial_sid_lsp, sizeof(initial_sid_lsp));
	decoder->cng.sid_gain = g729_sid_gain[0];
	decoder->cng.energy_shift = 1;
	decoder->cng.seed = G729_DTX_SEED_INITIAL;
	decoder->cng.past_speech = true;
}

/* One subframe of decoded excitation through the synthesis filter, with the
 * overflow rule of the normative C code: a saturated pass scales the whole
 * excitation history down by four and repeats the subframe. */
static void decoder_synthesis_apply(struct g729_decoder *decoder,
				    const int16_t *lpc, unsigned subframe,
				    int16_t *output)
{
	int16_t *excitation = decoder->excitation + G729_EXCITATION_HISTORY +
		subframe * G729_SUBFRAME;
	unsigned sample;

	if (!synthesis_filter(lpc, excitation, output,
			decoder->synthesis_memory))
		return;
	for (sample = 0; sample < G729_EXCITATION_HISTORY + G729_FRAME;
	     sample++)
		decoder->excitation[sample] =
			word_shift_right(decoder->excitation[sample], 2);
	synthesis_filter_plain(lpc, excitation, output, G729_SUBFRAME,
		decoder->synthesis_memory);
}

/* The energy term of clause B.4.5: every good frame snapshots its excitation
 * energy so an untransmitted frame right after speech can recover the SID
 * gain the erased SID carried. */
static void decoder_energy_save(struct g729_decoder *decoder)
{
	const int16_t *excitation =
		decoder->excitation + G729_EXCITATION_HISTORY;
	int32_t total = 0;
	int shift;
	unsigned i;

	for (i = 0; i < G729_FRAME; i++)
		total = long_mac(total, excitation[i], excitation[i]);
	shift = long_normalize(total);
	decoder->cng.energy = long_round(long_shift_left(total, shift));
	decoder->cng.energy_shift = (int16_t)(16 - shift);
}

/* The clause B.4.3 SID decode: the LSF path mirrors the encoder's B.4.2.2
 * quantizer through the mapped codebooks and the shared MA memory, and the
 * gain is a table lookup. */
static void decoder_sid_decode(struct g729_decoder *decoder,
			       const uint8_t payload[G729_SID_BYTES])
{
	struct g729_cng *cng = &decoder->cng;
	int16_t element[G729_LPC_ORDER];
	int16_t lsf[G729_LPC_ORDER];
	unsigned position = 0;
	unsigned mode;
	unsigned first;
	unsigned second;
	unsigned gain;
	unsigned i;

	mode = bits_read(payload, &position, 1);
	first = bits_read(payload, &position, 5);
	second = bits_read(payload, &position, 4);
	gain = bits_read(payload, &position, 5);
	cng->sid_gain = g729_sid_gain[gain];
	for (i = 0; i < G729_LPC_ORDER; i++) {
		unsigned half = i < 5 ? 0 : 1;

		element[i] = word_add(
			g729_lsp_stage1[g729_sid_stage1_map[first]][i],
			g729_lsp_stage2[g729_sid_stage2_map[half][second]][i]);
	}
	lsf_spacing_apply(element, 10);
	lsf_compose(decoder->lsf_history, element, g729_sid_present[mode],
		g729_sid_predictor[mode], lsf);
	lsf_history_update(decoder, element);
	lsf_stability_apply(lsf);
	lsf_to_lsp(lsf, cng->lsp);
	cng->sid_seen = true;
}

/* Clause B.4.4, one non-active frame: a SID frame decodes its parameters
 * first, an untransmitted frame right after speech recovers the gain from
 * the saved energy, and both continue as comfort noise through the shared
 * clause B.4.4 generator and the speech synthesis chain. */
static void decoder_cng_frame(struct g729_decoder *decoder,
			      const uint8_t *payload,
			      int16_t output[G729_FRAME])
{
	struct g729_cng *cng = &decoder->cng;
	int16_t lpc[2][G729_LPC_ORDER + 1];
	int16_t pitch[2];
	bool from_speech = cng->past_speech;
	unsigned subframe;

	if (payload) {
		decoder_sid_decode(decoder, payload);
	} else if (from_speech) {
		int16_t decoded;

		cng->sid_gain = g729_sid_gain[dtx_gain_quantize(&cng->energy,
			&cng->energy_shift, 0, &decoded)];
	}
	if (from_speech)
		cng->gain_current = cng->sid_gain;
	else
		cng->gain_current = dtx_gain_smooth(cng->gain_current,
			cng->sid_gain);
	dtx_excitation_generate(decoder->excitation + G729_EXCITATION_HISTORY,
		&cng->seed, cng->gain_current, NULL);
	lsp_pair_to_lpc(decoder->lsp_previous, cng->lsp, lpc);
	memcpy(decoder->lsp_previous, cng->lsp,
		sizeof(decoder->lsp_previous));
	for (subframe = 0; subframe < 2; subframe++) {
		decoder_synthesis_apply(decoder, lpc[subframe], subframe,
			output + subframe * G729_SUBFRAME);
		pitch[subframe] = decoder->pitch_previous;
	}
	decoder->sharp = 3277;
	decoder_energy_save(decoder);
	postfilter_apply(decoder, output, lpc, pitch, false);
	memmove(decoder->excitation, decoder->excitation + G729_FRAME,
		G729_EXCITATION_HISTORY * sizeof(decoder->excitation[0]));
	cng->past_speech = false;
}

static void decoder_frame_decode(struct g729_decoder *decoder,
				 const uint8_t *payload, bool erased,
				 int16_t output[G729_FRAME])
{
	struct g729_parameters parameters = { 0 };
	int16_t lpc[2][G729_LPC_ORDER + 1];
	int16_t code[G729_SUBFRAME];
	int16_t decoded_pitch[2];
	int16_t *excitation = decoder->excitation + G729_EXCITATION_HISTORY;
	int16_t pitch = decoder->pitch_previous;
	unsigned subframe;

	/* Clause B.4.5: every active frame, erased ones included, resets the
	 * comfort-noise generator on both sides. */
	decoder->cng.seed = G729_DTX_SEED_INITIAL;
	if (!erased)
		parameters_unpack(payload, &parameters);
	lpc_frame_get(decoder, &parameters, erased, lpc);
	for (subframe = 0; subframe < 2; subframe++) {
		int16_t fraction = 0;
		bool bad_pitch = erased;
		unsigned fixed_index = parameters.fixed[subframe];
		unsigned signs = parameters.signs[subframe];
		unsigned sample;

		if (!subframe && !erased)
			bad_pitch = pitch_parity_get(parameters.pitch[0]) != parameters.parity;
		if (bad_pitch) {
			pitch = decoder->pitch_previous;
			decoder->pitch_previous++;
			if (decoder->pitch_previous > G729_PITCH_MAX)
				decoder->pitch_previous = G729_PITCH_MAX;
		} else {
			pitch_decode(parameters.pitch[subframe], subframe != 0,
				     &pitch, &fraction);
			decoder->pitch_previous = pitch;
		}
		decoded_pitch[subframe] = pitch;
		adaptive_codebook_get(excitation + subframe * G729_SUBFRAME,
			pitch, fraction);
		if (erased) {
			fixed_index = decoder_random_get(decoder) & 0x1fff;
			signs = decoder_random_get(decoder) & 0x0f;
		}
		fixed_codebook_get(fixed_index, signs, code);
		if (pitch < (int)G729_SUBFRAME) {
			int16_t sharpening = word_shift_left(decoder->sharp, 1);

			for (sample = (unsigned)pitch; sample < G729_SUBFRAME; sample++)
				code[sample] = word_add(code[sample],
					word_multiply(code[sample - pitch], sharpening));
		}
		gains_decode(decoder, parameters.gain[subframe], code, erased);
		decoder->sharp = decoder->gain_pitch;
		if (decoder->sharp > 13017)
			decoder->sharp = 13017;
		if (decoder->sharp < 3277)
			decoder->sharp = 3277;
		for (sample = 0; sample < G729_SUBFRAME; sample++) {
			int32_t total = long_multiply(
				excitation[subframe * G729_SUBFRAME + sample],
				decoder->gain_pitch);

			total = long_mac(total, code[sample], decoder->gain_code);
			total = long_shift_left(total, 1);
			excitation[subframe * G729_SUBFRAME + sample] = long_round(total);
		}
		decoder_synthesis_apply(decoder, lpc[subframe], subframe,
			output + subframe * G729_SUBFRAME);
	}
	if (!erased)
		decoder_energy_save(decoder);
	postfilter_apply(decoder, output, lpc, decoded_pitch, true);
	memmove(decoder->excitation, decoder->excitation + G729_FRAME,
		G729_EXCITATION_HISTORY * sizeof(decoder->excitation[0]));
	decoder->cng.past_speech = true;
}

static struct udsp_codec *g729_create(const struct udsp_codec_params *params)
{
	struct udsp_codec *codec;
	struct g729_state *state;

	g729_tables_init();
	codec = udsp_codec_alloc(sizeof(struct g729_state));
	if (!codec)
		return NULL;
	state = codec->state;
	state->encoder.dtx.enabled = params->dtx;
	encoder_reset(&state->encoder);
	decoder_reset(&state->decoder);
	return codec;
}

static void g729_reset(struct udsp_codec *codec)
{
	struct g729_state *state = codec->state;

	encoder_reset(&state->encoder);
	decoder_reset(&state->decoder);
}

static unsigned g729_rate_get(const struct udsp_codec *codec)
{
	(void)codec;
	return G729_RATE;
}

static int g729_encode(struct udsp_codec *codec, const int16_t *pcm, unsigned n,
		       uint8_t *out, size_t out_cap)
{
	struct g729_state *state = codec->state;

	if (n != G729_FRAME)
		return -EINVAL;
	if (out_cap < G729_BYTES)
		return -ENOBUFS;
	return encoder_frame_encode(&state->encoder, pcm, out);
}

static int g729_decode(struct udsp_codec *codec, const uint8_t *in, size_t len,
		       int16_t *pcm, unsigned pcm_cap)
{
	struct g729_state *state = codec->state;
	unsigned speech_frames;
	unsigned units;
	unsigned frame;
	bool sid;

	if (!len) {
		/* codec.h: no SID since reset leaves no noise to continue.
		 * The normative C code recovers a gain here, because its
		 * frame-type channel marks an erased first SID (clause B.4.5).
		 * RTP delivers only a gap, and the engine conceals an
		 * unsignaled gap. */
		if (!state->decoder.cng.sid_seen)
			return -EAGAIN;
		if (pcm_cap < G729_FRAME)
			return -ENOBUFS;
		decoder_cng_frame(&state->decoder, NULL, pcm);
		return G729_FRAME;
	}
	if (len % G729_BYTES == 0)
		sid = false;
	else if (len % G729_BYTES == G729_SID_BYTES)
		sid = true;
	else
		return -EINVAL;
	speech_frames = (unsigned)(len / G729_BYTES);
	units = speech_frames + (sid ? 1 : 0);
	if (units > 3)
		return -EINVAL;
	/* codec.h: a capacity shortfall is -ENOBUFS, never -EINVAL, which
	 * matches what G.722 returns. */
	if (pcm_cap < units * G729_FRAME)
		return -ENOBUFS;
	for (frame = 0; frame < speech_frames; frame++)
		decoder_frame_decode(&state->decoder, in + frame * G729_BYTES,
				     false, pcm + frame * G729_FRAME);
	/* RFC 3551: the SID closes the packet, so it is the trailing unit
	 * and it occupies one full frame of comfort-noise output. */
	if (sid)
		decoder_cng_frame(&state->decoder,
			in + speech_frames * G729_BYTES,
			pcm + speech_frames * G729_FRAME);
	return (int)(units * G729_FRAME);
}

bool g729_vad_decision_get(const struct udsp_codec *codec)
{
	const struct g729_state *state = codec->state;

	return state->encoder.vad.decision;
}

void g729_vad_frame_count_set(struct udsp_codec *codec, int16_t frame_count)
{
	struct g729_state *state = codec->state;

	state->encoder.vad.frame_count = frame_count;
}

void g729_levinson_probe(struct udsp_codec *codec, const int16_t *high,
			 const int16_t *low, int16_t reflection[2])
{
	struct g729_state *state = codec->state;
	int16_t lpc[G729_LPC_ORDER + 1];

	levinson_get(&state->encoder, high, low, lpc, NULL);
	memcpy(reflection, state->encoder.reflection_previous,
		2 * sizeof(*reflection));
}

static int g729_plc(struct udsp_codec *codec, int16_t *pcm, unsigned n,
		    const uint8_t *next, size_t next_len)
{
	struct g729_state *state = codec->state;

	(void)next;
	(void)next_len;
	if (n != G729_FRAME)
		return -EINVAL;
	/* Equation (B.27): an erasure during comfort noise is an untransmitted
	 * frame, so the noise continues; erased speech concealment would reset
	 * the generator seed and desynchronize the smoothing at the next SID. */
	if (!state->decoder.cng.past_speech) {
		decoder_cng_frame(&state->decoder, NULL, pcm);
		return G729_FRAME;
	}
	decoder_frame_decode(&state->decoder, NULL, true, pcm);
	return G729_FRAME;
}

const struct udsp_codec_ops udsp_codec_g729_ops = {
	.id = UDSP_CODEC_G729,
	.name = "g729",
	.sample_rate = G729_RATE,
	.clock_rate = G729_RATE,
	.frame_ms = 10,
	.ptime_mask = 0x07,
	.max_payload = 30,
	.frame_bytes = G729_BYTES,
	.sid_bytes = G729_SID_BYTES,
	.flags = UDSP_CODEC_F_PLC,
	.create = g729_create,
	.destroy = udsp_codec_free,
	.reset = g729_reset,
	.rate_get = g729_rate_get,
	.encode = g729_encode,
	.decode = g729_decode,
	.plc = g729_plc,
	.cn = udsp_codec_cn,
};
