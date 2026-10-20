// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * A transport destination, which the SIP transaction layer, the shared DNS
 * resolver (shared/resolve.h) and the MGCP protocol unit share. libc only.
 */
#ifndef UVOICE_SIP_DST_H
#define UVOICE_SIP_DST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/*
 * A transport destination. sockaddr_storage rather than a text host, because
 * the production transport sends to it directly and a text round trip is a
 * source of interop bugs. The helpers keep the tests readable.
 *
 * flags carries what the transport learned about the destination. It takes no
 * part in the comparison: sip_dst_eq() reads the address and the port alone,
 * so a flag never splits one destination into two. sip_dst_set() zeroes it and
 * a copy carries it along, so a destination built anywhere else starts at
 * "nothing is known".
 */
struct sip_txn_dst {
	struct sockaddr_storage sa;
	socklen_t sa_len;
	uint32_t flags;
};

/* A literal IPv4 or IPv6 address and a port; false for anything else. */
bool sip_dst_set(struct sip_txn_dst *dst, const char *host, uint16_t port);
bool sip_dst_host(const struct sip_txn_dst *dst, char *out, size_t olen);
bool sip_dst_eq(const struct sip_txn_dst *a, const struct sip_txn_dst *b);

#endif
