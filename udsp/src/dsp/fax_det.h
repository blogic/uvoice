// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#ifndef UDSP_FAX_DET_H
#define UDSP_FAX_DET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dsp.h"

#define UDSP_FAX_RESULT_MAX 6U
#define UDSP_V21_TRACKS 4U

struct udsp_fax_result {
	uint16_t tone;
	uint8_t direction;
	bool start;
	uint32_t duration_ms;
};

struct udsp_v21_track {
	double mark_i;
	double mark_q;
	double space_i;
	double space_q;
	double energy;
	unsigned delay;
	unsigned baud_accumulator;
	unsigned samples;
	unsigned bits_since_flag;
	unsigned flags;
	uint8_t shift;
};

struct udsp_fax_detector {
	struct udsp_goertzel_bank named_bank;
	struct udsp_goertzel_bank modem_low_bank;
	struct udsp_goertzel_bank modem_mid_bank;
	struct udsp_goertzel_bank modem_high_bank;
	struct udsp_v21_track v21[UDSP_V21_TRACKS];
	double v21_sine[160];
	double hold_energy;
	double hold_previous_in;
	double hold_previous_out;
	double ans_phase;
	double ans_previous_i;
	double ans_previous_q;
	double am_sum;
	double am_cos;
	double am_sin;
	double am_basis_cos;
	double am_basis_sin;
	uint64_t sample_count;
	uint32_t frame_count;
	uint32_t cng_frames;
	uint32_t cng_good_frames;
	uint32_t cng_gap_frames;
	uint32_t ans_frames;
	uint32_t active_frames;
	uint32_t last_reversal_frame;
	uint32_t v21_last_flag_frame;
	uint32_t modem_frames;
	unsigned cng_misses;
	unsigned ans_misses;
	unsigned active_misses;
	unsigned reversal_count;
	unsigned modem_bin;
	unsigned modem_candidate_anchor_bin;
	unsigned modem_misses;
	unsigned active_modem_anchor_bin;
	uint16_t active_tone;
	uint8_t direction;
	bool ans_previous_valid;
	bool reversal_valid;
	bool cng_rearm_required;
};

/*
 * The energy of the last frame the detector processed, measured inside the
 * band the G.168 disabler holds on. The engine's tone disabler releases on it
 * (core/chan.c UDSP_FAX_HOLD_ENERGY); the energy the detector uses for its own
 * dominance gates is a different measurement and stays inside fax_det.c.
 */
static inline double
udsp_fax_detector_hold_energy_get(const struct udsp_fax_detector *detector)
{
	return detector->hold_energy;
}

void udsp_fax_detector_init(struct udsp_fax_detector *detector,
			    enum udsp_tone_dir direction);
size_t udsp_fax_detector_process(struct udsp_fax_detector *detector,
				 const int16_t *samples,
				 struct udsp_fax_result *results,
				 size_t capacity);
size_t udsp_fax_detector_flush(struct udsp_fax_detector *detector,
			       struct udsp_fax_result *results,
			       size_t capacity);

#endif
