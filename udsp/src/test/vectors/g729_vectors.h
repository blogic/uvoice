// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#ifndef UDSP_TEST_G729_VECTORS_H
#define UDSP_TEST_G729_VECTORS_H

static const uint8_t g729_speech_expected[10] = {
	0x06, 0x0a, 0xcb, 0xc0, 0x00, 0xfa, 0xd4, 0xad, 0x98, 0xac,
};

static const int16_t g729_decoder_expected[80] = {
	2, 6, 3, 1, -1, -1, -1, -1, -1, -1,
	-1, -1, -1, -1, -1, -1, -1, -1, -1, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 30, 17, 4, 2, 5,
	-4, -4, 0, -4, -4, -4, -4, -6, -39, -22,
	-5, -40, -62, -20, -3, -8, 4, 16, 9, 11,
};

#endif
