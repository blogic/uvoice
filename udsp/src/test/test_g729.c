// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp.h"
#include "g729.h"
#include "util.h"
#include "vectors/g729_vectors.h"

#define TEST_FRAME 80U
#define TEST_BYTES 10U
#define TEST_FRAMES 100U

extern const struct udsp_codec_ops udsp_codec_g729_ops;

static struct udsp_codec *codec_create(void)
{
	struct udsp_codec_params params = { 0 };

	return udsp_codec_g729_ops.create(&params);
}

static struct udsp_codec *dtx_codec_create(void)
{
	struct udsp_codec_params params = { .dtx = true };

	return udsp_codec_g729_ops.create(&params);
}

static void speech_fill(int16_t *pcm, uint32_t *seed, int32_t *shaped)
{
	unsigned i;

	for (i = 0; i < TEST_FRAME; i++) {
		int32_t random;

		*seed = *seed * 1664525U + 1013904223U;
		random = (int32_t)(*seed >> 16) - 32768;
		*shaped = (*shaped * 7 + random) / 8;
		pcm[i] = (int16_t)(*shaped / 2);
	}
}

static void tone_fill(int16_t *pcm, unsigned offset, double frequency)
{
	unsigned i;

	for (i = 0; i < TEST_FRAME; i++) {
		double time = (offset + i) / 8000.0;

		pcm[i] = (int16_t)lrint(12000.0 *
			sin(2.0 * M_PI * frequency * time));
	}
}

static uint64_t energy_get(const int16_t *pcm, unsigned count)
{
	uint64_t energy = 0;
	unsigned i;

	for (i = 0; i < count; i++)
		energy += (int64_t)pcm[i] * pcm[i];
	return energy;
}

static void metadata_check(void)
{
	struct udsp_codec *codec = codec_create();

	CASE("ops metadata");
	CHECK(codec != NULL);
	CHECK_UINT(udsp_codec_g729_ops.id, UDSP_CODEC_G729);
	CHECK(strcmp(udsp_codec_g729_ops.name, "g729") == 0);
	CHECK_UINT(udsp_codec_g729_ops.sample_rate, 8000);
	CHECK_UINT(udsp_codec_g729_ops.clock_rate, 8000);
	CHECK_UINT(udsp_codec_g729_ops.frame_ms, 10);
	CHECK_UINT(udsp_codec_g729_ops.ptime_mask, 0x07);
	CHECK_UINT(udsp_codec_g729_ops.max_payload, 30);
	CHECK_UINT(udsp_codec_g729_ops.flags, UDSP_CODEC_F_PLC);
	CHECK_UINT(udsp_codec_g729_ops.rate_get(codec), 8000);
	CHECK(udsp_codec_g729_ops.cn != NULL);
	udsp_codec_g729_ops.destroy(codec);
	udsp_codec_g729_ops.destroy(NULL);
}

static void errors_check(void)
{
	struct udsp_codec *codec = codec_create();
	struct udsp_codec *clean = codec_create();
	int16_t pcm[TEST_FRAME * 4] = { 0 };
	int16_t actual_pcm[TEST_FRAME];
	int16_t expected_pcm[TEST_FRAME];
	uint8_t payload[TEST_BYTES * 4] = { 0 };
	uint8_t actual[TEST_BYTES];
	uint8_t expected[TEST_BYTES];

	CASE("encode error paths");
	CHECK_UINT(udsp_codec_g729_ops.encode(codec, pcm, 79, payload,
		TEST_BYTES), -EINVAL);
	CHECK_UINT(udsp_codec_g729_ops.encode(codec, pcm, 81, payload,
		TEST_BYTES), -EINVAL);
	CHECK_UINT(udsp_codec_g729_ops.encode(codec, pcm, TEST_FRAME, payload,
		TEST_BYTES - 1), -ENOBUFS);

	CASE("decode error paths");
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, payload, 1, pcm,
		TEST_FRAME), -EINVAL);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, payload, 9, pcm,
		TEST_FRAME), -EINVAL);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, payload, 11, pcm,
		TEST_FRAME), -EINVAL);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, payload, TEST_BYTES, pcm,
		TEST_FRAME - 1), -ENOBUFS);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, payload, TEST_BYTES * 2, pcm,
		TEST_FRAME * 2 - 1), -ENOBUFS);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, payload, TEST_BYTES * 4, pcm,
		TEST_FRAME * 4), -EINVAL);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, payload, 0, pcm, 0),
		-EAGAIN);

	CASE("PLC error paths");
	CHECK_UINT(udsp_codec_g729_ops.plc(codec, pcm, 79, NULL, 0), -EINVAL);
	CHECK_UINT(udsp_codec_g729_ops.plc(codec, pcm, 81, payload,
		TEST_BYTES), -EINVAL);

	CASE("rejected calls preserve codec state");
	speech_fill(pcm, &(uint32_t){ 7 }, &(int32_t){ 0 });
	CHECK_UINT(udsp_codec_g729_ops.encode(codec, pcm, TEST_FRAME, actual,
		sizeof(actual)), TEST_BYTES);
	CHECK_UINT(udsp_codec_g729_ops.encode(clean, pcm, TEST_FRAME, expected,
		sizeof(expected)), TEST_BYTES);
	CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, payload, 9, actual_pcm,
		TEST_FRAME), -EINVAL);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, g729_speech_expected,
		TEST_BYTES, actual_pcm, TEST_FRAME), TEST_FRAME);
	CHECK_UINT(udsp_codec_g729_ops.decode(clean, g729_speech_expected,
		TEST_BYTES, expected_pcm, TEST_FRAME), TEST_FRAME);
	CHECK(memcmp(actual_pcm, expected_pcm, sizeof(actual_pcm)) == 0);
	udsp_codec_g729_ops.destroy(clean);
	udsp_codec_g729_ops.destroy(codec);
}

static void vectors_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t pcm[TEST_FRAME];
	int16_t decoded[TEST_FRAME];
	uint8_t payload[TEST_BYTES];
	uint32_t seed = 7;
	int32_t shaped = 0;

	CASE("frozen encoder speech frame");
	speech_fill(pcm, &seed, &shaped);
	CHECK_UINT(udsp_codec_g729_ops.encode(codec, pcm, TEST_FRAME, payload,
		sizeof(payload)), TEST_BYTES);
	CHECK(memcmp(payload, g729_speech_expected, sizeof(payload)) == 0);
	udsp_codec_g729_ops.reset(codec);

	CASE("frozen decoder speech frame");
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, g729_speech_expected,
		TEST_BYTES, decoded, TEST_FRAME), TEST_FRAME);
	CHECK(memcmp(decoded, g729_decoder_expected, sizeof(decoded)) == 0);
	udsp_codec_g729_ops.destroy(codec);
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

static void round_trip_check(void)
{
	struct udsp_codec *encoder = codec_create();
	struct udsp_codec *decoder = codec_create();
	static int16_t input[TEST_FRAME * TEST_FRAMES];
	static int16_t output[TEST_FRAME * TEST_FRAMES];
	uint8_t payload[TEST_BYTES];
	double best_snr = -200.0;
	unsigned frame;
	unsigned delay;

	for (frame = 0; frame < TEST_FRAMES; frame++) {
		tone_fill(input + frame * TEST_FRAME, frame * TEST_FRAME, 440.0);
		CHECK_UINT(udsp_codec_g729_ops.encode(encoder,
			input + frame * TEST_FRAME, TEST_FRAME, payload,
			sizeof(payload)), TEST_BYTES);
		CHECK_UINT(udsp_codec_g729_ops.decode(decoder, payload, TEST_BYTES,
			output + frame * TEST_FRAME, TEST_FRAME), TEST_FRAME);
	}
	for (delay = 0; delay <= 120; delay++) {
		double snr = snr_get(input + 240,
			output + 240 + delay,
			TEST_FRAME * TEST_FRAMES - 240 - delay);

		if (snr > best_snr)
			best_snr = snr;
	}
	CASE("round trip 440 hz aligned SNR");
	printf("# round trip aligned SNR %.2f dB\n", best_snr);
	CHECK(best_snr > 10.0);
	udsp_codec_g729_ops.destroy(decoder);
	udsp_codec_g729_ops.destroy(encoder);
}

static void packet_decode_case(unsigned frame_count, const char *name)
{
	struct udsp_codec *packet_codec = codec_create();
	struct udsp_codec *single_codec = codec_create();
	struct udsp_codec *encoder = codec_create();
	int16_t packet_pcm[TEST_FRAME * 3];
	int16_t single_pcm[TEST_FRAME * 3];
	int16_t input[TEST_FRAME];
	uint8_t payload[TEST_BYTES * 3];
	uint32_t seed = 11;
	int32_t shaped = 0;
	unsigned frame;

	for (frame = 0; frame < frame_count; frame++) {
		speech_fill(input, &seed, &shaped);
		CHECK_UINT(udsp_codec_g729_ops.encode(encoder, input, TEST_FRAME,
			payload + frame * TEST_BYTES, TEST_BYTES), TEST_BYTES);
	}
	memset(packet_pcm, 0x5a, sizeof(packet_pcm));
	memset(single_pcm, 0xa5, sizeof(single_pcm));
	CASE(name);
	CHECK_UINT(udsp_codec_g729_ops.decode(packet_codec, payload,
		frame_count * TEST_BYTES, packet_pcm, frame_count * TEST_FRAME),
		frame_count * TEST_FRAME);
	for (frame = 0; frame < frame_count; frame++) {
		CHECK_UINT(udsp_codec_g729_ops.decode(single_codec,
			payload + frame * TEST_BYTES, TEST_BYTES,
			single_pcm + frame * TEST_FRAME, TEST_FRAME), TEST_FRAME);
		CHECK(memcmp(packet_pcm + frame * TEST_FRAME,
			single_pcm + frame * TEST_FRAME,
			TEST_FRAME * sizeof(packet_pcm[0])) == 0);
	}
	udsp_codec_g729_ops.destroy(encoder);
	udsp_codec_g729_ops.destroy(single_codec);
	udsp_codec_g729_ops.destroy(packet_codec);
}

static void packet_decode_check(void)
{
	packet_decode_case(2, "decode packet matches two single frames");
	packet_decode_case(3, "decode packet matches three single frames");
}

static void reset_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t pcm[TEST_FRAME];
	int16_t first_pcm[TEST_FRAME];
	int16_t second_pcm[TEST_FRAME];
	uint8_t first[TEST_BYTES];
	uint8_t second[TEST_BYTES];
	uint32_t seed = 7;
	int32_t shaped = 0;

	speech_fill(pcm, &seed, &shaped);
	udsp_codec_g729_ops.encode(codec, pcm, TEST_FRAME, first, sizeof(first));
	udsp_codec_g729_ops.reset(codec);
	udsp_codec_g729_ops.encode(codec, pcm, TEST_FRAME, second, sizeof(second));
	CASE("reset restores encoder state");
	CHECK(memcmp(first, second, sizeof(first)) == 0);
	udsp_codec_g729_ops.decode(codec, first, TEST_BYTES, first_pcm, TEST_FRAME);
	udsp_codec_g729_ops.reset(codec);
	udsp_codec_g729_ops.decode(codec, first, TEST_BYTES, second_pcm, TEST_FRAME);
	CASE("reset restores decoder state");
	CHECK(memcmp(first_pcm, second_pcm, sizeof(first_pcm)) == 0);
	udsp_codec_g729_ops.destroy(codec);
}

static void plc_check(void)
{
	struct udsp_codec *encoder = codec_create();
	struct udsp_codec *decoder = codec_create();
	int16_t pcm[TEST_FRAME];
	uint8_t payload[TEST_BYTES];
	uint64_t energy[3];
	unsigned frame;

	for (frame = 0; frame < 12; frame++) {
		tone_fill(pcm, frame * TEST_FRAME, 200.0);
		udsp_codec_g729_ops.encode(encoder, pcm, TEST_FRAME, payload,
			sizeof(payload));
		udsp_codec_g729_ops.decode(decoder, payload, TEST_BYTES, pcm,
			TEST_FRAME);
	}
	CASE("native PLC conceals erased frames");
	for (frame = 0; frame < 3; frame++) {
		CHECK_UINT(udsp_codec_g729_ops.plc(decoder, pcm, TEST_FRAME,
			NULL, 0), TEST_FRAME);
		energy[frame] = energy_get(pcm, TEST_FRAME);
	}
	CHECK(energy[0] > 0);
	CHECK(energy[2] < energy[0]);
	udsp_codec_g729_ops.destroy(decoder);
	udsp_codec_g729_ops.destroy(encoder);
}

static void cn_check(void)
{
	struct udsp_codec *first = codec_create();
	struct udsp_codec *second = codec_create();
	int16_t initial[TEST_FRAME];
	int16_t repeated[TEST_FRAME];
	int16_t continued[TEST_FRAME];

	udsp_codec_g729_ops.cn(first, 20, initial, TEST_FRAME);
	udsp_codec_g729_ops.cn(second, 20, repeated, TEST_FRAME);
	CASE("g729 comfort noise uses codec seed");
	CHECK(memcmp(initial, repeated, sizeof(initial)) == 0);
	udsp_codec_g729_ops.reset(first);
	udsp_codec_g729_ops.cn(first, 20, continued, TEST_FRAME);
	CHECK(memcmp(initial, continued, sizeof(initial)) != 0);
	udsp_codec_g729_ops.destroy(second);
	udsp_codec_g729_ops.destroy(first);
}

static void vad_tone_fill(int16_t *pcm, unsigned offset, double frequency,
			  double amplitude)
{
	unsigned i;

	for (i = 0; i < TEST_FRAME; i++) {
		double time = (offset + i) / 8000.0;

		pcm[i] = (int16_t)lrint(amplitude *
			sin(2.0 * M_PI * frequency * time));
	}
}

static void vad_noise_fill(int16_t *pcm, uint32_t *seed, int32_t amplitude)
{
	unsigned i;

	for (i = 0; i < TEST_FRAME; i++) {
		*seed = *seed * 1664525U + 1013904223U;
		pcm[i] = (int16_t)((int32_t)(*seed >> 20) % (2 * amplitude + 1)
			- amplitude);
	}
}

static bool vad_frame_run(struct udsp_codec *codec, const int16_t *pcm)
{
	uint8_t payload[TEST_BYTES];

	CHECK_UINT(udsp_codec_g729_ops.encode(codec, pcm, TEST_FRAME, payload,
		sizeof(payload)), TEST_BYTES);
	return g729_vad_decision_get(codec);
}

static void vad_speech_frame_fill(int16_t *pcm, unsigned frame)
{
	vad_tone_fill(pcm, frame * TEST_FRAME, 250.0 + 150.0 * (frame % 5),
		6000.0 + 1500.0 * (frame % 4));
}

static void vad_silence_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t pcm[TEST_FRAME] = { 0 };
	unsigned voiced = 0;
	unsigned frame;

	CASE("vad zero input never decides voice");
	for (frame = 0; frame < 64; frame++) {
		if (vad_frame_run(codec, pcm))
			voiced++;
	}
	CHECK_UINT(voiced, 0);
	udsp_codec_g729_ops.destroy(codec);
}

static void vad_noise_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t pcm[TEST_FRAME];
	uint32_t seed = 7;
	unsigned early_voiced = 0;
	unsigned late_voiced = 0;
	unsigned frame;

	/* Stationary noise reads as voice until the clause B.3.3 long-term
	 * minimum aligns the noise energy average at frame 129; from there
	 * the decision settles on non-voice. */
	CASE("vad stationary noise converges to non-voice");
	for (frame = 0; frame < 200; frame++) {
		vad_noise_fill(pcm, &seed, 500);
		if (!vad_frame_run(codec, pcm))
			continue;
		if (frame < 32)
			early_voiced++;
		else if (frame >= 160)
			late_voiced++;
	}
	CHECK_UINT(early_voiced, 32);
	CHECK_UINT(late_voiced, 0);
	udsp_codec_g729_ops.destroy(codec);
}

static void vad_speech_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t pcm[TEST_FRAME];
	unsigned voiced = 0;
	unsigned frame;

	CASE("vad modulated tones hold voice");
	for (frame = 0; frame < 100; frame++) {
		vad_speech_frame_fill(pcm, frame);
		if (vad_frame_run(codec, pcm))
			voiced++;
	}
	CHECK_UINT(voiced, 100);
	udsp_codec_g729_ops.destroy(codec);
}

static void vad_hangover_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t pcm[TEST_FRAME];
	double amplitude = 10000.0;
	unsigned frame;

	CASE("vad smoothing holds voice across a short gap");
	for (frame = 0; frame < 40; frame++) {
		vad_tone_fill(pcm, frame * TEST_FRAME, 440.0, 10000.0);
		CHECK(vad_frame_run(codec, pcm));
	}
	/* The gap decays 2.5 dB per frame, inside the clause B.3.6 stage-two
	 * window of 3 dB between frames. The initial decision holds voice for
	 * the first four gap frames, the stage-two smoothing extends it over
	 * the next five, and the tenth frame exhausts the budget. */
	for (frame = 0; frame < 9; frame++) {
		amplitude *= 0.75;
		vad_tone_fill(pcm, (40 + frame) * TEST_FRAME, 440.0, amplitude);
		CHECK(vad_frame_run(codec, pcm));
	}
	amplitude *= 0.75;
	vad_tone_fill(pcm, 49 * TEST_FRAME, 440.0, amplitude);
	CHECK(!vad_frame_run(codec, pcm));
	for (frame = 0; frame < 6; frame++) {
		vad_tone_fill(pcm, (50 + frame) * TEST_FRAME, 440.0, 10000.0);
		CHECK(vad_frame_run(codec, pcm));
	}
	udsp_codec_g729_ops.destroy(codec);
}

/*
 * A 300-frame integer signal with a per-frame golden decision, so a test
 * covers every constant in the clause B.3 arithmetic: initialization (frames
 * 0..31), noise adaptation across the frame-129 minimum sync (32..179), a
 * modulated square-wave talkspurt (180..219), a decaying gap (220..231),
 * noise again (232..279) and a near-silent tail (280..299). The expected
 * decisions equal those of the ITU-T Annex B C code for these frames.
 */
static void vad_golden_frame_fill(int16_t *pcm, unsigned frame,
				  uint32_t *seed)
{
	unsigned i;

	for (i = 0; i < TEST_FRAME; i++) {
		unsigned t = frame * TEST_FRAME + i;
		int32_t half = frame >= 280 ? 40 : 500;
		int32_t sample;

		*seed = *seed * 1664525U + 1013904223U;
		sample = (int32_t)(*seed >> 20) % (2 * half + 1) - half;
		if (frame >= 180 && frame < 220) {
			unsigned period = 8 + 2 * (frame % 12);
			int32_t amplitude = 8000 + 500 * (int32_t)(frame % 7);

			sample += (t / period) & 1 ? amplitude : -amplitude;
		} else if (frame >= 220 && frame < 232) {
			int32_t amplitude = 8000;
			unsigned decay;

			for (decay = 220; decay < frame; decay++)
				amplitude = amplitude * 3 / 4;
			sample += (t / 16) & 1 ? amplitude : -amplitude;
		}
		pcm[i] = (int16_t)sample;
	}
}

static const uint8_t vad_golden_expected[38] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x0f, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00,
};

static void vad_golden_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t pcm[TEST_FRAME];
	uint32_t seed = 3;
	unsigned mismatches = 0;
	unsigned frame;

	CASE("vad golden decision sequence");
	for (frame = 0; frame < 300; frame++) {
		bool expected = (vad_golden_expected[frame / 8] >>
			(7 - frame % 8)) & 1;

		vad_golden_frame_fill(pcm, frame, &seed);
		if (vad_frame_run(codec, pcm) != expected)
			mismatches++;
	}
	CHECK_UINT(mismatches, 0);
	udsp_codec_g729_ops.destroy(codec);
}

/*
 * Two adversarial signals that test the clause B.3.5 arithmetic at its
 * margins, which the 300-frame golden signal and the ITU sequences do not
 * reach. The staircase walks a mildly low-passed noise probe from 700 to
 * 1400 in amplitude over an adapted 500-amplitude background, which sweeps
 * the full-band difference densely through the decision edges of
 * boundaries 5, 8 and 9. The threshold run holds near-silence through the
 * initialization phase and then dithers around the absolute 15 dB energy
 * threshold, the one comparison a one-step energy bias cannot escape by
 * adaptation. Both expected sequences equal the decisions of the ITU-T
 * Annex B C code.
 */
static void vad_stair_frame_fill(int16_t *pcm, unsigned frame,
				 uint32_t *seed, int32_t *previous)
{
	unsigned i;

	for (i = 0; i < TEST_FRAME; i++) {
		int32_t amplitude = frame < 200 ? 500 :
			700 + (int32_t)(frame - 200);
		int32_t raw_sample;
		int32_t sample;

		*seed = *seed * 1664525U + 1013904223U;
		raw_sample = (int32_t)(*seed >> 20) % (2 * amplitude + 1) -
			amplitude;
		sample = raw_sample;
		if (frame >= 200 && (frame & 2))
			sample = (*previous * 3 + raw_sample) / 4;
		*previous = raw_sample;
		pcm[i] = (int16_t)sample;
	}
}

static const uint8_t vad_stair_expected[113] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x80,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x0c, 0x07, 0xff, 0xc0, 0x44,
	0x04, 0x7f, 0xff, 0xff, 0xff, 0xe0, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
	0x07, 0xfe, 0x40, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x40, 0x00, 0x00,
};

static const uint8_t vad_threshold_expected[54] = {
	0x00, 0x00, 0x00, 0x00, 0x7f, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static void vad_margin_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t pcm[TEST_FRAME];
	uint32_t seed = 9;
	int32_t previous = 0;
	unsigned mismatches = 0;
	unsigned frame;
	unsigned i;

	CASE("vad staircase golden sequence");
	for (frame = 0; frame < 900; frame++) {
		bool expected = (vad_stair_expected[frame / 8] >>
			(7 - frame % 8)) & 1;

		vad_stair_frame_fill(pcm, frame, &seed, &previous);
		if (vad_frame_run(codec, pcm) != expected)
			mismatches++;
	}
	CHECK_UINT(mismatches, 0);
	udsp_codec_g729_ops.destroy(codec);

	codec = codec_create();
	seed = 3;
	mismatches = 0;
	CASE("vad threshold golden sequence");
	for (frame = 0; frame < 432; frame++) {
		bool expected = (vad_threshold_expected[frame / 8] >>
			(7 - frame % 8)) & 1;
		int32_t amplitude = frame < 32 ? 8 : 33;

		for (i = 0; i < TEST_FRAME; i++) {
			seed = seed * 1664525U + 1013904223U;
			pcm[i] = (int16_t)((int32_t)(seed >> 20) %
				(2 * amplitude + 1) - amplitude);
		}
		if (vad_frame_run(codec, pcm) != expected)
			mismatches++;
	}
	CHECK_UINT(mismatches, 0);
	udsp_codec_g729_ops.destroy(codec);
}

static void vad_wrap_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t pcm[TEST_FRAME];
	uint32_t seed = 5;
	unsigned frame;

	CASE("vad frame counter wraps into the steady state");
	for (frame = 0; frame < 200; frame++) {
		vad_noise_fill(pcm, &seed, 500);
		vad_frame_run(codec, pcm);
	}
	CHECK(!g729_vad_decision_get(codec));
	/* The counter must wrap 32767 to 256 and stay past the
	 * initialization phase: a wrap to 0 would re-enter it and force
	 * voice on this adapted noise. */
	g729_vad_frame_count_set(codec, 32760);
	for (frame = 0; frame < 24; frame++) {
		vad_noise_fill(pcm, &seed, 500);
		CHECK(!vad_frame_run(codec, pcm));
	}
	for (frame = 0; frame < 4; frame++) {
		vad_speech_frame_fill(pcm, frame);
		vad_frame_run(codec, pcm);
	}
	CHECK(g729_vad_decision_get(codec));
	udsp_codec_g729_ops.destroy(codec);
}

/* Split a Q31 value into the DPF pair the codec carries: value equals
 * high * 65536 + 2 * low. */
static void probe_correlation_set(int16_t *high, int16_t *low, unsigned lag,
				  double value)
{
	int32_t fixed = (int32_t)lrint(value * 1073741824.0);

	high[lag] = (int16_t)(fixed >> 16);
	low[lag] = (int16_t)((fixed - (int32_t)high[lag] * 65536) >> 1);
}

static void levinson_commit_check(void)
{
	static const double stable_rho[G729_LPC_ORDER + 1] = {
		1.0, 0.9, 0.75, 0.6, 0.45, 0.3, 0.2, 0.12, 0.06, 0.02, 0.0
	};
	/* This set drives the order-2 reflection to -32751, one past the
	 * stability bound, so the recursion takes its early return and must
	 * keep the pair the stable set committed. */
	static const double unstable_rho[G729_LPC_ORDER + 1] = {
		1.0, -0.999, 0.999999, -0.997, 0.993, -0.988, 0.982,
		-0.975, 0.967, -0.958, 0.948
	};
	struct udsp_codec *codec = codec_create();
	int16_t high[G729_LPC_ORDER + 1];
	int16_t low[G729_LPC_ORDER + 1];
	int16_t committed[2];
	int16_t held[2];
	unsigned i;

	CASE("levinson commits reflections only on success");
	for (i = 0; i <= G729_LPC_ORDER; i++)
		probe_correlation_set(high, low, i, stable_rho[i]);
	g729_levinson_probe(codec, high, low, committed);
	CHECK(committed[0] == -29492);
	CHECK(committed[1] == 10347);
	for (i = 0; i <= G729_LPC_ORDER; i++)
		probe_correlation_set(high, low, i, unstable_rho[i]);
	g729_levinson_probe(codec, high, low, held);
	CHECK(held[0] == committed[0]);
	CHECK(held[1] == committed[1]);
	udsp_codec_g729_ops.destroy(codec);
}

static void vad_reset_check(void)
{
	struct udsp_codec *fresh = codec_create();
	struct udsp_codec *reused = codec_create();
	int16_t pcm[TEST_FRAME];
	uint32_t seed = 21;
	unsigned frame;

	CASE("vad reset matches a fresh instance");
	for (frame = 0; frame < 30; frame++) {
		vad_noise_fill(pcm, &seed, 900);
		vad_frame_run(reused, pcm);
	}
	udsp_codec_g729_ops.reset(reused);
	CHECK(!g729_vad_decision_get(reused));
	for (frame = 0; frame < 50; frame++) {
		if (frame & 1)
			vad_speech_frame_fill(pcm, frame);
		else
			vad_noise_fill(pcm, &seed, 300);
		CHECK_UINT(vad_frame_run(reused, pcm),
			vad_frame_run(fresh, pcm));
	}
	udsp_codec_g729_ops.destroy(reused);
	udsp_codec_g729_ops.destroy(fresh);
}

static void dtx_disabled_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t pcm[TEST_FRAME] = { 0 };
	int16_t speech[TEST_FRAME];
	uint8_t payload[TEST_BYTES];
	uint32_t seed = 7;
	int32_t shaped = 0;
	unsigned frame;

	CASE("dtx off keeps every frame a speech frame");
	for (frame = 0; frame < 20; frame++)
		CHECK_UINT(udsp_codec_g729_ops.encode(codec, pcm, TEST_FRAME,
			payload, sizeof(payload)), TEST_BYTES);
	udsp_codec_g729_ops.reset(codec);
	speech_fill(speech, &seed, &shaped);
	CHECK_UINT(udsp_codec_g729_ops.encode(codec, speech, TEST_FRAME,
		payload, sizeof(payload)), TEST_BYTES);
	CHECK(memcmp(payload, g729_speech_expected, TEST_BYTES) == 0);
	udsp_codec_g729_ops.destroy(codec);
}

/* Six silent frames, eight modulated-tone frames, six silent frames: the
 * decision sequence walks SID, untransmitted, speech and back. */
static void dtx_frame_fill(int16_t *pcm, unsigned frame)
{
	if (frame < 6 || frame >= 14)
		memset(pcm, 0, TEST_FRAME * sizeof(*pcm));
	else
		vad_speech_frame_fill(pcm, frame - 6);
}

/*
 * The ITU-T Annex B C code, with the VAD on, gives these frame kinds, SID
 * payloads and resumed speech frame for this PCM. The bytes of the resumed
 * frame cover the random-parameter draw of the comfort noise and the seed
 * reset. The near-silent gain is too small for the arithmetic of the
 * excitation scaler to reach these bytes. dtx_cng_excitation_check and the
 * A+B encoder and decoder vectors check that arithmetic instead.
 */
static void dtx_sequence_check(void)
{
	static const int expected_bytes[20] = {
		2, 0, 0, 0, 0, 0, 10, 10, 10, 10,
		10, 10, 10, 10, 10, 10, 10, 2, 0, 0,
	};
	static const uint8_t first_sid[2] = { 0x34, 0x40 };
	static const uint8_t second_sid[2] = { 0x80, 0x8a };
	static const uint8_t resumed_speech[TEST_BYTES] = {
		0x8b, 0x4b, 0xc5, 0xe0, 0x00, 0xfa, 0xdd, 0xd6, 0x25, 0x2c,
	};
	struct udsp_codec *codec = dtx_codec_create();
	int16_t pcm[TEST_FRAME];
	uint8_t payload[TEST_BYTES];
	unsigned frame;

	CASE("dtx golden sid-untransmitted-speech sequence");
	for (frame = 0; frame < 20; frame++) {
		dtx_frame_fill(pcm, frame);
		memset(payload, 0xa5, sizeof(payload));
		CHECK_UINT(udsp_codec_g729_ops.encode(codec, pcm, TEST_FRAME,
			payload, sizeof(payload)), expected_bytes[frame]);
		if (frame == 0)
			CHECK(memcmp(payload, first_sid,
				sizeof(first_sid)) == 0);
		if (frame == 17)
			CHECK(memcmp(payload, second_sid,
				sizeof(second_sid)) == 0);
		if (frame == 6)
			CHECK(memcmp(payload, resumed_speech,
				TEST_BYTES) == 0);
		/* Octet transmission mode closes the second SID octet with
		 * one zero pad bit. */
		if (expected_bytes[frame] == 2)
			CHECK((payload[1] & 1) == 0);
	}
	udsp_codec_g729_ops.destroy(codec);
}

static void dtx_reset_check(void)
{
	struct udsp_codec *reused = dtx_codec_create();
	struct udsp_codec *fresh = dtx_codec_create();
	int16_t pcm[TEST_FRAME];
	uint8_t reused_payload[TEST_BYTES];
	uint8_t fresh_payload[TEST_BYTES];
	unsigned frame;

	CASE("dtx reset matches a fresh instance");
	for (frame = 0; frame < 20; frame++) {
		dtx_frame_fill(pcm, frame);
		udsp_codec_g729_ops.encode(reused, pcm, TEST_FRAME,
			reused_payload, sizeof(reused_payload));
	}
	udsp_codec_g729_ops.reset(reused);
	for (frame = 0; frame < 20; frame++) {
		int reused_bytes;
		int fresh_bytes;

		dtx_frame_fill(pcm, frame);
		memset(reused_payload, 0x5a, sizeof(reused_payload));
		memset(fresh_payload, 0xa5, sizeof(fresh_payload));
		reused_bytes = udsp_codec_g729_ops.encode(reused, pcm,
			TEST_FRAME, reused_payload, sizeof(reused_payload));
		fresh_bytes = udsp_codec_g729_ops.encode(fresh, pcm,
			TEST_FRAME, fresh_payload, sizeof(fresh_payload));
		CHECK_UINT(reused_bytes, fresh_bytes);
		CHECK(fresh_bytes >= 0);
		if (fresh_bytes > 0)
			CHECK(memcmp(reused_payload, fresh_payload,
				(size_t)fresh_bytes) == 0);
	}
	udsp_codec_g729_ops.destroy(fresh);
	udsp_codec_g729_ops.destroy(reused);
}

/*
 * The clause B.4.1.2 pacing and change test over the staircase signal of
 * vad_margin_check: the noise level walks upward, so the quantized energy
 * keeps crossing the 2 dB change threshold and SID emission is limited by
 * the minimum spacing alone through long stretches. The ITU-T Annex B C
 * code gives the same frame kind for all 900 frames of this PCM: 206 speech,
 * 68 SID and 626 untransmitted.
 */
static const uint16_t dtx_stair_sid_frames[68] = {
	137, 141, 160, 168, 198, 202, 205, 224, 228, 233, 236, 240, 248,
	251, 257, 260, 263, 268, 271, 274, 283, 286, 309, 312, 316, 326,
	329, 332, 346, 354, 358, 366, 403, 412, 453, 456, 459, 462, 471,
	517, 521, 534, 582, 603, 611, 614, 639, 642, 659, 664, 667, 680,
	683, 686, 695, 698, 704, 707, 721, 724, 780, 807, 810, 819, 822,
	831, 834, 882
};

static void dtx_pacing_check(void)
{
	/* Talkspurt resume frames after noise stretches whose SID gains are
	 * table entries 19 to 22. Their bytes check the gain values that feed
	 * the comfort-noise excitation, which the near-silent 20-frame
	 * sequence does not reach. They do not check the arithmetic of the
	 * excitation scaler: at these gains a one-bit rounding difference
	 * fades before the resume. dtx_cng_excitation_check and the A+B
	 * vectors check that arithmetic. The ITU-T Annex B C code gives the
	 * same bytes. */
	static const struct {
		uint16_t frame;
		uint8_t bytes[TEST_BYTES];
	} resumes[5] = {
		{ 273, { 0xf8, 0x62, 0xfa, 0xf1, 0xfa, 0x2d, 0x94, 0xcd,
			 0x84, 0x4c } },
		{ 324, { 0x60, 0xb8, 0xd3, 0x56, 0xa9, 0xfc, 0xda, 0x8a,
			 0xdc, 0xb0 } },
		{ 353, { 0x78, 0xa8, 0xca, 0x66, 0xbf, 0x36, 0x4a, 0x27,
			 0xa9, 0xb2 } },
		{ 777, { 0xf0, 0x13, 0x7a, 0xa3, 0x8a, 0x75, 0xca, 0x4a,
			 0x6e, 0x84 } },
		{ 797, { 0x78, 0xa2, 0xfc, 0x37, 0x56, 0xfd, 0x97, 0x92,
			 0x07, 0x60 } },
	};
	struct udsp_codec *codec = dtx_codec_create();
	int16_t pcm[TEST_FRAME];
	uint8_t payload[TEST_BYTES];
	uint32_t seed = 9;
	int32_t previous = 0;
	unsigned minimum_gap = UINT_MAX;
	unsigned sid_seen = 0;
	unsigned mismatches = 0;
	unsigned frame;
	unsigned i;

	CASE("dtx stair golden frame kinds and SID pacing");
	for (i = 1; i < 68; i++) {
		unsigned gap = (unsigned)(dtx_stair_sid_frames[i] -
			dtx_stair_sid_frames[i - 1]);

		if (gap < minimum_gap)
			minimum_gap = gap;
	}
	CHECK_UINT(minimum_gap, 3);
	for (frame = 0; frame < 900; frame++) {
		bool voice = (vad_stair_expected[frame / 8] >>
			(7 - frame % 8)) & 1;
		int expected = voice ? (int)TEST_BYTES : 0;
		int result;

		if (!voice && sid_seen < 68 &&
		    frame == dtx_stair_sid_frames[sid_seen]) {
			expected = 2;
			sid_seen++;
		}
		vad_stair_frame_fill(pcm, frame, &seed, &previous);
		result = udsp_codec_g729_ops.encode(codec, pcm, TEST_FRAME,
			payload, sizeof(payload));
		if (result != expected)
			mismatches++;
		for (i = 0; i < 5; i++) {
			if (frame == resumes[i].frame)
				CHECK(memcmp(payload, resumes[i].bytes,
					TEST_BYTES) == 0);
		}
	}
	CHECK_UINT(sid_seen, 68);
	CHECK_UINT(mismatches, 0);
	udsp_codec_g729_ops.destroy(codec);
}

/*
 * The run that covers the excitation scaler. Loud noise, with an amplitude
 * that steps from 2500 to 3700, drives the comfort-noise gain high. One-bit
 * rounding in the scaling and gain-solve arithmetic of clause B.4.4 then
 * reaches the bytes of the following talkspurt frames. Each of these faults
 * changes at least one of the six checked frames: a wrong gaussian shift, a
 * wrong rounding shift, a short square-root iteration, swapped equation
 * (B.19) weights and a wrong SID spacing. The ITU-T Annex B C code gives the
 * same bytes for all 900 frames.
 */
static void dtx_cng_excitation_check(void)
{
	static const struct {
		uint16_t frame;
		uint8_t bytes[TEST_BYTES];
	} pins[6] = {
		{ 286, { 0xc0, 0xea, 0xc9, 0xf7, 0x90, 0x2c, 0xbb, 0x84,
			 0xe7, 0x26 } },
		{ 313, { 0xb3, 0xf2, 0x75, 0xb8, 0xad, 0x8c, 0xd1, 0x16,
			 0x01, 0xa9 } },
		{ 342, { 0xa1, 0xdf, 0x4f, 0x6f, 0x88, 0xbf, 0x91, 0x75,
			 0x43, 0xe4 } },
		{ 551, { 0xa3, 0x1d, 0x89, 0x74, 0xe0, 0xbd, 0xfe, 0x3a,
			 0x40, 0xe4 } },
		{ 855, { 0x8b, 0x82, 0xc6, 0x33, 0x43, 0xd6, 0x3e, 0x19,
			 0x1b, 0xac } },
		{ 883, { 0xf1, 0xf3, 0xfa, 0xef, 0xa9, 0xdc, 0xc9, 0x66,
			 0x01, 0xa5 } },
	};
	struct udsp_codec *codec = dtx_codec_create();
	int16_t pcm[TEST_FRAME];
	uint8_t payload[TEST_BYTES];
	uint32_t seed = 17;
	unsigned speech_frames = 0;
	unsigned sid_frames = 0;
	unsigned quiet_frames = 0;
	unsigned frame;
	unsigned i;

	CASE("dtx loud-noise excitation scaler golden");
	for (frame = 0; frame < 900; frame++) {
		int result;

		if (frame >= 280 && (frame - 280) % 30 < 6)
			vad_speech_frame_fill(pcm, frame);
		else
			vad_noise_fill(pcm, &seed, 2500 + (frame < 280 ? 0 :
				400 * (int32_t)(((frame - 280) / 30) % 4)));
		result = udsp_codec_g729_ops.encode(codec, pcm, TEST_FRAME,
			payload, sizeof(payload));
		if (result == (int)TEST_BYTES)
			speech_frames++;
		else if (result == 2)
			sid_frames++;
		else if (!result)
			quiet_frames++;
		for (i = 0; i < 6; i++) {
			if (frame == pins[i].frame)
				CHECK(memcmp(payload, pins[i].bytes,
					TEST_BYTES) == 0);
		}
	}
	CHECK_UINT(speech_frames, 315);
	CHECK_UINT(sid_frames, 64);
	CHECK_UINT(quiet_frames, 521);
	udsp_codec_g729_ops.destroy(codec);
}

/* Two SID payloads in the clause B.4.3 wire order (predictor 1, stage one
 * 5, stage two 4, gain 5, one pad bit): predictor 0, first-stage entry 13,
 * second-stage entry 7, gain index 20; and predictor 1, entry 25, entry 2,
 * gain index 25. The distinct gain indices make the equation (B.19)
 * smoothing visible at the second SID. */
static const uint8_t g729_sid_first[2] = { 0x35, 0xe8 };
static const uint8_t g729_sid_second[2] = { 0xe4, 0xb2 };

static void decode_length_check(void)
{
	struct udsp_codec *codec = codec_create();
	struct udsp_codec *packet_codec = codec_create();
	struct udsp_codec *unit_codec = codec_create();
	int16_t pcm[TEST_FRAME * 3];
	int16_t unit_pcm[TEST_FRAME * 3];
	uint8_t packet[TEST_BYTES * 3 + 2];
	unsigned frame;

	CASE("decode length rules");
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, NULL, 0, pcm,
		TEST_FRAME), -EAGAIN);
	memset(packet, 0, sizeof(packet));
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, packet, 1, pcm,
		TEST_FRAME), -EINVAL);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, packet, 9, pcm,
		TEST_FRAME), -EINVAL);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, packet, 11, pcm,
		TEST_FRAME * 2), -EINVAL);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, packet, 21, pcm,
		TEST_FRAME * 3), -EINVAL);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, packet, 32, pcm,
		TEST_FRAME * 3), -EINVAL);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, g729_sid_first, 2, pcm,
		TEST_FRAME - 1), -ENOBUFS);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, g729_sid_first, 2, pcm,
		TEST_FRAME), TEST_FRAME);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, NULL, 0, pcm,
		TEST_FRAME - 1), -ENOBUFS);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, NULL, 0, pcm,
		TEST_FRAME), TEST_FRAME);

	CASE("decode len 0 gates again after reset");
	udsp_codec_g729_ops.reset(codec);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, NULL, 0, pcm,
		TEST_FRAME), -EAGAIN);

	CASE("trailing SID slices match sequential unit decodes");
	memcpy(packet, g729_speech_expected, TEST_BYTES);
	memcpy(packet + TEST_BYTES, g729_speech_expected, TEST_BYTES);
	memcpy(packet + 2 * TEST_BYTES, g729_sid_first, 2);
	CHECK_UINT(udsp_codec_g729_ops.decode(packet_codec,
		packet + TEST_BYTES, TEST_BYTES + 2, pcm,
		TEST_FRAME * 2 - 1), -ENOBUFS);
	CHECK_UINT(udsp_codec_g729_ops.decode(packet_codec,
		packet + TEST_BYTES, TEST_BYTES + 2, pcm, TEST_FRAME * 2),
		TEST_FRAME * 2);
	CHECK_UINT(udsp_codec_g729_ops.decode(unit_codec, packet, TEST_BYTES,
		unit_pcm, TEST_FRAME), TEST_FRAME);
	CHECK_UINT(udsp_codec_g729_ops.decode(unit_codec, g729_sid_first, 2,
		unit_pcm + TEST_FRAME, TEST_FRAME), TEST_FRAME);
	CHECK(memcmp(pcm, unit_pcm,
		TEST_FRAME * 2 * sizeof(pcm[0])) == 0);
	udsp_codec_g729_ops.reset(packet_codec);
	udsp_codec_g729_ops.reset(unit_codec);
	CHECK_UINT(udsp_codec_g729_ops.decode(packet_codec, packet,
		2 * TEST_BYTES + 2, pcm, TEST_FRAME * 3), TEST_FRAME * 3);
	for (frame = 0; frame < 2; frame++)
		CHECK_UINT(udsp_codec_g729_ops.decode(unit_codec,
			packet + frame * TEST_BYTES, TEST_BYTES,
			unit_pcm + frame * TEST_FRAME, TEST_FRAME),
			TEST_FRAME);
	CHECK_UINT(udsp_codec_g729_ops.decode(unit_codec, g729_sid_first, 2,
		unit_pcm + 2 * TEST_FRAME, TEST_FRAME), TEST_FRAME);
	CHECK(memcmp(pcm, unit_pcm,
		TEST_FRAME * 3 * sizeof(pcm[0])) == 0);
	udsp_codec_g729_ops.destroy(unit_codec);
	udsp_codec_g729_ops.destroy(packet_codec);
	udsp_codec_g729_ops.destroy(codec);
}

static const int16_t g729_cng_golden[480] = {
	720, 136, -486, 42, -787, -634, 445, 386, 244, 215, -1, -806, -402, 343,
	-85, -38, 295, -65, -163, 120, 136, 44, 20, -33, 180, -39, -187, 1176,
	617, -843, -379, -265, 124, 411, -111, 160, 91, -347, 189, -21, -42, 114,
	-851, -108, 242, 105, 253, -198, -40, 157, 464, -276, -46, 116, -3, 964,
	-92, -420, -119, -25, -294, 27, 169, -108, 165, 152, -134, 70, -173, -46,
	200, -78, -108, -214, -103, -138, 625, -207, -956, 201,
	82, -1000, 137, 192, -551, 88, 229, -34, 84, -68, 67, 423, 26, 158, -251,
	38, 101, 57, 126, -228, 858, -79, -178, -106, -184, 4, -292, 119, 241,
	-105, 35, 97, 201, 55, 14, 272, -13, -943, 250, 57, 312, 283, 4, -32,
	-162, 74, -61, 170, 10, 204, -122, -335, -515, -129, 302, 25, -172, 123,
	327, 131, 90, -78, -212, -773, 51, 67, 1013, 96, -71, 299, -92, -129, 3,
	-79, -190, -860, 133, 245, 212, 308,
	-30, -917, 174, 369, 44, 132, -130, -555, 289, 174, 153, 202, -93, 80,
	156, 33, -214, 38, -177, -93, 812, 190, 86, -238, -1064, 139, 176, 148,
	120, -180, 238, 49, 78, -144, -147, 58, 67, 44, 157, 244, -307, 719,
	-136, -355, 175, -13, 148, -386, -53, -28, -51, 156, 120, -133, 126, 58,
	73, 269, -1079, 73, 141, -172, 940, -237, 167, -208, 3, 140, -159, 36,
	-149, -195, -251, -267, -93, 563, -203, -221, 146, -74,
	-390, 97, -102, -83, -23, 95, 1085, -122, -230, 6, -87, -68, 45, 102,
	-326, 843, -21, -912, 419, -65, -142, 34, 90, 64, 25, 104, 245, 4, -146,
	44, -325, -24, 109, -181, -237, -120, 129, -256, 106, -566, 1046, 118,
	-415, 423, 47, 26, 7, -96, -117, 52, -290, -166, -145, 79, 212, -171, 57,
	202, 208, -35, -328, -758, 87, 82, 477, 122, 0, 55, 43, -262, -74, 320,
	-695, -146, 129, -182, 367, 337, 474, -228,
	-331, 206, -115, 91, 130, 24, -28, -90, 183, 98, -121, -62, -139, -43,
	109, 384, -77, -246, -277, 92, -32, -369, 226, 199, 191, 314, 1092, 1413,
	-146, -274, -26, -469, -1055, 687, 103, 197, 243, -300, -51, -284, -480,
	-108, 579, 273, 159, 547, 932, -148, -905, -13, -1400, -1316, 47, -235,
	96, 48, 182, 188, -669, -23, -190, -600, 69, 235, 112, 146, 435, 134,
	-1410, -693, 523, -176, -187, 484, 346, 5, -216, 147, -163, -307,
	140, 454, 159, -242, -6, 18, -223, -142, -59, -133, -122, 242, 283, 153,
	781, 2033, 920, -324, -41, -147, -433, -860, -178, 1329, 360, 240, 1704,
	326, -324, -132, -432, -179, -262, 83, 166, -192, 21, 1132, 588, -308,
	-404, -259, -292, -696, -2065, 96, 579, -1028, 246, 421, 96, -298, 827,
	1078, -711, -537, -112, -337, -577, -1028, -618, -443, -524, -8, -63,
	171, 155, -154, 148, 481, 20, -894, -546, -6, -55, -288, 199, 422, -72,
	113,
};

static const int16_t g729_recovery_golden[160] = {
	272, 165, 39, 23, -96, -120, -67, 54, 26, 64, 82, -117, -29, 26, -40,
	-62, 44, 9, -65, 43, 78, -34, -41, 33, 70, -21, -14, 314, 214, -25, -34,
	-11, -19, -61, 4, 27, -12, -44, 52, -2, -29, 6, -225, -38, 58, 6, 58,
	-40, -20, 24, 112, -78, -42, 18, -2, 232, -27, -113, -33, -15, -77, 16,
	35, -36, 36, 29, -40, 7, -45, -14, 41, -27, -33, -61, -30, -39, 153, -59,
	-245, 47,
	11, -249, 32, 42, -136, 16, 54, -13, 15, -19, 17, 105, 3, 37, -65, 6, 23,
	13, 31, -57, 212, -21, -44, -26, -45, 1, -74, 27, 60, -23, 9, 24, 49, 12,
	4, 68, -1, -232, 65, 14, 78, 69, 4, -11, -41, 17, -18, 45, 4, 51, -30,
	-81, -126, -31, 75, 8, -43, 30, 81, 32, 21, -18, -49, -192, 12, 20, 254,
	23, -16, 73, -20, -29, 1, -18, -47, -217, 34, 59, 53, 76,
};

/*
 * The golden continuation sequence: SID, untransmitted, an erased
 * untransmitted, untransmitted, a second SID, untransmitted. During comfort
 * noise, the rules of clause B.4.5 map the erased frame onto decode(NULL, 0).
 * The expected PCM is the output of the ITU-T Annex B C decoder for this
 * record sequence, with the erased record carried as a bad sync word.
 */
static void cng_golden_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t pcm[TEST_FRAME];
	unsigned frame;

	CASE("decoder cng golden sid-continuation-erasure sequence");
	for (frame = 0; frame < 6; frame++) {
		const uint8_t *payload = NULL;
		size_t len = 0;

		if (frame == 0) {
			payload = g729_sid_first;
			len = 2;
		} else if (frame == 4) {
			payload = g729_sid_second;
			len = 2;
		}
		CHECK_UINT(udsp_codec_g729_ops.decode(codec, payload, len,
			pcm, TEST_FRAME), TEST_FRAME);
		CHECK(memcmp(pcm, g729_cng_golden + frame * TEST_FRAME,
			sizeof(pcm)) == 0);
	}
	udsp_codec_g729_ops.destroy(codec);
}

/*
 * The routing of equation (B.27) inside plc(). An erasure during comfort
 * noise is an untransmitted frame. plc() must therefore continue the noise,
 * keep the generator seed running, and leave the past frame type non-active.
 * The second SID then takes the smoothing branch, not the direct reload. The
 * stream and the expected PCM are the records of cng_golden_check, with the
 * erased position run through plc() instead of decode(NULL, 0). The ITU-T C
 * decoder gives identical PCM for both, because equation (B.27) makes an
 * erasure after a non-active frame an untransmitted frame.
 */
static void cng_plc_routing_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t pcm[TEST_FRAME];
	unsigned frame;

	CASE("plc during comfort noise continues the noise");
	for (frame = 0; frame < 6; frame++) {
		int result;

		if (frame == 0)
			result = udsp_codec_g729_ops.decode(codec,
				g729_sid_first, 2, pcm, TEST_FRAME);
		else if (frame == 4)
			result = udsp_codec_g729_ops.decode(codec,
				g729_sid_second, 2, pcm, TEST_FRAME);
		else if (frame == 2)
			result = udsp_codec_g729_ops.plc(codec, pcm,
				TEST_FRAME, NULL, 0);
		else
			result = udsp_codec_g729_ops.decode(codec, NULL, 0,
				pcm, TEST_FRAME);
		CHECK_UINT(result, TEST_FRAME);
		CHECK(memcmp(pcm, g729_cng_golden + frame * TEST_FRAME,
			sizeof(pcm)) == 0);
	}
	udsp_codec_g729_ops.destroy(codec);
}

/*
 * The gain recovery of clause B.4.5. After comfort noise that a SID started,
 * the stream carries two speech frames, then an erased frame, then an
 * untransmitted frame whose SID never arrived. The past frame of the erasure
 * is active, so plc conceals the erasure as speech. The decoder quantizes the
 * saved excitation energy of the last good speech frame through the SID gain
 * quantizer, the one caller of the count-zero branch. The expected PCM of the
 * recovery frame and its successor is the output of the ITU-T C decoder for
 * these records.
 */
static void cng_recovery_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t pcm[TEST_FRAME];
	unsigned frame;

	CASE("decoder cng erasure recovery golden");
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, g729_sid_first, 2, pcm,
		TEST_FRAME), TEST_FRAME);
	CHECK_UINT(udsp_codec_g729_ops.decode(codec, NULL, 0, pcm,
		TEST_FRAME), TEST_FRAME);
	for (frame = 0; frame < 2; frame++)
		CHECK_UINT(udsp_codec_g729_ops.decode(codec,
			g729_speech_expected, TEST_BYTES, pcm, TEST_FRAME),
			TEST_FRAME);
	CHECK_UINT(udsp_codec_g729_ops.plc(codec, pcm, TEST_FRAME, NULL, 0),
		TEST_FRAME);
	for (frame = 0; frame < 2; frame++) {
		CHECK_UINT(udsp_codec_g729_ops.decode(codec, NULL, 0, pcm,
			TEST_FRAME), TEST_FRAME);
		CHECK(memcmp(pcm, g729_recovery_golden + frame * TEST_FRAME,
			sizeof(pcm)) == 0);
	}
	udsp_codec_g729_ops.destroy(codec);
}

static void cng_reset_check(void)
{
	struct udsp_codec *reused = codec_create();
	struct udsp_codec *fresh = codec_create();
	int16_t reused_pcm[TEST_FRAME];
	int16_t fresh_pcm[TEST_FRAME];
	unsigned frame;

	CASE("reset clears the cng state to a fresh instance");
	CHECK_UINT(udsp_codec_g729_ops.decode(reused, g729_sid_second, 2,
		reused_pcm, TEST_FRAME), TEST_FRAME);
	for (frame = 0; frame < 3; frame++)
		CHECK_UINT(udsp_codec_g729_ops.decode(reused, NULL, 0,
			reused_pcm, TEST_FRAME), TEST_FRAME);
	udsp_codec_g729_ops.reset(reused);
	CHECK_UINT(udsp_codec_g729_ops.decode(reused, NULL, 0, reused_pcm,
		TEST_FRAME), -EAGAIN);
	for (frame = 0; frame < 4; frame++) {
		const uint8_t *payload = frame & 1 ? g729_sid_first :
			g729_speech_expected;
		size_t len = frame & 1 ? 2 : TEST_BYTES;

		CHECK_UINT(udsp_codec_g729_ops.decode(reused, payload, len,
			reused_pcm, TEST_FRAME), TEST_FRAME);
		CHECK_UINT(udsp_codec_g729_ops.decode(fresh, payload, len,
			fresh_pcm, TEST_FRAME), TEST_FRAME);
		CHECK(memcmp(reused_pcm, fresh_pcm,
			sizeof(reused_pcm)) == 0);
	}
	udsp_codec_g729_ops.destroy(fresh);
	udsp_codec_g729_ops.destroy(reused);
}

struct oracle_sequence {
	const char *name;
	unsigned frames;
};

static uint16_t little_word_get(const uint8_t *bytes)
{
	return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static int pcm_frame_read(FILE *stream, int16_t *pcm)
{
	uint8_t bytes[TEST_FRAME * 2];
	size_t count;
	unsigned i;

	count = fread(bytes, 1, sizeof(bytes), stream);
	if (count != sizeof(bytes))
		return feof(stream) ? 0 : -1;
	for (i = 0; i < TEST_FRAME; i++) {
		uint16_t value = little_word_get(bytes + 2 * i);

		pcm[i] = value <= INT16_MAX ? (int16_t)value :
			(int16_t)((int32_t)value - 65536);
	}
	return 1;
}

/* One ITU serial record: a sync word, a size word of 0 (untransmitted),
 * 16 (octet-mode SID) or 80 (speech) bits, then one little-endian word per
 * bit. The bits pack MSB-first into size/8 payload bytes. The erasure rules
 * of the ITU-T C decoder: a zero bit word marks a sized record erased and
 * its sync word is never inspected; a size-0 record is erased when its sync
 * word differs from 0x6b21. */
static int bit_record_read(FILE *stream, unsigned *bytes, uint8_t *payload,
			   bool *erased)
{
	uint8_t head[4];
	uint8_t words[160];
	size_t count;
	unsigned bits;
	unsigned bit;

	count = fread(head, 1, sizeof(head), stream);
	if (count != sizeof(head))
		return !count && feof(stream) ? 0 : -1;
	bits = little_word_get(head + 2);
	if (bits != 0 && bits != 16 && bits != 80)
		return -1;
	if (!bits) {
		*erased = little_word_get(head) != 0x6b21;
		*bytes = 0;
		return 1;
	}
	if (fread(words, 1, 2 * bits, stream) != 2 * bits)
		return -1;
	*erased = false;
	*bytes = bits / 8;
	memset(payload, 0, TEST_BYTES);
	for (bit = 0; bit < bits; bit++) {
		uint16_t value = little_word_get(words + 2 * bit);

		if (!value) {
			*erased = true;
			continue;
		}
		if (value == 0x81)
			payload[bit / 8] |= (uint8_t)(1U << (7 - bit % 8));
		else if (value != 0x7f)
			return -1;
	}
	return 1;
}

static int bit_frame_read(FILE *stream, uint8_t *payload, bool *erased)
{
	unsigned bytes;
	int status = bit_record_read(stream, &bytes, payload, erased);

	if (status == 1 && bytes != TEST_BYTES)
		return -1;
	return status;
}

static FILE *oracle_file_open(const char *directory, const char *name,
			      const char *suffix)
{
	char path[1024];

	snprintf(path, sizeof(path), "%s/%s.%s", directory, name, suffix);
	return fopen(path, "rb");
}

static bool oracle_skip(const char *directory, const char *reason)
{
	if (directory)
		return false;
	tu_skip(reason);
	return true;
}

static void oracle_encoder_check(const char *directory,
				 const struct oracle_sequence *sequence)
{
	struct udsp_codec *codec;
	FILE *input;
	FILE *bits;
	unsigned frames = 0;
	unsigned differences = 0;
	bool complete = false;
	char case_name[96];

	snprintf(case_name, sizeof(case_name), "ITU encoder %s bit identity",
		sequence->name);
	CASE(case_name);
	if (oracle_skip(directory, "G729_TV_DIR not set"))
		return;
	codec = codec_create();
	input = oracle_file_open(directory, sequence->name, "IN");
	bits = oracle_file_open(directory, sequence->name, "BIT");
	CHECK(codec != NULL);
	CHECK(input != NULL);
	CHECK(bits != NULL);
	if (!codec || !input || !bits)
		goto out;
	while (frames < sequence->frames) {
		int16_t pcm[TEST_FRAME];
		uint8_t expected[TEST_BYTES];
		uint8_t actual[TEST_BYTES];
		bool erased;
		int pcm_status;
		int bit_status;

		pcm_status = pcm_frame_read(input, pcm);
		bit_status = bit_frame_read(bits, expected, &erased);
		CHECK_UINT(pcm_status, 1);
		CHECK_UINT(bit_status, 1);
		if (pcm_status != 1 || bit_status != 1)
			break;
		CHECK(!erased);
		CHECK_UINT(udsp_codec_g729_ops.encode(codec, pcm, TEST_FRAME,
			actual, sizeof(actual)), TEST_BYTES);
		if (memcmp(actual, expected, TEST_BYTES))
			differences++;
		frames++;
	}
	if (frames == sequence->frames) {
		int16_t pcm[TEST_FRAME];
		uint8_t payload[TEST_BYTES];
		bool erased;
		int pcm_status = pcm_frame_read(input, pcm);
		int bit_status = bit_frame_read(bits, payload, &erased);

		CHECK_UINT(pcm_status, 0);
		CHECK_UINT(bit_status, 0);
		complete = pcm_status == 0 && bit_status == 0;
	}
	CHECK_UINT(frames, sequence->frames);
	CHECK_UINT(differences, 0);
	printf("# encoder %-8s frames %u bytes %u %s\n", sequence->name,
		frames, frames * TEST_BYTES,
		!differences && complete ? "PASS" : "FAIL");
out:
	if (bits)
		fclose(bits);
	if (input)
		fclose(input);
	udsp_codec_g729_ops.destroy(codec);
}

static void oracle_decoder_check(const char *directory,
				 const struct oracle_sequence *sequence)
{
	struct udsp_codec *codec;
	FILE *bits;
	FILE *expected_stream;
	unsigned frames = 0;
	unsigned differences = 0;
	bool complete = false;
	char case_name[96];

	snprintf(case_name, sizeof(case_name), "ITU decoder %s bit identity",
		sequence->name);
	CASE(case_name);
	if (oracle_skip(directory, "G729_TV_DIR not set"))
		return;
	codec = codec_create();
	bits = oracle_file_open(directory, sequence->name, "BIT");
	expected_stream = oracle_file_open(directory, sequence->name, "PST");
	CHECK(codec != NULL);
	CHECK(bits != NULL);
	CHECK(expected_stream != NULL);
	if (!codec || !bits || !expected_stream)
		goto out;
	while (frames < sequence->frames) {
		int16_t expected[TEST_FRAME];
		int16_t actual[TEST_FRAME];
		uint8_t payload[TEST_BYTES];
		bool erased;
		int bit_status = bit_frame_read(bits, payload, &erased);
		int pcm_status = pcm_frame_read(expected_stream, expected);

		CHECK_UINT(bit_status, 1);
		CHECK_UINT(pcm_status, 1);
		if (bit_status != 1 || pcm_status != 1)
			break;
		if (erased)
			CHECK_UINT(udsp_codec_g729_ops.plc(codec, actual, TEST_FRAME,
				NULL, 0), TEST_FRAME);
		else
			CHECK_UINT(udsp_codec_g729_ops.decode(codec, payload,
				TEST_BYTES, actual, TEST_FRAME), TEST_FRAME);
		if (memcmp(actual, expected, sizeof(actual)))
			differences++;
		frames++;
	}
	if (frames == sequence->frames) {
		int16_t expected[TEST_FRAME];
		uint8_t payload[TEST_BYTES];
		bool erased;
		int bit_status = bit_frame_read(bits, payload, &erased);
		int pcm_status = pcm_frame_read(expected_stream, expected);

		CHECK_UINT(bit_status, 0);
		CHECK_UINT(pcm_status, 0);
		complete = bit_status == 0 && pcm_status == 0;
	}
	CHECK_UINT(frames, sequence->frames);
	CHECK_UINT(differences, 0);
	printf("# decoder %-8s frames %u bytes %u %s\n", sequence->name,
		frames, frames * TEST_FRAME * 2,
		!differences && complete ? "PASS" : "FAIL");
out:
	if (expected_stream)
		fclose(expected_stream);
	if (bits)
		fclose(bits);
	udsp_codec_g729_ops.destroy(codec);
}

static int vad_bit_frame_skip(FILE *stream, unsigned *bits)
{
	uint8_t payload[TEST_BYTES];
	unsigned bytes = 0;
	bool erased;
	int status = bit_record_read(stream, &bytes, payload, &erased);

	*bits = bytes * 8;
	return status;
}

/* The Annex B DTX bitstream carries the VAD verdict in each record's size
 * word: an 80-bit record is a voiced frame, a 16-bit SID record and a 0-bit
 * record are both non-voice. Encoding tstseqN.bin and comparing against the
 * record sizes of tstseqNa.bit checks the per-frame decision of this codec
 * against the decision of the ITU-T C encoder. */
static void oracle_vad_sequence_check(const char *directory,
				      const char *name)
{
	struct udsp_codec *codec;
	char bit_name[32];
	FILE *input;
	FILE *bits;
	unsigned frames = 0;
	unsigned differences = 0;
	bool complete = false;
	char case_name[96];

	snprintf(case_name, sizeof(case_name), "ITU VAD %s decision identity",
		name);
	CASE(case_name);
	if (oracle_skip(directory, "G729B_TV_DIR not set"))
		return;
	codec = codec_create();
	input = oracle_file_open(directory, name, "bin");
	snprintf(bit_name, sizeof(bit_name), "%sa", name);
	bits = oracle_file_open(directory, bit_name, "bit");
	CHECK(codec != NULL);
	CHECK(input != NULL);
	CHECK(bits != NULL);
	if (!codec || !input || !bits)
		goto out;
	for (;;) {
		int16_t pcm[TEST_FRAME];
		uint8_t payload[TEST_BYTES];
		unsigned record_bits = 0;
		int pcm_status = pcm_frame_read(input, pcm);
		int bit_status = vad_bit_frame_skip(bits, &record_bits);

		CHECK(pcm_status >= 0);
		CHECK(bit_status >= 0);
		if (pcm_status <= 0 || bit_status <= 0) {
			complete = pcm_status == 0 && bit_status == 0;
			break;
		}
		CHECK_UINT(udsp_codec_g729_ops.encode(codec, pcm, TEST_FRAME,
			payload, sizeof(payload)), TEST_BYTES);
		if (g729_vad_decision_get(codec) != (record_bits == 80))
			differences++;
		frames++;
	}
	CHECK(complete);
	CHECK(frames > 0);
	CHECK_UINT(differences, 0);
	printf("# vad %-8s frames %u %s\n", name, frames,
		complete && frames && !differences ? "PASS" : "FAIL");
out:
	if (bits)
		fclose(bits);
	if (input)
		fclose(input);
	udsp_codec_g729_ops.destroy(codec);
}

/* The A+B encoder check: encode the sequence with dtx and compare every
 * record against the ITU-T bit stream, the frame kind through the
 * record size and the payload bytes for speech and SID frames alike. */
static void oracle_ab_encoder_check(const char *directory, const char *name)
{
	struct udsp_codec *codec;
	char bit_name[32];
	FILE *input;
	FILE *bits;
	unsigned frames = 0;
	unsigned sid_frames = 0;
	unsigned quiet_frames = 0;
	unsigned differences = 0;
	bool complete = false;
	char case_name[96];

	snprintf(case_name, sizeof(case_name),
		"ITU A+B encoder %s bit identity", name);
	CASE(case_name);
	if (oracle_skip(directory, "G729B_TV_DIR not set"))
		return;
	codec = dtx_codec_create();
	input = oracle_file_open(directory, name, "bin");
	snprintf(bit_name, sizeof(bit_name), "%sa", name);
	bits = oracle_file_open(directory, bit_name, "bit");
	CHECK(codec != NULL);
	CHECK(input != NULL);
	CHECK(bits != NULL);
	if (!codec || !input || !bits)
		goto out;
	for (;;) {
		int16_t pcm[TEST_FRAME];
		uint8_t expected[TEST_BYTES];
		uint8_t actual[TEST_BYTES];
		unsigned expected_bytes = 0;
		bool erased = true;
		int pcm_status = pcm_frame_read(input, pcm);
		int bit_status = bit_record_read(bits, &expected_bytes,
			expected, &erased);
		int result;

		CHECK(pcm_status >= 0);
		CHECK(bit_status >= 0);
		if (pcm_status <= 0 || bit_status <= 0) {
			complete = pcm_status == 0 && bit_status == 0;
			break;
		}
		CHECK(!erased);
		result = udsp_codec_g729_ops.encode(codec, pcm, TEST_FRAME,
			actual, sizeof(actual));
		CHECK(result >= 0);
		if (result < 0)
			break;
		if ((unsigned)result != expected_bytes ||
		    memcmp(actual, expected, expected_bytes))
			differences++;
		if (expected_bytes == 2)
			sid_frames++;
		else if (!expected_bytes)
			quiet_frames++;
		frames++;
	}
	CHECK(complete);
	CHECK(frames > 0);
	CHECK(sid_frames > 0);
	CHECK(quiet_frames > 0);
	CHECK_UINT(differences, 0);
	printf("# ab encoder %-8s frames %u sid %u quiet %u %s\n", name,
		frames, sid_frames, quiet_frames,
		complete && frames && !differences ? "PASS" : "FAIL");
out:
	if (bits)
		fclose(bits);
	if (input)
		fclose(input);
	udsp_codec_g729_ops.destroy(codec);
}

/* The A+B decoder check. The record kind maps to the decode API of codec.h:
 * 80 bits to a 10-byte speech decode, 16 bits to a 2-byte SID decode, 0
 * bits to decode(NULL, 0), and every erased record to plc(). The codec
 * applies the equation (B.27) rule itself, so the test needs no past-frame
 * bookkeeping. */
static void oracle_ab_decoder_check(const char *directory,
				    const char *bit_name,
				    const char *out_name)
{
	struct udsp_codec *codec;
	FILE *bits;
	FILE *expected_stream;
	unsigned frames = 0;
	unsigned sid_frames = 0;
	unsigned quiet_frames = 0;
	unsigned erased_frames = 0;
	unsigned differences = 0;
	bool complete = false;
	char case_name[96];

	snprintf(case_name, sizeof(case_name),
		"ITU A+B decoder %s bit identity", bit_name);
	CASE(case_name);
	if (oracle_skip(directory, "G729B_TV_DIR not set"))
		return;
	codec = codec_create();
	bits = oracle_file_open(directory, bit_name, "bit");
	expected_stream = oracle_file_open(directory, out_name, "out");
	CHECK(codec != NULL);
	CHECK(bits != NULL);
	CHECK(expected_stream != NULL);
	if (!codec || !bits || !expected_stream)
		goto out;
	for (;;) {
		int16_t expected[TEST_FRAME];
		int16_t actual[TEST_FRAME];
		uint8_t payload[TEST_BYTES];
		unsigned bytes = 0;
		bool erased = false;
		int bit_status = bit_record_read(bits, &bytes, payload,
			&erased);
		int pcm_status = pcm_frame_read(expected_stream, expected);
		int result;

		CHECK(bit_status >= 0);
		CHECK(pcm_status >= 0);
		if (bit_status <= 0 || pcm_status <= 0) {
			complete = bit_status == 0 && pcm_status == 0;
			break;
		}
		if (erased) {
			erased_frames++;
			result = udsp_codec_g729_ops.plc(codec, actual,
				TEST_FRAME, NULL, 0);
		} else if (bytes == TEST_BYTES) {
			result = udsp_codec_g729_ops.decode(codec, payload,
				TEST_BYTES, actual, TEST_FRAME);
		} else if (bytes == 2) {
			result = udsp_codec_g729_ops.decode(codec, payload, 2,
				actual, TEST_FRAME);
			sid_frames++;
		} else {
			result = udsp_codec_g729_ops.decode(codec, NULL, 0,
				actual, TEST_FRAME);
			quiet_frames++;
		}
		CHECK_UINT(result, TEST_FRAME);
		if (result != (int)TEST_FRAME)
			break;
		if (memcmp(actual, expected, sizeof(actual)))
			differences++;
		frames++;
	}
	CHECK(complete);
	CHECK(frames > 0);
	CHECK_UINT(differences, 0);
	printf("# ab decoder %-9s frames %u sid %u quiet %u erased %u %s\n",
		bit_name, frames, sid_frames, quiet_frames, erased_frames,
		complete && frames && !differences ? "PASS" : "FAIL");
out:
	if (expected_stream)
		fclose(expected_stream);
	if (bits)
		fclose(bits);
	udsp_codec_g729_ops.destroy(codec);
}

static void oracle_annexb_check(void)
{
	static const char *const sequences[4] = {
		"tstseq1", "tstseq2", "tstseq3", "tstseq4",
	};
	/* readmeabTV.txt: tstseq1-4 pair the encoder's own bit streams with
	 * their decoded outputs; tstseq5 and tstseq6 are decoder-only bit
	 * streams whose A+B outputs carry the a suffix. */
	static const struct {
		const char *bit_name;
		const char *out_name;
	} decoder_pairs[6] = {
		{ "tstseq1a", "tstseq1a" }, { "tstseq2a", "tstseq2a" },
		{ "tstseq3a", "tstseq3a" }, { "tstseq4a", "tstseq4a" },
		{ "tstseq5", "tstseq5a" }, { "tstseq6", "tstseq6a" },
	};
	const char *directory = getenv("G729B_TV_DIR");
	unsigned i;

	for (i = 0; i < 4; i++)
		oracle_vad_sequence_check(directory, sequences[i]);
	for (i = 0; i < 4; i++)
		oracle_ab_encoder_check(directory, sequences[i]);
	for (i = 0; i < 6; i++)
		oracle_ab_decoder_check(directory, decoder_pairs[i].bit_name,
			decoder_pairs[i].out_name);
}

static void oracle_check(void)
{
	static const struct oracle_sequence encoder_sequences[] = {
		{ "ALGTHM", 35 }, { "FIXED", 120 }, { "LSP", 2232 },
		{ "PITCH", 1835 }, { "SPEECH", 3750 }, { "TAME", 128 },
	};
	static const struct oracle_sequence decoder_sequences[] = {
		{ "ALGTHM", 35 }, { "ERASURE", 300 }, { "FIXED", 120 },
		{ "LSP", 2232 }, { "OVERFLOW", 384 }, { "PARITY", 300 },
		{ "PITCH", 1835 }, { "SPEECH", 3750 }, { "TAME", 128 },
	};
	const char *directory = getenv("G729_TV_DIR");
	unsigned i;

	/* A host without the vectors reports each sequence case as a skip. A
	 * set directory that is wrong or truncated fails the sequence cases. */
	for (i = 0; i < sizeof(encoder_sequences) / sizeof(encoder_sequences[0]);
	     i++)
		oracle_encoder_check(directory, &encoder_sequences[i]);
	for (i = 0; i < sizeof(decoder_sequences) / sizeof(decoder_sequences[0]);
	     i++)
		oracle_decoder_check(directory, &decoder_sequences[i]);
}

/*
 * The constants of the normative C code for the tables that g729.c computes.
 * They are the independent reference of derived_tables_check and stay here,
 * apart from the library.
 */
static const int16_t expected_power2[33] = {
	16384, 16743, 17109, 17484, 17867, 18258, 18658, 19066, 19484, 19911,
	20347, 20792, 21247, 21713, 22188, 22674, 23170, 23678, 24196, 24726,
	25268, 25821, 26386, 26964, 27554, 28158, 28774, 29405, 30048, 30706,
	31379, 32066, 32767,
};

static const int16_t expected_log2[33] = {
	0, 1455, 2866, 4236, 5568, 6863, 8124, 9352, 10549, 11716, 12855,
	13967, 15054, 16117, 17156, 18172, 19167, 20142, 21097, 22033, 22951,
	23852, 24735, 25603, 26455, 27291, 28113, 28922, 29716, 30497, 31266,
	32023, 32767,
};

static const int16_t expected_inverse_sqrt[49] = {
	32767, 31790, 30894, 30070, 29309, 28602, 27945, 27330, 26755, 26214,
	25705, 25225, 24770, 24339, 23930, 23541, 23170, 22817, 22479, 22155,
	21845, 21548, 21263, 20988, 20724, 20470, 20225, 19988, 19760, 19539,
	19326, 19119, 18919, 18725, 18536, 18354, 18176, 18004, 17837, 17674,
	17515, 17361, 17211, 17064, 16921, 16782, 16646, 16514, 16384,
};

static const int16_t expected_hamwindow[240] = {
	2621, 2623, 2629, 2638, 2651, 2668, 2689, 2713, 2741, 2772, 2808, 2847,
	2890, 2936, 2986, 3040, 3097, 3158, 3223, 3291, 3363, 3438, 3517, 3599,
	3685, 3774, 3867, 3963, 4063, 4166, 4272, 4382, 4495, 4611, 4731, 4853,
	4979, 5108, 5240, 5376, 5514, 5655, 5800, 5947, 6097, 6250, 6406, 6565,
	6726, 6890, 7057, 7227, 7399, 7573, 7750, 7930, 8112, 8296, 8483, 8672,
	8863, 9057, 9252, 9450, 9650, 9852, 10055, 10261, 10468, 10677, 10888,
	11101, 11315, 11531, 11748, 11967, 12187, 12409, 12632, 12856, 13082,
	13308, 13536, 13764, 13994, 14225, 14456, 14688, 14921, 15155, 15389,
	15624, 15859, 16095, 16331, 16568, 16805, 17042, 17279, 17516, 17754,
	17991, 18228, 18465, 18702, 18939, 19175, 19411, 19647, 19882, 20117,
	20350, 20584, 20816, 21048, 21279, 21509, 21738, 21967, 22194, 22420,
	22644, 22868, 23090, 23311, 23531, 23749, 23965, 24181, 24394, 24606,
	24816, 25024, 25231, 25435, 25638, 25839, 26037, 26234, 26428, 26621,
	26811, 26999, 27184, 27368, 27548, 27727, 27903, 28076, 28247, 28415,
	28581, 28743, 28903, 29061, 29215, 29367, 29515, 29661, 29804, 29944,
	30081, 30214, 30345, 30472, 30597, 30718, 30836, 30950, 31062, 31170,
	31274, 31376, 31474, 31568, 31659, 31747, 31831, 31911, 31988, 32062,
	32132, 32198, 32261, 32320, 32376, 32428, 32476, 32521, 32561, 32599,
	32632, 32662, 32688, 32711, 32729, 32744, 32755, 32763, 32767, 32767,
	32741, 32665, 32537, 32359, 32129, 31850, 31521, 31143, 30716, 30242,
	29720, 29151, 28538, 27879, 27177, 26433, 25647, 24821, 23957, 23055,
	22117, 21145, 20139, 19102, 18036, 16941, 15820, 14674, 13505, 12315,
	11106, 9879, 8637, 7381, 6114, 4838, 3554, 2264, 971,
};

static const int16_t expected_cos[65] = {
	32767, 32729, 32610, 32413, 32138, 31786, 31357, 30853, 30274, 29622,
	28899, 28106, 27246, 26320, 25330, 24279, 23170, 22006, 20788, 19520,
	18205, 16846, 15447, 14010, 12540, 11039, 9512, 7962, 6393, 4808, 3212,
	1608, 0, -1608, -3212, -4808, -6393, -7962, -9512, -11039, -12540,
	-14010, -15447, -16846, -18205, -19520, -20788, -22006, -23170, -24279,
	-25330, -26320, -27246, -28106, -28899, -29622, -30274, -30853, -31357,
	-31786, -32138, -32413, -32610, -32729, -32768,
};

static const int16_t expected_cos_slope[64] = {
	-632, -1893, -3150, -4399, -5638, -6863, -8072, -9261, -10428, -11570,
	-12684, -13767, -14817, -15832, -16808, -17744, -18637, -19486, -20287,
	-21039, -21741, -22390, -22986, -23526, -24009, -24435, -24801, -25108,
	-25354, -25540, -25664, -25726, -25726, -25664, -25540, -25354, -25108,
	-24801, -24435, -24009, -23526, -22986, -22390, -21741, -21039, -20287,
	-19486, -18637, -17744, -16808, -15832, -14817, -13767, -12684, -11570,
	-10428, -9261, -8072, -6863, -5638, -4399, -3150, -1893, -632,
};

static const int16_t expected_acos_slope[64] = {
	-26887, -8812, -5323, -3813, -2979, -2444, -2081, -1811, -1608, -1450,
	-1322, -1219, -1132, -1059, -998, -946, -901, -861, -827, -797, -772,
	-750, -730, -713, -699, -687, -677, -668, -662, -657, -654, -652, -652,
	-654, -657, -662, -668, -677, -687, -699, -713, -730, -750, -772, -797,
	-827, -861, -901, -946, -998, -1059, -1132, -1219, -1322, -1450, -1608,
	-1811, -2081, -2444, -2979, -3813, -5323, -8812, -26887,
};

#define DERIVED_TABLE(name, expected) \
	{ #name, name, expected, sizeof(expected) / sizeof((expected)[0]) }

static void derived_tables_check(void)
{
	static const struct {
		const char *name;
		const int16_t *actual;
		const int16_t *expected;
		unsigned count;
	} tables[] = {
		DERIVED_TABLE(g729_power2_table, expected_power2),
		DERIVED_TABLE(g729_log2_table, expected_log2),
		DERIVED_TABLE(g729_inverse_sqrt_table, expected_inverse_sqrt),
		DERIVED_TABLE(g729_hamwindow, expected_hamwindow),
		DERIVED_TABLE(g729_cos_table, expected_cos),
		DERIVED_TABLE(g729_cos_slope, expected_cos_slope),
		DERIVED_TABLE(g729_acos_slope, expected_acos_slope),
	};
	unsigned mismatches = 0;
	unsigned entries = 0;
	unsigned table;

	CASE("derived tables equal the normative constants");
	g729_tables_init();
	for (table = 0; table < sizeof(tables) / sizeof(tables[0]); table++) {
		const int16_t *actual = tables[table].actual;
		const int16_t *expected = tables[table].expected;
		unsigned i;

		for (i = 0; i < tables[table].count; i++) {
			entries++;
			if (actual[i] == expected[i])
				continue;
			printf("# %s[%u] %d, want %d\n", tables[table].name, i,
				actual[i], expected[i]);
			mismatches++;
		}
	}
	CHECK_UINT(entries, 548);
	CHECK_UINT(mismatches, 0);

	CASE("derived tables match their CRC-32 values");
	CHECK(g729_tables_verify());
}

int main(void)
{
	metadata_check();
	errors_check();
	vectors_check();
	round_trip_check();
	packet_decode_check();
	reset_check();
	plc_check();
	cn_check();
	vad_silence_check();
	vad_noise_check();
	vad_speech_check();
	vad_hangover_check();
	vad_golden_check();
	vad_margin_check();
	vad_wrap_check();
	levinson_commit_check();
	vad_reset_check();
	dtx_disabled_check();
	dtx_sequence_check();
	dtx_reset_check();
	dtx_pacing_check();
	dtx_cng_excitation_check();
	decode_length_check();
	cng_golden_check();
	cng_plc_routing_check();
	cng_recovery_check();
	cng_reset_check();
	oracle_check();
	oracle_annexb_check();
	derived_tables_check();
	return tu_summary("g729");
}
