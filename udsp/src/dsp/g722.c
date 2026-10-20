// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"
#include "plc.h"

#define G722_RATE	16000U
#define G722_FRAME	160U
#define G722_BYTES	80U

struct g722_band {
	int s;
	int sz;
	int r[3];
	int a[3];
	int p[3];
	int d[7];
	int b[7];
	int nb;
	int det;
};

struct g722_adpcm {
	struct g722_band band[2];
	int16_t qmf[24];
};

struct g722_state {
	struct g722_adpcm encoder;
	struct g722_adpcm decoder;
	struct udsp_plc plc;
};

static const int qmf_coeff[12] = {
	3, -11, 12, 32, -210, 951, 3876, -805, 362, -156, 53, -11,
};

static const int quantizer_level[32] = {
	0, 35, 72, 110, 150, 190, 233, 276,
	323, 370, 422, 473, 530, 587, 650, 714,
	786, 858, 940, 1023, 1121, 1219, 1339, 1458,
	1612, 1765, 1980, 2195, 2557, 2919, 0, 0,
};

static const uint8_t lower_code_negative[32] = {
	0, 63, 62, 31, 30, 29, 28, 27,
	26, 25, 24, 23, 22, 21, 20, 19,
	18, 17, 16, 15, 14, 13, 12, 11,
	10, 9, 8, 7, 6, 5, 4, 0,
};

static const uint8_t lower_code_positive[32] = {
	0, 61, 60, 59, 58, 57, 56, 55,
	54, 53, 52, 51, 50, 49, 48, 47,
	46, 45, 44, 43, 42, 41, 40, 39,
	38, 37, 36, 35, 34, 33, 32, 0,
};

static const int inverse_low_4[16] = {
	0, -20456, -12896, -8968, -6288, -4240, -2584, -1200,
	20456, 12896, 8968, 6288, 4240, 2584, 1200, 0,
};

static const int inverse_low_6[64] = {
	-136, -136, -136, -136, -24808, -21904, -19008, -16704,
	-14984, -13512, -12280, -11192, -10232, -9360, -8576, -7856,
	-7192, -6576, -6000, -5456, -4944, -4464, -4008, -3576,
	-3168, -2776, -2400, -2032, -1688, -1360, -1040, -728,
	24808, 21904, 19008, 16704, 14984, 13512, 12280, 11192,
	10232, 9360, 8576, 7856, 7192, 6576, 6000, 5456,
	4944, 4464, 4008, 3576, 3168, 2776, 2400, 2032,
	1688, 1360, 1040, 728, 432, 136, -432, -136,
};

static const int inverse_high_2[4] = { -7408, -1616, 7408, 1616 };
static const int lower_log_delta[8] = { -60, -30, 58, 172, 334, 538, 1198, 3042 };
static const int higher_log_delta[3] = { 0, -214, 798 };
static const uint8_t lower_region[16] = {
	0, 7, 6, 5, 4, 3, 2, 1, 7, 6, 5, 4, 3, 2, 1, 0,
};
static const uint8_t higher_region[4] = { 2, 1, 2, 1 };
static const uint8_t higher_code_negative[3] = { 0, 1, 0 };
static const uint8_t higher_code_positive[3] = { 0, 3, 2 };
static const int log_scale[32] = {
	2048, 2093, 2139, 2186, 2233, 2282, 2332, 2383,
	2435, 2489, 2543, 2599, 2656, 2714, 2774, 2834,
	2896, 2960, 3025, 3091, 3158, 3228, 3298, 3371,
	3444, 3520, 3597, 3676, 3756, 3838, 3922, 4008,
};

static int value_saturate(int64_t value)
{
	if (value > 32767)
		return 32767;
	if (value < -32768)
		return -32768;
	return (int)value;
}

static int value_saturate15(int value)
{
	if (value > 16383)
		return 16383;
	if (value < -16384)
		return -16384;
	return value;
}

static void band_reset(struct g722_band *band, int det)
{
	memset(band, 0, sizeof(*band));
	band->det = det;
}

static void adpcm_reset(struct g722_adpcm *adpcm)
{
	memset(adpcm, 0, sizeof(*adpcm));
	band_reset(&adpcm->band[0], 32);
	band_reset(&adpcm->band[1], 8);
}

static void predictor_update(struct g722_band *band, int difference)
{
	int ap[3];
	int bp[7];
	int sg[7];
	int sp;
	int wd1;
	int wd2;
	int wd3;
	unsigned i;

	band->d[0] = difference;
	band->r[0] = value_saturate((int64_t)band->s + difference);
	band->p[0] = value_saturate((int64_t)band->sz + difference);
	for (i = 0; i < 3; i++)
		sg[i] = band->p[i] < 0;
	wd1 = value_saturate((int64_t)band->a[1] * 4);
	wd2 = sg[0] == sg[1] ? -wd1 : wd1;
	if (wd2 > 32767)
		wd2 = 32767;
	wd3 = sg[0] == sg[2] ? 128 : -128;
	wd3 += wd2 >> 7;
	wd3 += (band->a[2] * 32512) >> 15;
	ap[2] = value_saturate(wd3);
	if (ap[2] > 12288)
		ap[2] = 12288;
	if (ap[2] < -12288)
		ap[2] = -12288;
	wd1 = sg[0] == sg[1] ? 192 : -192;
	wd2 = (band->a[1] * 32640) >> 15;
	ap[1] = value_saturate(wd1 + wd2);
	wd3 = value_saturate(15360 - ap[2]);
	if (ap[1] > wd3)
		ap[1] = wd3;
	if (ap[1] < -wd3)
		ap[1] = -wd3;
	wd1 = difference ? 128 : 0;
	sg[0] = difference < 0;
	for (i = 1; i < 7; i++) {
		sg[i] = band->d[i] < 0;
		wd2 = sg[0] == sg[i] ? wd1 : -wd1;
		wd3 = (band->b[i] * 32640) >> 15;
		bp[i] = value_saturate(wd2 + wd3);
	}
	for (i = 6; i > 0; i--) {
		band->d[i] = band->d[i - 1];
		band->b[i] = bp[i];
	}
	for (i = 2; i > 0; i--) {
		band->r[i] = band->r[i - 1];
		band->p[i] = band->p[i - 1];
		band->a[i] = ap[i];
	}
	wd1 = value_saturate((int64_t)band->r[1] * 2);
	wd1 = (band->a[1] * wd1) >> 15;
	wd2 = value_saturate((int64_t)band->r[2] * 2);
	wd2 = (band->a[2] * wd2) >> 15;
	sp = value_saturate(wd1 + wd2);
	band->sz = 0;
	for (i = 1; i < 7; i++) {
		wd1 = value_saturate((int64_t)band->d[i] * 2);
		band->sz += (band->b[i] * wd1) >> 15;
	}
	band->sz = value_saturate(band->sz);
	band->s = value_saturate((int64_t)sp + band->sz);
}

static void scale_update(struct g722_band *band, int delta, int maximum,
			 unsigned exponent_base)
{
	int wd1;
	int wd2;
	int wd3;

	band->nb = ((band->nb * 127) >> 7) + delta;
	if (band->nb < 0)
		band->nb = 0;
	if (band->nb > maximum)
		band->nb = maximum;
	wd1 = (band->nb >> 6) & 31;
	wd2 = (int)exponent_base - (band->nb >> 11);
	wd3 = wd2 < 0 ? log_scale[wd1] * (1 << -wd2) :
		log_scale[wd1] >> wd2;
	band->det = wd3 * 4;
}

static unsigned lower_encode(struct g722_band *band, int sample)
{
	int difference = value_saturate((int64_t)sample - band->s);
	int magnitude = difference < 0 ? -difference - 1 : difference;
	unsigned interval;
	unsigned code;
	unsigned reduced;
	int reconstructed;

	for (interval = 1; interval < 30; interval++) {
		int threshold = (quantizer_level[interval] * band->det) >> 12;

		if (magnitude < threshold)
			break;
	}
	code = difference < 0 ? lower_code_negative[interval] :
		lower_code_positive[interval];
	reduced = code >> 2;
	reconstructed = (band->det * inverse_low_4[reduced]) >> 15;
	scale_update(band, lower_log_delta[lower_region[reduced]], 18432, 8);
	predictor_update(band, reconstructed);
	return code;
}

static unsigned higher_encode(struct g722_band *band, int sample)
{
	int difference = value_saturate((int64_t)sample - band->s);
	int magnitude = difference < 0 ? -difference - 1 : difference;
	int threshold = (564 * band->det) >> 12;
	unsigned interval = magnitude >= threshold ? 2 : 1;
	unsigned code = difference < 0 ? higher_code_negative[interval] :
		higher_code_positive[interval];
	int reconstructed = (band->det * inverse_high_2[code]) >> 15;

	scale_update(band, higher_log_delta[higher_region[code]], 22528, 10);
	predictor_update(band, reconstructed);
	return code;
}

static void analysis_sample(struct g722_adpcm *adpcm, const int16_t *pcm,
			    int *low, int *high)
{
	int64_t even = 0;
	int64_t odd = 0;
	unsigned i;

	memmove(adpcm->qmf, adpcm->qmf + 2, 22 * sizeof(adpcm->qmf[0]));
	adpcm->qmf[22] = pcm[0];
	adpcm->qmf[23] = pcm[1];
	for (i = 0; i < 12; i++) {
		even += (int64_t)adpcm->qmf[2 * i] * qmf_coeff[i];
		odd += (int64_t)adpcm->qmf[2 * i + 1] * qmf_coeff[11 - i];
	}
	*low = value_saturate((even + odd) >> 14);
	*high = value_saturate((odd - even) >> 14);
}

static uint8_t sample_encode(struct g722_adpcm *adpcm, const int16_t *pcm)
{
	int low;
	int high;
	unsigned low_code;
	unsigned high_code;

	analysis_sample(adpcm, pcm, &low, &high);
	low_code = lower_encode(&adpcm->band[0], low);
	high_code = higher_encode(&adpcm->band[1], high);
	return (uint8_t)(low_code | (high_code << 6));
}

static int lower_decode(struct g722_band *band, unsigned code)
{
	unsigned reduced = code >> 2;
	int output_difference = (band->det * inverse_low_6[code]) >> 15;
	int predictor_difference = (band->det * inverse_low_4[reduced]) >> 15;
	int output = value_saturate15(band->s + output_difference);

	scale_update(band, lower_log_delta[lower_region[reduced]], 18432, 8);
	predictor_update(band, predictor_difference);
	return output;
}

static int higher_decode(struct g722_band *band, unsigned code)
{
	int difference = (band->det * inverse_high_2[code]) >> 15;
	int output = value_saturate15(band->s + difference);

	scale_update(band, higher_log_delta[higher_region[code]], 22528, 10);
	predictor_update(band, difference);
	return output;
}

static void synthesis_sample(struct g722_adpcm *adpcm, int low, int high,
			     int16_t *pcm)
{
	int64_t first = 0;
	int64_t second = 0;
	unsigned i;

	memmove(adpcm->qmf, adpcm->qmf + 2, 22 * sizeof(adpcm->qmf[0]));
	adpcm->qmf[22] = value_saturate((int64_t)low + high);
	adpcm->qmf[23] = value_saturate((int64_t)low - high);
	for (i = 0; i < 12; i++) {
		second += (int64_t)adpcm->qmf[2 * i] * qmf_coeff[i];
		first += (int64_t)adpcm->qmf[2 * i + 1] * qmf_coeff[11 - i];
	}
	/*
	 * The shift is 11, not 12: the QMF has a DC gain of 4096 and the
	 * G.722 algorithm takes a 15-bit input. The output therefore runs one
	 * bit hotter than the ITU domain and passes 16 bits by about 15
	 * percent at full scale, so it needs the clamp.
	 */
	pcm[0] = value_saturate(first >> 11);
	pcm[1] = value_saturate(second >> 11);
}

static void sample_decode(struct g722_adpcm *adpcm, uint8_t code, int16_t *pcm)
{
	int low = lower_decode(&adpcm->band[0], code & 0x3f);
	int high = higher_decode(&adpcm->band[1], code >> 6);

	synthesis_sample(adpcm, low, high, pcm);
}

static struct udsp_codec *g722_create(const struct udsp_codec_params *params)
{
	struct udsp_codec *codec = udsp_codec_alloc(sizeof(struct g722_state));
	struct g722_state *state;

	(void)params;
	if (!codec)
		return NULL;
	state = codec->state;
	adpcm_reset(&state->encoder);
	adpcm_reset(&state->decoder);
	udsp_plc_init(&state->plc, G722_RATE, G722_FRAME);
	return codec;
}

static void g722_reset(struct udsp_codec *codec)
{
	struct g722_state *state = codec->state;

	adpcm_reset(&state->encoder);
	adpcm_reset(&state->decoder);
	udsp_plc_reset(&state->plc);
}

static unsigned g722_rate_get(const struct udsp_codec *codec)
{
	(void)codec;
	return G722_RATE;
}

static int g722_encode(struct udsp_codec *codec, const int16_t *pcm, unsigned n,
		       uint8_t *out, size_t out_cap)
{
	struct g722_state *state = codec->state;
	unsigned i;

	if (n != G722_FRAME)
		return -EINVAL;
	if (out_cap < G722_BYTES)
		return -ENOBUFS;
	for (i = 0; i < G722_BYTES; i++)
		out[i] = sample_encode(&state->encoder, pcm + 2 * i);
	return G722_BYTES;
}

static int g722_decode(struct udsp_codec *codec, const uint8_t *in, size_t len,
		       int16_t *pcm, unsigned pcm_cap)
{
	struct g722_state *state = codec->state;
	unsigned i;

	if (len != G722_BYTES)
		return -EINVAL;
	if (pcm_cap < G722_FRAME)
		return -ENOBUFS;
	for (i = 0; i < G722_BYTES; i++)
		sample_decode(&state->decoder, in[i], pcm + 2 * i);
	udsp_plc_frame_process(&state->plc, pcm);
	return G722_FRAME;
}

static int g722_plc(struct udsp_codec *codec, int16_t *pcm, unsigned n,
		    const uint8_t *next, size_t next_len)
{
	struct g722_state *state = codec->state;

	(void)next;
	(void)next_len;
	if (n != G722_FRAME)
		return -EINVAL;
	udsp_plc_generate(&state->plc, pcm);
	return G722_FRAME;
}

const struct udsp_codec_ops udsp_codec_g722_ops = {
	.id = UDSP_CODEC_G722,
	.name = "g722",
	.sample_rate = G722_RATE,
	.clock_rate = 8000,
	.frame_ms = 10,
	.ptime_mask = 0x07,
	.max_payload = 240,
	.frame_bytes = G722_BYTES,
	.sid_bytes = 0,
	.flags = UDSP_CODEC_F_PLC,
	.create = g722_create,
	.destroy = udsp_codec_free,
	.reset = g722_reset,
	.rate_get = g722_rate_get,
	.encode = g722_encode,
	.decode = g722_decode,
	.plc = g722_plc,
	.cn = udsp_codec_cn,
};
