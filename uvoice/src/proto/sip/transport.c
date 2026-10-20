// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * UDP transport for SIP. See sip/transport.h for the contracts.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "posix.h"
#include "str.h"
#include "proto/sip/transport.h"
#include "proto/sip/tap.h"

#define SIP_RX_BURST_MAX	32

static void sock_read(struct uloop_fd *ufd, unsigned int events);

static int family_of(const struct sip_sock *sk)
{
	return sk->cfg.family ? sk->cfg.family : AF_INET;
}

/* --------------------------------------------------------------- handles */

uint32_t sip_sock_handle(const struct sip_sock *sk)
{
	if (!sk || !sk->in_use)
		return SIP_HANDLE_NONE;

	return sip_handle_make(sk->index, sk->gen);
}

struct sip_sock *sip_sock_get(struct sip_transport *tp, uint32_t handle)
{
	struct sip_sock *sk;
	unsigned int idx;

	if (!tp || handle == SIP_HANDLE_NONE)
		return NULL;

	idx = sip_handle_index(handle);
	if (idx >= SIP_SOCK_MAX)
		return NULL;

	sk = &tp->sock[idx];
	if (!sk->in_use || sk->gen != sip_handle_gen(handle))
		return NULL;

	return sk;
}

unsigned int sip_sock_count(const struct sip_transport *tp)
{
	return tp->active;
}

uint16_t sip_sock_port(const struct sip_sock *sk)
{
	return sk->port;
}

const char *sip_sock_iface(const struct sip_sock *sk)
{
	return sk->iface;
}

bool sip_sock_up(const struct sip_sock *sk)
{
	return sk->up;
}

bool sip_sock_local_host(const struct sip_sock *sk, char *out, size_t olen)
{
	if (!sk->bound[0] || olen <= strlen(sk->bound))
		return false;

	memcpy(out, sk->bound, strlen(sk->bound) + 1);

	return true;
}

const struct sip_sock_stat *sip_transport_stat(const struct sip_transport *tp)
{
	return &tp->total;
}

/* ------------------------------------------------------------------ bind */

static void sock_fd_drop(struct sip_sock *sk)
{
	if (sk->ufd.registered)
		uloop_fd_delete(&sk->ufd);

	if (sk->ufd.fd >= 0)
		close(sk->ufd.fd);

	sk->ufd.fd = -1;
	sk->up = false;
	sk->tos_cur = SIP_TOS_UNSET;
}

/* The interface bind comes before the address bind, for the reason
 * sock_iface_bind() records. */
static int sock_fd_open(struct sip_sock *sk)
{
	struct sockaddr_storage sa;
	int family = family_of(sk);
	socklen_t slen;
	int fd;
	int on = 1;

	if (!addr_pton(family, sk->addr, &sa)) {
		errno = EINVAL;
		return -1;
	}

	sa_port_set(&sa, sk->cfg.port);

	fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
	if (fd < 0)
		return -1;

	if (family == AF_INET6 &&
	    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) < 0)
		goto fail;

	if (!sock_iface_bind(fd, sk->iface))
		goto fail;

	if (bind(fd, (struct sockaddr *)&sa, sa_len_of(family)) < 0)
		goto fail;

	slen = sizeof(sa);
	if (getsockname(fd, (struct sockaddr *)&sa, &slen) < 0)
		goto fail;

	sk->port = sa_port_get(&sa);
	if (!sa_text(&sa, sk->bound, sizeof(sk->bound)))
		sk->bound[0] = 0;

	sk->ufd.fd = fd;
	sk->ufd.cb = sock_read;
	if (uloop_fd_add(&sk->ufd, ULOOP_READ) < 0)
		goto fail;

	sk->up = true;
	sk->tos_cur = SIP_TOS_UNSET;

	return 0;

fail:
	{
		int err = errno;

		close(fd);
		sk->ufd.fd = -1;
		errno = err;
	}

	return -1;
}

static bool sock_key_eq(const struct sip_sock *sk, const struct sip_sock_cfg *cfg,
			const char *addr, const char *iface)
{
	if (family_of(sk) != (cfg->family ? cfg->family : AF_INET))
		return false;
	if (sk->cfg.port != cfg->port)
		return false;
	if (strcmp(sk->addr, addr))
		return false;

	return !strcmp(sk->iface, iface);
}

uint32_t sip_sock_open(struct sip_transport *tp, const struct sip_sock_cfg *cfg)
{
	const char *addr = cfg->addr ? cfg->addr : "";
	const char *iface = cfg->iface ? cfg->iface : "";
	struct sip_sock *sk = NULL;
	unsigned int i;

	if (strlen(addr) >= SIP_SOCK_HOST_MAX || strlen(iface) >= SIP_SOCK_IFACE_MAX) {
		errno = EINVAL;
		return SIP_HANDLE_NONE;
	}

	/*
	 * An ephemeral bind is not an identity: two accounts that both ask for
	 * "any port" want two ports, so port 0 never shares a socket.
	 */
	for (i = 0; cfg->port && i < SIP_SOCK_MAX; i++) {
		if (!tp->sock[i].in_use || !sock_key_eq(&tp->sock[i], cfg, addr, iface))
			continue;

		tp->sock[i].refs++;
		return sip_sock_handle(&tp->sock[i]);
	}

	for (i = 0; i < SIP_SOCK_MAX; i++) {
		if (tp->sock[i].in_use)
			continue;

		sk = &tp->sock[i];
		break;
	}

	if (!sk) {
		tp->denied++;
		errno = ENOSPC;
		return SIP_HANDLE_NONE;
	}

	memset(&sk->stat, 0, sizeof(sk->stat));
	sk->tp = tp;
	sk->cfg = *cfg;
	sk->cfg.addr = NULL;
	sk->cfg.iface = NULL;
	sk->cfg.family = cfg->family ? cfg->family : AF_INET;
	sk->cfg.rx_max = cfg->rx_max && cfg->rx_max < SIP_RX_BUF_MAX ? cfg->rx_max
								     : SIP_RX_BUF_MAX;
	str_copy(sk->addr, sizeof(sk->addr), addr);
	str_copy(sk->iface, sizeof(sk->iface), iface);
	sk->ufd.fd = -1;
	sk->bound[0] = 0;
	sk->port = 0;

	if (sock_fd_open(sk) < 0)
		return SIP_HANDLE_NONE;

	sk->in_use = true;
	sk->refs = 1;
	tp->active++;

	SIP_WIRE_TRACE("transport: socket %u bound to %s:%u iface '%s'", sk->index,
		       sk->bound, sk->port, sk->iface);

	return sip_sock_handle(sk);
}

void sip_sock_close(struct sip_transport *tp, uint32_t handle)
{
	struct sip_sock *sk = sip_sock_get(tp, handle);

	if (!sk || !sk->refs)
		return;

	if (--sk->refs)
		return;

	sock_fd_drop(sk);
	sk->in_use = false;
	sk->gen++;
	if (!sk->gen)
		sk->gen = 1;
	tp->active--;

	SIP_WIRE_TRACE("transport: socket %u closed", sk->index);
}

bool sip_sock_iface_set(struct sip_transport *tp, uint32_t handle, const char *iface)
{
	struct sip_sock *sk = sip_sock_get(tp, handle);
	const char *name = iface ? iface : "";

	if (!sk || strlen(name) >= sizeof(sk->iface))
		return false;

	if (!strcmp(sk->iface, name))
		return true;

	/* Only sock_fd_open() reads this, so the running fd keeps the old device
	 * and the move lands on the next rebind (sip/transport.h). */
	str_copy(sk->iface, sizeof(sk->iface), name);
	SIP_WIRE_TRACE("transport: socket %u takes device '%s' on its next rebind",
		       sk->index, name);

	return true;
}

bool sip_sock_rebind(struct sip_transport *tp, uint32_t handle)
{
	struct sip_sock *sk = sip_sock_get(tp, handle);

	if (!sk)
		return false;

	sock_fd_drop(sk);
	sk->stat.rebinds++;
	tp->total.rebinds++;

	if (sock_fd_open(sk) < 0) {
		SIP_WIRE_TRACE("transport: socket %u rebind failed: %s", sk->index,
			       strerror(errno));
		return false;
	}

	SIP_WIRE_TRACE("transport: socket %u rebound to %s:%u", sk->index, sk->bound,
		       sk->port);

	return true;
}

/* down_only leaves a working socket alone, so its conntrack tuple stands. */
static unsigned int pool_rebind(struct sip_transport *tp, bool down_only)
{
	unsigned int failed = 0;
	unsigned int i;

	for (i = 0; i < SIP_SOCK_MAX; i++) {
		struct sip_sock *sk = &tp->sock[i];

		if (!sk->in_use || (down_only && sk->up))
			continue;

		if (!sip_sock_rebind(tp, sip_sock_handle(sk)))
			failed++;
	}

	return failed;
}

unsigned int sip_transport_rebind_all(struct sip_transport *tp)
{
	return pool_rebind(tp, false);
}

unsigned int sip_transport_rebind_down(struct sip_transport *tp)
{
	return pool_rebind(tp, true);
}

unsigned int sip_transport_down_count(const struct sip_transport *tp)
{
	unsigned int down = 0;
	unsigned int i;

	for (i = 0; i < SIP_SOCK_MAX; i++) {
		if (tp->sock[i].in_use && !tp->sock[i].up)
			down++;
	}

	return down;
}

/* ------------------------------------------------------- local addresses */

bool sip_sock_source_host(const struct sip_sock *sk, const struct sip_txn_dst *dst,
			  char *out, size_t olen)
{
	struct sockaddr_storage sa;
	socklen_t slen = sizeof(sa);
	int family = family_of(sk);
	bool ok = false;
	int fd;

	if (sk->addr[0])
		return sip_sock_local_host(sk, out, olen);

	if (!dst || dst->sa.ss_family != (sa_family_t)family)
		return false;

	fd = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
	if (fd < 0)
		return false;

	if (!sock_iface_bind(fd, sk->iface))
		goto out;

	if (connect(fd, (const struct sockaddr *)&dst->sa, dst->sa_len) < 0)
		goto out;

	if (getsockname(fd, (struct sockaddr *)&sa, &slen) < 0)
		goto out;

	ok = sa_text(&sa, out, olen);

out:
	close(fd);

	return ok;
}

bool sip_sock_via_write(struct sip_transport *tp, uint32_t handle, struct sip_buf *b,
			const struct sip_txn_dst *dst, const char *branch)
{
	struct sip_sock *sk = sip_sock_get(tp, handle);
	char host[SIP_SOCK_HOST_MAX];

	if (!sk)
		return false;

	if (!sip_sock_source_host(sk, dst, host, sizeof(host)) &&
	    !sip_sock_local_host(sk, host, sizeof(host)))
		return false;

	return hdr_via_write(b, "UDP", host, sk->port, branch, true);
}

/* inet_pton() converts a wildcard bind of "0.0.0.0" or "::" to all zero. */
static void sock_endpoints_get(const struct sip_sock *sk,
			       const struct sip_txn_dst *peer,
			       struct sip_tap_endpoints *ep)
{
	int family = family_of(sk);

	memset(ep, 0, sizeof(*ep));
	ep->local.ipver = family == AF_INET6 ? 6 : 4;
	inet_pton(family, sk->bound, ep->local.addr);
	ep->local.port = sk->port;
	sip_tap_endpoint_set(&ep->peer, (const struct sockaddr *)&peer->sa);
	ep->kind = TRACE_PKT_SIP_UDP;
}

/* ------------------------------------------------------------------ send */

static bool tos_apply(struct sip_sock *sk, uint8_t tos)
{
	if (sk->tos_cur == (uint32_t)tos)
		return true;

	if (sock_tos_set(sk->ufd.fd, family_of(sk), tos) < 0) {
		sk->tos_cur = SIP_TOS_UNSET;
		sk->stat.tos_err++;
		sk->tp->total.tos_err++;
		return false;
	}

	sk->tos_cur = tos;

	return true;
}

int sip_sock_send(struct sip_transport *tp, uint32_t handle, uint8_t tos, const void *buf,
		  size_t len, const struct sip_txn_dst *dst)
{
	struct sip_sock *sk = sip_sock_get(tp, handle);
	char host[SIP_SOCK_HOST_MAX] = "";
	ssize_t n;

	/* A dead handle cannot be charged to a socket, but it is still a failed
	 * transmission and the caller sees it in the total. */
	if (!sk) {
		tp->total.tx_err++;
		return -1;
	}

	/* A down socket (a rebind that could not take its port again), a caller
	 * with no destination, or a destination of the wrong family is a failed
	 * send: count it in tx_err rather than returning silently. */
	if (!sk->up || !dst || !dst->sa_len ||
	    dst->sa.ss_family != (sa_family_t)family_of(sk)) {
		sk->stat.tx_err++;
		tp->total.tx_err++;
		return -1;
	}

	/*
	 * A refused TOS is not a reason to drop the request: the datagram still
	 * carries the call, it just carries the wrong DSCP. The failure is
	 * counted and the next send tries again.
	 */
	tos_apply(sk, tos);

	if (sip_tap_cb) {
		struct sip_tap_endpoints ep;

		sock_endpoints_get(sk, dst, &ep);
		sip_tap_cb(true, &ep, buf, len);
	}

	do {
		n = sendto(sk->ufd.fd, buf, len, MSG_DONTWAIT,
			   (const struct sockaddr *)&dst->sa, dst->sa_len);
	} while (n < 0 && errno == EINTR);

	if (n < 0) {
		int err = errno;

		sk->stat.tx_err++;
		tp->total.tx_err++;
		sip_dst_host(dst, host, sizeof(host));
		SIP_WIRE_TRACE("tx %zu bytes to %s:%u failed: %s", len, host,
			       sip_dst_port(dst), strerror(err));
		return -1;
	}

	sk->stat.tx++;
	sk->stat.tx_bytes += (uint64_t)n;
	tp->total.tx++;
	tp->total.tx_bytes += (uint64_t)n;

	if (sip_wire_host &&
	    sip_wire_host->trace_active(sip_wire_host->ctx, TRACE_SIP)) {
		sip_dst_host(dst, host, sizeof(host));
		sip_wire_host->trace_write(sip_wire_host->ctx, TRACE_SIP,
					   "tx %zd bytes to %s:%u tos 0x%02x", n,
					   host, sip_dst_port(dst), tos);
	}

	return (int)n;
}

/* --------------------------------------------------------------- receive */

static void rx_dispatch(struct sip_sock *sk, size_t len, const struct sip_txn_dst *src)
{
	struct sip_transport *tp = sk->tp;
	enum sip_parse_status st;
	struct sip_msg msg;
	char host[SIP_SOCK_HOST_MAX] = "";

	if (sip_tap_cb) {
		struct sip_tap_endpoints ep;

		sock_endpoints_get(sk, src, &ep);
		sip_tap_cb(false, &ep, tp->rx, len);
	}

	sip_dst_host(src, host, sizeof(host));

	st = msg_parse(&msg, tp->rx, len);
	if (st == SIP_PARSE_ERR_EMPTY) {
		sk->stat.ping++;
		tp->total.ping++;
		SIP_WIRE_TRACE("rx keepalive from %s:%u", host, sip_dst_port(src));

		if (tp->cb.ping)
			tp->cb.ping(tp, sip_sock_handle(sk), len, src, tp->priv);

		return;
	}

	if (st != SIP_PARSE_OK) {
		sk->stat.parse_err++;
		tp->total.parse_err++;
		SIP_WIRE_TRACE("rx %zu bytes from %s:%u refused: %s", len, host,
			       sip_dst_port(src), sip_parse_status_name(st));
		return;
	}

	sk->stat.rx++;
	sk->stat.rx_bytes += len;
	tp->total.rx++;
	tp->total.rx_bytes += len;

	if (msg.is_request)
		SIP_WIRE_TRACE("rx %zu bytes from %s:%u: " SIP_STR_PRI " " SIP_STR_PRI,
			       len, host, sip_dst_port(src), SIP_STR_ARG(msg.method_name),
			       SIP_STR_ARG(msg.ruri));
	else
		SIP_WIRE_TRACE("rx %zu bytes from %s:%u: %u " SIP_STR_PRI, len, host,
			       sip_dst_port(src), msg.status, SIP_STR_ARG(msg.reason));

	if (tp->cb.msg)
		tp->cb.msg(tp, sip_sock_handle(sk), &msg, src, tp->priv);
}

static void sock_read(struct uloop_fd *ufd, unsigned int events)
{
	struct sip_sock *sk = container_of(ufd, struct sip_sock, ufd);
	struct sip_transport *tp = sk->tp;
	struct sip_txn_dst src;
	struct msghdr mh;
	struct iovec iov;
	unsigned int burst;
	ssize_t n;

	/*
	 * The socket is level triggered, so a bounded burst gives the timers and
	 * the HAL event socket a turn between floods instead of letting one
	 * sender own the loop.
	 */
	for (burst = 0; burst < SIP_RX_BURST_MAX; burst++) {
		memset(&mh, 0, sizeof(mh));
		memset(&src, 0, sizeof(src));
		iov.iov_base = tp->rx;
		iov.iov_len = sk->cfg.rx_max;
		mh.msg_iov = &iov;
		mh.msg_iovlen = 1;
		mh.msg_name = &src.sa;
		mh.msg_namelen = sizeof(src.sa);

		/*
		 * MSG_TRUNC makes recvmsg report the real datagram size, so a
		 * datagram larger than the limit is recognised instead of being
		 * parsed as a message that happens to end early.
		 */
		n = recvmsg(ufd->fd, &mh, MSG_DONTWAIT | MSG_TRUNC);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (n < 0) {
			sk->stat.rx_err++;
			tp->total.rx_err++;
			SIP_WIRE_TRACE("rx failed on socket %u: %s", sk->index,
				       strerror(errno));
			return;
		}

		src.sa_len = mh.msg_namelen;

		if ((size_t)n > sk->cfg.rx_max || (mh.msg_flags & MSG_TRUNC)) {
			char host[SIP_SOCK_HOST_MAX] = "";

			sk->stat.truncated++;
			tp->total.truncated++;
			sip_dst_host(&src, host, sizeof(host));
			SIP_WIRE_TRACE("rx dropped %zd bytes from %s:%u, over the %zu "
				       "byte limit", n, host, sip_dst_port(&src), sk->cfg.rx_max);
			continue;
		}

		rx_dispatch(sk, (size_t)n, &src);
	}
}

/* ------------------------------------------------------------- lifecycle */

/*
 * The receive buffer is left as it is: sock_read writes a datagram into it
 * before rx_dispatch reads it, so zeroing the 64 KB would only make it
 * resident and locked before the first datagram arrives.
 */
_Static_assert(sizeof(struct sip_transport) - offsetof(struct sip_transport, rx) -
	       SIP_RX_BUF_MAX < _Alignof(struct sip_transport),
	       "rx must be the last member: nothing after it is zeroed");

void sip_transport_init(struct sip_transport *tp, const struct sip_transport_cb *cb, void *priv)
{
	unsigned int i;

	memset(tp, 0, offsetof(struct sip_transport, rx));
	if (cb)
		tp->cb = *cb;
	tp->priv = priv;

	for (i = 0; i < SIP_SOCK_MAX; i++) {
		tp->sock[i].index = i;
		tp->sock[i].gen = 1;
		tp->sock[i].ufd.fd = -1;
		tp->sock[i].tos_cur = SIP_TOS_UNSET;
	}
}

void sip_transport_done(struct sip_transport *tp)
{
	unsigned int i;

	for (i = 0; i < SIP_SOCK_MAX; i++) {
		if (!tp->sock[i].in_use)
			continue;

		sock_fd_drop(&tp->sock[i]);
		tp->sock[i].in_use = false;
		tp->sock[i].refs = 0;
	}

	tp->active = 0;
}

/* ------------------------------------------------- transaction environment */

static int udp_env_send(struct sip_txn_env *env, const void *buf, size_t len,
			const struct sip_txn_dst *dst)
{
	struct sip_udp_env *ue = env->base.priv;

	return sip_sock_send(ue->tp, ue->sock, ue->tos, buf, len, dst);
}

static void udp_env_trace(struct proto_timer_env *env, const char *msg)
{
	SIP_WIRE_TRACE("%s", msg);
}

void sip_udp_env_init(struct sip_udp_env *ue, struct sip_transport *tp, uint32_t sock,
		      uint8_t tos)
{
	memset(ue, 0, sizeof(*ue));
	ue->tp = tp;
	ue->sock = sock;
	ue->tos = tos;

	ue->env.base.priv = ue;
	ue->env.send = udp_env_send;
	ue->env.base.trace = udp_env_trace;
	sip_txn_env_uloop_init(&ue->env);
}

void sip_udp_env_tos_set(struct sip_udp_env *ue, uint8_t tos)
{
	ue->tos = tos;
}

void sip_udp_env_sock_set(struct sip_udp_env *ue, uint32_t sock)
{
	ue->sock = sock;
}
