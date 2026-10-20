// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <opus/opus.h>

#include "dsp.h"
#include "util.h"

#define TEST_PACKETS 60U
#define TEST_FRAME_MAX 320U
#define TEST_FRAME_LONG 640U
#define TEST_SAMPLES_MAX (TEST_PACKETS * TEST_FRAME_MAX)
#define KNOB_PACKETS 160U
#define BENCH_PACKETS 5000U
#define GUARD_BYTES 16U
#define GUARD_SAMPLES 8U
#define TEST_PI 3.14159265358979323846

extern const struct udsp_codec_ops udsp_codec_opus_ops;

static struct udsp_codec *codec_create(unsigned rate, unsigned ptime_ms,
				       uint32_t bitrate, uint8_t complexity,
				       uint8_t max_bandwidth, bool fec)
{
	struct udsp_codec_params params = {
		.line_rate = rate,
		.ptime_ms = ptime_ms,
		.bitrate = bitrate,
		.complexity = complexity,
		.max_bandwidth = max_bandwidth,
		.fec = fec,
	};

	return udsp_codec_opus_ops.create(&params);
}

static void tone_fill(int16_t *pcm, unsigned count, unsigned offset,
		      unsigned rate)
{
	double second_hz = rate == 16000 ? 3000.0 : 1800.0;
	unsigned i;

	for (i = 0; i < count; i++) {
		double time = (offset + i) / (double)rate;
		double sample = 9000.0 * sin(2.0 * TEST_PI * 440.0 * time);

		sample += 4500.0 * sin(2.0 * TEST_PI * second_hz * time);
		pcm[i] = (int16_t)lrint(sample);
	}
}

static void speech_fill(int16_t *pcm, unsigned count, unsigned offset,
			unsigned rate, uint32_t *seed, int32_t *shaped)
{
	unsigned packet = offset / (rate / 50);
	double fundamental = 120.0 + 23.0 * (packet % 7);
	unsigned i;

	for (i = 0; i < count; i++) {
		double time = (offset + i) / (double)rate;
		double sample;
		int32_t random;

		*seed = *seed * 1664525U + 1013904223U;
		random = (int32_t)(*seed >> 16) - 32768;
		*shaped = (*shaped * 5 + random) / 6;
		sample = 9500.0 * sin(2.0 * TEST_PI * fundamental * time);
		sample += 4200.0 * sin(4.0 * TEST_PI * fundamental * time);
		sample += 2200.0 * sin(6.0 * TEST_PI * fundamental * time);
		sample = (0.65 + 0.3 * sin(2.0 * TEST_PI * 3.7 * time)) * sample;
		sample += *shaped / 14.0;
		if (sample > 30000.0)
			sample = 30000.0;
		if (sample < -30000.0)
			sample = -30000.0;
		pcm[i] = (int16_t)lrint(sample);
	}
}

static double snr_get(const int16_t *reference, const int16_t *actual,
		      unsigned count)
{
	double signal = 0.0;
	double error = 0.0;
	unsigned i;

	for (i = 0; i < count; i++) {
		double difference = actual[i] - reference[i];

		signal += (double)reference[i] * reference[i];
		error += difference * difference;
	}
	if (!error)
		return 200.0;
	return 10.0 * log10(signal / error);
}

static double aligned_snr_get(const int16_t *reference, const int16_t *actual,
			      unsigned count, unsigned rate,
			      unsigned packet_samples)
{
	double best = -200.0;
	unsigned start = packet_samples * 2;
	unsigned delay;

	for (delay = 0; delay <= rate / 50; delay++) {
		double snr = snr_get(reference + start, actual + start + delay,
				     count - start - delay);

		if (snr > best)
			best = snr;
	}
	return best;
}

static uint64_t error_energy_get(const int16_t *reference,
				 const int16_t *actual, unsigned count)
{
	uint64_t energy = 0;
	unsigned i;

	for (i = 0; i < count; i++) {
		int64_t difference = (int32_t)actual[i] - reference[i];

		energy += difference * difference;
	}
	return energy;
}

static uint64_t pcm_energy_get(const int16_t *pcm, unsigned count)
{
	uint64_t energy = 0;
	unsigned i;

	for (i = 0; i < count; i++)
		energy += (int64_t)pcm[i] * pcm[i];
	return energy;
}

static bool byte_guard_check(const uint8_t *guard, unsigned count)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		if (guard[i] != 0x5a)
			return false;
	}
	return true;
}

static bool pcm_guard_check(const int16_t *guard, unsigned count)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		if ((uint16_t)guard[i] != UINT16_C(0x5a5a))
			return false;
	}
	return true;
}

static void metadata_check(void)
{
	struct udsp_codec *narrow = codec_create(8000, 20, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	struct udsp_codec *wide = codec_create(16000, 20, 0, 0,
		UDSP_OPUS_BW_AUTO, false);

	CASE("v2 opus ops metadata");
	CHECK_UINT(udsp_codec_opus_ops.id, UDSP_CODEC_OPUS);
	CHECK(strcmp(udsp_codec_opus_ops.name, "opus") == 0);
	CHECK_UINT(udsp_codec_opus_ops.sample_rate, 0);
	CHECK_UINT(udsp_codec_opus_ops.clock_rate, 48000);
	CHECK_UINT(udsp_codec_opus_ops.frame_ms, 0);
	CHECK_UINT(udsp_codec_opus_ops.ptime_mask, 0x03);
	CHECK_UINT(udsp_codec_opus_ops.max_payload, UDSP_CODEC_PAYLOAD_MAX);
	CHECK_UINT(udsp_codec_opus_ops.flags,
		   UDSP_CODEC_F_PACKET | UDSP_CODEC_F_PLC | UDSP_CODEC_F_FEC |
		   UDSP_CODEC_F_NO_CN);
	CHECK(udsp_codec_opus_ops.cn == NULL);
	CHECK(narrow != NULL);
	CHECK(wide != NULL);
	if (narrow)
		CHECK_UINT(udsp_codec_opus_ops.rate_get(narrow), 8000);
	if (wide)
		CHECK_UINT(udsp_codec_opus_ops.rate_get(wide), 16000);
	udsp_codec_opus_ops.destroy(wide);
	udsp_codec_opus_ops.destroy(narrow);
}

static void create_errors_check(void)
{
	struct udsp_codec_params params = { 0 };
	struct udsp_codec *codec;
	int16_t pcm[160] = { 0 };
	uint8_t payload[UDSP_CODEC_PAYLOAD_MAX];

	CASE("zero params select narrowband and 20 ms defaults");
	codec = udsp_codec_opus_ops.create(&params);
	CHECK(codec != NULL);
	if (codec) {
		CHECK_UINT(udsp_codec_opus_ops.rate_get(codec), 8000);
		CHECK(udsp_codec_opus_ops.encode(codec, pcm, 160, payload,
						 sizeof(payload)) > 0);
		CHECK_UINT(udsp_codec_opus_ops.encode(codec, pcm, 80, payload,
						      sizeof(payload)), -EINVAL);
	}
	udsp_codec_opus_ops.destroy(codec);

	CASE("create rejects invalid line rate");
	params.line_rate = 12000;
	CHECK(udsp_codec_opus_ops.create(&params) == NULL);
	params.line_rate = 8000;
	CASE("create rejects invalid packet duration");
	params.ptime_ms = 30;
	CHECK(udsp_codec_opus_ops.create(&params) == NULL);
	params.ptime_ms = 20;
	CASE("create rejects invalid complexity");
	params.complexity = 11;
	CHECK(udsp_codec_opus_ops.create(&params) == NULL);
	params.complexity = 0;
	CASE("create rejects invalid bandwidth enum");
	params.max_bandwidth = UDSP_OPUS_BW_WB + 1;
	CHECK(udsp_codec_opus_ops.create(&params) == NULL);
	params.max_bandwidth = UDSP_OPUS_BW_AUTO;
	CASE("create rejects bitrate outside libopus integer range");
	params.bitrate = UINT32_MAX;
	CHECK(udsp_codec_opus_ops.create(&params) == NULL);
	CASE("destroy of a null codec does not crash");
	udsp_codec_opus_ops.destroy(NULL);
}

static void round_trip_check(unsigned rate, unsigned ptime_ms, const char *name)
{
	static int16_t input[TEST_SAMPLES_MAX];
	static int16_t output[TEST_SAMPLES_MAX];
	struct udsp_codec *encoder = codec_create(rate, ptime_ms, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	struct udsp_codec *decoder = codec_create(rate, ptime_ms, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	unsigned samples = rate * ptime_ms / 1000;
	unsigned total = samples * TEST_PACKETS;
	uint8_t payload[UDSP_CODEC_PAYLOAD_MAX];
	unsigned packet;
	double snr;

	CASE(name);
	CHECK(encoder != NULL);
	CHECK(decoder != NULL);
	if (!encoder || !decoder) {
		udsp_codec_opus_ops.destroy(decoder);
		udsp_codec_opus_ops.destroy(encoder);
		return;
	}
	memset(output, 0, total * sizeof(output[0]));
	for (packet = 0; packet < TEST_PACKETS; packet++) {
		int bytes;

		tone_fill(input + packet * samples, samples, packet * samples, rate);
		bytes = udsp_codec_opus_ops.encode(encoder, input + packet * samples,
						 samples, payload, sizeof(payload));
		CHECK(bytes > 0);
		if (bytes > 0)
			CHECK_UINT(udsp_codec_opus_ops.decode(decoder, payload, bytes,
				output + packet * samples, samples), samples);
	}
	snr = aligned_snr_get(input, output, total, rate, samples);
	printf("# %s aligned snr %.2f dB\n", name, snr);
	CHECK(snr > 12.0);
	udsp_codec_opus_ops.destroy(decoder);
	udsp_codec_opus_ops.destroy(encoder);
}

static void bandwidth_check(void)
{
	struct udsp_codec *narrow = codec_create(16000, 20, 64000, 5,
		UDSP_OPUS_BW_NB, false);
	struct udsp_codec *automatic = codec_create(16000, 20, 64000, 5,
		UDSP_OPUS_BW_AUTO, false);
	int16_t pcm[TEST_FRAME_MAX];
	uint8_t payload[UDSP_CODEC_PAYLOAD_MAX];
	unsigned packet;

	CASE("16 kHz BW_NB emits narrowband packets");
	CHECK(narrow != NULL);
	if (narrow) {
		for (packet = 0; packet < 20; packet++) {
			int bytes;

			tone_fill(pcm, TEST_FRAME_MAX, packet * TEST_FRAME_MAX,
				16000);
			bytes = udsp_codec_opus_ops.encode(narrow, pcm,
				TEST_FRAME_MAX, payload, sizeof(payload));
			CHECK(bytes > 0);
			if (bytes > 0)
				CHECK_UINT(opus_packet_get_bandwidth(payload),
					OPUS_BANDWIDTH_NARROWBAND);
		}
	}
	CASE("16 kHz AUTO emits at most wideband packets");
	CHECK(automatic != NULL);
	if (automatic) {
		for (packet = 0; packet < 20; packet++) {
			int bytes;

			tone_fill(pcm, TEST_FRAME_MAX, packet * TEST_FRAME_MAX,
				16000);
			bytes = udsp_codec_opus_ops.encode(automatic, pcm,
				TEST_FRAME_MAX, payload, sizeof(payload));
			CHECK(bytes > 0);
			if (bytes > 0)
				CHECK(opus_packet_get_bandwidth(payload) <=
				      OPUS_BANDWIDTH_WIDEBAND);
		}
	}
	udsp_codec_opus_ops.destroy(automatic);
	udsp_codec_opus_ops.destroy(narrow);
}

static int packet_make(struct udsp_codec *encoder, unsigned rate,
		       unsigned samples, uint8_t *payload, int16_t *pcm)
{
	tone_fill(pcm, samples, 0, rate);
	return udsp_codec_opus_ops.encode(encoder, pcm, samples, payload,
					   UDSP_CODEC_PAYLOAD_MAX);
}

static void encode_errors_check(void)
{
	struct udsp_codec *encoder = codec_create(16000, 20, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	struct udsp_codec *sizing = codec_create(16000, 20, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	struct udsp_codec *exact = codec_create(16000, 20, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	int16_t pcm[TEST_FRAME_MAX];
	uint8_t payload[UDSP_CODEC_PAYLOAD_MAX];
	uint8_t guarded[UDSP_CODEC_PAYLOAD_MAX + GUARD_BYTES];
	int bytes;
	int result;

	CHECK(encoder != NULL);
	CHECK(sizing != NULL);
	CHECK(exact != NULL);
	if (!encoder || !sizing || !exact) {
		udsp_codec_opus_ops.destroy(exact);
		udsp_codec_opus_ops.destroy(sizing);
		udsp_codec_opus_ops.destroy(encoder);
		return;
	}
	tone_fill(pcm, TEST_FRAME_MAX, 0, 16000);
	CASE("encode rejects the wrong packet sample count");
	CHECK_UINT(udsp_codec_opus_ops.encode(encoder, pcm, 160, payload,
					      sizeof(payload)), -EINVAL);
	CASE("encode rejects null buffers");
	CHECK_UINT(udsp_codec_opus_ops.encode(encoder, NULL, TEST_FRAME_MAX,
					      payload, sizeof(payload)), -EINVAL);
	CHECK_UINT(udsp_codec_opus_ops.encode(encoder, pcm, TEST_FRAME_MAX,
					      NULL, sizeof(payload)), -EINVAL);
	bytes = udsp_codec_opus_ops.encode(sizing, pcm, TEST_FRAME_MAX, payload,
					    sizeof(payload));
	CASE("encode returns ENOBUFS without touching a short output");
	CHECK(bytes > 1);
	memset(guarded, 0x5a, sizeof(guarded));
	CHECK_UINT(udsp_codec_opus_ops.encode(encoder, pcm, TEST_FRAME_MAX,
					      guarded, bytes - 1), -ENOBUFS);
	CHECK(byte_guard_check(guarded, sizeof(guarded)));
	CASE("encode exact fit succeeds and staging preserves the guard");
	memset(guarded, 0x5a, sizeof(guarded));
	result = udsp_codec_opus_ops.encode(exact, pcm, TEST_FRAME_MAX,
					    guarded, bytes);
	CHECK_UINT(result, bytes);
	CHECK(byte_guard_check(guarded + bytes, sizeof(guarded) - bytes));
	udsp_codec_opus_ops.destroy(exact);
	udsp_codec_opus_ops.destroy(sizing);
	udsp_codec_opus_ops.destroy(encoder);
}

static void decode_errors_check(void)
{
	struct udsp_codec *encoder = codec_create(16000, 20, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	struct udsp_codec *decoder = codec_create(16000, 20, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	OpusEncoder *native_encoder;
	int16_t pcm[TEST_FRAME_MAX];
	int16_t guarded[TEST_FRAME_LONG + GUARD_SAMPLES];
	int16_t long_pcm[TEST_FRAME_LONG];
	uint8_t payload[UDSP_CODEC_PAYLOAD_MAX];
	uint8_t long_payload[UDSP_CODEC_PAYLOAD_MAX];
	uint8_t malformed = 0x03;
	int bytes;
	int error;
	int long_bytes;

	CHECK(encoder != NULL);
	CHECK(decoder != NULL);
	if (!encoder || !decoder) {
		udsp_codec_opus_ops.destroy(decoder);
		udsp_codec_opus_ops.destroy(encoder);
		return;
	}
	bytes = packet_make(encoder, 16000, TEST_FRAME_MAX, payload, pcm);
	CHECK(bytes > 0);
	if (bytes <= 0) {
		udsp_codec_opus_ops.destroy(decoder);
		udsp_codec_opus_ops.destroy(encoder);
		return;
	}
	CASE("decode rejects malformed packet shapes");
	CHECK_UINT(udsp_codec_opus_ops.decode(decoder, NULL, bytes, pcm,
					      TEST_FRAME_MAX), -EINVAL);
	CHECK_UINT(udsp_codec_opus_ops.decode(decoder, payload, 0, pcm,
					      TEST_FRAME_MAX), -EINVAL);
	CHECK_UINT(udsp_codec_opus_ops.decode(decoder, payload,
		UDSP_CODEC_PAYLOAD_MAX + 1, pcm, TEST_FRAME_MAX), -EINVAL);
	CHECK_UINT(udsp_codec_opus_ops.decode(decoder, &malformed, 1, pcm,
					      TEST_FRAME_MAX), -EINVAL);
	CHECK_UINT(udsp_codec_opus_ops.decode(decoder, payload, bytes, NULL,
					      TEST_FRAME_MAX), -EINVAL);
	CASE("decode returns ENOBUFS without touching short PCM");
	memset(guarded, 0x5a, sizeof(guarded));
	CHECK_UINT(udsp_codec_opus_ops.decode(decoder, payload, bytes, guarded,
					      TEST_FRAME_MAX - 1), -ENOBUFS);
	CHECK(pcm_guard_check(guarded, TEST_FRAME_LONG + GUARD_SAMPLES));
	CASE("decode exact-sized PCM preserves the guard");
	memset(guarded, 0x5a, sizeof(guarded));
	CHECK_UINT(udsp_codec_opus_ops.decode(decoder, payload, bytes, guarded,
					      TEST_FRAME_MAX), TEST_FRAME_MAX);
	CHECK(pcm_guard_check(guarded + TEST_FRAME_MAX,
		TEST_FRAME_LONG + GUARD_SAMPLES - TEST_FRAME_MAX));
	native_encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP,
		&error);
	CHECK(native_encoder != NULL);
	CHECK_UINT(error, OPUS_OK);
	long_bytes = -1;
	if (native_encoder) {
		tone_fill(long_pcm, TEST_FRAME_LONG, 0, 16000);
		long_bytes = opus_encode(native_encoder, long_pcm, TEST_FRAME_LONG,
			long_payload, sizeof(long_payload));
	}
	CASE("decode rejects a native 40 ms packet and preserves the guard");
	CHECK(long_bytes > 0);
	memset(guarded, 0x5a, sizeof(guarded));
	if (long_bytes > 0)
		CHECK_UINT(udsp_codec_opus_ops.decode(decoder, long_payload,
			long_bytes, guarded, TEST_FRAME_LONG), -EINVAL);
	CHECK(pcm_guard_check(guarded, TEST_FRAME_LONG + GUARD_SAMPLES));
	if (native_encoder)
		opus_encoder_destroy(native_encoder);
	udsp_codec_opus_ops.destroy(decoder);
	udsp_codec_opus_ops.destroy(encoder);
}

static void plc_check(void)
{
	struct udsp_codec *encoder = codec_create(16000, 20, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	struct udsp_codec *decoder = codec_create(16000, 20, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	struct udsp_codec *fresh = codec_create(16000, 20, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	struct udsp_codec *fallback = codec_create(16000, 20, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	int16_t pcm[TEST_FRAME_MAX];
	int16_t guarded[TEST_FRAME_MAX + GUARD_SAMPLES];
	uint8_t payload[UDSP_CODEC_PAYLOAD_MAX];
	uint8_t malformed = 0x03;
	int bytes;

	CHECK(encoder != NULL);
	CHECK(decoder != NULL);
	CHECK(fresh != NULL);
	CHECK(fallback != NULL);
	if (!encoder || !decoder || !fresh || !fallback) {
		udsp_codec_opus_ops.destroy(fallback);
		udsp_codec_opus_ops.destroy(fresh);
		udsp_codec_opus_ops.destroy(decoder);
		udsp_codec_opus_ops.destroy(encoder);
		return;
	}
	bytes = packet_make(encoder, 16000, TEST_FRAME_MAX, payload, pcm);
	CHECK(bytes > 0);
	CHECK_UINT(udsp_codec_opus_ops.decode(decoder, payload, bytes, pcm,
					      TEST_FRAME_MAX), TEST_FRAME_MAX);
	CASE("native PLC returns one voiced lost packet");
	CHECK_UINT(udsp_codec_opus_ops.plc(decoder, pcm, TEST_FRAME_MAX,
					   NULL, 0), TEST_FRAME_MAX);
	CHECK(pcm_energy_get(pcm, TEST_FRAME_MAX) > 0);
	CASE("fresh native PLC produces silence");
	memset(pcm, 0x5a, sizeof(pcm));
	CHECK_UINT(udsp_codec_opus_ops.plc(fresh, pcm, TEST_FRAME_MAX,
					   NULL, 0), TEST_FRAME_MAX);
	CHECK_UINT(pcm_energy_get(pcm, TEST_FRAME_MAX), 0);
	CASE("PLC rejects local shape errors");
	CHECK_UINT(udsp_codec_opus_ops.plc(decoder, NULL, TEST_FRAME_MAX,
					   NULL, 0), -EINVAL);
	CHECK_UINT(udsp_codec_opus_ops.plc(decoder, pcm, 120, NULL, 0),
		   -EINVAL);
	CHECK_UINT(udsp_codec_opus_ops.plc(decoder, pcm, TEST_FRAME_MAX,
					   NULL, bytes), -EINVAL);
	CHECK_UINT(udsp_codec_opus_ops.plc(decoder, pcm, TEST_FRAME_MAX,
					   payload, 0), -EINVAL);
	CHECK_UINT(udsp_codec_opus_ops.plc(decoder, pcm, TEST_FRAME_MAX,
		payload, UDSP_CODEC_PAYLOAD_MAX + 1), -EINVAL);
	CASE("PLC corrupt next packet falls back to native concealment");
	memset(guarded, 0x5a, sizeof(guarded));
	CHECK_UINT(udsp_codec_opus_ops.plc(fallback, guarded, TEST_FRAME_MAX,
		&malformed, 1), TEST_FRAME_MAX);
	CHECK(pcm_guard_check(guarded + TEST_FRAME_MAX, GUARD_SAMPLES));
	CASE("PLC smaller duration falls back and preserves its guard");
	memset(guarded, 0x5a, sizeof(guarded));
	CHECK_UINT(udsp_codec_opus_ops.plc(fallback, guarded,
		TEST_FRAME_MAX / 2, payload, bytes), TEST_FRAME_MAX / 2);
	CHECK(pcm_guard_check(guarded + TEST_FRAME_MAX / 2,
		TEST_FRAME_MAX / 2 + GUARD_SAMPLES));
	udsp_codec_opus_ops.destroy(fallback);
	udsp_codec_opus_ops.destroy(fresh);
	udsp_codec_opus_ops.destroy(decoder);
	udsp_codec_opus_ops.destroy(encoder);
}

static void fec_check(unsigned rate, unsigned ptime_ms, const char *name)
{
	struct udsp_codec *encoder = codec_create(rate, ptime_ms, 24000, 5,
		UDSP_OPUS_BW_AUTO, true);
	struct udsp_codec *reference = codec_create(rate, ptime_ms, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	struct udsp_codec *plain = codec_create(rate, ptime_ms, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	struct udsp_codec *fec = codec_create(rate, ptime_ms, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	uint8_t payload[TEST_PACKETS][UDSP_CODEC_PAYLOAD_MAX];
	int payload_len[TEST_PACKETS];
	int16_t pcm[TEST_FRAME_MAX];
	int16_t expected[TEST_FRAME_MAX];
	int16_t concealed[TEST_FRAME_MAX];
	int16_t recovered[TEST_FRAME_MAX];
	uint32_t seed = 3;
	int32_t shaped = 0;
	unsigned samples = rate * ptime_ms / 1000;
	unsigned missing = TEST_PACKETS;
	unsigned packet;
	uint64_t plain_error;
	uint64_t fec_error;
	double improvement;

	CASE(name);
	CHECK(encoder != NULL && reference != NULL && plain != NULL && fec != NULL);
	if (!encoder || !reference || !plain || !fec)
		goto cleanup;
	for (packet = 0; packet < TEST_PACKETS; packet++) {
		speech_fill(pcm, samples, packet * samples, rate,
			    &seed, &shaped);
		payload_len[packet] = udsp_codec_opus_ops.encode(encoder, pcm,
			samples, payload[packet], sizeof(payload[packet]));
		CHECK(payload_len[packet] > 0);
		if (packet > 4 && payload_len[packet] > 0 &&
		    opus_packet_has_lbrr(payload[packet], payload_len[packet]) == 1 &&
		    missing == TEST_PACKETS)
			missing = packet - 1;
	}
	CHECK(missing < TEST_PACKETS - 1);
	if (missing >= TEST_PACKETS - 1)
		goto cleanup;
	for (packet = 0; packet < missing; packet++) {
		CHECK_UINT(udsp_codec_opus_ops.decode(reference, payload[packet],
			payload_len[packet], pcm, samples), samples);
		CHECK_UINT(udsp_codec_opus_ops.decode(plain, payload[packet],
			payload_len[packet], pcm, samples), samples);
		CHECK_UINT(udsp_codec_opus_ops.decode(fec, payload[packet],
			payload_len[packet], pcm, samples), samples);
	}
	CHECK_UINT(udsp_codec_opus_ops.decode(reference, payload[missing],
		payload_len[missing], expected, samples), samples);
	CHECK_UINT(udsp_codec_opus_ops.plc(plain, concealed, samples,
		NULL, 0), samples);
	CHECK_UINT(udsp_codec_opus_ops.plc(fec, recovered, samples,
		payload[missing + 1], payload_len[missing + 1]), samples);
	plain_error = error_energy_get(expected, concealed, samples);
	fec_error = error_energy_get(expected, recovered, samples);
	improvement = 10.0 * log10((plain_error + 1.0) / (fec_error + 1.0));
	printf("# FEC packet %u improvement %.2f dB, plain %llu, FEC %llu\n",
	       missing, improvement, (unsigned long long)plain_error,
	       (unsigned long long)fec_error);
	CHECK(improvement > 1.0);

cleanup:
	udsp_codec_opus_ops.destroy(fec);
	udsp_codec_opus_ops.destroy(plain);
	udsp_codec_opus_ops.destroy(reference);
	udsp_codec_opus_ops.destroy(encoder);
}

static uint64_t payload_total_get(unsigned rate, uint32_t bitrate,
				  uint8_t complexity, uint64_t *hash)
{
	struct udsp_codec *encoder = codec_create(rate, 20, bitrate, complexity,
		UDSP_OPUS_BW_AUTO, false);
	int16_t pcm[TEST_FRAME_MAX];
	uint8_t payload[UDSP_CODEC_PAYLOAD_MAX];
	uint32_t seed = 7;
	int32_t shaped = 0;
	uint64_t total = 0;
	unsigned samples = rate / 50;
	unsigned packet;

	if (!encoder)
		return 0;
	for (packet = 0; packet < KNOB_PACKETS; packet++) {
		int bytes;
		int i;

		speech_fill(pcm, samples, packet * samples, rate, &seed, &shaped);
		bytes = udsp_codec_opus_ops.encode(encoder, pcm, samples, payload,
						 sizeof(payload));
		if (bytes <= 0) {
			total = 0;
			break;
		}
		total += bytes;
		for (i = 0; i < bytes; i++)
			*hash = (*hash ^ payload[i]) * UINT64_C(1099511628211);
	}
	udsp_codec_opus_ops.destroy(encoder);
	return total;
}

static void bitrate_check(void)
{
	uint64_t low_hash = UINT64_C(1469598103934665603);
	uint64_t high_hash = UINT64_C(1469598103934665603);
	uint64_t low = payload_total_get(16000, 12000, 5, &low_hash);
	uint64_t high = payload_total_get(16000, 64000, 5, &high_hash);

	CASE("higher bitrate raises aggregate payload size");
	printf("# bitrate payload totals 12k %llu, 64k %llu bytes\n",
	       (unsigned long long)low, (unsigned long long)high);
	CHECK(low > 0);
	CHECK(high > low * 2);
	CHECK(low_hash != high_hash);
}

static void complexity_check(void)
{
	uint64_t low_hash = UINT64_C(1469598103934665603);
	uint64_t high_hash = UINT64_C(1469598103934665603);
	uint64_t low = payload_total_get(16000, 32000, 1, &low_hash);
	uint64_t high = payload_total_get(16000, 32000, 10, &high_hash);

	CASE("complexity knob changes encoded size");
	printf("# complexity payload totals 1 %llu, 10 %llu bytes\n",
	       (unsigned long long)low, (unsigned long long)high);
	CHECK(low > 0);
	CHECK(high > 0);
	CHECK(low != high);
	CHECK(low_hash != high_hash);
}

static void dtx_check(void)
{
	struct udsp_codec *codec = codec_create(16000, 20, 0, 0,
		UDSP_OPUS_BW_AUTO, false);
	int16_t pcm[TEST_FRAME_MAX] = { 0 };
	uint8_t payload[UDSP_CODEC_PAYLOAD_MAX];
	unsigned packet;

	CASE("silence never enters DTX");
	CHECK(codec != NULL);
	if (!codec)
		return;
	for (packet = 0; packet < TEST_PACKETS; packet++)
		CHECK(udsp_codec_opus_ops.encode(codec, pcm, TEST_FRAME_MAX,
						 payload, sizeof(payload)) > 2);
	udsp_codec_opus_ops.destroy(codec);
}

static void reset_check(void)
{
	struct udsp_codec *codec = codec_create(16000, 20, 24000, 5,
		UDSP_OPUS_BW_AUTO, true);
	int16_t pcm[TEST_FRAME_MAX];
	int16_t decoded_first[TEST_FRAME_MAX];
	int16_t decoded_reset[TEST_FRAME_MAX];
	uint8_t first[UDSP_CODEC_PAYLOAD_MAX];
	uint8_t second[UDSP_CODEC_PAYLOAD_MAX];
	uint8_t repeated[UDSP_CODEC_PAYLOAD_MAX];
	int first_len;
	int second_len;
	int repeated_len;

	CASE("reset drops history and preserves create parameters");
	CHECK(codec != NULL);
	if (!codec)
		return;
	tone_fill(pcm, TEST_FRAME_MAX, 0, 16000);
	first_len = udsp_codec_opus_ops.encode(codec, pcm, TEST_FRAME_MAX,
					       first, sizeof(first));
	tone_fill(pcm, TEST_FRAME_MAX, TEST_FRAME_MAX, 16000);
	second_len = udsp_codec_opus_ops.encode(codec, pcm, TEST_FRAME_MAX,
						second, sizeof(second));
	udsp_codec_opus_ops.reset(codec);
	tone_fill(pcm, TEST_FRAME_MAX, 0, 16000);
	repeated_len = udsp_codec_opus_ops.encode(codec, pcm, TEST_FRAME_MAX,
						  repeated, sizeof(repeated));
	CHECK(first_len > 0);
	CHECK_UINT(repeated_len, first_len);
	if (first_len > 0 && repeated_len == first_len)
		CHECK(memcmp(first, repeated, first_len) == 0);
	CHECK_UINT(udsp_codec_opus_ops.decode(codec, first, first_len,
		decoded_first, TEST_FRAME_MAX), TEST_FRAME_MAX);
	CHECK_UINT(udsp_codec_opus_ops.decode(codec, second, second_len, pcm,
		TEST_FRAME_MAX), TEST_FRAME_MAX);
	udsp_codec_opus_ops.reset(codec);
	CHECK_UINT(udsp_codec_opus_ops.decode(codec, first, first_len,
		decoded_reset, TEST_FRAME_MAX), TEST_FRAME_MAX);
	CHECK(memcmp(decoded_first, decoded_reset, sizeof(decoded_first)) == 0);
	udsp_codec_opus_ops.destroy(codec);
}

static uint64_t time_ns_get(void)
{
	struct timespec value;

	clock_gettime(CLOCK_MONOTONIC, &value);
	return (uint64_t)value.tv_sec * UINT64_C(1000000000) + value.tv_nsec;
}

static void benchmark_run(unsigned rate, uint8_t complexity, const char *label)
{
	struct udsp_codec *codec = codec_create(rate, 20, 0, complexity,
		UDSP_OPUS_BW_AUTO, false);
	int16_t pcm[TEST_FRAME_MAX];
	int16_t decoded[TEST_FRAME_MAX];
	uint8_t payload[UDSP_CODEC_PAYLOAD_MAX];
	unsigned samples = rate / 50;
	uint64_t start;
	uint64_t elapsed;
	unsigned packet;
	int bytes = 0;

	if (!codec) {
		printf("bench rate %u complexity %s create failed\n", rate, label);
		return;
	}
	tone_fill(pcm, samples, 0, rate);
	for (packet = 0; packet < 100; packet++) {
		bytes = udsp_codec_opus_ops.encode(codec, pcm, samples, payload,
						    sizeof(payload));
		if (bytes > 0)
			udsp_codec_opus_ops.decode(codec, payload, bytes, decoded, samples);
	}
	start = time_ns_get();
	for (packet = 0; packet < BENCH_PACKETS; packet++) {
		bytes = udsp_codec_opus_ops.encode(codec, pcm, samples, payload,
						    sizeof(payload));
		if (bytes > 0)
			udsp_codec_opus_ops.decode(codec, payload, bytes, decoded, samples);
	}
	elapsed = time_ns_get() - start;
	printf("bench rate %u complexity %s encode+decode %.3f us/frame\n",
	       rate, label, elapsed / (double)BENCH_PACKETS / 1000.0);
	udsp_codec_opus_ops.destroy(codec);
}

static void benchmark_all(void)
{
	benchmark_run(8000, 0, "default(0)");
	benchmark_run(8000, 5, "5");
	benchmark_run(8000, 10, "10");
	benchmark_run(16000, 0, "default(0)");
	benchmark_run(16000, 5, "5");
	benchmark_run(16000, 10, "10");
}

static void feature_probe(void)
{
	OpusDREDDecoder *dred_decoder;
	OpusEncoder *encoder;
	opus_int32 complexity = -1;
	opus_int32 dred_duration = -1;
	opus_int32 dtx = -1;
	int dred_create_error = OPUS_OK;
	int error;

	printf("version: %s\n", opus_get_version_string());
	encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &error);
	if (!encoder || error != OPUS_OK) {
		printf("feature probe encoder create: %d\n", error);
		return;
	}
	error = opus_encoder_ctl(encoder, OPUS_GET_COMPLEXITY(&complexity));
	printf("default complexity ctl: %d, value %d\n", error, (int)complexity);
	error = opus_encoder_ctl(encoder, OPUS_GET_DTX(&dtx));
	printf("default dtx ctl: %d, value %d\n", error, (int)dtx);
	error = opus_encoder_ctl(encoder, OPUS_GET_DRED_DURATION(&dred_duration));
	printf("DRED get ctl: %d, value %d\n", error, (int)dred_duration);
	error = opus_encoder_ctl(encoder, OPUS_SET_DRED_DURATION(1));
	printf("DRED set ctl: %d\n", error);
	error = opus_encoder_ctl(encoder, OPUS_SET_DNN_BLOB(NULL, 0));
	printf("DNN blob ctl with empty blob: %d\n", error);
	printf("DRED state sizes: decoder %d, data %d\n",
	       opus_dred_decoder_get_size(), opus_dred_get_size());
	dred_decoder = opus_dred_decoder_create(&dred_create_error);
	printf("DRED decoder create: pointer %s, error %d\n",
	       dred_decoder ? "non-null" : "null", dred_create_error);
	if (dred_decoder)
		opus_dred_decoder_destroy(dred_decoder);
	opus_encoder_destroy(encoder);
}

int main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--bench") == 0) {
		benchmark_all();
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "--probe") == 0) {
		feature_probe();
		return 0;
	}
	metadata_check();
	create_errors_check();
	round_trip_check(8000, 10, "round trip 8 kHz 10 ms tone SNR");
	round_trip_check(8000, 20, "round trip 8 kHz 20 ms tone SNR");
	round_trip_check(16000, 10, "round trip 16 kHz 10 ms tone SNR");
	round_trip_check(16000, 20, "round trip 16 kHz 20 ms tone SNR");
	bandwidth_check();
	encode_errors_check();
	decode_errors_check();
	plc_check();
	fec_check(8000, 20, "in-band FEC 8 kHz 20 ms beats plain PLC");
	fec_check(16000, 10, "in-band FEC 16 kHz 10 ms beats plain PLC");
	fec_check(16000, 20, "in-band FEC 16 kHz 20 ms beats plain PLC");
	bitrate_check();
	complexity_check();
	dtx_check();
	reset_check();
	return tu_summary("opus");
}
