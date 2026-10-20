// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <math.h>
#include <string.h>

#include "cid.h"
#include "dsp.h"

/* spandsp adsi.h requires at least two stop bits after the checksum. */
#define FSK_POSTAMBLE_BITS 2U

struct fsk_mod_state {
	uint8_t data[UDSP_CID_MSG_MAX];
	double phase;
	double amplitude;
	size_t data_len;
	size_t bit_index;
	unsigned seizure_bits;
	unsigned mark_bits;
	unsigned baud_accumulator;
	unsigned rate_hz;
	unsigned mark_hz;
	unsigned space_hz;
	bool started;
	bool complete;
};

_Static_assert(sizeof(struct fsk_mod_state) <= sizeof(struct udsp_fsk_mod),
	       "FSK modulator state exceeds its opaque arena");

static struct fsk_mod_state *fsk_state_get(struct udsp_fsk_mod *modulator)
{
	return (struct fsk_mod_state *)modulator;
}

static bool fsk_bit_get(const struct fsk_mod_state *state)
{
	size_t data_bit;
	size_t byte_index;
	unsigned serial_bit;

	if (state->bit_index < state->seizure_bits)
		return (state->bit_index & 1U) != 0;
	data_bit = state->bit_index - state->seizure_bits;
	if (data_bit < state->mark_bits)
		return true;
	data_bit -= state->mark_bits;
	if (data_bit >= state->data_len * 10U)
		return true;
	byte_index = data_bit / 10U;
	serial_bit = data_bit % 10U;
	if (serial_bit == 0)
		return false;
	if (serial_bit == 9)
		return true;
	return ((state->data[byte_index] >> (serial_bit - 1U)) & 1U) != 0;
}

static int16_t sample_add(int16_t sample, int16_t addition)
{
	int value = sample + addition;

	if (value > INT16_MAX)
		return INT16_MAX;
	if (value < INT16_MIN)
		return INT16_MIN;
	return (int16_t)value;
}

void udsp_fsk_mod_init(struct udsp_fsk_mod *modulator,
		       enum udsp_fsk_std standard,
		       int level_dbm0, unsigned seizure_bits, unsigned mark_bits,
		       unsigned rate_hz)
{
	struct fsk_mod_state *state;

	if (!modulator)
		return;
	memset(modulator, 0, sizeof(*modulator));
	state = fsk_state_get(modulator);
	state->rate_hz = rate_hz;
	state->seizure_bits = seizure_bits;
	state->mark_bits = mark_bits;
	state->amplitude = udsp_dbm0_amplitude(level_dbm0);
	if (standard == UDSP_FSK_V23) {
		state->mark_hz = 1300;
		state->space_hz = 2100;
		return;
	}
	state->mark_hz = 1200;
	state->space_hz = 2200;
}

size_t udsp_fsk_mod_data(struct udsp_fsk_mod *modulator, const uint8_t *bytes,
			 size_t length)
{
	struct fsk_mod_state *state;
	size_t available;

	if (!modulator || !bytes || !length)
		return 0;
	state = fsk_state_get(modulator);
	if (state->started)
		return 0;
	available = sizeof(state->data) - state->data_len;
	if (length > available)
		length = available;
	memcpy(state->data + state->data_len, bytes, length);
	state->data_len += length;
	return length;
}

bool udsp_fsk_mod_run(struct udsp_fsk_mod *modulator, int16_t *frame,
		      unsigned n)
{
	struct fsk_mod_state *state;
	size_t total_bits;
	unsigned i;

	if (!modulator || !frame)
		return false;
	state = fsk_state_get(modulator);
	if (state->complete)
		return false;
	total_bits = state->seizure_bits + state->mark_bits +
		state->data_len * 10U;
	if (state->data_len)
		total_bits += FSK_POSTAMBLE_BITS;
	if (!total_bits) {
		state->complete = true;
		return false;
	}
	state->started = true;
	for (i = 0; i < n && state->bit_index < total_bits; i++) {
		double frequency = fsk_bit_get(state) ? state->mark_hz : state->space_hz;
		int16_t sample = (int16_t)lrint(state->amplitude * sin(state->phase));

		frame[i] = sample_add(frame[i], sample);
		state->phase += 2.0 * M_PI * frequency / state->rate_hz;
		if (state->phase >= 2.0 * M_PI)
			state->phase -= 2.0 * M_PI;
		state->baud_accumulator += 1200U;
		if (state->baud_accumulator < state->rate_hz)
			continue;
		state->baud_accumulator -= state->rate_hz;
		state->bit_index++;
	}
	if (state->bit_index == total_bits)
		state->complete = true;
	return true;
}
