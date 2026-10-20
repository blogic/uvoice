// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The SIP message tap. The call trace installs one function that sees the
 * wire bytes of every SIP message the daemon sends or receives, on every
 * transport, before the bytes reach the socket and before the parser reads
 * them. A TLS message passes here in the clear.
 *
 * One tap, on the uloop thread. The transports read the pointer once per
 * message, so a NULL tap costs one load. The endpoints carry the local and
 * the peer address of the message: for a stream connection the local
 * address is what getsockname() answered; for a datagram socket bound to the
 * wildcard the local address is unspecified (an all-zero addr) and the tap
 * owner substitutes the voice interface address.
 * A stream message staged before the local address is known carries the
 * peer's family, an all-zero addr and port 0.
 *
 * The record and the function type are those of the signaling message tap
 * of proto_tap.h. The SIP names stay as aliases, and kind is one of the
 * SIP_* kinds of enum trace_pkt_kind.
 *
 * The transports reach the core through sip_wire_host alone: the sip trace
 * domain and the endpoint of a tapped message. The SIP ops table sets it at
 * ops->init, and a suite that drives a transport sets its recorder host.
 * While it is NULL, the transports trace nothing and a tapped message carries
 * zero endpoints.
 */
#ifndef UVOICE_SIP_TAP_H
#define UVOICE_SIP_TAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "proto/proto_tap.h"
#include "proto/proto_trace.h"

extern const struct proto_host *sip_wire_host;

/* One line in the sip trace domain, through sip_wire_host. */
#define SIP_WIRE_TRACE(fmt, ...)					\
	do {								\
		if (sip_wire_host)					\
			PROTO_TRACE(sip_wire_host, TRACE_SIP, fmt,	\
				    ##__VA_ARGS__);			\
	} while (0)

void sip_tap_endpoint_set(struct trace_endpoint *out,
			  const struct sockaddr *sa);

#define sip_tap_endpoints	proto_tap_endpoints

typedef proto_tap_fn sip_tap_fn;

extern sip_tap_fn sip_tap_cb;

void sip_tap_set(sip_tap_fn fn);

#endif
