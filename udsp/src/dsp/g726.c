// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"

struct g726_state {
	long yl;
	int yu;
	int dms;
	int dml;
	int ap;
	int a[2];
	int b[6];
	int pk[2];
	int dq[6];
	int sr[2];
	int td;
};

struct g726_rate {
	unsigned bits;
	const int *qtab;
	unsigned qtab_count;
	const int *dqln;
	const int *wi;
	const int *fi;
};

static const int power2[15] = {
	1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192,
	16384,
};

static const int qtab_16[] = { 261 };
static const int dqln_16[] = { 116, 365, 365, 116 };
static const int wi_16[] = { -704, 14048, 14048, -704 };
static const int fi_16[] = { 0, 0xe00, 0xe00, 0 };

static const int qtab_24[] = { 8, 218, 331 };
static const int dqln_24[] = { -2048, 135, 273, 373, 373, 273, 135, -2048 };
static const int wi_24[] = { -128, 960, 4384, 18624, 18624, 4384, 960, -128 };
static const int fi_24[] = { 0, 0x200, 0x400, 0xe00, 0xe00, 0x400, 0x200, 0 };

static const int qtab_32[] = { -124, 80, 178, 246, 300, 349, 400 };
static const int dqln_32[] = {
	-2048, 4, 135, 213, 273, 323, 373, 425,
	425, 373, 323, 273, 213, 135, 4, -2048,
};
static const int wi_32[] = {
	-384, 576, 1312, 2048, 3584, 6336, 11360, 35904,
	35904, 11360, 6336, 3584, 2048, 1312, 576, -384,
};
static const int fi_32[] = {
	0, 0, 0, 0x200, 0x200, 0x200, 0x600, 0xe00,
	0xe00, 0x600, 0x200, 0x200, 0x200, 0, 0, 0,
};

static const int qtab_40[] = {
	-122, -16, 68, 139, 198, 250, 298, 339, 378, 413, 445, 475, 502,
	528, 553,
};
static const int dqln_40[] = {
	-2048, -66, 28, 104, 169, 224, 274, 318,
	358, 395, 429, 459, 488, 514, 539, 566,
	566, 539, 514, 488, 459, 429, 395, 358,
	318, 274, 224, 169, 104, 28, -66, -2048,
};
static const int wi_40[] = {
	448, 448, 768, 1248, 1280, 1312, 1856, 3200,
	4512, 5728, 7008, 8960, 11456, 14080, 16928, 22272,
	22272, 16928, 14080, 11456, 8960, 7008, 5728, 4512,
	3200, 1856, 1312, 1280, 1248, 768, 448, 448,
};
static const int fi_40[] = {
	0, 0, 0, 0, 0, 0x200, 0x200, 0x200,
	0x200, 0x200, 0x400, 0x600, 0x800, 0xa00, 0xc00, 0xc00,
	0xc00, 0xc00, 0xa00, 0x800, 0x600, 0x400, 0x200, 0x200,
	0x200, 0x200, 0x200, 0, 0, 0, 0, 0,
};

static const struct g726_rate rates[] = {
	{ 2, qtab_16, 1, dqln_16, wi_16, fi_16 },
	{ 3, qtab_24, 3, dqln_24, wi_24, fi_24 },
	{ 4, qtab_32, 7, dqln_32, wi_32, fi_32 },
	{ 5, qtab_40, 15, dqln_40, wi_40, fi_40 },
};

static int table_index_get(int value, const int *table, unsigned size)
{
	unsigned i;

	for (i = 0; i < size; i++) {
		if (value < table[i])
			break;
	}
	return (int)i;
}

static int floating_multiply(int an, int srn)
{
	int magnitude = an > 0 ? an : ((-an) & 0x1fff);
	int exponent = table_index_get(magnitude, power2, 15) - 6;
	int mantissa;
	int result_exponent;
	int result_mantissa;
	int result;

	if (!magnitude)
		mantissa = 32;
	else if (exponent >= 0)
		mantissa = magnitude >> exponent;
	else
		mantissa = magnitude << -exponent;
	result_exponent = exponent + ((srn >> 6) & 0x0f) - 13;
	result_mantissa = (mantissa * (srn & 0x3f) + 0x30) >> 4;
	result = result_exponent >= 0 ?
		(result_mantissa << result_exponent) & 0x7fff :
		result_mantissa >> -result_exponent;
	return (an ^ srn) < 0 ? -result : result;
}

static int predictor_zero_get(const struct g726_state *state)
{
	int prediction = 0;
	unsigned i;

	for (i = 0; i < 6; i++)
		prediction += floating_multiply(state->b[i] >> 2, state->dq[i]);
	return (int16_t)prediction;
}

static int predictor_pole_get(const struct g726_state *state)
{
	return (int16_t)(floating_multiply(state->a[1] >> 2, state->sr[1]) +
		floating_multiply(state->a[0] >> 2, state->sr[0]));
}

static int step_size_get(const struct g726_state *state)
{
	int step;
	int difference;
	int coefficient;

	if (state->ap >= 256)
		return state->yu;
	step = (int)(state->yl >> 6);
	difference = state->yu - step;
	coefficient = state->ap >> 2;
	if (difference > 0)
		step += (difference * coefficient) >> 6;
	else
		step += (difference * coefficient + 0x3f) >> 6;
	return step;
}

static int difference_quantize(int difference, int step,
			       const struct g726_rate *rate)
{
	int magnitude = difference < 0 ? -difference : difference;
	int exponent = table_index_get(magnitude >> 1, power2, 15);
	int mantissa = ((magnitude << 7) >> exponent) & 0x7f;
	int logarithm = (exponent << 7) + mantissa;
	int normalized = logarithm - (step >> 2);
	int index = table_index_get(normalized, rate->qtab, rate->qtab_count);
	int size = (int)rate->qtab_count;

	if (difference < 0)
		return (size << 1) + 1 - index;
	/* The 16 kbit table has no unused codeword available for zero. */
	if (!index)
		return rate->bits == 2 ? 0 : (size << 1) + 1;
	return index;
}

static int difference_reconstruct(int sign, int logarithm, int step)
{
	int combined = logarithm + (step >> 2);
	int exponent;
	int mantissa;
	int difference;

	if (combined < 0)
		return sign ? -0x8000 : 0;
	exponent = (combined >> 7) & 0x0f;
	mantissa = 128 + (combined & 0x7f);
	difference = (mantissa << 7) >> (14 - exponent);
	return sign ? difference - 0x8000 : difference;
}

static int floating_value_get(int value)
{
	int magnitude;
	int exponent;
	int floating;

	if (!value)
		return 0x20;
	if (value == -32768)
		return (int16_t)0xfc20;
	magnitude = value > 0 ? value : -value;
	exponent = table_index_get(magnitude, power2, 15);
	floating = (exponent << 6) + ((magnitude << 6) >> exponent);
	return value < 0 ? floating - 0x400 : floating;
}

static void predictor_update(struct g726_state *state, int code_size, int step,
			     int wi, int fi, int difference, int reconstructed,
			     int pole_difference)
{
	int sign = pole_difference < 0;
	int magnitude = difference & 0x7fff;
	int threshold_log = (int)(state->yl >> 15);
	int threshold_fraction = (int)(state->yl >> 10) & 0x1f;
	int threshold = (32 + threshold_fraction) << threshold_log;
	int transition;
	int a2 = state->a[1];
	unsigned i;

	if (threshold_log > 9)
		threshold = 31 << 10;
	threshold = (threshold + (threshold >> 1)) >> 1;
	transition = state->td && magnitude > threshold;
	state->yu = step + ((wi - step) >> 5);
	if (state->yu < 544)
		state->yu = 544;
	if (state->yu > 5120)
		state->yu = 5120;
	state->yl += state->yu + ((-state->yl) >> 6);
	if (transition) {
		state->a[0] = 0;
		state->a[1] = 0;
		memset(state->b, 0, sizeof(state->b));
	} else {
		int signs_differ = sign ^ state->pk[0];

		a2 -= a2 >> 7;
		if (pole_difference) {
			int correction = signs_differ ? state->a[0] : -state->a[0];

			if (correction < -8191)
				a2 -= 0x100;
			else if (correction > 8191)
				a2 += 0xff;
			else
				a2 += correction >> 5;
			if (sign ^ state->pk[1]) {
				if (a2 <= -12160)
					a2 = -12288;
				else if (a2 >= 12416)
					a2 = 12288;
				else
					a2 -= 0x80;
			} else if (a2 <= -12416) {
				a2 = -12288;
			} else if (a2 >= 12160) {
				a2 = 12288;
			} else {
				a2 += 0x80;
			}
		}
		state->a[1] = a2;
		state->a[0] -= state->a[0] >> 8;
		if (pole_difference)
			state->a[0] += signs_differ ? -192 : 192;
		if (state->a[0] > 15360 - a2)
			state->a[0] = 15360 - a2;
		if (state->a[0] < a2 - 15360)
			state->a[0] = a2 - 15360;
		for (i = 0; i < 6; i++) {
			state->b[i] -= state->b[i] >> (code_size == 5 ? 9 : 8);
			if (difference & 0x7fff)
				state->b[i] += (difference ^ state->dq[i]) >= 0 ?
					128 : -128;
		}
	}
	for (i = 5; i > 0; i--)
		state->dq[i] = state->dq[i - 1];
	if (!(difference & 0x7fff))
		state->dq[0] = difference < 0 ? (int16_t)0xfc20 : 0x20;
	else
		state->dq[0] = floating_value_get(difference < 0 ?
			-(difference & 0x7fff) : difference);
	state->sr[1] = state->sr[0];
	state->sr[0] = floating_value_get(reconstructed);
	state->pk[1] = state->pk[0];
	state->pk[0] = sign;
	state->td = transition ? 0 : a2 < -11776;
	state->dms += (fi - state->dms) >> 5;
	state->dml += ((fi << 2) - state->dml) >> 7;
	if (transition)
		state->ap = 256;
	else if (step < 1536 || state->td ||
		 abs((state->dms << 2) - state->dml) >= state->dml >> 3)
		state->ap += (0x200 - state->ap) >> 4;
	else
		state->ap += (-state->ap) >> 4;
}

static void g726_state_reset(struct g726_state *state)
{
	unsigned i;

	memset(state, 0, sizeof(*state));
	state->yl = 34816;
	state->yu = 544;
	for (i = 0; i < 6; i++)
		state->dq[i] = 32;
	state->sr[0] = 32;
	state->sr[1] = 32;
}

static const struct g726_rate *rate_get(unsigned bits)
{
	if (bits < 2 || bits > 5)
		return NULL;
	return &rates[bits - 2];
}

static unsigned sample_encode(struct g726_state *state,
			      const struct g726_rate *rate, int16_t sample)
{
	int zero = predictor_zero_get(state);
	int pole = predictor_pole_get(state);
	int estimate = (int16_t)(zero + pole) >> 1;
	int difference = (sample >> 2) - estimate;
	int step = step_size_get(state);
	unsigned code = difference_quantize(difference, step, rate);
	int dq = difference_reconstruct(code & (1U << (rate->bits - 1)),
					rate->dqln[code], step);
	/* dq adds as a sign and a 14-bit magnitude, so the predictor state
	 * stays bit-exact with the far end's decoder. */
	int reconstructed = dq < 0 ? estimate - (dq & 0x3fff) : estimate + dq;

	predictor_update(state, rate->bits, step, rate->wi[code], rate->fi[code],
			 dq, reconstructed, reconstructed + (zero >> 1) - estimate);
	return code;
}

static int16_t sample_decode(struct g726_state *state,
			     const struct g726_rate *rate, unsigned code)
{
	int zero = predictor_zero_get(state);
	int pole = predictor_pole_get(state);
	int estimate = (int16_t)(zero + pole) >> 1;
	int step = step_size_get(state);
	int dq = difference_reconstruct(code & (1U << (rate->bits - 1)),
					rate->dqln[code], step);
	/* The reconstruction of sample_encode, so the predictor state stays
	 * bit-exact with the far end's encoder. */
	int reconstructed = dq < 0 ? estimate - (dq & 0x3fff) : estimate + dq;
	int output = reconstructed * 4;

	predictor_update(state, rate->bits, step, rate->wi[code], rate->fi[code],
			 dq, reconstructed, reconstructed + (zero >> 1) - estimate);
	if (output > 32767)
		return 32767;
	if (output < -32768)
		return -32768;
	return (int16_t)output;
}

static struct udsp_codec *g726_create_bits(unsigned bits)
{
	struct udsp_codec *codec = udsp_codec_alloc(sizeof(struct g726_state));

	if (!codec)
		return NULL;
	codec->bits = bits;
	g726_state_reset(codec->state);
	return codec;
}

static struct udsp_codec *g726_create(unsigned bits,
				      const struct udsp_codec_params *params)
{
	(void)params;
	return g726_create_bits(bits);
}

static struct udsp_codec *g726_16_create(const struct udsp_codec_params *params)
{
	return g726_create(2, params);
}

static struct udsp_codec *g726_24_create(const struct udsp_codec_params *params)
{
	return g726_create(3, params);
}

static struct udsp_codec *g726_32_create(const struct udsp_codec_params *params)
{
	return g726_create(4, params);
}

static struct udsp_codec *g726_40_create(const struct udsp_codec_params *params)
{
	return g726_create(5, params);
}

static void g726_reset(struct udsp_codec *codec)
{
	g726_state_reset(codec->state);
	codec->cn_seed = 1;
}

static unsigned g726_rate_get(const struct udsp_codec *codec)
{
	(void)codec;
	return UDSP_SAMPLE_RATE;
}

static int g726_encode(struct udsp_codec *codec, const int16_t *pcm, unsigned n,
		       uint8_t *out, size_t out_cap)
{
	struct g726_state *state = codec->state;
	const struct g726_rate *rate = rate_get(codec->bits);
	size_t expected = UDSP_FRAME_SAMPLES * codec->bits / 8;
	uint64_t accumulator = 0;
	unsigned held = 0;
	size_t bytes = 0;
	unsigned i;

	if (n != UDSP_FRAME_SAMPLES)
		return -EINVAL;
	if (expected > out_cap)
		return -ENOBUFS;
	for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
		accumulator |= (uint64_t)sample_encode(state, rate, pcm[i]) << held;
		held += codec->bits;
		while (held >= 8) {
			out[bytes++] = accumulator & 0xff;
			accumulator >>= 8;
			held -= 8;
		}
	}
	if (held)
		out[bytes++] = accumulator & 0xff;
	return (int)bytes;
}

static int g726_decode(struct udsp_codec *codec, const uint8_t *in, size_t len,
		       int16_t *pcm, unsigned pcm_cap)
{
	struct g726_state *state = codec->state;
	const struct g726_rate *rate = rate_get(codec->bits);
	size_t expected = UDSP_FRAME_SAMPLES * codec->bits / 8;
	uint64_t accumulator = 0;
	unsigned held = 0;
	size_t offset = 0;
	unsigned i;

	if (len != expected)
		return -EINVAL;
	if (UDSP_FRAME_SAMPLES > pcm_cap)
		return -ENOBUFS;
	for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
		while (held < codec->bits) {
			accumulator |= (uint64_t)in[offset++] << held;
			held += 8;
		}
		pcm[i] = sample_decode(state, rate,
			accumulator & ((1U << codec->bits) - 1));
		accumulator >>= codec->bits;
		held -= codec->bits;
	}
	return UDSP_FRAME_SAMPLES;
}

#define G726_OPS(symbol, codec_id, codec_name, create_fn, payload, frame) \
	const struct udsp_codec_ops symbol = { \
		.id = codec_id, .name = codec_name, \
		.sample_rate = UDSP_SAMPLE_RATE, .clock_rate = UDSP_SAMPLE_RATE, \
		.frame_ms = 10, .ptime_mask = 0x07, .max_payload = payload, \
		.frame_bytes = frame, .sid_bytes = 0, \
		.flags = 0, .create = create_fn, \
		.destroy = udsp_codec_free, .reset = g726_reset, \
		.rate_get = g726_rate_get, \
		.encode = g726_encode, .decode = g726_decode, \
		.plc = NULL, .cn = udsp_codec_cn, \
	}

G726_OPS(udsp_codec_g726_16_ops, UDSP_CODEC_G726_16, "G726-16",
	 g726_16_create, 60, 20);
G726_OPS(udsp_codec_g726_24_ops, UDSP_CODEC_G726_24, "G726-24",
	 g726_24_create, 90, 30);
G726_OPS(udsp_codec_g726_32_ops, UDSP_CODEC_G726_32, "G726-32",
	 g726_32_create, 120, 40);
G726_OPS(udsp_codec_g726_40_ops, UDSP_CODEC_G726_40, "G726-40",
	 g726_40_create, 150, 50);
