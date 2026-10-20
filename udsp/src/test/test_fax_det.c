// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_SPANDSP
#include <spandsp.h>
#endif

#include "../dsp/fax_det.h"
#include "util.h"

#define LOG_MAX 32U

struct result_log {
	struct udsp_fax_result result[LOG_MAX];
	size_t batch_size[LOG_MAX];
	size_t count;
};

static double phase_wrap(double phase)
{
	while (phase >= 2.0 * M_PI)
		phase -= 2.0 * M_PI;
	return phase;
}

static double level_amplitude_get(int level_dbm0)
{
	return 32767.0 * pow(10.0, (-3.14 + level_dbm0) / 20.0);
}

static void log_feed(struct udsp_fax_detector *detector,
		     const int16_t *samples, struct result_log *log)
{
	struct udsp_fax_result results[UDSP_FAX_RESULT_MAX];
	size_t count;

	count = udsp_fax_detector_process(detector, samples, results,
					  UDSP_FAX_RESULT_MAX);
	CHECK(log->count + count <= LOG_MAX);
	if (log->count + count > LOG_MAX)
		return;
	memcpy(log->result + log->count, results, count * sizeof(results[0]));
	for (size_t i = 0; i < count; i++)
		log->batch_size[log->count + i] = count;
	log->count += count;
}

static void log_flush(struct udsp_fax_detector *detector,
		      struct result_log *log)
{
	struct udsp_fax_result results[UDSP_FAX_RESULT_MAX];
	size_t count;

	count = udsp_fax_detector_flush(detector, results, UDSP_FAX_RESULT_MAX);
	CHECK(log->count + count <= LOG_MAX);
	if (log->count + count > LOG_MAX)
		return;
	memcpy(log->result + log->count, results, count * sizeof(results[0]));
	log->count += count;
}

static void silence_feed(struct udsp_fax_detector *detector,
			 struct result_log *log, unsigned frames)
{
	int16_t samples[UDSP_FRAME_SAMPLES] = { 0 };
	unsigned frame;

	for (frame = 0; frame < frames; frame++)
		log_feed(detector, samples, log);
}

static void result_pair_check(const struct result_log *log, uint16_t tone,
			      uint32_t start_min, uint32_t start_max,
			      uint32_t end_min, uint32_t end_max)
{
	CHECK_UINT(log->count, 2);
	if (log->count != 2) {
		size_t i;

		for (i = 0; i < log->count; i++)
			printf("# result %zu tone %u start %u duration %u\n", i,
			       log->result[i].tone, log->result[i].start,
			       log->result[i].duration_ms);
		return;
	}
	CHECK_UINT(log->result[0].tone, tone);
	CHECK(log->result[0].start);
	CHECK(log->result[0].duration_ms >= start_min);
	CHECK(log->result[0].duration_ms <= start_max);
	CHECK_UINT(log->result[1].tone, tone);
	CHECK(!log->result[1].start);
	CHECK(log->result[1].duration_ms >= end_min);
	CHECK(log->result[1].duration_ms <= end_max);
	CHECK_UINT(log->result[0].direction, UDSP_TONE_DIR_REMOTE);
	CHECK_UINT(log->result[1].direction, UDSP_TONE_DIR_REMOTE);
}

static void sine_frames_feed(struct udsp_fax_detector *detector,
			     struct result_log *log, double frequency,
			     double amplitude, unsigned frames,
			     int dropout_frame)
{
	double phase = 0.0;
	unsigned frame;

	for (frame = 0; frame < frames; frame++) {
		int16_t samples[UDSP_FRAME_SAMPLES];
		unsigned i;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			samples[i] = frame == (unsigned)dropout_frame ? 0 :
				(int16_t)lrint(amplitude * sin(phase));
			phase = phase_wrap(phase + 2.0 * M_PI * frequency /
					   UDSP_SAMPLE_RATE);
		}
		log_feed(detector, samples, log);
	}
}

static bool single_tone_matches(double frequency, uint16_t expected_tone,
				unsigned frames, int dropout_frame)
{
	struct udsp_fax_detector detector;
	struct result_log log = { 0 };

	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	sine_frames_feed(&detector, &log, frequency, level_amplitude_get(-15),
			 frames, dropout_frame);
	silence_feed(&detector, &log, 4);
	if (log.count == 2 && log.result[0].tone == expected_tone &&
	    log.result[0].start && log.result[1].tone == expected_tone &&
	    !log.result[1].start)
		return true;
	printf("# %.0f Hz produced", frequency);
	for (size_t i = 0; i < log.count; i++)
		printf(" %u/%u", log.result[i].tone, log.result[i].start);
	printf("\n");
	return false;
}

static double sweep_frequency_get(double start_frequency,
				  double end_frequency,
				  uint64_t sample_index,
				  uint64_t sample_count)
{
	return start_frequency + (end_frequency - start_frequency) *
		sample_index / (sample_count - 1U);
}

static void sweep_frame_get(int16_t *samples, double *phase,
			    uint64_t *sample_index, uint64_t sample_count,
			    double start_frequency, double end_frequency)
{
	unsigned i;

	for (i = 0; i < UDSP_FRAME_SAMPLES; i++, (*sample_index)++) {
		double frequency = sweep_frequency_get(start_frequency,
			end_frequency, *sample_index, sample_count);

		samples[i] = (int16_t)lrint(level_amplitude_get(-15) *
					 sin(*phase));
		*phase = phase_wrap(*phase + 2.0 * M_PI * frequency /
					UDSP_SAMPLE_RATE);
	}
}

static bool modem_candidate_bound_check(void)
{
	const unsigned sweep_frames = 120U;
	const uint64_t sample_count = sweep_frames * UDSP_FRAME_SAMPLES;
	struct udsp_fax_detector detector;
	double phase = 0.0;
	uint64_t sample_index = 0;
	unsigned frame;

	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	for (frame = 0; frame < sweep_frames; frame++) {
		struct udsp_fax_result results[UDSP_FAX_RESULT_MAX];
		int16_t samples[UDSP_FRAME_SAMPLES];
		size_t count;
		size_t i;

		sweep_frame_get(samples, &phase, &sample_index, sample_count,
				1500.0, 1700.0);
		count = udsp_fax_detector_process(&detector, samples, results,
						  UDSP_FAX_RESULT_MAX);
		for (i = 0; i < count; i++) {
			if (results[i].tone == UDSP_TONE_DET_MODEM &&
			    results[i].start)
				return false;
		}
	}
	return true;
}

static bool modem_active_bound_check(void)
{
	const unsigned sweep_frames = 1000U;
	const uint64_t sample_count = sweep_frames * UDSP_FRAME_SAMPLES;
	struct udsp_fax_detector detector;
	double phase = 0.0;
	uint64_t sample_index = 0;
	bool started = false;
	unsigned frame;

	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	for (frame = 0; frame < sweep_frames; frame++) {
		struct udsp_fax_result results[UDSP_FAX_RESULT_MAX];
		int16_t samples[UDSP_FRAME_SAMPLES];
		size_t count;
		size_t i;

		sweep_frame_get(samples, &phase, &sample_index, sample_count,
				1500.0, 1700.0);
		count = udsp_fax_detector_process(&detector, samples, results,
						  UDSP_FAX_RESULT_MAX);
		for (i = 0; i < count; i++) {
			if (results[i].tone != UDSP_TONE_DET_MODEM)
				continue;
			if (results[i].start)
				started = true;
			else if (started)
				return true;
		}
	}
	return false;
}

static void modem_sweep_test(void)
{
	const unsigned sweep_frames = 2000U;
	const uint64_t sweep_samples = sweep_frames * UDSP_FRAME_SAMPLES;
	struct udsp_fax_detector detector;
	double phase = 0.0;
	uint32_t modem_end_frame = UINT32_MAX;
	uint32_t ced_frame = UINT32_MAX;
	bool modem_started = false;
	bool modem_ended = false;
	bool ced_reported = false;
	double ced_frequency = 0.0;
	uint64_t sample_index = 0;
	unsigned frame;

	CASE("modem swept tone releases the band");
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	for (frame = 0; frame < sweep_frames; frame++) {
		struct udsp_fax_result results[UDSP_FAX_RESULT_MAX];
		int16_t samples[UDSP_FRAME_SAMPLES];
		double frame_frequency;
		size_t count;
		size_t result_index;

		sweep_frame_get(samples, &phase, &sample_index, sweep_samples,
				2050.0, 2150.0);
		frame_frequency = sweep_frequency_get(
			2050.0, 2150.0,
			(frame + 1U) * UDSP_FRAME_SAMPLES,
			sweep_samples);
		count = udsp_fax_detector_process(&detector, samples, results,
						  UDSP_FAX_RESULT_MAX);
		for (result_index = 0; result_index < count; result_index++) {
			if (results[result_index].tone == UDSP_TONE_DET_MODEM &&
			    results[result_index].start)
				modem_started = true;
			if (results[result_index].tone == UDSP_TONE_DET_MODEM &&
			    !results[result_index].start && !modem_ended) {
				modem_ended = true;
				modem_end_frame = frame;
			}
			if (results[result_index].tone != UDSP_TONE_DET_CED ||
			    !results[result_index].start || ced_reported)
				continue;
			ced_reported = true;
			ced_frame = frame;
			ced_frequency = frame_frequency;
		}
	}
	CHECK(modem_candidate_bound_check());
	CHECK(modem_active_bound_check());
	CHECK(modem_started);
	CHECK(modem_ended);
	CHECK(ced_reported);
	if (ced_reported &&
	    (ced_frequency < 2072.0 || ced_frequency > 2128.0))
		printf("# CED reported at %.2f Hz\n", ced_frequency);
	CHECK(ced_frequency >= 2072.0 && ced_frequency <= 2128.0);
	if (modem_ended && ced_reported)
		CHECK(modem_end_frame <= ced_frame);
}

static void cng_test(void)
{
	struct udsp_fax_detector detector;
	struct result_log log;
	const double edge_frequencies[] = { 1060.0, 1140.0 };
	unsigned i;

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("fax CNG minus 43 dBm0 cadence with one-frame dropout");
	sine_frames_feed(&detector, &log, 1100.0, level_amplitude_get(-43),
			 50, 25);
	silence_feed(&detector, &log, 300);
	result_pair_check(&log, UDSP_TONE_DET_CNG, 490, 520, 490, 520);

	for (i = 0; i < sizeof(edge_frequencies) /
	     sizeof(edge_frequencies[0]); i++) {
		char name[80];

		memset(&log, 0, sizeof(log));
		udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
		snprintf(name, sizeof(name), "fax CNG captures %.0f Hz",
			 edge_frequencies[i]);
		CASE(name);
		sine_frames_feed(&detector, &log, edge_frequencies[i],
			level_amplitude_get(-20), 50, -1);
		silence_feed(&detector, &log, 15);
		result_pair_check(&log, UDSP_TONE_DET_CNG, 500, 520,
			500, 520);
	}
}

static void answer_tone_feed(struct udsp_fax_detector *detector,
			     struct result_log *log, unsigned frames,
			     double modulation_hz,
			     unsigned reversal_ms, int dropout_frame)
{
	const double amplitude = level_amplitude_get(-12);
	uint64_t sample_index = 0;
	unsigned frame;

	for (frame = 0; frame < frames; frame++) {
		int16_t samples[UDSP_FRAME_SAMPLES];
		unsigned i;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++, sample_index++) {
			double envelope = 1.0;
			double phase = 2.0 * M_PI * 2100.0 * sample_index /
				UDSP_SAMPLE_RATE;

			if (modulation_hz > 0.0)
				envelope += 0.2 * sin(2.0 * M_PI * modulation_hz *
					sample_index / UDSP_SAMPLE_RATE);
			if (reversal_ms)
				phase += (sample_index / (reversal_ms * 8U)) * M_PI;
			samples[i] = frame == (unsigned)dropout_frame ? 0 :
				(int16_t)lrint(amplitude * envelope * sin(phase));
		}
		log_feed(detector, samples, log);
	}
}

static void answer_tests(void)
{
	struct udsp_fax_detector detector;
	struct result_log log;

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("fax CED 500 ms early report with one-frame dropout");
	answer_tone_feed(&detector, &log, 70, 0.0, 0, 30);
	silence_feed(&detector, &log, 15);
	result_pair_check(&log, UDSP_TONE_DET_CED, 500, 520, 700, 750);

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("modem ANSam 15 Hz discriminator with one-frame dropout");
	answer_tone_feed(&detector, &log, 80, 15.0, 0, 35);
	silence_feed(&detector, &log, 15);
	result_pair_check(&log, UDSP_TONE_DET_ANSAM, 500, 520, 800, 850);

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("modem ANS phase reversal 450 ms with one-frame dropout");
	answer_tone_feed(&detector, &log, 130, 15.0, 450, 60);
	silence_feed(&detector, &log, 15);
	result_pair_check(&log, UDSP_TONE_DET_ANSBAR, 890, 940, 1300, 1360);

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("modem ANSam 14 Hz tolerance");
	answer_tone_feed(&detector, &log, 70, 14.0, 0, -1);
	silence_feed(&detector, &log, 15);
	result_pair_check(&log, UDSP_TONE_DET_ANSAM, 500, 520, 700, 750);

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("modem ANSam 16 Hz tolerance");
	answer_tone_feed(&detector, &log, 70, 16.0, 0, -1);
	silence_feed(&detector, &log, 15);
	result_pair_check(&log, UDSP_TONE_DET_ANSAM, 500, 520, 700, 750);

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("modem pure ANS phase reversal 425 ms tolerance");
	answer_tone_feed(&detector, &log, 140, 0.0, 425, -1);
	silence_feed(&detector, &log, 15);
	result_pair_check(&log, UDSP_TONE_DET_ANSBAR, 830, 910, 1400, 1460);

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("modem pure ANS phase reversal 475 ms tolerance");
	answer_tone_feed(&detector, &log, 140, 0.0, 475, -1);
	silence_feed(&detector, &log, 15);
	result_pair_check(&log, UDSP_TONE_DET_ANSBAR, 930, 1010, 1400, 1460);
}

static void answer_gap_test(void)
{
	struct udsp_fax_detector detector;
	struct result_log log = { 0 };

	CASE("a CED survives a 20 ms gap without a second start");
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	answer_tone_feed(&detector, &log, 60, 0.0, 0, -1);
	CHECK_UINT(log.count, 1);
	silence_feed(&detector, &log, 2);
	CHECK_UINT(log.count, 1);
	answer_tone_feed(&detector, &log, 60, 0.0, 0, -1);
	CHECK_UINT(log.count, 1);
	silence_feed(&detector, &log, 3);
	result_pair_check(&log, UDSP_TONE_DET_CED, 500, 500, 1250, 1250);

	CASE("a 30 ms gap ends CED and the restart needs 500 ms");
	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	answer_tone_feed(&detector, &log, 60, 0.0, 0, -1);
	silence_feed(&detector, &log, 2);
	CHECK_UINT(log.count, 1);
	silence_feed(&detector, &log, 1);
	result_pair_check(&log, UDSP_TONE_DET_CED, 500, 500, 630, 630);
	answer_tone_feed(&detector, &log, 49, 0.0, 0, -1);
	CHECK_UINT(log.count, 2);
	answer_tone_feed(&detector, &log, 1, 0.0, 0, -1);
	CHECK_UINT(log.count, 3);
	CHECK_UINT(log.result[2].tone, UDSP_TONE_DET_CED);
	CHECK(log.result[2].start);
	CHECK_UINT(log.result[2].direction, UDSP_TONE_DIR_REMOTE);
	CHECK_UINT(log.result[2].duration_ms, 500);
}

static void answer_upgrade_check(double modulation_hz, unsigned reversal_ms,
				 uint16_t tone)
{
	struct udsp_fax_detector detector;
	struct result_log log = { 0 };

	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	answer_tone_feed(&detector, &log, 50, 0.0, 0, -1);
	CHECK_UINT(log.count, 1);
	CHECK_UINT(log.result[0].tone, UDSP_TONE_DET_CED);
	CHECK(log.result[0].start);
	answer_tone_feed(&detector, &log, 200, modulation_hz, reversal_ms, -1);
	CHECK_UINT(log.count, 3);
	CHECK_UINT(log.result[1].tone, UDSP_TONE_DET_CED);
	CHECK(!log.result[1].start);
	CHECK_UINT(log.result[2].tone, tone);
	CHECK(log.result[2].start);
	CHECK_UINT(log.result[1].direction, UDSP_TONE_DIR_REMOTE);
	CHECK_UINT(log.result[2].direction, UDSP_TONE_DIR_REMOTE);
	CHECK_UINT(log.batch_size[1], 2);
	CHECK_UINT(log.batch_size[2], 2);
	CHECK_UINT(log.result[1].duration_ms, log.result[2].duration_ms);
}

static void answer_upgrade_test(void)
{
	CASE("a CED to ANSam upgrade ends and starts in one batch");
	answer_upgrade_check(15.0, 0, UDSP_TONE_DET_ANSAM);
	CASE("a CED to ANSbar upgrade ends and starts in one batch");
	answer_upgrade_check(0.0, 450, UDSP_TONE_DET_ANSBAR);
}

static bool flag_bit_get(uint64_t bit_index)
{
	static const bool flag_bits[] = {
		false, true, true, true, true, true, true, false,
	};

	return flag_bits[bit_index % 8U];
}

static void v21_feed(struct udsp_fax_detector *detector,
		     struct result_log *log, unsigned frames, int dropout_frame)
{
	double phase = 0.0;
	uint64_t sample_index = 0;
	unsigned baud_accumulator = 0;
	uint64_t bit_index = 0;
	unsigned frame;

	for (frame = 0; frame < frames; frame++) {
		int16_t samples[UDSP_FRAME_SAMPLES];
		unsigned i;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++, sample_index++) {
			double frequency = flag_bit_get(bit_index) ? 1650.0 : 1850.0;

			samples[i] = frame == (unsigned)dropout_frame ? 0 :
				(int16_t)lrint(level_amplitude_get(-12) * sin(phase));
			phase = phase_wrap(phase + 2.0 * M_PI * frequency /
					   UDSP_SAMPLE_RATE);
			baud_accumulator += 300U;
			if (baud_accumulator < UDSP_SAMPLE_RATE)
				continue;
			baud_accumulator -= UDSP_SAMPLE_RATE;
			bit_index++;
		}
		log_feed(detector, samples, log);
	}
}

static void v21_test(void)
{
	struct udsp_fax_detector detector;
	struct result_log log = { 0 };

	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("fax V.21 channel 2 flags with one-frame dropout");
	v21_feed(&detector, &log, 140, 60);
	silence_feed(&detector, &log, 20);
	result_pair_check(&log, UDSP_TONE_DET_V21_PREAMBLE, 10, 120,
			  1350, 1550);
}

static void modem_test(void)
{
	struct udsp_fax_detector detector;
	struct result_log log;
	const double regression_frequencies[] = {
		1216.0, 1536.0, 1650.0, 1850.0, 2384.0, 2680.0,
	};
	const double legacy_frequencies[] = { 1200.0, 1525.0, 2700.0 };
	unsigned frequency;
	unsigned i;

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("long 1550 Hz modem tone with one-frame dropout");
	sine_frames_feed(&detector, &log, 1550.0, level_amplitude_get(-15),
			 130, 50);
	silence_feed(&detector, &log, 15);
	result_pair_check(&log, UDSP_TONE_DET_MODEM, 1010, 1030,
			  1300, 1350);

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("modem 1550 Hz dropout at threshold frame 100");
	sine_frames_feed(&detector, &log, 1550.0, level_amplitude_get(-15),
			 130, 100);
	silence_feed(&detector, &log, 15);
	result_pair_check(&log, UDSP_TONE_DET_MODEM, 1010, 1030,
			  1300, 1350);

	CASE("modem 1550 Hz dropout positions 97 through 103");
	for (i = 97; i <= 103; i++)
		CHECK(single_tone_matches(1550.0, UDSP_TONE_DET_MODEM,
					  110, (int)i));

	CASE("modem full band coverage in 8 Hz steps");
	for (frequency = 1200; frequency <= 2700; frequency += 8) {
		uint16_t expected = frequency >= 2072 && frequency <= 2128 ?
			UDSP_TONE_DET_CED : UDSP_TONE_DET_MODEM;

		CHECK(single_tone_matches(frequency, expected, 110, -1));
	}
	modem_sweep_test();

	for (i = 0; i < sizeof(regression_frequencies) /
	     sizeof(regression_frequencies[0]); i++) {
		char name[80];

		snprintf(name, sizeof(name), "modem regression %.0f Hz",
			 regression_frequencies[i]);
		CASE(name);
		CHECK(single_tone_matches(regression_frequencies[i],
					  UDSP_TONE_DET_MODEM, 110, -1));
	}

	for (i = 0; i < sizeof(legacy_frequencies) /
	     sizeof(legacy_frequencies[0]); i++) {
		char name[80];

		snprintf(name, sizeof(name), "modem band coverage %.0f Hz",
			 legacy_frequencies[i]);
		CASE(name);
		CHECK(single_tone_matches(legacy_frequencies[i],
					  UDSP_TONE_DET_MODEM, 110, -1));
	}
}

static uint32_t random_next(uint32_t *state)
{
	uint32_t value = *state;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	*state = value;
	return value;
}

static void noise_feed(struct udsp_fax_detector *detector,
		       struct result_log *log, unsigned frames)
{
	uint32_t random_state = 0x13579bdf;
	uint64_t sample_index = 0;
	unsigned frame;

	for (frame = 0; frame < frames; frame++) {
		int16_t samples[UDSP_FRAME_SAMPLES];
		unsigned i;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++, sample_index++) {
			double noise = (int32_t)random_next(&random_state) / 2147483648.0;
			double envelope = 0.55 + 0.45 * sin(2.0 * M_PI * 6.3 *
				sample_index / UDSP_SAMPLE_RATE);
			double speech = 2500.0 * sin(2.0 * M_PI * 173.0 * sample_index /
				UDSP_SAMPLE_RATE) + 1800.0 * sin(2.0 * M_PI * 347.0 *
				sample_index / UDSP_SAMPLE_RATE);

			samples[i] = (int16_t)lrint(envelope * speech + 2500.0 * noise);
		}
		log_feed(detector, samples, log);
	}
}

static void dtmf_feed(struct udsp_fax_detector *detector,
		      struct result_log *log, unsigned frames)
{
	const double row[] = { 697.0, 770.0, 852.0, 941.0 };
	const double column[] = { 1209.0, 1336.0, 1477.0, 1633.0 };
	uint64_t sample_index = 0;
	unsigned frame;

	for (frame = 0; frame < frames; frame++) {
		int16_t samples[UDSP_FRAME_SAMPLES];
		unsigned digit = (frame / 20U) % 16U;
		unsigned i;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++, sample_index++) {
			double first = sin(2.0 * M_PI * row[digit / 4U] * sample_index /
				UDSP_SAMPLE_RATE);
			double second = sin(2.0 * M_PI * column[digit % 4U] *
				sample_index / UDSP_SAMPLE_RATE);

			samples[i] = (int16_t)lrint(3500.0 * (first + second));
		}
		log_feed(detector, samples, log);
	}
}

static void cng_bad_cadence_feed(struct udsp_fax_detector *detector,
				 struct result_log *log, unsigned cycles)
{
	double phase = 0.0;
	unsigned frame;

	for (frame = 0; frame < cycles * 53U; frame++) {
		int16_t samples[UDSP_FRAME_SAMPLES];
		bool tone = frame % 53U < 50U;
		unsigned i;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			samples[i] = tone ? (int16_t)lrint(
				level_amplitude_get(-20) * sin(phase)) : 0;
			phase = phase_wrap(phase + 2.0 * M_PI * 1100.0 /
				UDSP_SAMPLE_RATE);
		}
		log_feed(detector, samples, log);
	}
}

static void negative_test(void)
{
	struct udsp_fax_detector detector;
	struct result_log log;

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("fax speech-like noise stays silent");
	noise_feed(&detector, &log, 300);
	log_flush(&detector, &log);
	CHECK_UINT(log.count, 0);

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("fax DTMF digits stay silent");
	dtmf_feed(&detector, &log, 300);
	log_flush(&detector, &log);
	CHECK_UINT(log.count, 0);

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("fax 1004 Hz milliwatt tone stays silent");
	sine_frames_feed(&detector, &log, 1004.0, level_amplitude_get(0), 300, -1);
	log_flush(&detector, &log);
	CHECK_UINT(log.count, 0);

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("fax 2100 Hz short burst stays silent");
	sine_frames_feed(&detector, &log, 2100.0, level_amplitude_get(-12), 40, -1);
	silence_feed(&detector, &log, 15);
	log_flush(&detector, &log);
	CHECK_UINT(log.count, 0);

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("fax CNG short burst cadence rejected");
	sine_frames_feed(&detector, &log, 1100.0, level_amplitude_get(-20), 20, -1);
	silence_feed(&detector, &log, 15);
	log_flush(&detector, &log);
	CHECK_UINT(log.count, 0);

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("fax uncadenced 1100 Hz stays silent");
	sine_frames_feed(&detector, &log, 1100.0, level_amplitude_get(-20), 300, -1);
	log_flush(&detector, &log);
	CHECK_UINT(log.count, 0);

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("fax CNG 500 ms on 30 ms off does not re-fire");
	cng_bad_cadence_feed(&detector, &log, 4);
	log_flush(&detector, &log);
	{
		unsigned starts = 0;
		size_t i;

		for (i = 0; i < log.count; i++) {
			CHECK_UINT(log.result[i].tone, UDSP_TONE_DET_CNG);
			if (log.result[i].start)
				starts++;
		}
		CHECK(starts <= 1U);
	}
}

/*
 * The hold energy is the measurement that the tone disabler of the engine
 * reads. The disabler holds while the energy is at or above
 * UDSP_FAX_HOLD_ENERGY of core/chan.c, 1.0e7, the energy of a -33 dBm0 frame.
 * The high-pass filter weakens a hum in the measurement. At -30 dBm0, a
 * 2100 Hz tone reads at or above the floor, and a 120 Hz hum reads under it.
 */
static void hold_energy_test(void)
{
	struct udsp_fax_detector detector;
	struct result_log log;

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("the hold energy answers the disabler's floor");
	sine_frames_feed(&detector, &log, 2100.0, level_amplitude_get(-12), 5, -1);
	CHECK(udsp_fax_detector_hold_energy_get(&detector) >= 1.0e7);
	silence_feed(&detector, &log, 5);
	CHECK(udsp_fax_detector_hold_energy_get(&detector) < 1.0e7);

	CASE("the hold energy leaves a hum below the disabler's floor");
	sine_frames_feed(&detector, &log, 2100.0, level_amplitude_get(-30), 5, -1);
	CHECK(udsp_fax_detector_hold_energy_get(&detector) >= 1.0e7);
	sine_frames_feed(&detector, &log, 120.0, level_amplitude_get(-30), 5, -1);
	CHECK(udsp_fax_detector_hold_energy_get(&detector) < 1.0e7);
	/* None of the bursts is long enough to be a tone, so the reading is
	 * the only thing these frames produced. */
	CHECK_UINT(log.count, 0);
}

/*
 * A frame under TONE_MIN_ENERGY reaches no verdict, because tone_dominant
 * refuses it whatever the banks read. The detector therefore does not run the
 * banks on such a frame. The test checks that these frames leave the detector
 * in the state that silence leaves. They give no report, and they still feed
 * the hold energy. The next CNG cadence at the threshold level reports on the
 * same schedule as after silence.
 */
static void quiet_frame_test(void)
{
	struct udsp_fax_detector detector;
	struct result_log log;

	memset(&log, 0, sizeof(log));
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	CASE("fax tones at -50 dBm0 stay silent and still feed the hold energy");
	sine_frames_feed(&detector, &log, 1100.0, level_amplitude_get(-50), 50, -1);
	CHECK(udsp_fax_detector_hold_energy_get(&detector) > 1.0e5);
	CHECK(udsp_fax_detector_hold_energy_get(&detector) < 1.0e7);
	silence_feed(&detector, &log, 300);
	sine_frames_feed(&detector, &log, 2100.0, level_amplitude_get(-50), 60, -1);
	sine_frames_feed(&detector, &log, 1550.0, level_amplitude_get(-50), 120, -1);
	CHECK_UINT(log.count, 0);

	CASE("fax CNG at the threshold level reports on schedule after quiet frames");
	sine_frames_feed(&detector, &log, 1100.0, level_amplitude_get(-43), 50, 25);
	silence_feed(&detector, &log, 300);
	result_pair_check(&log, UDSP_TONE_DET_CNG, 490, 520, 490, 520);
}

static void duration_saturation_test(void)
{
	struct udsp_fax_detector detector;
	struct udsp_fax_result result[UDSP_FAX_RESULT_MAX];
	size_t count;

	CASE("fax result duration saturates at uint16 maximum");
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	detector.active_tone = UDSP_TONE_DET_MODEM;
	detector.active_frames = 7000;
	count = udsp_fax_detector_flush(&detector, result,
					UDSP_FAX_RESULT_MAX);
	CHECK_UINT(count, 1);
	CHECK_UINT(result[0].duration_ms, UINT16_MAX);
}

#ifdef HAVE_SPANDSP
static void spandsp_tone_test(int source_tone, uint16_t expected_tone,
			      const char *name, unsigned frames)
{
	modem_connect_tones_tx_state_t *generator;
	struct udsp_fax_detector detector;
	struct result_log log = { 0 };
	unsigned frame;
	bool found = false;

	CASE(name);
	generator = modem_connect_tones_tx_init(NULL, source_tone);
	CHECK(generator != NULL);
	if (!generator)
		return;
	udsp_fax_detector_init(&detector, UDSP_TONE_DIR_REMOTE);
	for (frame = 0; frame < frames; frame++) {
		int16_t samples[UDSP_FRAME_SAMPLES] = { 0 };

		modem_connect_tones_tx(generator, samples, UDSP_FRAME_SAMPLES);
		log_feed(&detector, samples, &log);
	}
	silence_feed(&detector, &log, 15);
	for (frame = 0; frame < log.count; frame++) {
		CHECK_UINT(log.result[frame].tone, expected_tone);
		if (log.result[frame].tone == expected_tone &&
		    log.result[frame].start)
			found = true;
	}
	CHECK(found);
	modem_connect_tones_tx_free(generator);
}

static void spandsp_test(void)
{
	spandsp_tone_test(MODEM_CONNECT_TONES_FAX_CNG, UDSP_TONE_DET_CNG,
			   "spandsp CNG generator cross-check", 360);
	spandsp_tone_test(MODEM_CONNECT_TONES_FAX_CED, UDSP_TONE_DET_CED,
			   "spandsp CED generator cross-check", 340);
	spandsp_tone_test(MODEM_CONNECT_TONES_ANSAM, UDSP_TONE_DET_ANSAM,
			   "spandsp ANSam generator cross-check", 340);
	spandsp_tone_test(MODEM_CONNECT_TONES_ANS_PR, UDSP_TONE_DET_ANSBAR,
			   "spandsp ANS_PR generator cross-check", 340);
	spandsp_tone_test(MODEM_CONNECT_TONES_ANSAM_PR,
			   UDSP_TONE_DET_ANSBAR,
			   "spandsp ANSam_PR generator cross-check", 340);
}
#else
static void spandsp_test(void)
{
}
#endif

int main(void)
{
	cng_test();
	answer_tests();
	answer_gap_test();
	answer_upgrade_test();
	v21_test();
	modem_test();
	negative_test();
	hold_energy_test();
	quiet_frame_test();
	duration_saturation_test();
	spandsp_test();
	return tu_summary("fax_det");
}
