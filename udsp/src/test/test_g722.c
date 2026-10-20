// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_G722_ORACLE
#include <spandsp.h>
#endif

#include "dsp.h"
#include "plc.h"
#include "util.h"
#include "vectors/g722_vectors.h"

#define TEST_FRAME	160U
#define TEST_BYTES	80U
#define TEST_FRAMES	100U
#define TEST_SAMPLES	(TEST_FRAME * TEST_FRAMES)
#define PLC_DELAY	60U

extern const struct udsp_codec_ops udsp_codec_g722_ops;

static void tone_fill(int16_t *pcm, unsigned offset, double first_hz,
		      double second_hz)
{
	unsigned i;

	for (i = 0; i < TEST_FRAME; i++) {
		double time = (offset + i) / 16000.0;
		double sample = 8000.0 * sin(2.0 * M_PI * first_hz * time);

		if (second_hz > 0.0)
			sample += 4000.0 * sin(2.0 * M_PI * second_hz * time);
		pcm[i] = (int16_t)lrint(sample);
	}
}

static void tone_amplitude_fill(int16_t *pcm, unsigned offset, double frequency,
				double amplitude)
{
	unsigned i;

	for (i = 0; i < TEST_FRAME; i++) {
		double time = (offset + i) / 16000.0;

		pcm[i] = (int16_t)lrint(amplitude *
					 sin(2.0 * M_PI * frequency * time));
	}
}

#ifdef HAVE_G722_ORACLE
static void tone_level_fill(int16_t *pcm, unsigned offset, double frequency,
			    double level_dbov)
{
	tone_amplitude_fill(pcm, offset, frequency,
			    32767.0 * pow(10.0, level_dbov / 20.0));
}
#endif

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

static struct udsp_codec *codec_create(void)
{
	struct udsp_codec_params params = { 0 };

	return udsp_codec_g722_ops.create(&params);
}

static uint64_t pcm_hash_get(uint64_t hash, const int16_t *pcm, unsigned count)
{
	unsigned i;

	for (i = 0; i < count; i++) {
		uint16_t sample = (uint16_t)pcm[i];

		hash ^= sample & 0xff;
		hash *= UINT64_C(1099511628211);
		hash ^= sample >> 8;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

enum predictor_signal {
	PREDICTOR_POLE,
	PREDICTOR_UPPOL1,
};

static void predictor_payload_fill(uint8_t *payload, unsigned frame,
				   enum predictor_signal signal)
{
	unsigned i;

	for (i = 0; i < TEST_BYTES; i++) {
		if (signal == PREDICTOR_UPPOL1) {
			payload[i] = (uint8_t)(i + frame);
			continue;
		}
		payload[i] = (i / 3 + frame / 2) & 1 ? 0x3f : 0xc0;
	}
}

static uint64_t predictor_hash_get(enum predictor_signal signal)
{
	struct udsp_codec *codec = codec_create();
	uint64_t hash = UINT64_C(1469598103934665603);
	unsigned frame;

	for (frame = 0; frame < TEST_FRAMES; frame++) {
		int16_t pcm[TEST_FRAME];
		uint8_t payload[TEST_BYTES];

		predictor_payload_fill(payload, frame, signal);
		udsp_codec_g722_ops.decode(codec, payload, TEST_BYTES,
					    pcm, TEST_FRAME);
		hash = pcm_hash_get(hash, pcm, TEST_FRAME);
	}
	udsp_codec_g722_ops.destroy(codec);
	return hash;
}

static void predictor_check(void)
{
	CASE("predictor second pole limit");
	CHECK_UINT(predictor_hash_get(PREDICTOR_POLE),
		   UINT64_C(0x0ab3cd6ebcce7982));

	CASE("predictor UPPOL1 stability clamp");
	CHECK_UINT(predictor_hash_get(PREDICTOR_UPPOL1),
		   UINT64_C(0x49e1fe6590198e1f));
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
			      unsigned count)
{
	double best = -200.0;
	unsigned delay;

	for (delay = 0; delay <= 120; delay++) {
		double snr = snr_get(reference + 240, actual + 240 + delay,
				     count - 240 - delay);

		if (snr > best)
			best = snr;
	}
	return best;
}

static void vectors_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t decoded[TEST_FRAME * 2];
	int16_t pcm[TEST_FRAME];
	uint8_t payload[160];
	uint32_t seed = 1;
	int32_t shaped = 0;
	unsigned frame;

	CASE("frozen encoder two tone");
	CHECK(codec != NULL);
	for (frame = 0; frame < 2; frame++) {
		tone_fill(pcm, frame * TEST_FRAME, 1000.0, 5000.0);
		CHECK_UINT(udsp_codec_g722_ops.encode(codec, pcm, TEST_FRAME,
						      payload + frame * TEST_BYTES,
						      TEST_BYTES), TEST_BYTES);
	}
	CHECK(memcmp(payload, g722_tone_expected, sizeof(payload)) == 0);
	udsp_codec_g722_ops.reset(codec);

	CASE("frozen encoder speech shaped");
	for (frame = 0; frame < 2; frame++) {
		speech_fill(pcm, &seed, &shaped);
		CHECK_UINT(udsp_codec_g722_ops.encode(codec, pcm, TEST_FRAME,
						      payload + frame * TEST_BYTES,
						      TEST_BYTES), TEST_BYTES);
	}
	CHECK(memcmp(payload, g722_speech_expected, sizeof(payload)) == 0);
	udsp_codec_g722_ops.reset(codec);

	CASE("frozen decoder two frames");
	for (frame = 0; frame < 2; frame++) {
		CHECK_UINT(udsp_codec_g722_ops.decode(codec,
			g722_tone_expected + frame * TEST_BYTES, TEST_BYTES,
			decoded + frame * TEST_FRAME, TEST_FRAME), TEST_FRAME);
	}
	CHECK(memcmp(decoded, g722_decoder_expected, sizeof(decoded)) == 0);
	udsp_codec_g722_ops.destroy(codec);
}

#ifdef HAVE_G722_ORACLE
static unsigned sample_mismatch_count(const int16_t *expected,
				      const int16_t *actual, unsigned count)
{
	unsigned mismatches = 0;
	unsigned i;

	for (i = 0; i < count; i++) {
		if (expected[i] != actual[i])
			mismatches++;
	}
	return mismatches;
}

static void oracle_encoder_check(bool speech)
{
	struct udsp_codec *codec = codec_create();
	g722_encode_state_t *oracle = g722_encode_init(NULL, 64000, G722_PACKED);
	int16_t pcm[TEST_FRAME];
	uint8_t actual[TEST_BYTES];
	uint8_t expected[TEST_BYTES];
	uint32_t seed = 1;
	int32_t shaped = 0;
	bool identical = true;
	unsigned frame;

	CASE(speech ? "oracle encoder speech shaped 100 frames" :
	     "oracle encoder two tone 100 frames");
	CHECK(codec != NULL);
	CHECK(oracle != NULL);
	for (frame = 0; frame < TEST_FRAMES; frame++) {
		if (speech)
			speech_fill(pcm, &seed, &shaped);
		else
			tone_fill(pcm, frame * TEST_FRAME, 1000.0, 5000.0);
		CHECK_UINT(udsp_codec_g722_ops.encode(codec, pcm, TEST_FRAME,
						      actual, sizeof(actual)), TEST_BYTES);
		CHECK_UINT(g722_encode(oracle, expected, pcm, TEST_FRAME), TEST_BYTES);
		if (memcmp(actual, expected, sizeof(actual)) != 0)
			identical = false;
	}
	CHECK(identical);
	g722_encode_free(oracle);
	udsp_codec_g722_ops.destroy(codec);
}

static void oracle_decoder_check(void)
{
	struct udsp_codec *encoder = codec_create();
	struct udsp_codec *decoder = codec_create();
	g722_decode_state_t *oracle = g722_decode_init(NULL, 64000, G722_PACKED);
	int16_t pcm[TEST_FRAME];
	int16_t actual[TEST_SAMPLES];
	int16_t expected[TEST_SAMPLES];
	uint8_t payload[TEST_BYTES];
	unsigned frame;
	double snr;

	CASE("oracle decoder aligned snr");
	CHECK(encoder != NULL);
	CHECK(decoder != NULL);
	CHECK(oracle != NULL);
	for (frame = 0; frame < TEST_FRAMES; frame++) {
		tone_fill(pcm, frame * TEST_FRAME, 1000.0, 5000.0);
		udsp_codec_g722_ops.encode(encoder, pcm, TEST_FRAME,
					    payload, sizeof(payload));
		udsp_codec_g722_ops.decode(decoder, payload, sizeof(payload),
					    actual + frame * TEST_FRAME, TEST_FRAME);
		g722_decode(oracle, expected + frame * TEST_FRAME,
			    payload, sizeof(payload));
	}
	snr = snr_get(expected, actual + PLC_DELAY, TEST_SAMPLES - PLC_DELAY);
	printf("# oracle decoder aligned snr %.2f dB\n", snr);
	CHECK(snr > 30.0);
	g722_decode_free(oracle);
	udsp_codec_g722_ops.destroy(decoder);
	udsp_codec_g722_ops.destroy(encoder);
}

static void oracle_full_scale_check(double frequency, double level_dbov,
				    const char *name)
{
	struct udsp_codec *encoder = codec_create();
	struct udsp_codec *decoder = codec_create();
	g722_decode_state_t *oracle = g722_decode_init(NULL, 64000, G722_PACKED);
	static int16_t actual[TEST_SAMPLES];
	static int16_t expected[TEST_SAMPLES];
	int16_t pcm[TEST_FRAME];
	uint8_t payload[TEST_BYTES];
	unsigned mismatches;
	unsigned frame;

	CASE(name);
	CHECK(encoder != NULL);
	CHECK(decoder != NULL);
	CHECK(oracle != NULL);
	for (frame = 0; frame < TEST_FRAMES; frame++) {
		tone_level_fill(pcm, frame * TEST_FRAME, frequency, level_dbov);
		udsp_codec_g722_ops.encode(encoder, pcm, TEST_FRAME,
					    payload, sizeof(payload));
		udsp_codec_g722_ops.decode(decoder, payload, sizeof(payload),
					    actual + frame * TEST_FRAME, TEST_FRAME);
		g722_decode(oracle, expected + frame * TEST_FRAME,
			    payload, sizeof(payload));
	}
	mismatches = sample_mismatch_count(expected, actual + PLC_DELAY,
					   TEST_SAMPLES - PLC_DELAY);
	printf("# %s mismatches %u/%u\n", name, mismatches,
	       TEST_SAMPLES - PLC_DELAY);
	CHECK_UINT(mismatches, 0);
	g722_decode_free(oracle);
	udsp_codec_g722_ops.destroy(decoder);
	udsp_codec_g722_ops.destroy(encoder);
}
#endif

static double round_trip_run(double frequency, double amplitude,
			     int16_t *output)
{
	struct udsp_codec *encoder = codec_create();
	struct udsp_codec *decoder = codec_create();
	static int16_t input[TEST_SAMPLES];
	int16_t frame_pcm[TEST_FRAME];
	uint8_t payload[TEST_BYTES];
	unsigned frame;
	double snr;

	for (frame = 0; frame < TEST_FRAMES; frame++) {
		tone_amplitude_fill(frame_pcm, frame * TEST_FRAME, frequency,
				    amplitude);
		memcpy(input + frame * TEST_FRAME, frame_pcm, sizeof(frame_pcm));
		udsp_codec_g722_ops.encode(encoder, frame_pcm, TEST_FRAME,
					    payload, sizeof(payload));
		udsp_codec_g722_ops.decode(decoder, payload, sizeof(payload),
					    output + frame * TEST_FRAME, TEST_FRAME);
	}
	snr = aligned_snr_get(input, output, TEST_SAMPLES);
	udsp_codec_g722_ops.destroy(decoder);
	udsp_codec_g722_ops.destroy(encoder);
	return snr;
}

static void round_trip_check(void)
{
	static int16_t output[TEST_SAMPLES];
	double snr;

	CASE("round trip 1 khz snr");
	snr = round_trip_run(1000.0, 8000.0, output);
	printf("# round trip 1 khz snr %.2f dB\n", snr);
	CHECK(snr > 25.0);

	CASE("round trip 5 khz snr");
	snr = round_trip_run(5000.0, 8000.0, output);
	printf("# round trip 5 khz snr %.2f dB\n", snr);
	CHECK(snr > 20.0);
}

static int step_max_get(const int16_t *pcm, unsigned count)
{
	int largest = 0;
	unsigned i;

	for (i = 1; i < count; i++) {
		int step = abs(pcm[i] - pcm[i - 1]);

		if (step > largest)
			largest = step;
	}
	return largest;
}

static void full_scale_check(void)
{
	static int16_t output[TEST_SAMPLES];
	double snr;
	int step;

	/*
	 * A wrapped QMF sample jumps by about the whole 16-bit range in one
	 * step and destroys the tone. An ideal 1 kHz tone at 16 kHz steps by
	 * 12785 counts at most, and the coding noise adds little, so both
	 * bounds separate the clamp from the wrap.
	 */
	CASE("decoder full scale saturates, never wraps");
	snr = round_trip_run(1000.0, 32767.0, output);
	step = step_max_get(output + PLC_DELAY, TEST_SAMPLES - PLC_DELAY);
	printf("# full scale 1 khz snr %.2f dB, largest step %d\n", snr, step);
	CHECK(snr > 40.0);
	CHECK(step < 20000);
}

static void wideband_check(void)
{
	static int16_t output[TEST_SAMPLES];
	double cosine = 0.0;
	double sine = 0.0;
	double total = 0.0;
	double component;
	double floor;
	unsigned i;

	round_trip_run(6800.0, 8000.0, output);
	for (i = 400; i < TEST_SAMPLES; i++) {
		double phase = 2.0 * M_PI * 6800.0 * (i - 60) / 16000.0;

		cosine += output[i] * cos(phase);
		sine += output[i] * sin(phase);
		total += (double)output[i] * output[i];
	}
	component = 2.0 * (cosine * cosine + sine * sine) /
		(TEST_SAMPLES - 400);
	floor = total - component;
	CASE("round trip 6.8 khz above noise floor");
	printf("# round trip 6.8 khz component %.2f dB above residual\n",
	       10.0 * log10(component / floor));
	CHECK(component > 0.0);
	CHECK(floor > 0.0);
	CHECK(10.0 * log10(component / floor) > 0.0);
}

static void errors_check(void)
{
	struct udsp_codec *codec = codec_create();
	struct udsp_codec *clean = codec_create();
	int16_t pcm[TEST_FRAME] = { 0 };
	uint8_t payload[TEST_BYTES + 1] = { 0 };
	uint8_t actual[TEST_BYTES];
	uint8_t expected[TEST_BYTES];

	CASE("v2 error edges");
	CHECK_UINT(udsp_codec_g722_ops.encode(codec, pcm, 161, payload,
					    TEST_BYTES), -EINVAL);
	CHECK_UINT(udsp_codec_g722_ops.encode(codec, pcm, TEST_FRAME, payload,
					    0), -ENOBUFS);
	CHECK_UINT(udsp_codec_g722_ops.decode(codec, payload, 81, pcm,
					    TEST_FRAME), -EINVAL);
	CHECK_UINT(udsp_codec_g722_ops.decode(codec, payload, TEST_BYTES, pcm,
					    0), -ENOBUFS);
	CHECK_UINT(udsp_codec_g722_ops.plc(codec, pcm, 161, NULL, 0), -EINVAL);

	CASE("rejected calls preserve encoder state");
	tone_fill(pcm, 0, 1000.0, 5000.0);
	CHECK_UINT(udsp_codec_g722_ops.encode(codec, pcm, TEST_FRAME,
					    actual, sizeof(actual)), TEST_BYTES);
	CHECK_UINT(udsp_codec_g722_ops.encode(clean, pcm, TEST_FRAME,
					    expected, sizeof(expected)), TEST_BYTES);
	CHECK(memcmp(actual, expected, sizeof(actual)) == 0);
	udsp_codec_g722_ops.destroy(clean);
	udsp_codec_g722_ops.destroy(codec);
}

static uint64_t energy_get(const int16_t *pcm)
{
	uint64_t energy = 0;
	unsigned i;

	for (i = 0; i < TEST_FRAME; i++)
		energy += (int64_t)pcm[i] * pcm[i];
	return energy;
}

static void plc_check(void)
{
	struct udsp_codec *encoder = codec_create();
	struct udsp_codec *decoder = codec_create();
	int16_t pcm[TEST_FRAME];
	uint8_t payload[TEST_BYTES];
	uint64_t energy[7];
	unsigned frame;

	CASE("plc continuity and decay");
	for (frame = 0; frame < 10; frame++) {
		tone_fill(pcm, frame * TEST_FRAME, 200.0, 0.0);
		udsp_codec_g722_ops.encode(encoder, pcm, TEST_FRAME,
					    payload, sizeof(payload));
		udsp_codec_g722_ops.decode(decoder, payload, TEST_BYTES,
					    pcm, TEST_FRAME);
	}
	for (frame = 0; frame < 7; frame++) {
		CHECK_UINT(udsp_codec_g722_ops.plc(decoder, pcm, TEST_FRAME,
						   NULL, 0), TEST_FRAME);
		energy[frame] = energy_get(pcm);
	}
	CHECK(energy[0] > 0);
	CHECK(energy[5] < energy[0]);
	CHECK(energy[5] < energy[2]);

	CASE("seventh erasure arms silence");
	CHECK(energy[5] > 0);
	CHECK(energy[6] > 0);
	for (frame = PLC_DELAY; frame < TEST_FRAME; frame++)
		CHECK_UINT(pcm[frame], 0);
	udsp_codec_g722_ops.destroy(decoder);
	udsp_codec_g722_ops.destroy(encoder);
}

static void plc_delay_check(void)
{
	struct udsp_plc plc;
	int16_t pcm[TEST_FRAME];
	unsigned leading_zeros = 0;
	unsigned i;

	for (i = 0; i < TEST_FRAME; i++)
		pcm[i] = 1;
	udsp_plc_init(&plc, 16000, TEST_FRAME);
	udsp_plc_frame_process(&plc, pcm);
	while (leading_zeros < TEST_FRAME && !pcm[leading_zeros])
		leading_zeros++;
	CASE("fresh decoder PLC delay is 60 samples");
	CHECK_UINT(leading_zeros, PLC_DELAY);
}

static void plc_before_decode_check(void)
{
	struct udsp_codec *decoder = codec_create();
	int16_t pcm[TEST_FRAME];

	memset(pcm, 0x5a, sizeof(pcm));
	CASE("PLC before decode produces silence");
	CHECK_UINT(udsp_codec_g722_ops.plc(decoder, pcm, TEST_FRAME,
					   NULL, 0), TEST_FRAME);
	CHECK_UINT(energy_get(pcm), 0);
	udsp_codec_g722_ops.destroy(decoder);
}

static void plc_recovery_check(void)
{
	struct udsp_codec *encoder = codec_create();
	struct udsp_codec *decoder = codec_create();
	struct udsp_codec *reference = codec_create();
	int16_t recovered[TEST_FRAME];
	int16_t expected[TEST_FRAME];
	int16_t pcm[TEST_FRAME];
	uint8_t payload[TEST_BYTES];
	unsigned differences = 0;
	unsigned frame;
	unsigned i;

	for (frame = 0; frame < 10; frame++) {
		tone_fill(pcm, frame * TEST_FRAME, 200.0, 0.0);
		udsp_codec_g722_ops.encode(encoder, pcm, TEST_FRAME,
					    payload, sizeof(payload));
		udsp_codec_g722_ops.decode(decoder, payload, TEST_BYTES,
					    recovered, TEST_FRAME);
		udsp_codec_g722_ops.decode(reference, payload, TEST_BYTES,
					    expected, TEST_FRAME);
	}
	udsp_codec_g722_ops.plc(decoder, recovered, TEST_FRAME, NULL, 0);
	tone_fill(pcm, frame * TEST_FRAME, 200.0, 0.0);
	udsp_codec_g722_ops.encode(encoder, pcm, TEST_FRAME,
				    payload, sizeof(payload));
	udsp_codec_g722_ops.decode(decoder, payload, TEST_BYTES,
				    recovered, TEST_FRAME);
	udsp_codec_g722_ops.decode(reference, payload, TEST_BYTES,
				    expected, TEST_FRAME);
	for (i = PLC_DELAY; i < PLC_DELAY + UDSP_PLC_QUARTER_MAX; i++) {
		if (recovered[i] != expected[i])
			differences++;
	}
	CASE("decode after loss applies recovery cross fade");
	CHECK(differences > 0);
	CHECK(memcmp(recovered + PLC_DELAY + UDSP_PLC_QUARTER_MAX,
		     expected + PLC_DELAY + UDSP_PLC_QUARTER_MAX,
		     (TEST_FRAME - PLC_DELAY - UDSP_PLC_QUARTER_MAX) *
		     sizeof(recovered[0])) == 0);
	udsp_codec_g722_ops.destroy(reference);
	udsp_codec_g722_ops.destroy(decoder);
	udsp_codec_g722_ops.destroy(encoder);
}

static void plc_geometry_check(void)
{
	struct udsp_plc plc;

	udsp_plc_init(&plc, 96000, 960);
	CASE("PLC geometry clamps to named maxima");
	CHECK_UINT(plc.geometry.frame, UDSP_PLC_FRAME_MAX);
	CHECK(plc.geometry.pitch_max <= UDSP_PLC_QUARTER_MAX * 4);
	CHECK(plc.geometry.history_len <= UDSP_PLC_HISTORY_MAX);
	CHECK(plc.geometry.corr_len <= plc.geometry.history_len);
	CHECK(plc.geometry.corr_buf <= plc.geometry.history_len);
}

static void cn_check(void)
{
	struct udsp_codec *first = codec_create();
	struct udsp_codec *second = codec_create();
	int16_t initial[TEST_FRAME];
	int16_t repeated[TEST_FRAME];
	int16_t continued[TEST_FRAME];

	udsp_codec_g722_ops.cn(first, 20, initial, TEST_FRAME);
	udsp_codec_g722_ops.cn(second, 20, repeated, TEST_FRAME);
	CASE("g722 comfort noise uses codec seed");
	CHECK(memcmp(initial, repeated, sizeof(initial)) == 0);
	udsp_codec_g722_ops.reset(first);
	udsp_codec_g722_ops.cn(first, 20, continued, TEST_FRAME);
	CHECK(memcmp(initial, continued, sizeof(initial)) != 0);
	udsp_codec_g722_ops.destroy(second);
	udsp_codec_g722_ops.destroy(first);
}

static void reset_check(void)
{
	struct udsp_codec *codec = codec_create();
	int16_t pcm[TEST_FRAME];
	uint8_t first[TEST_BYTES];
	uint8_t second[TEST_BYTES];

	tone_fill(pcm, 0, 1000.0, 5000.0);
	udsp_codec_g722_ops.encode(codec, pcm, TEST_FRAME, first, sizeof(first));
	udsp_codec_g722_ops.reset(codec);
	udsp_codec_g722_ops.encode(codec, pcm, TEST_FRAME, second, sizeof(second));
	CASE("reset restores initial state");
	CHECK(memcmp(first, second, sizeof(first)) == 0);
	udsp_codec_g722_ops.destroy(codec);
}

static void metadata_check(void)
{
	struct udsp_codec *codec = codec_create();

	CASE("v2 ops metadata");
	CHECK_UINT(udsp_codec_g722_ops.id, UDSP_CODEC_G722);
	CHECK(strcmp(udsp_codec_g722_ops.name, "g722") == 0);
	CHECK_UINT(udsp_codec_g722_ops.sample_rate, 16000);
	CHECK_UINT(udsp_codec_g722_ops.clock_rate, 8000);
	CHECK_UINT(udsp_codec_g722_ops.frame_ms, 10);
	CHECK_UINT(udsp_codec_g722_ops.ptime_mask, 0x07);
	CHECK_UINT(udsp_codec_g722_ops.max_payload, 240);
	CHECK_UINT(udsp_codec_g722_ops.flags, UDSP_CODEC_F_PLC);
	CHECK_UINT(udsp_codec_g722_ops.rate_get(codec), 16000);
	udsp_codec_g722_ops.destroy(codec);
}

int main(void)
{
	metadata_check();
	vectors_check();
	predictor_check();
#ifdef HAVE_G722_ORACLE
	oracle_encoder_check(false);
	oracle_encoder_check(true);
	oracle_decoder_check();
	oracle_full_scale_check(1000.0, 0.0,
		"oracle decoder 1 khz 0 dbov bit identity");
	oracle_full_scale_check(1000.0, -1.0,
		"oracle decoder 1 khz -1 dbov bit identity");
	oracle_full_scale_check(5000.0, 0.0,
		"oracle decoder 5 khz 0 dbov bit identity");
	oracle_full_scale_check(5000.0, -1.0,
		"oracle decoder 5 khz -1 dbov bit identity");
#endif
	round_trip_check();
	full_scale_check();
	wideband_check();
	errors_check();
	plc_check();
	plc_delay_check();
	plc_before_decode_check();
	plc_recovery_check();
	plc_geometry_check();
	cn_check();
	reset_check();
	return tu_summary("g722");
}
