// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#ifndef UDSP_PLC_H
#define UDSP_PLC_H

#include <stdint.h>

#define UDSP_PLC_FRAME_MAX	160U
#define UDSP_PLC_HISTORY_MAX	780U
#define UDSP_PLC_QUARTER_MAX	60U

struct udsp_plc_geometry {
	unsigned frame;
	unsigned pitch_min;
	unsigned pitch_max;
	unsigned history_len;
	unsigned corr_len;
	unsigned corr_buf;
};

struct udsp_plc {
	struct udsp_plc_geometry geometry;
	float pitch_buffer[UDSP_PLC_HISTORY_MAX];
	float last_quarter[UDSP_PLC_QUARTER_MAX];
	int16_t history[UDSP_PLC_HISTORY_MAX];
	unsigned loss_frames;
	unsigned pitch;
	unsigned pitch_overlap;
	unsigned pitch_offset;
	unsigned pitch_length;
	unsigned pitch_start;
};

void udsp_plc_init(struct udsp_plc *plc, unsigned rate, unsigned frame);
void udsp_plc_reset(struct udsp_plc *plc);
/*
 * Process one decoded frame and rewrite the caller's samples in place.
 * The output delay is pitch_max/4 samples, which is 3.75 ms at both rates.
 * Appendix I uses this delay for the loss-onset overlap-add operation.
 * The G.722 and G.711 codecs both decode through this path.
 */
void udsp_plc_frame_process(struct udsp_plc *plc, int16_t *samples);
void udsp_plc_generate(struct udsp_plc *plc, int16_t *samples);

#endif
