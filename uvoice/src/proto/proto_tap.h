// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The signaling message tap of the call trace. The call trace installs one
 * function through struct proto_ops tap_set, and the protocol calls it with
 * the wire bytes of every signaling message it sends or receives, on every
 * transport, before the bytes reach the socket and before the parser reads
 * them. A message that a transport encrypts passes here in the clear.
 *
 * One tap, on the uloop thread. The endpoints carry the local and the peer
 * address of the message. For a datagram socket bound to the wildcard the
 * local address is unspecified (an all-zero addr), and the call trace
 * substitutes the voice interface address. A stream message staged before
 * the local address is known carries the peer's family, an all-zero addr and
 * port 0.
 *
 * kind is an enum trace_pkt_kind, below. A protocol whose messages need kinds
 * of their own appends them at the end of that enum.
 *
 * The endpoint record and the packet kinds live here and not in the core:
 * the call trace rings (trace_ring.h) include this header for them, so a
 * protocol includes no core header to fill a tap record.
 */
#ifndef UVOICE_PROTO_TAP_H
#define UVOICE_PROTO_TAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One end of a synthesized datagram. ipver is 4 or 6 and names how many of
 * the address bytes count. */
struct trace_endpoint {
	uint8_t ipver;
	uint8_t addr[16];
	uint16_t port;
};

enum trace_pkt_kind {
	TRACE_PKT_SIP_UDP,
	TRACE_PKT_SIP_TCP,
	TRACE_PKT_SIP_TLS,
	TRACE_PKT_RTP,
	TRACE_PKT_RTCP,
	TRACE_PKT_UDPTL,
	TRACE_PKT_MGCP,
};

struct proto_tap_endpoints {
	struct trace_endpoint local;
	struct trace_endpoint peer;
	uint8_t kind;			/* enum trace_pkt_kind */
};

/* tx is true for a message this side sends, false for one it receives. */
typedef void (*proto_tap_fn)(bool tx, const struct proto_tap_endpoints *ep,
			     const void *buf, size_t len);

#endif
