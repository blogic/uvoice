// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Private helpers shared by the tdm back ends (tdm/alsa.c, tdm/alsa_raw.c,
 * tdm/file.c). This header is not public. The stable contract is
 * include/udsp/tdm.h, which also declares the three backend objects. The
 * engine-facing buffer is always planar with channel 0 first; a direction
 * whose device access layout is interleaved converts here on the way in and
 * out.
 */
#ifndef UDSP_TDM_COMMON_H
#define UDSP_TDM_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "udsp/tdm.h"

/* tdm.h allows channels 1 or 2; stack-side per-channel pointer arrays size to
 * this so the RT read/write path never allocates. */
#define TDM_MAX_CHANNELS 2

/* Writes the caller's refusal record, when there is one, and hands the errno
 * back so a refusal site is one return statement. */
static inline int tdm_fail_set(struct udsp_tdm_fail *fail,
			       enum udsp_tdm_dir stream, bool geometry, int err,
			       const char *what)
{
	if (!fail)
		return err;
	fail->geometry = geometry;
	fail->err = err;
	fail->stream = stream;
	fail->what = what;

	return err;
}

static inline void tdm_planar_to_interleaved(int16_t *dst, const int16_t *planar,
					     unsigned channels, unsigned frames)
{
	for (unsigned ch = 0; ch < channels; ch++)
		for (unsigned f = 0; f < frames; f++)
			dst[f * channels + ch] = planar[ch * frames + f];
}

static inline void tdm_interleaved_to_planar(int16_t *planar, const int16_t *src,
					     unsigned channels, unsigned frames)
{
	for (unsigned ch = 0; ch < channels; ch++)
		for (unsigned f = 0; f < frames; f++)
			planar[ch * frames + f] = src[f * channels + ch];
}

#endif
