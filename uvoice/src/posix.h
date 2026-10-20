// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Small helpers over the POSIX clock and socket calls. They are a 64-bit
 * monotonic clock in milliseconds, its 32-bit stamp form, the delay of a retry
 * ladder, the sockaddr steps of the SIP transports, the device bind and the
 * TOS mark. The core, every signaling protocol and the HAL backends share
 * them (proto/proto.h, the include rule).
 *
 * The header carries only libc includes. The SIP object libraries do not
 * include uvoice.h, which pulls libubox and the daemon state. A file in sip/
 * therefore includes this header as "../posix.h".
 */
#ifndef UVOICE_POSIX_H
#define UVOICE_POSIX_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

static inline uint64_t mono_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return 0;

	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

/* The stamp both the poll cache and a backend tick are measured on. */
static inline uint32_t uvoice_mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);

	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/*
 * The clamp for a stamp whose 0 means "never". call.c holds the last answer
 * tone of a leg that way (struct call fax_hint_ms). mono_ms reads 0 for the
 * first millisecond after boot, and again for a 1 ms window every 2^32 ms of
 * uptime. Both map to 1 here. One millisecond of stamp error is invisible; a
 * hint that reads as none is not.
 */
static inline uint32_t uvoice_stamp_nonzero(uint32_t ms)
{
	return ms ? ms : 1;
}

/*
 * The delay of attempt number fails of a retry ladder: base doubled per
 * attempt and held at max. The shift stops at four, because the cap holds
 * from the fifth attempt on and a wider shift would say nothing more.
 */
static inline unsigned int retry_delay_ms(unsigned int fails, unsigned int base,
					  unsigned int max)
{
	unsigned int ms;

	if (!fails)
		return base;

	ms = base << (fails < 5 ? fails - 1 : 4);

	return ms > max ? max : ms;
}

static inline socklen_t sa_len_of(int family)
{
	if (family == AF_INET6)
		return sizeof(struct sockaddr_in6);

	return sizeof(struct sockaddr_in);
}

/*
 * The length the bind needs, and 0 for a text this family cannot read. A NULL
 * or empty text is the wildcard address of the family.
 */
static inline socklen_t addr_pton(int family, const char *text,
				  struct sockaddr_storage *sa)
{
	struct sockaddr_in *v4 = (struct sockaddr_in *)sa;
	struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)sa;

	memset(sa, 0, sizeof(*sa));
	sa->ss_family = (sa_family_t)family;

	if (family == AF_INET6) {
		if (!text || !text[0]) {
			v6->sin6_addr = in6addr_any;
			return sa_len_of(family);
		}

		return inet_pton(AF_INET6, text, &v6->sin6_addr) == 1 ?
		       sa_len_of(family) : 0;
	}

	if (!text || !text[0]) {
		v4->sin_addr.s_addr = htonl(INADDR_ANY);
		return sa_len_of(family);
	}

	return inet_pton(AF_INET, text, &v4->sin_addr) == 1 ?
	       sa_len_of(family) : 0;
}

static inline void sa_port_set(struct sockaddr_storage *sa, uint16_t port)
{
	struct sockaddr_in *v4 = (struct sockaddr_in *)sa;
	struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)sa;

	if (sa->ss_family == AF_INET6)
		v6->sin6_port = htons(port);
	else
		v4->sin_port = htons(port);
}

static inline uint16_t sa_port_get(const struct sockaddr_storage *sa)
{
	const struct sockaddr_in *v4 = (const struct sockaddr_in *)sa;
	const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)sa;

	if (sa->ss_family == AF_INET6)
		return ntohs(v6->sin6_port);

	return ntohs(v4->sin_port);
}

static inline bool sa_text(const struct sockaddr_storage *sa, char *out,
			   size_t olen)
{
	const struct sockaddr_in *v4 = (const struct sockaddr_in *)sa;
	const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)sa;

	if (sa->ss_family == AF_INET6)
		return inet_ntop(AF_INET6, &v6->sin6_addr, out, olen) != NULL;
	if (sa->ss_family == AF_INET)
		return inet_ntop(AF_INET, &v4->sin_addr, out, olen) != NULL;

	return false;
}

/*
 * The device bind must happen before the address bind and before the connect.
 * SO_BINDTODEVICE does not move a socket that already carries a route. A
 * wildcard bind then still takes datagrams from every interface.
 */
static inline bool sock_iface_bind(int fd, const char *iface)
{
	if (!iface[0])
		return true;

	return setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, iface,
			  (socklen_t)strlen(iface) + 1) == 0;
}

static inline int sock_tos_set(int fd, int family, int tos)
{
	int level = family == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP;
	int opt = family == AF_INET6 ? IPV6_TCLASS : IP_TOS;

	return setsockopt(fd, level, opt, &tos, sizeof(tos));
}

#endif
