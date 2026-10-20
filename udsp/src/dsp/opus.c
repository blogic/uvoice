// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <opus/opus.h>

#include "dsp.h"

#define OPUS_BITRATE_DEFAULT	32000U
#define OPUS_PTIME_DEFAULT	20U
/* A fixed estimate; no measured loss rate from RTCP reaches the encoder. */
#define OPUS_PACKET_LOSS_PERC	15

struct udsp_opus_state {
	OpusEncoder *encoder;
	OpusDecoder *decoder;
	unsigned rate;
	unsigned packet_samples;
	uint32_t bitrate;
	uint8_t complexity;
	int bandwidth;
	bool fec;
	int encoder_error;
	int decoder_error;
};

static int opus_errno_get(int error)
{
	if (error == OPUS_BUFFER_TOO_SMALL)
		return -ENOBUFS;
	if (error == OPUS_BAD_ARG || error == OPUS_INVALID_PACKET)
		return -EINVAL;
	if (error == OPUS_ALLOC_FAIL)
		return -ENOMEM;
	return -EIO;
}

static bool ptime_valid(unsigned ptime_ms)
{
	return ptime_ms == 10 || ptime_ms == 20;
}

static int bandwidth_get(unsigned rate, uint8_t bandwidth)
{
	/* libopus 1.5.2 never emits mediumband packets, so MB behaves as WB. */
	static const int opus_bandwidth[] = {
		OPUS_AUTO,
		OPUS_BANDWIDTH_NARROWBAND,
		OPUS_BANDWIDTH_MEDIUMBAND,
		OPUS_BANDWIDTH_WIDEBAND,
	};

	if (bandwidth > UDSP_OPUS_BW_WB)
		return 0;
	if (bandwidth != UDSP_OPUS_BW_AUTO)
		return opus_bandwidth[bandwidth];
	if (rate == 8000)
		return OPUS_BANDWIDTH_NARROWBAND;
	return OPUS_BANDWIDTH_WIDEBAND;
}

static int encoder_configure(struct udsp_opus_state *state)
{
	int error;

	error = opus_encoder_ctl(state->encoder,
				 OPUS_SET_BITRATE((opus_int32)state->bitrate));
	if (error != OPUS_OK)
		return error;
	error = opus_encoder_ctl(state->encoder, OPUS_SET_VBR(1));
	if (error != OPUS_OK)
		return error;
	error = opus_encoder_ctl(state->encoder,
				 OPUS_SET_MAX_BANDWIDTH(state->bandwidth));
	if (error != OPUS_OK)
		return error;
	if (state->complexity) {
		error = opus_encoder_ctl(state->encoder,
					 OPUS_SET_COMPLEXITY(state->complexity));
		if (error != OPUS_OK)
			return error;
	}
	error = opus_encoder_ctl(state->encoder,
				 OPUS_SET_INBAND_FEC(state->fec));
	if (error != OPUS_OK)
		return error;
	error = opus_encoder_ctl(state->encoder,
		OPUS_SET_PACKET_LOSS_PERC(state->fec ? OPUS_PACKET_LOSS_PERC : 0));
	if (error != OPUS_OK)
		return error;
	return opus_encoder_ctl(state->encoder, OPUS_SET_DTX(0));
}

static void opus_destroy(struct udsp_codec *codec)
{
	struct udsp_opus_state *state;

	if (!codec)
		return;
	state = codec->state;
	if (state) {
		if (state->decoder)
			opus_decoder_destroy(state->decoder);
		if (state->encoder)
			opus_encoder_destroy(state->encoder);
	}
	free(state);
	free(codec);
}

static struct udsp_codec *opus_create(const struct udsp_codec_params *params)
{
	struct udsp_codec *codec;
	struct udsp_opus_state *state;
	unsigned ptime_ms = params->ptime_ms ? params->ptime_ms : OPUS_PTIME_DEFAULT;
	unsigned rate = params->line_rate ? params->line_rate : 8000;
	int bandwidth;
	int error;

	if ((rate != 8000 && rate != 16000) || !ptime_valid(ptime_ms))
		return NULL;
	if (params->complexity > 10 || params->bitrate > INT_MAX)
		return NULL;
	bandwidth = bandwidth_get(rate, params->max_bandwidth);
	if (!bandwidth)
		return NULL;
	codec = udsp_codec_alloc(sizeof(struct udsp_opus_state));
	if (!codec)
		return NULL;
	state = codec->state;
	state->rate = rate;
	state->packet_samples = rate * ptime_ms / 1000;
	state->bitrate = params->bitrate ? params->bitrate : OPUS_BITRATE_DEFAULT;
	state->complexity = params->complexity;
	state->bandwidth = bandwidth;
	state->fec = params->fec;
	state->encoder = opus_encoder_create(rate, 1, OPUS_APPLICATION_VOIP, &error);
	if (!state->encoder || error != OPUS_OK)
		goto fail;
	state->decoder = opus_decoder_create(rate, 1, &error);
	if (!state->decoder || error != OPUS_OK)
		goto fail;
	if (encoder_configure(state) != OPUS_OK)
		goto fail;
	if (!state->complexity) {
		opus_int32 complexity;

		error = opus_encoder_ctl(state->encoder,
					 OPUS_GET_COMPLEXITY(&complexity));
		if (error != OPUS_OK)
			goto fail;
		state->complexity = (uint8_t)complexity;
	}
	return codec;

fail:
	opus_destroy(codec);
	return NULL;
}

static void opus_reset(struct udsp_codec *codec)
{
	struct udsp_opus_state *state = codec->state;
	int error;

	error = opus_encoder_ctl(state->encoder, OPUS_RESET_STATE);
	if (error == OPUS_OK)
		error = encoder_configure(state);
	state->encoder_error = error == OPUS_OK ? 0 : opus_errno_get(error);
	error = opus_decoder_ctl(state->decoder, OPUS_RESET_STATE);
	state->decoder_error = error == OPUS_OK ? 0 : opus_errno_get(error);
}

static unsigned opus_rate_get(const struct udsp_codec *codec)
{
	const struct udsp_opus_state *state = codec->state;

	return state->rate;
}

static int opus_encode_packet(struct udsp_codec *codec, const int16_t *pcm,
			      unsigned n, uint8_t *out, size_t out_cap)
{
	struct udsp_opus_state *state = codec->state;
	uint8_t packet[UDSP_CODEC_PAYLOAD_MAX];
	int bytes;

	if (!pcm || !out || n != state->packet_samples)
		return -EINVAL;
	if (state->encoder_error) {
		int error = state->encoder_error;

		state->encoder_error = 0;
		return error;
	}
	bytes = opus_encode(state->encoder, pcm, n, packet, sizeof(packet));
	if (bytes < 0)
		return opus_errno_get(bytes);
	if ((size_t)bytes > out_cap)
		return -ENOBUFS;
	memcpy(out, packet, bytes);
	return bytes;
}

static int packet_samples_get(const struct udsp_opus_state *state,
			      const uint8_t *packet, size_t len)
{
	int samples;

	if (!packet || !len || len > UDSP_CODEC_PAYLOAD_MAX)
		return -EINVAL;
	samples = opus_packet_get_nb_samples(packet, len, state->rate);
	if (samples < 0)
		return opus_errno_get(samples);
	if ((unsigned)samples != state->rate / 100 &&
	    (unsigned)samples != state->rate / 50)
		return -EINVAL;
	return samples;
}

static int opus_decode_packet(struct udsp_codec *codec, const uint8_t *in,
			      size_t len, int16_t *pcm, unsigned pcm_cap)
{
	struct udsp_opus_state *state = codec->state;
	int samples;
	int decoded;

	if (!pcm)
		return -EINVAL;
	if (state->decoder_error) {
		int error = state->decoder_error;

		state->decoder_error = 0;
		return error;
	}
	samples = packet_samples_get(state, in, len);
	if (samples < 0)
		return samples;
	if (pcm_cap < (unsigned)samples)
		return -ENOBUFS;
	/* Opus has no integrity check, so SRTP or RTP must detect truncation. */
	decoded = opus_decode(state->decoder, in, len, pcm, samples, 0);
	if (decoded < 0)
		return opus_errno_get(decoded);
	if (decoded != samples)
		return -EINVAL;
	return decoded;
}

static int opus_plc(struct udsp_codec *codec, int16_t *pcm, unsigned n,
		    const uint8_t *next, size_t next_len)
{
	struct udsp_opus_state *state = codec->state;
	int samples;
	int decoded;

	if (!pcm || (n != state->rate / 100 && n != state->rate / 50))
		return -EINVAL;
	if ((!next && next_len) || (next && !next_len))
		return -EINVAL;
	if (next_len > UDSP_CODEC_PAYLOAD_MAX)
		return -EINVAL;
	if (state->decoder_error) {
		int error = state->decoder_error;

		state->decoder_error = 0;
		return error;
	}
	if (next) {
		samples = packet_samples_get(state, next, next_len);
		if (samples == (int)n) {
			decoded = opus_decode(state->decoder, next, next_len,
					      pcm, n, 1);
			if (decoded == (int)n)
				return decoded;
		}
	}
	decoded = opus_decode(state->decoder, NULL, 0, pcm, n, 0);
	if (decoded < 0)
		return opus_errno_get(decoded);
	if ((unsigned)decoded != n)
		return -EIO;
	return decoded;
}

const struct udsp_codec_ops udsp_codec_opus_ops = {
	.id = UDSP_CODEC_OPUS,
	.name = "opus",
	.sample_rate = 0,
	.clock_rate = 48000,
	.frame_ms = 0,
	.ptime_mask = 0x03,
	.max_payload = UDSP_CODEC_PAYLOAD_MAX,
	.frame_bytes = 0,
	.sid_bytes = 0,
	.flags = UDSP_CODEC_F_PACKET | UDSP_CODEC_F_PLC | UDSP_CODEC_F_FEC |
		 UDSP_CODEC_F_NO_CN,
	.create = opus_create,
	.destroy = opus_destroy,
	.reset = opus_reset,
	.rate_get = opus_rate_get,
	.encode = opus_encode_packet,
	.decode = opus_decode_packet,
	.plc = opus_plc,
	.cn = NULL,
};
