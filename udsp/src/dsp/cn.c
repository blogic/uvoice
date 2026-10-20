// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 */
#include <math.h>

#include "dsp.h"

static uint32_t random_next(uint32_t *seed)
{
	uint32_t value = *seed;

	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	*seed = value ? value : 1;
	return *seed;
}

void udsp_cn_init(struct udsp_cn *cn, uint32_t seed)
{
	cn->seed = seed ? seed : 1;
}

int udsp_cn_level_get(const int16_t *samples, size_t count)
{
	double energy = 0.0;
	double rms;
	int level;
	size_t i;

	if (!count)
		return 127;
	for (i = 0; i < count; i++)
		energy += (double)samples[i] * samples[i];
	rms = sqrt(energy / count);
	if (rms < 1.0)
		return 127;
	level = (int)lrint(-20.0 * log10(rms / 32767.0));
	if (level < 0)
		return 0;
	if (level > 127)
		return 127;
	return level;
}

void udsp_cn_generate(struct udsp_cn *cn, int level_dbov,
		      int16_t *samples, size_t count)
{
	double target;
	size_t i;

	if (level_dbov < 0)
		level_dbov = 0;
	if (level_dbov > 127)
		level_dbov = 127;
	target = 32767.0 * pow(10.0, -level_dbov / 20.0) * sqrt(3.0);
	for (i = 0; i < count; i++) {
		double unit = (random_next(&cn->seed) / 4294967295.0) * 2.0 - 1.0;

		samples[i] = udsp_sat16_round(unit * target);
	}
}

void udsp_codec_cn(struct udsp_codec *codec, int level_dbov, int16_t *samples,
		   unsigned count)
{
	struct udsp_cn cn;

	udsp_cn_init(&cn, codec->cn_seed);
	udsp_cn_generate(&cn, level_dbov, samples, count);
	codec->cn_seed = cn.seed;
}
