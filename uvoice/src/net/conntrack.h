// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The conntrack udp bound and the media punch cadence it sets
 * (src/net/conntrack.c). Under restrict_source 2 the kernel admits inbound
 * traffic only on a conntrack entry this box opened, so the signaling
 * refresher and the media punch both run inside that bound.
 */
#ifndef UVOICE_NET_CONNTRACK_H
#define UVOICE_NET_CONNTRACK_H

#include <stdint.h>

/*
 * The smaller of net.netfilter.nf_conntrack_udp_timeout and
 * nf_conntrack_udp_timeout_stream in seconds, read from procfs on every call.
 * A file that cannot be read takes the shipped 60 s or 180 s.
 */
unsigned int net_ct_udp_bound_s(void);

/*
 * The pair a suite puts in place of the two procfs files. A nonzero member
 * stands in for its file, a zero member beside a nonzero one takes the
 * shipped default, and (0, 0) restores the procfs read. Never used by the
 * daemon.
 */
void net_ct_udp_bound_test_set(unsigned int timeout_s, unsigned int stream_s);

/*
 * The longest a media descriptor stays silent before the backend sends an
 * empty datagram on it (hal.h hal_media.punch_interval_ms): half the bound
 * above, and never more than the RFC 6263 section 7 interval of 15 s. A leg
 * takes it at its start.
 */
uint16_t net_ct_punch_interval_ms(void);

#endif
