// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The media sockets a backend owns when its engine makes none. A backend binds
 * the advertised local pair, marks it from the two DSCP fields, connects it to
 * the far end and hands the numbers to its engine, which dups them. The engine
 * type never appears here; the record is struct hal_media_sock.
 */
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hal_media.h"

#include "../posix.h"
#include "../trace.h"

int hal_media_dscp_mark(int fd, int family, uint8_t dscp)
{
	int tos = ((int)dscp << 2) & 0xfc;

	if (sock_tos_set(fd, family, tos) == 0)
		return 0;

	TRACE(TRACE_HAL, "media mark 0x%02x: %s", (unsigned)tos, strerror(errno));

	return -1;
}

void hal_media_sock_init(struct hal_media_sock *s)
{
	s->rtp_fd = -1;
	s->rtcp_fd = -1;
}

void hal_media_sock_close(struct hal_media_sock *s)
{
	if (s->rtp_fd >= 0)
		close(s->rtp_fd);
	if (s->rtcp_fd >= 0)
		close(s->rtcp_fd);

	/* The record goes back to "no sockets": the identity and the marks a
	 * later program reads are cleared, and the DSCP returns to the sentinel
	 * so the next mark is applied and not skipped as already present. */
	memset(s, 0, sizeof(*s));
	s->rtp_fd = -1;
	s->rtcp_fd = -1;
	s->rtp_dscp = HAL_MEDIA_DSCP_NONE;
	s->rtcp_dscp = HAL_MEDIA_DSCP_NONE;
}

void hal_media_rtcp_peer(const struct hal_media *m, struct hal_addr *out)
{
	*out = m->remote;

	if (!m->rtcp_remote.ipver)
		return;

	out->ipver = m->rtcp_remote.ipver;
	memcpy(out->addr, m->rtcp_remote.addr, sizeof(out->addr));
}

/*
 * A bind failure closes what it made, names the port and gives up: the caller
 * owns the failure. The mark is not applied here; hal_media_sock_tos_refresh
 * owns every mark, so there is one place that decides whether a DSCP reached
 * the socket.
 *
 * CLOEXEC because the firewall helper and the event publisher fork children
 * that must not inherit a voice socket; an engine dups these with
 * F_DUPFD_CLOEXEC, so its copies already carry it.
 */
static int media_fd_bind(const struct hal_addr *local, uint16_t port,
			 const char *bind_dev)
{
	size_t dev_len = strnlen(bind_dev, HAL_IFNAME_MAX);
	int family = addr_family(local);
	union sockaddr_any sa;
	socklen_t len;
	int on = 1;
	int fd;

	fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		TRACE(TRACE_HAL, "media socket: %s", strerror(errno));
		return -1;
	}

	/*
	 * An AF_INET6 media socket takes no IPv4 mapped source. hal_addr names
	 * one family per description and rule 4 refuses a mixed one, so a mapped
	 * peer would be an address the description never named. The failure is
	 * hard, unlike the SO_BINDTODEVICE below.
	 */
	if (family == AF_INET6 &&
	    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) != 0) {
		TRACE(TRACE_HAL, "media v6only: %s", strerror(errno));
		close(fd);

		return -1;
	}

	if (dev_len && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, bind_dev,
				  (socklen_t)dev_len) != 0)
		TRACE(TRACE_HAL, "media bind to device %.*s: %s", (int)dev_len,
		      bind_dev, strerror(errno));

	len = sockaddr_fill(&sa, local, port);
	if (bind(fd, &sa.sa, len) == 0)
		return fd;

	TRACE(TRACE_HAL, "media bind port %u: %s", port, strerror(errno));
	close(fd);

	return -1;
}

void hal_media_sock_tos_refresh(struct hal_media_sock *s,
				const struct hal_media *m)
{
	int family = addr_family(&s->local);

	media_tos_apply(s->rtp_fd, family, m->rtp_dscp, &s->rtp_dscp,
			hal_media_dscp_mark);
	media_tos_apply(s->rtcp_fd, family, m->rtcp_dscp, &s->rtcp_dscp,
			hal_media_dscp_mark);
}

enum hal_status hal_media_sock_open(struct hal_media_sock *s,
				    const struct hal_media *m)
{
	s->rtp_fd = media_fd_bind(&m->local, m->local.port, m->bind_dev);
	s->rtcp_fd = media_fd_bind(&m->local, rtcp_port_of(&m->local),
				   m->bind_dev);
	if (s->rtp_fd < 0 || s->rtcp_fd < 0) {
		hal_media_sock_close(s);
		return HAL_FATAL;
	}

	s->local = m->local;
	memcpy(s->bind_dev, m->bind_dev, sizeof(s->bind_dev));
	/* Nothing is marked yet, so the refresh marks both and records only what
	 * actually reached the socket. */
	s->rtp_dscp = HAL_MEDIA_DSCP_NONE;
	s->rtcp_dscp = HAL_MEDIA_DSCP_NONE;
	hal_media_sock_tos_refresh(s, m);

	return HAL_OK;
}

/*
 * The connect makes the stream work both ways: an engine sends with no
 * destination (a connected descriptor), and the connect also filters the far
 * end on receive. The two descriptors can take two different peers; the record
 * keeps both, so a description that moved the RTCP address alone still
 * reconnects.
 */
enum hal_status hal_media_sock_connect(struct hal_media_sock *s,
				       const struct hal_media *m)
{
	struct hal_addr rtcp;

	hal_media_rtcp_peer(m, &rtcp);

	if (media_fd_connect(s->rtp_fd, &m->remote, m->remote.port) != 0 ||
	    media_fd_connect(s->rtcp_fd, &rtcp, rtcp_port_of(&rtcp)) != 0) {
		TRACE(TRACE_HAL, "media connect port %u: %s", m->remote.port,
		      strerror(errno));
		return HAL_FATAL;
	}

	s->remote = m->remote;
	s->rtcp_remote = rtcp;

	return HAL_OK;
}

enum hal_status hal_media_validate_addrs(const struct hal_media *m,
					 uint32_t features)
{
	/*
	 * One socket has one family: a version the record does not define, two
	 * ends in different families, or a separate RTCP address in a third, is
	 * a shape no socket can hold.
	 */
	if (!ipver_known(m->local.ipver) || !ipver_known(m->remote.ipver) ||
	    m->local.ipver != m->remote.ipver ||
	    (m->rtcp_remote.ipver && m->rtcp_remote.ipver != m->remote.ipver)) {
		TRACE(TRACE_HAL, "media local ipver %u remote ipver %u rtcp ipver "
		      "%u: one stream carries one family", m->local.ipver,
		      m->remote.ipver, m->rtcp_remote.ipver);
		return HAL_UNSUPPORTED;
	}

	if (m->local.ipver == 6 && !(features & HAL_FEAT_IPV6_MEDIA)) {
		TRACE(TRACE_HAL, "media ipver 6 on a backend that does not carry "
		      "HAL_FEAT_IPV6_MEDIA");
		return HAL_UNSUPPORTED;
	}

	/* Port 0 would take an ephemeral pair, which is the very thing this form
	 * exists to stop: the SDP names a port the far end sends to. */
	if (!m->local.port) {
		TRACE(TRACE_HAL, "media program carries no local port");
		return HAL_FATAL;
	}

	/* A zero remote port is the one shape that would skip the connect, and
	 * an unconnected media socket takes any source and can send to none. */
	if (!m->remote.port) {
		TRACE(TRACE_HAL, "media program carries no remote port");
		return HAL_FATAL;
	}

	/*
	 * The derived RTCP port wraps at 65535 silently: port 65535 with no
	 * rtcp port of its own derives 65536, which the uint16 truncates to 0,
	 * and both the ephemeral bind and the port-0 connect are silent. The
	 * shape is refused here, as HAL_FATAL for the reason a missing port is
	 * one: the core should never have built this description.
	 */
	if (!rtcp_port_of(&m->local) || !rtcp_port_of(&m->remote)) {
		TRACE(TRACE_HAL, "media rtcp port derives from %u and wraps to "
		      "zero", m->local.port);
		return HAL_FATAL;
	}

	return HAL_OK;
}
