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

/* The socket family of a hal_addr: AF_INET6 for version 6, AF_INET otherwise.
 * A caller validates the version first (hal_media_validate_addrs). */
static inline int addr_family(const struct hal_addr *a)
{
	return a->ipver == 6 ? AF_INET6 : AF_INET;
}

/* The two versions a hal_addr record defines. Any other value names no family,
 * so no socket can hold it. */
static inline bool ipver_known(uint8_t v)
{
	return v == 4 || v == 6;
}

/*
 * A DSCP a backend has not successfully put on a socket yet. Outside the 0..63
 * a DSCP can hold, so "nothing is on this socket" can never read as a value a
 * description asked for: a mark that failed must leave the record saying so, or
 * the same-value early return of media_tos_apply would decide the socket is
 * already marked and never try again.
 */
#define HAL_MEDIA_DSCP_NONE		0xffu

/*
 * The media sockets of one stream, owned by a backend that makes its own
 * descriptors (hal.h stream_config_set). It carries no backend engine type, so
 * either backend embeds it in its own stream record beside the engine state.
 *
 * rtp_fd and rtcp_fd are -1 when the stream holds no pair; a caller sets them
 * with hal_media_sock_init before any op runs, because a calloc zero would name
 * descriptor 0. local, remote and rtcp_remote say what the sockets carry, and
 * bind_dev is part of that local identity (SO_BINDTODEVICE). rtp_dscp and
 * rtcp_dscp are HAL_MEDIA_DSCP_NONE until a mark actually reaches the socket.
 */
struct hal_media_sock {
	int rtp_fd;
	int rtcp_fd;
	struct hal_addr local;
	struct hal_addr remote;
	struct hal_addr rtcp_remote;
	char bind_dev[HAL_IFNAME_MAX];
	uint8_t rtp_dscp;
	uint8_t rtcp_dscp;
};

/* IP_TOS for AF_INET, IPV6_TCLASS for AF_INET6, computed (dscp << 2) from the
 * 0..63 DSCP hal_media carries. 0 when the mark reached the socket. */
int hal_media_dscp_mark(int fd, int family, uint8_t dscp);

/* Set a stream's pair to "no sockets". */
void hal_media_sock_init(struct hal_media_sock *s);

/* Close both descriptors and set the pair to "no sockets". A backend calls its
 * engine's stream stop first, because the engine holds dups of these. */
void hal_media_sock_close(struct hal_media_sock *s);

/* The RTCP peer of one description: the far end's own RTCP address when its
 * a=rtcp named one (RFC 3605 2.1), and the media address otherwise. */
void hal_media_rtcp_peer(const struct hal_media *m, struct hal_addr *out);

/*
 * Open, bind and mark a fresh pair for @m, recording the local identity and the
 * marks in @s. The far end is connected by hal_media_sock_connect. Returns
 * HAL_OK, or HAL_FATAL with the pair closed again.
 */
enum hal_status hal_media_sock_open(struct hal_media_sock *s,
				    const struct hal_media *m);

/* Re-apply the DSCP marks of @m to a pair that a program kept (no rebind). */
void hal_media_sock_tos_refresh(struct hal_media_sock *s,
				const struct hal_media *m);

/*
 * Connect the pair to the far end of @m and record both peers. Returns HAL_OK,
 * or HAL_FATAL (the caller owns the standing pair).
 */
enum hal_status hal_media_sock_connect(struct hal_media_sock *s,
				       const struct hal_media *m);

/*
 * The address checks of hal.h stream_config_set rule 4, with no engine
 * capability in them: the versions are known and equal across the two ends and
 * any separate RTCP address, the local and remote ports are nonzero, and the
 * derived RTCP ports do not wrap to zero. @features carries the backend's
 * hal_feature bits; an IPv6 description is refused unless HAL_FEAT_IPV6_MEDIA is
 * set. Returns HAL_OK, HAL_UNSUPPORTED (a shape the backend cannot carry) or
 * HAL_FATAL (a description the core should never have built). The codec and
 * SRTP capability checks stay with the backend, which asks its engine.
 */
enum hal_status hal_media_validate_addrs(const struct hal_media *m,
					 uint32_t features);

#endif
