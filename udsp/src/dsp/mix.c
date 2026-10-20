// SPDX-License-Identifier: LGPL-2.1-only
/*
 * udsp - software voice DSP for FXS lines
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * The conference mixer. UDSP_CONF_LOCAL_MIX builds one mix per destination:
 * every leg hears the line plus every other leg, and never its own return. A
 * leg that hears its own audio hears an echo, not a conference, so the
 * subtraction below removes that return.
 *
 * The bus accumulates every leg once and each destination is the bus minus its
 * own contribution, so the cost is one pass per leg to fill plus one pass per
 * destination. Summing the other legs per destination instead would cost the
 * square of the leg count and would tie the mixer to one UDSP_STREAM_MAX.
 *
 * One saturation, at the end. int16 adds that saturate at every step make the
 * result depend on the order the legs are visited, and a clipped add cannot be
 * undone by the subtraction that removes a leg. The int32 accumulator carries
 * every leg plus the line frame at full scale with 15 bits to spare, so the
 * only clip is the final one.
 *
 * The engine tick calls all three: they never allocate, never lock and never
 * log.
 */
#include <string.h>

#include "dsp.h"

_Static_assert((int64_t)(UDSP_STREAM_MAX + 1u) * 32768 < INT32_MAX,
	       "the mix bus accumulator can overflow");

static int16_t mix_sat(int32_t v)
{
	if (v > INT16_MAX)
		return INT16_MAX;
	if (v < INT16_MIN)
		return INT16_MIN;

	return (int16_t)v;
}

void udsp_mix_bus_reset(struct udsp_mix_bus *bus)
{
	memset(bus->acc, 0, sizeof(bus->acc));
}

void udsp_mix_bus_add(struct udsp_mix_bus *bus, const int16_t *src, unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		bus->acc[i] += src[i];
}

void udsp_mix_minus(const struct udsp_mix_bus *bus, const int16_t *self,
		    const int16_t *base, int16_t *out, unsigned n)
{
	if (!self) {
		for (unsigned i = 0; i < n; i++)
			out[i] = mix_sat(bus->acc[i] + base[i]);
		return;
	}

	for (unsigned i = 0; i < n; i++)
		out[i] = mix_sat(bus->acc[i] + base[i] - self[i]);
}
