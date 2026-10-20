// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Remez used 47 taps, equal weights, grid density 64, 1000 iterations, and
 * normalized bands [0, 0.425] and [0.575, 1].
 * The stored table folds in gain 2; its sum 65495 puts the passband peak
 * 0.013 dB above unity.
 * Down2 halves each stored even tap and rounds the 32767 centre half from
 * 16383.5 to 16384.
 * Up2 splits coefficient signs between two int32 accumulators because their
 * separate 0 dBFS bounds fit int32.
 * Down2 emits after each second input and selects the phase at odd 16 kHz
 * instants, whose centre response is input offset 23.
 * The up2 pure-delay phase fills odd output slots; the symmetric FIR phase
 * fills even output slots.
 */
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "resamp.h"

static const int16_t resamp_coeff_q15[UDSP_RESAMP_TAPS] = {
	-54, 0, 98, 0, -184, 0, 310, 0,
	-492, 0, 750, 0, -1112, 0, 1632, 0,
	-2416, 0, 3752, 0, -6694, 0, 20774, 32767,
	20774, 0, -6694, 0, 3752, 0, -2416, 0,
	1632, 0, -1112, 0, 750, 0, -492, 0,
	310, 0, -184, 0, 98, 0, -54
};

/* 32767 / 2 is 16383.5, so the down2 centre rounds to Q15 half. */
static const int16_t resamp_down_centre_q15 = 1 << 14;

static int16_t resamp_sample_round(int32_t acc_a, int32_t acc_b)
{
	const int32_t scale = 1 << 15;
	const int32_t half = scale >> 1;
	const int32_t high = INT16_MAX * scale + half;
	const int32_t low = INT16_MIN * scale - half;
	int32_t acc;

	/* At 0 dBFS, each coefficient-sign sum fits int32, but their combined
	 * worst case does not. */
	if (acc_b > 0 && acc_a > INT32_MAX - acc_b)
		return INT16_MAX;
	if (acc_b < 0 && acc_a < INT32_MIN - acc_b)
		return INT16_MIN;
	acc = acc_a + acc_b;
	if (acc >= high)
		return INT16_MAX;
	if (acc <= low)
		return INT16_MIN;
	if (acc >= 0)
		return (int16_t)((acc + half) >> 15);
	return (int16_t)-((-acc + half) >> 15);
}

/* An index at or past the window, which only a caller that skipped the reset
 * can hold, restarts at 0 as the ring it replaced did. */
static void resamp_history_push(struct udsp_resamp *rs, int16_t sample)
{
	unsigned index = rs->index < UDSP_RESAMP_TAPS ? rs->index : 0;

	index = index ? index - 1U : UDSP_RESAMP_TAPS - 1U;
	rs->history[index] = sample;
	rs->history[index + UDSP_RESAMP_TAPS] = sample;
	rs->index = index;
}

/* win[k] is the input delayed by k samples (resamp.h). */
static const int16_t *resamp_window_get(const struct udsp_resamp *rs)
{
	return &rs->history[rs->index];
}

static int16_t resamp_down_sample_get(const struct udsp_resamp *rs)
{
	const int16_t *win = resamp_window_get(rs);
	int32_t acc = (int32_t)win[UDSP_RESAMP_TAPS / 2U] *
		resamp_down_centre_q15;
	unsigned tap;

	for (tap = 0; tap < UDSP_RESAMP_TAPS / 2U; tap += 2U)
		acc += ((int32_t)win[tap] + win[UDSP_RESAMP_TAPS - 1U - tap]) *
			(resamp_coeff_q15[tap] >> 1);
	return resamp_sample_round(acc, 0);
}

static int16_t resamp_up_even_get(const struct udsp_resamp *rs)
{
	const int16_t *win = resamp_window_get(rs);
	int32_t positive_acc = 0;
	int32_t negative_acc = 0;
	unsigned tap;

	for (tap = 0; tap < UDSP_RESAMP_TAPS / 2U; tap += 2U) {
		int32_t term = ((int32_t)win[tap / 2U] +
				win[(UDSP_RESAMP_TAPS - 1U - tap) / 2U]) *
			resamp_coeff_q15[tap];

		if (resamp_coeff_q15[tap] > 0)
			positive_acc += term;
		else
			negative_acc += term;
	}
	return resamp_sample_round(positive_acc, negative_acc);
}

static int16_t resamp_up_odd_get(const struct udsp_resamp *rs)
{
	const int16_t *win = resamp_window_get(rs);
	int32_t acc = (int32_t)win[UDSP_RESAMP_TAPS / 4U] *
		resamp_coeff_q15[UDSP_RESAMP_TAPS / 2U];

	return resamp_sample_round(acc, 0);
}

void udsp_resamp_reset(struct udsp_resamp *rs)
{
	memset(rs, 0, sizeof(*rs));
}

void udsp_resamp_down2(struct udsp_resamp *rs, const int16_t *in,
		       unsigned n_in, int16_t *out)
{
	unsigned i;

	n_in &= ~1U;
	for (i = 0; i < n_in; i += 2U) {
		resamp_history_push(rs, in[i]);
		resamp_history_push(rs, in[i + 1U]);
		out[i / 2U] = resamp_down_sample_get(rs);
	}
}

void udsp_resamp_up2(struct udsp_resamp *rs, const int16_t *in,
		     unsigned n_in, int16_t *out)
{
	unsigned i;

	for (i = 0; i < n_in; i++) {
		resamp_history_push(rs, in[i]);
		out[2U * i] = resamp_up_even_get(rs);
		out[2U * i + 1U] = resamp_up_odd_get(rs);
	}
}

#ifdef UDSP_RESAMP_TEST
int16_t udsp_resamp_coefficient_get(unsigned index)
{
	return resamp_coeff_q15[index];
}
#endif
