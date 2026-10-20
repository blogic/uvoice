// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Transport destinations. See shared/dst.h.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>

#include "proto/shared/dst.h"

bool sip_dst_set(struct sip_txn_dst *dst, const char *host, uint16_t port)
{
	struct sockaddr_in *v4 = (struct sockaddr_in *)&dst->sa;
	struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&dst->sa;

	memset(dst, 0, sizeof(*dst));

	if (host && inet_pton(AF_INET, host, &v4->sin_addr) == 1) {
		dst->sa.ss_family = AF_INET;
		dst->sa_len = sizeof(*v4);
		v4->sin_port = htons(port);
		return true;
	}

	/* No resolver here: a name reaches this layer already resolved. */
	if (host && inet_pton(AF_INET6, host, &v6->sin6_addr) == 1) {
		dst->sa.ss_family = AF_INET6;
		dst->sa_len = sizeof(*v6);
		v6->sin6_port = htons(port);
		return true;
	}

	return false;
}

bool sip_dst_host(const struct sip_txn_dst *dst, char *out, size_t olen)
{
	const struct sockaddr_in *v4 = (const struct sockaddr_in *)&dst->sa;
	const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&dst->sa;

	if (dst->sa.ss_family == AF_INET)
		return inet_ntop(AF_INET, &v4->sin_addr, out, olen) != NULL;
	if (dst->sa.ss_family == AF_INET6)
		return inet_ntop(AF_INET6, &v6->sin6_addr, out, olen) != NULL;

	return false;
}

bool sip_dst_eq(const struct sip_txn_dst *a, const struct sip_txn_dst *b)
{
	const struct sockaddr_in *a4 = (const struct sockaddr_in *)&a->sa;
	const struct sockaddr_in *b4 = (const struct sockaddr_in *)&b->sa;
	const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&a->sa;
	const struct sockaddr_in6 *b6 = (const struct sockaddr_in6 *)&b->sa;

	if (a->sa.ss_family != b->sa.ss_family)
		return false;
	if (a->sa.ss_family == AF_INET)
		return a4->sin_addr.s_addr == b4->sin_addr.s_addr &&
		       a4->sin_port == b4->sin_port;
	if (a->sa.ss_family == AF_INET6)
		return !memcmp(&a6->sin6_addr, &b6->sin6_addr, sizeof(a6->sin6_addr)) &&
		       a6->sin6_port == b6->sin6_port;

	return false;
}
