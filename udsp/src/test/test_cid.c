// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../dsp/cid.h"
#include "../dsp/dsp.h"
#include "util.h"

#define WAVE_SAMPLES_MAX 32768U
#define BITS_MAX 4096U
#define POSTAMBLE_BITS 2U

static size_t bit_boundary_get(size_t bit)
{
	return (bit * UDSP_SAMPLE_RATE + 1199U) / 1200U;
}

static bool bit_correlate(const int16_t *wave, size_t bit,
			  double mark_hz, double space_hz)
{
	size_t start = bit_boundary_get(bit);
	size_t end = bit_boundary_get(bit + 1U);
	double mark_i = 0.0;
	double mark_q = 0.0;
	double space_i = 0.0;
	double space_q = 0.0;
	size_t sample;

	for (sample = start; sample < end; sample++) {
		double mark_angle = 2.0 * M_PI * mark_hz * sample / UDSP_SAMPLE_RATE;
		double space_angle = 2.0 * M_PI * space_hz * sample / UDSP_SAMPLE_RATE;

		mark_i += wave[sample] * cos(mark_angle);
		mark_q += wave[sample] * sin(mark_angle);
		space_i += wave[sample] * cos(space_angle);
		space_q += wave[sample] * sin(space_angle);
	}
	return mark_i * mark_i + mark_q * mark_q >
		space_i * space_i + space_q * space_q;
}

static size_t wave_render(enum udsp_fsk_std standard, int level_dbm0,
			  unsigned seizure_bits, unsigned mark_bits,
			  const uint8_t *data, size_t data_length,
			  int16_t *wave)
{
	struct udsp_fsk_mod modulator;
	int16_t frame[UDSP_FRAME_SAMPLES];
	size_t frames = 0;
	size_t expected_bits = seizure_bits + mark_bits + data_length * 10U;
	size_t expected_samples;

	if (data_length)
		expected_bits += POSTAMBLE_BITS;
	expected_samples = bit_boundary_get(expected_bits);

	udsp_fsk_mod_init(&modulator, standard, level_dbm0, seizure_bits, mark_bits,
			  UDSP_SAMPLE_RATE);
	CHECK_UINT(udsp_fsk_mod_data(&modulator, data, data_length), data_length);
	while (frames * UDSP_FRAME_SAMPLES < WAVE_SAMPLES_MAX) {
		memset(frame, 0, sizeof(frame));
		if (!udsp_fsk_mod_run(&modulator, frame, UDSP_FRAME_SAMPLES))
			break;
		memcpy(wave + frames * UDSP_FRAME_SAMPLES, frame, sizeof(frame));
		frames++;
	}
	CHECK(frames * UDSP_FRAME_SAMPLES >= expected_samples);
	CHECK(frames * UDSP_FRAME_SAMPLES < WAVE_SAMPLES_MAX);
	return expected_samples;
}

static void fsk_demodulation_check(enum udsp_fsk_std standard, int level_dbm0,
				   const char *name)
{
	static const uint8_t input[] = { 0x01, 0x55, 0x80, 0xfe, 0x37 };
	const unsigned seizure_bits = 30;
	const unsigned mark_bits = 12;
	const double mark_hz = standard == UDSP_FSK_BELL202 ? 1200.0 : 1300.0;
	const double space_hz = standard == UDSP_FSK_BELL202 ? 2200.0 : 2100.0;
	int16_t wave[WAVE_SAMPLES_MAX];
	bool bits[BITS_MAX];
	size_t total_bits = seizure_bits + mark_bits + sizeof(input) * 10U +
		POSTAMBLE_BITS;
	size_t measured_seizure = 0;
	size_t measured_mark = 0;
	size_t sample_count;
	size_t bit;
	size_t byte;

	CASE(name);
	sample_count = wave_render(standard, level_dbm0, seizure_bits, mark_bits,
				   input, sizeof(input), wave);
	CHECK_UINT(sample_count, bit_boundary_get(total_bits));
	for (bit = 0; bit < total_bits; bit++)
		bits[bit] = bit_correlate(wave, bit, mark_hz, space_hz);
	while (measured_seizure < total_bits &&
	       bits[measured_seizure] == ((measured_seizure & 1U) != 0))
		measured_seizure++;
	while (measured_seizure + measured_mark < total_bits &&
	       bits[measured_seizure + measured_mark])
		measured_mark++;
	CHECK_UINT(measured_seizure, seizure_bits);
	CHECK_UINT(measured_mark, mark_bits);
	CHECK(bits[total_bits - 2U]);
	CHECK(bits[total_bits - 1U]);
	for (byte = 0; byte < sizeof(input); byte++) {
		size_t offset = seizure_bits + mark_bits + byte * 10U;
		uint8_t decoded = 0;
		unsigned data_bit;

		CHECK(!bits[offset]);
		CHECK(bits[offset + 9U]);
		for (data_bit = 0; data_bit < 8U; data_bit++)
			decoded |= bits[offset + data_bit + 1U] << data_bit;
		CHECK_UINT(decoded, input[byte]);
	}
}

static bool fsk_payload_matches(const uint8_t *input, size_t input_length)
{
	int16_t wave[WAVE_SAMPLES_MAX];
	size_t total_bits = input_length * 10U + POSTAMBLE_BITS;
	size_t byte;

	wave_render(UDSP_FSK_BELL202, -15, 0, 0, input, input_length, wave);
	for (byte = 0; byte < input_length; byte++) {
		size_t offset = byte * 10U;
		uint8_t decoded = 0;
		unsigned data_bit;

		if (bit_correlate(wave, offset, 1200.0, 2200.0))
			return false;
		if (!bit_correlate(wave, offset + 9U, 1200.0, 2200.0))
			return false;
		for (data_bit = 0; data_bit < 8U; data_bit++)
			decoded |= bit_correlate(wave, offset + data_bit + 1U,
						  1200.0, 2200.0) << data_bit;
		if (decoded != input[byte])
			return false;
	}
	return bit_boundary_get(total_bits) <= WAVE_SAMPLES_MAX;
}

static void fsk_test(void)
{
	const int levels[] = { -8, -12, -16, -20 };
	unsigned standard;
	unsigned level;

	for (standard = UDSP_FSK_BELL202; standard <= UDSP_FSK_V23; standard++) {
		for (level = 0; level < sizeof(levels) / sizeof(levels[0]); level++) {
			char name[96];

			snprintf(name, sizeof(name), "%s independent demodulation %d dBm0",
				 standard == UDSP_FSK_BELL202 ? "Bell 202" : "V.23 mode 2",
				 levels[level]);
			fsk_demodulation_check(standard, levels[level], name);
		}
	}

	CASE("FSK queue closes after first run");
	{
		struct udsp_fsk_mod modulator;
		const uint8_t byte = 0x55;
		int16_t frame[UDSP_FRAME_SAMPLES] = { 0 };

		udsp_fsk_mod_init(&modulator, UDSP_FSK_BELL202, -10, 0, 0,
				  UDSP_SAMPLE_RATE);
		CHECK_UINT(udsp_fsk_mod_data(&modulator, &byte, 1), 1);
		CHECK(udsp_fsk_mod_run(&modulator, frame, UDSP_FRAME_SAMPLES));
		CHECK_UINT(udsp_fsk_mod_data(&modulator, &byte, 1), 0);
	}

	CASE("FSK completed run leaves frame untouched");
	{
		struct udsp_fsk_mod modulator;
		const uint8_t byte = 0xa5;
		int16_t frame[UDSP_FRAME_SAMPLES];
		int16_t expected[UDSP_FRAME_SAMPLES];
		unsigned i;

		udsp_fsk_mod_init(&modulator, UDSP_FSK_BELL202, -10, 0, 0,
				  UDSP_SAMPLE_RATE);
		CHECK_UINT(udsp_fsk_mod_data(&modulator, &byte, 1), 1);
		do {
			memset(frame, 0, sizeof(frame));
		} while (udsp_fsk_mod_run(&modulator, frame, UDSP_FRAME_SAMPLES));
		for (i = 0; i < UDSP_FRAME_SAMPLES; i++)
			frame[i] = (int16_t)(1000 + i);
		memcpy(expected, frame, sizeof(expected));
		CHECK(!udsp_fsk_mod_run(&modulator, frame, UDSP_FRAME_SAMPLES));
		CHECK(memcmp(frame, expected, sizeof(frame)) == 0);
	}

	CASE("FSK saturating additive render");
	{
		struct udsp_fsk_mod positive;
		struct udsp_fsk_mod negative;
		int16_t high[UDSP_FRAME_SAMPLES];
		int16_t low[UDSP_FRAME_SAMPLES];
		bool high_saturated = false;
		bool low_saturated = false;
		unsigned i;

		for (i = 0; i < UDSP_FRAME_SAMPLES; i++) {
			high[i] = INT16_MAX - 8;
			low[i] = INT16_MIN;
		}
		udsp_fsk_mod_init(&positive, UDSP_FSK_BELL202, 0, 0, 10,
				  UDSP_SAMPLE_RATE);
		udsp_fsk_mod_init(&negative, UDSP_FSK_BELL202, 0, 1, 9,
				  UDSP_SAMPLE_RATE);
		CHECK(udsp_fsk_mod_run(&positive, high, UDSP_FRAME_SAMPLES));
		CHECK(udsp_fsk_mod_run(&negative, low, UDSP_FRAME_SAMPLES));
		CHECK_UINT(high[1], INT16_MAX);
		for (i = 1; i < UDSP_FRAME_SAMPLES; i++) {
			high_saturated |= high[i] == INT16_MAX;
			low_saturated |= low[i] == INT16_MIN;
		}
		CHECK(high_saturated);
		CHECK(low_saturated);
	}
}

static struct udsp_cid fixed_cid_get(void)
{
	struct udsp_cid cid = { 0 };

	memcpy(cid.number, "5551234", 8);
	memcpy(cid.name, "TEST", 5);
	memcpy(cid.datetime, "08241200", 9);
	return cid;
}

static void cid_vectors_test(void)
{
	struct udsp_cid cid = fixed_cid_get();
	uint8_t output[UDSP_CID_MSG_MAX];
	size_t length;
	/* Sum: 0x80 + 0x19 + date 0x19a + CLI 0x172 + name 0x14b = 0x4f0. */
	static const uint8_t mdmf_expected[] = {
		0x80, 0x19, 0x01, 0x08, '0', '8', '2', '4', '1', '2', '0', '0',
		0x02, 0x07, '5', '5', '5', '1', '2', '3', '4',
		0x07, 0x04, 'T', 'E', 'S', 'T', 0x10,
	};
	/* Sum: 0x06 + 0x03 + 0x42 + 0x42 + 0x42 = 0xcf; checksum 0x31. */
	static const uint8_t vmwi_on_sdmf_expected[] = {
		0x06, 0x03, 0x42, 0x42, 0x42, 0x31,
	};
	/* Sum: 0x06 + 0x03 + 0x6f + 0x6f + 0x6f = 0x156; checksum 0xaa. */
	static const uint8_t vmwi_off_sdmf_expected[] = {
		0x06, 0x03, 0x6f, 0x6f, 0x6f, 0xaa,
	};
	/* Sum before checksum: 0x82 + 0x0d + date 0x19a + visual 0x10b = 0x334. */
	static const uint8_t vmwi_on_mdmf_expected[] = {
		0x82, 0x0d, 0x01, 0x08, '0', '8', '2', '4', '1', '2', '0', '0',
		0x0b, 0x01, 0xff, 0xcc,
	};
	/* Sum before checksum: 0x82 + 0x0d + date 0x19a + visual 0x0c = 0x235. */
	static const uint8_t vmwi_off_mdmf_expected[] = {
		0x82, 0x0d, 0x01, 0x08, '0', '8', '2', '4', '1', '2', '0', '0',
		0x0b, 0x01, 0x00, 0xcb,
	};
	/* Sum: 0x80 + 0x13 + date 0x19a + reason 0x55 + name 0x14b = 0x3cd. */
	static const uint8_t private_expected[] = {
		0x80, 0x13, 0x01, 0x08, '0', '8', '2', '4', '1', '2', '0', '0',
		0x04, 0x01, 'P', 0x07, 0x04, 'T', 'E', 'S', 'T', 0x33,
	};

	CASE("CID literal MDMF fixed vector");
	length = udsp_cid_msg_build(&cid, UDSP_CIDF_BELLCORE_FSK, false,
				    output, sizeof(output));
	CHECK_UINT(length, sizeof(mdmf_expected));
	CHECK(memcmp(output, mdmf_expected, sizeof(mdmf_expected)) == 0);
	CHECK(fsk_payload_matches(mdmf_expected, sizeof(mdmf_expected)));

	CASE("CID literal SDMF VMWI on vector");
	length = udsp_cid_vmwi_build(true, "", UDSP_CIDF_BELLCORE_FSK,
				     output, sizeof(output));
	CHECK_UINT(length, sizeof(vmwi_on_sdmf_expected));
	CHECK(memcmp(output, vmwi_on_sdmf_expected,
		     sizeof(vmwi_on_sdmf_expected)) == 0);
	CHECK(fsk_payload_matches(vmwi_on_sdmf_expected,
				  sizeof(vmwi_on_sdmf_expected)));

	CASE("CID literal SDMF VMWI off vector");
	length = udsp_cid_vmwi_build(false, "", UDSP_CIDF_BELLCORE_FSK,
				     output, sizeof(output));
	CHECK_UINT(length, sizeof(vmwi_off_sdmf_expected));
	CHECK(memcmp(output, vmwi_off_sdmf_expected,
		     sizeof(vmwi_off_sdmf_expected)) == 0);

	CASE("CID literal MDMF VMWI on vector");
	length = udsp_cid_vmwi_build(true, "08241200", UDSP_CIDF_ETSI_FSK,
				     output, sizeof(output));
	CHECK_UINT(length, sizeof(vmwi_on_mdmf_expected));
	CHECK(memcmp(output, vmwi_on_mdmf_expected,
		     sizeof(vmwi_on_mdmf_expected)) == 0);
	CHECK(fsk_payload_matches(vmwi_on_mdmf_expected,
				  sizeof(vmwi_on_mdmf_expected)));

	CASE("CID literal MDMF VMWI off vector");
	length = udsp_cid_vmwi_build(false, "08241200", UDSP_CIDF_ETSI_FSK,
				     output, sizeof(output));
	CHECK_UINT(length, sizeof(vmwi_off_mdmf_expected));
	CHECK(memcmp(output, vmwi_off_mdmf_expected,
		     sizeof(vmwi_off_mdmf_expected)) == 0);

	CASE("CID literal private number vector");
	cid = fixed_cid_get();
	cid.number_private = true;
	length = udsp_cid_msg_build(&cid, UDSP_CIDF_BELLCORE_FSK, false,
				    output, sizeof(output));
	CHECK_UINT(length, sizeof(private_expected));
	CHECK(memcmp(output, private_expected, sizeof(private_expected)) == 0);
	CHECK(fsk_payload_matches(private_expected, sizeof(private_expected)));
}

static void cid_variants_test(void)
{
	const enum udsp_cid_format etsi_formats[] = {
		UDSP_CIDF_ETSI_FSK, UDSP_CIDF_ETSI_RPAS, UDSP_CIDF_ETSI_DTAS,
		UDSP_CIDF_ETSI_LR_DTAS,
	};
	struct udsp_cid cid = fixed_cid_get();
	uint8_t bellcore[UDSP_CID_MSG_MAX];
	uint8_t variant[UDSP_CID_MSG_MAX];
	size_t bellcore_length;
	size_t length;
	unsigned i;

	bellcore_length = udsp_cid_msg_build(&cid, UDSP_CIDF_BELLCORE_FSK,
					     false, bellcore, sizeof(bellcore));
	CASE("CID ETSI transport variants share message bytes");
	for (i = 0; i < sizeof(etsi_formats) / sizeof(etsi_formats[0]); i++) {
		length = udsp_cid_msg_build(&cid, etsi_formats[i], false,
					    variant, sizeof(variant));
		CHECK_UINT(length, bellcore_length);
		CHECK(memcmp(variant, bellcore, length) == 0);
	}

	CASE("CID type 2 transport leaves message bytes unchanged");
	length = udsp_cid_msg_build(&cid, UDSP_CIDF_BELLCORE_FSK, true,
				    variant, sizeof(variant));
	CHECK_UINT(length, bellcore_length);
	CHECK(memcmp(variant, bellcore, length) == 0);

	CASE("CID MWI context leaves call-setup message unchanged");
	cid.mwi = true;
	length = udsp_cid_msg_build(&cid, UDSP_CIDF_BELLCORE_FSK, false,
				    variant, sizeof(variant));
	CHECK_UINT(length, bellcore_length);
	CHECK(memcmp(variant, bellcore, length) == 0);
	cid.mwi = false;

	CASE("CID malformed datetime skips date parameter");
	memcpy(cid.datetime, "0824120", 8);
	length = udsp_cid_msg_build(&cid, UDSP_CIDF_BELLCORE_FSK, false,
				    variant, sizeof(variant));
	CHECK_UINT(length, 18);
	CHECK_UINT(variant[0], 0x80);
	CHECK_UINT(variant[1], 0x0f);
	CHECK_UINT(variant[2], 0x02);
	CHECK_UINT(variant[11], 0x07);
	CHECK_UINT(variant[17], 0xb4);

	CASE("CID single-character absence reason passes through");
	cid = fixed_cid_get();
	memset(cid.number, 0, sizeof(cid.number));
	memcpy(cid.blockinfo, "S", 2);
	length = udsp_cid_msg_build(&cid, UDSP_CIDF_BELLCORE_FSK, false,
				    variant, sizeof(variant));
	CHECK_UINT(length, 22);
	CHECK_UINT(variant[12], 0x04);
	CHECK_UINT(variant[13], 0x01);
	CHECK_UINT(variant[14], 'S');

	CASE("CID VMWI rejects DTMF format");
	CHECK_UINT(udsp_cid_vmwi_build(true, "", UDSP_CIDF_DTMF,
				       variant, sizeof(variant)), 0);

	CASE("CID NTT unsupported without authoritative frame constants");
	CHECK_UINT(udsp_cid_msg_build(&cid, UDSP_CIDF_NTT, false,
				      variant, sizeof(variant)), 0);

	CASE("CID message capacity rejects partial frame");
	cid = fixed_cid_get();
	CHECK_UINT(udsp_cid_msg_build(&cid, UDSP_CIDF_BELLCORE_FSK, false,
				      variant, bellcore_length - 1U), 0);
}

static void dtmf_test(void)
{
	struct udsp_cid cid = fixed_cid_get();
	char output[64];
	size_t length;

	CASE("CID DTMF number markers");
	length = udsp_cid_dtmf_string(&cid, UDSP_CIDF_DTMF, output,
				      sizeof(output));
	CHECK_UINT(length, 9);
	CHECK(strcmp(output, "A5551234C") == 0);

	CASE("CID DTMF DTAS shares digit string");
	length = udsp_cid_dtmf_string(&cid, UDSP_CIDF_DTMF_DTAS, output,
				      sizeof(output));
	CHECK_UINT(length, 9);
	CHECK(strcmp(output, "A5551234C") == 0);

	CASE("CID DTMF withheld reason");
	cid.number_private = true;
	length = udsp_cid_dtmf_string(&cid, UDSP_CIDF_DTMF, output,
				      sizeof(output));
	CHECK_UINT(length, 5);
	CHECK(strcmp(output, "AB10C") == 0);

	CASE("CID DTMF unavailable reason");
	memset(cid.number, 0, sizeof(cid.number));
	cid.number_private = false;
	memcpy(cid.blockinfo, "O", 2);
	length = udsp_cid_dtmf_string(&cid, UDSP_CIDF_DTMF, output,
				      sizeof(output));
	CHECK_UINT(length, 5);
	CHECK(strcmp(output, "AB00C") == 0);

	CASE("CID DTMF rejects FSK format");
	CHECK_UINT(udsp_cid_dtmf_string(&cid, UDSP_CIDF_BELLCORE_FSK,
					output, sizeof(output)), 0);
}

int main(void)
{
	fsk_test();
	cid_vectors_test();
	cid_variants_test();
	dtmf_test();
	return tu_summary("cid");
}
