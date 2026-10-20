// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <stdio.h>

#include "net/conntrack.h"

/*
 * The two conntrack timeouts a restrict_source 2 pinhole lives under, read
 * from procfs at runtime because /etc/config/conntrack can override both, and
 * the values the box ships when a file cannot be read. The smaller one bounds
 * the refresher: the kernel grants the stream timeout only to an entry that
 * saw a reply more than 2 s after its creation, so a REGISTER exchange inside
 * 2 s leaves the entry on the plain timeout.
 */
#define NET_CT_PROC_DIR			"/proc/sys/net/netfilter/"
#define NET_CT_UDP_TIMEOUT_PATH \
	NET_CT_PROC_DIR "nf_conntrack_udp_timeout"
#define NET_CT_UDP_STREAM_PATH \
	NET_CT_PROC_DIR "nf_conntrack_udp_timeout_stream"
#define NET_CT_UDP_TIMEOUT_DEFAULT_S	60
#define NET_CT_UDP_STREAM_DEFAULT_S	180

/*
 * The longest an idle media descriptor stays silent before the backend sends
 * its empty datagram (hal.h, hal_media.punch_interval_ms): the RFC 6263
 * section 7 interval, which the backend also defaults to, and less when the
 * conntrack bound above asks for it.
 */
#define NET_CT_PUNCH_MAX_MS		15000

static unsigned int proc_uint_read(const char *path, unsigned int def)
{
	FILE *f = fopen(path, "r");
	unsigned int v;
	int n;

	if (!f)
		return def;

	n = fscanf(f, "%u", &v);
	fclose(f);

	return n == 1 ? v : def;
}

/* the pair the suite puts in place of the two procfs files (conntrack.h) */
static struct {
	unsigned int timeout_s;
	unsigned int stream_s;
} ct_test;

void net_ct_udp_bound_test_set(unsigned int timeout_s, unsigned int stream_s)
{
	ct_test.timeout_s = timeout_s;
	ct_test.stream_s = stream_s;
}

unsigned int net_ct_udp_bound_s(void)
{
	unsigned int timeout = ct_test.timeout_s;
	unsigned int stream = ct_test.stream_s;

	if (!timeout && !stream) {
		timeout = proc_uint_read(NET_CT_UDP_TIMEOUT_PATH,
					 NET_CT_UDP_TIMEOUT_DEFAULT_S);
		stream = proc_uint_read(NET_CT_UDP_STREAM_PATH,
					NET_CT_UDP_STREAM_DEFAULT_S);
	}

	if (!timeout)
		timeout = NET_CT_UDP_TIMEOUT_DEFAULT_S;
	if (!stream)
		stream = NET_CT_UDP_STREAM_DEFAULT_S;

	return timeout < stream ? timeout : stream;
}

/*
 * Half the bound, so an idle descriptor is refreshed inside it under
 * restrict_source 2. It is passed in every mode, because the empty datagram
 * costs nothing under a firewall rule and the cadence has to hold the moment
 * the mode moves.
 */
uint16_t net_ct_punch_interval_ms(void)
{
	unsigned int bound = net_ct_udp_bound_s();

	if (bound >= NET_CT_PUNCH_MAX_MS / 500)
		return NET_CT_PUNCH_MAX_MS;

	return (uint16_t)(bound * 500);
}
