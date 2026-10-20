// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#ifndef UDSP_G729_H
#define UDSP_G729_H

#include <stdbool.h>
#include <stdint.h>

#define G729_LPC_ORDER 10
#define G729_VAD_ORDER 12
#define G729_LSP_STAGE1_SIZE 128
#define G729_LSP_STAGE2_SIZE 32

/* g729_tables_init() computes these tables once and is safe to call from
 * any thread. g729_tables_verify() returns true when the CRC-32 of each
 * computed table equals its expected value. */
extern int16_t g729_power2_table[33];
extern int16_t g729_log2_table[33];
extern int16_t g729_inverse_sqrt_table[49];
extern int16_t g729_hamwindow[240];
extern int16_t g729_cos_table[65];
extern int16_t g729_cos_slope[64];
extern int16_t g729_acos_slope[64];
void g729_tables_init(void);
bool g729_tables_verify(void);

extern const int16_t g729_lag_h[G729_VAD_ORDER];
extern const int16_t g729_lag_l[G729_VAD_ORDER];
extern const int16_t g729_lsp_stage1[G729_LSP_STAGE1_SIZE][G729_LPC_ORDER];
extern const int16_t g729_lsp_stage2[G729_LSP_STAGE2_SIZE][G729_LPC_ORDER];
extern const int16_t g729_lsp_predictor[2][4][G729_LPC_ORDER];
extern const int16_t g729_lsp_present[2][G729_LPC_ORDER];
extern const int16_t g729_lsp_present_inverse[2][G729_LPC_ORDER];
extern const int16_t g729_lsp_grid[51];
extern const int16_t g729_pitch_interpolation[31];
extern const int16_t g729_gain_predictor[4];
extern const int16_t g729_gain_stage1[8][2];
extern const int16_t g729_gain_stage2[16][2];
extern const int16_t g729_gain_map1[8];
extern const int16_t g729_gain_map2[16];
extern const int16_t g729_gain_inverse1[8];
extern const int16_t g729_gain_inverse2[16];
extern const int16_t g729_high_pass_b[3];
extern const int16_t g729_high_pass_a[3];
extern const int16_t g729_preprocess_b[3];
extern const int16_t g729_preprocess_a[3];
extern const int16_t g729_vad_low_band[G729_VAD_ORDER + 1];
extern const int16_t g729_vad_init_factor[33];
extern const int16_t g729_vad_init_shift[33];
extern const int16_t g729_sid_gain[32];
extern const int16_t g729_sid_gain_factor[3];
extern const int16_t g729_sid_gain_margin[3];
extern const int16_t g729_sid_predictor[2][4][G729_LPC_ORDER];
extern const int16_t g729_sid_present[2][G729_LPC_ORDER];
extern const int16_t g729_sid_present_inverse[2][G729_LPC_ORDER];
extern const int16_t g729_sid_stage1_map[32];
extern const int16_t g729_sid_stage2_map[2][16];
extern const int16_t g729_sid_mode_weight[2];

/* dsp-internal test hooks for test_g729. vad_decision reads the Annex B
 * clause B.3 voice decision the encoder stored for its last frame.
 * frame_count_set preloads the VAD frame counter so the 32767-to-256 wrap
 * is testable without encoding 32767 frames. levinson_probe runs one
 * Levinson recursion on a caller-built lag-windowed autocorrelation and
 * copies out the committed reflection pair: the unstable early return is
 * unreachable from PCM input (the lag window bounds the reflection
 * magnitude), so its commit discipline is only testable here. */
struct udsp_codec;
bool g729_vad_decision_get(const struct udsp_codec *codec);
void g729_vad_frame_count_set(struct udsp_codec *codec, int16_t frame_count);
void g729_levinson_probe(struct udsp_codec *codec, const int16_t *high,
			 const int16_t *low, int16_t reflection[2]);

#endif
