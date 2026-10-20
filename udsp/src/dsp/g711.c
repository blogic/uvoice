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
#include "plc.h"

/*
 * plc_want is the create parameter plc_off, held in the sense the decode path
 * reads. The generic udsp_plc at the 8 kHz, 80 sample geometry keeps a 30
 * sample delay line inside the history it fills on every decode, and the frame
 * it returns is that late; the recovery cross-fade then blends the synthetic
 * pitch into the first frame after a loss. Both belong to speech, so a stream
 * that carries a data carrier creates the instance with plc_off set and the
 * decoder keeps no history at all: the samples the far end demodulates leave
 * this codec exactly as they arrived.
 */
struct g711_state {
	struct udsp_plc plc;
	bool alaw;
	bool plc_want;
};

int16_t udsp_ulaw_decode(uint8_t code)
{
	int magnitude;

	code = (uint8_t)~code;
	magnitude = ((code & 0x0f) << 3) + 0x84;
	magnitude <<= (code >> 4) & 0x07;
	return (code & 0x80) ? (int16_t)(0x84 - magnitude) :
		(int16_t)(magnitude - 0x84);
}

uint8_t udsp_ulaw_encode(int16_t sample)
{
	int magnitude = sample;
	unsigned exponent = 0;
	unsigned mantissa;
	unsigned sign = 0;

	if (magnitude < 0) {
		sign = 0x80;
		magnitude = -magnitude;
	}
	if (magnitude > 32635)
		magnitude = 32635;
	magnitude += 0x84;
	while (exponent < 7 && magnitude >= (0x100 << exponent))
		exponent++;
	mantissa = ((unsigned)magnitude >> (exponent + 3)) & 0x0f;
	return (uint8_t)~(sign | (exponent << 4) | mantissa);
}

int16_t udsp_alaw_decode(uint8_t code)
{
	int magnitude;
	unsigned exponent;

	code ^= 0x55;
	magnitude = ((code & 0x0f) << 4) + 8;
	exponent = (code >> 4) & 0x07;
	if (exponent)
		magnitude += 0x100;
	if (exponent > 1)
		magnitude <<= exponent - 1;
	return (code & 0x80) ? (int16_t)magnitude : (int16_t)-magnitude;
}

uint8_t udsp_alaw_encode(int16_t sample)
{
	int magnitude = sample;
	unsigned exponent = 0;
	unsigned mantissa;
	unsigned sign = 0x80;

	if (magnitude < 0) {
		sign = 0;
		magnitude = -magnitude - 1;
	}
	if (magnitude > 32767)
		magnitude = 32767;
	if (magnitude >= 256) {
		int scaled = magnitude >> 8;

		exponent = 1;
		while (scaled >>= 1)
			exponent++;
		mantissa = ((unsigned)magnitude >> (exponent + 3)) & 0x0f;
	} else {
		mantissa = (unsigned)magnitude >> 4;
	}
	return (uint8_t)((sign | (exponent << 4) | mantissa) ^ 0x55);
}

static struct udsp_codec *g711_create(bool alaw,
				      const struct udsp_codec_params *params)
{
	struct udsp_codec *codec = udsp_codec_alloc(sizeof(struct g711_state));
	struct g711_state *state;

	if (!codec)
		return NULL;
	state = codec->state;
	state->alaw = alaw;
	state->plc_want = !params->plc_off;
	udsp_plc_init(&state->plc, UDSP_SAMPLE_RATE, UDSP_FRAME_SAMPLES);
	return codec;
}

static struct udsp_codec *pcmu_create(const struct udsp_codec_params *params)
{
	return g711_create(false, params);
}

static struct udsp_codec *pcma_create(const struct udsp_codec_params *params)
{
	return g711_create(true, params);
}

static void g711_reset(struct udsp_codec *codec)
{
	struct g711_state *state = codec->state;

	udsp_plc_reset(&state->plc);
	codec->cn_seed = 1;
}

static unsigned g711_rate_get(const struct udsp_codec *codec)
{
	(void)codec;
	return UDSP_SAMPLE_RATE;
}

static int g711_encode(struct udsp_codec *codec, const int16_t *pcm, unsigned n,
		       uint8_t *out, size_t out_cap)
{
	struct g711_state *state = codec->state;
	unsigned i;

	if (n != UDSP_FRAME_SAMPLES)
		return -EINVAL;
	if (UDSP_FRAME_SAMPLES > out_cap)
		return -ENOBUFS;
	for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
		out[i] = state->alaw ? udsp_alaw_encode(pcm[i]) :
			udsp_ulaw_encode(pcm[i]);
	return UDSP_FRAME_SAMPLES;
}

static int g711_decode(struct udsp_codec *codec, const uint8_t *in, size_t len,
		       int16_t *pcm, unsigned pcm_cap)
{
	struct g711_state *state = codec->state;
	unsigned i;

	if (len != UDSP_FRAME_SAMPLES)
		return -EINVAL;
	if (UDSP_FRAME_SAMPLES > pcm_cap)
		return -ENOBUFS;
	for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
		pcm[i] = state->alaw ? udsp_alaw_decode(in[i]) :
			udsp_ulaw_decode(in[i]);
	if (state->plc_want)
		udsp_plc_frame_process(&state->plc, pcm);
	return UDSP_FRAME_SAMPLES;
}

static int g711_plc(struct udsp_codec *codec, int16_t *pcm, unsigned n,
		    const uint8_t *next, size_t next_len)
{
	struct g711_state *state = codec->state;

	(void)next;
	(void)next_len;
	if (n != UDSP_FRAME_SAMPLES)
		return -EINVAL;
	udsp_plc_generate(&state->plc, pcm);
	return (int)n;
}

const struct udsp_codec_ops udsp_codec_pcmu_ops = {
	.id = UDSP_CODEC_PCMU,
	.name = "PCMU",
	.sample_rate = UDSP_SAMPLE_RATE,
	.clock_rate = UDSP_SAMPLE_RATE,
	.frame_ms = 10,
	.ptime_mask = 0x07,
	.max_payload = 240,
	.frame_bytes = 80,
	.sid_bytes = 0,
	.flags = UDSP_CODEC_F_PLC,
	.create = pcmu_create,
	.destroy = udsp_codec_free,
	.reset = g711_reset,
	.rate_get = g711_rate_get,
	.encode = g711_encode,
	.decode = g711_decode,
	.plc = g711_plc,
	.cn = udsp_codec_cn,
};

const struct udsp_codec_ops udsp_codec_pcma_ops = {
	.id = UDSP_CODEC_PCMA,
	.name = "PCMA",
	.sample_rate = UDSP_SAMPLE_RATE,
	.clock_rate = UDSP_SAMPLE_RATE,
	.frame_ms = 10,
	.ptime_mask = 0x07,
	.max_payload = 240,
	.frame_bytes = 80,
	.sid_bytes = 0,
	.flags = UDSP_CODEC_F_PLC,
	.create = pcma_create,
	.destroy = udsp_codec_free,
	.reset = g711_reset,
	.rate_get = g711_rate_get,
	.encode = g711_encode,
	.decode = g711_decode,
	.plc = g711_plc,
	.cn = udsp_codec_cn,
};

static const struct udsp_codec_ops *const builtin_ops[] = {
	&udsp_codec_pcmu_ops,
	&udsp_codec_pcma_ops,
	&udsp_codec_g726_16_ops,
	&udsp_codec_g726_24_ops,
	&udsp_codec_g726_32_ops,
	&udsp_codec_g726_40_ops,
	&udsp_codec_g722_ops,
	&udsp_codec_g729_ops,
#ifdef HAVE_OPUS
	&udsp_codec_opus_ops,
#endif
	NULL,
};

const struct udsp_codec_descriptor udsp_codec_builtin = {
	.abi_version = UDSP_CODEC_ABI_VERSION,
	.struct_size = sizeof(struct udsp_codec_ops),
	.ops = builtin_ops,
};
