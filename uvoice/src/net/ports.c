// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <string.h>

#include "net/ports.h"

/*
 * The pool covers even ports only; the RTCP port is always the odd one above
 * it (RFC 3550 11). The bitmap is static and therefore bounded: a range wider
 * than this covers its first NET_PORT_BITS even ports, which is far more than
 * the four legs a two line board can hold.
 */
#define NET_PORT_BITS		512
#define NET_PORT_WORDS		(NET_PORT_BITS / 32)
#define NET_PORT_MIN_DEFAULT	10000
#define NET_PORT_MAX_DEFAULT	10030

static struct {
	uint32_t bits[NET_PORT_WORDS];
	uint16_t base;
	uint16_t count;
	unsigned int next;
	unsigned int used;
} pa;

void net_port_reset(void)
{
	memset(&pa, 0, sizeof(pa));
}

/*
 * The one place the unset and incoherent cases are decided. net_port_range()
 * and the allocator both go through it, so the firewall and the pool cannot
 * disagree about which ports a call may use.
 */
static void port_range_norm(unsigned int *min, unsigned int *max)
{
	if (*min && *max && *max >= *min && *max <= 65535)
		return;

	*min = NET_PORT_MIN_DEFAULT;
	*max = NET_PORT_MAX_DEFAULT;
}

void net_port_range(const struct cfg_channel *ch, uint16_t *min, uint16_t *max)
{
	unsigned int lo = ch ? ch->rtp_port_min : 0;
	unsigned int hi = ch ? ch->rtp_port_max : 0;

	port_range_norm(&lo, &hi);

	if (min)
		*min = (uint16_t)lo;
	if (max)
		*max = (uint16_t)hi;
}

/*
 * The comparison is on the effective range, so two configurations that differ
 * only in how they express the same range (unset against the built in values,
 * or an incoherent pair against the fallback it lands on) do not clear a line
 * that has nothing to move to.
 */
bool net_port_range_eq(const struct cfg_channel *a,
		       const struct cfg_channel *b)
{
	uint16_t amin, amax, bmin, bmax;

	net_port_range(a, &amin, &amax);
	net_port_range(b, &bmin, &bmax);

	return amin == bmin && amax == bmax;
}

static void port_range_take(unsigned int min, unsigned int max, uint16_t *base,
			    uint16_t *count)
{
	unsigned int first, last, n;

	port_range_norm(&min, &max);

	first = min + (min & 1);		/* even ports only */
	last = max - ((max & 1) ? 1 : 0);
	if (last < first || last < 2) {
		*base = 0;
		*count = 0;
		return;
	}

	n = (last - first) / 2 + 1;
	if (n > NET_PORT_BITS)
		n = NET_PORT_BITS;

	*base = (uint16_t)first;
	*count = (uint16_t)n;
}

static bool port_bit_is(unsigned int i)
{
	return (pa.bits[i / 32] >> (i % 32)) & 1u;
}

/*
 * Round robin next fit. A range that moves under a live leg is not adopted
 * until the pool is empty: the ports the running calls hold are indexes into
 * the old base, and rebasing under them would free the wrong bit.
 */
bool net_port_alloc(unsigned int min, unsigned int max, uint16_t *out)
{
	uint16_t base, count;
	unsigned int i, n;

	if (!out)
		return false;

	port_range_take(min, max, &base, &count);
	if (!count)
		return false;

	if (!pa.used && (pa.base != base || pa.count != count)) {
		memset(&pa, 0, sizeof(pa));
		pa.base = base;
		pa.count = count;
	}

	if (!pa.count)
		return false;

	for (n = 0; n < pa.count; n++) {
		i = (pa.next + n) % pa.count;
		if (port_bit_is(i))
			continue;

		pa.bits[i / 32] |= 1u << (i % 32);
		pa.next = (i + 1) % pa.count;
		pa.used++;
		*out = (uint16_t)(pa.base + 2 * i);

		return true;
	}

	return false;
}

void net_port_free(uint16_t port)
{
	unsigned int i;

	if (!port || !pa.count || port < pa.base)
		return;

	i = (unsigned int)(port - pa.base) / 2;
	if (i >= pa.count || !port_bit_is(i))
		return;

	pa.bits[i / 32] &= ~(1u << (i % 32));
	pa.used--;
}
