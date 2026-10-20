// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HAVE_SPANDSP
#include <spandsp.h>
#endif

#include "../dsp/dsp.h"
#include "util.h"
#include "vectors/g726_vectors.h"

static double dbm0_amplitude(double level_dbm0);

static void sine_fill(int16_t *samples, size_t count, double frequency,
		      double amplitude, size_t offset)
{
	size_t i;

	for (i = 0; i < count; i++)
		samples[i] = (int16_t)lrint(amplitude * sin(2.0 * M_PI * frequency *
			(offset + i) / UDSP_SAMPLE_RATE));
}

static double frame_energy_get(const int16_t *samples, size_t count)
{
	double energy = 0.0;
	size_t i;

	for (i = 0; i < count; i++)
		energy += (double)samples[i] * samples[i];
	return energy / count;
}

static uint32_t test_random_next(uint32_t *state)
{
	uint32_t value = *state;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	*state = value;
	return value;
}

static struct udsp_codec *codec_create(const struct udsp_codec_ops *ops)
{
	const struct udsp_codec_params params = { 0 };

	return ops->create(&params);
}

static void g711_test(void)
{
	const struct udsp_codec_ops *const *ops;
	unsigned count = 0;

	/* Oracle: ITU-T G.711 A-law and mu-law characteristic endpoints. */
	CASE("g711 known compression points");
	CHECK_UINT(udsp_ulaw_encode(0), 0xff);
	CHECK_UINT(udsp_ulaw_encode(32767), 0x80);
	CHECK_UINT(udsp_ulaw_encode(-32768), 0x00);
	CHECK_UINT((uint16_t)udsp_ulaw_decode(0x80), 32124);
	CHECK_UINT((uint16_t)udsp_ulaw_decode(0x00), (uint16_t)-32124);
	CHECK_UINT(udsp_alaw_encode(0), 0xd5);
	CHECK_UINT(udsp_alaw_encode(32767), 0xaa);
	CHECK_UINT(udsp_alaw_encode(-32768), 0x2a);
	CHECK_UINT((uint16_t)udsp_alaw_decode(0xaa), 32256);
	CHECK_UINT((uint16_t)udsp_alaw_decode(0x2a), (uint16_t)-32256);

	/* Oracle: ITU-T G.711 zero and overload compression points. */
	CASE("g711 zero and full-scale round trip");
	CHECK(udsp_ulaw_decode(udsp_ulaw_encode(0)) == 0);
	CHECK(abs(udsp_ulaw_decode(udsp_ulaw_encode(32767)) - 32124) <= 1);
	CHECK(abs(udsp_alaw_decode(udsp_alaw_encode(0))) == 8);
	CHECK(abs(udsp_alaw_decode(udsp_alaw_encode(-32768)) + 32256) <= 1);

	CASE("g711 codec descriptor");
	CHECK_UINT(udsp_codec_builtin.abi_version, UDSP_CODEC_ABI_VERSION);
	CHECK_UINT(udsp_codec_builtin.struct_size, sizeof(struct udsp_codec_ops));
	CHECK(udsp_codec_builtin.ops[0] == &udsp_codec_pcmu_ops);
	CHECK(udsp_codec_builtin.ops[5] == &udsp_codec_g726_40_ops);
	CHECK(udsp_codec_builtin.ops[6] == &udsp_codec_g722_ops);
	CHECK(udsp_codec_builtin.ops[7] == &udsp_codec_g729_ops);
	ops = udsp_codec_builtin.ops;
	while (ops[count])
		count++;
	CHECK_UINT(count, 8);
}

static void codec_v2_test(void)
{
	const struct {
		const struct udsp_codec_ops *ops;
		unsigned max_payload;
		uint32_t flags;
	} codecs[] = {
		{ &udsp_codec_pcmu_ops, 240, UDSP_CODEC_F_PLC },
		{ &udsp_codec_pcma_ops, 240, UDSP_CODEC_F_PLC },
		{ &udsp_codec_g726_16_ops, 60, 0 },
		{ &udsp_codec_g726_24_ops, 90, 0 },
		{ &udsp_codec_g726_32_ops, 120, 0 },
		{ &udsp_codec_g726_40_ops, 150, 0 },
	};
	unsigned i;

	for (i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
		const struct udsp_codec_ops *ops = codecs[i].ops;
		struct udsp_codec *codec;
		char name[80];

		snprintf(name, sizeof(name), "%s v2 fields", ops->name);
		CASE(name);
		CHECK_UINT(ops->sample_rate, 8000);
		CHECK_UINT(ops->clock_rate, 8000);
		CHECK_UINT(ops->frame_ms, 10);
		CHECK_UINT(ops->ptime_mask, 0x07);
		CHECK_UINT(ops->max_payload, codecs[i].max_payload);
		CHECK_UINT(ops->flags, codecs[i].flags);
		CHECK(ops->cn == udsp_codec_cn);
		CHECK((ops->plc != NULL) ==
		      ((codecs[i].flags & UDSP_CODEC_F_PLC) != 0));
		codec = codec_create(ops);
		CHECK(codec != NULL);
		if (!codec)
			continue;
		CHECK_UINT(ops->rate_get(codec), 8000);
		ops->destroy(codec);
	}
}

/* The widest packet ptime_mask allows, counted in frames (codec.h Frames). */
static unsigned ptime_frames_max(uint8_t ptime_mask)
{
	unsigned frames = 0;
	unsigned bit;

	for (bit = 0; bit < 8; bit++) {
		if (ptime_mask & (1u << bit))
			frames = bit + 1;
	}
	return frames;
}

/*
 * codec.h v4: the frame shape that every registered codec encodes to. The walk
 * runs over the registry, and the sweep after it runs over the table. A codec
 * that joins the registry without a row fails, and so does a row whose codec
 * leaves the registry. Neither goes quiet.
 */
static void codec_v4_test(void)
{
	const struct {
		enum udsp_codec_id id;
		unsigned frame_bytes;
		unsigned sid_bytes;
	} expect[] = {
		{ UDSP_CODEC_PCMU, 80, 0 },
		{ UDSP_CODEC_PCMA, 80, 0 },
		{ UDSP_CODEC_G726_16, 20, 0 },
		{ UDSP_CODEC_G726_24, 30, 0 },
		{ UDSP_CODEC_G726_32, 40, 0 },
		{ UDSP_CODEC_G726_40, 50, 0 },
		{ UDSP_CODEC_G722, 80, 0 },
		{ UDSP_CODEC_G729, 10, 2 },
	};
	bool matched[sizeof(expect) / sizeof(expect[0])] = { false };
	const struct udsp_codec_ops *const *walk;
	unsigned i;

	for (walk = udsp_codec_builtin.ops; *walk; walk++) {
		const struct udsp_codec_ops *ops = *walk;
		bool pinned = false;
		char name[80];

		snprintf(name, sizeof(name), "%s v4 frame shape", ops->name);
		CASE(name);
		for (i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
			if (expect[i].id != ops->id)
				continue;
			pinned = true;
			matched[i] = true;
			CHECK_UINT(ops->frame_bytes, expect[i].frame_bytes);
			CHECK_UINT(ops->sid_bytes, expect[i].sid_bytes);
		}
		CHECK(pinned);
		if (!ops->frame_ms) {
			CHECK_UINT(ops->frame_bytes, 0);
			continue;
		}
		CHECK_UINT(ops->max_payload,
			   ops->frame_bytes * ptime_frames_max(ops->ptime_mask));
	}

	CASE("every v4 frame shape row has a registered codec");
	for (i = 0; i < sizeof(expect) / sizeof(expect[0]); i++)
		CHECK(matched[i]);
}

static void codec_v2_error_test(void)
{
	struct udsp_codec *codec = codec_create(&udsp_codec_pcmu_ops);
	const struct {
		const struct udsp_codec_ops *ops;
		unsigned rate;
		size_t encoded_len;
	} g726_codecs[] = {
		{ &udsp_codec_g726_16_ops, 16, 20 },
		{ &udsp_codec_g726_24_ops, 24, 30 },
		{ &udsp_codec_g726_32_ops, 32, 40 },
		{ &udsp_codec_g726_40_ops, 40, 50 },
	};
	int16_t pcm[UDSP_FRAME_SAMPLES] = { 0 };
	uint8_t encoded[UDSP_FRAME_SAMPLES] = { 0 };
	unsigned i;

	CASE("PCMU encode rejects 79 samples");
	CHECK(codec != NULL);
	if (!codec)
		return;
	CHECK(udsp_codec_pcmu_ops.encode(codec, pcm, 79, encoded,
					 sizeof(encoded)) == -EINVAL);

	CASE("PCMU encode rejects short output capacity");
	CHECK(udsp_codec_pcmu_ops.encode(codec, pcm, UDSP_FRAME_SAMPLES, encoded,
					 sizeof(encoded) - 1) == -ENOBUFS);

	CASE("PCMU decode rejects 79 sample capacity");
	CHECK(udsp_codec_pcmu_ops.decode(codec, encoded, sizeof(encoded), pcm,
					 79) == -ENOBUFS);

	CASE("g711 plc returns 80 samples");
	CHECK_UINT(udsp_codec_pcmu_ops.plc(codec, pcm, UDSP_FRAME_SAMPLES,
					  NULL, 0), UDSP_FRAME_SAMPLES);

	CASE("g711 plc rejects 79 samples");
	CHECK(udsp_codec_pcmu_ops.plc(codec, pcm, 79, NULL, 0) == -EINVAL);
	udsp_codec_pcmu_ops.destroy(codec);

	/* PCMA shares all G.711 validation paths with PCMU. */
	CASE("PCMA shared G711 validation paths");
	codec = codec_create(&udsp_codec_pcma_ops);
	CHECK(codec != NULL);
	if (!codec)
		return;
	CHECK(udsp_codec_pcma_ops.encode(codec, pcm, 79, encoded,
					 sizeof(encoded)) == -EINVAL);
	CHECK(udsp_codec_pcma_ops.encode(codec, pcm, UDSP_FRAME_SAMPLES, encoded,
					 sizeof(encoded) - 1) == -ENOBUFS);
	CHECK(udsp_codec_pcma_ops.decode(codec, encoded, sizeof(encoded), pcm,
					 79) == -ENOBUFS);
	CHECK(udsp_codec_pcma_ops.plc(codec, pcm, 79, NULL, 0) == -EINVAL);
	udsp_codec_pcma_ops.destroy(codec);

	for (i = 0; i < sizeof(g726_codecs) / sizeof(g726_codecs[0]); i++) {
		const struct udsp_codec_ops *ops = g726_codecs[i].ops;
		char name[80];

		codec = codec_create(ops);
		snprintf(name, sizeof(name),
			 "g726 %u kbit encode rejects 79 samples",
			 g726_codecs[i].rate);
		CASE(name);
		CHECK(codec != NULL);
		if (!codec)
			continue;
		CHECK(ops->encode(codec, pcm, 79, encoded,
				  sizeof(encoded)) == -EINVAL);

		snprintf(name, sizeof(name),
			 "g726 %u kbit encode rejects short capacity",
			 g726_codecs[i].rate);
		CASE(name);
		CHECK(ops->encode(codec, pcm, UDSP_FRAME_SAMPLES, encoded,
				  g726_codecs[i].encoded_len - 1) == -ENOBUFS);

		snprintf(name, sizeof(name),
			 "g726 %u kbit decode rejects 79 sample capacity",
			 g726_codecs[i].rate);
		CASE(name);
		CHECK(ops->decode(codec, encoded, g726_codecs[i].encoded_len,
				  pcm, 79) == -ENOBUFS);
		ops->destroy(codec);
	}
}

static void g711_plc_test(void)
{
	struct udsp_codec *codec = codec_create(&udsp_codec_pcmu_ops);
	int16_t input[UDSP_FRAME_SAMPLES];
	int16_t decoded[UDSP_FRAME_SAMPLES];
	int16_t concealed[UDSP_FRAME_SAMPLES];
	uint8_t encoded[UDSP_FRAME_SAMPLES];
	double energy;
	unsigned frame;

	for (frame = 0; frame < 4; frame++) {
		sine_fill(input, UDSP_FRAME_SAMPLES, 200.0, 9000.0,
			  frame * UDSP_FRAME_SAMPLES);
		udsp_codec_pcmu_ops.encode(codec, input, UDSP_FRAME_SAMPLES,
					    encoded, sizeof(encoded));
		udsp_codec_pcmu_ops.decode(codec, encoded, sizeof(encoded), decoded,
					    UDSP_FRAME_SAMPLES);
	}
	udsp_codec_pcmu_ops.plc(codec, concealed, UDSP_FRAME_SAMPLES, NULL, 0);
	energy = frame_energy_get(concealed, UDSP_FRAME_SAMPLES);
	CASE("g711 plc pitch continuation");
	CHECK(energy > 10000000.0);
	CHECK(abs(concealed[0] - decoded[0]) < 3000);

	for (frame = 0; frame < 7; frame++)
		udsp_codec_pcmu_ops.plc(codec, concealed, UDSP_FRAME_SAMPLES,
					NULL, 0);
	CASE("g711 plc appendix i attenuation");
	CHECK(frame_energy_get(concealed, UDSP_FRAME_SAMPLES) == 0.0);
	udsp_codec_pcmu_ops.destroy(codec);
}

static double g726_snr_get(const struct udsp_codec_ops *ops)
{
	struct udsp_codec *encoder = codec_create(ops);
	struct udsp_codec *decoder = codec_create(ops);
	int16_t input[UDSP_FRAME_SAMPLES];
	int16_t output[UDSP_FRAME_SAMPLES];
	uint8_t encoded[UDSP_FRAME_SAMPLES];
	double signal = 0.0;
	double noise = 0.0;
	unsigned frame;

	for (frame = 0; frame < 100; frame++) {
		unsigned i;
		size_t length;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			size_t n = frame * UDSP_FRAME_SAMPLES + i;
			input[i] = (int16_t)lrint(7000.0 * sin(2.0 * M_PI * 440.0 * n /
				UDSP_SAMPLE_RATE) + 3000.0 * sin(2.0 * M_PI * 1000.0 * n /
				UDSP_SAMPLE_RATE));
		}
		length = (size_t)ops->encode(encoder, input, UDSP_FRAME_SAMPLES,
					     encoded, sizeof(encoded));
		CHECK_UINT(ops->decode(decoder, encoded, length, output,
				       UDSP_FRAME_SAMPLES),
			   UDSP_FRAME_SAMPLES);
		if (frame < 10)
			continue;
		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			double error = input[i] - output[i];

			signal += (double)input[i] * input[i];
			noise += error * error;
		}
	}
	ops->destroy(encoder);
	ops->destroy(decoder);
	return 10.0 * log10(signal / noise);
}

static void g726_vector_input_fill(int16_t *samples)
{
	unsigned i;

	for (i = 0; i < 160; i++) {
		int ramp = (int)(i % 40) * 300 - 5850;

		samples[i] = (int16_t)lrint(ramp + 5000.0 *
			sin(2.0 * M_PI * 440.0 * i / UDSP_SAMPLE_RATE));
	}
}

static void g726_vector_test(void)
{
	const struct {
		const struct udsp_codec_ops *ops;
		const uint8_t *expected;
		size_t expected_length;
		const char *name;
	} vectors[] = {
		{ &udsp_codec_g726_16_ops, g726_16_expected,
		  sizeof(g726_16_expected), "g726 16 kbit frozen bytes" },
		{ &udsp_codec_g726_24_ops, g726_24_expected,
		  sizeof(g726_24_expected), "g726 24 kbit frozen bytes" },
		{ &udsp_codec_g726_32_ops, g726_32_expected,
		  sizeof(g726_32_expected), "g726 32 kbit frozen bytes" },
		{ &udsp_codec_g726_40_ops, g726_40_expected,
		  sizeof(g726_40_expected), "g726 40 kbit frozen bytes" },
	};
	int16_t input[160];
	unsigned i;

	g726_vector_input_fill(input);
	for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		struct udsp_codec *codec = codec_create(vectors[i].ops);
		uint8_t encoded[100];
		int first_result;
		int second_result;
		size_t first;
		size_t length;

		CASE(vectors[i].name);
		first_result = vectors[i].ops->encode(codec, input,
			UDSP_FRAME_SAMPLES, encoded, sizeof(encoded));
		CHECK(first_result > 0);
		if (first_result <= 0) {
			vectors[i].ops->destroy(codec);
			continue;
		}
		first = (size_t)first_result;
		second_result = vectors[i].ops->encode(codec,
			input + UDSP_FRAME_SAMPLES, UDSP_FRAME_SAMPLES,
			encoded + first, sizeof(encoded) - first);
		CHECK(second_result > 0);
		if (second_result <= 0) {
			vectors[i].ops->destroy(codec);
			continue;
		}
		length = first + (size_t)second_result;
		CHECK_UINT(length, vectors[i].expected_length);
		CHECK(memcmp(encoded, vectors[i].expected, length) == 0);
		vectors[i].ops->destroy(codec);
	}

	CASE("g726 codeword zero occupies octet zero low bits");
	{
		const int16_t first[] = { 5000, -5000, 2000, -1000 };
		struct udsp_codec *codec = codec_create(&udsp_codec_g726_16_ops);
		int16_t input[UDSP_FRAME_SAMPLES] = { 0 };
		uint8_t encoded[UDSP_FRAME_SAMPLES];

		memcpy(input, first, sizeof(first));
		udsp_codec_g726_16_ops.encode(codec, input, UDSP_FRAME_SAMPLES,
					       encoded, sizeof(encoded));
		CHECK_UINT(encoded[0] & 0x03, 0x01);
		udsp_codec_g726_16_ops.destroy(codec);
	}
}

static void g726_decode_reference_test(void)
{
	const unsigned selected_index[] = { 74, 686, 1194 };
	const int16_t expected[] = { -11528, 32767, -18924 };
	struct udsp_codec *codec = codec_create(&udsp_codec_g726_40_ops);
	uint32_t state = 1;
	int16_t selected[3] = { 0 };
	unsigned frame;

	CASE("g726 40 kbit decode of a seeded stream matches three reference "
	     "samples");
	for (frame = 0; frame < 15; frame++) {
		uint8_t encoded[50];
		int16_t decoded[UDSP_FRAME_SAMPLES];
		unsigned i;

		for (i = 0; i < sizeof(encoded); i++)
			encoded[i] = test_random_next(&state);
		CHECK_UINT(udsp_codec_g726_40_ops.decode(codec, encoded,
			     sizeof(encoded), decoded, UDSP_FRAME_SAMPLES),
			   UDSP_FRAME_SAMPLES);
		for (i = 0; i < sizeof(selected_index) /
		     sizeof(selected_index[0]); i++) {
			unsigned offset = selected_index[i] -
				frame * UDSP_FRAME_SAMPLES;

			if (offset < UDSP_FRAME_SAMPLES)
				selected[i] = decoded[offset];
		}
	}
	udsp_codec_g726_40_ops.destroy(codec);
	CHECK_UINT((uint16_t)selected[0], (uint16_t)expected[0]);
	CHECK_UINT((uint16_t)selected[1], (uint16_t)expected[1]);
	CHECK_UINT((uint16_t)selected[2], (uint16_t)expected[2]);
}

static void g726_test(void)
{
	const struct udsp_codec_ops *ops[] = {
		&udsp_codec_g726_16_ops, &udsp_codec_g726_24_ops,
		&udsp_codec_g726_32_ops, &udsp_codec_g726_40_ops,
	};
	const double floor[] = { 18.0, 26.0, 34.0, 40.0 };
	unsigned rate[] = { 16, 24, 32, 40 };
	unsigned i;

	/* Oracle: 18/26/34/40 dB round-trip SNR fallback, not reset vectors. */
	for (i = 0; i < 4; i++) {
		char name[80];
		double snr;

		snprintf(name, sizeof(name), "g726 %u kbit round-trip snr", rate[i]);
		CASE(name);
		snr = g726_snr_get(ops[i]);
		printf("# G726-%u SNR %.2f dB\n", rate[i], snr);
		CHECK(snr >= floor[i]);
	}

	CASE("g726 malformed frame rejected");
	{
		struct udsp_codec *codec = codec_create(&udsp_codec_g726_32_ops);
		uint8_t input[39] = { 0 };
		int16_t output[UDSP_FRAME_SAMPLES];

		CHECK(udsp_codec_g726_32_ops.decode(codec, input, sizeof(input),
						 output, UDSP_FRAME_SAMPLES) < 0);
		udsp_codec_g726_32_ops.destroy(codec);
	}
}

#ifdef HAVE_SPANDSP
static void g726_spandsp_packing_test(void)
{
	g726_state_t *reference;
	struct udsp_codec *codec;
	int16_t input[160];
	uint8_t expected[80];
	uint8_t actual[80];
	double amplitude;
	int first;
	int second;
	int expected_length;

	CASE("G.726 packing matches spandsp RFC 3551 order byte for byte");
	amplitude = dbm0_amplitude(-10.0);
	sine_fill(input, 160, 1000.0, amplitude, 0);
	reference = g726_init(NULL, 32000, G726_ENCODING_LINEAR,
			      G726_PACKING_RIGHT);
	codec = codec_create(&udsp_codec_g726_32_ops);
	CHECK(reference != NULL);
	CHECK(codec != NULL);
	if (!reference || !codec) {
		if (reference)
			g726_free(reference);
		if (codec)
			udsp_codec_g726_32_ops.destroy(codec);
		return;
	}

	expected_length = g726_encode(reference, expected, input, 160);
	first = udsp_codec_g726_32_ops.encode(codec, input,
		UDSP_FRAME_SAMPLES, actual, sizeof(actual));
	second = udsp_codec_g726_32_ops.encode(codec,
		input + UDSP_FRAME_SAMPLES, UDSP_FRAME_SAMPLES,
		actual + 40, sizeof(actual) - 40);
	CHECK_UINT(expected_length, sizeof(expected));
	CHECK_UINT(first, 40);
	CHECK_UINT(second, 40);
	if (expected_length == (int)sizeof(expected) && first == 40 &&
	    second == 40)
		CHECK(memcmp(actual, expected, sizeof(expected)) == 0);

	g726_free(reference);
	udsp_codec_g726_32_ops.destroy(codec);
}
#endif

static void goertzel_test(void)
{
	const double frequencies[] = { 350.0, 440.0, 1000.0 };
	struct udsp_goertzel_bank bank;
	int16_t samples[800];

	sine_fill(samples, 800, 440.0, 6000.0, 0);
	udsp_goertzel_init(&bank, frequencies, 3);
	udsp_goertzel_process(&bank, samples, 800);
	CASE("goertzel frequency discrimination");
	CHECK(udsp_goertzel_power(&bank, 1) > udsp_goertzel_power(&bank, 0) * 100.0);
	CHECK(udsp_goertzel_power(&bank, 1) > udsp_goertzel_power(&bank, 2) * 100.0);

	CASE("goertzel complex power identity");
	{
		unsigned i;

		sine_fill(samples, 205, 440.0, dbm0_amplitude(-10.0), 0);
		udsp_goertzel_reset(&bank);
		udsp_goertzel_process(&bank, samples, 205);
		for (i = 0; i < bank.count; i++) {
			struct udsp_goertzel_iq result;
			double power = udsp_goertzel_power(&bank, i);

			udsp_goertzel_result(&bank, i, &result);
			CHECK(fabs(result.re * result.re +
				   result.im * result.im - power) <=
			      1e-9 * power);
		}
	}

	CASE("goertzel single index equivalence");
	{
		struct udsp_goertzel_bank single = bank;
		struct udsp_goertzel_iq expected, actual;

		udsp_goertzel_result(&bank, 1, &expected);
		udsp_goertzel_reset_index(&single, 1);
		CHECK(single.q1[1] == 0.0);
		CHECK(single.q2[1] == 0.0);
		udsp_goertzel_process_index(&single, 1, samples, 205);
		udsp_goertzel_result(&single, 1, &actual);
		CHECK(actual.re == expected.re);
		CHECK(actual.im == expected.im);
		CHECK(memcmp(single.q1, bank.q1, sizeof(bank.q1)) == 0);
		CHECK(memcmp(single.q2, bank.q2, sizeof(bank.q2)) == 0);
	}

	CASE("goertzel invalid index");
	{
		struct udsp_goertzel_bank saved = bank;
		struct udsp_goertzel_iq result = { 1.0, 1.0 };

		udsp_goertzel_reset_index(&bank, bank.count);
		udsp_goertzel_process_index(&bank, bank.count, samples, 205);
		udsp_goertzel_result(&bank, bank.count, &result);
		CHECK(result.re == 0.0);
		CHECK(result.im == 0.0);
		CHECK(memcmp(bank.q1, saved.q1, sizeof(bank.q1)) == 0);
		CHECK(memcmp(bank.q2, saved.q2, sizeof(bank.q2)) == 0);
	}

	CASE("goertzel complex frequency offset recovery");
	{
		const double frequency = 941.0;
		const double w = 2.0 * M_PI * frequency / UDSP_SAMPLE_RATE;
		struct udsp_goertzel_iq y1, y2;
		double phase, delta;

		sine_fill(samples, 205, frequency + 10.0, dbm0_amplitude(-10.0), 0);
		udsp_goertzel_init(&bank, &frequency, 1);
		udsp_goertzel_process_index(&bank, 0, samples, 102);
		udsp_goertzel_result(&bank, 0, &y1);
		udsp_goertzel_reset_index(&bank, 0);
		udsp_goertzel_process_index(&bank, 0, samples + 102, 103);
		udsp_goertzel_result(&bank, 0, &y2);
		phase = atan2(y2.im * y1.re - y2.re * y1.im,
			      y2.re * y1.re + y2.im * y1.im);
		delta = remainder(phase - w * 103.0, 2.0 * M_PI) *
			UDSP_SAMPLE_RATE / (2.0 * M_PI * 102.5);
		CHECK(fabs(delta - 10.0) <= 0.5);
	}
}

static bool digit_detect(char digit, unsigned frames, uint16_t *duration)
{
	struct udsp_dtmf_generator generator;
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	int16_t samples[UDSP_FRAME_SAMPLES];
	unsigned frame;
	bool detected = false;

	udsp_dtmf_generator_init(&generator, digit, -10, UDSP_SAMPLE_RATE);
	udsp_dtmf_detector_init(&detector, false);
	for (frame = 0; frame < frames; frame++) {
		udsp_dtmf_generator_render(&generator, samples, UDSP_FRAME_SAMPLES);
		detected |= udsp_dtmf_detector_process(&detector, samples, &event);
	}
	detected |= udsp_dtmf_detector_flush(&detector, &event);
	if (detected)
		*duration = event.u.digit.duration_ms;
	return detected && event.u.digit.digit == digit;
}

static void dtmf_round_trip_test(void)
{
	const char digits[] = "123A456B789C*0#D";
	unsigned i;

	for (i = 0; i < sizeof(digits) - 1; i++) {
		char name[80];
		uint16_t duration = 0;

		snprintf(name, sizeof(name), "dtmf generated round trip %c", digits[i]);
		CASE(name);
		CHECK(digit_detect(digits[i], 8, &duration));
		CHECK_UINT(duration, 80);
	}

	CASE("dtmf 80 ms duration at end");
	{
		uint16_t duration = 0;

		/* Eleven decisions after warm-up plus five flush decisions
		 * account for the full 80 ms from a cold start. */
		CHECK(digit_detect('5', 8, &duration));
		CHECK_UINT(duration, 80);
	}

	CASE("dtmf cold start detects a 60 ms burst in frame zero");
	{
		uint16_t duration = 0;

		CHECK(digit_detect('5', 6, &duration));
		CHECK_UINT(duration, 60);
	}
}

static void dtmf_remote_test(void)
{
	struct udsp_dtmf_generator generator;
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	int16_t samples[UDSP_FRAME_SAMPLES];
	unsigned frame;
	bool detected = false;

	udsp_dtmf_generator_init(&generator, '5', -10, UDSP_SAMPLE_RATE);
	udsp_dtmf_detector_init(&detector, true);
	for (frame = 0; frame < 8; frame++) {
		udsp_dtmf_generator_render(&generator, samples, UDSP_FRAME_SAMPLES);
		detected |= udsp_dtmf_detector_process(&detector, samples, &event);
	}
	detected |= udsp_dtmf_detector_flush(&detector, &event);
	CASE("dtmf remote direction at end");
	CHECK(detected);
	CHECK_UINT(event.u.digit.remote, 1);
	CHECK_UINT(event.u.digit.duration_ms, 80);
}

static bool signal_detected(const int16_t frames[][UDSP_FRAME_SAMPLES],
			    unsigned count)
{
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	unsigned i;
	bool detected = false;

	udsp_dtmf_detector_init(&detector, false);
	for (i = 0; i < count; i++)
		detected |= udsp_dtmf_detector_process(&detector, frames[i], &event);
	detected |= udsp_dtmf_detector_flush(&detector, &event);
	return detected;
}

static void dtmf_pair_fill(int16_t *samples, unsigned frame,
			   double frequency_scale)
{
	unsigned i;

	for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
		size_t n = frame * UDSP_FRAME_SAMPLES + i;

		samples[i] = (int16_t)lrint(
			5000.0 * sin(2.0 * M_PI * 770.0 * frequency_scale * n /
				     UDSP_SAMPLE_RATE) +
			5000.0 * sin(2.0 * M_PI * 1336.0 * frequency_scale * n /
				     UDSP_SAMPLE_RATE));
	}
}

static unsigned dtmf_offset_events_get(double frequency_scale,
				       unsigned frames, int gap_frame,
				       uint16_t *duration)
{
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	int16_t samples[UDSP_FRAME_SAMPLES];
	unsigned events = 0;
	unsigned frame;

	udsp_dtmf_detector_init(&detector, false);
	for (frame = 0; frame < frames; frame++) {
		if ((int)frame == gap_frame)
			memset(samples, 0, sizeof(samples));
		else
			dtmf_pair_fill(samples, frame, frequency_scale);
		if (!udsp_dtmf_detector_process(&detector, samples, &event))
			continue;
		events++;
		*duration = event.u.digit.duration_ms;
	}
	if (udsp_dtmf_detector_flush(&detector, &event)) {
		events++;
		*duration = event.u.digit.duration_ms;
	}
	return events;
}

static void dtmf_tolerance_test(void)
{
	const double scale[] = { 0.985, 1.0, 1.015 };
	const char *name[] = {
		"dtmf q24 minus 1.5 percent",
		"dtmf q24 nominal frequency",
		"dtmf q24 plus 1.5 percent",
	};
	uint16_t duration;
	unsigned i;

	for (i = 0; i < 3; i++) {
		CASE(name[i]);
		duration = 0;
		CHECK_UINT(dtmf_offset_events_get(scale[i], 20, -1, &duration), 1);
		CHECK_UINT(duration, 200);
	}

	CASE("dtmf q24 plus 3.5 percent rejected");
	duration = 0;
	CHECK_UINT(dtmf_offset_events_get(1.035, 20, -1, &duration), 0);
}

static void dtmf_gap_flush_test(void)
{
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	int16_t silence[UDSP_FRAME_SAMPLES] = { 0 };
	int16_t off_grid[UDSP_FRAME_SAMPLES];
	uint16_t duration = 0;

	CASE("dtmf 100 ms digit rides through one gap");
	CHECK_UINT(dtmf_offset_events_get(1.0, 10, 6, &duration), 1);
	/* The 10 ms gap keeps tone in every window and preserves the run:
	 * fifteen decisions after warm-up plus five flush decisions. */
	CHECK_UINT(duration, 100);

	udsp_dtmf_detector_init(&detector, false);
	detector.active = '5';
	detector.active_decisions = 20;
	detector.sample_count = UDSP_DTMF_WINDOW_SAMPLES;
	sine_fill(detector.sample_window, UDSP_DTMF_WINDOW_SAMPLES, 500.0,
		  dbm0_amplitude(-20.0), 0);
	sine_fill(off_grid, UDSP_FRAME_SAMPLES, 500.0,
		  dbm0_amplitude(-20.0), UDSP_DTMF_WINDOW_SAMPLES);
	CASE("dtmf active digit rides four non-silent misses "
	     "and ends on fifth");
	CHECK(!udsp_dtmf_detector_process(&detector, off_grid, &event));
	CHECK_UINT(detector.missed_decisions, 2);
	CHECK(!udsp_dtmf_detector_process(&detector, off_grid, &event));
	CHECK_UINT(detector.missed_decisions, 4);
	CHECK_UINT(detector.missed_silent, 0);
	CHECK_UINT(detector.active, '5');
	CHECK(udsp_dtmf_detector_process(&detector, off_grid, &event));
	CHECK_UINT(event.u.digit.digit, '5');
	CHECK_UINT(event.u.digit.duration_ms, 120);
	CHECK_UINT(detector.active, 0);
	CHECK_UINT(detector.no_vsc_run, 6);
	CHECK(!memcmp(detector.sample_window + UDSP_DTMF_WINDOW_SAMPLES -
		      UDSP_FRAME_SAMPLES, off_grid, sizeof(off_grid)));

	udsp_dtmf_detector_init(&detector, false);
	detector.active = '5';
	detector.active_decisions = 20;
	detector.sample_count = UDSP_DTMF_WINDOW_SAMPLES;
	CASE("dtmf active digit ends on fourth consecutive silent miss");
	CHECK(!udsp_dtmf_detector_process(&detector, silence, &event));
	CHECK_UINT(detector.missed_decisions, 2);
	CHECK_UINT(detector.missed_silent, 2);
	CHECK_UINT(detector.active, '5');
	CHECK(udsp_dtmf_detector_process(&detector, silence, &event));
	CHECK_UINT(event.u.digit.digit, '5');
	CHECK_UINT(event.u.digit.duration_ms, 115);
	CHECK_UINT(detector.active, 0);
	CHECK_UINT(detector.no_vsc_run, 4);

	udsp_dtmf_detector_init(&detector, false);
	detector.active = '5';
	detector.active_decisions = 20;
	detector.sample_count = UDSP_DTMF_WINDOW_SAMPLES;
	detector.missed_decisions = 2;
	detector.missed_silent = 2;
	CASE("dtmf non-silent miss resets only the silent miss run");
	CHECK(!udsp_dtmf_detector_process(&detector, off_grid, &event));
	CHECK_UINT(detector.missed_decisions, 4);
	CHECK_UINT(detector.missed_silent, 0);
	CHECK_UINT(detector.active, '5');
	CHECK(udsp_dtmf_detector_process(&detector, silence, &event));
	CHECK_UINT(event.u.digit.digit, '5');
	CHECK_UINT(event.u.digit.duration_ms, 110);

	udsp_dtmf_detector_init(&detector, false);
	detector.active = '8';
	detector.active_decisions = 16;
	CASE("dtmf detector flush emits pending end");
	CHECK(udsp_dtmf_detector_flush(&detector, &event));
	CHECK_UINT(event.u.digit.digit, '8');
	CHECK_UINT(event.u.digit.duration_ms, 105);
	CHECK_UINT(detector.ended_digit, 0);
	CHECK(!detector.ended_digit_clear);
	CHECK_UINT(detector.no_vsc_run, 0);

	udsp_dtmf_detector_init(&detector, false);
	detector.active = '9';
	detector.active_decisions = UINT16_MAX / 5U + 100U;
	CASE("dtmf duration saturates at uint16 max");
	CHECK(udsp_dtmf_detector_flush(&detector, &event));
	CHECK_UINT(event.u.digit.duration_ms, UINT16_MAX);
}

static void dtmf_guard_test(void)
{
	struct udsp_dtmf_generator generator;
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	int16_t samples[UDSP_FRAME_SAMPLES] = { 0 };
	unsigned frame;

	CASE("dtmf same digit guard counts the misses that end the digit");
	udsp_dtmf_detector_init(&detector, false);
	detector.active = '5';
	detector.active_decisions = 20;
	detector.sample_count = UDSP_DTMF_WINDOW_SAMPLES;
	CHECK(!udsp_dtmf_detector_process(&detector, samples, &event));
	CHECK(udsp_dtmf_detector_process(&detector, samples, &event));
	CHECK_UINT(detector.ended_digit, '5');
	CHECK(!detector.ended_digit_clear);
	CHECK_UINT(detector.no_vsc_run, 4);
	CHECK(!udsp_dtmf_detector_process(&detector, samples, &event));
	CHECK_UINT(detector.no_vsc_run, 6);
	CHECK(!detector.ended_digit_clear);
	CHECK(!udsp_dtmf_detector_process(&detector, samples, &event));
	CHECK_UINT(detector.no_vsc_run, 7);
	CHECK(detector.ended_digit_clear);
	udsp_dtmf_generator_init(&generator, '5', -10, UDSP_SAMPLE_RATE);
	for (frame = 0; frame < 8; frame++) {
		udsp_dtmf_generator_render(&generator, samples,
					   UDSP_FRAME_SAMPLES);
		CHECK(!udsp_dtmf_detector_process(&detector, samples, &event));
	}
	CHECK_UINT(detector.active, '5');

	CASE("dtmf same digit guard resets the no-VSC run on a passing digit");
	udsp_dtmf_detector_init(&detector, false);
	detector.ended_digit = '5';
	detector.no_vsc_run = 6;
	detector.sample_count = UDSP_DTMF_WINDOW_SAMPLES;
	udsp_dtmf_generator_init(&generator, '5', -10, UDSP_SAMPLE_RATE);
	udsp_dtmf_generator_render(&generator, detector.sample_window,
				   UDSP_DTMF_WINDOW_SAMPLES);
	for (frame = 0; frame < 8; frame++) {
		udsp_dtmf_generator_render(&generator, samples,
					   UDSP_FRAME_SAMPLES);
		CHECK(!udsp_dtmf_detector_process(&detector, samples, &event));
	}
	CHECK_UINT(detector.active, 0);
	CHECK_UINT(detector.candidate, 0);
	CHECK_UINT(detector.candidate_decisions, 0);
	CHECK_UINT(detector.no_vsc_run, 0);
	CHECK(!detector.ended_digit_clear);

	CASE("dtmf different passing digit clears the same digit guard");
	udsp_dtmf_generator_init(&generator, '8', -10, UDSP_SAMPLE_RATE);
	udsp_dtmf_generator_render(&generator, detector.sample_window,
				   UDSP_DTMF_WINDOW_SAMPLES);
	udsp_dtmf_generator_render(&generator, samples, UDSP_FRAME_SAMPLES);
	CHECK(!udsp_dtmf_detector_process(&detector, samples, &event));
	CHECK_UINT(detector.active, 0);
	CHECK_UINT(detector.candidate, '8');
	CHECK_UINT(detector.candidate_decisions, 2);
	CHECK(detector.ended_digit_clear);
	udsp_dtmf_generator_init(&generator, '5', -10, UDSP_SAMPLE_RATE);
	udsp_dtmf_generator_render(&generator, detector.sample_window,
				   UDSP_DTMF_WINDOW_SAMPLES);
	for (frame = 0; frame < 4; frame++) {
		udsp_dtmf_generator_render(&generator, samples,
					   UDSP_FRAME_SAMPLES);
		CHECK(!udsp_dtmf_detector_process(&detector, samples, &event));
	}
	CHECK_UINT(detector.active, '5');

	CASE("dtmf init and inactive flush clear the same digit guard");
	udsp_dtmf_detector_init(&detector, false);
	CHECK_UINT(detector.ended_digit, 0);
	CHECK(!detector.ended_digit_clear);
	CHECK_UINT(detector.no_vsc_run, 0);
	detector.ended_digit = '5';
	detector.no_vsc_run = 6;
	CHECK(!udsp_dtmf_detector_flush(&detector, &event));
	CHECK_UINT(detector.ended_digit, 0);
	CHECK(!detector.ended_digit_clear);
	CHECK_UINT(detector.no_vsc_run, 0);
}

static void dtmf_talkoff_test(void)
{
	int16_t frames[6][UDSP_FRAME_SAMPLES];
	unsigned frame;

	for (frame = 0; frame < 6; frame++)
		sine_fill(frames[frame], UDSP_FRAME_SAMPLES, 697.0, 10000.0,
			  frame * UDSP_FRAME_SAMPLES);
	CASE("dtmf talk-off single tone rejected");
	CHECK(!signal_detected(frames, 6));

	for (frame = 0; frame < 6; frame++) {
		unsigned i;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			size_t n = frame * UDSP_FRAME_SAMPLES + i;
			double envelope = 0.55 + 0.45 * sin(2.0 * M_PI * 7.0 * n /
				UDSP_SAMPLE_RATE);

			frames[frame][i] = (int16_t)lrint(envelope *
				(5000.0 * sin(2.0 * M_PI * 430.0 * n / UDSP_SAMPLE_RATE) +
				 4000.0 * sin(2.0 * M_PI * 1080.0 * n / UDSP_SAMPLE_RATE)));
		}
	}
	CASE("dtmf talk-off speech envelope rejected");
	CHECK(!signal_detected(frames, 6));

	for (frame = 0; frame < 6; frame++) {
		unsigned i;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			size_t n = frame * UDSP_FRAME_SAMPLES + i;
			double value = 6000.0 * sin(2.0 * M_PI * 941.0 * n /
				UDSP_SAMPLE_RATE) + 6000.0 * sin(2.0 * M_PI * 1633.0 * n /
				UDSP_SAMPLE_RATE) + 3500.0 * sin(2.0 * M_PI * 1882.0 * n /
				UDSP_SAMPLE_RATE) + 3500.0 * sin(2.0 * M_PI * 3266.0 * n /
				UDSP_SAMPLE_RATE);

			frames[frame][i] = (int16_t)lrint(value);
		}
	}
	CASE("dtmf talk-off harmonic guard");
	CHECK(!signal_detected(frames, 6));

	for (frame = 0; frame < 6; frame++) {
		unsigned i;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			size_t n = frame * UDSP_FRAME_SAMPLES + i;

			frames[frame][i] = (int16_t)lrint(
				4500.0 * sin(2.0 * M_PI * 770.0 * n /
					     UDSP_SAMPLE_RATE) +
				12000.0 * sin(2.0 * M_PI * 1336.0 * n /
					      UDSP_SAMPLE_RATE));
		}
	}
	CASE("dtmf excessive forward twist of 8.5 dB rejected");
	CHECK(!signal_detected(frames, 6));
}

/*
 * The talk-off audio. A field capture shows the detector
 * validating two digits from the near-end talker's own speech, so the oracle
 * is a speech-shaped signal that carries no digit at all. The talk-off audio
 * is generated, not stored: a seeded source of voiced segments, unvoiced
 * bursts and pauses, filtered by the 300 Hz to 3400 Hz response the loop and
 * the codec put in front of the detector.
 *
 * What aims the talk-off audio at the detector. Two thirds of the voiced
 * segments ask for a grid pitch, which puts one harmonic on a row frequency
 * and another on a column frequency. That is the mechanism the field events
 * show, because 697/1209 and 770/1336 both sit within one percent of the 4:7
 * harmonic ratio. The pitch then drifts by 0.8 to 2.0 percent per frame and
 * jitters by up to 0.4 percent either way, and the three formants move, so
 * the alignment comes and goes.
 *
 * Why the talk-off audio carries no digit. The drift alone does not bound how
 * long an alignment lasts. The jitter can cancel a whole frame of drift.
 * Without the break below, a pair of this audio can stay inside the
 * acceptance band for five frames, which is longer than the four
 * decisions the detector needs. The generator therefore measures the
 * alignment on the tones it just rendered and breaks it: after
 * TALKOFF_GRID_FRAMES_MAX frames on the grid the segment ends and a pause
 * follows, and a pause carries no tone at all. No run of frames that holds a
 * grid pair inside the receiver acceptance band reaches 40 ms, the shortest
 * digit a receiver must report. The talk-off case checks that bound over the
 * same audio the zero assertion uses, so the assertion is a property of the
 * generator and not of the seeds.
 *
 * What the talk-off audio does not claim. Two aligned runs separated by a
 * pause are still not a digit, because a digit is one continuous tone pair;
 * the bound is on the continuous run. The talk-off audio is also much harsher
 * than a talker, so it is a stress oracle and not a rate calibration.
 */
/* Three frames is 30 ms of tone: one frame short of the four decisions the
 * detector needs, and 10 ms short of the shortest digit a receiver must
 * report. */
#define TALKOFF_GRID_FRAMES_MAX	3u
#define TALKOFF_HARMONIC_MAX	40u

enum talkoff_segment {
	TALKOFF_VOICED,
	TALKOFF_UNVOICED,
	TALKOFF_PAUSE,
};

enum talkoff_family {
	TALKOFF_FAMILY_MIX,
	TALKOFF_FAMILY_VOICED,
	TALKOFF_FAMILY_GLIDE,
};

struct talkoff_biquad {
	double b0, b1, b2, a1, a2, x1, x2, y1, y2;
};

struct talkoff_source {
	struct talkoff_biquad line[2];
	uint32_t state;
	unsigned family;
	unsigned segment;
	unsigned pos;
	unsigned length;
	double f0;
	double f0_rate;
	double formant[3];
	double formant_rate[3];
	double bandwidth[3];
	double level;
	double level_rate;
	double phase[TALKOFF_HARMONIC_MAX];
	double noise_z1, noise_z2, noise_frequency, noise_bandwidth;
	double tone_frequency[2], tone_rate[2], tone_phase[2], tone_level;
	unsigned grid_frames;
	unsigned grid_breaks;
	unsigned grid_run_max;
	unsigned voiced_segments;
	unsigned grid_segments;
	bool grid_now;
	bool force_pause;
};

static const double dtmf_row_hz[4] = { 697.0, 770.0, 852.0, 941.0 };
static const double dtmf_column_hz[4] = { 1209.0, 1336.0, 1477.0, 1633.0 };
static const char dtmf_digit_map[4][4] = {
	{ '1', '2', '3', 'A' },
	{ '4', '5', '6', 'B' },
	{ '7', '8', '9', 'C' },
	{ '*', '0', '#', 'D' },
};

static int16_t sample_clamp(double value)
{
	if (value > 32767.0)
		return 32767;
	if (value < -32768.0)
		return -32768;
	return (int16_t)lrint(value);
}

/* 0 dBm0 is 3.14 dB below a full scale sine, the level plan dsp/tone_gen.c
 * and dsp/dtmf_gen.c already encode. */
static double dbm0_amplitude(double level_dbm0)
{
	return 32767.0 * pow(10.0, (level_dbm0 - 3.14) / 20.0);
}

static double dbm0_rms(double level_dbm0)
{
	return dbm0_amplitude(level_dbm0) / sqrt(2.0);
}

static double talkoff_uniform(struct talkoff_source *source, double low,
			      double high)
{
	return low + (high - low) *
		(test_random_next(&source->state) / 4294967296.0);
}

static void talkoff_biquad_init(struct talkoff_biquad *filter, double frequency,
				bool high_pass)
{
	double w0 = 2.0 * M_PI * frequency / UDSP_SAMPLE_RATE;
	double alpha = sin(w0) / sqrt(2.0);
	double a0 = 1.0 + alpha;
	double cw = cos(w0);

	memset(filter, 0, sizeof(*filter));
	if (high_pass) {
		filter->b0 = (1.0 + cw) / 2.0 / a0;
		filter->b1 = -(1.0 + cw) / a0;
	} else {
		filter->b0 = (1.0 - cw) / 2.0 / a0;
		filter->b1 = (1.0 - cw) / a0;
	}
	filter->b2 = filter->b0;
	filter->a1 = -2.0 * cw / a0;
	filter->a2 = (1.0 - alpha) / a0;
}

static double talkoff_biquad_run(struct talkoff_biquad *filter, double x)
{
	double y = filter->b0 * x + filter->b1 * filter->x1 +
		filter->b2 * filter->x2 - filter->a1 * filter->y1 -
		filter->a2 * filter->y2;

	filter->x2 = filter->x1;
	filter->x1 = x;
	filter->y2 = filter->y1;
	filter->y1 = y;
	return y;
}

/*
 * ETSI ES 201 235-2 lets a transmitter sit 1.5 percent off the nominal
 * frequency and ES 201 235-3 makes a receiver accept 1.5 percent plus 2 Hz,
 * so a tone inside that band is a tone a conformant receiver may have to act
 * on. The bands of the two groups do not touch: the widest row band ends at
 * 957 Hz and the lowest column band starts at 1189 Hz, so one tone can never
 * satisfy both.
 */
static bool talkoff_near_grid(double frequency, const double *grid)
{
	unsigned i;

	for (i = 0; i < 4; i++)
		if (fabs(frequency - grid[i]) <= grid[i] * 0.015 + 2.0)
			return true;
	return false;
}

static bool talkoff_tone_pair_on_grid(double first, double second)
{
	if (talkoff_near_grid(first, dtmf_row_hz) &&
	    talkoff_near_grid(second, dtmf_column_hz))
		return true;
	return talkoff_near_grid(second, dtmf_row_hz) &&
		talkoff_near_grid(first, dtmf_column_hz);
}

static bool talkoff_comb_on_grid(double f0)
{
	bool row = false;
	bool column = false;
	unsigned k;

	for (k = 1; k < TALKOFF_HARMONIC_MAX; k++) {
		double frequency = k * f0;

		if (frequency > 3700.0)
			break;
		row = row || talkoff_near_grid(frequency, dtmf_row_hz);
		column = column || talkoff_near_grid(frequency, dtmf_column_hz);
	}
	return row && column;
}

/*
 * The search covers all sixteen row and column combinations from a random
 * start, so a voiced segment that asks for a grid pitch always gets one and
 * the share of grid segments is the two thirds the caller draws.
 */
static bool talkoff_grid_pitch_get(struct talkoff_source *source, double *f0,
				   double *row, double *column)
{
	unsigned start = test_random_next(&source->state) % 16u;
	unsigned i;

	for (i = 0; i < 16; i++) {
		unsigned r = (start + i) % 4u;
		unsigned c = (start + i) / 4u % 4u;
		unsigned k;

		for (k = 3; k <= 8; k++) {
			double base = dtmf_row_hz[r] / (double)k;
			unsigned m;

			if (base < 100.0 || base > 300.0)
				continue;
			m = (unsigned)lrint(dtmf_column_hz[c] / base);
			if (m <= k)
				continue;
			if (fabs(m * base - dtmf_column_hz[c]) >
			    0.015 * dtmf_column_hz[c])
				continue;
			*f0 = base;
			*row = dtmf_row_hz[r];
			*column = m * base;
			return true;
		}
	}
	return false;
}

static void talkoff_segment_next(struct talkoff_source *source)
{
	unsigned pick = test_random_next(&source->state) % 100u;
	double f0 = 0.0, row = 0.0, column = 0.0;
	double drift;

	source->pos = 0;
	source->level_rate = talkoff_uniform(source, -0.5, 0.5);
	if (source->force_pause) {
		/* the alignment cap fired: a pause carries no tone at all, so
		 * the run cannot continue into the next frame */
		source->force_pause = false;
		source->segment = TALKOFF_PAUSE;
		source->length = (unsigned)talkoff_uniform(source, 2.0, 6.0);
		source->level = -55.0;
		source->level_rate = 0.0;
		return;
	}
	if (source->family == TALKOFF_FAMILY_GLIDE) {
		source->segment = pick < 82u ? TALKOFF_VOICED : TALKOFF_PAUSE;
		source->length = (unsigned)talkoff_uniform(source, 8.0, 30.0);
		source->level = talkoff_uniform(source, -22.0, -8.0);
		source->tone_level = talkoff_uniform(source, -7.0, 4.0);
		source->tone_rate[0] = talkoff_uniform(source, 30.0, 90.0) *
			(test_random_next(&source->state) & 1u ? 1.0 : -1.0);
		source->tone_rate[1] = talkoff_uniform(source, 30.0, 120.0) *
			(test_random_next(&source->state) & 1u ? 1.0 : -1.0);
		if (source->tone_frequency[0] < 550.0 ||
		    source->tone_frequency[0] > 1100.0)
			source->tone_frequency[0] = talkoff_uniform(source, 600.0,
								   1000.0);
		if (source->tone_frequency[1] < 1050.0 ||
		    source->tone_frequency[1] > 1900.0)
			source->tone_frequency[1] = talkoff_uniform(source, 1150.0,
								   1750.0);
		return;
	}
	if (source->family == TALKOFF_FAMILY_VOICED)
		source->segment = pick < 88u ? TALKOFF_VOICED : TALKOFF_PAUSE;
	else if (pick < 62u)
		source->segment = TALKOFF_VOICED;
	else if (pick < 78u)
		source->segment = TALKOFF_UNVOICED;
	else
		source->segment = TALKOFF_PAUSE;
	switch (source->segment) {
	case TALKOFF_VOICED:
		source->length = (unsigned)talkoff_uniform(source, 6.0, 26.0);
		source->level = talkoff_uniform(source, -26.0, -8.0);
		source->voiced_segments++;
		if (test_random_next(&source->state) % 3u &&
		    talkoff_grid_pitch_get(source, &f0, &row, &column)) {
			source->grid_segments++;
			source->f0 = f0 * talkoff_uniform(source, 0.982, 1.018);
			source->formant[0] = row;
			source->formant[1] = column;
			source->bandwidth[0] = talkoff_uniform(source, 40.0, 90.0);
			source->bandwidth[1] = talkoff_uniform(source, 55.0, 130.0);
		} else {
			source->f0 = talkoff_uniform(source, 95.0, 290.0);
			source->formant[0] = talkoff_uniform(source, 270.0, 950.0);
			source->formant[1] = talkoff_uniform(source, 950.0, 2500.0);
			source->bandwidth[0] = talkoff_uniform(source, 45.0, 130.0);
			source->bandwidth[1] = talkoff_uniform(source, 60.0, 190.0);
		}
		source->formant[2] = talkoff_uniform(source, 2300.0, 3400.0);
		source->bandwidth[2] = talkoff_uniform(source, 120.0, 260.0);
		drift = talkoff_uniform(source, 0.008, 0.020);
		source->f0_rate = 1.0 +
			(test_random_next(&source->state) & 1u ? drift : -drift);
		source->formant_rate[0] = talkoff_uniform(source, -18.0, 18.0);
		source->formant_rate[1] = talkoff_uniform(source, -35.0, 35.0);
		source->formant_rate[2] = talkoff_uniform(source, -25.0, 25.0);
		break;
	case TALKOFF_UNVOICED:
		source->length = (unsigned)talkoff_uniform(source, 3.0, 9.0);
		source->level = talkoff_uniform(source, -32.0, -14.0);
		source->noise_frequency = talkoff_uniform(source, 1200.0, 3600.0);
		source->noise_bandwidth = talkoff_uniform(source, 250.0, 1200.0);
		break;
	default:
		source->length = (unsigned)talkoff_uniform(source, 4.0, 40.0);
		source->level = -55.0;
		source->level_rate = 0.0;
		break;
	}
}

static void talkoff_source_init(struct talkoff_source *source, uint32_t seed,
				unsigned family)
{
	memset(source, 0, sizeof(*source));
	talkoff_biquad_init(&source->line[0], 300.0, true);
	talkoff_biquad_init(&source->line[1], 3400.0, false);
	source->state = seed ? seed : 1u;
	source->family = family;
	source->f0 = 140.0;
	source->f0_rate = 1.0;
	source->formant[0] = 500.0;
	source->formant[1] = 1500.0;
	source->formant[2] = 2600.0;
	source->bandwidth[0] = 80.0;
	source->bandwidth[1] = 110.0;
	source->bandwidth[2] = 180.0;
	source->tone_frequency[0] = 700.0;
	source->tone_frequency[1] = 1300.0;
	/* a pause runs the same resonator as an unvoiced burst, and a zero
	 * bandwidth would put its pole on the unit circle */
	source->noise_frequency = 1500.0;
	source->noise_bandwidth = 600.0;
	source->level = -18.0;
	talkoff_segment_next(source);
}

static double talkoff_resonance(double frequency, double centre,
				double bandwidth)
{
	double r = frequency / centre;
	double imaginary = r * bandwidth / centre;
	double real = 1.0 - r * r;

	return 1.0 / sqrt(real * real + imaginary * imaginary);
}

static void talkoff_voiced_fill(struct talkoff_source *source, double *dst)
{
	static const double gain[3] = { 1.0, 0.72, 0.42 };
	double amplitude[TALKOFF_HARMONIC_MAX];
	double f0 = source->f0 * talkoff_uniform(source, 0.996, 1.004);
	unsigned k, n, count = 1;

	for (k = 1; k < TALKOFF_HARMONIC_MAX; k++) {
		double frequency = k * f0;
		double shape = 0.0;
		unsigned i;

		if (frequency > 3700.0)
			break;
		for (i = 0; i < 3; i++)
			shape += gain[i] * talkoff_resonance(frequency,
							     source->formant[i],
							     source->bandwidth[i]);
		amplitude[k] = shape / (double)k;
		count = k + 1;
	}
	source->grid_now = talkoff_comb_on_grid(f0);
	memset(dst, 0, UDSP_FRAME_SAMPLES * sizeof(*dst));
	for (k = 1; k < count; k++) {
		double w = 2.0 * M_PI * k * f0 / UDSP_SAMPLE_RATE;
		double c = 2.0 * cos(w);
		double y1 = sin(source->phase[k] - w);
		double y2 = sin(source->phase[k] - 2.0 * w);

		/* one oscillator recurrence per harmonic, not a sine call per
		 * sample: the talk-off audio is minutes long */
		for (n = 0; n < UDSP_FRAME_SAMPLES; n++) {
			double y = c * y1 - y2;

			y2 = y1;
			y1 = y;
			dst[n] += amplitude[k] * y;
		}
		source->phase[k] = fmod(source->phase[k] + w * UDSP_FRAME_SAMPLES,
					2.0 * M_PI);
	}
}

static void talkoff_unvoiced_fill(struct talkoff_source *source, double *dst)
{
	double r = exp(-M_PI * source->noise_bandwidth / UDSP_SAMPLE_RATE);
	double c = 2.0 * r * cos(2.0 * M_PI * source->noise_frequency /
				 UDSP_SAMPLE_RATE);
	unsigned n;

	for (n = 0; n < UDSP_FRAME_SAMPLES; n++) {
		double x = test_random_next(&source->state) / 2147483648.0 - 1.0;
		double value = x + c * source->noise_z1 -
			r * r * source->noise_z2;

		source->noise_z2 = source->noise_z1;
		source->noise_z1 = value;
		dst[n] = value;
	}
}

static void talkoff_glide_fill(struct talkoff_source *source, double *dst)
{
	unsigned n, i;

	source->grid_now = talkoff_tone_pair_on_grid(source->tone_frequency[0],
						     source->tone_frequency[1]);
	for (n = 0; n < UDSP_FRAME_SAMPLES; n++) {
		double value = 0.0;

		for (i = 0; i < 2; i++) {
			value += pow(10.0, i * source->tone_level / 20.0) *
				sin(source->tone_phase[i]);
			source->tone_phase[i] += 2.0 * M_PI *
				source->tone_frequency[i] / UDSP_SAMPLE_RATE;
		}
		dst[n] = value;
	}
	for (i = 0; i < 2; i++)
		source->tone_phase[i] = fmod(source->tone_phase[i], 2.0 * M_PI);
}

static void talkoff_frame_fill(struct talkoff_source *source, int16_t *dst)
{
	double raw[UDSP_FRAME_SAMPLES];
	double level = source->level + source->level_rate * source->pos +
		3.5 * sin(2.0 * M_PI * 3.1 * source->pos * UDSP_FRAME_SAMPLES /
			  UDSP_SAMPLE_RATE);
	double energy = 0.0, scale;
	unsigned i;

	if (level > -4.0)
		level = -4.0;
	switch (source->segment) {
	case TALKOFF_VOICED:
		if (source->family == TALKOFF_FAMILY_GLIDE) {
			talkoff_glide_fill(source, raw);
			for (i = 0; i < 2; i++)
				source->tone_frequency[i] += source->tone_rate[i];
			break;
		}
		talkoff_voiced_fill(source, raw);
		source->f0 *= source->f0_rate;
		for (i = 0; i < 3; i++)
			source->formant[i] += source->formant_rate[i];
		if (source->f0 < 80.0 || source->f0 > 330.0)
			source->f0_rate = 1.0 / source->f0_rate;
		break;
	case TALKOFF_UNVOICED:
		talkoff_unvoiced_fill(source, raw);
		source->grid_now = false;
		break;
	default:
		talkoff_unvoiced_fill(source, raw);
		source->grid_now = false;
		level = -55.0;
		break;
	}
	for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
		raw[i] = talkoff_biquad_run(&source->line[1],
					    talkoff_biquad_run(&source->line[0],
							       raw[i]));
		energy += raw[i] * raw[i];
	}
	scale = energy > 0.0 ? dbm0_rms(level) /
		sqrt(energy / UDSP_FRAME_SAMPLES) : 0.0;
	for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
		dst[i] = sample_clamp(raw[i] * scale);
	source->grid_frames = source->grid_now ? source->grid_frames + 1u : 0u;
	if (source->grid_frames > source->grid_run_max)
		source->grid_run_max = source->grid_frames;
	if (source->grid_frames >= TALKOFF_GRID_FRAMES_MAX) {
		source->grid_frames = 0;
		source->grid_breaks++;
		source->force_pause = true;
	}
	source->pos++;
	if (source->force_pause || source->pos >= source->length)
		talkoff_segment_next(source);
}

static unsigned talkoff_events_get(unsigned family, uint32_t seed,
				   unsigned minutes, struct talkoff_source *stats)
{
	struct talkoff_source source;
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	int16_t samples[UDSP_FRAME_SAMPLES];
	unsigned frames = minutes * 6000u;
	unsigned events = 0;
	unsigned frame;

	talkoff_source_init(&source, seed, family);
	udsp_dtmf_detector_init(&detector, false);
	for (frame = 0; frame < frames; frame++) {
		talkoff_frame_fill(&source, samples);
		if (udsp_dtmf_detector_process(&detector, samples, &event))
			events++;
	}
	if (udsp_dtmf_detector_flush(&detector, &event))
		events++;
	if (stats)
		*stats = source;
	return events;
}

static void dtmf_talkoff_audio_test(void)
{
	static const uint32_t seed[3] = { 0x5eed1234u, 0x00c0ffeeu, 0x13572468u };
	static const char *name[3] = {
		"dtmf talk-off audio mixed speech",
		"dtmf talk-off audio voiced formants",
		"dtmf talk-off audio formant glides",
	};
	unsigned voiced = 0, grid = 0, breaks = 0, run_max = 0;
	unsigned family, i;

	/* Four minutes per seed and family, twelve minutes per family. */
	for (family = 0; family < 3; family++) {
		unsigned events = 0;

		CASE(name[family]);
		for (i = 0; i < 3; i++) {
			struct talkoff_source stats;

			events += talkoff_events_get(family, seed[i], 4, &stats);
			voiced += stats.voiced_segments;
			grid += stats.grid_segments;
			breaks += stats.grid_breaks;
			if (stats.grid_run_max > run_max)
				run_max = stats.grid_run_max;
		}
		CHECK_UINT(events, 0);
		printf("# %s: %u events\n", name[family], events);
	}

	/* The zero above is only worth something if the talk-off audio cannot
	 * hold a digit. These are the same 36 minutes of audio. */
	CASE("dtmf talk-off audio holds no conformant digit");
	CHECK(run_max <= TALKOFF_GRID_FRAMES_MAX);
	CHECK(breaks > 0);
	CHECK(grid * 3u > voiced * 2u - voiced / 20u);
	CHECK(grid * 3u < voiced * 2u + voiced / 20u);
}

static double dtmf_pair_offsets_sample_get(unsigned row, unsigned column,
		unsigned n, double row_amplitude, double column_amplitude,
		const double offsets[2])
{
	double t = 2.0 * M_PI * n / UDSP_SAMPLE_RATE;

	return row_amplitude * sin((dtmf_row_hz[row] + offsets[0]) * t) +
		column_amplitude * sin((dtmf_column_hz[column] + offsets[1]) * t);
}

/* The ETSI accept edge of a tone, 1.5 percent plus 2 Hz from nominal. */
static double dtmf_edge_hz(double nominal)
{
	return 0.015 * nominal + 2.0;
}

static unsigned dtmf_burst_offsets_events_get(unsigned row, unsigned column,
				      unsigned on_ms, double level_dbm0,
				      double twist_db, unsigned lead_samples,
				      unsigned gap_ms, char *digit,
				      uint16_t *duration, unsigned *end_samples,
				      const double offsets[2])
{
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	int16_t samples[UDSP_FRAME_SAMPLES];
	double row_amplitude = dbm0_amplitude(twist_db > 0.0 ?
					      level_dbm0 - twist_db : level_dbm0);
	double column_amplitude = dbm0_amplitude(twist_db > 0.0 ?
						 level_dbm0 : level_dbm0 + twist_db);
	unsigned events = 0;
	unsigned gap_start;
	unsigned frame;

	udsp_dtmf_detector_init(&detector, false);
	lead_samples += 5u * UDSP_FRAME_SAMPLES;
	/* The gap follows 50 ms so the detector can validate the digit. */
	gap_start = lead_samples + 50u * UDSP_SAMPLE_RATE / 1000u;
	for (frame = 0; frame < 40; frame++) {
		unsigned counted = detector.active_decisions;
		unsigned i;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			unsigned n = frame * UDSP_FRAME_SAMPLES + i;

			samples[i] = 0;
			if (n < lead_samples ||
			    n >= lead_samples +
				 on_ms * UDSP_SAMPLE_RATE / 1000u)
				continue;
			if (n >= gap_start &&
			    n < gap_start + gap_ms * UDSP_SAMPLE_RATE / 1000u)
				continue;
			samples[i] = sample_clamp(
				dtmf_pair_offsets_sample_get(row, column,
					n - lead_samples, row_amplitude,
					column_amplitude, offsets));
		}
		if (!udsp_dtmf_detector_process(&detector, samples, &event))
			continue;
		events++;
		*digit = event.u.digit.digit;
		*duration = event.u.digit.duration_ms;
		/* The duration counts every decision of the digit but the one
		 * that ends it, so it names the decision of the frame that
		 * ended it. */
		if (end_samples)
			*end_samples = frame * UDSP_FRAME_SAMPLES +
				(1u + *duration / 5u - counted) *
				UDSP_DTMF_HOP_SAMPLES;
	}
	if (udsp_dtmf_detector_flush(&detector, &event)) {
		events++;
		*digit = event.u.digit.digit;
		*duration = event.u.digit.duration_ms;
		if (end_samples)
			*end_samples = frame * UDSP_FRAME_SAMPLES;
	}
	return events;
}

static unsigned dtmf_burst_events_get(unsigned row, unsigned column,
		unsigned on_ms, double level_dbm0, double twist_db,
		unsigned lead_samples, unsigned gap_ms, char *digit,
		uint16_t *duration, unsigned *end_samples)
{
	const double offsets[2] = { 0.0, 0.0 };

	return dtmf_burst_offsets_events_get(row, column, on_ms, level_dbm0,
		twist_db, lead_samples, gap_ms, digit, duration, end_samples,
		offsets);
}

struct dtmf_offset_case {
	unsigned pair;
	unsigned on_ms;
	unsigned lead;
	double level;
	double twist;
	double offsets[2];
};

static void dtmf_edge_set(struct dtmf_offset_case *cell, unsigned signs)
{
	cell->offsets[0] = (signs & 1u ? 1.0 : -1.0) *
		dtmf_edge_hz(dtmf_row_hz[cell->pair / 4]);
	cell->offsets[1] = (signs & 2u ? 1.0 : -1.0) *
		dtmf_edge_hz(dtmf_column_hz[cell->pair % 4]);
}

static bool dtmf_offset_expected_run(const struct dtmf_offset_case *cell,
				     char expected, bool diagnose, bool report)
{
	unsigned row = cell->pair / 4;
	unsigned column = cell->pair % 4;
	uint16_t duration = 0;
	char digit = 0;
	unsigned events = dtmf_burst_offsets_events_get(row, column, cell->on_ms,
		cell->level, cell->twist, cell->lead, 0, &digit, &duration, NULL,
		cell->offsets);
	bool pass = expected ? events == 1 && digit == expected :
		events == 0;

	if (report || (!pass && diagnose))
		printf("# offset %s: digit=%c ms=%u lead=%u level=%.0f "
		       "twist=%+.0f row=%+.3f Hz column=%+.3f Hz events=%u "
		       "seen=%c expected=%c\n", report ? "report" : "failure",
		       dtmf_digit_map[row][column],
		       cell->on_ms, cell->lead, cell->level, cell->twist,
		       cell->offsets[0], cell->offsets[1], events,
		       digit ? digit : '.', expected ? expected : '.');
	return pass;
}

static bool dtmf_offset_case_run(const struct dtmf_offset_case *cell,
				 bool accept, bool diagnose)
{
	char expected = accept ?
		dtmf_digit_map[cell->pair / 4][cell->pair % 4] : 0;

	return dtmf_offset_expected_run(cell, expected, diagnose, false);
}

static const unsigned dtmf_edge_pairs[5] = { 3, 7, 11, 15, 12 };
static const double dtmf_edge_levels[3] = { -4, -13, -28 };
static const double dtmf_edge_twists[3] = { -6, 0, 6 };

static void dtmf_band_edge_test(void)
{
	static const unsigned leads[4] = { 0, 19, 37, 61 };
	static const unsigned durations[3] = { 40, 50, 100 };
	static const unsigned twist_leads[8] = { 0, 11, 23, 34, 45, 57, 68, 79 };
	unsigned i, bad = 0;

	CASE("dtmf independent band edges accept every digit");
	for (i = 0; i < 16u * 4u * 4u; i++) {
		struct dtmf_offset_case cell = {
			.pair = i % 16, .on_ms = 200, .lead = leads[i / 16 % 4],
			.level = -10,
		};

		dtmf_edge_set(&cell, i / 64);
		bad += !dtmf_offset_case_run(&cell, true, true);
	}
	CHECK_UINT(bad, 0);
	printf("# band edge: %u/256 accepted\n", 256u - bad);

	/* This sweep asserts the corners from 50 ms on ten leads. The duration
	 * test below checks the floor at 60 ms on every lead, and reports the
	 * 40 ms cells. */
	CASE("dtmf band edges accept A B C D star from 50 ms");
	bad = 0;
	for (i = 0; i < 5u * 3u * 10u * 3u * 3u * 4u; i++) {
		unsigned on_ms = durations[i / 5 % 3];
		struct dtmf_offset_case cell = {
			.pair = dtmf_edge_pairs[i % 5],
			.on_ms = on_ms, .lead = i / 15 % 10 * 8,
			.level = dtmf_edge_levels[i / 150 % 3],
			.twist = dtmf_edge_twists[i / 450 % 3],
		};
		bool asserted = on_ms >= 50;
		bool pass;

		dtmf_edge_set(&cell, i / 1350);
		pass = dtmf_offset_case_run(&cell, true, asserted);
		bad += asserted && !pass;
	}
	CHECK_UINT(bad, 0);
	printf("# band edge corners at 50+: %u bad of 3600\n", bad);

	CASE("dtmf true 6 dB twist accepted at independent band edges");
	bad = 0;
	for (i = 0; i < 16u * 2u * 8u * 4u; i++) {
		struct dtmf_offset_case cell = {
			.pair = i % 16, .on_ms = 100,
			.lead = twist_leads[i / 32 % 8], .level = -10,
			.twist = i / 16 % 2 ? 6 : -6,
		};

		dtmf_edge_set(&cell, i / 256);
		bad += !dtmf_offset_case_run(&cell, true, true);
	}
	CHECK_UINT(bad, 0);
	printf("# band edge twist: %u/1024 accepted\n", 1024u - bad);
}

static unsigned dtmf_edge_duration_count(unsigned pair, unsigned on_ms,
					 bool diagnose)
{
	unsigned i, accepted = 0;

	for (i = 0; i < 80u * 4u * 3u * 3u; i++) {
		struct dtmf_offset_case cell = {
			.pair = pair, .on_ms = on_ms, .lead = i % 80,
			.level = dtmf_edge_levels[i / 320 % 3],
			.twist = dtmf_edge_twists[i / 960],
		};

		dtmf_edge_set(&cell, i / 80 % 4);
		accepted += dtmf_offset_case_run(&cell, true, diagnose);
	}
	return accepted;
}

static void dtmf_edge_duration_test(void)
{
	static const unsigned on[5] = { 35, 40, 45, 50, 60 };
	unsigned i;

	/* The deepest corner: each tone at an accept edge, in all four sign
	 * pairs, with a twist of -6, 0 or +6 dB. The louder tone is -4 to
	 * -28 dBm0, so the weaker tone reaches -34 dBm0. Measured floor: 100
	 * percent from 60 ms, 99 to 100 percent at 50 ms, 83 to 97 percent at
	 * 40 ms. A common clock error shifts both tones the same way, so only
	 * independent tone errors reach opposite edges. The floor at nominal
	 * frequency is 35 ms. */
	CASE("dtmf band edge floor reported, pinned at 60 ms");
	puts("# edge_floor\tdigit\tms\taccepted\ttotal");
	for (i = 0; i < 25; i++) {
		unsigned pair = dtmf_edge_pairs[i / 5];
		unsigned on_ms = on[i % 5];
		bool asserted = on_ms == 60;
		unsigned accepted = dtmf_edge_duration_count(pair, on_ms,
							     asserted);

		printf("# edge_floor\t%c\t%u\t%u\t2880\n",
		       dtmf_digit_map[pair / 4][pair % 4], on_ms, accepted);
		if (asserted)
			CHECK_UINT(accepted, 2880);
	}
}

static int dtmf_offset_expected_get(const struct dtmf_offset_case *cell,
				    bool *mandatory)
{
	const double *groups[2] = { dtmf_row_hz, dtmf_column_hz };
	unsigned original[2] = { cell->pair / 4, cell->pair % 4 };
	unsigned nearest[2];
	bool accept = true, reject = true;
	unsigned side, i;

	*mandatory = fabs(cell->twist) <= 6.0;
	for (side = 0; side < 2; side++) {
		const double *group = groups[side];
		double frequency = group[original[side]] + cell->offsets[side];
		double distance = INFINITY;

		for (i = 0; i < 4; i++) {
			double delta = fabs(frequency - group[i]);

			if (delta >= distance)
				continue;
			distance = delta;
			nearest[side] = i;
		}
		accept &= distance <= 0.015 * group[nearest[side]] + 2.0;
		*mandatory &= distance <= 0.015 * group[nearest[side]] + 1e-9;
		if (cell->offsets[side])
			reject &= distance + 1e-9 >= 0.030 * group[nearest[side]];
	}
	if (accept)
		return dtmf_digit_map[nearest[0]][nearest[1]];
	return reject ? 0 : -1;
}

static void dtmf_offset_reject_test(void)
{
	static const double percents[5] = { 0.030, 0.035, 0.045, 0.060, 0.080 };
	static const double twists[3] = { -8, 0, 6 };
	static const unsigned durations[3] = { 10, 15, 16 };
	unsigned accepted = 0, rejected = 0, skipped = 0;
	unsigned accept_total = 0, reject_total = 0;
	unsigned reported = 0, report_total = 0;
	unsigned i, bad = 0;

	CASE("dtmf swept offsets follow the nearest nominal bands");
	for (i = 0; i < 16u * 5u * 8u * 3u * 2u * 2u; i++) {
		struct dtmf_offset_case cell = {
			.pair = i % 16, .on_ms = 200,
			.twist = twists[i / 640 % 3],
			.level = i / 1920 % 2 ? -28 : -4,
			.lead = i / 3840 ? 37 : 0,
		};
		unsigned mode = i / 80 % 8;
		double fraction = percents[i / 16 % 5];
		int expected;
		bool pass, mandatory;

		cell.offsets[0] = mode >= 6 ? 0 :
			(mode & 1u ? 1 : -1) * fraction * dtmf_row_hz[cell.pair / 4];
		cell.offsets[1] = mode == 4 || mode == 5 ? 0 :
			(mode < 4 ? (mode & 2u ? 1 : -1) : (mode & 1u ? 1 : -1)) *
			fraction * dtmf_column_hz[cell.pair % 4];
		expected = dtmf_offset_expected_get(&cell, &mandatory);
		if (expected < 0) {
			skipped++;
			continue;
		}
		pass = dtmf_offset_expected_run(&cell, expected, true,
						expected && !mandatory);
		if (expected && !mandatory) {
			report_total++;
			reported += pass;
			continue;
		}
		bad += !pass;
		if (expected) {
			accept_total++;
			accepted += pass;
		} else {
			reject_total++;
			rejected += pass;
		}
	}
	CHECK_UINT(bad, 0);
	printf("# offset rejection: %u/%u zero-event cells; "
	       "neighbor acceptance: %u/%u exact-digit cells; "
	       "may-band: %u skipped\n", rejected, reject_total,
	       accepted, accept_total, skipped);
	printf("# neighbor report: %u/%u exact-digit cells\n", reported, report_total);

	CASE("dtmf 3 percent short pairs reject at every lead");
	bad = 0;
	for (i = 0; i < 16u * 3u * 80u * 2u; i++) {
		struct dtmf_offset_case cell = {
			.pair = i % 16, .on_ms = durations[i / 16 % 3],
			.lead = i / 48 % 80, .level = -4,
		};
		double fraction = i / 3840 ? 0.03 : -0.03;

		cell.offsets[0] = fraction * dtmf_row_hz[cell.pair / 4];
		cell.offsets[1] = fraction * dtmf_column_hz[cell.pair % 4];
		bad += !dtmf_offset_case_run(&cell, false, true);
	}
	CHECK_UINT(bad, 0);
	printf("# gate skip: %u/7680 zero-event cells\n", 7680u - bad);
}

static void dtmf_offset_level_test(void)
{
	unsigned i, bad = 0;

	CASE("dtmf level floor rejects -34 and accepts -28 at band edges");
	for (i = 0; i < 16u * 5u * 2u * 4u; i++) {
		struct dtmf_offset_case cell = {
			.pair = i % 16, .on_ms = 200, .lead = i / 160 * 19,
			.level = i / 80 % 2 ? -28 : -34,
		};
		unsigned edge = i / 16 % 5;

		if (edge)
			dtmf_edge_set(&cell, edge - 1);
		bad += !dtmf_offset_case_run(&cell, cell.level == -28, true);
	}
	CHECK_UINT(bad, 0);
	printf("# offset level floor: %u/640 passed\n", 640u - bad);
}

static void dtmf_drift_frame_fill(int16_t samples[UDSP_FRAME_SAMPLES],
				  unsigned pair, unsigned frame)
{
	unsigned i;

	for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
		double n = (double)frame * UDSP_FRAME_SAMPLES + i - 400;
		/* Integrating a linear frequency ramp halves its phase term. */
		double offsets[2] = {
			dtmf_row_hz[pair / 4] * 0.005 * n / 80,
			dtmf_column_hz[pair % 4] * 0.005 * n / 80,
		};

		samples[i] = 0;
		if (n >= 0 && n < 2400)
			samples[i] = sample_clamp(dtmf_pair_offsets_sample_get(
				pair / 4, pair % 4, n, 5000, 5000, offsets));
	}
}

static unsigned dtmf_drift_events_get(unsigned pair)
{
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	unsigned frame, events = 0;

	udsp_dtmf_detector_init(&detector, false);
	for (frame = 0; frame < 41; frame++) {
		int16_t samples[UDSP_FRAME_SAMPLES];

		dtmf_drift_frame_fill(samples, pair, frame);
		events += udsp_dtmf_detector_process(&detector, samples, &event);
	}
	return events + udsp_dtmf_detector_flush(&detector, &event);
}

static void dtmf_offset_drift_test(void)
{
	unsigned pair, event_cells = 0, total = 0;

	for (pair = 0; pair < 16; pair++) {
		unsigned events = dtmf_drift_events_get(pair);

		CHECK_UINT(events, 0);
		event_cells += events != 0;
		total += events;
	}
	printf("# frequency drift: %u/16 zero-event cells, %u events\n",
	       16u - event_cells, total);
}

static double dtmf_run_delta_get(const struct udsp_dtmf_offset_run *run)
{
	double hop, coarse, fine;
	double step = UDSP_SAMPLE_RATE / 102.5;

	if (!run->decisions || !run->adjacent)
		return NAN;
	hop = 0.5 * run->centroid_hops_x2 / run->adjacent;
	coarse = carg(run->quarter) * UDSP_SAMPLE_RATE / (2.0 * M_PI * hop);
	fine = carg(run->half);
	if (fine <= -M_PI)
		fine += 2.0 * M_PI;
	fine *= UDSP_SAMPLE_RATE / (2.0 * M_PI * 102.5);
	return fine + round((coarse - fine) / step) * step;
}

static double dtmf_offset_hold_error_get(const struct dtmf_offset_case *cell)
{
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	double row_amplitude = dbm0_amplitude(cell->twist > 0 ? -34 : -28);
	double column_amplitude = dbm0_amplitude(cell->twist > 0 ? -28 : -34);
	double error = 0;
	unsigned frame, checked = 0, events = 0;
	bool flushed;

	udsp_dtmf_detector_init(&detector, false);
	for (frame = 0; frame < 35; frame++) {
		int16_t samples[UDSP_FRAME_SAMPLES] = { 0 };
		unsigned i;

		for (i = 0; i < UDSP_FRAME_SAMPLES && frame >= 5; i++)
			samples[i] = sample_clamp(dtmf_pair_offsets_sample_get(
				cell->pair / 4, cell->pair % 4, (frame - 5) * 80 + i,
				row_amplitude, column_amplitude, cell->offsets));
		events += udsp_dtmf_detector_process(&detector, samples, &event);
		if (!detector.active)
			continue;
		CHECK_UINT(detector.active,
			   dtmf_digit_map[cell->pair / 4][cell->pair % 4]);
		for (i = 0; i < 2; i++) {
			double delta = dtmf_run_delta_get(&detector.offset_run[i]);

			error = isfinite(delta) ?
				fmax(error, fabs(delta - cell->offsets[i])) : INFINITY;
		}
		checked++;
	}
	flushed = udsp_dtmf_detector_flush(&detector, &event);
	if (events || checked < 20 || !flushed || error >= 5)
		printf("# hold failure: pair=%u twist=%+.0f row=%+.3f Hz "
		       "column=%+.3f Hz active_frames=%u events=%u flushed=%u "
		       "error=%.6f Hz\n", cell->pair, cell->twist,
		       cell->offsets[0], cell->offsets[1], checked, events,
		       flushed, error);
	CHECK_UINT(events, 0);
	CHECK(checked >= 20);
	CHECK(flushed);
	CHECK_UINT(detector.offset_run[0].decisions, 0);
	CHECK_UINT(detector.offset_run[1].decisions, 0);
	return error;
}

static void dtmf_offset_validation_test(void)
{
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	struct dtmf_offset_case cell = { .pair = 3 };
	double amplitude = dbm0_amplitude(-28);
	double coherence[2];
	unsigned frame, side;

	dtmf_edge_set(&cell, 3);
	udsp_dtmf_detector_init(&detector, false);
	for (frame = 0; frame < 10; frame++) {
		int16_t samples[UDSP_FRAME_SAMPLES] = { 0 };
		unsigned i;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			unsigned n = frame * UDSP_FRAME_SAMPLES + i;

			if (n < 440)
				continue;
			samples[i] = sample_clamp(dtmf_pair_offsets_sample_get(
				0, 3, n - 440, amplitude, amplitude, cell.offsets));
		}
		CHECK_UINT(detector.active, 0);
		CHECK(!udsp_dtmf_detector_process(&detector, samples, &event));
	}
	CHECK_UINT(detector.candidate, 'A');
	CHECK_UINT(detector.candidate_decisions, 7);
	CHECK_UINT(detector.active, 'A');
	CHECK_UINT(detector.active_decisions, 7);
	for (side = 0; side < 2; side++) {
		const struct udsp_dtmf_offset_run *run = &detector.offset_run[side];

		CHECK_UINT(run->decisions, 7);
		coherence[side] = cabs(run->half) / run->half_magnitude;
		CHECK(isfinite(coherence[side]));
	}
	printf("# drift bar: A +band edges, -28 dBm0, lead 40, decision 7, "
	       "row coherence %.9f, column coherence %.9f\n",
	       coherence[0], coherence[1]);
}

static void dtmf_offset_hold_test(void)
{
	double maximum = 0;
	unsigned i;

	CASE("dtmf accumulated band edge estimate stays within 5 Hz after validation");
	dtmf_offset_validation_test();
	for (i = 0; i < 16u * 2u * 4u; i++) {
		struct dtmf_offset_case cell = {
			.pair = i % 16, .twist = i / 16 % 2 ? 6 : -6,
		};
		double error;

		dtmf_edge_set(&cell, i / 32);
		error = dtmf_offset_hold_error_get(&cell);
		CHECK(error < 5);
		maximum = fmax(maximum, error);
	}
	printf("# offset hold: 128 cells, maximum error %.6f Hz\n", maximum);
}

static void dtmf_cost_test(void)
{
	const struct udsp_tone_desc dial = {
		.tone = UDSP_TONE_DIAL,
		.frequency_count = 2,
		.frequency_hz = { 350, 440 },
		.level_dbm0 = { -13, -13 },
	};
	struct udsp_dtmf_detector detector;
	struct udsp_dtmf_generator generator;
	struct udsp_tone_generator dial_generator;
	struct udsp_event event;
	struct timespec start, end;
	int16_t (*frames)[UDSP_FRAME_SAMPLES] =
		malloc(10000u * UDSP_FRAME_SAMPLES * sizeof(int16_t));
	double microseconds[3];
	unsigned signal, frame;

	CASE("dtmf silence, dialtone and digit cost below 60 us per frame");
	CHECK(frames != NULL);
	if (!frames)
		return;
	udsp_dtmf_generator_init(&generator, '5', -10, UDSP_SAMPLE_RATE);
	CHECK(udsp_tone_generator_init(&dial_generator, &dial, UDSP_SAMPLE_RATE));
	memset(frames, 0, 10000u * sizeof(*frames));
	for (signal = 0; signal < 3; signal++) {
		unsigned events = 0;

		for (frame = 0; frame < 10000; frame++) {
			if (signal == 1)
				udsp_tone_generator_render(&dial_generator,
					frames[frame], UDSP_FRAME_SAMPLES);
			if (signal == 2)
				udsp_dtmf_generator_render(&generator,
					frames[frame], UDSP_FRAME_SAMPLES);
		}
		udsp_dtmf_detector_init(&detector, false);
		CHECK_UINT(clock_gettime(CLOCK_MONOTONIC, &start), 0);
		for (frame = 0; frame < 10000; frame++)
			events += udsp_dtmf_detector_process(&detector,
							 frames[frame], &event);
		CHECK_UINT(clock_gettime(CLOCK_MONOTONIC, &end), 0);
		microseconds[signal] = ((end.tv_sec - start.tv_sec) * 1000000.0 +
			(end.tv_nsec - start.tv_nsec) / 1000.0) / 10000;
		CHECK(microseconds[signal] < 60);
		CHECK_UINT(events, 0);
		CHECK_UINT(detector.active, signal == 2 ? '5' : 0);
	}
	printf("# DTMF cost silence %.3f us/frame, dialtone %.3f us/frame, "
	       "digit %.3f us/frame\n",
	       microseconds[0], microseconds[1], microseconds[2]);
	free(frames);
}

static void dtmf_offset_reset_test(void)
{
	struct udsp_dtmf_detector detector;
	struct udsp_dtmf_generator generator;
	struct udsp_event event;
	int16_t samples[UDSP_FRAME_SAMPLES];
	unsigned count;

	CASE("dtmf offset sums restart with a different candidate");
	udsp_dtmf_detector_init(&detector, false);
	detector.candidate = '5';
	detector.candidate_decisions = 4;
	detector.offset_run[0].decisions = 4;
	detector.offset_run[1].decisions = 4;
	udsp_dtmf_generator_init(&generator, '8', -10, UDSP_SAMPLE_RATE);
	udsp_dtmf_generator_render(&generator, detector.sample_window,
				   UDSP_DTMF_WINDOW_SAMPLES);
	detector.sample_count = UDSP_DTMF_WINDOW_SAMPLES;
	udsp_dtmf_generator_render(&generator, samples, UDSP_FRAME_SAMPLES);
	CHECK(!udsp_dtmf_detector_process(&detector, samples, &event));
	CHECK_UINT(detector.candidate, '8');
	CHECK_UINT(detector.candidate_decisions, 2);
	CHECK_UINT(detector.offset_run[0].decisions, 2);
	CHECK_UINT(detector.offset_run[1].decisions, 2);

	CASE("dtmf offset sums accumulate with the same candidate");
	udsp_dtmf_generator_render(&generator, samples, UDSP_FRAME_SAMPLES);
	CHECK(!udsp_dtmf_detector_process(&detector, samples, &event));
	CHECK_UINT(detector.candidate_decisions, 4);
	CHECK_UINT(detector.offset_run[0].decisions, 4);
	CHECK_UINT(detector.offset_run[1].decisions, 4);

	CASE("dtmf offset sums survive active misses until the digit ends");
	detector.active = '8';
	detector.active_decisions = 7;
	count = detector.offset_run[0].decisions;
	memset(detector.sample_window, 0, sizeof(detector.sample_window));
	memset(samples, 0, sizeof(samples));
	CHECK(!udsp_dtmf_detector_process(&detector, samples, &event));
	CHECK_UINT(detector.active, '8');
	CHECK_UINT(detector.candidate, 0);
	CHECK_UINT(detector.offset_run[0].decisions, count);
	CHECK_UINT(detector.offset_run[1].decisions, count);
	CHECK(udsp_dtmf_detector_process(&detector, samples, &event));
	CHECK_UINT(detector.active, 0);
	CHECK_UINT(detector.offset_run[0].decisions, 0);
	CHECK_UINT(detector.offset_run[1].decisions, 0);

	CASE("dtmf inactive flush clears all offset sums and counts");
	detector.offset_run[0] = (struct udsp_dtmf_offset_run) {
		.quarter = 1 + 2 * I, .half = 3 + 4 * I, .decisions = 7,
		.half_magnitude = 9,
		.adjacent = 21, .centroid_hops_x2 = 2149,
	};
	detector.offset_run[1] = detector.offset_run[0];
	CHECK(!udsp_dtmf_detector_flush(&detector, &event));
	for (count = 0; count < 2; count++) {
		CHECK(detector.offset_run[count].quarter == 0);
		CHECK(detector.offset_run[count].half == 0);
		CHECK(detector.offset_run[count].half_magnitude == 0);
		CHECK_UINT(detector.offset_run[count].decisions, 0);
		CHECK_UINT(detector.offset_run[count].adjacent, 0);
		CHECK_UINT(detector.offset_run[count].centroid_hops_x2, 0);
	}
}

static unsigned dtmf_cadence_events_get(const char *digits, unsigned on_ms,
		unsigned off_ms, unsigned lead_samples, double level_dbm0,
		double twist_db, const double edge[2], char *seen,
		unsigned seen_max)
{
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	double row_amplitude = dbm0_amplitude(twist_db > 0.0 ?
		level_dbm0 - twist_db : level_dbm0);
	double column_amplitude = dbm0_amplitude(twist_db > 0.0 ?
		level_dbm0 : level_dbm0 + twist_db);
	unsigned on_samples = on_ms * UDSP_SAMPLE_RATE / 1000u;
	unsigned period = (on_ms + off_ms) * UDSP_SAMPLE_RATE / 1000u;
	unsigned start = 5u * UDSP_FRAME_SAMPLES + lead_samples;
	unsigned length = (unsigned)strlen(digits) * period;
	unsigned end = start + length + 6u * UDSP_FRAME_SAMPLES;
	unsigned events = 0;
	unsigned fed;

	udsp_dtmf_detector_init(&detector, false);
	for (fed = 0; fed < end; fed += UDSP_FRAME_SAMPLES) {
		int16_t samples[UDSP_FRAME_SAMPLES] = { 0 };
		unsigned i;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			unsigned n = fed + i - start;
			const char *match;
			double offsets[2];
			unsigned pair;

			if (fed + i < start || n >= length ||
			    n % period >= on_samples)
				continue;
			match = memchr(dtmf_digit_map, digits[n / period],
				       sizeof(dtmf_digit_map));
			if (!match)
				continue;
			pair = (unsigned)(match - (const char *)dtmf_digit_map);
			offsets[0] = edge[0] *
				dtmf_edge_hz(dtmf_row_hz[pair / 4u]);
			offsets[1] = edge[1] *
				dtmf_edge_hz(dtmf_column_hz[pair % 4u]);
			samples[i] = sample_clamp(dtmf_pair_offsets_sample_get(
				pair / 4u, pair % 4u, n % period,
				row_amplitude, column_amplitude, offsets));
		}
		if (!udsp_dtmf_detector_process(&detector, samples, &event))
			continue;
		/* A pause longer than the cease keeps each end inside its
		 * source interval. */
		CHECK(fed >= start && fed - start < length &&
		      event.u.digit.digit == digits[(fed - start) / period]);
		if (events < seen_max)
			seen[events] = event.u.digit.digit;
		events++;
	}
	if (udsp_dtmf_detector_flush(&detector, &event)) {
		CHECK(length &&
		      event.u.digit.digit == digits[length / period - 1u]);
		if (events < seen_max)
			seen[events] = event.u.digit.digit;
		events++;
	}
	return events;
}

static void dtmf_cadence_test(void)
{
	static const char digits[] = "1234567890";
	static const char repeated[] = "3342001145";
	static const double nominal[2] = { 0.0, 0.0 };
	static const double levels[] = { -3.0, -13.0, -23.0 };
	static const double twists[] = { 0.0, 3.0, -6.0 };
	static const unsigned leads[] = { 0, 11, 23, 34, 45, 57, 68, 79 };
	unsigned lead;
	unsigned cell;

	CASE("dtmf cadence 40 ms on 60 ms off detects every digit");
	for (lead = 0; lead < UDSP_FRAME_SAMPLES; lead++) {
		char seen[10] = { 0 };
		unsigned events = dtmf_cadence_events_get(digits, 40, 60, lead,
			-10.0, 0.0, nominal, seen, sizeof(seen));

		CHECK_UINT(events, 10);
		CHECK(!memcmp(seen, digits, sizeof(seen)));
	}

	CASE("dtmf cadence 20 ms on 60 ms off detects nothing");
	for (lead = 0; lead < UDSP_FRAME_SAMPLES; lead++)
		CHECK_UINT(dtmf_cadence_events_get(digits, 20, 60, lead,
			-10.0, 0.0, nominal, NULL, 0), 0);

	CASE("dtmf cadence 50 ms on 60 ms off across levels and twists");
	for (cell = 0; cell < 3u * 3u * 8u; cell++) {
		char seen[10] = { 0 };
		unsigned events = dtmf_cadence_events_get(digits, 50, 60,
			leads[cell % 8u], levels[cell / 24u],
			twists[cell / 8u % 3u], nominal, seen, sizeof(seen));

		CHECK_UINT(events, 10);
		CHECK(!memcmp(seen, digits, sizeof(seen)));
	}

	CASE("dtmf cadence 70 ms on 70 ms off detects every repeated digit");
	for (lead = 0; lead < UDSP_FRAME_SAMPLES; lead++) {
		char seen[10] = { 0 };
		unsigned events = dtmf_cadence_events_get(repeated, 70, 70,
			lead, -10.0, 0.0, nominal, seen, sizeof(seen));

		CHECK_UINT(events, 10);
		CHECK(!memcmp(seen, repeated, sizeof(seen)));
	}

	/* The last digit ceases about 2.4 ms before the 41 ms pause ends, so
	 * the source interval check above still holds. */
	CASE("dtmf cadence 41 ms on 41 ms off splits every repeated digit");
	for (cell = 0; cell < 3u * 3u * 8u; cell++) {
		char seen[10] = { 0 };
		unsigned events = dtmf_cadence_events_get(repeated, 41, 41,
			leads[cell % 8u], levels[cell / 24u],
			twists[cell / 8u % 3u], nominal, seen, sizeof(seen));

		CHECK_UINT(events, 10);
		CHECK(!memcmp(seen, repeated, sizeof(seen)));
	}

	/* 60 ms tones, because a 41 ms tone at an accept edge does not
	 * validate at every alignment. */
	CASE("dtmf cadence 60 ms on 41 ms off splits repeats at band edges");
	for (cell = 0; cell < 4u * UDSP_FRAME_SAMPLES; cell++) {
		const double edge[2] = {
			cell & 1u ? 1.0 : -1.0, cell & 2u ? 1.0 : -1.0,
		};
		char seen[10] = { 0 };
		unsigned events = dtmf_cadence_events_get(repeated, 60, 41,
			cell / 4u, -10.0, 0.0, edge, seen, sizeof(seen));

		CHECK_UINT(events, 10);
		CHECK(!memcmp(seen, repeated, sizeof(seen)));
	}

	CASE("dtmf cadence 30 ms on 60 ms off reported");
	for (lead = 0; lead < UDSP_FRAME_SAMPLES; lead++) {
		unsigned events = dtmf_cadence_events_get(digits, 30, 60, lead,
			-10.0, 0.0, nominal, NULL, 0);

		printf("# dtmf cadence 30/60 lead %u: %u events\n",
		       lead, events);
	}
}

static unsigned dtmf_break_events_get(unsigned cell, unsigned break_ms,
				      double twist_db, char *digit,
				      uint16_t *duration)
{
	struct dtmf_offset_case tone = {
		.pair = cell % 16u, .lead = cell / 80u,
	};
	unsigned edge = cell / 16u % 5u;

	if (edge)
		dtmf_edge_set(&tone, edge - 1u);
	return dtmf_burst_offsets_events_get(tone.pair / 4u, tone.pair % 4u,
		50u + break_ms + 70u, -10.0, twist_db, tone.lead, break_ms,
		digit, duration, NULL, tone.offsets);
}

#define DTMF_BREAK_CELLS	(16u * 5u * UDSP_FRAME_SAMPLES)

/*
 * One key press at -10 dBm0 with a break inside it, after the first 50 ms.
 * Each sweep runs the 16 digits, at nominal frequency and at the four
 * combinations of accept edges, on every lead.
 *
 * A digit that ends inside the break reports a short duration: less than the
 * first part and the break together. A break below 20 ms holds too few
 * decisions to clear the guard, so the part after it reports nothing of its
 * own. At an accept edge the first 50 ms can fail to validate. The part after
 * the break then validates alone, and also reports one event.
 */
static void dtmf_break_test(void)
{
	static const unsigned short_ms[2] = { 15, 19 };
	static const double twists[3] = { -6.0, 0.0, 6.0 };
	static const unsigned reported_ms[2] = { 25, 30 };
	unsigned cell, i, k;

	CASE("dtmf an 18 ms break never ends the digit at the band edges");
	for (cell = 0; cell < DTMF_BREAK_CELLS; cell++) {
		uint16_t duration = 0;
		char digit = 0;

		CHECK_UINT(dtmf_break_events_get(cell, 18, 0.0, &digit,
						 &duration), 1);
		CHECK(duration >= 50u + 18u + 70u);
	}

	CASE("dtmf a break below 20 ms never reports two digits");
	for (i = 0; i < 2u; i++) {
		for (k = 0; k < 3u; k++) {
			unsigned ended = 0;

			for (cell = 0; cell < DTMF_BREAK_CELLS; cell++) {
				uint16_t duration = 0;
				char digit = 0;

				CHECK_UINT(dtmf_break_events_get(cell,
					short_ms[i], twists[k], &digit,
					&duration), 1);
				ended += duration < 50u + short_ms[i] + 70u;
			}
			printf("# dtmf %u ms break, twist %+.0f dB: %u of %u "
			       "cells end the digit inside the break\n",
			       short_ms[i], twists[k], ended, DTMF_BREAK_CELLS);
		}
	}

	CASE("dtmf a 22 ms break without twist reports once");
	for (cell = 0; cell < DTMF_BREAK_CELLS; cell++) {
		uint16_t duration = 0;
		char digit = 0;

		CHECK_UINT(dtmf_break_events_get(cell, 22, 0.0, &digit,
						 &duration), 1);
		CHECK_UINT(digit, dtmf_digit_map[cell % 16u / 4u][cell % 4u]);
	}

	CASE("dtmf 25 and 30 ms breaks without twist reported");
	for (i = 0; i < 2u; i++) {
		unsigned split = 0;

		for (cell = 0; cell < DTMF_BREAK_CELLS; cell++) {
			uint16_t duration = 0;
			char digit = 0;

			split += dtmf_break_events_get(cell, reported_ms[i],
						       0.0, &digit,
						       &duration) > 1;
		}
		printf("# dtmf %u ms break: %u of %u cells report two digits\n",
		       reported_ms[i], split, DTMF_BREAK_CELLS);
	}
}

/*
 * The bar the hardening rests on is the minimum validated duration, so the
 * acceptance sweep runs every digit against the level range, both twist
 * directions and four sample alignments of the tone against the frame grid.
 * The alignment matters: the deciding window is 205 samples and it moves in
 * steps of 40, so where the tone starts decides how many windows it fills.
 */
static const double dtmf_accept_level[3] = { -3.0, -13.0, -23.0 };
static const double dtmf_accept_twist[4] = { 0.0, 3.0, 6.0, -6.0 };
static const unsigned dtmf_accept_on_ms[5] = { 35, 40, 50, 60, 100 };
static const unsigned dtmf_accept_lead[4] = { 0, 19, 37, 61 };

#define DTMF_ACCEPT_CASES	(4u * 4u * 3u * 4u * 5u * 4u)

static bool dtmf_accept_case_ok(unsigned index)
{
	unsigned row = index % 4u;
	unsigned column = index / 4u % 4u;
	unsigned l = index / 16u % 3u;
	unsigned t = index / 48u % 4u;
	unsigned d = index / 192u % 5u;
	unsigned p = index / 960u % 4u;
	unsigned on_ms = dtmf_accept_on_ms[d];
	uint16_t duration = 0;
	char digit = 0;
	unsigned events;

	events = dtmf_burst_events_get(row, column, on_ms, dtmf_accept_level[l],
				       dtmf_accept_twist[t], dtmf_accept_lead[p],
				       0, &digit, &duration, NULL);
	if (events != 1 || digit != dtmf_digit_map[row][column])
		return false;
	/* duration_get() counts 5 ms per decision, including four misses,
	 * so a burst reports 20 to 30 ms beyond the tone. A pair at the
	 * 6 dB twist boundary can lose its first onset decision to the
	 * twist bound and report one decision less. */
	return duration >= on_ms + 15u && duration <= on_ms + 30u;
}

static unsigned dtmf_short_burst_events_get(unsigned on_ms)
{
	unsigned events = 0;
	unsigned index;

	for (index = 0; index < 4u * 4u * UDSP_FRAME_SAMPLES; index++) {
		uint16_t duration = 0;
		char digit = 0;
		unsigned count;

		count = dtmf_burst_events_get(index % 4u, index / 4u % 4u,
			on_ms, -10.0, 0.0, index / 16u, 0,
			&digit, &duration, NULL);
		if (on_ms == 35) {
			CHECK_UINT(count, 1);
			CHECK_UINT(digit, dtmf_digit_map[index % 4u]
						      [index / 4u % 4u]);
		}
		events += count;
	}
	return events;
}

static void dtmf_acceptance_test(void)
{
	unsigned bad = 0;
	unsigned index;

	CASE("dtmf acceptance every digit at 35 ms and longer");
	for (index = 0; index < DTMF_ACCEPT_CASES; index++)
		bad += dtmf_accept_case_ok(index) ? 0u : 1u;
	CHECK_UINT(bad, 0);

	CASE("dtmf 25 ms bursts rejected");
	CHECK_UINT(dtmf_short_burst_events_get(25), 0);
	CASE("dtmf warm 35 ms bursts accepted at every alignment");
	CHECK_UINT(dtmf_short_burst_events_get(35),
		   16u * UDSP_FRAME_SAMPLES);
}

static void dtmf_fast_cease_test(void)
{
	unsigned index;
	unsigned gap;

	for (gap = 0; gap < 2; gap++) {
		CASE(gap ? "dtmf -28 dBm0 -6 dB twist bridges a 19 ms gap" :
		     "dtmf -28 dBm0 -6 dB twist ceases within 40 ms "
		     "of silence");
		for (index = 0; index < 16u * UDSP_FRAME_SAMPLES; index++) {
			unsigned row = index % 4u;
			unsigned column = index / 4u % 4u;
			unsigned lead = index / 16u;
			unsigned tone_end =
				150u * UDSP_SAMPLE_RATE / 1000u + lead;
			unsigned end_samples = 0;
			uint16_t duration = 0;
			char digit = 0;

			CHECK_UINT(dtmf_burst_events_get(row, column, 100,
				-28.0, -6.0, lead, gap ? 19 : 0, &digit,
				&duration, &end_samples), 1);
			CHECK_UINT(digit, dtmf_digit_map[row][column]);
			CHECK(end_samples >= tone_end);
			CHECK(end_samples - tone_end <=
			      40u * UDSP_SAMPLE_RATE / 1000u);
		}
	}
}

static void dtmf_sequence_test(void)
{
	static const char digits[3] = { '5', '3', '5' };
	struct udsp_dtmf_generator generator;
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	int16_t samples[UDSP_FRAME_SAMPLES];
	char seen[3] = { 0 };
	unsigned events = 0;
	unsigned frame;

	/* 80 ms on and 40 ms off, the shortest pair a keypad string uses. */
	udsp_dtmf_detector_init(&detector, false);
	udsp_dtmf_generator_init(&generator, digits[0], -10, UDSP_SAMPLE_RATE);
	for (frame = 0; frame < 36; frame++) {
		if (frame % 12u == 0)
			udsp_dtmf_generator_init(&generator, digits[frame / 12u],
						 -10, UDSP_SAMPLE_RATE);
		if (frame % 12u < 8u)
			udsp_dtmf_generator_render(&generator, samples,
						   UDSP_FRAME_SAMPLES);
		else
			memset(samples, 0, sizeof(samples));
		if (!udsp_dtmf_detector_process(&detector, samples, &event))
			continue;
		if (events < 3)
			seen[events] = event.u.digit.digit;
		events++;
	}
	if (udsp_dtmf_detector_flush(&detector, &event)) {
		if (events < 3)
			seen[events] = event.u.digit.digit;
		events++;
	}
	CASE("dtmf back to back digits all reported");
	CHECK_UINT(events, 3);
	CHECK_UINT(seen[0], '5');
	CHECK_UINT(seen[1], '3');
	CHECK_UINT(seen[2], '5');
}

/* Over one second at 8 kHz, every whole-hertz tone falls on a bin. The power
 * of a sine of peak A is then (A * N / 2)^2, and the other tone adds nothing. */
static double dtmf_tone_level_get(const int16_t *samples, unsigned count,
				  double frequency)
{
	struct udsp_goertzel_bank bank;
	double amplitude;

	udsp_goertzel_init(&bank, &frequency, 1);
	udsp_goertzel_process(&bank, samples, count);
	amplitude = 2.0 * sqrt(udsp_goertzel_power(&bank, 0)) / count;
	return 20.0 * log10(amplitude / dbm0_amplitude(0.0));
}

static void dtmf_generator_level_test(void)
{
	static const unsigned pair[2][2] = { { 1, 1 }, { 3, 3 } };
	const unsigned count = UDSP_SAMPLE_RATE;
	static int16_t samples[UDSP_SAMPLE_RATE];
	unsigned i, at;

	CASE("dtmf generator renders the row at its level and the column 2 dB "
	     "higher");
	for (i = 0; i < 2; i++) {
		struct udsp_dtmf_generator generator;
		unsigned row = pair[i][0];
		unsigned column = pair[i][1];
		char digit = dtmf_digit_map[row][column];
		double row_level, column_level;

		udsp_dtmf_generator_init(&generator, digit, -10,
					 UDSP_SAMPLE_RATE);
		for (at = 0; at < count; at += UDSP_FRAME_SAMPLES) {
			bool rendered = udsp_dtmf_generator_render(&generator,
				samples + at, UDSP_FRAME_SAMPLES);

			CHECK(rendered);
		}
		row_level = dtmf_tone_level_get(samples, count,
						dtmf_row_hz[row]);
		column_level = dtmf_tone_level_get(samples, count,
						   dtmf_column_hz[column]);
		printf("# dtmf generator %c at -10 dBm0: row %.3f dBm0, "
		       "column %.3f dBm0\n", digit, row_level, column_level);
		CHECK(fabs(row_level + 10.0) <= 0.1);
		CHECK(fabs(column_level + 8.0) <= 0.1);
	}
}

/* Five frames warm the window, so the onset falls on a running decision grid,
 * and the run is long enough for a 100 ms digit to end. */
#define DTMF_LEVEL_LEAD		(5u * UDSP_FRAME_SAMPLES)
#define DTMF_LEVEL_FRAMES	40u
#define DTMF_LEVEL_SAMPLES	(DTMF_LEVEL_FRAMES * UDSP_FRAME_SAMPLES)

static const int dtmf_level_dbm0[2] = { -10, -20 };

/* The range of the level errors of one sweep, in dB. */
struct dtmf_level_sweep {
	int low;
	int high;
	unsigned bad;
};

static void dtmf_level_sweep_add(struct dtmf_level_sweep *sweep, bool ok,
				 int got, int want)
{
	sweep->bad += !ok;
	if (!ok)
		return;
	if (got - want < sweep->low)
		sweep->low = got - want;
	if (got - want > sweep->high)
		sweep->high = got - want;
}

static int dtmf_level_sweep_worst(const struct dtmf_level_sweep *sweep)
{
	return -sweep->low > sweep->high ? -sweep->low : sweep->high;
}

/*
 * The level the detector reports for the one digit of the signal, read on the
 * frame that validates it. The read must stay until the digit ends, and read 0
 * once it ended. The digit must end exactly once.
 */
static bool dtmf_level_read(const int16_t *signal, char expected, int *level)
{
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	unsigned frame, events = 0;
	bool validated = false;
	bool stable = true;

	udsp_dtmf_detector_init(&detector, false);
	for (frame = 0; frame < DTMF_LEVEL_FRAMES; frame++) {
		const int16_t *samples = signal + frame * UDSP_FRAME_SAMPLES;

		if (udsp_dtmf_detector_process(&detector, samples, &event)) {
			events++;
			stable &= event.u.digit.digit == expected &&
				udsp_dtmf_detector_active_level(&detector) == 0;
		}
		if (!detector.active)
			continue;
		if (!validated)
			*level = udsp_dtmf_detector_active_level(&detector);
		validated = true;
		stable &= udsp_dtmf_detector_active_level(&detector) == *level;
	}
	if (udsp_dtmf_detector_flush(&detector, &event)) {
		events++;
		stable &= event.u.digit.digit == expected;
	}
	return validated && stable && events == 1;
}

static void dtmf_level_generator_fill(int16_t *signal, char digit,
				      int level_dbm0, unsigned lead,
				      unsigned on_ms)
{
	struct udsp_dtmf_generator generator;

	memset(signal, 0, DTMF_LEVEL_SAMPLES * sizeof(*signal));
	udsp_dtmf_generator_init(&generator, digit, level_dbm0,
				 UDSP_SAMPLE_RATE);
	udsp_dtmf_generator_render(&generator, signal + lead,
				   on_ms * UDSP_SAMPLE_RATE / 1000u);
}

/* cell->level is the row level here, and the column plays cell->twist dB
 * higher, so the twist names the column against the row whatever its sign. */
static void dtmf_level_pair_fill(int16_t *signal,
				 const struct dtmf_offset_case *cell)
{
	double row = dbm0_amplitude(cell->level);
	double column = dbm0_amplitude(cell->level + cell->twist);
	unsigned on = cell->on_ms * UDSP_SAMPLE_RATE / 1000u;
	unsigned i;

	memset(signal, 0, DTMF_LEVEL_SAMPLES * sizeof(*signal));
	for (i = 0; i < on; i++)
		signal[cell->lead + i] = sample_clamp(
			dtmf_pair_offsets_sample_get(cell->pair / 4,
				cell->pair % 4, i, row, column, cell->offsets));
}

static void dtmf_level_print(const char *name,
			     const struct dtmf_level_sweep *sweep,
			     unsigned total)
{
	printf("# dtmf level %s: worst error %d dB (from %+d to %+d), "
	       "%u of %u digits read\n", name, dtmf_level_sweep_worst(sweep),
	       sweep->low, sweep->high, total - sweep->bad, total);
}

/*
 * The detector reports the level of the row tone of a validated digit, the
 * level the relay sends as the RFC 4733 volume. The generator plays the row
 * at its level and the column 2 dB higher. The pair builders set the twist
 * and the frequency offsets explicitly.
 */
static void dtmf_level_test(void)
{
	static const unsigned on_ms[3] = { 35, 40, 60 };
	static const double twists[3] = { 0.0, 2.0, 6.0 };
	static int16_t signal[DTMF_LEVEL_SAMPLES];
	struct dtmf_level_sweep sweep = { 0 };
	struct dtmf_level_sweep align = { 0 };
	unsigned i, d;
	int got = 0;

	CASE("dtmf detector reports the row level of the generator");
	for (i = 0; i < 16u * 2u; i++) {
		char digit = dtmf_digit_map[i % 16 / 4][i % 4];
		int want = dtmf_level_dbm0[i / 16];
		bool ok;

		dtmf_level_generator_fill(signal, digit, want,
					  DTMF_LEVEL_LEAD, 100);
		ok = dtmf_level_read(signal, digit, &got);
		dtmf_level_sweep_add(&sweep, ok, got, want);
	}
	dtmf_level_print("nominal", &sweep, 32);
	CHECK_UINT(sweep.bad, 0);
	CHECK(dtmf_level_sweep_worst(&sweep) <= 1);

	CASE("dtmf detector reports the row level at the accepted offset "
	     "edges");
	memset(&sweep, 0, sizeof(sweep));
	for (i = 0; i < 16u * 4u * 2u; i++) {
		struct dtmf_offset_case cell = {
			.pair = i % 16, .on_ms = 100, .lead = DTMF_LEVEL_LEAD,
			.level = dtmf_level_dbm0[i / 64], .twist = 2.0,
		};
		char digit = dtmf_digit_map[cell.pair / 4][cell.pair % 4];
		bool ok;

		dtmf_edge_set(&cell, i / 16 % 4);
		dtmf_level_pair_fill(signal, &cell);
		ok = dtmf_level_read(signal, digit, &got);
		dtmf_level_sweep_add(&sweep, ok, got, (int)cell.level);
	}
	dtmf_level_print("offset edges", &sweep, 128);
	CHECK_UINT(sweep.bad, 0);
	CHECK(dtmf_level_sweep_worst(&sweep) <= 1);

	CASE("dtmf detector reports the row level of short digits at every "
	     "onset alignment");
	for (d = 0; d < 3u; d++) {
		char name[32];

		memset(&sweep, 0, sizeof(sweep));
		for (i = 0; i < 16u * 10u * 2u; i++) {
			char digit = dtmf_digit_map[i % 16 / 4][i % 4];
			unsigned offset = i / 16 % 10 * 4u;
			int want = dtmf_level_dbm0[i / 160];
			bool ok;

			dtmf_level_generator_fill(signal, digit, want,
						  DTMF_LEVEL_LEAD + offset,
						  on_ms[d]);
			ok = dtmf_level_read(signal, digit, &got);
			dtmf_level_sweep_add(&sweep, ok, got, want);
			dtmf_level_sweep_add(&align, ok, got, want);
		}
		snprintf(name, sizeof(name), "%u ms", on_ms[d]);
		dtmf_level_print(name, &sweep, 320);
		CHECK_UINT(sweep.bad, 0);
		CHECK(dtmf_level_sweep_worst(&sweep) <= 1);
	}
	dtmf_level_print("onset alignments 0 to 36", &align, 960);

	CASE("dtmf detector reports the row level whatever the twist");
	for (d = 0; d < 3u; d++) {
		char name[32];

		memset(&sweep, 0, sizeof(sweep));
		for (i = 0; i < 16u * 2u; i++) {
			struct dtmf_offset_case cell = {
				.pair = i % 16, .on_ms = 100,
				.lead = DTMF_LEVEL_LEAD,
				.level = dtmf_level_dbm0[i / 16],
				.twist = twists[d],
			};
			char digit = dtmf_digit_map[cell.pair / 4]
						   [cell.pair % 4];
			bool ok;

			dtmf_level_pair_fill(signal, &cell);
			ok = dtmf_level_read(signal, digit, &got);
			dtmf_level_sweep_add(&sweep, ok, got, (int)cell.level);
		}
		snprintf(name, sizeof(name), "twist %+.0f dB", twists[d]);
		dtmf_level_print(name, &sweep, 32);
		CHECK_UINT(sweep.bad, 0);
		CHECK(dtmf_level_sweep_worst(&sweep) <= 1);
	}
}

static bool dtmf_tone_set_detected(const double *frequency, const double *level,
				   unsigned count, const double *ramp,
				   unsigned frames)
{
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	int16_t samples[UDSP_FRAME_SAMPLES];
	bool detected = false;
	unsigned frame, i, k;

	udsp_dtmf_detector_init(&detector, false);
	for (frame = 0; frame < frames; frame++) {
		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			unsigned n = frame * UDSP_FRAME_SAMPLES + i;
			double value = 0.0;

			for (k = 0; k < count; k++)
				value += dbm0_amplitude(level[k] +
							(ramp ? ramp[k] * frame : 0.0)) *
					sin(2.0 * M_PI * frequency[k] * n /
					    UDSP_SAMPLE_RATE);
			samples[i] = sample_clamp(value);
		}
		detected |= udsp_dtmf_detector_process(&detector, samples, &event);
	}
	detected |= udsp_dtmf_detector_flush(&detector, &event);
	return detected;
}

/*
 * One named case per acceptance bar of the detector. Each signal holds a valid
 * pair for 80 ms, so the duration bar cannot be what refuses it. Each signal
 * breaks exactly one of the other bars. If that bar alone is weakened, the
 * case reports a digit.
 */
static void dtmf_bar_case_test(void)
{
	static const double harmonic_comb[4] = { 941.0, 1633.0, 1882.0, 3266.0 };
	static const double harmonic_level[4] = { -10.0, -10.0, -18.2, -18.2 };
	static const double group_intruder[3] = { 697.0, 852.0, 1209.0 };
	static const double group_level[3] = { -10.0, -17.0, -10.0 };
	static const double band_intruder[3] = { 770.0, 1000.0, 1336.0 };
	static const double band_level[3] = { -10.0, -6.1, -10.0 };
	static const double ramp_pair[2] = { 770.0, 1336.0 };
	static const double ramp_level[2] = { -10.0, -17.5 };
	static const double ramp_step[2] = { 0.0, 16.6 };

	/* The nominal comb needs almost no relief. Its 8.2 dB attenuation
	 * leaves 1.008188 dB of margin against the 0.12 bar. The measured minimum
	 * normalized ratio, 0.136381489, leaves 0.555742 dB of margin. */
	CASE("dtmf talk-off second harmonic comb rejected");
	CHECK(!dtmf_tone_set_detected(harmonic_comb, harmonic_level, 4, NULL, 8));

	/* A second row tone 7 dB below the first: two harmonics of one comb
	 * inside one group, which no key press produces. */
	CASE("dtmf talk-off in group intruder rejected");
	CHECK(!dtmf_tone_set_detected(group_intruder, group_level, 3, NULL, 8));

	/* A 1000 Hz intruder that leaves the pair with 45 percent of the
	 * window energy: off-frequency energy no key press carries. */
	CASE("dtmf talk-off off frequency intruder rejected");
	CHECK(!dtmf_tone_set_detected(band_intruder, band_level, 3, NULL, 8));

	/* The 16.6 dB per-frame ramp crosses the absolute twist bounds,
	 * which limit the candidate duration before validation. */
	CASE("dtmf talk-off twist ramp rejected");
	CHECK(!dtmf_tone_set_detected(ramp_pair, ramp_level, 2, ramp_step, 8));
}

/*
 * A valid digit D for 100 ms that turns into the harmonic comb of
 * dtmf_bar_case_test for 500 ms. The comb fills every window, so the harmonic
 * bar ends the digit. The uplink mute and the relay then do not carry the comb
 * as part of the digit.
 */
static void dtmf_comb_after_digit_test(void)
{
	static const double frequency[4] = { 941.0, 1633.0, 1882.0, 3266.0 };
	static const double level[4] = { -10.0, -10.0, -18.2, -18.2 };
	struct udsp_dtmf_detector detector;
	struct udsp_event event;
	uint16_t duration = 0;
	unsigned events = 0;
	char digit = 0;
	unsigned frame;

	CASE("dtmf a digit that turns into a harmonic comb ends");
	udsp_dtmf_detector_init(&detector, false);
	for (frame = 0; frame < 75u; frame++) {
		int16_t samples[UDSP_FRAME_SAMPLES] = { 0 };
		unsigned tones = frame < 5u || frame >= 65u ? 0u :
			frame < 15u ? 2u : 4u;
		unsigned i, k;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			unsigned n = frame * UDSP_FRAME_SAMPLES + i;
			double value = 0.0;

			for (k = 0; k < tones; k++)
				value += dbm0_amplitude(level[k]) *
					sin(2.0 * M_PI * frequency[k] * n /
					    UDSP_SAMPLE_RATE);
			samples[i] = sample_clamp(value);
		}
		if (!udsp_dtmf_detector_process(&detector, samples, &event))
			continue;
		events++;
		digit = event.u.digit.digit;
		duration = event.u.digit.duration_ms;
	}
	if (udsp_dtmf_detector_flush(&detector, &event)) {
		events++;
		duration = event.u.digit.duration_ms;
	}
	CHECK_UINT(events, 1);
	CHECK_UINT(digit, 'D');
	CHECK(duration <= 200u);
}

static void vad_test(void)
{
	struct udsp_vad vad;
	int16_t samples[UDSP_FRAME_SAMPLES] = { 0 };
	unsigned i;

	udsp_vad_init(&vad);
	CASE("vad silence inactive");
	CHECK(!udsp_vad_process(&vad, samples));

	udsp_vad_init(&vad);
	sine_fill(samples, UDSP_FRAME_SAMPLES, 440.0, 8000.0, 0);
	CASE("vad tone active");
	CHECK(udsp_vad_process(&vad, samples));

	udsp_vad_init(&vad);
	for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
		double envelope = (i < 40 ? i : 80 - i) / 40.0;

		samples[i] = (int16_t)lrint(envelope *
			(7000.0 * sin(2.0 * M_PI * 180.0 * i / UDSP_SAMPLE_RATE) +
			 5000.0 * sin(2.0 * M_PI * 720.0 * i / UDSP_SAMPLE_RATE)));
	}
	CASE("vad speech envelope active");
	CHECK(udsp_vad_process(&vad, samples));
}

static void white_noise_fill(int16_t *samples, int level_dbm0,
			     uint32_t *state)
{
	double rms = 16139.0 * pow(10.0, level_dbm0 / 20.0);
	double peak = rms * sqrt(3.0);
	unsigned i;

	for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
		double unit = test_random_next(state) / 4294967295.0 * 2.0 - 1.0;

		samples[i] = (int16_t)lrint(unit * peak);
	}
}

static void harmonic_speech_fill(int16_t *samples, unsigned frame,
				 int level_dbm0)
{
	double rms = 16139.0 * pow(10.0, level_dbm0 / 20.0);
	double amplitude = rms / sqrt(2.0);
	unsigned i;

	for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
		size_t n = frame * UDSP_FRAME_SAMPLES + i;
		double value = 0.0;
		unsigned harmonic;

		for (harmonic = 1; harmonic <= 4; harmonic++)
			value += amplitude * sin(2.0 * M_PI * 200.0 * harmonic * n /
					       UDSP_SAMPLE_RATE);
		samples[i] = (int16_t)lrint(value);
	}
}

static void vad_acceptance_test(void)
{
	struct udsp_vad vad;
	int16_t samples[UDSP_FRAME_SAMPLES];
	uint32_t state;
	double high_energy = 0.0;
	unsigned voice_frames;
	unsigned frame;

	state = 0x12345678;
	udsp_vad_init(&vad);
	voice_frames = 0;
	for (frame = 0; frame < 50; frame++) {
		white_noise_fill(samples, -10, &state);
		voice_frames += udsp_vad_process(&vad, samples);
	}
	CASE("vad white noise minus 10 dbm0 rejected");
	CHECK_UINT(voice_frames, 0);

	state = 0x87654321;
	udsp_vad_init(&vad);
	voice_frames = 0;
	for (frame = 0; frame < 50; frame++) {
		white_noise_fill(samples, -20, &state);
		voice_frames += udsp_vad_process(&vad, samples);
	}
	CASE("vad white noise minus 20 dbm0 rejected");
	CHECK_UINT(voice_frames, 0);

	udsp_vad_init(&vad);
	voice_frames = 0;
	for (frame = 0; frame < 50; frame++) {
		harmonic_speech_fill(samples, frame, -26);
		voice_frames += udsp_vad_process(&vad, samples);
	}
	CASE("vad harmonic speech minus 26 dbm0 accepted");
	CHECK_UINT(voice_frames, 50);

	state = 0x31415926;
	udsp_vad_init(&vad);
	for (frame = 0; frame < 20; frame++) {
		white_noise_fill(samples, -30, &state);
		udsp_vad_process(&vad, samples);
	}
	for (frame = 0; frame < 200; frame++) {
		white_noise_fill(samples, -20, &state);
		high_energy += frame_energy_get(samples, UDSP_FRAME_SAMPLES);
		udsp_vad_process(&vad, samples);
	}
	high_energy /= 200.0;
	CASE("vad learns ten db noise step within two seconds");
	CHECK(vad.noise_energy > high_energy * 0.95);
}

static void vad_cost_test(void)
{
	struct udsp_vad vad;
	struct timespec start;
	struct timespec end;
	int16_t samples[UDSP_FRAME_SAMPLES];
	double microseconds;
	unsigned voice_frames = 0;
	unsigned frame;

	udsp_vad_init(&vad);
	harmonic_speech_fill(samples, 0, -10);
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (frame = 0; frame < 10000; frame++)
		voice_frames += udsp_vad_process(&vad, samples);
	clock_gettime(CLOCK_MONOTONIC, &end);
	microseconds = (end.tv_sec - start.tv_sec) * 1000000.0 +
		(end.tv_nsec - start.tv_nsec) / 1000.0;
	microseconds /= 10000.0;
	printf("# VAD cost %.3f us/frame\n", microseconds);
	CASE("vad recursive spectrum cost below direct dft");
	CHECK_UINT(voice_frames, 10000);
	CHECK(microseconds < 15.0);
}

static void tone_test(void)
{
	struct udsp_tone_desc dial = {
		.tone = UDSP_TONE_DIAL,
		.frequency_count = 2,
		.frequency_hz = { 350, 440 },
		.level_dbm0 = { -13, -13 },
	};
	struct udsp_tone_desc busy = {
		.tone = UDSP_TONE_BUSY,
		.frequency_count = 2,
		.frequency_hz = { 480, 620 },
		.level_dbm0 = { -15, -15 },
		.cadence_count = 1,
		.cadence = { { 500, 500 } },
		.repeat = true,
	};
	struct udsp_tone_generator generator;
	struct udsp_goertzel_bank bank;
	const double frequencies[] = { 350.0, 440.0, 1000.0 };
	const double busy_frequencies[] = { 480.0, 620.0, 1000.0 };
	int16_t samples[800];
	int16_t frame[UDSP_FRAME_SAMPLES];
	unsigned i;

	CASE("tone dial 350 plus 440 read-back");
	CHECK(udsp_tone_generator_init(&generator, &dial, UDSP_SAMPLE_RATE));
	udsp_tone_generator_render(&generator, samples, 800);
	udsp_goertzel_init(&bank, frequencies, 3);
	udsp_goertzel_process(&bank, samples, 800);
	CHECK(udsp_goertzel_power(&bank, 0) > udsp_goertzel_power(&bank, 2) * 50.0);
	CHECK(udsp_goertzel_power(&bank, 1) > udsp_goertzel_power(&bank, 2) * 50.0);

	CASE("tone busy 480 plus 620 read-back");
	CHECK(udsp_tone_generator_init(&generator, &busy, UDSP_SAMPLE_RATE));
	udsp_tone_generator_render(&generator, samples, 800);
	udsp_goertzel_init(&bank, busy_frequencies, 3);
	udsp_goertzel_process(&bank, samples, 800);
	CHECK(udsp_goertzel_power(&bank, 0) >
	      udsp_goertzel_power(&bank, 2) * 50.0);
	CHECK(udsp_goertzel_power(&bank, 1) >
	      udsp_goertzel_power(&bank, 2) * 50.0);

	CASE("tone busy 500 ms cadence");
	CHECK(udsp_tone_generator_init(&generator, &busy, UDSP_SAMPLE_RATE));
	for (i = 0; i < 110; i++) {
		udsp_tone_generator_render(&generator, frame, UDSP_FRAME_SAMPLES);
		if (i < 50 || i >= 100)
			CHECK(frame_energy_get(frame, UDSP_FRAME_SAMPLES) > 100000.0);
		else
			CHECK(frame_energy_get(frame, UDSP_FRAME_SAMPLES) == 0.0);
	}

	CASE("tone all enum values accepted");
	for (i = UDSP_TONE_NONE; i <= UDSP_TONE_NUMBER_UNOBTAINABLE; i++) {
		struct udsp_tone_desc desc = dial;

		desc.tone = i;
		CHECK(udsp_tone_generator_init(&generator, &desc, UDSP_SAMPLE_RATE));
		udsp_tone_generator_render(&generator, frame, UDSP_FRAME_SAMPLES);
	}

	CASE("tone frequency below voice band rejected");
	dial.frequency_hz[0] = 99;
	CHECK(!udsp_tone_generator_init(&generator, &dial, UDSP_SAMPLE_RATE));

	CASE("tone frequency above voice band rejected");
	dial.frequency_hz[0] = 3401;
	CHECK(!udsp_tone_generator_init(&generator, &dial, UDSP_SAMPLE_RATE));
}

static void cn_gain_test(void)
{
	struct udsp_cn cn;
	int16_t noise[8000];
	int16_t reference[80];
	int16_t gain_samples[] = { 1000, -1000, 30000, -30000 };
	int level;
	unsigned i;

	udsp_cn_init(&cn, 0x12345678);
	udsp_cn_generate(&cn, 30, noise, sizeof(noise) / sizeof(noise[0]));
	level = udsp_cn_level_get(noise, sizeof(noise) / sizeof(noise[0]));
	CASE("cn level round trip");
	CHECK(abs(level - 30) <= 1);

	for (i = 0; i < sizeof(reference) / sizeof(reference[0]); i++)
		reference[i] = i & 1 ? 32767 : -32767;
	CASE("cn zero dbov reference");
	CHECK_UINT(udsp_cn_level_get(reference,
				     sizeof(reference) / sizeof(reference[0])), 0);

	udsp_gain_apply(gain_samples,
			sizeof(gain_samples) / sizeof(gain_samples[0]), 12);
	CASE("gain half-db steps and saturation");
	CHECK(abs(gain_samples[0] - 1995) <= 1);
	CHECK(abs(gain_samples[1] + 1995) <= 1);
	CHECK_UINT((uint16_t)gain_samples[2], 32767);
	CHECK_UINT((uint16_t)gain_samples[3], (uint16_t)-32768);
}

static void codec_cn_test(void)
{
	const struct udsp_codec_ops *reset_ops[] = {
		&udsp_codec_pcmu_ops,
		&udsp_codec_g726_32_ops,
	};
	const int16_t sentinel = INT16_MIN;
	struct udsp_codec *codec;
	int16_t samples[160];
	int16_t first[UDSP_FRAME_SAMPLES];
	int16_t replay[UDSP_FRAME_SAMPLES];
	unsigned i;

	CASE("codec cn honours 40 sample count");
	codec = codec_create(&udsp_codec_pcmu_ops);
	CHECK(codec != NULL);
	if (!codec)
		return;
	for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++)
		samples[i] = sentinel;
	udsp_codec_cn(codec, 30, samples, 40);
	for (i = 0; i < 40; i++)
		CHECK(samples[i] != sentinel);
	for (; i < sizeof(samples) / sizeof(samples[0]); i++)
		CHECK(samples[i] == sentinel);
	udsp_codec_pcmu_ops.destroy(codec);

	CASE("codec reset restores comfort noise seed");
	for (i = 0; i < sizeof(reset_ops) / sizeof(reset_ops[0]); i++) {
		codec = codec_create(reset_ops[i]);
		CHECK(codec != NULL);
		if (!codec)
			continue;
		reset_ops[i]->cn(codec, 30, first, UDSP_FRAME_SAMPLES);
		reset_ops[i]->reset(codec);
		reset_ops[i]->cn(codec, 30, replay, UDSP_FRAME_SAMPLES);
		CHECK(memcmp(first, replay, sizeof(first)) == 0);
		reset_ops[i]->destroy(codec);
	}
}

int main(void)
{
	CASE("codec enum mappings pinned");
	CHECK_UINT(UDSP_CODEC_ENUMS_PINNED, 1);
	g711_test();
	codec_v2_test();
	codec_v4_test();
	codec_v2_error_test();
	g711_plc_test();
	g726_test();
	g726_vector_test();
	g726_decode_reference_test();
#ifdef HAVE_SPANDSP
	g726_spandsp_packing_test();
#endif
	goertzel_test();
	dtmf_generator_level_test();
	dtmf_level_test();
	dtmf_round_trip_test();
	dtmf_remote_test();
	dtmf_tolerance_test();
	dtmf_gap_flush_test();
	dtmf_guard_test();
	dtmf_talkoff_test();
	dtmf_bar_case_test();
	dtmf_comb_after_digit_test();
	dtmf_acceptance_test();
	dtmf_fast_cease_test();
	dtmf_cadence_test();
	dtmf_break_test();
	dtmf_sequence_test();
	dtmf_band_edge_test();
	dtmf_edge_duration_test();
	dtmf_offset_reject_test();
	dtmf_offset_level_test();
	dtmf_offset_drift_test();
	dtmf_offset_hold_test();
	dtmf_cost_test();
	dtmf_offset_reset_test();
	dtmf_talkoff_audio_test();
	vad_test();
	vad_acceptance_test();
	vad_cost_test();
	tone_test();
	cn_gain_test();
	codec_cn_test();
	return tu_summary("dsp");
}
