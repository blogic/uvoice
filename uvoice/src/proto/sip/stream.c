// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * TCP and TLS transport for SIP. See sip/stream.h for the contracts.
 *
 * One pool, no allocation. Eight connection slots and four listener slots live
 * inside struct sip_stream, and every buffer a connection needs lives inside
 * its slot. Nothing on this path calls malloc, which is why the receive slab
 * and the transmit staging buffer are sized once in the header and never grown.
 *
 * The state picks the step, not the event bits. uloop reports an epoll error
 * with neither ULOOP_READ nor ULOOP_WRITE set, and a refused connect is exactly
 * that event. conn_io() therefore dispatches on enum sip_conn_state and lets
 * each step ask the socket what happened. A read attempt that had nothing to
 * read costs one EAGAIN, which is cheaper than losing a refusal.
 *
 * The armed mask is the record layer's answer. struct uloop_fd holds the mask
 * it was registered with, so a TLS operation that answered SIP_TLS_WANT_WRITE
 * keeps ULOOP_WRITE armed without a second copy of that state in struct
 * sip_conn. Only the uloop step recomputes the whole mask, from results it
 * just obtained; a send from outside the loop may add ULOOP_WRITE for the
 * bytes it staged and never clears what the record layer asked for.
 *
 * Handles across upcalls. Both upcalls of struct sip_stream_cb can close the
 * connection they are told about, and the close can hand the slot straight to
 * another connection. Every path that continues after an upcall therefore holds
 * the handle, resolves it again and stops when it no longer names the same
 * connection (sip/stream.h "Rx reentrancy"). conn_drop() reports SIP_CONN_FREE
 * as its last statement for the same reason.
 *
 * What a slot remembers after a close. The key, the backoff interval and the
 * backoff deadline survive, because the reconnect backoff belongs to the
 * destination and not to the connection that failed to reach it. Everything
 * else is set when the slot is taken again.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "posix.h"
#include "str.h"
#include "proto/sip/stream.h"
#include "proto/sip/tap.h"

/*
 * The sweep period. Every deadline this module owns is measured in tens of
 * seconds (SIP_STREAM_CONNECT_MS) or minutes (SIP_STREAM_IDLE_MS), so one
 * second of resolution is far finer than any of them and the pool is eight
 * slots, which makes the sweep itself free. The tick runs only while a slot is
 * in use or holds a running backoff, so a daemon that registers over UDP alone
 * never wakes for it.
 */
#define SIP_STREAM_TICK_MS	1000

/* accepts taken per readable event, so one flood cannot own the loop */
#define SIP_STREAM_ACCEPT_BURST	8

/*
 * Read passes taken per readable event, for the same reason: uvoiced runs one
 * uloop_run(), so a peer that keeps its socket fed would otherwise hold the
 * tick, every transaction timer and every other connection inside this loop.
 * The socket is level triggered, so what the bound leaves behind is collected
 * on the next pass round the loop, and a full 16 KB TLS record needs two
 * passes out of the eight.
 */
#define SIP_STREAM_RX_BURST	8

/*
 * A message stages into all but the last SIP_FRAME_PONG_LEN bytes of the
 * transmit buffer. Those two bytes belong to the RFC 5626 5.4 keepalive answer
 * by reservation: a buffer that a burst of messages filled to the brim must
 * never be the reason a ping goes unanswered, because the peer that stops
 * seeing pongs drops the connection and takes every one of those messages with
 * it (sip/stream.h "Ping is answered inside the connection"). The queue the
 * header describes is therefore SIP_STREAM_MSG_MAX deep for a caller, and the
 * remaining two bytes are never a caller's to use.
 */
#define SIP_STREAM_MSG_MAX	(SIP_STREAM_TX_MAX - SIP_FRAME_PONG_LEN)

enum rx_step {
	RX_DATA,		/* bytes landed in the framer */
	RX_IDLE,		/* nothing to read right now */
	RX_EOF,			/* the peer ended the stream in order */
	RX_FAIL,		/* terminal read failure */
};

static void conn_io(struct uloop_fd *ufd, unsigned int events);
static void listen_io(struct uloop_fd *ufd, unsigned int events);
static void conn_drop(struct sip_conn *c);
static void conn_up(struct sip_conn *c);
static void tick_arm(struct sip_stream *st);

/* ---------------------------------------------------------------- helpers */

/* the two families this layer carries (sip/stream.h "Both families") */
static bool family_ok(int family)
{
	return family == AF_INET || family == AF_INET6;
}

/* 0 takes AF_INET, as struct sip_sock_cfg does (sip/stream.h) */
static int family_of(const struct sip_stream_listen_cfg *cfg)
{
	return cfg->family ? cfg->family : AF_INET;
}

/*
 * A kernel that refuses one keepalive knob must not cost the connection: the
 * transaction timers above still bound every request, so the probe schedule is
 * an improvement and never a precondition.
 */
static void sock_keepalive(int fd)
{
	int on = 1;
	int idle = SIP_STREAM_KA_IDLE_S;
	int intvl = SIP_STREAM_KA_INTVL_S;
	int cnt = SIP_STREAM_KA_CNT;

	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

static bool key_peer_ok(const struct sip_stream_key *key)
{
	int family = key->peer.sa.ss_family;

	/* an address family this layer does not carry is refused here and
	 * counted by the caller */
	if (!family_ok(family) || key->peer.sa_len < sa_len_of(family))
		return false;

	return sip_dst_port(&key->peer) != 0;
}

/* -------------------------------------------------------------- accessors */

const char *sip_stream_kind_name(enum sip_stream_kind kind)
{
	switch (kind) {
	case SIP_STREAM_TCP:
		return "tcp";
	case SIP_STREAM_TLS:
		return "tls";
	case SIP_STREAM_KIND_MAX:
		break;
	}

	return "unknown";
}

const char *sip_conn_state_name(enum sip_conn_state state)
{
	switch (state) {
	case SIP_CONN_FREE:
		return "free";
	case SIP_CONN_CONNECTING:
		return "connecting";
	case SIP_CONN_HANDSHAKE:
		return "handshake";
	case SIP_CONN_UP:
		return "up";
	case SIP_CONN_CLOSING:
		return "closing";
	}

	return "unknown";
}

/*
 * The local identity, the peer and the kind, and nothing else. sip_dst_eq()
 * reads the address and the port alone, so the SIP_DST_F_NO_CONNECT flag a
 * destination carries never splits one connection into two (sip/stream.h).
 */
bool sip_stream_key_eq(const struct sip_stream_key *a, const struct sip_stream_key *b)
{
	if (!a || !b)
		return false;

	if (a->kind != b->kind || a->local_port != b->local_port)
		return false;

	if (strncmp(a->iface, b->iface, SIP_SOCK_IFACE_MAX))
		return false;

	return sip_dst_eq(&a->peer, &b->peer);
}

uint32_t sip_stream_conn_handle(const struct sip_conn *c)
{
	if (!c || !c->in_use)
		return SIP_HANDLE_NONE;

	return sip_handle_make(c->index, c->gen);
}

struct sip_conn *sip_stream_conn_get(struct sip_stream *st, uint32_t handle)
{
	struct sip_conn *c;
	unsigned int idx;

	if (!st || handle == SIP_HANDLE_NONE)
		return NULL;

	idx = sip_handle_index(handle);
	if (idx >= SIP_STREAM_CONN_MAX)
		return NULL;

	c = &st->conn[idx];
	if (!c->in_use || c->gen != sip_handle_gen(handle))
		return NULL;

	return c;
}

unsigned int sip_stream_conn_count(const struct sip_stream *st)
{
	return st ? st->active : 0;
}

size_t sip_stream_conn_queued(const struct sip_conn *c)
{
	if (!c || !c->in_use)
		return 0;

	return c->tx_len - c->tx_off;
}

static uint32_t listener_handle(const struct sip_listener *l)
{
	if (!l || !l->in_use)
		return SIP_HANDLE_NONE;

	return sip_handle_make(l->index, l->gen);
}

struct sip_listener *sip_stream_listener_get(struct sip_stream *st, uint32_t handle)
{
	struct sip_listener *l;
	unsigned int idx;

	if (!st || handle == SIP_HANDLE_NONE)
		return NULL;

	idx = sip_handle_index(handle);
	if (idx >= SIP_STREAM_LISTEN_MAX)
		return NULL;

	l = &st->listen[idx];
	if (!l->in_use || l->gen != sip_handle_gen(handle))
		return NULL;

	return l;
}

unsigned int sip_stream_listen_count(const struct sip_stream *st)
{
	unsigned int count = 0;
	unsigned int i;

	if (!st)
		return 0;

	for (i = 0; i < SIP_STREAM_LISTEN_MAX; i++) {
		if (st->listen[i].in_use)
			count++;
	}

	return count;
}

/*
 * The statistics live inside the pool, so the pointer is the answer even
 * before a counter moves. NULL in, NULL out, to guard like every other
 * accessor.
 */
const struct sip_stream_stat *sip_stream_stat(const struct sip_stream *st)
{
	return st ? &st->stat : NULL;
}

/*
 * For a TLS key the stored name has to match, because a connection handshaken
 * for name A carries name A's certificate and a send for name B would reach the
 * wrong virtual host (sip/stream.h "SNI splits a TLS connection"). A TCP key
 * has no server_name at all, so it ignores the argument.
 */
static bool conn_sni_eq(const struct sip_conn *c, uint8_t kind, const char *sni)
{
	if (kind != SIP_STREAM_TLS)
		return true;

	return !strncmp(c->sni, sni ? sni : "", SIP_STREAM_NAME_MAX);
}

struct sip_conn *sip_stream_conn_find(struct sip_stream *st, const struct sip_stream_key *key,
				      const char *sni)
{
	unsigned int i;

	if (!st || !key)
		return NULL;

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		struct sip_conn *c = &st->conn[i];

		if (!c->in_use || !sip_stream_key_eq(&c->key, key))
			continue;

		if (conn_sni_eq(c, key->kind, sni))
			return c;
	}

	return NULL;
}

/* ------------------------------------------------------------- uloop mask */

static unsigned int conn_mask_get(const struct sip_conn *c)
{
	return c->ufd.registered ? c->ufd.flags : 0;
}

static void conn_mask_set(struct sip_conn *c, unsigned int mask)
{
	if (c->ufd.fd < 0)
		return;

	if (c->ufd.registered && conn_mask_get(c) == mask)
		return;

	uloop_fd_add(&c->ufd, mask);
}

static void conn_mask_add(struct sip_conn *c, unsigned int extra)
{
	conn_mask_set(c, conn_mask_get(c) | extra);
}

static size_t tx_pending(const struct sip_conn *c)
{
	return c->tx_len - c->tx_off;
}

/* ------------------------------------------------------ upcalls and close */

/*
 * Report a state transition and say whether the connection survived it. The
 * consumer may close the connection from inside the upcall, which is what the
 * account layer does when a reconnect forces a REGISTER refresh.
 */
static bool conn_event(struct sip_conn *c, enum sip_conn_state state)
{
	struct sip_stream *st = c->st;
	uint32_t h = sip_stream_conn_handle(c);

	if (!st->cb.conn)
		return true;

	st->cb.conn(st, h, state, st->priv);

	return sip_stream_conn_get(st, h) == c;
}

static void conn_backoff_clear(struct sip_conn *c)
{
	c->backoff_ms = 0;
	c->backoff_until = 0;
}

/*
 * The next interval of the per slot reconnect series: T1, doubling, capped at
 * 64*T1. A retransmission series must not become a connect series
 * (sip/stream.h "Reconnect backoff").
 */
static void conn_backoff_bump(struct sip_conn *c)
{
	uint32_t ms = c->backoff_ms ? c->backoff_ms * 2 : SIP_STREAM_BACKOFF_MS;

	if (ms > SIP_STREAM_BACKOFF_MAX_MS)
		ms = SIP_STREAM_BACKOFF_MAX_MS;

	c->backoff_ms = ms;
	c->backoff_until = mono_ms() + ms;
	tick_arm(c->st);
}

/*
 * Release the socket and the slot without an upcall. The framer is not reset
 * here: a slot that is not in use is never read, and conn_take() clears it when
 * the slot serves a connection again.
 */
static void conn_teardown(struct sip_conn *c)
{
	if (c->key.kind == SIP_STREAM_TLS)
		sip_tls_conn_close(&c->tls);

	/*
	 * Unconditionally, never behind ufd.registered. uloop_fetch_events()
	 * auto-deletes an fd that reported EPOLLERR or EPOLLHUP while its
	 * dispatch still sits queued in the same batch: registered is already
	 * false and the queued entry still points at this struct, so a teardown
	 * that skipped the delete would let that entry run conn_io() on a slot
	 * another connection has since taken. uloop_fd_delete() scrubs the
	 * queued entry before it looks at registered at all, and it is safe on
	 * an unregistered fd and on -1. The cleared callback is the second belt:
	 * libubox skips a dispatch whose cb is NULL.
	 */
	uloop_fd_delete(&c->ufd);

	if (c->ufd.fd >= 0)
		close(c->ufd.fd);

	memset(&c->local, 0, sizeof(c->local));
	c->ufd.cb = NULL;
	c->ufd.fd = -1;
	c->tx_len = 0;
	c->tx_off = 0;
	c->deadline_ms = 0;
	c->tos_cur = SIP_TOS_UNSET;
	c->state = SIP_CONN_FREE;
	c->inbound = false;
	c->in_use = false;
	c->gen++;
	if (!c->gen)
		c->gen = 1;

	c->st->active--;
}

static void conn_drop(struct sip_conn *c)
{
	struct sip_stream *st = c->st;
	uint32_t h = sip_stream_conn_handle(c);

	if (!c->in_use)
		return;

	SIP_WIRE_TRACE("stream: connection %u to %s closed", c->index,
		       sip_stream_kind_name((enum sip_stream_kind)c->key.kind));

	conn_teardown(c);

	/*
	 * The last step. The upcall may open a connection that takes this very
	 * slot, so nothing after it may touch c, and every caller returns from
	 * here.
	 */
	if (st->cb.conn)
		st->cb.conn(st, h, SIP_CONN_FREE, st->priv);
}

/* a connect or a handshake that did not come up: the slot starts a backoff */
static void conn_fail(struct sip_conn *c)
{
	/* an accepted connection has no series to back off: its key names the
	 * peer's ephemeral port, which this side never dials */
	if (!c->inbound)
		conn_backoff_bump(c);

	conn_drop(c);
}

/* ------------------------------------------------------------------ write */

/* cap is SIP_STREAM_MSG_MAX for a message and SIP_STREAM_TX_MAX for the pong
 * that the reserve above keeps room for */
static bool tx_stage(struct sip_conn *c, const void *buf, size_t len, size_t cap)
{
	size_t held = tx_pending(c);

	if (held > cap || len > cap - held)
		return false;

	if (c->tx_off) {
		memmove(c->tx, c->tx + c->tx_off, held);
		c->tx_len = held;
		c->tx_off = 0;
	}

	memcpy(c->tx + c->tx_len, buf, len);
	c->tx_len += len;

	return true;
}

/* 0 with *done set is progress or a full socket; -1 is terminal */
static int tcp_write(struct sip_conn *c, const char *buf, size_t len, size_t *done)
{
	ssize_t n;

	do {
		n = send(c->ufd.fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
	} while (n < 0 && errno == EINTR);

	if (n >= 0) {
		*done = (size_t)n;
		return 0;
	}

	return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

/*
 * sip/tls.h states that a write can want a read: the caller must wait for the
 * mask the record layer names and never for the one its own direction
 * suggests.
 *
 * Interleaving a read with a blocked write is safe here, and it is safe only
 * because of four properties a maintainer must not remove:
 *
 *   - SSL_OP_NO_RENEGOTIATION plus the TLS 1.2 floor means the read can never
 *     start a second handshake underneath the pending write;
 *   - SSL_MODE_ENABLE_PARTIAL_WRITE lets a write report the bytes it moved
 *     instead of owning the whole payload across the retry;
 *   - SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER lets the retry present the payload
 *     from another address;
 *   - the staging buffer is append only between retries (tx_stage() compacts
 *     only what was already written), so a retry always presents the identical
 *     prefix of the identical logical stream.
 *
 * test_sip_tls.c checks the three OpenSSL modes through a getter.
 * A read taken while a write is blocked on SIP_TLS_WANT_READ is directionally
 * safe: the only thing it can feed the record layer is the very input that
 * write is waiting for.
 */
static int tls_write(struct sip_conn *c, const char *buf, size_t len, size_t *done,
		     unsigned int *want)
{
	enum sip_tls_io io = sip_tls_conn_write(&c->tls, buf, len, done);

	if (io == SIP_TLS_ERR)
		return -1;

	if (io == SIP_TLS_WANT_READ)
		*want = ULOOP_READ;

	return 0;
}

static int conn_write(struct sip_conn *c, const char *buf, size_t len, size_t *done,
		      unsigned int *want)
{
	if (c->key.kind == SIP_STREAM_TLS)
		return tls_write(c, buf, len, done, want);

	return tcp_write(c, buf, len, done);
}

/*
 * Drain the staging buffer and report the uloop bit the remaining bytes need:
 * ULOOP_WRITE for a socket that is simply full, and 0 both when everything
 * left and when the record layer is waiting for a read, which the ULOOP_READ
 * of an up connection already covers. It may close the connection, so it is
 * the last thing a caller does with c before it resolves the handle again.
 */
static unsigned int tx_drain(struct sip_conn *c)
{
	struct sip_stream *st = c->st;
	unsigned int want;
	size_t done;

	while (c->tx_off < c->tx_len) {
		done = 0;
		want = 0;

		if (conn_write(c, c->tx + c->tx_off, c->tx_len - c->tx_off, &done,
			       &want) < 0) {
			st->stat.tx_err++;
			conn_drop(c);
			return 0;
		}

		if (!done)
			return want == ULOOP_READ ? 0 : ULOOP_WRITE;

		c->tx_off += done;
	}

	c->tx_off = 0;
	c->tx_len = 0;

	return 0;
}

/*
 * Two accounts that share a local port share a connection and may carry
 * different sip_tos values, so the byte belongs to the send, as it does in
 * transport.c. A refused byte is counted and the next send tries again; it is
 * never a reason to drop the message.
 *
 * What the byte cannot promise. The staging buffer of struct sip_conn carries
 * no per message boundary, so the socket holds one byte for whatever is in
 * flight. conn_send() flushes what is already staged before it moves the
 * byte, which covers the common case of two accounts taking turns on a
 * connection that keeps up. Two windows remain and are accepted: bytes that
 * backpressure left staged go out under the next sender's byte, and everything
 * staged while the connection was still connecting drains under the byte of
 * the last send once it comes up. Both mismark DSCP and neither loses a
 * message.
 */
static void tos_apply(struct sip_conn *c, uint8_t tos)
{
	if (c->tos_cur == (uint32_t)tos)
		return;

	if (sock_tos_set(c->ufd.fd, c->key.peer.sa.ss_family, tos) < 0) {
		c->tos_cur = SIP_TOS_UNSET;
		c->st->stat.tos_err++;
		return;
	}

	c->tos_cur = tos;
}

/* stage the pong answer without counting it as a message (sip/stream.h) */
static void conn_pong(struct sip_conn *c)
{
	struct sip_stream *st = c->st;
	uint32_t h = sip_stream_conn_handle(c);
	unsigned int want;

	if (!tx_stage(c, SIP_FRAME_PONG, SIP_FRAME_PONG_LEN, SIP_STREAM_TX_MAX)) {
		st->stat.tx_err++;
		return;
	}

	st->stat.ping++;
	want = tx_drain(c);

	c = sip_stream_conn_get(st, h);
	if (c && want)
		conn_mask_add(c, want);
}

static void conn_endpoints_get(const struct sip_conn *c,
			       struct sip_tap_endpoints *ep)
{
	ep->local = c->local;
	sip_tap_endpoint_set(&ep->peer,
			     (const struct sockaddr *)&c->key.peer.sa);

	/* The local address and port stay zero until the connect completes. */
	if (!ep->local.ipver)
		ep->local.ipver = ep->peer.ipver;

	ep->kind = c->key.kind == SIP_STREAM_TLS ?
		   TRACE_PKT_SIP_TLS : TRACE_PKT_SIP_TCP;
}

static int conn_send(struct sip_conn *c, uint8_t tos, const void *buf, size_t len)
{
	struct sip_stream *st = c->st;
	uint32_t h = sip_stream_conn_handle(c);
	unsigned int want;

	/* the bytes already staged were accepted under another byte, so they
	 * leave under it before this send moves the socket to its own */
	if (tx_pending(c) && c->state == SIP_CONN_UP && c->tos_cur != (uint32_t)tos) {
		tx_drain(c);

		/* a drain that killed the connection counted its own tx_err */
		c = sip_stream_conn_get(st, h);
		if (!c)
			return -1;
	}

	/* Staged before the byte moves: a refusal must leave the socket marked
	 * for whatever is still in flight on it. */
	if (!tx_stage(c, buf, len, SIP_STREAM_MSG_MAX)) {
		st->stat.tx_err++;
		return -1;
	}

	if (sip_tap_cb) {
		struct sip_tap_endpoints ep;

		conn_endpoints_get(c, &ep);
		sip_tap_cb(true, &ep, buf, len);
	}

	tos_apply(c, tos);

	c->used_ms = mono_ms();
	st->stat.tx++;
	st->stat.tx_bytes += len;

	/* sip/stream.h "Queued send semantics": staging on a connection that is
	 * still coming up is success, and the transaction's own life timer owns
	 * the wait */
	if (c->state != SIP_CONN_UP) {
		st->stat.queued++;
		return (int)len;
	}

	want = tx_drain(c);

	c = sip_stream_conn_get(st, h);
	if (c && want)
		conn_mask_add(c, want);

	return (int)len;
}

/* ------------------------------------------------------------------- read */

static enum rx_step tcp_read(struct sip_conn *c, char *buf, size_t len, size_t *got)
{
	ssize_t n;

	do {
		n = recv(c->ufd.fd, buf, len, MSG_DONTWAIT);
	} while (n < 0 && errno == EINTR);

	if (n > 0) {
		*got = (size_t)n;
		return RX_DATA;
	}

	if (!n)
		return RX_EOF;

	return (errno == EAGAIN || errno == EWOULDBLOCK) ? RX_IDLE : RX_FAIL;
}

/* sip/tls.h: SIP_TLS_OK with no bytes is the peer's close_notify */
static enum rx_step tls_read(struct sip_conn *c, char *buf, size_t len, size_t *got,
			     unsigned int *want)
{
	enum sip_tls_io io = sip_tls_conn_read(&c->tls, buf, len, got);

	if (io == SIP_TLS_OK)
		return *got ? RX_DATA : RX_EOF;

	if (io == SIP_TLS_ERR)
		return RX_FAIL;

	if (io == SIP_TLS_WANT_WRITE)
		*want = ULOOP_WRITE;

	return RX_IDLE;
}

static enum rx_step conn_read(struct sip_conn *c, char *buf, size_t len, size_t *got,
			      unsigned int *want)
{
	if (c->key.kind == SIP_STREAM_TLS)
		return tls_read(c, buf, len, got, want);

	return tcp_read(c, buf, len, got);
}

static void conn_eof(struct sip_conn *c)
{
	/* a trailing lone CRLF the framer held is a pong once the stream ends
	 * (sip/frame.h "The deferred CRLF rule") */
	if (frame_close(&c->fr) == FRAME_PONG)
		c->st->stat.pong++;

	conn_drop(c);
}

/*
 * One complete message. The slice points into the connection's own receive
 * slab, which msg_parse() unfolds in place, so the writable pointer is
 * recovered from the slab rather than cast away from the slice.
 */
static void rx_msg(struct sip_conn *c, struct sip_str slice)
{
	struct sip_stream *st = c->st;
	uint32_t h = sip_stream_conn_handle(c);
	struct sip_txn_dst src = c->key.peer;
	char *wire = c->rx + (size_t)(slice.p - c->rx);
	struct sip_msg msg;

	if (sip_tap_cb) {
		struct sip_tap_endpoints ep;

		conn_endpoints_get(c, &ep);
		sip_tap_cb(false, &ep, wire, slice.len);
	}

	st->stat.frames++;
	c->frames++;

	if (msg_parse(&msg, wire, slice.len) != SIP_PARSE_OK) {
		/* the framer honoured the declared length, so the boundary of
		 * the next message is still known (sip/frame.h) */
		st->stat.parse_err++;
		return;
	}

	st->stat.rx++;
	st->stat.rx_bytes += slice.len;

	/* sip/stream.h "Dst provenance": the peer port of an accepted
	 * connection is ephemeral and nothing listens on it, so it must never
	 * be connected to */
	if (c->inbound)
		src.flags |= SIP_DST_F_NO_CONNECT;

	c->used_ms = mono_ms();

	if (!st->cb.msg)
		return;

	/* the upcall may close the connection; rx_pump() is the one place that
	 * decides whether the loop goes on, so nothing is decided here */
	st->cb.msg(st, h, &msg, &src, st->priv);
}

/* false ends the receive loop: the framer has nothing more, or it lost the
 * message boundary and the connection went with it */
static bool rx_event(struct sip_conn *c, enum frame_ev ev, struct sip_str slice)
{
	struct sip_stream *st = c->st;

	switch (ev) {
	case FRAME_MSG:
		rx_msg(c, slice);
		return true;
	case FRAME_PING:
		/* answered inside the connection, so a TLS connection answers
		 * inside the record layer by construction (sip/stream.h "Ping
		 * is answered inside the connection") */
		conn_pong(c);
		return true;
	case FRAME_PONG:
		st->stat.pong++;
		return true;
	case FRAME_MORE:
		return false;
	case FRAME_ERR_OVERSIZE:
	case FRAME_ERR_NO_CLEN:
	case FRAME_ERR_BAD_CLEN:
		break;
	}

	SIP_WIRE_TRACE("stream: connection %u framing error: %s", c->index,
		       frame_ev_name(ev));
	st->stat.frame_errs++;
	conn_drop(c);

	return false;
}

/*
 * Rx reentrancy. One read can carry several messages and dispatching one of
 * them can close the connection: the upcall can answer, the answer can fail to
 * send, an oversize frame can close, a TLS error can close. The loop therefore
 * holds the handle and not a pointer, resolves it again before every
 * frame_next() call, and stops the moment it no longer names a connection. This
 * single resolve is the whole rule; no step below it decides the loop's fate
 * (sip/stream.h "Rx reentrancy").
 */
static void rx_pump(struct sip_stream *st, uint32_t handle)
{
	for (;;) {
		struct sip_conn *c = sip_stream_conn_get(st, handle);
		struct sip_str slice;
		enum frame_ev ev;

		if (!c)
			return;

		ev = frame_next(&c->fr, &slice);
		if (ev == FRAME_MORE)
			return;

		if (!rx_event(c, ev, slice))
			return;
	}
}

/*
 * Read until the source is dry, not until the socket is. One TLS record can
 * carry more plaintext than one read consumes, and what stays inside OpenSSL
 * is invisible to epoll: a single read per event leaves it there, and a peer
 * that then goes quiet never produces the readable event that would collect
 * it. Only SIP_TLS_WANT_READ, or its plain EAGAIN twin, says there is nothing
 * more, so that is the loop's exit. The handle is resolved again between
 * passes, because dispatching a message can close the connection.
 *
 * SIP_STREAM_RX_BURST bounds the passes for the reason the accept burst is
 * bounded: what the bound leaves is collected on the next turn of a level
 * triggered loop, and holding the loop is what nothing else can recover from.
 *
 * The answer is the mask the last pass asked for, and 0 for none.
 */
static unsigned int conn_rx(struct sip_conn *c)
{
	struct sip_stream *st = c->st;
	uint32_t h = sip_stream_conn_handle(c);
	unsigned int want = 0;
	unsigned int pass;
	size_t space;
	size_t got;
	char *tail;

	for (pass = 0; pass < SIP_STREAM_RX_BURST; pass++) {
		space = 0;
		got = 0;
		want = 0;

		tail = frame_tail(&c->fr, &space);
		if (!tail) {
			st->stat.frame_errs++;
			conn_drop(c);
			return 0;
		}

		switch (conn_read(c, tail, space, &got, &want)) {
		case RX_DATA:
			break;
		case RX_IDLE:
			return want;
		case RX_EOF:
			conn_eof(c);
			return 0;
		case RX_FAIL:
			st->stat.rx_err++;
			conn_drop(c);
			return 0;
		}

		frame_fed(&c->fr, got);
		c->used_ms = mono_ms();
		rx_pump(st, h);

		c = sip_stream_conn_get(st, h);
		if (!c)
			return 0;
	}

	return want;
}

/* --------------------------------------------------------- state machine */

static void conn_up(struct sip_conn *c)
{
	struct sip_stream *st = c->st;
	uint32_t h = sip_stream_conn_handle(c);
	unsigned int want;

	c->state = SIP_CONN_UP;
	c->deadline_ms = 0;
	c->used_ms = mono_ms();
	conn_backoff_clear(c);
	conn_mask_set(c, ULOOP_READ);

	SIP_WIRE_TRACE("stream: connection %u up (%s)", c->index,
		       sip_stream_kind_name((enum sip_stream_kind)c->key.kind));

	if (!conn_event(c, SIP_CONN_UP))
		return;

	want = tx_drain(c);

	c = sip_stream_conn_get(st, h);
	if (c)
		conn_mask_set(c, ULOOP_READ | want);
}

static void conn_handshake(struct sip_conn *c)
{
	enum sip_tls_io io = sip_tls_conn_io(&c->tls);

	if (io == SIP_TLS_ERR) {
		SIP_WIRE_TRACE("stream: handshake on %u failed: %s", c->index,
			       sip_tls_conn_error(&c->tls));
		c->st->stat.handshakes_failed++;
		conn_fail(c);
		return;
	}

	if (io == SIP_TLS_OK) {
		conn_up(c);
		return;
	}

	conn_mask_set(c, io == SIP_TLS_WANT_WRITE ? ULOOP_WRITE : ULOOP_READ);
}

static void conn_handshake_start(struct sip_conn *c, bool server)
{
	struct sip_stream *st = c->st;

	if (!sip_tls_conn_start(st->tls, &c->tls, c->ufd.fd, server,
				c->sni[0] ? c->sni : NULL)) {
		SIP_WIRE_TRACE("stream: connection %u has no TLS: %s", c->index,
			       sip_tls_conn_error(&c->tls));
		st->stat.handshakes_failed++;

		/* an ACCEPTED slot has reported no state yet, so its release is
		 * not a transition the consumer could match, exactly as a
		 * connect that never left the syscall (conn_open) */
		if (server)
			conn_teardown(c);
		else
			conn_fail(c);

		return;
	}

	c->state = SIP_CONN_HANDSHAKE;

	/*
	 * An outbound connection keeps the deadline conn_connect() set:
	 * SIP_STREAM_CONNECT_MS bounds a connect plus a handshake, so a second
	 * budget here would give the pair almost 64*T1 and let a queued request
	 * leave after the transaction timer that owns it already reported
	 * (sip/stream.h SIP_STREAM_CONNECT_MS). An accepted connection had no
	 * connect phase, so its bound starts here.
	 */
	if (server)
		c->deadline_ms = mono_ms() + SIP_STREAM_CONNECT_MS;

	conn_mask_set(c, ULOOP_READ);

	if (!conn_event(c, SIP_CONN_HANDSHAKE))
		return;

	conn_handshake(c);
}

static void conn_local_set(struct sip_conn *c)
{
	struct sockaddr_storage sa;
	socklen_t len = sizeof(sa);

	if (getsockname(c->ufd.fd, (struct sockaddr *)&sa, &len) == 0)
		sip_tap_endpoint_set(&c->local, (const struct sockaddr *)&sa);
}

static void conn_connected(struct sip_conn *c)
{
	socklen_t len = sizeof(int);
	int err = 0;

	if (getsockopt(c->ufd.fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) {
		SIP_WIRE_TRACE("stream: connect on %u refused: %s", c->index,
			       strerror(err ? err : errno));
		conn_fail(c);
		return;
	}

	conn_local_set(c);

	if (c->key.kind == SIP_STREAM_TLS) {
		conn_handshake_start(c, false);
		return;
	}

	conn_up(c);
}

/*
 * One step of an up connection: read what arrived, then move what is staged.
 * ULOOP_READ is always armed, and ULOOP_WRITE joins it only for what the two
 * fresh results just asked for, which is why this is the one place that
 * recomputes the whole mask.
 */
static void conn_up_io(struct sip_conn *c)
{
	struct sip_stream *st = c->st;
	uint32_t h = sip_stream_conn_handle(c);
	unsigned int mask = ULOOP_READ;

	mask |= conn_rx(c);

	c = sip_stream_conn_get(st, h);
	if (!c)
		return;

	mask |= tx_drain(c);

	c = sip_stream_conn_get(st, h);
	if (!c)
		return;

	conn_mask_set(c, mask);
}

static void conn_io(struct uloop_fd *ufd, unsigned int events)
{
	struct sip_conn *c = container_of(ufd, struct sip_conn, ufd);

	switch ((enum sip_conn_state)c->state) {
	case SIP_CONN_CONNECTING:
		conn_connected(c);
		return;
	case SIP_CONN_HANDSHAKE:
		conn_handshake(c);
		return;
	case SIP_CONN_UP:
		conn_up_io(c);
		return;
	case SIP_CONN_FREE:
	case SIP_CONN_CLOSING:
		break;
	}
}

/* ------------------------------------------------------------------ slots */

/*
 * The destination a slot remembers, which for a TLS key is the key and the
 * stored name: two names at one address are two destinations, so name B must
 * not inherit name A's reconnect backoff and fail its first send for a peer it
 * never reached (sip/stream.h "SNI splits a TLS connection"). A TCP key has no
 * server_name and ignores the name, exactly as conn_sni_eq() does everywhere.
 */
static bool conn_slot_eq(const struct sip_conn *c, const struct sip_stream_key *key,
			 const char *sni)
{
	return sip_stream_key_eq(&c->key, key) && conn_sni_eq(c, key->kind, sni);
}

/* a slot that is free but still remembers this destination, and its backoff */
static struct sip_conn *conn_slot_find(struct sip_stream *st, const struct sip_stream_key *key,
				       const char *sni)
{
	unsigned int i;

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		struct sip_conn *c = &st->conn[i];

		if (c->in_use || !c->key.peer.sa_len)
			continue;

		if (conn_slot_eq(c, key, sni))
			return c;
	}

	return NULL;
}

static struct sip_conn *conn_slot_free(struct sip_stream *st, uint64_t now)
{
	struct sip_conn *held = NULL;
	unsigned int i;

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		struct sip_conn *c = &st->conn[i];

		if (c->in_use)
			continue;

		/* a slot whose backoff still runs keeps it for its own key as
		 * long as any other slot can serve instead */
		if (!c->backoff_until || now >= c->backoff_until)
			return c;

		if (!held)
			held = c;
	}

	return held;
}

static struct sip_conn *conn_slot_lru(struct sip_stream *st)
{
	struct sip_conn *lru = NULL;
	unsigned int i;

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		struct sip_conn *c = &st->conn[i];

		/* eviction never takes a connection with queued transmit bytes:
		 * those bytes belong to a live transaction (sip/stream.h) */
		if (!c->in_use || tx_pending(c))
			continue;

		if (!lru || c->used_ms < lru->used_ms)
			lru = c;
	}

	return lru;
}

static struct sip_conn *conn_slot_take(struct sip_stream *st, const struct sip_stream_key *key,
				       const char *sni, uint64_t now)
{
	struct sip_conn *c = conn_slot_find(st, key, sni);

	if (c)
		return c;

	c = conn_slot_free(st, now);
	if (c)
		return c;

	c = conn_slot_lru(st);
	if (!c)
		return NULL;

	st->stat.evictions++;
	conn_drop(c);

	/* the SIP_CONN_FREE upcall may have taken the slot that just opened */
	return conn_slot_free(st, now);
}

/* claim a slot for one connection. No upcall: the caller reports the state. */
static void conn_take(struct sip_conn *c, const struct sip_stream_key *key, const char *sni,
		      uint64_t now)
{
	struct sip_stream *st = c->st;

	/* a slot that serves another destination starts with no backoff of its
	 * own, and one that serves the same one keeps the series it earned */
	if (!conn_slot_eq(c, key, sni))
		conn_backoff_clear(c);

	c->key = *key;
	c->key.peer.flags = 0;

	/* a TCP connection carries no server_name, so the stored name is the
	 * TLS reuse discriminator and nothing else */
	if (key->kind == SIP_STREAM_TLS)
		str_copy(c->sni, sizeof(c->sni), sni);
	else
		c->sni[0] = '\0';

	frame_reset(&c->fr);
	memset(&c->tls, 0, sizeof(c->tls));
	c->tx_len = 0;
	c->tx_off = 0;
	c->frames = 0;
	c->tos_cur = SIP_TOS_UNSET;
	c->used_ms = now;
	c->deadline_ms = 0;
	c->state = SIP_CONN_FREE;
	c->inbound = false;
	c->in_use = true;
	st->active++;
	tick_arm(st);
}

/* ---------------------------------------------------------- connect (out) */

/*
 * Ephemeral source ports. The local port is not bound first: binding it would
 * put every connection to one peer in the same four tuple, and a closed one
 * holds that tuple in TIME_WAIT for two minutes (sip/stream.h).
 */
static int conn_connect(struct sip_conn *c)
{
	const struct sip_txn_dst *dst = &c->key.peer;
	int fd;

	fd = socket(dst->sa.ss_family,
		    SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	if (!sock_iface_bind(fd, c->key.iface))
		goto fail;

	sock_keepalive(fd);

	if (connect(fd, (const struct sockaddr *)&dst->sa, dst->sa_len) < 0 &&
	    errno != EINPROGRESS && errno != EINTR)
		goto fail;

	c->ufd.fd = fd;
	c->ufd.cb = conn_io;

	if (uloop_fd_add(&c->ufd, ULOOP_WRITE) < 0) {
		c->ufd.fd = -1;
		goto fail;
	}

	c->state = SIP_CONN_CONNECTING;
	c->deadline_ms = mono_ms() + SIP_STREAM_CONNECT_MS;

	return 0;

fail:
	{
		int err = errno;

		close(fd);
		errno = err;
	}

	return -1;
}

static bool backoff_running(struct sip_stream *st, const struct sip_stream_key *key,
			    const char *sni, uint64_t now)
{
	const struct sip_conn *c = conn_slot_find(st, key, sni);

	return c && c->backoff_until && now < c->backoff_until;
}

static struct sip_conn *conn_open(struct sip_stream *st, const struct sip_stream_key *key,
				  const char *sni)
{
	uint64_t now = mono_ms();
	struct sip_conn *c;
	bool again;

	/* sip/stream.h "Dst provenance": a destination that came from an
	 * accepted connection is never connected to; its port reaches no SIP
	 * element */
	if (key->peer.flags & SIP_DST_F_NO_CONNECT)
		return NULL;

	if (key->kind == SIP_STREAM_TLS && !sip_tls_client_ready(st->tls))
		return NULL;

	if (backoff_running(st, key, sni, now))
		return NULL;

	c = conn_slot_take(st, key, sni, now);
	if (!c) {
		st->stat.denied++;
		return NULL;
	}

	again = c->key.peer.sa_len && conn_slot_eq(c, key, sni);
	conn_take(c, key, sni, now);

	if (conn_connect(c) < 0) {
		SIP_WIRE_TRACE("stream: connect from slot %u failed: %s", c->index,
			       strerror(errno));

		/* no state was reported for this slot yet, so its release is
		 * not a transition the consumer could match to anything */
		conn_backoff_bump(c);
		conn_teardown(c);
		return NULL;
	}

	st->stat.connects++;
	if (again)
		st->stat.reconnects++;

	if (!conn_event(c, SIP_CONN_CONNECTING))
		return NULL;

	return c;
}

/* ------------------------------------------------------------------- send */

static int send_refuse(struct sip_stream *st)
{
	if (st)
		st->stat.tx_err++;

	return -1;
}

/*
 * The SNI takes part in the outbound reuse match only. A destination carrying
 * SIP_DST_F_NO_CONNECT names an accepted connection, which always holds an
 * empty sni, so asking for it under the account's own name would never find it
 * and the response of sip/stream.h "Responses travel back on the arriving
 * connection" would be lost.
 */
static struct sip_conn *conn_resolve(struct sip_stream *st, const struct sip_stream_key *key,
				     const char *sni)
{
	if (key->peer.flags & SIP_DST_F_NO_CONNECT)
		return sip_stream_conn_find(st, key, NULL);

	return sip_stream_conn_find(st, key, sni);
}

int sip_stream_send(struct sip_stream *st, const struct sip_stream_key *key, uint8_t tos,
		    const void *buf, size_t len)
{
	return sip_stream_send_sni(st, key, tos, buf, len, NULL);
}

int sip_stream_send_sni(struct sip_stream *st, const struct sip_stream_key *key, uint8_t tos,
			const void *buf, size_t len, const char *sni)
{
	struct sip_conn *c;

	if (!st || !key || !buf || !len)
		return send_refuse(st);

	if (key->kind >= SIP_STREAM_KIND_MAX || !key_peer_ok(key))
		return send_refuse(st);

	/* the staging buffer is the queue, and nothing here ever allocates. The
	 * reserve above is not a caller's to use, so the bound is MSG_MAX. */
	if (len > SIP_STREAM_MSG_MAX)
		return send_refuse(st);

	c = conn_resolve(st, key, sni);
	if (c)
		return conn_send(c, tos, buf, len);

	c = conn_open(st, key, sni);
	if (!c)
		return send_refuse(st);

	return conn_send(c, tos, buf, len);
}

/* ------------------------------------------------------------------ close */

/*
 * No drain before the close. A caller closes because the connection must be
 * gone now: an address change invalidated its source address, a certificate
 * bundle reload invalidated its session secrets, or an eviction needs the slot,
 * and an eviction never takes queued bytes in the first place. Draining would
 * need a second close path on a timer, and every message that still matters is
 * owned by a transaction that retransmits or times out above this layer.
 */
void sip_stream_conn_close(struct sip_stream *st, uint32_t handle)
{
	struct sip_conn *c = sip_stream_conn_get(st, handle);

	if (c)
		conn_drop(c);
}

unsigned int sip_stream_close_iface(struct sip_stream *st, const char *iface)
{
	const char *name = iface ? iface : "";
	unsigned int closed = 0;
	unsigned int i;

	if (!st)
		return 0;

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		struct sip_conn *c = &st->conn[i];

		if (!c->in_use || strncmp(c->key.iface, name, SIP_SOCK_IFACE_MAX))
			continue;

		conn_drop(c);
		closed++;
	}

	return closed;
}

unsigned int sip_stream_close_all(struct sip_stream *st)
{
	unsigned int closed = 0;
	unsigned int i;

	if (!st)
		return 0;

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		if (!st->conn[i].in_use)
			continue;

		conn_drop(&st->conn[i]);
		closed++;
	}

	return closed;
}

/* ---------------------------------------------------------------- accepts */

static void conn_accept(struct sip_listener *l, int fd, const struct sockaddr *sa,
			socklen_t slen)
{
	struct sip_stream *st = l->st;
	struct sip_stream_key key;
	uint64_t now = mono_ms();
	struct sip_conn *c;

	memset(&key, 0, sizeof(key));
	memcpy(&key.peer.sa, sa, slen);
	key.peer.sa_len = slen;
	key.local_port = l->port;
	key.kind = l->kind;
	str_copy(key.iface, sizeof(key.iface), l->iface);

	/* an accepted connection carries no server_name, so it looks for a slot
	 * that remembers this peer under no name either */
	c = conn_slot_take(st, &key, NULL, now);
	if (!c) {
		st->stat.denied++;
		close(fd);
		return;
	}

	conn_take(c, &key, NULL, now);
	c->inbound = true;
	c->ufd.fd = fd;
	c->ufd.cb = conn_io;
	conn_local_set(c);
	sock_keepalive(fd);
	st->stat.accepts++;

	SIP_WIRE_TRACE("stream: connection %u accepted on port %u", c->index, l->port);

	if (key.kind == SIP_STREAM_TLS) {
		conn_handshake_start(c, true);
		return;
	}

	conn_up(c);
}

/* false ends the accept burst */
static bool listen_accept(struct sip_listener *l)
{
	struct sockaddr_storage sa;
	socklen_t slen = sizeof(sa);
	int fd;

	fd = accept4(l->ufd.fd, (struct sockaddr *)&sa, &slen, SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (fd < 0) {
		/* a peer that went between the SYN and the accept says nothing
		 * about the ones queued behind it */
		return errno == EINTR || errno == ECONNABORTED;
	}

	if (!family_ok(sa.ss_family) || slen > sizeof(sa)) {
		close(fd);
		return true;
	}

	conn_accept(l, fd, (const struct sockaddr *)&sa, slen);

	return true;
}

static void listen_io(struct uloop_fd *ufd, unsigned int events)
{
	struct sip_listener *l = container_of(ufd, struct sip_listener, ufd);
	struct sip_stream *st = l->st;
	uint32_t h = listener_handle(l);
	unsigned int i;

	/* every accept reports a state, and a consumer may drop the listener's
	 * last reference from inside that upcall: the burst holds the handle */
	for (i = 0; i < SIP_STREAM_ACCEPT_BURST; i++) {
		l = sip_stream_listener_get(st, h);
		if (!l)
			return;

		if (!listen_accept(l))
			return;
	}
}

/* -------------------------------------------------------------- listeners */

static int listen_fd_open(struct sip_listener *l, int backlog)
{
	struct sockaddr_storage sa;
	socklen_t slen;
	int on = 1;
	int fd;

	slen = addr_pton(l->family, l->addr, &sa);
	if (!slen) {
		errno = EINVAL;
		return -1;
	}

	sa_port_set(&sa, l->port);

	fd = socket(l->family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	/*
	 * SO_REUSEADDR is set and SO_REUSEPORT is not (sip/stream.h). The
	 * accepted connections of a listener that closed sit in TIME_WAIT on
	 * its port, and the bind after a reload has to take the port back;
	 * SO_REUSEPORT would instead let a second uvoiced take a share of the
	 * inbound connections rather than fail to start.
	 */
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
		goto fail;

	/*
	 * Before the bind, for the reason sock_iface_bind() records: the option
	 * does not move a socket that already carries an address. An AF_INET6
	 * listener that took an IPv4 mapped peer would hand this layer a key of
	 * the wrong family, so a refused option fails the open, as transport.c
	 * does.
	 */
	if (l->family == AF_INET6 &&
	    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) < 0)
		goto fail;

	if (!sock_iface_bind(fd, l->iface))
		goto fail;

	if (bind(fd, (const struct sockaddr *)&sa, slen) < 0)
		goto fail;

	if (listen(fd, backlog) < 0)
		goto fail;

	l->ufd.fd = fd;
	l->ufd.cb = listen_io;

	if (uloop_fd_add(&l->ufd, ULOOP_READ) < 0) {
		l->ufd.fd = -1;
		goto fail;
	}

	l->up = true;

	return 0;

fail:
	{
		int err = errno;

		close(fd);
		errno = err;
	}

	return -1;
}

static struct sip_listener *listen_find(struct sip_stream *st, const char *addr,
					const char *iface, const struct sip_stream_listen_cfg *cfg)
{
	unsigned int i;

	for (i = 0; i < SIP_STREAM_LISTEN_MAX; i++) {
		struct sip_listener *l = &st->listen[i];

		if (!l->in_use || l->port != cfg->port || l->kind != cfg->kind)
			continue;

		if (l->family != family_of(cfg))
			continue;

		if (strncmp(l->addr, addr, SIP_SOCK_HOST_MAX))
			continue;

		if (!strncmp(l->iface, iface, SIP_SOCK_IFACE_MAX))
			return l;
	}

	return NULL;
}

static struct sip_listener *listen_slot_free(struct sip_stream *st)
{
	unsigned int i;

	for (i = 0; i < SIP_STREAM_LISTEN_MAX; i++) {
		if (!st->listen[i].in_use)
			return &st->listen[i];
	}

	return NULL;
}

static bool listen_cfg_ok(struct sip_stream *st, const struct sip_stream_listen_cfg *cfg,
			  const char *addr, const char *iface)
{
	/* port 0 is refused: a listener needs a known port, and an ephemeral
	 * one would not be the port the Via and the Contact name */
	if (!cfg->port || cfg->kind >= SIP_STREAM_KIND_MAX) {
		errno = EINVAL;
		return false;
	}

	if (strlen(addr) >= SIP_SOCK_HOST_MAX || strlen(iface) >= SIP_SOCK_IFACE_MAX) {
		errno = EINVAL;
		return false;
	}

	/* sip/tls.h "Client only is a normal state": a TLS listener stays down
	 * until the certificate bundle lands */
	if (cfg->kind == SIP_STREAM_TLS && !sip_tls_server_ready(st->tls)) {
		errno = ENOTSUP;
		return false;
	}

	return true;
}

uint32_t sip_stream_listen(struct sip_stream *st, const struct sip_stream_listen_cfg *cfg)
{
	struct sip_listener *l;
	const char *addr;
	const char *iface;

	if (!st || !cfg) {
		errno = EINVAL;
		return SIP_HANDLE_NONE;
	}

	addr = cfg->addr ? cfg->addr : "";
	iface = cfg->iface ? cfg->iface : "";

	if (!listen_cfg_ok(st, cfg, addr, iface))
		return SIP_HANDLE_NONE;

	l = listen_find(st, addr, iface, cfg);
	if (l) {
		l->refs++;
		return listener_handle(l);
	}

	l = listen_slot_free(st);
	if (!l) {
		errno = ENOSPC;
		return SIP_HANDLE_NONE;
	}

	str_copy(l->addr, sizeof(l->addr), addr);
	str_copy(l->iface, sizeof(l->iface), iface);
	l->family = family_of(cfg);
	l->port = cfg->port;
	l->kind = (uint8_t)cfg->kind;
	l->ufd.fd = -1;
	l->up = false;

	if (listen_fd_open(l, cfg->backlog > 0 ? cfg->backlog : SIP_STREAM_BACKLOG) < 0)
		return SIP_HANDLE_NONE;

	l->in_use = true;
	l->refs = 1;

	SIP_WIRE_TRACE("stream: %s listener %u bound to %s %s:%u iface '%s'",
		       sip_stream_kind_name(cfg->kind), l->index,
		       l->family == AF_INET6 ? "ipv6" : "ipv4", l->addr, l->port,
		       l->iface);

	return listener_handle(l);
}

static void listen_teardown(struct sip_listener *l)
{
	/* unconditionally, and for the reason conn_teardown() records */
	uloop_fd_delete(&l->ufd);

	if (l->ufd.fd >= 0)
		close(l->ufd.fd);

	l->ufd.cb = NULL;
	l->ufd.fd = -1;
	l->up = false;
	l->refs = 0;
	l->in_use = false;
	l->gen++;
	if (!l->gen)
		l->gen = 1;
}

void sip_stream_listen_close(struct sip_stream *st, uint32_t handle)
{
	struct sip_listener *l = sip_stream_listener_get(st, handle);

	if (!l || !l->refs)
		return;

	if (--l->refs)
		return;

	SIP_WIRE_TRACE("stream: listener %u released", l->index);
	listen_teardown(l);
}

/* ------------------------------------------------------------------- tick */

static bool tick_needed(const struct sip_stream *st)
{
	unsigned int i;

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		if (st->conn[i].in_use || st->conn[i].backoff_until)
			return true;
	}

	return false;
}

static void tick_arm(struct sip_stream *st)
{
	if (!st->tick.pending)
		uloop_timeout_set(&st->tick, SIP_STREAM_TICK_MS);
}

/*
 * The difference is taken in unsigned arithmetic on purpose: it stays correct
 * across the whole range of the monotonic clock and needs no reference point.
 */
static void conn_sweep(struct sip_conn *c, uint64_t now)
{
	if (!c->in_use) {
		if (c->backoff_until && now >= c->backoff_until)
			c->backoff_until = 0;

		return;
	}

	if (c->deadline_ms && now >= c->deadline_ms) {
		if (c->state == SIP_CONN_HANDSHAKE)
			c->st->stat.handshakes_failed++;

		SIP_WIRE_TRACE("stream: connection %u overdue in %s", c->index,
			       sip_conn_state_name((enum sip_conn_state)c->state));
		conn_fail(c);
		return;
	}

	/* an outbound connection is never idle closed: behind NAT it is the
	 * inbound path (sip/stream.h "Connection lifetime") */
	if (!c->inbound || c->state != SIP_CONN_UP)
		return;

	if (now - c->used_ms >= SIP_STREAM_IDLE_MS) {
		SIP_WIRE_TRACE("stream: connection %u idle closed", c->index);
		conn_drop(c);
	}
}

static void tick_run(struct uloop_timeout *t)
{
	struct sip_stream *st = container_of(t, struct sip_stream, tick);
	uint64_t now = mono_ms();
	unsigned int i;

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++)
		conn_sweep(&st->conn[i], now);

	/* re-armed from what the sweep and its upcalls left behind, so a pool
	 * that emptied stops waking the daemon */
	if (tick_needed(st))
		uloop_timeout_set(t, SIP_STREAM_TICK_MS);
}

/* -------------------------------------------------------------- lifecycle */

/*
 * The pool is zeroed around its slabs and never through them. The eight
 * connections carry 640 KB of receive and transmit staging, which a write
 * makes resident and locked for the life of the daemon, and a slab is written
 * before it is read on every path that takes a slot: the framer starts at
 * length zero (frame_init, frame_reset) and tx_len and tx_off gate the
 * staging buffer. So the header memsets stop at rx, and the slabs of a fresh
 * process stay on the untouched zero page until a connection uses them.
 */
_Static_assert(offsetof(struct sip_conn, tx) ==
	       offsetof(struct sip_conn, rx) + SIP_STREAM_RX_MAX,
	       "tx must follow rx: the connection header memset stops at rx");
_Static_assert(sizeof(struct sip_conn) - offsetof(struct sip_conn, tx) -
	       SIP_STREAM_TX_MAX < _Alignof(struct sip_conn),
	       "rx and tx must be the last members: nothing after tx is zeroed");

void sip_stream_init(struct sip_stream *st, const struct sip_stream_cb *cb, void *priv,
		     struct sip_tls *tls)
{
	size_t tail = offsetof(struct sip_stream, conn) + sizeof(st->conn);
	unsigned int i;

	if (!st)
		return;

	memset(st, 0, offsetof(struct sip_stream, conn));
	memset((char *)st + tail, 0, sizeof(*st) - tail);

	if (cb)
		st->cb = *cb;

	st->priv = priv;
	st->tls = tls;
	st->tick.cb = tick_run;

	for (i = 0; i < SIP_STREAM_LISTEN_MAX; i++) {
		st->listen[i].st = st;
		st->listen[i].index = i;
		st->listen[i].gen = 1;
		st->listen[i].ufd.fd = -1;
	}

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		struct sip_conn *c = &st->conn[i];

		memset(c, 0, offsetof(struct sip_conn, rx));
		c->st = st;
		c->index = i;
		c->gen = 1;
		c->ufd.fd = -1;
		c->tos_cur = SIP_TOS_UNSET;
		frame_init(&c->fr, c->rx, sizeof(c->rx));
	}
}

void sip_stream_done(struct sip_stream *st)
{
	unsigned int i;

	if (!st)
		return;

	uloop_timeout_cancel(&st->tick);

	/*
	 * No SIP_CONN_FREE upcall from here. The owner of the pool is going
	 * away, so an upcall would reach a consumer that is already tearing
	 * itself down, as sip_transport_done does.
	 */
	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		if (st->conn[i].in_use)
			conn_teardown(&st->conn[i]);
	}

	for (i = 0; i < SIP_STREAM_LISTEN_MAX; i++) {
		if (st->listen[i].in_use)
			listen_teardown(&st->listen[i]);
	}

	st->active = 0;
}

/* ------------------------------------------------- transaction environment */

static int stream_env_send(struct sip_txn_env *env, const void *buf, size_t len,
			   const struct sip_txn_dst *dst)
{
	struct sip_stream_env *se = env->base.priv;
	struct sip_stream_key key;

	if (!se || !sip_stream_env_key(se, dst, &key))
		return send_refuse(se ? se->st : NULL);

	return sip_stream_send_sni(se->st, &key, se->tos, buf, len, se->sni);
}

static void stream_env_trace(struct proto_timer_env *env, const char *msg)
{
	SIP_WIRE_TRACE("%s", msg);
}

void sip_stream_env_init(struct sip_stream_env *se, struct sip_stream *st,
			 enum sip_stream_kind kind, uint16_t local_port, const char *iface,
			 uint8_t tos)
{
	if (!se)
		return;

	memset(se, 0, sizeof(*se));
	se->st = st;
	se->kind = (uint8_t)kind;
	se->local_port = local_port;
	se->tos = tos;
	str_copy(se->iface, sizeof(se->iface), iface);

	se->env.base.priv = se;
	/* the reliable timer set of sip/txn.h comes with the environment, so an
	 * account that builds this one needs no second call */
	se->env.flags = SIP_TXN_ENV_RELIABLE;
	se->env.send = stream_env_send;
	se->env.base.trace = stream_env_trace;
	sip_txn_env_uloop_init(&se->env);
}

void sip_stream_env_tos_set(struct sip_stream_env *se, uint8_t tos)
{
	if (se)
		se->tos = tos;
}

void sip_stream_env_local_set(struct sip_stream_env *se, uint16_t local_port, const char *iface)
{
	if (!se)
		return;

	se->local_port = local_port;
	str_copy(se->iface, sizeof(se->iface), iface);
}

void sip_stream_env_sni_set(struct sip_stream_env *se, const char *name)
{
	if (!se)
		return;

	str_copy(se->sni, sizeof(se->sni), name);
}

bool sip_stream_env_key(const struct sip_stream_env *se, const struct sip_txn_dst *dst,
			struct sip_stream_key *out)
{
	if (!out)
		return false;

	memset(out, 0, sizeof(*out));

	if (!se || !dst || !dst->sa_len || !family_ok(dst->sa.ss_family))
		return false;

	/* the flag travels with every copy of the destination (sip/txn.h) */
	out->peer = *dst;
	out->local_port = se->local_port;
	out->kind = se->kind;
	str_copy(out->iface, sizeof(out->iface), se->iface);

	return true;
}
