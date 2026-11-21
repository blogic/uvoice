// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The client to server stream. See xmpp/stream.h for the contracts.
 *
 * The parser runs inside the read loop. A callback of the parser may end
 * the connection, so every callback checks that the socket is still there
 * before it acts. The read loop checks the connection generation after
 * every feed. A restart of the XML document, which RFC 6120 asks for after
 * SASL, never happens inside a callback: the callback marks it, then the read
 * loop performs it once the parser returned. The TLS handshake after
 * <proceed/> takes the same path, because its first step may already finish
 * a document.
 */
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

#include <libubox/utils.h>

#include "posix.h"
#include "str.h"
#include "proto/proto_trace.h"
#include "proto/xmpp/disco.h"
#include "proto/xmpp/stream.h"

#define XMPP_TRACE_TEXT_MAX	400
#define XMPP_HEADER_MAX		(XMPP_STREAM_ID_MAX + 4 * JID_MAX)
#define XMPP_REPLY_MAX		2048U
#define XMPP_SASL_B64_MAX	B64_ENCODE_LEN(SASL_MSG_MAX)
#define XMPP_MECHS_MAX		16U
#define XMPP_KA_IDLE_S		120
#define XMPP_KA_INTVL_S		30
#define XMPP_KA_CNT		3

#define XMPP_TRACE(xs, fmt, ...)					\
	do {								\
		if ((xs)->env.host)					\
			PROTO_TRACE((xs)->env.host, TRACE_XMPP,		\
				    "xmpp %s: " fmt, (xs)->jid_text,	\
				    ##__VA_ARGS__);			\
	} while (0)

enum rx_step {
	RX_DATA,
	RX_IDLE,
	RX_EOF,
	RX_FAIL,
};

static const char *const state_names[XMPP_ST_MAX] = {
	[XMPP_ST_IDLE] = "idle",
	[XMPP_ST_RESOLVE] = "resolve",
	[XMPP_ST_CONNECT] = "connect",
	[XMPP_ST_TLS] = "tls",
	[XMPP_ST_FEATURES] = "features",
	[XMPP_ST_STARTTLS] = "starttls",
	[XMPP_ST_SASL] = "sasl",
	[XMPP_ST_BIND] = "bind",
	[XMPP_ST_SESSION] = "session",
	[XMPP_ST_SM] = "sm",
	[XMPP_ST_CARBONS] = "carbons",
	[XMPP_ST_READY] = "ready",
	[XMPP_ST_BACKOFF] = "backoff",
};

static void stream_fail(struct xmpp_stream *xs, const char *reason);
static void header_send(struct xmpp_stream *xs);
static void handshake(struct xmpp_stream *xs);
static void connect_next(struct xmpp_stream *xs);
static void negotiate_next(struct xmpp_stream *xs);
static void fd_io(struct uloop_fd *ufd, unsigned int events);

const char *xmpp_stream_state_name(enum xmpp_stream_state state)
{
	if (state >= XMPP_ST_MAX || !state_names[state])
		return "unknown";

	return state_names[state];
}

/**
 * random_fill() - draw entropy through the configured source
 * @xs: the stream
 * @buf: receives the bytes
 * @len: how many
 */
static void random_fill(struct xmpp_stream *xs, void *buf, size_t len)
{
	if (xs->cfg.random) {
		xs->cfg.random(buf, len);
		return;
	}

	if (!sip_os_random(buf, len))
		memset(buf, 0, len);
}

/**
 * log_line() - one syslog line through the host, when there is one
 * @xs: the stream
 * @level: the syslog level
 * @text: the line
 */
static void log_line(struct xmpp_stream *xs, int level, const char *text)
{
	if (!xs->env.host || !xs->env.host->log)
		return;

	xs->env.host->log(xs->env.host->ctx, level, "xmpp %s: %s", xs->jid_text,
			  text);
}

/**
 * state_set() - move the stream and tell the owner
 * @xs: the stream
 * @state: the new state
 * @reason: the text of a connection that ended, "" otherwise
 */
static void state_set(struct xmpp_stream *xs, enum xmpp_stream_state state,
		      const char *reason)
{
	xs->state = state;
	XMPP_TRACE(xs, "state %s%s%s", xmpp_stream_state_name(state),
		   reason[0] ? ": " : "", reason);

	if (xs->cb.state)
		xs->cb.state(xs, state, reason);
}

/**
 * tick_arm() - keep the sweep running while the stream lives
 * @xs: the stream
 */
static void tick_arm(struct xmpp_stream *xs)
{
	if (xs->state == XMPP_ST_IDLE)
		return;

	uloop_timeout_set(&xs->tick, XMPP_TICK_MS);
}

/**
 * deadline_set() - bound the step the stream just started
 * @xs: the stream
 */
static void deadline_set(struct xmpp_stream *xs)
{
	xs->deadline_ms = mono_ms() + XMPP_STEP_MS;
}

/**
 * mask_set() - the uloop events the socket waits for
 * @xs: the stream
 * @mask: ULOOP_READ, ULOOP_WRITE or both
 */
static void mask_set(struct xmpp_stream *xs, unsigned int mask)
{
	if (xs->ufd.fd < 0)
		return;

	uloop_fd_add(&xs->ufd, mask);
}

/* ------------------------------------------------------------ pending iqs */

/**
 * iq_slot_find() - the pending request with an id
 * @xs: the stream
 * @id: the id, NULL matches nothing
 *
 * Return: the slot, or NULL.
 */
static struct xmpp_iq_pending *iq_slot_find(struct xmpp_stream *xs,
					    const char *id)
{
	unsigned int i;

	if (!id)
		return NULL;

	for (i = 0; i < XMPP_IQ_MAX; i++) {
		if (xs->iq[i].in_use && !strcmp(xs->iq[i].id, id))
			return &xs->iq[i];
	}

	return NULL;
}

/**
 * iq_slot_take() - a free pending slot
 * @xs: the stream
 *
 * Return: the slot, or NULL when XMPP_IQ_MAX requests wait.
 */
static struct xmpp_iq_pending *iq_slot_take(struct xmpp_stream *xs)
{
	unsigned int i;

	for (i = 0; i < XMPP_IQ_MAX; i++) {
		if (!xs->iq[i].in_use)
			return &xs->iq[i];
	}

	return NULL;
}

/**
 * iq_slot_fire() - release a slot and run its callback
 * @xs: the stream
 * @slot: the slot
 * @reply: the answer, NULL for none
 */
static void iq_slot_fire(struct xmpp_stream *xs, struct xmpp_iq_pending *slot,
			 const struct xml_doc *reply)
{
	xmpp_iq_cb cb = slot->cb;
	void *priv = slot->priv;

	memset(slot, 0, sizeof(*slot));

	if (cb)
		cb(xs, reply, priv);
}

/**
 * iq_pending_flush() - answer every waiting request with nothing
 * @xs: the stream
 */
static void iq_pending_flush(struct xmpp_stream *xs)
{
	unsigned int i;

	for (i = 0; i < XMPP_IQ_MAX; i++) {
		if (xs->iq[i].in_use)
			iq_slot_fire(xs, &xs->iq[i], NULL);
	}
}

/**
 * iq_sweep() - time out the requests that waited too long
 * @xs: the stream
 * @now: the clock
 */
static void iq_sweep(struct xmpp_stream *xs, uint64_t now)
{
	unsigned int i;

	for (i = 0; i < XMPP_IQ_MAX; i++) {
		if (!xs->iq[i].in_use || now < xs->iq[i].deadline_ms)
			continue;

		xs->stat.iq_timeouts++;
		iq_slot_fire(xs, &xs->iq[i], NULL);
	}
}

/* ------------------------------------------------------------- connection */

/**
 * fd_drop() - close the socket and forget it
 * @xs: the stream
 */
static void fd_drop(struct xmpp_stream *xs)
{
	if (xs->ufd.fd < 0)
		return;

	uloop_fd_delete(&xs->ufd);
	sip_tls_conn_close(&xs->tls);
	close(xs->ufd.fd);
	xs->ufd.fd = -1;
}

/**
 * conn_close() - end the connection and clear everything it carried
 * @xs: the stream
 *
 * The pending requests are answered with nothing after the socket is gone,
 * so a callback that sends finds the stream closed.
 */
static void conn_close(struct xmpp_stream *xs)
{
	xs->gen++;
	fd_drop(xs);

	if (xs->dns_query != SIP_HANDLE_NONE && xs->env.dns) {
		sip_dns_cancel(xs->env.dns, xs->dns_query);
		xs->dns_query = SIP_HANDLE_NONE;
	}

	xs->tls_up = false;
	xs->authenticated = false;
	xs->session_needed = false;
	xs->pending = XMPP_PENDING_NONE;
	xs->ping_waiting = false;
	memset(&xs->sm, 0, sizeof(xs->sm));
	memset(&xs->bound, 0, sizeof(xs->bound));
	xs->tx_len = 0;
	xs->tx_off = 0;
	sasl_client_wipe(&xs->sasl);

	iq_pending_flush(xs);
}

/**
 * stream_fail() - end the connection and schedule the reconnect
 * @xs: the stream
 * @reason: why, copied before the connection goes
 */
static void stream_fail(struct xmpp_stream *xs, const char *reason)
{
	unsigned int delay;

	str_copy(xs->reason, sizeof(xs->reason), reason);
	xs->stat.failures++;
	log_line(xs, LOG_NOTICE, xs->reason);

	conn_close(xs);
	if (xs->state == XMPP_ST_IDLE)
		return;

	xs->fails++;
	delay = retry_delay_ms(xs->fails, XMPP_BACKOFF_MS, XMPP_BACKOFF_MAX_MS);
	xs->backoff_until = mono_ms() + delay;
	state_set(xs, XMPP_ST_BACKOFF, xs->reason);
	tick_arm(xs);
}

/**
 * sock_keepalive() - the TCP keepalive schedule of the socket
 * @fd: the socket
 */
static void sock_keepalive(int fd)
{
	int on = 1;
	int idle = XMPP_KA_IDLE_S;
	int intvl = XMPP_KA_INTVL_S;
	int cnt = XMPP_KA_CNT;

	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

/**
 * connect_start() - open a non blocking connect to one target
 * @xs: the stream
 * @target: the resolved target
 *
 * Return: 0 when the connect is in flight, -1 when the socket refused it.
 */
static int connect_start(struct xmpp_stream *xs,
			 const struct sip_dns_target *target)
{
	const struct sip_txn_dst *dst = &target->dst;
	int fd;

	fd = socket(dst->sa.ss_family,
		    SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	sock_keepalive(fd);

	if (connect(fd, (const struct sockaddr *)&dst->sa, dst->sa_len) < 0 &&
	    errno != EINPROGRESS && errno != EINTR) {
		close(fd);
		return -1;
	}

	xs->ufd.fd = fd;
	xs->ufd.cb = fd_io;

	if (uloop_fd_add(&xs->ufd, ULOOP_WRITE) < 0) {
		close(fd);
		xs->ufd.fd = -1;
		return -1;
	}

	xs->stat.connects++;
	deadline_set(xs);
	XMPP_TRACE(xs, "connect %s:%u", target->host, target->port);

	return 0;
}

/**
 * connect_next() - try the next resolved target, or give up
 * @xs: the stream
 */
static void connect_next(struct xmpp_stream *xs)
{
	while (xs->target_next < xs->targets) {
		if (!connect_start(xs, &xs->target[xs->target_next++]))
			return;
	}

	stream_fail(xs, "connect: no target reachable");
}

/**
 * resolve_start() - ask the resolver for the server
 * @xs: the stream
 */
static void resolve_start(struct xmpp_stream *xs)
{
	const char *service = xs->cfg.direct_tls ? XMPP_SRV_TLS :
						   XMPP_SRV_STARTTLS;
	uint16_t port = xs->cfg.port;
	uint32_t query;

	if (!port)
		port = xs->cfg.direct_tls ? XMPP_PORT_TLS : XMPP_PORT;

	if (!xs->env.dns) {
		stream_fail(xs, "dns: no resolver");
		return;
	}

	if (xs->cfg.host[0])
		query = sip_dns_resolve(xs->env.dns, xs->cfg.host, SIP_DNS_ADDR,
					port, xs->env.dns_owner);
	else
		query = sip_dns_resolve_svc(xs->env.dns, xs->jid.domain,
					    SIP_DNS_SRV, port,
					    xs->env.dns_owner, service);

	if (query == SIP_HANDLE_NONE) {
		stream_fail(xs, "dns: the resolver refused the query");
		return;
	}

	xs->dns_query = query;
	state_set(xs, XMPP_ST_RESOLVE, "");
	deadline_set(xs);
	tick_arm(xs);
}

void xmpp_stream_dns_done(struct xmpp_stream *xs, uint32_t query,
			  const struct sip_dns_result *res)
{
	char reason[XMPP_REASON_MAX];
	unsigned int count;

	if (xs->state != XMPP_ST_RESOLVE || query != xs->dns_query)
		return;

	xs->dns_query = SIP_HANDLE_NONE;

	if (res->status != SIP_DNS_OK || !res->count) {
		snprintf(reason, sizeof(reason), "dns: %s",
			 sip_dns_status_name(res->status));
		stream_fail(xs, reason);
		return;
	}

	count = res->count;
	if (count > SIP_DNS_TARGET_MAX)
		count = SIP_DNS_TARGET_MAX;

	memcpy(xs->target, res->target, count * sizeof(xs->target[0]));
	xs->targets = count;
	xs->target_next = 0;

	state_set(xs, XMPP_ST_CONNECT, "");
	connect_next(xs);
}

/* ---------------------------------------------------------------- transmit */

/**
 * tx_stage() - append bytes to the staging buffer
 * @xs: the stream
 * @buf: the bytes
 * @len: their count
 *
 * Return: true when they fit.
 */
static bool tx_stage(struct xmpp_stream *xs, const char *buf, size_t len)
{
	size_t held = xs->tx_len - xs->tx_off;

	if (len > XMPP_TX_MAX - held)
		return false;

	if (xs->tx_off) {
		memmove(xs->tx, xs->tx + xs->tx_off, held);
		xs->tx_len = held;
		xs->tx_off = 0;
	}

	memcpy(xs->tx + xs->tx_len, buf, len);
	xs->tx_len += len;

	return true;
}

/**
 * conn_write() - move bytes to the socket, through TLS once it is up
 * @xs: the stream
 * @buf: the bytes
 * @len: their count
 * @done: receives how many moved
 * @want: receives ULOOP_READ when the record layer waits for input
 *
 * Return: 0, or -1 when the connection is dead.
 */
static int conn_write(struct xmpp_stream *xs, const char *buf, size_t len,
		      size_t *done, unsigned int *want)
{
	enum sip_tls_io io;
	ssize_t moved;

	*done = 0;
	*want = 0;

	if (xs->tls_up) {
		io = sip_tls_conn_write(&xs->tls, buf, len, done);
		if (io == SIP_TLS_ERR)
			return -1;
		if (io == SIP_TLS_WANT_READ)
			*want = ULOOP_READ;
		return 0;
	}

	do {
		moved = send(xs->ufd.fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
	} while (moved < 0 && errno == EINTR);

	if (moved >= 0) {
		*done = (size_t)moved;
		return 0;
	}

	return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}

/**
 * tx_drain() - write what is staged
 * @xs: the stream
 *
 * It may end the connection, so the caller checks the generation after it.
 *
 * Return: ULOOP_WRITE when the socket is full, 0 otherwise.
 */
static unsigned int tx_drain(struct xmpp_stream *xs)
{
	char reason[XMPP_REASON_MAX];
	unsigned int want;
	size_t done;

	while (xs->tx_off < xs->tx_len) {
		if (conn_write(xs, xs->tx + xs->tx_off, xs->tx_len - xs->tx_off,
			       &done, &want) < 0) {
			snprintf(reason, sizeof(reason), "write: %s",
				 xs->tls_up ? sip_tls_conn_error(&xs->tls) :
					      strerror(errno));
			stream_fail(xs, reason);
			return 0;
		}

		if (!done)
			return want == ULOOP_READ ? 0 : ULOOP_WRITE;

		xs->tx_off += done;
	}

	xs->tx_off = 0;
	xs->tx_len = 0;

	return 0;
}

/**
 * tx_put() - stage text and push it toward the socket
 * @xs: the stream
 * @text: the text
 * @len: its length
 * @secret: whether the trace must not show it
 *
 * Return: 0, -ENOTCONN without a socket, -ENOSPC when it does not fit.
 */
static int tx_put(struct xmpp_stream *xs, const char *text, size_t len,
		  bool secret)
{
	unsigned int gen = xs->gen;
	unsigned int mask;

	if (xs->ufd.fd < 0)
		return -ENOTCONN;

	if (!tx_stage(xs, text, len)) {
		xs->stat.tx_refused++;
		return -ENOSPC;
	}

	if (!secret)
		XMPP_TRACE(xs, "tx %.*s", (int)(len > XMPP_TRACE_TEXT_MAX ?
						 XMPP_TRACE_TEXT_MAX : len),
			   text);
	xs->stat.tx_bytes += len;

	mask = tx_drain(xs);
	if (xs->gen != gen)
		return -ENOTCONN;

	mask_set(xs, ULOOP_READ | mask);

	return 0;
}

/**
 * stanza_put() - send one stanza and count it
 * @xs: the stream
 * @text: the serialised stanza
 * @len: its length
 *
 * Return: the tx_put() verdict.
 */
static int stanza_put(struct xmpp_stream *xs, const char *text, size_t len)
{
	int ret = tx_put(xs, text, len, false);

	if (ret)
		return ret;

	xs->stat.tx_stanzas++;
	if (xs->sm.enabled)
		xs->sm.out++;

	return 0;
}

/**
 * iq_send() - send an iq and remember who waits for its answer
 * @xs: the stream
 * @text: the serialised iq
 * @len: its length
 * @id: its id
 * @cb: the callback
 * @priv: handed to the callback
 *
 * Return: 0, -EBUSY when no slot is free, or the stanza_put() error.
 */
static int iq_send(struct xmpp_stream *xs, const char *text, size_t len,
		   const char *id, xmpp_iq_cb cb, void *priv)
{
	struct xmpp_iq_pending *slot = iq_slot_take(xs);
	int ret;

	if (!slot)
		return -EBUSY;

	str_copy(slot->id, sizeof(slot->id), id);
	slot->cb = cb;
	slot->priv = priv;
	slot->deadline_ms = mono_ms() + XMPP_IQ_MS;
	slot->in_use = true;

	ret = stanza_put(xs, text, len);
	if (ret)
		memset(slot, 0, sizeof(*slot));

	return ret;
}

/**
 * writer_put() - send what a writer built
 * @xs: the stream
 * @writer: the writer, finished here
 * @secret: whether the trace must not show it
 *
 * Return: 0, -EINVAL when the writer failed, or the tx_put() error.
 */
static int writer_put(struct xmpp_stream *xs, struct xml_w *writer, bool secret)
{
	size_t len = xml_w_finish(writer);

	if (!len)
		return -EINVAL;

	return tx_put(xs, writer->mem, len, secret);
}

/* ------------------------------------------------------------- negotiation */

/**
 * header_send() - start a new XML document toward the server
 * @xs: the stream
 */
static void header_send(struct xmpp_stream *xs)
{
	char from[2 * JID_MAX];
	char buf[XMPP_HEADER_MAX];
	int len;

	if (xml_stream_reset(&xs->parser)) {
		stream_fail(xs, "parser: reset failed");
		return;
	}

	if (!xml_escape(xs->jid_text, from, sizeof(from))) {
		stream_fail(xs, "jid: too long for the header");
		return;
	}

	len = snprintf(buf, sizeof(buf),
		       "<?xml version='1.0'?><stream:stream from='%s' to='%s' "
		       "version='1.0' xml:lang='en' xmlns='" XMPP_NS_CLIENT
		       "' xmlns:stream='" XMPP_NS_STREAM "'>",
		       from, xs->jid.domain);
	if (len < 0 || (size_t)len >= sizeof(buf)) {
		stream_fail(xs, "header: too long");
		return;
	}

	if (tx_put(xs, buf, (size_t)len, false))
		return;

	state_set(xs, XMPP_ST_FEATURES, "");
	deadline_set(xs);
}

/**
 * tls_start() - begin the handshake on the connected socket
 * @xs: the stream
 */
static void tls_start(struct xmpp_stream *xs)
{
	struct sip_tls_client_opts opts = {
		.sni = xs->jid.domain,
		.host = xs->cfg.tls_verify ? xs->jid.domain : NULL,
		.alpn = xs->cfg.direct_tls ? XMPP_ALPN : NULL,
	};
	char reason[XMPP_REASON_MAX];

	if (!xs->env.tls ||
	    !sip_tls_conn_connect(xs->env.tls, &xs->tls, xs->ufd.fd, &opts)) {
		snprintf(reason, sizeof(reason), "tls: %s",
			 xs->env.tls ? sip_tls_conn_error(&xs->tls) :
				       "no context");
		stream_fail(xs, reason);
		return;
	}

	state_set(xs, XMPP_ST_TLS, "");
	deadline_set(xs);
	mask_set(xs, ULOOP_READ);
	handshake(xs);
}

/**
 * handshake() - one step of the TLS handshake
 * @xs: the stream
 */
static void handshake(struct xmpp_stream *xs)
{
	enum sip_tls_io io = sip_tls_conn_io(&xs->tls);
	char reason[XMPP_REASON_MAX];

	if (io == SIP_TLS_ERR) {
		snprintf(reason, sizeof(reason), "tls: %s",
			 sip_tls_conn_error(&xs->tls));
		stream_fail(xs, reason);
		return;
	}

	if (io != SIP_TLS_OK) {
		mask_set(xs, io == SIP_TLS_WANT_WRITE ? ULOOP_WRITE :
							ULOOP_READ);
		return;
	}

	xs->tls_up = true;
	XMPP_TRACE(xs, "tls up");
	header_send(xs);
}

/**
 * connected() - the connect finished, one way or the other
 * @xs: the stream
 */
static void connected(struct xmpp_stream *xs)
{
	socklen_t len = sizeof(int);
	int err = 0;

	if (getsockopt(xs->ufd.fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 ||
	    err) {
		XMPP_TRACE(xs, "connect refused: %s",
			   strerror(err ? err : errno));
		fd_drop(xs);
		connect_next(xs);
		return;
	}

	if (xs->cfg.direct_tls) {
		tls_start(xs);
		return;
	}

	mask_set(xs, ULOOP_READ);
	header_send(xs);
}

/**
 * starttls_send() - ask the server to secure the connection
 * @xs: the stream
 */
static void starttls_send(struct xmpp_stream *xs)
{
	static const char text[] = "<starttls xmlns='" XMPP_NS_TLS "'/>";

	if (tx_put(xs, text, sizeof(text) - 1, false))
		return;

	state_set(xs, XMPP_ST_STARTTLS, "");
	deadline_set(xs);
}

/**
 * sasl_element_send() - one SASL element with a base64 body
 * @xs: the stream
 * @name: "auth" or "response"
 * @mech: the mechanism attribute of an auth, NULL for a response
 * @clear: the message in the clear
 * @clen: its length
 */
static void sasl_element_send(struct xmpp_stream *xs, const char *name,
			      const char *mech, const char *clear, size_t clen)
{
	char b64[XMPP_SASL_B64_MAX];
	char mem[XMPP_SASL_B64_MAX + 128];
	struct xml_w writer;

	if (clen && b64_encode(clear, clen, b64, sizeof(b64)) < 0) {
		stream_fail(xs, "sasl: message too long");
		return;
	}

	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, name);
	xml_w_attr(&writer, "xmlns", XMPP_NS_SASL);
	if (mech)
		xml_w_attr(&writer, "mechanism", mech);
	xml_w_text(&writer, clen ? b64 : "=");
	xml_w_end(&writer);

	if (writer_put(xs, &writer, true))
		stream_fail(xs, "sasl: send refused");
}

/**
 * sasl_start() - pick a mechanism from the features and open the exchange
 * @xs: the stream
 * @features: the stream features
 */
static void sasl_start(struct xmpp_stream *xs, const struct xml_doc *features)
{
	const char *names[XMPP_MECHS_MAX];
	uint8_t nonce[SASL_NONCE_BYTES];
	char clear[SASL_MSG_MAX];
	unsigned int list, node, count = 0;
	enum sasl_mech mech;
	int len;

	list = xml_child(features, XML_NONE, "mechanisms", XMPP_NS_SASL);
	if (list == XML_NONE) {
		stream_fail(xs, "sasl: server offers no mechanism");
		return;
	}

	node = xml_child(features, list, "mechanism", XMPP_NS_SASL);
	while (node != XML_NONE && count < XMPP_MECHS_MAX) {
		names[count++] = xml_text_get(features, node);
		node = xml_next(features, node, "mechanism", XMPP_NS_SASL);
	}

	mech = sasl_mech_pick(names, count, xs->cfg.allow_plain);
	if (mech == SASL_MECH_NONE) {
		stream_fail(xs, "sasl: no usable mechanism");
		return;
	}

	random_fill(xs, nonce, sizeof(nonce));
	memset(&xs->sasl, 0, sizeof(xs->sasl));
	if (sasl_client_init(&xs->sasl, mech, xs->jid.local, xs->cfg.password,
			     nonce)) {
		stream_fail(xs, "sasl: credentials refused");
		return;
	}

	len = sasl_client_first(&xs->sasl, clear, sizeof(clear));
	if (len < 0) {
		stream_fail(xs, "sasl: first message failed");
		return;
	}

	XMPP_TRACE(xs, "sasl %s", sasl_mech_name(mech));
	sasl_element_send(xs, "auth", sasl_mech_name(mech), clear, (size_t)len);
	if (xs->ufd.fd < 0)
		return;

	state_set(xs, XMPP_ST_SASL, "");
	deadline_set(xs);
}

/**
 * sasl_body_decode() - the clear text of a SASL element
 * @doc: the element
 * @out: receives the clear text, terminated
 * @len: capacity of @out
 *
 * Return: the clear length, or -1 for base64 this layer cannot read.
 */
static int sasl_body_decode(const struct xml_doc *doc, char *out, size_t len)
{
	const char *text = xml_text_get(doc, 0);
	int decoded;

	if (!text[0] || !strcmp(text, "=")) {
		out[0] = 0;
		return 0;
	}

	decoded = b64_decode(text, out, len - 1);
	if (decoded < 0)
		return -1;

	out[decoded] = 0;

	return decoded;
}

/**
 * sasl_challenge_rx() - answer the server-first message
 * @xs: the stream
 * @doc: the challenge
 */
static void sasl_challenge_rx(struct xmpp_stream *xs, const struct xml_doc *doc)
{
	char server[SASL_MSG_MAX];
	char clear[SASL_MSG_MAX];
	int slen, clen;

	slen = sasl_body_decode(doc, server, sizeof(server));
	if (slen < 0) {
		stream_fail(xs, "sasl: unreadable challenge");
		return;
	}

	clen = sasl_client_step(&xs->sasl, server, (size_t)slen, clear,
				sizeof(clear));
	if (clen < 0) {
		stream_fail(xs, "sasl: challenge refused");
		return;
	}

	sasl_element_send(xs, "response", NULL, clear, (size_t)clen);
}

/**
 * sasl_success_rx() - check the server proof and restart the stream
 * @xs: the stream
 * @doc: the success element
 */
static void sasl_success_rx(struct xmpp_stream *xs, const struct xml_doc *doc)
{
	char server[SASL_MSG_MAX];
	int slen;

	slen = sasl_body_decode(doc, server, sizeof(server));
	if (slen < 0 || sasl_client_verify(&xs->sasl, server, (size_t)slen)) {
		stream_fail(xs, "sasl: server proof wrong");
		return;
	}

	xs->authenticated = true;
	sasl_client_wipe(&xs->sasl);
	XMPP_TRACE(xs, "authenticated");
	xs->pending = XMPP_PENDING_RESTART;
}

/**
 * sasl_failure_rx() - the server refused the credentials
 * @xs: the stream
 * @doc: the failure element
 */
static void sasl_failure_rx(struct xmpp_stream *xs, const struct xml_doc *doc)
{
	char reason[XMPP_REASON_MAX];
	unsigned int cond = xml_child(doc, XML_NONE, NULL, XMPP_NS_SASL);

	snprintf(reason, sizeof(reason), "sasl: %s",
		 cond == XML_NONE ? "failure" : doc->node[cond].name);
	log_line(xs, LOG_WARNING, "authentication failed, check the password");
	xs->fails = XMPP_AUTH_FAILS;
	stream_fail(xs, reason);
}

/**
 * sasl_rx() - one element of the SASL namespace
 * @xs: the stream
 * @doc: the element
 */
static void sasl_rx(struct xmpp_stream *xs, const struct xml_doc *doc)
{
	const char *name = doc->node[0].name;

	if (xs->state != XMPP_ST_SASL)
		return;

	if (!strcmp(name, "challenge"))
		sasl_challenge_rx(xs, doc);
	else if (!strcmp(name, "success"))
		sasl_success_rx(xs, doc);
	else if (!strcmp(name, "failure"))
		sasl_failure_rx(xs, doc);
}

/**
 * tls_rx() - one element of the STARTTLS namespace
 * @xs: the stream
 * @doc: the element
 */
static void tls_rx(struct xmpp_stream *xs, const struct xml_doc *doc)
{
	if (xs->state != XMPP_ST_STARTTLS)
		return;

	if (!strcmp(doc->node[0].name, "proceed"))
		xs->pending = XMPP_PENDING_TLS;
	else
		stream_fail(xs, "starttls refused");
}

/**
 * bind_done() - the answer to the resource bind
 * @xs: the stream
 * @reply: the iq, NULL for none
 * @priv: unused
 */
static void bind_done(struct xmpp_stream *xs, const struct xml_doc *reply,
		      void *priv)
{
	const char *type;
	unsigned int bind, jid;

	if (xs->state != XMPP_ST_BIND)
		return;

	if (!reply) {
		stream_fail(xs, "bind: no answer");
		return;
	}

	type = xml_attr_get(reply, 0, "type");
	if (!type || strcmp(type, "result")) {
		stream_fail(xs, "bind: refused");
		return;
	}

	bind = xml_child(reply, XML_NONE, "bind", XMPP_NS_BIND);
	jid = xml_child(reply, bind, "jid", XMPP_NS_BIND);
	if (!jid_parse(xml_text_get(reply, jid), &xs->bound)) {
		stream_fail(xs, "bind: unreadable jid");
		return;
	}

	XMPP_TRACE(xs, "bound %s", xml_text_get(reply, jid));
	negotiate_next(xs);
}

/**
 * bind_start() - read the features after SASL and bind the resource
 * @xs: the stream
 * @features: the stream features
 */
static void bind_start(struct xmpp_stream *xs, const struct xml_doc *features)
{
	const char *resource = xs->jid.resource[0] ? xs->jid.resource :
						     XMPP_RESOURCE_DEFAULT;
	char id[XMPP_ID_MAX];
	char mem[XMPP_REPLY_MAX];
	struct xml_w writer;
	unsigned int session;

	if (xml_child(features, XML_NONE, "bind", XMPP_NS_BIND) == XML_NONE) {
		stream_fail(xs, "server offers no bind");
		return;
	}

	session = xml_child(features, XML_NONE, "session", XMPP_NS_SESSION);
	xs->session_needed = session != XML_NONE &&
			     xml_child(features, session, "optional", NULL) ==
			     XML_NONE;
	xs->sm.offered = xml_child(features, XML_NONE, "sm", XMPP_NS_SM) !=
			 XML_NONE;

	xmpp_stream_id_next(xs, id, sizeof(id));
	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "iq");
	xml_w_attr(&writer, "type", "set");
	xml_w_attr(&writer, "id", id);
	xml_w_start(&writer, "bind");
	xml_w_attr(&writer, "xmlns", XMPP_NS_BIND);
	xml_w_start(&writer, "resource");
	xml_w_text(&writer, resource);
	xml_w_end(&writer);
	xml_w_end(&writer);
	xml_w_end(&writer);

	state_set(xs, XMPP_ST_BIND, "");
	deadline_set(xs);

	if (!xml_w_finish(&writer) ||
	    iq_send(xs, mem, writer.used, id, bind_done, NULL))
		stream_fail(xs, "bind: send refused");
}

/**
 * session_done() - the answer to the session request
 * @xs: the stream
 * @reply: the iq, NULL for none
 * @priv: unused
 */
static void session_done(struct xmpp_stream *xs, const struct xml_doc *reply,
			 void *priv)
{
	if (xs->state != XMPP_ST_SESSION)
		return;

	if (!reply) {
		stream_fail(xs, "session: no answer");
		return;
	}

	negotiate_next(xs);
}

/**
 * session_send() - the session request of RFC 3921
 * @xs: the stream
 */
static void session_send(struct xmpp_stream *xs)
{
	char id[XMPP_ID_MAX];
	char mem[XMPP_REPLY_MAX];
	struct xml_w writer;

	xmpp_stream_id_next(xs, id, sizeof(id));
	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "iq");
	xml_w_attr(&writer, "type", "set");
	xml_w_attr(&writer, "id", id);
	xml_w_start(&writer, "session");
	xml_w_attr(&writer, "xmlns", XMPP_NS_SESSION);
	xml_w_end(&writer);
	xml_w_end(&writer);

	state_set(xs, XMPP_ST_SESSION, "");
	deadline_set(xs);

	if (!xml_w_finish(&writer) ||
	    iq_send(xs, mem, writer.used, id, session_done, NULL))
		stream_fail(xs, "session: send refused");
}

/**
 * sm_enable_send() - turn on stream management
 * @xs: the stream
 */
static void sm_enable_send(struct xmpp_stream *xs)
{
	static const char text[] = "<enable xmlns='" XMPP_NS_SM "'/>";

	state_set(xs, XMPP_ST_SM, "");
	deadline_set(xs);

	if (tx_put(xs, text, sizeof(text) - 1, false))
		stream_fail(xs, "sm: send refused");
}

/**
 * carbons_done() - the answer to the carbons request
 * @xs: the stream
 * @reply: the iq, NULL for none
 * @priv: unused
 */
static void carbons_done(struct xmpp_stream *xs, const struct xml_doc *reply,
			 void *priv)
{
	const char *type = reply ? xml_attr_get(reply, 0, "type") : NULL;

	if (xs->state != XMPP_ST_CARBONS)
		return;

	xs->carbons = type && !strcmp(type, "result");
	negotiate_next(xs);
}

/**
 * carbons_send() - ask for the copies of the other resources' messages
 * @xs: the stream
 */
static void carbons_send(struct xmpp_stream *xs)
{
	char id[XMPP_ID_MAX];
	char mem[XMPP_REPLY_MAX];
	struct xml_w writer;

	xmpp_stream_id_next(xs, id, sizeof(id));
	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "iq");
	xml_w_attr(&writer, "type", "set");
	xml_w_attr(&writer, "id", id);
	xml_w_start(&writer, "enable");
	xml_w_attr(&writer, "xmlns", XMPP_NS_CARBONS);
	xml_w_end(&writer);
	xml_w_end(&writer);

	state_set(xs, XMPP_ST_CARBONS, "");
	deadline_set(xs);

	if (!xml_w_finish(&writer) ||
	    iq_send(xs, mem, writer.used, id, carbons_done, NULL))
		stream_fail(xs, "carbons: send refused");
}

/**
 * ready() - the negotiation finished
 * @xs: the stream
 */
static void ready(struct xmpp_stream *xs)
{
	unsigned int ping_s = xs->cfg.ping_s ? xs->cfg.ping_s : XMPP_PING_S;

	xs->fails = 0;
	xs->deadline_ms = 0;
	xs->ping_waiting = false;
	xs->ping_due_ms = mono_ms() + (uint64_t)ping_s * 1000;
	state_set(xs, XMPP_ST_READY, "");
}

/**
 * negotiate_next() - the step after the one that just finished
 * @xs: the stream
 */
static void negotiate_next(struct xmpp_stream *xs)
{
	switch (xs->state) {
	case XMPP_ST_BIND:
		if (xs->session_needed) {
			session_send(xs);
			return;
		}
		fallthrough;
	case XMPP_ST_SESSION:
		if (xs->sm.offered) {
			sm_enable_send(xs);
			return;
		}
		fallthrough;
	case XMPP_ST_SM:
		carbons_send(xs);
		return;
	case XMPP_ST_CARBONS:
		ready(xs);
		return;
	default:
		return;
	}
}

/**
 * features_rx() - the stream features of the current document
 * @xs: the stream
 * @doc: the features element
 */
static void features_rx(struct xmpp_stream *xs, const struct xml_doc *doc)
{
	if (xs->state != XMPP_ST_FEATURES)
		return;

	if (!xs->tls_up) {
		if (xml_child(doc, XML_NONE, "starttls", XMPP_NS_TLS) ==
		    XML_NONE)
			stream_fail(xs, "server offers no starttls");
		else
			starttls_send(xs);
		return;
	}

	if (!xs->authenticated) {
		sasl_start(xs, doc);
		return;
	}

	bind_start(xs, doc);
}

/**
 * stream_error_rx() - the server ends the stream with an error
 * @xs: the stream
 * @doc: the error element
 */
static void stream_error_rx(struct xmpp_stream *xs, const struct xml_doc *doc)
{
	char reason[XMPP_REASON_MAX];
	unsigned int cond = xml_child(doc, XML_NONE, NULL, XMPP_NS_STREAMS);

	snprintf(reason, sizeof(reason), "stream error: %s",
		 cond == XML_NONE ? "unspecified" : doc->node[cond].name);
	stream_fail(xs, reason);
}

/* -------------------------------------------------------- stanzas in ready */

/**
 * sm_ack_send() - report how many stanzas arrived
 * @xs: the stream
 */
static void sm_ack_send(struct xmpp_stream *xs)
{
	char mem[128];
	struct xml_w writer;

	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "a");
	xml_w_attr(&writer, "xmlns", XMPP_NS_SM);
	xml_w_attr_uint(&writer, "h", xs->sm.in);
	xml_w_end(&writer);
	writer_put(xs, &writer, false);
}

/**
 * sm_rx() - one element of the stream management namespace
 * @xs: the stream
 * @doc: the element
 */
static void sm_rx(struct xmpp_stream *xs, const struct xml_doc *doc)
{
	const char *name = doc->node[0].name;
	const char *h;

	if (xs->state == XMPP_ST_SM) {
		if (!strcmp(name, "enabled")) {
			xs->sm.enabled = true;
			XMPP_TRACE(xs, "stream management on");
		} else if (!strcmp(name, "failed")) {
			xs->sm.offered = false;
		} else {
			return;
		}
		negotiate_next(xs);
		return;
	}

	if (!xs->sm.enabled)
		return;

	if (!strcmp(name, "r")) {
		sm_ack_send(xs);
		return;
	}

	h = xml_attr_get(doc, 0, "h");
	if (!strcmp(name, "a") && h)
		xs->sm.acked = (uint32_t)strtoul(h, NULL, 10);
}

/**
 * reply_open() - start the iq that answers another
 * @writer: the writer
 * @mem: its buffer
 * @len: the capacity
 * @iq: the iq to answer
 * @type: "result" or "error"
 */
static void reply_open(struct xml_w *writer, char *mem, size_t len,
		       const struct xml_doc *iq, const char *type)
{
	const char *from = xml_attr_get(iq, 0, "from");
	const char *id = xml_attr_get(iq, 0, "id");

	xml_w_init(writer, mem, len);
	xml_w_start(writer, "iq");
	xml_w_attr(writer, "type", type);
	if (from)
		xml_w_attr(writer, "to", from);
	if (id)
		xml_w_attr(writer, "id", id);
}

/**
 * reply_close() - finish the answer and send it
 * @xs: the stream
 * @writer: the writer with the open iq
 *
 * Return: the stanza_put() verdict, or -EINVAL when the writer failed.
 */
static int reply_close(struct xmpp_stream *xs, struct xml_w *writer)
{
	size_t len;

	xml_w_end(writer);
	len = xml_w_finish(writer);
	if (!len)
		return -EINVAL;

	return stanza_put(xs, writer->mem, len);
}

int xmpp_stream_iq_result(struct xmpp_stream *xs, const struct xml_doc *iq)
{
	char mem[XMPP_REPLY_MAX];
	struct xml_w writer;

	reply_open(&writer, mem, sizeof(mem), iq, "result");

	return reply_close(xs, &writer);
}

int xmpp_stream_iq_error(struct xmpp_stream *xs, const struct xml_doc *iq,
			 const char *type, const char *condition)
{
	char mem[XMPP_REPLY_MAX];
	struct xml_w writer;

	reply_open(&writer, mem, sizeof(mem), iq, "error");
	xml_w_start(&writer, "error");
	xml_w_attr(&writer, "type", type);
	xml_w_start(&writer, condition);
	xml_w_attr(&writer, "xmlns", XMPP_NS_STANZAS);
	xml_w_end(&writer);
	xml_w_end(&writer);

	return reply_close(xs, &writer);
}

/**
 * disco_reply() - answer a disco#info query with the feature set
 * @xs: the stream
 * @iq: the query
 * @query: the query node
 */
static void disco_reply(struct xmpp_stream *xs, const struct xml_doc *iq,
			unsigned int query)
{
	char mem[XMPP_REPLY_MAX];
	struct xml_w writer;

	reply_open(&writer, mem, sizeof(mem), iq, "result");
	disco_info_write(&writer, xml_attr_get(iq, query, "node"));
	reply_close(xs, &writer);
}

/**
 * iq_reply_rx() - an iq of type result or error
 * @xs: the stream
 * @doc: the iq
 */
static void iq_reply_rx(struct xmpp_stream *xs, const struct xml_doc *doc)
{
	struct xmpp_iq_pending *slot;

	slot = iq_slot_find(xs, xml_attr_get(doc, 0, "id"));
	if (!slot) {
		xs->stat.iq_unmatched++;
		return;
	}

	iq_slot_fire(xs, slot, doc);
}

/**
 * iq_rx() - one iq stanza
 * @xs: the stream
 * @doc: the iq
 */
static void iq_rx(struct xmpp_stream *xs, const struct xml_doc *doc)
{
	const char *type = xml_attr_get(doc, 0, "type");
	unsigned int query;

	if (!type)
		return;

	if (!strcmp(type, "result") || !strcmp(type, "error")) {
		iq_reply_rx(xs, doc);
		return;
	}

	if (strcmp(type, "get") && strcmp(type, "set"))
		return;

	if (!strcmp(type, "get")) {
		if (xml_child(doc, XML_NONE, "ping", XMPP_NS_PING) !=
		    XML_NONE) {
			xs->stat.pings++;
			xmpp_stream_iq_result(xs, doc);
			return;
		}

		query = xml_child(doc, XML_NONE, "query", DISCO_NS_INFO);
		if (query != XML_NONE) {
			disco_reply(xs, doc, query);
			return;
		}
	}

	if (xs->state == XMPP_ST_READY && xs->cb.stanza &&
	    xs->cb.stanza(xs, doc))
		return;

	xmpp_stream_iq_error(xs, doc, "cancel", "service-unavailable");
}

/**
 * stanza_rx() - one stanza of the client namespace
 * @xs: the stream
 * @doc: the stanza
 */
static void stanza_rx(struct xmpp_stream *xs, const struct xml_doc *doc)
{
	xs->stat.rx_stanzas++;
	if (xs->sm.enabled)
		xs->sm.in++;

	if (!strcmp(doc->node[0].name, "iq")) {
		iq_rx(xs, doc);
		return;
	}

	if (xs->state == XMPP_ST_READY && xs->cb.stanza)
		xs->cb.stanza(xs, doc);
}

/**
 * element_rx() - one depth 1 element of the stream
 * @xs: the stream
 * @doc: the element
 */
static void element_rx(struct xmpp_stream *xs, const struct xml_doc *doc)
{
	const struct xml_node *root = &doc->node[0];
	unsigned int ping_s = xs->cfg.ping_s ? xs->cfg.ping_s : XMPP_PING_S;

	xs->ping_waiting = false;
	xs->ping_due_ms = mono_ms() + (uint64_t)ping_s * 1000;

	if (!strcmp(root->ns, XMPP_NS_STREAM)) {
		if (!strcmp(root->name, "features"))
			features_rx(xs, doc);
		else if (!strcmp(root->name, "error"))
			stream_error_rx(xs, doc);
		return;
	}

	if (!strcmp(root->ns, XMPP_NS_TLS)) {
		tls_rx(xs, doc);
		return;
	}

	if (!strcmp(root->ns, XMPP_NS_SASL)) {
		sasl_rx(xs, doc);
		return;
	}

	if (!strcmp(root->ns, XMPP_NS_SM)) {
		sm_rx(xs, doc);
		return;
	}

	if (!strcmp(root->ns, XMPP_NS_CLIENT)) {
		stanza_rx(xs, doc);
		return;
	}

	XMPP_TRACE(xs, "rx: element %s of %s ignored", root->name, root->ns);
}

/* ------------------------------------------------------------------ parser */

/**
 * parser_open() - the server's stream header
 * @user: the stream
 * @header: the root element
 */
static void parser_open(void *user, const struct xml_doc *header)
{
	struct xmpp_stream *xs = user;
	const char *id = xml_attr_get(header, 0, "id");

	if (xs->ufd.fd < 0)
		return;

	str_copy(xs->stream_id, sizeof(xs->stream_id), id ? id : "");
	XMPP_TRACE(xs, "stream open, id %s", xs->stream_id);
}

/**
 * parser_stanza() - one complete depth 1 element
 * @user: the stream
 * @doc: the element
 */
static void parser_stanza(void *user, const struct xml_doc *doc)
{
	struct xmpp_stream *xs = user;

	if (xs->ufd.fd < 0)
		return;

	element_rx(xs, doc);
}

/**
 * parser_close() - the server closed its document
 * @user: the stream
 */
static void parser_close(void *user)
{
	struct xmpp_stream *xs = user;

	if (xs->ufd.fd < 0 || xs->stopping)
		return;

	stream_fail(xs, "peer closed the stream");
}

/**
 * parser_error() - the document broke
 * @user: the stream
 * @reason: why
 */
static void parser_error(void *user, const char *reason)
{
	struct xmpp_stream *xs = user;
	char text[XMPP_REASON_MAX];

	if (xs->ufd.fd < 0)
		return;

	snprintf(text, sizeof(text), "xml: %s", reason);
	stream_fail(xs, text);
}

static const struct xml_stream_cb parser_cb = {
	.stream_open = parser_open,
	.stanza = parser_stanza,
	.stream_close = parser_close,
	.error = parser_error,
};

/* ----------------------------------------------------------------- receive */

/**
 * conn_read() - take bytes from the socket, through TLS once it is up
 * @xs: the stream
 * @buf: receives the bytes
 * @len: its capacity
 * @got: receives how many arrived
 * @want: receives ULOOP_WRITE when the record layer waits for output
 *
 * Return: what happened.
 */
static enum rx_step conn_read(struct xmpp_stream *xs, char *buf, size_t len,
			      size_t *got, unsigned int *want)
{
	enum sip_tls_io io;
	ssize_t moved;

	*got = 0;
	*want = 0;

	if (xs->tls_up) {
		io = sip_tls_conn_read(&xs->tls, buf, len, got);
		if (io == SIP_TLS_OK)
			return *got ? RX_DATA : RX_EOF;
		if (io == SIP_TLS_ERR)
			return RX_FAIL;
		if (io == SIP_TLS_WANT_WRITE)
			*want = ULOOP_WRITE;
		return RX_IDLE;
	}

	do {
		moved = recv(xs->ufd.fd, buf, len, MSG_DONTWAIT);
	} while (moved < 0 && errno == EINTR);

	if (moved > 0) {
		*got = (size_t)moved;
		return RX_DATA;
	}

	if (!moved)
		return RX_EOF;

	return (errno == EAGAIN || errno == EWOULDBLOCK) ? RX_IDLE : RX_FAIL;
}

/**
 * rx_feed() - parse one chunk and perform what its callbacks asked for
 * @xs: the stream
 * @chunk: the bytes
 * @got: their count
 *
 * Return: true while the connection is the same one.
 */
static bool rx_feed(struct xmpp_stream *xs, const char *chunk, size_t got)
{
	unsigned int gen = xs->gen;

	xs->stat.rx_bytes += got;
	XMPP_TRACE(xs, "rx %.*s",
		   (int)(got > XMPP_TRACE_TEXT_MAX ? XMPP_TRACE_TEXT_MAX : got),
		   chunk);

	xml_stream_feed(&xs->parser, chunk, got);
	if (xs->gen != gen)
		return false;

	switch (xs->pending) {
	case XMPP_PENDING_TLS:
		xs->pending = XMPP_PENDING_NONE;
		tls_start(xs);
		break;
	case XMPP_PENDING_RESTART:
		xs->pending = XMPP_PENDING_NONE;
		header_send(xs);
		break;
	case XMPP_PENDING_NONE:
		break;
	}

	return xs->gen == gen;
}

/**
 * rx_pump() - read until the socket is empty or the burst is spent
 * @xs: the stream
 *
 * Return: ULOOP_WRITE when the record layer needs it, 0 otherwise.
 */
static unsigned int rx_pump(struct xmpp_stream *xs)
{
	char chunk[XMPP_RX_CHUNK];
	char reason[XMPP_REASON_MAX];
	unsigned int want, i;
	size_t got;

	for (i = 0; i < XMPP_RX_BURST; i++) {
		switch (conn_read(xs, chunk, sizeof(chunk), &got, &want)) {
		case RX_DATA:
			if (!rx_feed(xs, chunk, got))
				return 0;
			break;
		case RX_IDLE:
			return want;
		case RX_EOF:
			stream_fail(xs, "peer closed the connection");
			return 0;
		case RX_FAIL:
			snprintf(reason, sizeof(reason), "read: %s",
				 xs->tls_up ? sip_tls_conn_error(&xs->tls) :
					      strerror(errno));
			stream_fail(xs, reason);
			return 0;
		}
	}

	return 0;
}

/**
 * up_io() - a socket event of a connection that speaks XML
 * @xs: the stream
 */
static void up_io(struct xmpp_stream *xs)
{
	unsigned int gen = xs->gen;
	unsigned int mask = ULOOP_READ;

	mask |= rx_pump(xs);
	if (xs->gen != gen)
		return;

	mask |= tx_drain(xs);
	if (xs->gen != gen)
		return;

	mask_set(xs, mask);
}

/**
 * fd_io() - the uloop callback of the socket
 * @ufd: the socket
 * @events: unused, the state picks the step
 */
static void fd_io(struct uloop_fd *ufd, unsigned int events)
{
	struct xmpp_stream *xs = container_of(ufd, struct xmpp_stream, ufd);

	switch (xs->state) {
	case XMPP_ST_CONNECT:
		connected(xs);
		return;
	case XMPP_ST_TLS:
		handshake(xs);
		return;
	case XMPP_ST_FEATURES:
	case XMPP_ST_STARTTLS:
	case XMPP_ST_SASL:
	case XMPP_ST_BIND:
	case XMPP_ST_SESSION:
	case XMPP_ST_SM:
	case XMPP_ST_CARBONS:
	case XMPP_ST_READY:
		up_io(xs);
		return;
	default:
		return;
	}
}

/* -------------------------------------------------------------------- tick */

/**
 * ping_done() - the answer to a liveness ping, which element_rx() took
 * @xs: unused
 * @reply: unused
 * @priv: unused
 */
static void ping_done(struct xmpp_stream *xs, const struct xml_doc *reply,
		      void *priv)
{
}

/**
 * ping_send() - ask the server for a sign of life
 * @xs: the stream
 */
static void ping_send(struct xmpp_stream *xs)
{
	static const char request[] = "<r xmlns='" XMPP_NS_SM "'/>";
	char id[XMPP_ID_MAX];
	char mem[256];
	struct xml_w writer;

	xs->stat.pings++;

	if (xs->sm.enabled) {
		tx_put(xs, request, sizeof(request) - 1, false);
		return;
	}

	xmpp_stream_id_next(xs, id, sizeof(id));
	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "iq");
	xml_w_attr(&writer, "type", "get");
	xml_w_attr(&writer, "to", xs->jid.domain);
	xml_w_attr(&writer, "id", id);
	xml_w_start(&writer, "ping");
	xml_w_attr(&writer, "xmlns", XMPP_NS_PING);
	xml_w_end(&writer);
	xml_w_end(&writer);

	if (xml_w_finish(&writer))
		iq_send(xs, mem, writer.used, id, ping_done, NULL);
}

/**
 * ping_tick() - the liveness rule of a ready stream
 * @xs: the stream
 * @now: the clock
 */
static void ping_tick(struct xmpp_stream *xs, uint64_t now)
{
	if (xs->ping_waiting) {
		if (now >= xs->deadline_ms)
			stream_fail(xs, "no answer to the ping");
		return;
	}

	if (now < xs->ping_due_ms)
		return;

	xs->ping_waiting = true;
	xs->deadline_ms = now + XMPP_PONG_MS;
	ping_send(xs);
}

/**
 * step_tick() - the deadline of a connecting or negotiating stream
 * @xs: the stream
 * @now: the clock
 */
static void step_tick(struct xmpp_stream *xs, uint64_t now)
{
	char reason[XMPP_REASON_MAX];

	if (now < xs->deadline_ms)
		return;

	if (xs->state == XMPP_ST_CONNECT) {
		XMPP_TRACE(xs, "connect timed out");
		fd_drop(xs);
		connect_next(xs);
		return;
	}

	snprintf(reason, sizeof(reason), "timeout in %s",
		 xmpp_stream_state_name(xs->state));
	stream_fail(xs, reason);
}

/**
 * tick_fire() - the one second sweep
 * @timeout: the timer
 */
static void tick_fire(struct uloop_timeout *timeout)
{
	struct xmpp_stream *xs = container_of(timeout, struct xmpp_stream,
					      tick);
	uint64_t now = mono_ms();

	iq_sweep(xs, now);

	switch (xs->state) {
	case XMPP_ST_IDLE:
		return;
	case XMPP_ST_BACKOFF:
		if (now >= xs->backoff_until)
			resolve_start(xs);
		break;
	case XMPP_ST_READY:
		ping_tick(xs, now);
		break;
	default:
		step_tick(xs, now);
		break;
	}

	tick_arm(xs);
}

/* --------------------------------------------------------------- lifecycle */

void xmpp_stream_init(struct xmpp_stream *xs, const struct xmpp_stream_cb *cb,
		      void *priv, const struct xmpp_stream_env *env)
{
	memset(xs, 0, sizeof(*xs));
	xs->cb = *cb;
	xs->priv = priv;
	xs->env = *env;
	xs->ufd.fd = -1;
	xs->tick.cb = tick_fire;
	xs->dns_query = SIP_HANDLE_NONE;
}

int xmpp_stream_start(struct xmpp_stream *xs, const struct xmpp_stream_cfg *cfg)
{
	if (xs->state != XMPP_ST_IDLE)
		return -EBUSY;

	if (!jid_parse(cfg->jid, &xs->jid) || !xs->jid.local[0] ||
	    !cfg->password[0])
		return -EINVAL;

	if (!xs->parser_ready) {
		if (xml_stream_init(&xs->parser, &parser_cb, xs))
			return -ENOMEM;
		xs->parser_ready = true;
	}

	xs->cfg = *cfg;
	jid_bare(&xs->jid, xs->jid_text, sizeof(xs->jid_text));
	xs->reason[0] = 0;
	xs->fails = 0;
	resolve_start(xs);

	return 0;
}

/**
 * header_sent() - whether this document carries an open stream header
 * @xs: the stream
 *
 * Return: true when a close tag is owed.
 */
static bool header_sent(const struct xmpp_stream *xs)
{
	switch (xs->state) {
	case XMPP_ST_FEATURES:
	case XMPP_ST_STARTTLS:
	case XMPP_ST_SASL:
	case XMPP_ST_BIND:
	case XMPP_ST_SESSION:
	case XMPP_ST_SM:
	case XMPP_ST_CARBONS:
	case XMPP_ST_READY:
		return true;
	default:
		return false;
	}
}

void xmpp_stream_stop(struct xmpp_stream *xs)
{
	static const char close_tag[] = "</stream:stream>";

	if (xs->state == XMPP_ST_IDLE)
		return;

	xs->stopping = true;
	if (header_sent(xs))
		tx_put(xs, close_tag, sizeof(close_tag) - 1, false);

	conn_close(xs);
	uloop_timeout_cancel(&xs->tick);
	xs->fails = 0;
	xs->stopping = false;
	state_set(xs, XMPP_ST_IDLE, "stopped");
}

void xmpp_stream_done(struct xmpp_stream *xs)
{
	xmpp_stream_stop(xs);

	if (xs->parser_ready) {
		xml_stream_done(&xs->parser);
		xs->parser_ready = false;
	}
}

enum xmpp_stream_state xmpp_stream_state(const struct xmpp_stream *xs)
{
	return xs->state;
}

bool xmpp_stream_ready(const struct xmpp_stream *xs)
{
	return xs->state == XMPP_ST_READY;
}

const struct jid *xmpp_stream_jid(const struct xmpp_stream *xs)
{
	return &xs->bound;
}

const char *xmpp_stream_reason(const struct xmpp_stream *xs)
{
	return xs->reason;
}

void xmpp_stream_id_next(struct xmpp_stream *xs, char *out, size_t len)
{
	uint32_t salt;

	random_fill(xs, &salt, sizeof(salt));
	xs->id_seq++;
	snprintf(out, len, "uv%08x%08x", xs->id_seq, salt);
}

int xmpp_stream_send(struct xmpp_stream *xs, const char *text, size_t len)
{
	if (xs->state != XMPP_ST_READY)
		return -ENOTCONN;

	return stanza_put(xs, text, len);
}

int xmpp_stream_iq_send(struct xmpp_stream *xs, const char *text, size_t len,
			const char *id, xmpp_iq_cb cb, void *priv)
{
	if (xs->state != XMPP_ST_READY)
		return -ENOTCONN;

	return iq_send(xs, text, len, id, cb, priv);
}

const struct xmpp_stream_stat *xmpp_stream_stat(const struct xmpp_stream *xs)
{
	return &xs->stat;
}
