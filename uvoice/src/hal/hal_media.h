// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Media helpers for a backend that owns its media sockets. They are the RTCP
 * port rule, the endpoint compare, the SRTP suite check, the sockaddr fill,
 * the connect and the DSCP record. Each backend keeps its own socket open,
 * mark function and close order.
 *
 * The header is private to the backends. The helpers read struct hal_addr,
 * and hal.h pulls libubox, so they cannot join the libc-only posix.h.
 */
#ifndef UVOICE_HAL_MEDIA_H
#define UVOICE_HAL_MEDIA_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

#include "hal.h"

/* The RTCP port is the RTP port plus one unless the caller named another one,
 * which is what RFC 3550 section 11 asks for and what the core allocates. */
static inline uint16_t rtcp_port_of(const struct hal_addr *a)
{
	return a->rtcp_port ? a->rtcp_port : (uint16_t)(a->port + 1);
}

/*
 * Only the bytes the family uses are compared. hal_addr carries 16 address
 * bytes whatever the version. The 12 tail bytes of an IPv4 address belong to
 * no one. A caller that leaves them dirty must not read as a moved endpoint,
 * because a move stops a live stream.
 */
static inline bool addr_same(const struct hal_addr *a, const struct hal_addr *b)
{
	size_t len = a->ipver == 6 ? 16 : 4;

	return a->ipver == b->ipver && a->port == b->port &&
	       rtcp_port_of(a) == rtcp_port_of(b) &&
	       !memcmp(a->addr, b->addr, len);
}

/* The three values enum hal_srtp_suite defines, spelled out so a renumbering
 * of that enum cannot widen the set a backend accepts. */
static inline bool srtp_suite_known(uint8_t suite)
{
	return suite == HAL_SRTP_NONE ||
	       suite == HAL_SRTP_AES_CM_128_SHA1_80 ||
	       suite == HAL_SRTP_AES_CM_128_SHA1_32;
}

union sockaddr_any {
	struct sockaddr sa;
	struct sockaddr_in v4;
	struct sockaddr_in6 v6;
};

static inline socklen_t sockaddr_fill(union sockaddr_any *dst,
				      const struct hal_addr *a, uint16_t port)
{
	memset(dst, 0, sizeof(*dst));

	if (a->ipver == 6) {
		dst->v6.sin6_family = AF_INET6;
		dst->v6.sin6_port = htons(port);
		memcpy(&dst->v6.sin6_addr, a->addr, sizeof(dst->v6.sin6_addr));

		return (socklen_t)sizeof(dst->v6);
	}

	dst->v4.sin_family = AF_INET;
	dst->v4.sin_port = htons(port);
	/* hal_addr.addr is network byte order already. */
	memcpy(&dst->v4.sin_addr, a->addr, sizeof(dst->v4.sin_addr));

	return (socklen_t)sizeof(dst->v4);
}

static inline int media_fd_connect(int fd, const struct hal_addr *remote,
				   uint16_t port)
{
	union sockaddr_any sa;
	socklen_t len = sockaddr_fill(&sa, remote, port);

	return connect(fd, &sa.sa, len);
}

/*
 * The record follows the mark and never the request. If the record took a
 * value that the mark refused, the early return would decide that the socket
 * already carries it. The mark would then never run again, and the stream
 * would run unmarked for the whole call. The old value stays in place, so the
 * next re-program tries again.
 */
static inline void media_tos_apply(int fd, int family, uint8_t want,
				   uint8_t *have,
				   int (*mark)(int, int, uint8_t))
{
	if (*have == want)
		return;

	if (mark(fd, family, want) == 0)
		*have = want;
}

#endif
