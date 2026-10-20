// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the TCP and TLS transport (src/proto/sip/stream.c).
 *
 * Real sockets, real uloop. Every layer above this one runs on a virtual clock
 * in its own suite. This suite therefore tests only the engine against the
 * kernel, with loopback listeners and non blocking connects. When a case needs
 * byte level control, the peer is a plain socket. When a case needs the accept
 * side, the peer is a second struct sip_stream of the same module.
 *
 * Each case drives the loop in short slices until the event that it waits for
 * arrives or its window ends. A broken expectation therefore fails and does not
 * hang.
 *
 * The tick is the virtual clock. Two rules of sip/stream.h take too long for a
 * real wait: the inbound idle close at SIP_STREAM_IDLE_MS and the connect bound
 * at SIP_STREAM_CONNECT_MS. A struct sip_stream carries its sweep as a public
 * struct uloop_timeout. A struct sip_conn carries used_ms, deadline_ms and
 * backoff_until. A case moves the timestamp of a connection into the past and
 * calls st.tick.cb(&st.tick) by hand. That is the whole virtual clock of this
 * suite, and the production code needs no test hook for it.
 *
 * The cases never assume an old monotonic clock. A case pushes a timestamp back
 * with an unsigned subtraction, and the sweep undoes it with the same unsigned
 * arithmetic. The cases therefore hold on a machine that booted a second ago.
 *
 * The two families. Every case that binds ::1 is in a group that asks
 * tu_v6_loopback() once. A host with no IPv6 loopback skips every case of that
 * group, with the reason that the probe reports. Such a case counts as skipped
 * and never as passed. A run in a network namespace with IPv6 loopback runs
 * all of them. The IPv4 helpers call the family-wide helpers for 127.0.0.1, so
 * an IPv4 case names no family.
 *
 * TLS fixtures. The fixtures are the committed PEM files of test/unit/tls/.
 * src/CMakeLists.txt defines UVOICE_TLS_FIXTURE_DIR for test_sip_stream and
 * test_sip_tls. This suite takes the environment variable UVOICE_TLS_FIXTURES
 * first, then that define when a build supplies it, then the tls directory
 * beside its own source path. A run that finds none fails the suite through a
 * suite check, which prints the failed expression. A silent skip would report
 * a pass for a suite that verifies nothing about the TLS kind.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <openssl/ssl.h>

#include "posix.h"
#include "proto/sip/stream.h"
#include "proto/sip/tap.h"
#include "proto_host_fake.h"
#include "test_util.h"

#define LOG_MAX		24

/* one turn of the loop when a case waits for something to happen */
#define SLICE_MS	10

/* the window a case gives an event that has to cross the loopback */
#define WAIT_MS		2000

/*
 * The bound a MESSAGE stages into. stream.c reserves the last
 * SIP_FRAME_PONG_LEN bytes of the transmit buffer for the keepalive answer, so
 * a caller never reaches SIP_STREAM_TX_MAX; the reserve is spelled out here
 * because it is an internal rule of the engine and not a fixed constant.
 */
#define MSG_MAX		(SIP_STREAM_TX_MAX - SIP_FRAME_PONG_LEN)

/* small enough that a few kilobytes fill the loopback pipe both ways */
#define TINY_BUF	2048

/* the reason tu_v6_loopback() reports to a group it turns into skips */
#define WHY_MAX		128

struct log {
	unsigned int msgs;
	unsigned int want;

	uint32_t conn[LOG_MAX];
	char method[LOG_MAX][16];
	uint32_t status[LOG_MAX];
	struct sip_txn_dst src[LOG_MAX];
	size_t len[LOG_MAX];

	unsigned int evs;
	uint32_t ev_conn[LOG_MAX];
	uint8_t ev_state[LOG_MAX];

	/* the "Rx reentrancy" switch: close the connection inside the upcall */
	bool close_on_msg;
};

static struct sip_stream cli;
static struct sip_stream srv;
static struct log cli_log;
static struct log srv_log;
static struct sip_tls tls;

static char fix_cert[PATH_MAX];
static char fix_key[PATH_MAX];

/* larger than the staging buffer, for the refusal case */
static char oversize[SIP_STREAM_TX_MAX + 64];

/* --------------------------------------------------------------- upcalls */

static void log_reset(struct log *l, unsigned int want)
{
	memset(l, 0, sizeof(*l));
	l->want = want;
}

static void on_msg(struct sip_stream *st, uint32_t conn, struct sip_msg *msg,
		   const struct sip_txn_dst *src, void *priv)
{
	struct log *l = priv;
	unsigned int i = l->msgs;

	if (i < LOG_MAX) {
		l->conn[i] = conn;
		l->len[i] = msg->len;
		l->status[i] = msg->status;
		l->src[i] = *src;
		if (msg->is_request)
			sip_str_copy(l->method[i], sizeof(l->method[i]), msg->method_name);
	}

	l->msgs++;

	if (l->close_on_msg)
		sip_stream_conn_close(st, conn);

	if (l->want && l->msgs >= l->want)
		uloop_end();
}

static void on_conn(struct sip_stream *st, uint32_t conn, enum sip_conn_state state,
		    void *priv)
{
	struct log *l = priv;

	if (l->evs < LOG_MAX) {
		l->ev_conn[l->evs] = conn;
		l->ev_state[l->evs] = (uint8_t)state;
	}

	l->evs++;
}

static const struct sip_stream_cb cb = {
	.msg = on_msg,
	.conn = on_conn,
};

/* ------------------------------------------------------------- the loop */

static void guard_expire(struct uloop_timeout *t)
{
	uloop_end();
}

static void loop_pump(unsigned int ms)
{
	static struct uloop_timeout guard;

	guard.cb = guard_expire;
	uloop_timeout_set(&guard, (int)ms);
	uloop_cancelled = false;
	uloop_run();
	uloop_timeout_cancel(&guard);
}

static void wait_msgs(struct log *l, unsigned int count, unsigned int ms)
{
	unsigned int spent;

	for (spent = 0; spent < ms && l->msgs < count; spent += SLICE_MS)
		loop_pump(SLICE_MS);
}

static void wait_conns(struct sip_stream *st, unsigned int count, unsigned int ms)
{
	unsigned int spent;

	for (spent = 0; spent < ms && sip_stream_conn_count(st) != count; spent += SLICE_MS)
		loop_pump(SLICE_MS);
}

static struct sip_conn *conn_of(struct sip_stream *st, const struct sip_stream_key *key,
				const char *sni)
{
	return sip_stream_conn_find(st, key, sni);
}

static bool wait_up(struct sip_stream *st, const struct sip_stream_key *key, const char *sni,
		    unsigned int ms)
{
	struct sip_conn *c;
	unsigned int spent;

	for (spent = 0; spent < ms; spent += SLICE_MS) {
		c = conn_of(st, key, sni);
		if (c && c->state == SIP_CONN_UP)
			return true;

		loop_pump(SLICE_MS);
	}

	c = conn_of(st, key, sni);

	return c && c->state == SIP_CONN_UP;
}

static void wait_absent(struct sip_stream *st, const struct sip_stream_key *key,
			const char *sni, unsigned int ms)
{
	unsigned int spent;

	for (spent = 0; spent < ms && sip_stream_conn_find(st, key, sni); spent += SLICE_MS)
		loop_pump(SLICE_MS);
}

static void wait_state(struct sip_stream *st, const struct sip_stream_key *key,
		       const char *sni, enum sip_conn_state want, unsigned int ms)
{
	struct sip_conn *c;
	unsigned int spent;

	for (spent = 0; spent < ms; spent += SLICE_MS) {
		c = sip_stream_conn_find(st, key, sni);
		if (c && c->state == want)
			return;

		loop_pump(SLICE_MS);
	}
}

/* every in-use connection of the pool has drained its staging buffer */
static bool wait_drained(struct sip_stream *st, unsigned int ms)
{
	unsigned int spent, i;
	size_t held;

	for (spent = 0; spent < ms; spent += SLICE_MS) {
		held = 0;
		for (i = 0; i < SIP_STREAM_CONN_MAX; i++)
			held += sip_stream_conn_queued(&st->conn[i]);

		if (!held)
			return true;

		loop_pump(SLICE_MS);
	}

	return false;
}

/* ------------------------------------------------------- socket helpers */

/* the loopback address of one family, in the form a bind takes */
static socklen_t sa_loopback(int family, uint16_t port,
			     struct sockaddr_storage *sa)
{
	struct sockaddr_in *v4 = (struct sockaddr_in *)sa;
	struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)sa;

	memset(sa, 0, sizeof(*sa));

	if (family == AF_INET6) {
		v6->sin6_family = AF_INET6;
		v6->sin6_addr = in6addr_loopback;
		v6->sin6_port = htons(port);

		return sizeof(*v6);
	}

	v4->sin_family = AF_INET;
	v4->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	v4->sin_port = htons(port);

	return sizeof(*v4);
}

static uint16_t sa_port_of(const struct sockaddr_storage *sa)
{
	const struct sockaddr_in *v4 = (const struct sockaddr_in *)sa;
	const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)sa;

	if (sa->ss_family == AF_INET6)
		return ntohs(v6->sin6_port);

	return ntohs(v4->sin_port);
}

/* the loopback address of one family, in the form a configuration takes */
static const char *loop_addr(int family)
{
	return family == AF_INET6 ? "::1" : "127.0.0.1";
}

static uint16_t free_port_af(int family)
{
	struct sockaddr_storage sa;
	socklen_t blen = sa_loopback(family, 0, &sa);
	socklen_t slen = sizeof(sa);
	int fd = socket(family, SOCK_STREAM, 0);
	uint16_t port = 0;

	if (fd < 0)
		return 0;

	if (!bind(fd, (struct sockaddr *)&sa, blen) &&
	    !getsockname(fd, (struct sockaddr *)&sa, &slen))
		port = sa_port_of(&sa);

	close(fd);

	return port;
}

static uint16_t free_port(void)
{
	return free_port_af(AF_INET);
}

/* a listening socket this suite drives by hand, for byte level control */
static int raw_listen_af(int family, uint16_t *port, int backlog)
{
	struct sockaddr_storage sa;
	socklen_t blen = sa_loopback(family, 0, &sa);
	socklen_t slen = sizeof(sa);
	int fd = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

	if (fd < 0)
		return -1;

	if (bind(fd, (struct sockaddr *)&sa, blen) || listen(fd, backlog) ||
	    getsockname(fd, (struct sockaddr *)&sa, &slen)) {
		close(fd);
		return -1;
	}

	*port = sa_port_of(&sa);

	return fd;
}

static int raw_listen(uint16_t *port, int backlog)
{
	return raw_listen_af(AF_INET, port, backlog);
}

static int raw_accept(int lfd, unsigned int ms)
{
	unsigned int spent;
	int fd;

	for (spent = 0; spent < ms; spent += SLICE_MS) {
		fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
		if (fd >= 0)
			return fd;

		loop_pump(SLICE_MS);
	}

	return accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
}

static bool raw_write(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	size_t done = 0;
	ssize_t n;

	while (done < len) {
		n = send(fd, p + done, len - done, MSG_NOSIGNAL);
		if (n <= 0)
			return false;

		done += (size_t)n;
	}

	return true;
}

/* read exactly len bytes, driving the loop so the pool can write them */
static bool raw_read(int fd, char *buf, size_t len, unsigned int ms)
{
	unsigned int spent;
	size_t done = 0;
	ssize_t n;

	for (spent = 0; spent < ms && done < len; spent += SLICE_MS) {
		n = recv(fd, buf + done, len - done, MSG_DONTWAIT);
		if (n > 0) {
			done += (size_t)n;
			continue;
		}

		if (!n)
			break;

		loop_pump(SLICE_MS);
	}

	return done == len;
}

/* nothing came back, which is what a pong that must never be answered needs */
static bool raw_quiet(int fd, unsigned int ms)
{
	char scratch[64];
	ssize_t n;

	loop_pump(ms);
	n = recv(fd, scratch, sizeof(scratch), MSG_DONTWAIT);

	return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
}

/* a small pipe, so a few kilobytes leave bytes stuck in the staging buffer */
static void sock_buf_shrink(int fd)
{
	int bytes = TINY_BUF;

	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
}

/* drain the peer with no loop turn, so the engine keeps its staged bytes
 * until the next send decides what to do with them */
static size_t raw_sink(int fd)
{
	char scratch[4096];
	size_t total = 0;
	ssize_t n;

	for (;;) {
		n = recv(fd, scratch, sizeof(scratch), MSG_DONTWAIT);
		if (n <= 0)
			return total;

		total += (size_t)n;
	}
}

/*
 * Stage one byte at a time until the engine refuses the next one. The transmit
 * buffer is then at exactly the bound a message may reach, whatever that bound
 * is, so the case never has to encode the reserve to measure it.
 */
static size_t tx_fill(struct sip_stream *st, const struct sip_stream_key *key, uint8_t tos)
{
	unsigned int i;

	for (i = 0; i < SIP_STREAM_TX_MAX + 8; i++) {
		if (sip_stream_send(st, key, tos, "x", 1) < 0)
			break;
	}

	return sip_stream_conn_queued(sip_stream_conn_find(st, key, NULL));
}

static bool wait_eof(int fd, unsigned int ms)
{
	char scratch[8];
	unsigned int spent;
	ssize_t n;

	for (spent = 0; spent < ms; spent += SLICE_MS) {
		n = recv(fd, scratch, sizeof(scratch), MSG_DONTWAIT);
		if (!n)
			return true;

		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			return false;

		loop_pump(SLICE_MS);
	}

	return false;
}

static int tos_read_af(const struct sip_conn *c, int family)
{
	int level = family == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP;
	int opt = family == AF_INET6 ? IPV6_TCLASS : IP_TOS;
	socklen_t len = sizeof(int);
	int val = -1;

	if (!c || getsockopt(c->ufd.fd, level, opt, &val, &len) < 0)
		return -1;

	return val;
}

static int tos_read(const struct sip_conn *c)
{
	return tos_read_af(c, AF_INET);
}

/* the option that keeps an accepted key in the family the peer really used */
static int v6only_read(const struct sip_listener *l)
{
	socklen_t len = sizeof(int);
	int val = -1;

	if (!l || getsockopt(l->ufd.fd, IPPROTO_IPV6, IPV6_V6ONLY, &val,
			     &len) < 0)
		return -1;

	return val;
}

/* ------------------------------------------------------- a hand driven peer */

/*
 * The stream pool drives its own records, and a record boundary is exactly
 * what the receive rules below have to be tested against. This peer is an
 * OpenSSL client on a plain socket, so a case says how many records leave and
 * how big each one is.
 */
struct tls_peer {
	struct ssl_ctx_st *ctx;
	struct ssl_st *ssl;
	int fd;
};

static bool tls_peer_open(struct tls_peer *p, uint16_t port)
{
	struct sockaddr_in sa;

	memset(p, 0, sizeof(*p));
	p->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (p->fd < 0)
		return false;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = htons(port);

	if (connect(p->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 && errno != EINPROGRESS)
		return false;

	p->ctx = SSL_CTX_new(TLS_client_method());
	if (!p->ctx)
		return false;

	SSL_CTX_set_mode(p->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE |
			 SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
	p->ssl = SSL_new(p->ctx);
	if (!p->ssl)
		return false;

	SSL_set_fd(p->ssl, p->fd);
	SSL_set_connect_state(p->ssl);

	return true;
}

static void tls_peer_close(struct tls_peer *p)
{
	if (p->ssl)
		SSL_free(p->ssl);

	if (p->ctx)
		SSL_CTX_free(p->ctx);

	if (p->fd >= 0)
		close(p->fd);

	memset(p, 0, sizeof(*p));
	p->fd = -1;
}

static bool tls_peer_up(struct tls_peer *p, unsigned int ms)
{
	unsigned int spent;

	for (spent = 0; spent < ms; spent += SLICE_MS) {
		if (SSL_do_handshake(p->ssl) == 1)
			return true;

		loop_pump(SLICE_MS);
	}

	return SSL_do_handshake(p->ssl) == 1;
}

/* one SSL_write, so the bytes leave as exactly one record */
static bool tls_peer_record(struct tls_peer *p, const void *buf, size_t len)
{
	size_t done = 0;

	if (SSL_write_ex(p->ssl, buf, len, &done) != 1)
		return false;

	return done == len;
}

/* ---------------------------------------------------------- keys and wire */

static void key_make_addr(struct sip_stream_key *key, const char *addr,
			  uint16_t peer_port, uint16_t local_port,
			  enum sip_stream_kind kind)
{
	memset(key, 0, sizeof(*key));
	sip_dst_set(&key->peer, addr, peer_port);
	key->local_port = local_port;
	key->kind = (uint8_t)kind;
}

static void key_make(struct sip_stream_key *key, uint16_t peer_port,
		     uint16_t local_port, enum sip_stream_kind kind)
{
	key_make_addr(key, "127.0.0.1", peer_port, local_port, kind);
}

static void key_from_src(struct sip_stream_key *key, const struct sip_txn_dst *src,
			 uint16_t local_port, enum sip_stream_kind kind)
{
	memset(key, 0, sizeof(*key));
	key->peer = *src;
	key->local_port = local_port;
	key->kind = (uint8_t)kind;
}

/* the pool slot that remembers a key, in use or not: the backoff lives there */
static struct sip_conn *slot_of(struct sip_stream *st, const struct sip_stream_key *key)
{
	unsigned int i;

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		if (sip_stream_key_eq(&st->conn[i].key, key))
			return &st->conn[i];
	}

	return NULL;
}

static struct sip_conn *conn_inbound(struct sip_stream *st)
{
	unsigned int i;

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		if (st->conn[i].in_use && st->conn[i].inbound)
			return &st->conn[i];
	}

	return NULL;
}

static const char *reg_wire(void)
{
	return "REGISTER sip:example.com SIP/2.0\r\n"
	       "Via: SIP/2.0/TCP 127.0.0.1:5060;branch=z9hG4bK-uvoice-1;rport\r\n"
	       "From: <sip:a@example.com>;tag=t1\r\n"
	       "To: <sip:a@example.com>\r\n"
	       "Call-ID: c1@127.0.0.1\r\n"
	       "CSeq: 1 REGISTER\r\n"
	       "Content-Length: 0\r\n\r\n";
}

static const char *ok_wire(void)
{
	return "SIP/2.0 200 OK\r\n"
	       "Via: SIP/2.0/TCP 127.0.0.1:5060;branch=z9hG4bK-uvoice-1;rport\r\n"
	       "From: <sip:a@example.com>;tag=t1\r\n"
	       "To: <sip:a@example.com>;tag=t2\r\n"
	       "Call-ID: c1@127.0.0.1\r\n"
	       "CSeq: 1 REGISTER\r\n"
	       "Content-Length: 0\r\n\r\n";
}

static int send_reg(struct sip_stream *st, const struct sip_stream_key *key)
{
	const char *wire = reg_wire();

	return sip_stream_send(st, key, 0, wire, strlen(wire));
}

/* ------------------------------------------------------------- fixtures */

static const char *fixture_dir_define(void)
{
#ifdef UVOICE_TLS_FIXTURE_DIR
	return UVOICE_TLS_FIXTURE_DIR;
#else
	return NULL;
#endif
}

static bool fixture_dir_take(char *out, size_t olen)
{
	const char *env = getenv("UVOICE_TLS_FIXTURES");
	const char *def = fixture_dir_define();
	const char *sep;

	if (env && env[0]) {
		snprintf(out, olen, "%s", env);
		return true;
	}

	if (def && def[0]) {
		snprintf(out, olen, "%s", def);
		return true;
	}

	sep = strrchr(__FILE__, '/');
	if (!sep)
		return false;

	snprintf(out, olen, "%.*s/tls", (int)(sep - __FILE__), __FILE__);

	return true;
}

static bool fixtures_take(void)
{
	char dir[PATH_MAX - 32];

	if (!fixture_dir_take(dir, sizeof(dir)))
		return false;

	snprintf(fix_cert, sizeof(fix_cert), "%s/server.crt", dir);
	snprintf(fix_key, sizeof(fix_key), "%s/server.key", dir);

	return !access(fix_cert, R_OK) && !access(fix_key, R_OK);
}

/* ---------------------------------------------------------- pool fixtures */

/* a listening pool and a fresh client pool; the returned port is the listener */
static uint32_t pair_open_af(int family, uint16_t *port,
			     enum sip_stream_kind kind, struct sip_tls *t)
{
	struct sip_stream_listen_cfg cfg;

	sip_stream_init(&cli, &cb, &cli_log, t);
	sip_stream_init(&srv, &cb, &srv_log, t);
	log_reset(&cli_log, 0);
	log_reset(&srv_log, 0);

	memset(&cfg, 0, sizeof(cfg));
	cfg.addr = loop_addr(family);
	cfg.family = family;
	cfg.port = free_port_af(family);
	cfg.kind = kind;
	*port = cfg.port;

	return sip_stream_listen(&srv, &cfg);
}

static uint32_t pair_open(uint16_t *port, enum sip_stream_kind kind,
			  struct sip_tls *t)
{
	return pair_open_af(AF_INET, port, kind, t);
}

static void pair_close(void)
{
	sip_tap_set(NULL);
	sip_stream_done(&cli);
	sip_stream_done(&srv);
	loop_pump(SLICE_MS);
}

/* -------------------------------------------------------------------- init */

/*
 * The pool is zeroed around its slabs and never through them, so this case
 * runs first. Every case after it drives both pools over slabs that hold 0xff.
 * A byte read before a write put it there therefore fails a frame or a parse
 * below.
 */
static void t_init_dirty(void)
{
	static const struct sip_stream_stat zero_stat;
	unsigned int i;

	CASE("init: a pool initialised over garbage is clean around its slabs");
	memset(&cli, 0xff, sizeof(cli));
	sip_stream_init(&cli, &cb, &cli_log, NULL);
	CHECK(cli.cb.msg == cb.msg);
	CHECK(cli.cb.conn == cb.conn);
	CHECK(cli.priv == &cli_log);
	CHECK(cli.tls == NULL);
	CHECK(cli.tick.cb != NULL);
	CHECK(!cli.tick.pending);
	CHECK_UINT(cli.active, 0);
	CHECK(!memcmp(&cli.stat, &zero_stat, sizeof(zero_stat)));
	for (i = 0; i < SIP_STREAM_LISTEN_MAX; i++) {
		const struct sip_listener *l = &cli.listen[i];

		CHECK(l->st == &cli);
		CHECK_UINT(l->index, i);
		CHECK_UINT(l->gen, 1);
		CHECK(l->ufd.fd == -1);
		CHECK(l->ufd.cb == NULL);
		CHECK(!l->in_use);
		CHECK(!l->up);
		CHECK_UINT(l->refs, 0);
		CHECK_UINT(l->port, 0);
	}
	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		const struct sip_conn *c = &cli.conn[i];

		CHECK(c->st == &cli);
		CHECK_UINT(c->index, i);
		CHECK_UINT(c->gen, 1);
		CHECK(c->ufd.fd == -1);
		CHECK(c->ufd.cb == NULL);
		CHECK(!c->in_use);
		CHECK(!c->inbound);
		CHECK_UINT(c->state, SIP_CONN_FREE);
		CHECK_UINT(c->tx_len, 0);
		CHECK_UINT(c->tx_off, 0);
		CHECK_UINT(c->frames, 0);
		CHECK_UINT(c->used_ms, 0);
		CHECK_UINT(c->deadline_ms, 0);
		CHECK_UINT(c->backoff_until, 0);
		CHECK_UINT(c->backoff_ms, 0);
		CHECK_UINT(c->tos_cur, SIP_TOS_UNSET);
		CHECK_UINT(c->sni[0], 0);
		CHECK_UINT(c->key.kind, 0);
		CHECK_UINT(c->key.local_port, 0);
		CHECK_UINT(c->key.peer.sa_len, 0);
		CHECK(c->fr.buf == c->rx);
		CHECK_UINT(c->fr.cap, SIP_STREAM_RX_MAX);
		CHECK_UINT(c->fr.len, 0);
		CHECK_UINT(c->fr.scan, 0);
		CHECK(!c->fr.crlf_held);
		/* the slabs are what the init leaves alone */
		CHECK_UINT((unsigned char)c->rx[0], 0xff);
		CHECK_UINT((unsigned char)c->rx[SIP_STREAM_RX_MAX - 1], 0xff);
		CHECK_UINT((unsigned char)c->tx[0], 0xff);
		CHECK_UINT((unsigned char)c->tx[SIP_STREAM_TX_MAX - 1], 0xff);
	}

	/* the server pool takes the same treatment, so both ends of every
	 * later case run over dirty slabs */
	memset(&srv, 0xff, sizeof(srv));
	sip_stream_init(&srv, &cb, &srv_log, NULL);
	CHECK_UINT(srv.active, 0);
	CHECK(!memcmp(&srv.stat, &zero_stat, sizeof(zero_stat)));
}

/* --------------------------------------------------------------- constants */

static void t_constants(void)
{
	CASE("stream: the frozen pool, key and timing constants");
	CHECK_UINT(SIP_STREAM_CONN_MAX, 8);
	CHECK_UINT(SIP_STREAM_TCP, 0);
	CHECK_UINT(SIP_STREAM_RX_MAX, SIP_FRAME_BUF_MAX);
	CHECK_UINT(SIP_STREAM_IDLE_MS, 600000);
	CHECK_UINT(SIP_STREAM_BACKOFF_MAX_MS, SIP_STREAM_CONNECT_MS);
	CHECK_UINT(SIP_CONN_FREE, 0);
	CHECK_UINT(SIP_DST_F_NO_CONNECT, 1u);

	/* env first, so the send path recovers the binding by container_of */
	CHECK_UINT(offsetof(struct sip_stream_env, env), 0);

	CASE("stream: every kind and every state has a name");
	CHECK(!strcmp(sip_stream_kind_name(SIP_STREAM_TCP), "tcp"));
	CHECK(!strcmp(sip_stream_kind_name(SIP_STREAM_TLS), "tls"));
	CHECK(!strcmp(sip_stream_kind_name(SIP_STREAM_KIND_MAX), "unknown"));
	CHECK(!strcmp(sip_stream_kind_name((enum sip_stream_kind)42), "unknown"));
	CHECK(!strcmp(sip_conn_state_name(SIP_CONN_FREE), "free"));
	CHECK(!strcmp(sip_conn_state_name(SIP_CONN_CONNECTING), "connecting"));
	CHECK(!strcmp(sip_conn_state_name(SIP_CONN_HANDSHAKE), "handshake"));
	CHECK(!strcmp(sip_conn_state_name(SIP_CONN_UP), "up"));
	CHECK(!strcmp(sip_conn_state_name(SIP_CONN_CLOSING), "closing"));
	CHECK(!strcmp(sip_conn_state_name((enum sip_conn_state)42), "unknown"));
}

/* -------------------------------------------------------------- the key */

static void t_key(void)
{
	struct sip_stream_key a, b;

	CASE("key: the local identity, the peer and the kind all name a connection");
	key_make(&a, 5060, 5060, SIP_STREAM_TCP);
	b = a;
	CHECK(sip_stream_key_eq(&a, &b));

	b = a;
	b.local_port = 5062;
	CHECK(!sip_stream_key_eq(&a, &b));

	b = a;
	b.kind = SIP_STREAM_TLS;
	CHECK(!sip_stream_key_eq(&a, &b));

	b = a;
	snprintf(b.iface, sizeof(b.iface), "lo");
	CHECK(!sip_stream_key_eq(&a, &b));

	b = a;
	CHECK(sip_dst_set(&b.peer, "127.0.0.2", 5060));
	CHECK(!sip_stream_key_eq(&a, &b));

	b = a;
	CHECK(sip_dst_set(&b.peer, "127.0.0.1", 5061));
	CHECK(!sip_stream_key_eq(&a, &b));

	CASE("key: the connect-forbidden flag never splits one connection into two");
	b = a;
	b.peer.flags = SIP_DST_F_NO_CONNECT;
	CHECK(sip_stream_key_eq(&a, &b));

	CASE("key: a missing key compares to nothing");
	CHECK(!sip_stream_key_eq(NULL, &a));
	CHECK(!sip_stream_key_eq(&a, NULL));
	CHECK(!sip_stream_key_eq(NULL, NULL));
}

/*
 * What the send path accepts as a peer. The usable v6 key needs a socket in
 * that family, so it is the one case of this group behind the probe. The two
 * refusals never reach a socket, and run everywhere.
 */
static void t_key_family(void)
{
	struct sip_stream_key key;
	const char *reg = reg_wire();
	char why[WHY_MAX];
	uint64_t errs, conns;
	uint16_t port = 0;
	int lfd = -1;

	sip_stream_init(&cli, &cb, &cli_log, NULL);
	log_reset(&cli_log, 0);

	CASE("key: an AF_INET6 peer with a port is a usable key");
	if (!tu_v6_loopback(why, sizeof(why))) {
		tu_skip(why);
	} else {
		lfd = raw_listen_af(AF_INET6, &port, 8);
		CHECK(lfd >= 0);
		key_make_addr(&key, "::1", port, 5060, SIP_STREAM_TCP);
		CHECK_UINT(send_reg(&cli, &key), strlen(reg));
		CHECK_UINT(cli.stat.connects, 1);
		CHECK(conn_of(&cli, &key, NULL) != NULL);
		CHECK(wait_up(&cli, &key, NULL, WAIT_MS));
	}

	CASE("key: an AF_INET6 peer with a port of zero is refused and "
	     "counted");
	errs = cli.stat.tx_err;
	conns = cli.stat.connects;
	key_make_addr(&key, "::1", 0, 5060, SIP_STREAM_TCP);
	CHECK(sip_stream_send(&cli, &key, 0, reg, strlen(reg)) < 0);
	CHECK_UINT(cli.stat.tx_err, errs + 1);
	CHECK_UINT(cli.stat.connects, conns);

	CASE("key: an AF_UNIX peer is refused and counted");
	errs = cli.stat.tx_err;
	memset(&key, 0, sizeof(key));
	key.peer.sa.ss_family = AF_UNIX;
	key.peer.sa_len = sizeof(struct sockaddr_un);
	key.local_port = 5060;
	CHECK(sip_stream_send(&cli, &key, 0, reg, strlen(reg)) < 0);
	CHECK_UINT(cli.stat.tx_err, errs + 1);
	CHECK_UINT(cli.stat.connects, conns);

	if (lfd >= 0)
		close(lfd);

	sip_stream_done(&cli);
}

/* ------------------------------------------------------------- listeners */

static void t_listen(void)
{
	struct sip_stream_listen_cfg cfg, other;
	struct sip_listener *l;
	uint32_t h, h2, h3;
	socklen_t vlen;
	uint16_t port;
	int raw, val;

	sip_stream_init(&srv, &cb, &srv_log, NULL);
	log_reset(&srv_log, 0);

	CASE("listen: a fresh pool owns no listener, no connection and a zeroed stat");
	CHECK_UINT(sip_stream_listen_count(&srv), 0);
	CHECK_UINT(sip_stream_conn_count(&srv), 0);
	CHECK(sip_stream_stat(&srv) != NULL);
	CHECK_UINT(sip_stream_stat(&srv)->accepts, 0);
	CHECK(sip_stream_stat(NULL) == NULL);

	CASE("listen: a bound listener is up and holds one reference");
	port = free_port();
	CHECK(port != 0);
	memset(&cfg, 0, sizeof(cfg));
	cfg.addr = "127.0.0.1";
	cfg.port = port;
	cfg.kind = SIP_STREAM_TCP;
	h = sip_stream_listen(&srv, &cfg);
	CHECK(h != SIP_HANDLE_NONE);
	l = sip_stream_listener_get(&srv, h);
	CHECK(l != NULL);
	CHECK(l && l->up);
	CHECK_UINT(l ? l->refs : 0, 1);
	CHECK_UINT(l ? l->port : 0, port);
	CHECK_UINT(l ? l->kind : 99, SIP_STREAM_TCP);
	CHECK_UINT(sip_stream_listen_count(&srv), 1);

	CASE("listen: SO_REUSEADDR is set and SO_REUSEPORT is not");
	val = -1;
	vlen = sizeof(val);
	CHECK(l && getsockopt(l->ufd.fd, SOL_SOCKET, SO_REUSEADDR, &val, &vlen) == 0);
	CHECK(val != 0);
	val = -1;
	vlen = sizeof(val);
	CHECK(l && getsockopt(l->ufd.fd, SOL_SOCKET, SO_REUSEPORT, &val, &vlen) == 0);
	CHECK_UINT(val, 0);

	CASE("listen: two accounts on one port share one listener and two references");
	h2 = sip_stream_listen(&srv, &cfg);
	CHECK_UINT(h2, h);
	CHECK_UINT(sip_stream_listen_count(&srv), 1);
	CHECK_UINT(sip_stream_listener_get(&srv, h)->refs, 2);

	CASE("listen: another port takes a listener of its own");
	other = cfg;
	other.port = free_port();
	CHECK(other.port != 0 && other.port != port);
	h3 = sip_stream_listen(&srv, &other);
	CHECK(h3 != SIP_HANDLE_NONE);
	CHECK(h3 != h);
	CHECK_UINT(sip_stream_listen_count(&srv), 2);

	CASE("listen: port 0 is refused, because a listener needs a known port");
	other = cfg;
	other.port = 0;
	errno = 0;
	CHECK(sip_stream_listen(&srv, &other) == SIP_HANDLE_NONE);
	CHECK_UINT(errno, EINVAL);
	CHECK_UINT(sip_stream_listen_count(&srv), 2);

	CASE("listen: a TLS listener with no server context stays down");
	other = cfg;
	other.port = free_port();
	other.kind = SIP_STREAM_TLS;
	CHECK(sip_stream_listen(&srv, &other) == SIP_HANDLE_NONE);
	CHECK_UINT(sip_stream_listen_count(&srv), 2);

	CASE("listen: an unusable bind address and an oversized name are refused");
	other = cfg;
	other.port = free_port();
	other.addr = "not-an-address";
	CHECK(sip_stream_listen(&srv, &other) == SIP_HANDLE_NONE);
	other = cfg;
	other.port = free_port();
	other.iface = "an-interface-name-far-too-long";
	errno = 0;
	CHECK(sip_stream_listen(&srv, &other) == SIP_HANDLE_NONE);
	CHECK_UINT(errno, EINVAL);
	CHECK(sip_stream_listen(&srv, NULL) == SIP_HANDLE_NONE);
	CHECK(sip_stream_listen(NULL, &cfg) == SIP_HANDLE_NONE);

	CASE("listen: a port another socket already listens on is refused, never shared");
	{
		uint16_t taken = 0;

		raw = raw_listen(&taken, 4);
		CHECK(raw >= 0);
		other = cfg;
		other.port = taken;
		CHECK(sip_stream_listen(&srv, &other) == SIP_HANDLE_NONE);
		if (raw >= 0)
			close(raw);
	}
	CHECK_UINT(sip_stream_listen_count(&srv), 2);

	CASE("listen: the pool refuses one listener past SIP_STREAM_LISTEN_MAX");
	while (sip_stream_listen_count(&srv) < SIP_STREAM_LISTEN_MAX) {
		other = cfg;
		other.port = free_port();
		CHECK(sip_stream_listen(&srv, &other) != SIP_HANDLE_NONE);
	}
	CHECK_UINT(sip_stream_listen_count(&srv), SIP_STREAM_LISTEN_MAX);
	other = cfg;
	other.port = free_port();
	errno = 0;
	CHECK(sip_stream_listen(&srv, &other) == SIP_HANDLE_NONE);
	CHECK_UINT(errno, ENOSPC);

	CASE("listen: every reference needs its own close, and the last one releases it");
	sip_stream_listen_close(&srv, h);
	CHECK(sip_stream_listener_get(&srv, h) != NULL);
	CHECK_UINT(sip_stream_listen_count(&srv), SIP_STREAM_LISTEN_MAX);
	sip_stream_listen_close(&srv, h);
	CHECK(sip_stream_listener_get(&srv, h) == NULL);
	CHECK_UINT(sip_stream_listen_count(&srv), SIP_STREAM_LISTEN_MAX - 1);

	CASE("listen: a released handle never resolves again and a stray close is inert");
	sip_stream_listen_close(&srv, h);
	sip_stream_listen_close(&srv, 0xdeadbeef);
	sip_stream_listen_close(NULL, h);
	CHECK(sip_stream_listener_get(&srv, h) == NULL);
	CHECK_UINT(sip_stream_listen_count(&srv), SIP_STREAM_LISTEN_MAX - 1);

	CASE("listen: a released port can be bound again");
	other = cfg;
	other.port = port;
	CHECK(sip_stream_listen(&srv, &other) != SIP_HANDLE_NONE);
	CHECK_UINT(sip_stream_listen_count(&srv), SIP_STREAM_LISTEN_MAX);

	sip_stream_done(&srv);
	CHECK_UINT(sip_stream_listen_count(&srv), 0);
}

/* what a host with no IPv6 loopback reports instead of the group below */
static void t_listen6_skip(const char *why)
{
	CASE("listen6: a bound ::1 listener is up and holds one reference");
	tu_skip(why);
	CASE("listen6: IPV6_V6ONLY is set on the v6 listener");
	tu_skip(why);
	CASE("listen6: a v4 and a v6 listener on one port are two slots");
	tu_skip(why);
	CASE("listen6: the family joins the listener identity, so a refcount "
	     "is not shared");
	tu_skip(why);
}

static void t_listen6(void)
{
	struct sip_stream_listen_cfg cfg, v4;
	struct sip_listener *l;
	uint32_t h, h4, h6, again;
	char why[WHY_MAX];
	uint16_t port;

	if (!tu_v6_loopback(why, sizeof(why))) {
		t_listen6_skip(why);

		return;
	}

	sip_stream_init(&srv, &cb, &srv_log, NULL);
	log_reset(&srv_log, 0);

	CASE("listen6: a bound ::1 listener is up and holds one reference");
	port = free_port_af(AF_INET6);
	CHECK(port != 0);
	memset(&cfg, 0, sizeof(cfg));
	cfg.addr = "::1";
	cfg.family = AF_INET6;
	cfg.port = port;
	cfg.kind = SIP_STREAM_TCP;
	h = sip_stream_listen(&srv, &cfg);
	CHECK(h != SIP_HANDLE_NONE);
	l = sip_stream_listener_get(&srv, h);
	CHECK(l != NULL);
	CHECK(l && l->up);
	CHECK_UINT(l ? l->refs : 0, 1);
	CHECK_UINT(l ? l->family : 0, AF_INET6);
	CHECK_UINT(l ? l->port : 0, port);
	CHECK_UINT(sip_stream_listen_count(&srv), 1);

	/*
	 * The second read is the one that answers for this layer. A socket
	 * bound to a named IPv6 address is v6 only whatever the caller asked
	 * for, so only a wildcard listener shows the option the open set.
	 */
	CASE("listen6: IPV6_V6ONLY is set on the v6 listener");
	CHECK(v6only_read(l) > 0);
	cfg.addr = "";
	cfg.port = free_port_af(AF_INET6);
	CHECK(cfg.port != 0);
	h6 = sip_stream_listen(&srv, &cfg);
	CHECK(h6 != SIP_HANDLE_NONE);
	CHECK(v6only_read(sip_stream_listener_get(&srv, h6)) > 0);
	sip_stream_listen_close(&srv, h6);
	CHECK_UINT(sip_stream_listen_count(&srv), 1);

	CASE("listen6: a v4 and a v6 listener on one port are two slots");
	sip_stream_listen_close(&srv, h);
	CHECK_UINT(sip_stream_listen_count(&srv), 0);
	port = free_port();
	CHECK(port != 0);
	memset(&v4, 0, sizeof(v4));
	v4.addr = "127.0.0.1";
	v4.port = port;
	v4.kind = SIP_STREAM_TCP;
	cfg = v4;
	cfg.addr = "::1";
	cfg.family = AF_INET6;
	h4 = sip_stream_listen(&srv, &v4);
	h6 = sip_stream_listen(&srv, &cfg);
	CHECK(h4 != SIP_HANDLE_NONE);
	CHECK(h6 != SIP_HANDLE_NONE);
	CHECK(h4 != h6);
	CHECK_UINT(sip_stream_listen_count(&srv), 2);
	l = sip_stream_listener_get(&srv, h4);
	CHECK_UINT(l ? l->family : 0, AF_INET);
	l = sip_stream_listener_get(&srv, h6);
	CHECK_UINT(l ? l->family : 0, AF_INET6);
	sip_stream_listen_close(&srv, h4);
	sip_stream_listen_close(&srv, h6);
	CHECK_UINT(sip_stream_listen_count(&srv), 0);

	/*
	 * The wildcard names no family of its own, so the two configurations
	 * differ in the family alone. The v6 listener takes the port first and
	 * the v4 one still binds it, which is what IPV6_V6ONLY buys, and the
	 * second v6 open has to find the v6 slot rather than the v4 one.
	 */
	CASE("listen6: the family joins the listener identity, so a refcount "
	     "is not shared");
	port = free_port();
	CHECK(port != 0);
	memset(&v4, 0, sizeof(v4));
	v4.port = port;
	v4.kind = SIP_STREAM_TCP;
	cfg = v4;
	cfg.family = AF_INET6;
	h6 = sip_stream_listen(&srv, &cfg);
	CHECK(h6 != SIP_HANDLE_NONE);
	h4 = sip_stream_listen(&srv, &v4);
	CHECK(h4 != SIP_HANDLE_NONE);
	CHECK(h4 != h6);
	again = sip_stream_listen(&srv, &cfg);
	CHECK_UINT(again, h6);
	CHECK_UINT(sip_stream_listen_count(&srv), 2);
	l = sip_stream_listener_get(&srv, h6);
	CHECK_UINT(l ? l->refs : 0, 2);
	l = sip_stream_listener_get(&srv, h4);
	CHECK_UINT(l ? l->refs : 0, 1);
	sip_stream_listen_close(&srv, h6);
	CHECK(sip_stream_listener_get(&srv, h6) != NULL);
	sip_stream_listen_close(&srv, h6);
	CHECK(sip_stream_listener_get(&srv, h6) == NULL);
	CHECK(sip_stream_listener_get(&srv, h4) != NULL);
	CHECK_UINT(sip_stream_listen_count(&srv), 1);

	sip_stream_done(&srv);
}

/* --------------------------------------------------------- round trip */

static void t_roundtrip(void)
{
	struct sip_stream_key key, key2, back;
	const char *reg = reg_wire();
	const char *ok = ok_wire();
	struct sip_conn *c;
	uint16_t port = 0;
	uint32_t lh, h;

	lh = pair_open(&port, SIP_STREAM_TCP, NULL);
	CHECK(lh != SIP_HANDLE_NONE);

	CASE("tcp: a request crosses real sockets and arrives parsed");
	key_make(&key, port, 5060, SIP_STREAM_TCP);
	log_reset(&srv_log, 1);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	CHECK(!strcmp(srv_log.method[0], "REGISTER"));
	CHECK_UINT(srv_log.len[0], strlen(reg));
	CHECK_UINT(cli.stat.connects, 1);
	CHECK_UINT(srv.stat.accepts, 1);
	CHECK_UINT(sip_stream_conn_count(&cli), 1);
	CHECK_UINT(sip_stream_conn_count(&srv), 1);

	CASE("tcp: the reported source is the peer tuple, marked connect forbidden");
	{
		char host[64] = "";

		CHECK(sip_dst_host(&srv_log.src[0], host, sizeof(host)));
		CHECK(!strcmp(host, "127.0.0.1"));
		CHECK(sip_dst_port(&srv_log.src[0]) != 0);
		CHECK(sip_dst_port(&srv_log.src[0]) != port);
		CHECK_UINT(srv_log.src[0].flags & SIP_DST_F_NO_CONNECT,
			   SIP_DST_F_NO_CONNECT);
	}

	CASE("tcp: the response travels back on the arriving connection");
	key_from_src(&back, &srv_log.src[0], port, SIP_STREAM_TCP);
	log_reset(&cli_log, 1);
	CHECK_UINT(sip_stream_send(&srv, &back, 0, ok, strlen(ok)), strlen(ok));
	wait_msgs(&cli_log, 1, WAIT_MS);
	CHECK_UINT(cli_log.msgs, 1);
	CHECK_UINT(cli_log.status[0], 200);
	CHECK_UINT(sip_stream_conn_count(&srv), 1);
	CHECK_UINT(srv.stat.connects, 0);

	CASE("tcp: an outbound connection reports a source that is not connect forbidden");
	CHECK_UINT(cli_log.src[0].flags & SIP_DST_F_NO_CONNECT, 0);
	CHECK_UINT(sip_dst_port(&cli_log.src[0]), port);

	CASE("tcp: the same key reuses the connection it opened");
	log_reset(&srv_log, 1);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	CHECK_UINT(cli.stat.connects, 1);
	CHECK_UINT(sip_stream_conn_count(&cli), 1);

	CASE("tcp: two local ports are two connections, never one");
	key_make(&key2, port, 5062, SIP_STREAM_TCP);
	log_reset(&srv_log, 1);
	CHECK_UINT(send_reg(&cli, &key2), strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	CHECK_UINT(cli.stat.connects, 2);
	CHECK_UINT(sip_stream_conn_count(&cli), 2);
	CHECK_UINT(srv.stat.accepts, 2);

	CASE("tcp: the keepalive schedule is on the socket, not only in the header");
	{
		socklen_t vlen;
		int val;

		c = conn_of(&cli, &key, NULL);
		CHECK(c != NULL);
		val = -1;
		vlen = sizeof(val);
		CHECK(c && getsockopt(c->ufd.fd, SOL_SOCKET, SO_KEEPALIVE, &val,
				      &vlen) == 0);
		CHECK(val != 0);
		val = -1;
		vlen = sizeof(val);
		CHECK(c && getsockopt(c->ufd.fd, IPPROTO_TCP, TCP_KEEPIDLE, &val,
				      &vlen) == 0);
		CHECK_UINT(val, SIP_STREAM_KA_IDLE_S);
		val = -1;
		vlen = sizeof(val);
		CHECK(c && getsockopt(c->ufd.fd, IPPROTO_TCP, TCP_KEEPINTVL, &val,
				      &vlen) == 0);
		CHECK_UINT(val, SIP_STREAM_KA_INTVL_S);
		val = -1;
		vlen = sizeof(val);
		CHECK(c && getsockopt(c->ufd.fd, IPPROTO_TCP, TCP_KEEPCNT, &val,
				      &vlen) == 0);
		CHECK_UINT(val, SIP_STREAM_KA_CNT);
	}

	CASE("tcp: a handle resolves its connection and nothing else");
	c = conn_of(&cli, &key, NULL);
	CHECK(c != NULL);
	h = sip_stream_conn_handle(c);
	CHECK(h != SIP_HANDLE_NONE);
	CHECK(sip_stream_conn_get(&cli, h) == c);
	CHECK(sip_stream_conn_get(&cli, 0xdeadbeef) == NULL);
	CHECK(sip_stream_conn_get(&cli, SIP_HANDLE_NONE) == NULL);
	CHECK(sip_stream_conn_get(NULL, h) == NULL);
	CHECK_UINT(sip_stream_conn_handle(NULL), SIP_HANDLE_NONE);
	CHECK_UINT(c->frames, 1);		/* the response it carried */

	CASE("tcp: a closed connection reports SIP_CONN_FREE and frees its handle");
	log_reset(&cli_log, 0);
	sip_stream_conn_close(&cli, h);
	CHECK(sip_stream_conn_get(&cli, h) == NULL);
	CHECK_UINT(sip_stream_conn_count(&cli), 1);
	CHECK_UINT(cli_log.evs, 1);
	CHECK_UINT(cli_log.ev_state[0], SIP_CONN_FREE);
	CHECK_UINT(cli_log.ev_conn[0], h);

	CASE("tcp: the next send to the same key is a reconnect and counted as one");
	log_reset(&srv_log, 1);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	CHECK_UINT(cli.stat.connects, 3);
	CHECK_UINT(cli.stat.reconnects, 1);
	CHECK(sip_stream_conn_handle(conn_of(&cli, &key, NULL)) != h);

	CASE("tcp: close_all empties the pool and reports what it closed");
	CHECK_UINT(sip_stream_close_all(&cli), 2);
	CHECK_UINT(sip_stream_conn_count(&cli), 0);
	CHECK_UINT(sip_stream_close_all(&cli), 0);
	CHECK_UINT(sip_stream_close_all(NULL), 0);

	pair_close();
}

/* ------------------------------------------------------------ the queue */

static void t_queue(void)
{
	struct sip_stream_key key, key2;
	const char *reg = reg_wire();
	char buf[1024];
	struct sip_conn *c;
	uint16_t port = 0;
	int lfd, pfd;

	sip_stream_init(&cli, &cb, &cli_log, NULL);
	log_reset(&cli_log, 0);
	lfd = raw_listen(&port, 8);
	CHECK(lfd >= 0);

	CASE("queue: a send on a connection that is still connecting is staged and succeeds");
	key_make(&key, port, 5060, SIP_STREAM_TCP);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	c = conn_of(&cli, &key, NULL);
	CHECK(c != NULL);
	CHECK_UINT(c ? c->state : 0, SIP_CONN_CONNECTING);
	CHECK_UINT(sip_stream_conn_queued(c), strlen(reg));
	CHECK_UINT(cli.stat.queued, 1);
	CHECK_UINT(cli.stat.tx, 1);
	CHECK_UINT(cli.stat.tx_bytes, strlen(reg));
	CHECK_UINT(cli.stat.connects, 1);
	CHECK_UINT(cli_log.evs, 1);
	CHECK_UINT(cli_log.ev_state[0], SIP_CONN_CONNECTING);

	CASE("queue: a second send while connecting joins the same staging buffer");
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	CHECK_UINT(sip_stream_conn_queued(c), 2 * strlen(reg));
	CHECK_UINT(cli.stat.queued, 2);
	CHECK_UINT(cli.stat.connects, 1);

	CASE("queue: the staged bytes leave in order once the connection is up");
	pfd = raw_accept(lfd, WAIT_MS);
	CHECK(pfd >= 0);
	CHECK(raw_read(pfd, buf, 2 * strlen(reg), WAIT_MS));
	CHECK(!memcmp(buf, reg, strlen(reg)));
	CHECK(!memcmp(buf + strlen(reg), reg, strlen(reg)));
	CHECK_UINT(sip_stream_conn_queued(conn_of(&cli, &key, NULL)), 0);
	CHECK_UINT(conn_of(&cli, &key, NULL)->state, SIP_CONN_UP);

	CASE("queue: a payload past the reserved bound is refused, and opens nothing");
	key_make(&key2, port, 5064, SIP_STREAM_TCP);
	memset(oversize, 'x', sizeof(oversize));
	CHECK(sip_stream_send(&cli, &key2, 0, oversize, MSG_MAX + 1) < 0);
	CHECK(conn_of(&cli, &key2, NULL) == NULL);
	CHECK_UINT(cli.stat.connects, 1);
	CHECK_UINT(cli.stat.tx_err, 1);

	CASE("queue: a payload of exactly the reserved bound fits");
	CHECK_UINT(sip_stream_send(&cli, &key2, 0, oversize, MSG_MAX), MSG_MAX);
	c = conn_of(&cli, &key2, NULL);
	CHECK(c != NULL);
	CHECK_UINT(sip_stream_conn_queued(c), MSG_MAX);
	CHECK_UINT(cli.stat.connects, 2);

	CASE("queue: a full staging buffer refuses the next message and stays intact");
	CHECK(sip_stream_send(&cli, &key2, 0, reg, strlen(reg)) < 0);
	CHECK_UINT(sip_stream_conn_queued(c), MSG_MAX);
	CHECK_UINT(cli.stat.tx_err, 2);
	CHECK_UINT(cli.stat.tx, 3);

	if (pfd >= 0)
		close(pfd);
	close(lfd);
	sip_stream_done(&cli);
}

/* ------------------------------------------------------- dst provenance */

static void t_no_connect(void)
{
	struct sip_stream_key back, direct;
	const char *ok = ok_wire();
	const char *reg = reg_wire();
	struct sip_conn *c;
	uint16_t port = 0;
	uint32_t lh;

	lh = pair_open(&port, SIP_STREAM_TCP, NULL);
	CHECK(lh != SIP_HANDLE_NONE);

	{
		struct sip_stream_key out;

		key_make(&out, port, 5060, SIP_STREAM_TCP);
		log_reset(&srv_log, 1);
		CHECK_UINT(send_reg(&cli, &out), strlen(reg));
	}
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);

	CASE("provenance: a connect-forbidden destination sends on its live connection");
	key_from_src(&back, &srv_log.src[0], port, SIP_STREAM_TCP);
	CHECK_UINT(back.peer.flags & SIP_DST_F_NO_CONNECT, SIP_DST_F_NO_CONNECT);
	log_reset(&cli_log, 1);
	CHECK_UINT(sip_stream_send(&srv, &back, 0, ok, strlen(ok)), strlen(ok));
	wait_msgs(&cli_log, 1, WAIT_MS);
	CHECK_UINT(cli_log.msgs, 1);
	CHECK_UINT(srv.stat.connects, 0);
	CHECK_UINT(sip_stream_conn_count(&srv), 1);

	CASE("provenance: once its connection is gone the send fails at once, with no connect");
	c = conn_of(&srv, &back, NULL);
	CHECK(c != NULL);
	sip_stream_conn_close(&srv, sip_stream_conn_handle(c));
	CHECK(conn_of(&srv, &back, NULL) == NULL);
	CHECK_UINT(sip_stream_conn_count(&srv), 0);
	CHECK(sip_stream_send(&srv, &back, 0, ok, strlen(ok)) < 0);
	CHECK_UINT(srv.stat.connects, 0);
	CHECK_UINT(srv.stat.tx_err, 1);
	CHECK_UINT(sip_stream_conn_count(&srv), 0);

	CASE("provenance: the same destination without the flag is what a connect follows");
	direct = back;
	direct.peer.flags = 0;
	CHECK(sip_stream_send(&srv, &direct, 0, ok, strlen(ok)) >= 0);
	CHECK_UINT(srv.stat.connects, 1);
	CHECK_UINT(sip_stream_close_all(&srv), 1);

	pair_close();
}

/* --------------------------------------------------------- rx reentrancy */

static void t_reentrancy(void)
{
	struct sip_stream_key key;
	const char *reg = reg_wire();
	const char *ok = ok_wire();
	char two[2048];
	uint64_t frames0;
	uint16_t port = 0;
	uint32_t h;
	int lfd, pfd;

	sip_stream_init(&cli, &cb, &cli_log, NULL);
	log_reset(&cli_log, 0);
	lfd = raw_listen(&port, 8);
	CHECK(lfd >= 0);

	key_make(&key, port, 5060, SIP_STREAM_TCP);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	pfd = raw_accept(lfd, WAIT_MS);
	CHECK(pfd >= 0);
	CHECK(wait_up(&cli, &key, NULL, WAIT_MS));
	h = sip_stream_conn_handle(conn_of(&cli, &key, NULL));
	snprintf(two, sizeof(two), "%s%s", ok, ok);

	CASE("reentrancy: two pipelined messages in one read both reach the callback");
	log_reset(&cli_log, 2);
	frames0 = cli.stat.frames;
	CHECK(raw_write(pfd, two, strlen(two)));
	wait_msgs(&cli_log, 2, WAIT_MS);
	CHECK_UINT(cli_log.msgs, 2);
	CHECK_UINT(cli.stat.frames - frames0, 2);
	CHECK_UINT(cli_log.conn[0], h);
	CHECK_UINT(cli_log.conn[1], h);
	CHECK(sip_stream_conn_get(&cli, h) != NULL);

	CASE("reentrancy: a close from inside the first message stops the loop");
	log_reset(&cli_log, 1);
	cli_log.close_on_msg = true;
	frames0 = cli.stat.frames;
	CHECK(raw_write(pfd, two, strlen(two)));
	wait_msgs(&cli_log, 1, 200);
	CHECK_UINT(cli_log.msgs, 1);
	CHECK_UINT(cli.stat.frames - frames0, 1);
	CHECK(sip_stream_conn_get(&cli, h) == NULL);
	CHECK_UINT(sip_stream_conn_count(&cli), 0);

	CASE("reentrancy: the pool keeps working after the reentrant close");
	cli_log.close_on_msg = false;
	if (pfd >= 0)
		close(pfd);
	log_reset(&cli_log, 1);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	pfd = raw_accept(lfd, WAIT_MS);
	CHECK(pfd >= 0);
	CHECK(raw_write(pfd, ok, strlen(ok)));
	wait_msgs(&cli_log, 1, WAIT_MS);
	CHECK_UINT(cli_log.msgs, 1);

	if (pfd >= 0)
		close(pfd);
	close(lfd);
	sip_stream_done(&cli);
}

/* ------------------------------------------------------------ keepalive */

static void t_ping(void)
{
	struct sip_stream_key key;
	const char *reg = reg_wire();
	const char *ok = ok_wire();
	char buf[16];
	char lead[2048];
	uint16_t port = 0;
	int lfd, pfd;

	sip_stream_init(&cli, &cb, &cli_log, NULL);
	log_reset(&cli_log, 0);
	lfd = raw_listen(&port, 8);
	CHECK(lfd >= 0);

	key_make(&key, port, 5060, SIP_STREAM_TCP);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	pfd = raw_accept(lfd, WAIT_MS);
	CHECK(pfd >= 0);
	CHECK(wait_up(&cli, &key, NULL, WAIT_MS));
	CHECK(raw_read(pfd, lead, strlen(reg), WAIT_MS));

	CASE("keepalive: a double CRLF is answered with exactly one CRLF on the connection");
	CHECK(raw_write(pfd, SIP_FRAME_PING, SIP_FRAME_PING_LEN));
	CHECK(raw_read(pfd, buf, SIP_FRAME_PONG_LEN, WAIT_MS));
	CHECK(!memcmp(buf, SIP_FRAME_PONG, SIP_FRAME_PONG_LEN));
	CHECK_UINT(cli.stat.ping, 1);
	CHECK_UINT(cli.stat.pong, 0);
	CHECK_UINT(cli.stat.tx, 1);		/* the pong is not a message */

	CASE("keepalive: a lone CRLF before a message is a pong, and is never answered");
	log_reset(&cli_log, 1);
	snprintf(lead, sizeof(lead), "%s%s", SIP_FRAME_PONG, ok);
	CHECK(raw_write(pfd, lead, strlen(lead)));
	wait_msgs(&cli_log, 1, WAIT_MS);
	CHECK_UINT(cli_log.msgs, 1);
	CHECK_UINT(cli.stat.pong, 1);
	CHECK_UINT(cli.stat.ping, 1);
	CHECK(raw_quiet(pfd, 50));

	CASE("keepalive: a ping split across two reads is one ping, never two pongs");
	CHECK(raw_write(pfd, SIP_FRAME_PONG, SIP_FRAME_PONG_LEN));
	CHECK(raw_quiet(pfd, 100));
	CHECK_UINT(cli.stat.ping, 1);
	CHECK_UINT(cli.stat.pong, 1);
	CHECK(raw_write(pfd, SIP_FRAME_PONG, SIP_FRAME_PONG_LEN));
	CHECK(raw_read(pfd, buf, SIP_FRAME_PONG_LEN, WAIT_MS));
	CHECK(!memcmp(buf, SIP_FRAME_PONG, SIP_FRAME_PONG_LEN));
	CHECK_UINT(cli.stat.ping, 2);
	CHECK_UINT(cli.stat.pong, 1);

	CASE("keepalive: a stream that ends on a held CRLF reports the deferred pong");
	CHECK(raw_write(pfd, SIP_FRAME_PONG, SIP_FRAME_PONG_LEN));
	CHECK(raw_quiet(pfd, 50));
	CHECK_UINT(cli.stat.pong, 1);
	close(pfd);
	pfd = -1;
	wait_conns(&cli, 0, WAIT_MS);
	CHECK_UINT(sip_stream_conn_count(&cli), 0);
	CHECK_UINT(cli.stat.pong, 2);
	CHECK_UINT(cli.stat.rx_err, 0);

	if (pfd >= 0)
		close(pfd);
	close(lfd);
	sip_stream_done(&cli);
}

/* ------------------------------------------------------------- framing */

static void t_framing(void)
{
	struct sip_stream_key key;
	const char *reg = reg_wire();
	const char *ok = ok_wire();
	char lead[512];
	uint64_t errs;
	uint16_t port = 0;
	int lfd, pfd;

	sip_stream_init(&cli, &cb, &cli_log, NULL);
	log_reset(&cli_log, 0);
	lfd = raw_listen(&port, 8);
	CHECK(lfd >= 0);

	CASE("framing: a message with no Content-Length closes the connection");
	key_make(&key, port, 5060, SIP_STREAM_TCP);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	pfd = raw_accept(lfd, WAIT_MS);
	CHECK(pfd >= 0);
	CHECK(wait_up(&cli, &key, NULL, WAIT_MS));
	CHECK(raw_read(pfd, lead, strlen(reg), WAIT_MS));
	errs = cli.stat.frame_errs;
	CHECK(raw_write(pfd, "SIP/2.0 200 OK\r\nCSeq: 1 REGISTER\r\n\r\n", 36));
	wait_conns(&cli, 0, WAIT_MS);
	CHECK_UINT(sip_stream_conn_count(&cli), 0);
	CHECK_UINT(cli.stat.frame_errs - errs, 1);
	close(pfd);

	CASE("framing: a Content-Length that is not a number closes the connection");
	key_make(&key, port, 5061, SIP_STREAM_TCP);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	pfd = raw_accept(lfd, WAIT_MS);
	CHECK(pfd >= 0);
	CHECK(wait_up(&cli, &key, NULL, WAIT_MS));
	CHECK(raw_read(pfd, lead, strlen(reg), WAIT_MS));
	errs = cli.stat.frame_errs;
	CHECK(raw_write(pfd, "SIP/2.0 200 OK\r\nContent-Length: bogus\r\n\r\n", 41));
	wait_conns(&cli, 0, WAIT_MS);
	CHECK_UINT(sip_stream_conn_count(&cli), 0);
	CHECK_UINT(cli.stat.frame_errs - errs, 1);
	close(pfd);

	CASE("framing: a body larger than the receive slab closes the connection");
	key_make(&key, port, 5062, SIP_STREAM_TCP);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	pfd = raw_accept(lfd, WAIT_MS);
	CHECK(pfd >= 0);
	CHECK(wait_up(&cli, &key, NULL, WAIT_MS));
	CHECK(raw_read(pfd, lead, strlen(reg), WAIT_MS));
	errs = cli.stat.frame_errs;
	CHECK(raw_write(pfd, "SIP/2.0 200 OK\r\nContent-Length: 999999\r\n\r\n", 42));
	wait_conns(&cli, 0, WAIT_MS);
	CHECK_UINT(sip_stream_conn_count(&cli), 0);
	CHECK_UINT(cli.stat.frame_errs - errs, 1);
	close(pfd);

	CASE("framing: a slice the parser refuses is counted and the connection stays up");
	key_make(&key, port, 5063, SIP_STREAM_TCP);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	pfd = raw_accept(lfd, WAIT_MS);
	CHECK(pfd >= 0);
	CHECK(wait_up(&cli, &key, NULL, WAIT_MS));
	CHECK(raw_read(pfd, lead, strlen(reg), WAIT_MS));
	errs = cli.stat.frame_errs;
	log_reset(&cli_log, 1);
	snprintf(lead, sizeof(lead), "@@@ not a start line\r\nContent-Length: 0\r\n\r\n%s", ok);
	CHECK(raw_write(pfd, lead, strlen(lead)));
	wait_msgs(&cli_log, 1, WAIT_MS);
	CHECK_UINT(cli_log.msgs, 1);
	CHECK_UINT(cli.stat.parse_err, 1);
	CHECK_UINT(cli.stat.frame_errs - errs, 0);
	CHECK_UINT(sip_stream_conn_count(&cli), 1);

	if (pfd >= 0)
		close(pfd);
	close(lfd);
	sip_stream_done(&cli);
}

/* ------------------------------------------------------------ idle close */

static void t_idle(void)
{
	struct sip_stream_key key, next;
	const char *reg = reg_wire();
	struct sip_conn *in, *out;
	uint32_t hin, hout;
	uint16_t port = 0;
	uint32_t lh;

	lh = pair_open(&port, SIP_STREAM_TCP, NULL);
	CHECK(lh != SIP_HANDLE_NONE);

	key_make(&key, port, 5060, SIP_STREAM_TCP);
	log_reset(&srv_log, 1);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);

	in = conn_inbound(&srv);
	out = conn_of(&cli, &key, NULL);
	CHECK(in != NULL);
	CHECK(out != NULL);
	hin = sip_stream_conn_handle(in);
	hout = sip_stream_conn_handle(out);

	CASE("idle: an inbound connection inside the bound is left alone");
	in->used_ms = mono_ms() - SIP_STREAM_IDLE_MS + 5000;
	srv.tick.cb(&srv.tick);
	CHECK(sip_stream_conn_get(&srv, hin) != NULL);
	CHECK_UINT(sip_stream_conn_count(&srv), 1);

	CASE("idle: an OUTBOUND connection is never idle closed, however old it is");
	out->used_ms = mono_ms() - 4 * SIP_STREAM_IDLE_MS;
	cli.tick.cb(&cli.tick);
	CHECK(sip_stream_conn_get(&cli, hout) != NULL);
	CHECK_UINT(sip_stream_conn_count(&cli), 1);

	CASE("idle: an inbound connection past the bound is closed by the sweep");
	log_reset(&srv_log, 0);
	in->used_ms = mono_ms() - SIP_STREAM_IDLE_MS - 1;
	srv.tick.cb(&srv.tick);
	CHECK(sip_stream_conn_get(&srv, hin) == NULL);
	CHECK_UINT(sip_stream_conn_count(&srv), 0);
	CHECK_UINT(srv_log.evs, 1);
	CHECK_UINT(srv_log.ev_state[0], SIP_CONN_FREE);
	CHECK_UINT(srv_log.ev_conn[0], hin);

	/*
	 * A second local port, because the client keeps the connection whose
	 * accepted twin the sweep just closed: only a new key opens a new one.
	 */
	CASE("idle: a fresh inbound connection starts its clock at the accept");
	key_make(&next, port, 5061, SIP_STREAM_TCP);
	log_reset(&srv_log, 1);
	CHECK_UINT(send_reg(&cli, &next), strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	in = conn_inbound(&srv);
	CHECK(in != NULL);
	CHECK(in && in->inbound);
	CHECK(in && mono_ms() - in->used_ms < SIP_STREAM_IDLE_MS);
	srv.tick.cb(&srv.tick);
	CHECK_UINT(sip_stream_conn_count(&srv), 1);

	/*
	 * A keepalive is not a message, so only the receive path's own clock
	 * refresh can save the connection that carries nothing else.
	 */
	CASE("idle: a keepalive on an inbound connection renews its clock");
	in = conn_inbound(&srv);
	CHECK(in != NULL);
	if (in)
		in->used_ms = mono_ms() - SIP_STREAM_IDLE_MS - 1;
	CHECK_UINT(sip_stream_send(&cli, &next, 0, SIP_FRAME_PING, SIP_FRAME_PING_LEN),
		   SIP_FRAME_PING_LEN);
	loop_pump(100);
	CHECK_UINT(srv.stat.ping, 1);
	srv.tick.cb(&srv.tick);
	CHECK_UINT(sip_stream_conn_count(&srv), 1);
	CHECK(conn_inbound(&srv) != NULL);

	CASE("connect bound: an overdue connect is closed by the sweep and starts a backoff");
	{
		struct sip_stream_key dead;
		struct sip_conn *c;

		key_make(&dead, free_port(), 5070, SIP_STREAM_TCP);
		CHECK(send_reg(&cli, &dead) > 0);
		c = conn_of(&cli, &dead, NULL);
		CHECK(c != NULL);
		CHECK_UINT(c ? c->state : 0, SIP_CONN_CONNECTING);
		CHECK(c && c->deadline_ms != 0);
		if (c)
			c->deadline_ms = mono_ms();
		cli.tick.cb(&cli.tick);
		CHECK(conn_of(&cli, &dead, NULL) == NULL);
		c = slot_of(&cli, &dead);
		CHECK(c != NULL);
		CHECK_UINT(c ? c->backoff_ms : 0, SIP_STREAM_BACKOFF_MS);
	}

	pair_close();
}

/* -------------------------------------------------------------- eviction */

static void t_lru(void)
{
	struct sip_stream_key key[SIP_STREAM_CONN_MAX + 2];
	const char *reg = reg_wire();
	struct sip_conn *c;
	uint64_t base;
	uint16_t port = 0;
	unsigned int i;
	int lfd;

	sip_stream_init(&cli, &cb, &cli_log, NULL);
	log_reset(&cli_log, 0);
	lfd = raw_listen(&port, 64);
	CHECK(lfd >= 0);

	CASE("lru: the pool takes exactly SIP_STREAM_CONN_MAX connections");
	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		key_make(&key[i], port, (uint16_t)(5060 + i), SIP_STREAM_TCP);
		CHECK_UINT(send_reg(&cli, &key[i]), strlen(reg));
	}
	CHECK_UINT(sip_stream_conn_count(&cli), SIP_STREAM_CONN_MAX);
	CHECK_UINT(cli.stat.connects, SIP_STREAM_CONN_MAX);

	CASE("lru: with queued bytes everywhere the next connect is denied, not an eviction");
	key_make(&key[SIP_STREAM_CONN_MAX], port, 5099, SIP_STREAM_TCP);
	CHECK(send_reg(&cli, &key[SIP_STREAM_CONN_MAX]) < 0);
	CHECK_UINT(cli.stat.denied, 1);
	CHECK_UINT(cli.stat.evictions, 0);
	CHECK_UINT(cli.stat.tx_err, 1);
	CHECK_UINT(sip_stream_conn_count(&cli), SIP_STREAM_CONN_MAX);
	for (i = 0; i < SIP_STREAM_CONN_MAX; i++)
		CHECK(conn_of(&cli, &key[i], NULL) != NULL);

	CASE("lru: once the queues drain the next connect evicts the least recently used");
	CHECK(wait_drained(&cli, WAIT_MS));
	base = mono_ms();
	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		c = conn_of(&cli, &key[i], NULL);
		CHECK(c != NULL);
		if (c)
			c->used_ms = base + i;
	}
	log_reset(&cli_log, 0);
	CHECK_UINT(send_reg(&cli, &key[SIP_STREAM_CONN_MAX]), strlen(reg));
	CHECK_UINT(cli.stat.evictions, 1);
	CHECK_UINT(cli.stat.denied, 1);
	CHECK(conn_of(&cli, &key[0], NULL) == NULL);
	CHECK(conn_of(&cli, &key[1], NULL) != NULL);
	CHECK(conn_of(&cli, &key[SIP_STREAM_CONN_MAX], NULL) != NULL);
	CHECK_UINT(sip_stream_conn_count(&cli), SIP_STREAM_CONN_MAX);
	CHECK_UINT(cli_log.evs, 2);
	CHECK_UINT(cli_log.ev_state[0], SIP_CONN_FREE);
	CHECK_UINT(cli_log.ev_state[1], SIP_CONN_CONNECTING);

	CASE("lru: a connection with queued bytes is never the victim, however old it is");
	CHECK(wait_drained(&cli, WAIT_MS));
	c = conn_of(&cli, &key[SIP_STREAM_CONN_MAX], NULL);
	CHECK(c != NULL);
	sip_stream_conn_close(&cli, sip_stream_conn_handle(c));
	CHECK_UINT(sip_stream_conn_count(&cli), SIP_STREAM_CONN_MAX - 1);

	/* the oldest connection is the one that still holds staged bytes */
	key_make(&key[SIP_STREAM_CONN_MAX + 1], port, 5098, SIP_STREAM_TCP);
	CHECK_UINT(send_reg(&cli, &key[SIP_STREAM_CONN_MAX + 1]), strlen(reg));
	c = conn_of(&cli, &key[SIP_STREAM_CONN_MAX + 1], NULL);
	CHECK(c != NULL);
	CHECK(c && sip_stream_conn_queued(c) > 0);
	if (c)
		c->used_ms = base - 1000;
	base = mono_ms();
	for (i = 1; i < SIP_STREAM_CONN_MAX; i++) {
		struct sip_conn *o = conn_of(&cli, &key[i], NULL);

		if (o)
			o->used_ms = base + i;
	}
	CHECK_UINT(send_reg(&cli, &key[SIP_STREAM_CONN_MAX]), strlen(reg));
	CHECK_UINT(cli.stat.evictions, 2);
	CHECK(conn_of(&cli, &key[SIP_STREAM_CONN_MAX + 1], NULL) != NULL);
	CHECK(conn_of(&cli, &key[1], NULL) == NULL);
	CHECK_UINT(sip_stream_conn_count(&cli), SIP_STREAM_CONN_MAX);

	close(lfd);
	sip_stream_done(&cli);
}

/* ------------------------------------------------------- a refused accept */

/*
 * The pool is full, and every connection holds transmit bytes that cannot
 * leave. The LRU rule therefore has no victim to offer. The inbound connection
 * is refused outright, its socket is closed, and the peer reads the end of it.
 */
static void t_accept_denied(void)
{
	struct sip_stream_key key[SIP_STREAM_CONN_MAX];
	struct sip_stream_listen_cfg lcfg;
	struct sockaddr_in sa;
	struct sip_conn *c;
	uint16_t port = 0;
	unsigned int i;
	uint32_t lh;
	int lfd, cfd;

	sip_stream_init(&cli, &cb, &cli_log, NULL);
	log_reset(&cli_log, 0);
	memset(oversize, 'x', sizeof(oversize));
	lfd = raw_listen(&port, 64);
	CHECK(lfd >= 0);
	sock_buf_shrink(lfd);

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		key_make(&key[i], port, (uint16_t)(5060 + i), SIP_STREAM_TCP);
		CHECK(sip_stream_send(&cli, &key[i], 0, oversize, MSG_MAX) > 0);
		c = conn_of(&cli, &key[i], NULL);
		CHECK(c != NULL);
		if (c)
			sock_buf_shrink(c->ufd.fd);
	}
	CHECK_UINT(sip_stream_conn_count(&cli), SIP_STREAM_CONN_MAX);

	memset(&lcfg, 0, sizeof(lcfg));
	lcfg.addr = "127.0.0.1";
	lcfg.port = free_port();
	lcfg.kind = SIP_STREAM_TCP;
	lh = sip_stream_listen(&cli, &lcfg);
	CHECK(lh != SIP_HANDLE_NONE);

	CASE("accept: a full pool with nothing evictable refuses the connection");
	cfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	CHECK(cfd >= 0);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = htons(lcfg.port);
	CHECK(connect(cfd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
	loop_pump(100);

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++)
		CHECK(sip_stream_conn_queued(conn_of(&cli, &key[i], NULL)) > 0);

	CHECK_UINT(cli.stat.denied, 1);
	CHECK_UINT(cli.stat.accepts, 0);
	CHECK_UINT(cli.stat.evictions, 0);
	CHECK_UINT(sip_stream_conn_count(&cli), SIP_STREAM_CONN_MAX);
	CHECK(wait_eof(cfd, WAIT_MS));

	close(cfd);
	close(lfd);
	sip_stream_done(&cli);
}

/* --------------------------------------------------------------- backoff */

static void t_backoff(void)
{
	static const uint32_t series[] = { 1000, 2000, 4000, 8000, 16000, 32000, 32000 };
	struct sip_stream_key key, live;
	const char *reg = reg_wire();
	struct sip_conn *slot;
	uint64_t errs;
	uint16_t port, closed;
	unsigned int i;
	int lfd;

	sip_stream_init(&cli, &cb, &cli_log, NULL);
	log_reset(&cli_log, 0);
	closed = free_port();
	CHECK(closed != 0);

	CASE("backoff: a refused connect drops the connection and starts the series at T1");
	key_make(&key, closed, 5060, SIP_STREAM_TCP);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	CHECK_UINT(cli.stat.connects, 1);
	wait_conns(&cli, 0, WAIT_MS);
	CHECK_UINT(sip_stream_conn_count(&cli), 0);
	slot = slot_of(&cli, &key);
	CHECK(slot != NULL);
	CHECK_UINT(slot ? slot->backoff_ms : 0, SIP_STREAM_BACKOFF_MS);
	CHECK(slot && slot->backoff_until != 0);
	CHECK_UINT(cli.stat.handshakes_failed, 0);

	CASE("backoff: a send inside the backoff fails at once and starts no new connect");
	errs = cli.stat.tx_err;
	CHECK(send_reg(&cli, &key) < 0);
	CHECK_UINT(cli.stat.connects, 1);
	CHECK_UINT(cli.stat.tx_err, errs + 1);
	CHECK_UINT(sip_stream_conn_count(&cli), 0);
	CHECK(send_reg(&cli, &key) < 0);
	CHECK_UINT(cli.stat.connects, 1);

	CASE("backoff: the interval doubles on every failure and caps at 64*T1");
	for (i = 0; i < sizeof(series) / sizeof(series[0]); i++) {
		slot->backoff_until = 0;
		CHECK(send_reg(&cli, &key) > 0);
		wait_conns(&cli, 0, WAIT_MS);
		CHECK_UINT(slot->backoff_ms, series[i]);
	}
	CHECK_UINT(cli.stat.connects, 1 + sizeof(series) / sizeof(series[0]));
	CHECK_UINT(slot->backoff_ms, SIP_STREAM_BACKOFF_MAX_MS);

	CASE("backoff: the sweep clears an expired backoff and the next send connects again");
	slot->backoff_until = mono_ms();
	cli.tick.cb(&cli.tick);
	CHECK_UINT(slot->backoff_until, 0);
	CHECK(send_reg(&cli, &key) > 0);
	CHECK_UINT(cli.stat.connects, 2 + sizeof(series) / sizeof(series[0]));
	wait_conns(&cli, 0, WAIT_MS);

	/*
	 * The series a slot earned survives its close for the same destination,
	 * so only a connection that actually comes up can end it.
	 */
	CASE("backoff: a connection that comes up clears the series its slot carried");
	lfd = raw_listen(&port, 8);
	CHECK(lfd >= 0);
	key_make(&live, port, 5060, SIP_STREAM_TCP);
	CHECK(send_reg(&cli, &live) > 0);
	CHECK(wait_up(&cli, &live, NULL, WAIT_MS));
	slot = conn_of(&cli, &live, NULL);
	CHECK(slot != NULL);
	if (slot) {
		slot->backoff_ms = 8000;
		sip_stream_conn_close(&cli, sip_stream_conn_handle(slot));
	}
	slot = slot_of(&cli, &live);
	CHECK(slot != NULL);
	CHECK_UINT(slot ? slot->backoff_ms : 0, 8000);
	if (slot)
		slot->backoff_until = 0;
	CHECK(send_reg(&cli, &live) > 0);
	CHECK_UINT(slot ? slot->backoff_ms : 0, 8000);
	CHECK(wait_up(&cli, &live, NULL, WAIT_MS));
	CHECK_UINT(slot ? slot->backoff_ms : 99, 0);
	CHECK_UINT(slot ? slot->backoff_until : 99, 0);
	close(lfd);

	sip_stream_done(&cli);
}

/* ------------------------------------------------------------------- tos */

static void t_tos(void)
{
	struct sip_stream_key key;
	const char *reg = reg_wire();
	struct sip_conn *c;
	uint16_t port = 0;
	int lfd, pfd;

	sip_stream_init(&cli, &cb, &cli_log, NULL);
	log_reset(&cli_log, 0);
	lfd = raw_listen(&port, 8);
	CHECK(lfd >= 0);

	CASE("tos: the byte reaches the socket before the message leaves");
	key_make(&key, port, 5060, SIP_STREAM_TCP);
	CHECK_UINT(sip_stream_send(&cli, &key, 0x60, reg, strlen(reg)), strlen(reg));
	c = conn_of(&cli, &key, NULL);
	CHECK(c != NULL);
	CHECK_UINT(c ? c->tos_cur : 0, 0x60);
	CHECK_UINT(tos_read(c), 0x60);
	pfd = raw_accept(lfd, WAIT_MS);
	CHECK(pfd >= 0);
	CHECK(wait_up(&cli, &key, NULL, WAIT_MS));

	CASE("tos: a second send with the same byte leaves the socket alone");
	CHECK_UINT(sip_stream_send(&cli, &key, 0x60, reg, strlen(reg)), strlen(reg));
	CHECK_UINT(c->tos_cur, 0x60);
	CHECK_UINT(tos_read(c), 0x60);
	CHECK_UINT(cli.stat.tos_err, 0);

	CASE("tos: two accounts on one connection each get their own byte per send");
	CHECK_UINT(sip_stream_send(&cli, &key, 0xb8, reg, strlen(reg)), strlen(reg));
	CHECK_UINT(tos_read(c), 0xb8);
	CHECK_UINT(c->tos_cur, 0xb8);
	CHECK_UINT(sip_stream_send(&cli, &key, 0x60, reg, strlen(reg)), strlen(reg));
	CHECK_UINT(tos_read(c), 0x60);
	CHECK_UINT(c->tos_cur, 0x60);

	CASE("tos: a reconnect starts again from no byte at all");
	sip_stream_conn_close(&cli, sip_stream_conn_handle(c));
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	c = conn_of(&cli, &key, NULL);
	CHECK(c != NULL);
	CHECK_UINT(c ? c->tos_cur : 1, 0);

	if (pfd >= 0)
		close(pfd);
	close(lfd);
	sip_stream_done(&cli);
}

/* ------------------------------------------------------ backpressure rules */

/*
 * The reserve and the TOS flush both need a connection whose transmit buffer
 * stops emptying. The cases use a small socket pipe both ways, and a peer that
 * reads only when the case tells it to, never through the loop.
 */
static void t_backpressure(void)
{
	struct sip_stream_key key;
	struct sip_conn *c;
	uint64_t errs;
	uint16_t port = 0;
	unsigned int i;
	int lfd, pfd;

	sip_stream_init(&cli, &cb, &cli_log, NULL);
	log_reset(&cli_log, 0);
	memset(oversize, 'x', sizeof(oversize));
	lfd = raw_listen(&port, 8);
	CHECK(lfd >= 0);
	sock_buf_shrink(lfd);

	key_make(&key, port, 5060, SIP_STREAM_TCP);
	CHECK(sip_stream_send(&cli, &key, 0x60, oversize, 4096) > 0);
	c = conn_of(&cli, &key, NULL);
	CHECK(c != NULL);
	if (c)
		sock_buf_shrink(c->ufd.fd);
	pfd = raw_accept(lfd, WAIT_MS);
	CHECK(pfd >= 0);
	CHECK(wait_up(&cli, &key, NULL, WAIT_MS));

	CASE("backpressure: a socket that stops taking bytes leaves them staged");
	for (i = 0; i < 64 && !sip_stream_conn_queued(c); i++)
		CHECK(sip_stream_send(&cli, &key, 0x60, oversize, 4096) > 0);
	CHECK(i < 64);
	CHECK(sip_stream_conn_queued(c) > 0);

	CASE("reserve: the transmit buffer fills to the bound and keeps the pong's room");
	CHECK_UINT(tx_fill(&cli, &key, 0x60), MSG_MAX);

	/*
	 * The case writes the ping first and the last top up after it, so no
	 * drain runs between the moment the buffer reaches its bound and the
	 * moment the receive path answers. The engine reads before it writes on
	 * a readable event, which is exactly the moment the reserve is the only
	 * room a pong can have.
	 */
	CASE("reserve: a ping on a buffer full to the bound is still answered");
	CHECK(raw_write(pfd, SIP_FRAME_PING, SIP_FRAME_PING_LEN));
	CHECK_UINT(tx_fill(&cli, &key, 0x60), MSG_MAX);
	errs = cli.stat.tx_err;		/* the fill's own refusal is not the pong's */
	loop_pump(100);
	CHECK_UINT(cli.stat.ping, 1);
	CHECK_UINT(cli.stat.tx_err, errs);

	/*
	 * The case drains the peer with no loop turn, so the socket has room
	 * again and the engine still holds every staged byte: the only thing
	 * that can move them now is the next send, and only when its byte
	 * differs.
	 */
	CASE("tos: a refused payload leaves the socket marked for what is in flight");
	CHECK_UINT(tx_fill(&cli, &key, 0x60), MSG_MAX);
	CHECK_UINT(tos_read(c), 0x60);
	CHECK(sip_stream_send(&cli, &key, 0x28, oversize, 4096) < 0);
	CHECK_UINT(tos_read(c), 0x60);
	CHECK_UINT(c->tos_cur, 0x60);

	CASE("tos: a new byte flushes what an older byte staged, the same byte does not");
	CHECK_UINT(tx_fill(&cli, &key, 0x60), MSG_MAX);
	CHECK(raw_sink(pfd) > 0);
	CHECK(sip_stream_send(&cli, &key, 0x60, "x", 1) < 0);
	CHECK_UINT(sip_stream_conn_queued(c), MSG_MAX);
	CHECK_UINT(tos_read(c), 0x60);
	CHECK_UINT(sip_stream_send(&cli, &key, 0xb8, "x", 1), 1);
	CHECK(sip_stream_conn_queued(c) < MSG_MAX);
	CHECK_UINT(tos_read(c), 0xb8);

	if (pfd >= 0)
		close(pfd);
	close(lfd);
	sip_stream_done(&cli);
}

/* ------------------------------------------------------------------- tls */

static void t_tls(void)
{
	struct sip_tls_cfg tcfg;
	struct sip_stream_key key, back;
	const char *reg = reg_wire();
	const char *ok = ok_wire();
	struct sip_conn *c;
	uint16_t port = 0;
	bool fixtures_ok;
	uint32_t lh;

	fixtures_ok = fixtures_take();
	SUITE_CHECK(fixtures_ok);
	if (!fixtures_ok) {
		printf("    set UVOICE_TLS_FIXTURES to test/unit/tls\n");
		return;
	}

	memset(&tcfg, 0, sizeof(tcfg));
	tcfg.cert_path = fix_cert;
	tcfg.key_path = fix_key;
	sip_tls_init(&tls);
	SUITE_CHECK(sip_tls_ctx_load(&tls, &tcfg));
	SUITE_CHECK(sip_tls_client_ready(&tls));
	SUITE_CHECK(sip_tls_server_ready(&tls));

	lh = pair_open(&port, SIP_STREAM_TLS, &tls);
	SUITE_CHECK(lh != SIP_HANDLE_NONE);

	CASE("tls: a handshake over real sockets brings the connection up and carries a message");
	key_make(&key, port, 5060, SIP_STREAM_TLS);
	log_reset(&cli_log, 0);
	log_reset(&srv_log, 1);
	CHECK_UINT(sip_stream_send_sni(&cli, &key, 0, reg, strlen(reg), "sip.example.test"),
		   strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	CHECK(!strcmp(srv_log.method[0], "REGISTER"));
	c = conn_of(&cli, &key, "sip.example.test");
	CHECK(c != NULL);
	CHECK_UINT(c ? c->state : 0, SIP_CONN_UP);
	CHECK(c && sip_tls_conn_up(&c->tls));
	CHECK_UINT(cli.stat.handshakes_failed, 0);
	CHECK_UINT(srv.stat.handshakes_failed, 0);
	CHECK_UINT(srv.stat.accepts, 1);

	CASE("tls: the connection reports connecting, handshake and up in that order");
	CHECK_UINT(cli_log.evs, 3);
	CHECK_UINT(cli_log.ev_state[0], SIP_CONN_CONNECTING);
	CHECK_UINT(cli_log.ev_state[1], SIP_CONN_HANDSHAKE);
	CHECK_UINT(cli_log.ev_state[2], SIP_CONN_UP);
	CHECK_UINT(srv_log.evs, 2);
	CHECK_UINT(srv_log.ev_state[0], SIP_CONN_HANDSHAKE);
	CHECK_UINT(srv_log.ev_state[1], SIP_CONN_UP);

	CASE("tls: two server names on one address and port are two connections");
	CHECK(conn_of(&cli, &key, "other.example.test") == NULL);
	CHECK(conn_of(&cli, &key, NULL) == NULL);
	log_reset(&srv_log, 1);
	CHECK_UINT(sip_stream_send_sni(&cli, &key, 0, reg, strlen(reg), "other.example.test"),
		   strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	CHECK_UINT(sip_stream_conn_count(&cli), 2);
	CHECK_UINT(cli.stat.connects, 2);
	CHECK(conn_of(&cli, &key, "sip.example.test") != NULL);
	CHECK(conn_of(&cli, &key, "other.example.test") != NULL);

	CASE("tls: the same name reuses the connection it opened");
	log_reset(&srv_log, 1);
	CHECK_UINT(sip_stream_send_sni(&cli, &key, 0, reg, strlen(reg), "sip.example.test"),
		   strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	CHECK_UINT(sip_stream_conn_count(&cli), 2);
	CHECK_UINT(cli.stat.connects, 2);

	CASE("tls: the response travels back through the record layer of the accepted leg");
	key_from_src(&back, &srv_log.src[0], port, SIP_STREAM_TLS);
	CHECK_UINT(back.peer.flags & SIP_DST_F_NO_CONNECT, SIP_DST_F_NO_CONNECT);
	log_reset(&cli_log, 1);
	CHECK_UINT(sip_stream_send(&srv, &back, 0, ok, strlen(ok)), strlen(ok));
	wait_msgs(&cli_log, 1, WAIT_MS);
	CHECK_UINT(cli_log.msgs, 1);
	CHECK_UINT(cli_log.status[0], 200);
	CHECK_UINT(srv.stat.connects, 0);

	/*
	 * The account that answers carries its own server_name, and the
	 * destination it answers to is an accepted connection, which holds
	 * none. The name must stay out of that match, or every response over
	 * an inbound TLS leg would open a connection to an ephemeral port
	 * instead (sip/stream.h "Dst provenance").
	 */
	CASE("tls: a named send to the connect-forbidden destination uses the arriving leg");
	log_reset(&cli_log, 1);
	CHECK_UINT(sip_stream_send_sni(&srv, &back, 0, ok, strlen(ok), "sip.example.test"),
		   strlen(ok));
	wait_msgs(&cli_log, 1, WAIT_MS);
	CHECK_UINT(cli_log.msgs, 1);
	CHECK_UINT(cli_log.status[0], 200);
	CHECK_UINT(srv.stat.connects, 0);
	CHECK_UINT(srv.stat.tx_err, 0);

	CASE("tls: a ping is answered inside the record layer, and the pong is counted");
	CHECK_UINT(sip_stream_send(&srv, &back, 0, SIP_FRAME_PING, SIP_FRAME_PING_LEN),
		   SIP_FRAME_PING_LEN);
	loop_pump(100);
	CHECK_UINT(cli.stat.ping, 1);
	log_reset(&srv_log, 1);
	CHECK_UINT(sip_stream_send_sni(&cli, &key, 0, reg, strlen(reg), "sip.example.test"),
		   strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	CHECK_UINT(srv.stat.pong, 1);
	CHECK_UINT(srv.stat.ping, 0);

	/*
	 * The reuse split above proves the engine keeps two connections apart by
	 * name; only the server's own view proves the name actually left in the
	 * ClientHello. struct sip_tls_conn is public and holds the SSL object,
	 * so the library is the oracle, exactly as test_sip_tls.c uses it.
	 */
	CASE("tls: the accepted leg saw the name the client sent");
	{
		const char *seen;

		c = conn_of(&srv, &back, NULL);
		CHECK(c != NULL);
		CHECK(c && c->tls.ssl != NULL);
		seen = c && c->tls.ssl ?
		       SSL_get_servername(c->tls.ssl, TLSEXT_NAMETYPE_host_name) : NULL;
		CHECK(seen != NULL);
		CHECK(seen && !strcmp(seen, "sip.example.test"));
	}

	/*
	 * Two SSL_write calls with no loop turn between them are two TLS records
	 * in one socket flush. One read of the socket pulls both into OpenSSL and
	 * one SSL_read returns only the first, so the second lives where epoll
	 * cannot see it: nothing else on this connection speaks afterwards, and
	 * the receive loop has to be what collects it.
	 */
	CASE("tls: two records the peer flushed together both reach the callback");
	log_reset(&cli_log, 2);
	CHECK_UINT(sip_stream_send(&srv, &back, 0, ok, strlen(ok)), strlen(ok));
	CHECK_UINT(sip_stream_send(&srv, &back, 0, ok, strlen(ok)), strlen(ok));
	wait_msgs(&cli_log, 2, WAIT_MS);
	CHECK_UINT(cli_log.msgs, 2);
	CHECK_UINT(cli_log.status[1], 200);

	CASE("tls: a target with no name sends no server_name at all");
	{
		struct sip_stream_key anon, anon_back;

		key_make(&anon, port, 5063, SIP_STREAM_TLS);
		log_reset(&srv_log, 1);
		CHECK_UINT(sip_stream_send_sni(&cli, &anon, 0, reg, strlen(reg), NULL),
			   strlen(reg));
		wait_msgs(&srv_log, 1, WAIT_MS);
		CHECK_UINT(srv_log.msgs, 1);
		key_from_src(&anon_back, &srv_log.src[0], port, SIP_STREAM_TLS);
		c = conn_of(&srv, &anon_back, NULL);
		CHECK(c != NULL);
		CHECK(c && c->tls.ssl != NULL);
		CHECK(c && c->tls.ssl &&
		      SSL_get_servername(c->tls.ssl, TLSEXT_NAMETYPE_host_name) == NULL);
	}

	CASE("tls: a reconnect after a close handshakes again");
	c = conn_of(&cli, &key, "sip.example.test");
	CHECK(c != NULL);
	sip_stream_conn_close(&cli, sip_stream_conn_handle(c));
	CHECK(conn_of(&cli, &key, "sip.example.test") == NULL);
	log_reset(&cli_log, 0);
	log_reset(&srv_log, 1);
	CHECK_UINT(sip_stream_send_sni(&cli, &key, 0, reg, strlen(reg), "sip.example.test"),
		   strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	CHECK_UINT(cli.stat.connects, 4);
	CHECK_UINT(cli.stat.reconnects, 1);
	CHECK_UINT(cli.stat.handshakes_failed, 0);
	c = conn_of(&cli, &key, "sip.example.test");
	CHECK(c && sip_tls_conn_up(&c->tls));

	CASE("tls: a peer that closes mid handshake counts a failure and starts a backoff");
	{
		struct sip_stream_key bad;
		uint16_t raw_port = 0;
		int lfd, pfd;

		lfd = raw_listen(&raw_port, 4);
		CHECK(lfd >= 0);
		key_make(&bad, raw_port, 5070, SIP_STREAM_TLS);
		CHECK(sip_stream_send_sni(&cli, &bad, 0, reg, strlen(reg), NULL) > 0);
		pfd = raw_accept(lfd, WAIT_MS);
		CHECK(pfd >= 0);
		if (pfd >= 0)
			close(pfd);
		wait_conns(&cli, 2, WAIT_MS);
		CHECK(conn_of(&cli, &bad, NULL) == NULL);
		CHECK_UINT(cli.stat.handshakes_failed, 1);
		c = slot_of(&cli, &bad);
		CHECK(c != NULL);
		CHECK_UINT(c ? c->backoff_ms : 0, SIP_STREAM_BACKOFF_MS);
		close(lfd);
	}

	/*
	 * SIP_STREAM_CONNECT_MS bounds a connect and its handshake together, so
	 * the budget a slow connect already spent must still be spent when the
	 * handshake starts. The case drives the sweep by hand so the pushed
	 * deadline cannot expire inside a pump, and the peer is a plain TCP
	 * socket that never answers the ClientHello.
	 */
	CASE("tls: the handshake keeps the connect budget and never opens a second one");
	{
		struct sip_stream_key slow;
		uint64_t failed0 = cli.stat.handshakes_failed;
		uint64_t bound;
		uint16_t raw_port = 0;
		int lfd, pfd;

		lfd = raw_listen(&raw_port, 4);
		CHECK(lfd >= 0);
		key_make(&slow, raw_port, 5072, SIP_STREAM_TLS);
		CHECK(sip_stream_send_sni(&cli, &slow, 0, reg, strlen(reg), NULL) > 0);
		c = conn_of(&cli, &slow, NULL);
		CHECK(c != NULL);
		CHECK_UINT(c ? c->state : 99, SIP_CONN_CONNECTING);
		uloop_timeout_cancel(&cli.tick);
		bound = mono_ms();
		if (c)
			c->deadline_ms = bound;

		pfd = raw_accept(lfd, WAIT_MS);
		CHECK(pfd >= 0);
		wait_state(&cli, &slow, NULL, SIP_CONN_HANDSHAKE, WAIT_MS);
		c = conn_of(&cli, &slow, NULL);
		CHECK(c != NULL);
		CHECK_UINT(c ? c->state : 99, SIP_CONN_HANDSHAKE);
		CHECK_UINT(c ? c->deadline_ms : 0, bound);

		cli.tick.cb(&cli.tick);
		CHECK(conn_of(&cli, &slow, NULL) == NULL);
		CHECK_UINT(cli.stat.handshakes_failed, failed0 + 1);
		if (pfd >= 0)
			close(pfd);
		close(lfd);
	}

	/*
	 * A backoff belongs to one destination, and over TLS two names at one
	 * address and port are two destinations: name B has never failed and
	 * must not inherit name A's series.
	 */
	CASE("tls: a backoff belongs to one name, and another name at that address connects");
	{
		struct sip_stream_key split;
		uint64_t connects0 = cli.stat.connects;
		uint16_t raw_port = 0;
		int lfd, pfd;

		lfd = raw_listen(&raw_port, 8);
		CHECK(lfd >= 0);
		key_make(&split, raw_port, 5073, SIP_STREAM_TLS);
		CHECK(sip_stream_send_sni(&cli, &split, 0, reg, strlen(reg),
					  "a.example.test") > 0);
		pfd = raw_accept(lfd, WAIT_MS);
		CHECK(pfd >= 0);
		if (pfd >= 0)
			close(pfd);
		wait_absent(&cli, &split, "a.example.test", WAIT_MS);
		CHECK(conn_of(&cli, &split, "a.example.test") == NULL);
		CHECK_UINT(cli.stat.connects, connects0 + 1);

		CHECK(sip_stream_send_sni(&cli, &split, 0, reg, strlen(reg),
					  "b.example.test") > 0);
		CHECK_UINT(cli.stat.connects, connects0 + 2);
		CHECK(conn_of(&cli, &split, "b.example.test") != NULL);

		CHECK(sip_stream_send_sni(&cli, &split, 0, reg, strlen(reg),
					  "a.example.test") < 0);
		CHECK_UINT(cli.stat.connects, connects0 + 2);

		c = conn_of(&cli, &split, "b.example.test");
		if (c)
			sip_stream_conn_close(&cli, sip_stream_conn_handle(c));
		close(lfd);
	}

	/*
	 * One record can carry more plaintext than one read consumes, and what
	 * stays behind is inside OpenSSL, where no epoll event can reach it. The
	 * case fills the receiving framer until its free tail is smaller than a
	 * record, then sends one record that completes the held message and
	 * also carries a whole second one. The peer says nothing afterwards, so
	 * the only thing that can collect the second message is the receive
	 * loop.
	 */
	CASE("tls: plaintext a short read left in the record layer is still collected");
	{
		static char wire1[58546];
		static char wire2[8000];
		static char flush[16000];
		struct tls_peer peer;
		size_t held = 50546;
		size_t chunk = 16382;
		int h1, h2;

		h1 = snprintf(wire1, sizeof(wire1),
			      "OPTIONS sip:one SIP/2.0\r\n"
			      "Via: SIP/2.0/TLS 127.0.0.1:5060;branch=z9hG4bK-big1\r\n"
			      "CSeq: 1 OPTIONS\r\n"
			      "Content-Length: %05zu\r\n\r\n", (size_t)0);
		CHECK(h1 > 0);
		snprintf(wire1, sizeof(wire1),
			 "OPTIONS sip:one SIP/2.0\r\n"
			 "Via: SIP/2.0/TLS 127.0.0.1:5060;branch=z9hG4bK-big1\r\n"
			 "CSeq: 1 OPTIONS\r\n"
			 "Content-Length: %05zu\r\n\r\n", sizeof(wire1) - (size_t)h1);
		memset(wire1 + h1, 'a', sizeof(wire1) - (size_t)h1);

		h2 = snprintf(wire2, sizeof(wire2),
			      "OPTIONS sip:two SIP/2.0\r\n"
			      "Via: SIP/2.0/TLS 127.0.0.1:5060;branch=z9hG4bK-big2\r\n"
			      "CSeq: 2 OPTIONS\r\n"
			      "Content-Length: %05zu\r\n\r\n", (size_t)0);
		CHECK(h2 > 0);
		snprintf(wire2, sizeof(wire2),
			 "OPTIONS sip:two SIP/2.0\r\n"
			 "Via: SIP/2.0/TLS 127.0.0.1:5060;branch=z9hG4bK-big2\r\n"
			 "CSeq: 2 OPTIONS\r\n"
			 "Content-Length: %05zu\r\n\r\n", sizeof(wire2) - (size_t)h2);
		memset(wire2 + h2, 'b', sizeof(wire2) - (size_t)h2);

		/* the tail the last record has to overflow */
		CHECK(SIP_STREAM_RX_MAX - held < sizeof(flush));

		CHECK(tls_peer_open(&peer, port));
		CHECK(tls_peer_up(&peer, WAIT_MS));
		log_reset(&srv_log, 0);

		CHECK(tls_peer_record(&peer, wire1, chunk));
		loop_pump(SLICE_MS);
		CHECK(tls_peer_record(&peer, wire1 + chunk, chunk));
		loop_pump(SLICE_MS);
		CHECK(tls_peer_record(&peer, wire1 + 2 * chunk, chunk));
		loop_pump(SLICE_MS);
		CHECK(tls_peer_record(&peer, wire1 + 3 * chunk, held - 3 * chunk));
		loop_pump(SLICE_MS);
		CHECK_UINT(srv_log.msgs, 0);

		memcpy(flush, wire1 + held, sizeof(wire1) - held);
		memcpy(flush + (sizeof(wire1) - held), wire2, sizeof(wire2));
		CHECK_UINT(sizeof(wire1) - held + sizeof(wire2), sizeof(flush));
		CHECK(tls_peer_record(&peer, flush, sizeof(flush)));

		wait_msgs(&srv_log, 2, WAIT_MS);
		CHECK_UINT(srv_log.msgs, 2);
		CHECK_UINT(srv_log.len[0], sizeof(wire1));
		CHECK_UINT(srv_log.len[1], sizeof(wire2));
		tls_peer_close(&peer);
		loop_pump(SLICE_MS);
	}

	CASE("tls: a TLS key with no loaded context is refused before any socket");
	pair_close();
	sip_stream_init(&cli, &cb, &cli_log, NULL);
	log_reset(&cli_log, 0);
	CHECK(sip_stream_send_sni(&cli, &key, 0, reg, strlen(reg), "sip.example.test") < 0);
	CHECK_UINT(cli.stat.connects, 0);
	CHECK_UINT(cli.stat.tx_err, 1);
	CHECK_UINT(sip_stream_conn_count(&cli), 0);
	sip_stream_done(&cli);

	sip_tls_done(&tls);
}

/* ------------------------------------------------------------------ stats */

static void t_stats(void)
{
	struct sip_stream_key key, back;
	const struct sip_stream_stat *cs, *ss;
	const char *reg = reg_wire();
	const char *ok = ok_wire();
	uint16_t port = 0;
	uint32_t lh;

	lh = pair_open(&port, SIP_STREAM_TCP, NULL);
	CHECK(lh != SIP_HANDLE_NONE);

	key_make(&key, port, 5060, SIP_STREAM_TCP);
	log_reset(&srv_log, 1);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	key_from_src(&back, &srv_log.src[0], port, SIP_STREAM_TCP);
	log_reset(&cli_log, 1);
	CHECK_UINT(sip_stream_send(&srv, &back, 0, ok, strlen(ok)), strlen(ok));
	wait_msgs(&cli_log, 1, WAIT_MS);
	CHECK_UINT(sip_stream_send(&srv, &back, 0, SIP_FRAME_PING, SIP_FRAME_PING_LEN),
		   SIP_FRAME_PING_LEN);
	loop_pump(100);
	CHECK(wait_drained(&cli, WAIT_MS));

	cs = sip_stream_stat(&cli);
	ss = sip_stream_stat(&srv);

	CASE("stats: the client counted one connect, one message out and one in");
	CHECK_UINT(cs->connects, 1);
	CHECK_UINT(cs->accepts, 0);
	CHECK_UINT(cs->tx, 1);
	CHECK_UINT(cs->tx_bytes, strlen(reg));
	CHECK_UINT(cs->rx, 1);
	CHECK_UINT(cs->rx_bytes, strlen(ok));
	CHECK_UINT(cs->frames, 1);
	CHECK_UINT(cs->queued, 1);
	CHECK_UINT(cs->ping, 1);
	CHECK_UINT(cs->pong, 0);
	CHECK_UINT(cs->tx_err, 0);
	CHECK_UINT(cs->rx_err, 0);
	CHECK_UINT(cs->parse_err, 0);
	CHECK_UINT(cs->frame_errs, 0);
	CHECK_UINT(cs->evictions, 0);
	CHECK_UINT(cs->reconnects, 0);
	CHECK_UINT(cs->denied, 0);
	CHECK_UINT(cs->handshakes_failed, 0);
	CHECK_UINT(cs->tos_err, 0);

	CASE("stats: the server counted one accept, no connect, and its own two sends");
	CHECK_UINT(ss->accepts, 1);
	CHECK_UINT(ss->connects, 0);
	CHECK_UINT(ss->rx, 1);
	CHECK_UINT(ss->rx_bytes, strlen(reg));
	CHECK_UINT(ss->frames, 1);
	CHECK_UINT(ss->tx, 2);
	CHECK_UINT(ss->tx_bytes, strlen(ok) + SIP_FRAME_PING_LEN);
	CHECK_UINT(ss->queued, 0);
	CHECK_UINT(ss->tx_err, 0);
	CHECK_UINT(ss->denied, 0);

	CASE("stats: a refused send moves tx_err and nothing else");
	{
		uint64_t tx = cs->tx;

		CHECK(sip_stream_send(&cli, &key, 0, reg, 0) < 0);
		CHECK(sip_stream_send(&cli, &key, 0, NULL, strlen(reg)) < 0);
		CHECK(sip_stream_send(&cli, NULL, 0, reg, strlen(reg)) < 0);
		CHECK(sip_stream_send(NULL, &key, 0, reg, strlen(reg)) < 0);
		CHECK_UINT(cs->tx_err, 3);
		CHECK_UINT(cs->tx, tx);
	}

	CASE("stats: a key with a port of zero is refused and counted");
	{
		struct sip_stream_key bad;

		key_make(&bad, 0, 5060, SIP_STREAM_TCP);
		CHECK(sip_stream_send(&cli, &bad, 0, reg, strlen(reg)) < 0);
		CHECK_UINT(cs->tx_err, 4);

		key_make(&bad, port, 5060, SIP_STREAM_KIND_MAX);
		CHECK(sip_stream_send(&cli, &bad, 0, reg, strlen(reg)) < 0);
		CHECK_UINT(cs->tx_err, 5);
		CHECK_UINT(cs->connects, 1);
	}

	CASE("stats: the accessors refuse a missing pool without touching a counter");
	CHECK(sip_stream_conn_find(NULL, &key, NULL) == NULL);
	CHECK(sip_stream_conn_find(&cli, NULL, NULL) == NULL);
	CHECK_UINT(sip_stream_conn_queued(NULL), 0);
	CHECK_UINT(sip_stream_conn_count(NULL), 0);
	CHECK_UINT(sip_stream_listen_count(NULL), 0);
	CHECK(sip_stream_listener_get(NULL, 1) == NULL);
	CHECK(sip_stream_listener_get(&cli, SIP_HANDLE_NONE) == NULL);
	CHECK_UINT(sip_stream_close_iface(NULL, "lo"), 0);
	sip_stream_conn_close(NULL, 1);
	sip_stream_conn_close(&cli, 0xdeadbeef);
	sip_stream_init(NULL, &cb, NULL, NULL);
	sip_stream_done(NULL);
	CHECK_UINT(cs->tx_err, 5);

	pair_close();
}

/* ------------------------------------------------------------ close_iface */

static void t_close_iface(void)
{
	struct sip_stream_key key, scoped;
	uint16_t port = 0;
	int lfd;

	sip_stream_init(&cli, &cb, &cli_log, NULL);
	log_reset(&cli_log, 0);
	lfd = raw_listen(&port, 8);
	CHECK(lfd >= 0);

	CASE("close_iface: a device no connection is scoped to closes nothing");
	key_make(&key, port, 5060, SIP_STREAM_TCP);
	CHECK(send_reg(&cli, &key) > 0);
	CHECK_UINT(sip_stream_conn_count(&cli), 1);
	CHECK_UINT(sip_stream_close_iface(&cli, "uvoice-no-dev"), 0);
	CHECK_UINT(sip_stream_conn_count(&cli), 1);

	CASE("close_iface: a scoped connection joins its device, an unscoped one does not");
	key_make(&scoped, port, 5061, SIP_STREAM_TCP);
	snprintf(scoped.iface, sizeof(scoped.iface), "lo");
	if (send_reg(&cli, &scoped) > 0) {
		printf("    note: SO_BINDTODEVICE is available in this environment\n");
		CHECK_UINT(sip_stream_conn_count(&cli), 2);
		CHECK_UINT(sip_stream_close_iface(&cli, "lo"), 1);
		CHECK_UINT(sip_stream_conn_count(&cli), 1);
		CHECK(conn_of(&cli, &key, NULL) != NULL);
	} else {
		printf("    note: SO_BINDTODEVICE refused, the scoped connect is the "
		       "path this case covers\n");
		CHECK_UINT(sip_stream_conn_count(&cli), 1);
		CHECK_UINT(cli.stat.tx_err, 1);
	}

	CASE("close_iface: the unscoped connections answer to the empty device name");
	CHECK_UINT(sip_stream_close_iface(&cli, NULL), 1);
	CHECK_UINT(sip_stream_conn_count(&cli), 0);
	CHECK_UINT(sip_stream_close_iface(&cli, ""), 0);

	CASE("done: the destructor releases what is left and reports no transition");
	CHECK(send_reg(&cli, &key) > 0);
	CHECK_UINT(sip_stream_conn_count(&cli), 1);
	log_reset(&cli_log, 0);
	sip_stream_done(&cli);
	CHECK_UINT(sip_stream_conn_count(&cli), 0);
	CHECK_UINT(cli_log.evs, 0);
	sip_stream_done(&cli);

	close(lfd);
}

/* --------------------------------------------------- transaction environment */

static struct sip_txn_mgr mgr;

static void t_env(void)
{
	struct sip_stream_env se;
	struct sip_stream_key key;
	struct sip_txn_dst dst, other, empty;
	struct sip_timer_cfg tc;
	const char *reg = reg_wire();
	char branch[SIP_BRANCH_MAX];
	char wire[512];
	struct sip_txn *txn;
	uint16_t port = 0;
	uint32_t lh;
	int len;

	lh = pair_open(&port, SIP_STREAM_TCP, NULL);
	CHECK(lh != SIP_HANDLE_NONE);

	CASE("env: the environment carries the reliable flag and the uloop clock");
	sip_stream_env_init(&se, &cli, SIP_STREAM_TCP, 5060, NULL, 0x60);
	CHECK_UINT(se.env.flags & SIP_TXN_ENV_RELIABLE, SIP_TXN_ENV_RELIABLE);
	CHECK(se.env.base.priv == &se);
	CHECK(se.env.send != NULL);
	CHECK(se.env.base.now_ms != NULL);
	CHECK(se.env.base.timer_set != NULL);
	CHECK(se.env.base.timer_cancel != NULL);
	CHECK(se.env.base.trace != NULL);
	CHECK(se.env.base.now_ms(&se.env.base) > 0);
	CHECK(se.st == &cli);
	CHECK_UINT(se.local_port, 5060);
	CHECK_UINT(se.kind, SIP_STREAM_TCP);
	CHECK_UINT(se.tos, 0x60);
	CHECK(!se.iface[0]);
	CHECK(!se.sni[0]);

	CASE("env: the key it builds carries its own local identity");
	CHECK(sip_dst_set(&dst, "127.0.0.1", port));
	CHECK(sip_stream_env_key(&se, &dst, &key));
	CHECK_UINT(key.local_port, 5060);
	CHECK_UINT(key.kind, SIP_STREAM_TCP);
	CHECK(!key.iface[0]);
	CHECK(sip_dst_eq(&key.peer, &dst));

	CASE("env: a connect-forbidden destination keeps its flag through the key");
	{
		struct sip_txn_dst marked = dst;

		marked.flags = SIP_DST_F_NO_CONNECT;
		CHECK(sip_stream_env_key(&se, &marked, &key));
		CHECK_UINT(key.peer.flags & SIP_DST_F_NO_CONNECT, SIP_DST_F_NO_CONNECT);
	}

	CASE("env: a missing destination is refused");
	memset(&empty, 0, sizeof(empty));
	CHECK(!sip_stream_env_key(&se, &empty, &key));
	CHECK(!key.peer.sa_len);
	CHECK(!sip_stream_env_key(&se, NULL, &key));
	CHECK(!sip_stream_env_key(NULL, &dst, &key));
	CHECK(!sip_stream_env_key(&se, &dst, NULL));

	CASE("env: an AF_UNIX destination through send is refused and counted");
	memset(&other, 0, sizeof(other));
	other.sa.ss_family = AF_UNIX;
	other.sa_len = sizeof(struct sockaddr_un);
	CHECK(!sip_stream_env_key(&se, &other, &key));
	CHECK(se.env.send(&se.env, reg, strlen(reg), &other) < 0);
	CHECK_UINT(cli.stat.tx_err, 1);
	CHECK_UINT(cli.stat.connects, 0);

	CASE("env: the local identity, the name and the byte all follow a reload");
	sip_stream_env_local_set(&se, 5062, "lo");
	CHECK_UINT(se.local_port, 5062);
	CHECK(!strcmp(se.iface, "lo"));
	sip_stream_env_sni_set(&se, "sip.example.test");
	CHECK(!strcmp(se.sni, "sip.example.test"));
	sip_stream_env_tos_set(&se, 0xb8);
	CHECK_UINT(se.tos, 0xb8);
	sip_stream_env_local_set(&se, 5060, NULL);
	sip_stream_env_sni_set(&se, NULL);
	sip_stream_env_tos_set(&se, 0x60);
	CHECK(!se.iface[0]);
	CHECK(!se.sni[0]);
	sip_stream_env_init(NULL, &cli, SIP_STREAM_TCP, 5060, NULL, 0);
	sip_stream_env_local_set(NULL, 1, "lo");
	sip_stream_env_sni_set(NULL, "x");
	sip_stream_env_tos_set(NULL, 1);

	CASE("env: a send through the environment reaches the pool and the peer");
	log_reset(&srv_log, 1);
	CHECK_UINT(se.env.send(&se.env, reg, strlen(reg), &dst), strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	CHECK_UINT(cli.stat.connects, 1);
	CHECK(sip_stream_env_key(&se, &dst, &key));
	CHECK_UINT(conn_of(&cli, &key, NULL)->tos_cur, 0x60);

	CASE("env: a client transaction over the stream arms no retransmission timer");
	sip_timer_cfg_default(&tc);
	sip_txn_mgr_init(&mgr, &se.env, &tc);
	CHECK(sip_branch_new(&mgr, branch, sizeof(branch)));
	len = snprintf(wire, sizeof(wire),
		       "OPTIONS sip:example.com SIP/2.0\r\n"
		       "Via: SIP/2.0/TCP 127.0.0.1:5060;branch=%s;rport\r\n"
		       "From: <sip:a@example.com>;tag=t9\r\n"
		       "To: <sip:b@example.com>\r\n"
		       "Call-ID: c9@127.0.0.1\r\n"
		       "CSeq: 9 OPTIONS\r\n"
		       "Content-Length: 0\r\n\r\n", branch);
	CHECK(len > 0 && (size_t)len < sizeof(wire));
	log_reset(&srv_log, 1);
	txn = sip_txn_client_start(&mgr, SIP_METHOD_OPTIONS, sip_str_cstr(branch), wire,
				   (size_t)len, &dst, 0x00010001);
	CHECK(txn != NULL);
	CHECK(txn && !txn->retx.armed);
	CHECK(txn && txn->life.armed);
	CHECK_UINT(txn ? txn->life.id : 0, SIP_TIMER_F);
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	CHECK(!strcmp(srv_log.method[0], "OPTIONS"));
	CHECK_UINT(cli.stat.tx, 2);
	CHECK_UINT(cli.stat.connects, 1);

	sip_txn_mgr_done(&mgr);
	pair_close();
}

/* --------------------------------------------------- connections over ::1 */

static void t_conn6_skip(const char *why)
{
	CASE("conn6: a connect to a ::1 listener reaches up and carries a "
	     "message");
	tu_skip(why);
	CASE("conn6: the accepted v6 peer is taken, and the key round-trips");
	tu_skip(why);
	CASE("conn6: the key of a v6 peer is not equal to the v4 key of the "
	     "same port");
	tu_skip(why);
}

static void t_conn6(void)
{
	struct sip_stream_key key, v4key, back;
	const char *reg = reg_wire();
	const char *ok = ok_wire();
	struct sip_conn *in;
	char host[64] = "";
	char why[WHY_MAX];
	uint16_t port = 0;
	uint32_t lh;

	if (!tu_v6_loopback(why, sizeof(why))) {
		t_conn6_skip(why);

		return;
	}

	lh = pair_open_af(AF_INET6, &port, SIP_STREAM_TCP, NULL);
	CHECK(lh != SIP_HANDLE_NONE);

	CASE("conn6: a connect to a ::1 listener reaches up and carries a "
	     "message");
	key_make_addr(&key, "::1", port, 5060, SIP_STREAM_TCP);
	log_reset(&srv_log, 1);
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	CHECK(!strcmp(srv_log.method[0], "REGISTER"));
	CHECK_UINT(srv_log.len[0], strlen(reg));
	CHECK_UINT(cli.stat.connects, 1);
	CHECK_UINT(srv.stat.accepts, 1);
	CHECK(wait_up(&cli, &key, NULL, WAIT_MS));

	CASE("conn6: the accepted v6 peer is taken, and the key round-trips");
	CHECK_UINT(srv_log.src[0].sa.ss_family, AF_INET6);
	CHECK(sip_dst_host(&srv_log.src[0], host, sizeof(host)));
	CHECK(!strcmp(host, "::1"));
	CHECK(sip_dst_port(&srv_log.src[0]) != 0);
	CHECK(sip_dst_port(&srv_log.src[0]) != port);
	CHECK_UINT(srv_log.src[0].flags & SIP_DST_F_NO_CONNECT,
		   SIP_DST_F_NO_CONNECT);
	in = conn_inbound(&srv);
	CHECK(in != NULL);
	CHECK_UINT(in ? in->key.peer.sa.ss_family : 0, AF_INET6);
	key_from_src(&back, &srv_log.src[0], port, SIP_STREAM_TCP);
	log_reset(&cli_log, 1);
	CHECK_UINT(sip_stream_send(&srv, &back, 0, ok, strlen(ok)), strlen(ok));
	wait_msgs(&cli_log, 1, WAIT_MS);
	CHECK_UINT(cli_log.msgs, 1);
	CHECK_UINT(cli_log.status[0], 200);
	CHECK_UINT(cli_log.src[0].sa.ss_family, AF_INET6);
	CHECK_UINT(sip_dst_port(&cli_log.src[0]), port);
	CHECK_UINT(srv.stat.connects, 0);

	CASE("conn6: the key of a v6 peer is not equal to the v4 key of the "
	     "same port");
	key_make(&v4key, port, 5060, SIP_STREAM_TCP);
	CHECK(!sip_stream_key_eq(&key, &v4key));
	CHECK(conn_of(&cli, &key, NULL) != NULL);
	CHECK(conn_of(&cli, &v4key, NULL) == NULL);

	pair_close();
}

static void t_env6_skip(const char *why)
{
	CASE("env6: a v6 destination produces a key and the send reaches the "
	     "peer");
	tu_skip(why);
	CASE("env6: a v4 destination on a v6 env takes its own v4 connection");
	tu_skip(why);
}

static void t_env6(void)
{
	struct sip_stream_env se;
	struct sip_stream_key key;
	struct sip_txn_dst dst, v4dst;
	const char *reg = reg_wire();
	char why[WHY_MAX];
	uint16_t port = 0;
	uint16_t v4port = 0;
	uint32_t lh;
	int lfd;

	if (!tu_v6_loopback(why, sizeof(why))) {
		t_env6_skip(why);

		return;
	}

	lh = pair_open_af(AF_INET6, &port, SIP_STREAM_TCP, NULL);
	CHECK(lh != SIP_HANDLE_NONE);
	sip_stream_env_init(&se, &cli, SIP_STREAM_TCP, 5060, NULL, 0x60);

	CASE("env6: a v6 destination produces a key and the send reaches the "
	     "peer");
	CHECK(sip_dst_set(&dst, "::1", port));
	CHECK(sip_stream_env_key(&se, &dst, &key));
	CHECK_UINT(key.peer.sa.ss_family, AF_INET6);
	CHECK_UINT(key.local_port, 5060);
	CHECK_UINT(key.kind, SIP_STREAM_TCP);
	CHECK(sip_dst_eq(&key.peer, &dst));
	log_reset(&srv_log, 1);
	CHECK_UINT(se.env.send(&se.env, reg, strlen(reg), &dst), strlen(reg));
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	CHECK_UINT(cli.stat.connects, 1);
	CHECK_UINT(cli.stat.tx_err, 0);

	/*
	 * The environment carries no family of its own: it names a local port,
	 * a device and a kind, and the destination names the family. A v4
	 * destination therefore builds a v4 key and opens a second connection
	 * beside the v6 one. The account layer holds a line in one family at a
	 * time, and its media follows the signalling (RFC 6157 section 5), so
	 * this layer refuses no address it can carry.
	 */
	CASE("env6: a v4 destination on a v6 env takes its own v4 connection");
	lfd = raw_listen(&v4port, 8);
	CHECK(lfd >= 0);
	CHECK(sip_dst_set(&v4dst, "127.0.0.1", v4port));
	CHECK(sip_stream_env_key(&se, &v4dst, &key));
	CHECK_UINT(key.peer.sa.ss_family, AF_INET);
	CHECK_UINT(se.env.send(&se.env, reg, strlen(reg), &v4dst), strlen(reg));
	CHECK_UINT(cli.stat.connects, 2);
	CHECK_UINT(cli.stat.tx_err, 0);
	CHECK_UINT(sip_stream_conn_count(&cli), 2);
	CHECK(conn_of(&cli, &key, NULL) != NULL);

	if (lfd >= 0)
		close(lfd);

	pair_close();
}

static void t_tos6(void)
{
	struct sip_stream_key key;
	const char *reg = reg_wire();
	struct sip_conn *c;
	char why[WHY_MAX];
	uint16_t port = 0;
	int lfd;

	if (!tu_v6_loopback(why, sizeof(why))) {
		CASE("tos6: a v6 connection socket carries the byte as "
		     "IPV6_TCLASS");
		tu_skip(why);

		return;
	}

	sip_stream_init(&cli, &cb, &cli_log, NULL);
	log_reset(&cli_log, 0);
	lfd = raw_listen_af(AF_INET6, &port, 8);
	CHECK(lfd >= 0);

	CASE("tos6: a v6 connection socket carries the byte as IPV6_TCLASS");
	key_make_addr(&key, "::1", port, 5060, SIP_STREAM_TCP);
	CHECK_UINT(sip_stream_send(&cli, &key, 0xb8, reg, strlen(reg)),
		   strlen(reg));
	c = conn_of(&cli, &key, NULL);
	CHECK(c != NULL);
	CHECK_UINT(c ? c->tos_cur : 0, 0xb8);
	CHECK_UINT(tos_read_af(c, AF_INET6), 0xb8);
	CHECK_UINT(cli.stat.tos_err, 0);

	if (lfd >= 0)
		close(lfd);

	sip_stream_done(&cli);
}

static struct {
	unsigned int count;
	struct sip_tap_endpoints ep;
	size_t len;
	char wire[1024];
} tap_log[2];

static void tap_record(bool tx, const struct sip_tap_endpoints *ep,
		       const void *buf, size_t len)
{
	tap_log[tx].count++;
	tap_log[tx].ep = *ep;
	tap_log[tx].len = len;
	CHECK(len <= sizeof(tap_log[tx].wire));
	if (len <= sizeof(tap_log[tx].wire))
		memcpy(tap_log[tx].wire, buf, len);
}

static void tap_start(void)
{
	memset(tap_log, 0, sizeof(tap_log));
	sip_tap_set(tap_record);
}

static void tap_case(enum sip_stream_kind kind, const char *name)
{
	char text[160];

	snprintf(text, sizeof(text), "tap %s %s",
		 sip_stream_kind_name(kind), name);
	CASE(text);
}

static void tap_check(bool tx, enum sip_stream_kind kind, const char *wire,
		      uint16_t local_port, uint16_t peer_port)
{
	const struct sip_tap_endpoints *ep = &tap_log[tx].ep;
	uint8_t loopback[16] = { 127, 0, 0, 1 };
	static const struct trace_endpoint zero;

	CHECK_UINT(tap_log[tx].count, 1);
	CHECK_UINT(tap_log[tx].len, strlen(wire));
	CHECK(!memcmp(tap_log[tx].wire, wire, strlen(wire)));
	CHECK_UINT(ep->kind, kind == SIP_STREAM_TLS ?
		   TRACE_PKT_SIP_TLS : TRACE_PKT_SIP_TCP);
	CHECK_UINT(ep->peer.ipver, 4);
	CHECK_UINT(ep->peer.port, peer_port);
	CHECK(!memcmp(ep->peer.addr, loopback, sizeof(loopback)));
	if (!local_port) {
		CHECK_UINT(ep->local.ipver, ep->peer.ipver);
		CHECK_UINT(ep->local.port, 0);
		CHECK(!memcmp(ep->local.addr, zero.addr, sizeof(zero.addr)));
		return;
	}

	CHECK_UINT(ep->local.ipver, 4);
	CHECK_UINT(ep->local.port, local_port);
	CHECK(!memcmp(ep->local.addr, loopback, sizeof(loopback)));
}

static uint16_t conn_port_get(const struct sip_conn *c)
{
	struct sockaddr_in sa;
	socklen_t len = sizeof(sa);

	CHECK(c != NULL);
	if (!c)
		return 0;

	CHECK(getsockname(c->ufd.fd, (struct sockaddr *)&sa, &len) == 0);
	CHECK_UINT(sa.sin_family, AF_INET);
	CHECK(ntohs(sa.sin_port) != 0);
	return ntohs(sa.sin_port);
}

static void t_tap(enum sip_stream_kind kind)
{
	static const struct trace_endpoint zero;
	struct sip_stream_key key, back;
	struct sip_tls_cfg cfg = { 0 };
	const char *reg = reg_wire();
	const char *ok = ok_wire();
	struct sip_conn *out, *in;
	uint16_t port = 0, client_port;
	unsigned int spent;

	tap_case(kind,
		 "tx: a connecting message keeps its family and zero address");
	if (kind == SIP_STREAM_TLS) {
		CHECK(fixtures_take());
		cfg.cert_path = fix_cert;
		cfg.key_path = fix_key;
		sip_tls_init(&tls);
		CHECK(sip_tls_ctx_load(&tls, &cfg));
	}
	CHECK(pair_open(&port, kind, kind == SIP_STREAM_TLS ? &tls : NULL)
	      != SIP_HANDLE_NONE);
	key_make(&key, port, 5060, kind);
	tap_start();
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	out = conn_of(&cli, &key, NULL);
	CHECK(out != NULL);
	CHECK_UINT(out ? out->state : 0, SIP_CONN_CONNECTING);
	tap_check(true, kind, reg, 0, port);
	CHECK_UINT(tap_log[false].count, 0);
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	CHECK_UINT(tap_log[true].count, 1);
	sip_tap_set(NULL);

	in = conn_inbound(&srv);
	CHECK(in != NULL);
	if (!out || !in)
		goto done;
	client_port = conn_port_get(out);
	CHECK_UINT(conn_port_get(in), port);
	key_from_src(&back, &srv_log.src[0], port, kind);

	tap_case(kind,
		 "tx: an outbound message has plaintext and local address");
	log_reset(&srv_log, 1);
	tap_start();
	CHECK_UINT(send_reg(&cli, &key), strlen(reg));
	tap_check(true, kind, reg, client_port, port);
	CHECK_UINT(tap_log[false].count, 0);
	sip_tap_set(NULL);

	tap_case(kind,
		 "rx: an accepted message has plaintext and local address");
	tap_start();
	wait_msgs(&srv_log, 1, WAIT_MS);
	CHECK_UINT(srv_log.msgs, 1);
	tap_check(false, kind, reg, port, client_port);
	CHECK_UINT(tap_log[true].count, 0);
	sip_tap_set(NULL);

	tap_case(kind,
		 "tx: an accepted message has plaintext and local address");
	log_reset(&cli_log, 1);
	tap_start();
	CHECK_UINT(sip_stream_send(&srv, &back, 0, ok, strlen(ok)), strlen(ok));
	tap_check(true, kind, ok, port, client_port);
	CHECK_UINT(tap_log[false].count, 0);
	sip_tap_set(NULL);

	tap_case(kind,
		 "rx: an outbound message has plaintext and local address");
	tap_start();
	wait_msgs(&cli_log, 1, WAIT_MS);
	CHECK_UINT(cli_log.msgs, 1);
	tap_check(false, kind, ok, client_port, port);
	CHECK_UINT(tap_log[true].count, 0);
	sip_tap_set(NULL);

	tap_case(kind, "keepalive: the PONG answer produces no tap call");
	CHECK_UINT(sip_stream_send(&cli, &key, 0, SIP_FRAME_PING,
				  SIP_FRAME_PING_LEN), SIP_FRAME_PING_LEN);
	tap_start();
	for (spent = 0; spent < WAIT_MS && !srv.stat.ping; spent += SLICE_MS)
		loop_pump(SLICE_MS);
	CHECK_UINT(srv.stat.ping, 1);
	CHECK(wait_drained(&srv, WAIT_MS));
	loop_pump(SLICE_MS);
	CHECK_UINT(tap_log[true].count, 0);
	CHECK_UINT(tap_log[false].count, 0);
	sip_tap_set(NULL);

	tap_case(kind, "local: a released slot clears its endpoint");
	CHECK_UINT(sip_stream_close_all(&srv), 1);
	CHECK_UINT(sip_stream_close_all(&cli), 1);
	CHECK(!memcmp(&out->local, &zero, sizeof(zero)));
	CHECK(!memcmp(&in->local, &zero, sizeof(zero)));

done:
	pair_close();
	if (kind == SIP_STREAM_TLS)
		sip_tls_done(&tls);
}

static void t_tap_refused(void)
{
	struct sip_stream_key key;
	struct sip_conn *c;
	uint16_t port = 0;

	CASE("tap tcp tx: refused payloads produce no tap call");
	CHECK(pair_open(&port, SIP_STREAM_TCP, NULL) != SIP_HANDLE_NONE);
	key_make(&key, port, 5060, SIP_STREAM_TCP);
	memset(oversize, 'x', sizeof(oversize));
	tap_start();
	CHECK(sip_stream_send(&cli, &key, 0, oversize, MSG_MAX + 1) < 0);
	CHECK_UINT(cli.stat.connects, 0);
	CHECK_UINT(tap_log[true].count, 0);
	CHECK_UINT(tap_log[false].count, 0);
	sip_tap_set(NULL);

	CHECK_UINT(sip_stream_send(&cli, &key, 0, oversize, MSG_MAX), MSG_MAX);
	c = conn_of(&cli, &key, NULL);
	CHECK(c != NULL);
	CHECK_UINT(c ? c->state : 0, SIP_CONN_CONNECTING);
	CHECK_UINT(sip_stream_conn_queued(c), MSG_MAX);
	tap_start();
	CHECK(send_reg(&cli, &key) < 0);
	CHECK_UINT(sip_stream_conn_queued(c), MSG_MAX);
	CHECK_UINT(cli.stat.tx, 1);
	CHECK_UINT(cli.stat.tx_err, 2);
	CHECK_UINT(tap_log[true].count, 0);
	CHECK_UINT(tap_log[false].count, 0);
	sip_tap_set(NULL);
	pair_close();
}

int main(void)
{
	/* a write to a peer that vanished must surface as a case failure, not
	 * as a signal that takes the whole suite with it, as in src/main.c */
	signal(SIGPIPE, SIG_IGN);
	alarm(240);
	uloop_init();
	sip_wire_host = proto_host_fake();

	t_init_dirty();
	t_constants();
	t_key();
	t_key_family();
	t_listen();
	t_listen6();
	t_roundtrip();
	t_queue();
	t_no_connect();
	t_reentrancy();
	t_ping();
	t_framing();
	t_idle();
	t_lru();
	t_accept_denied();
	t_backoff();
	t_tos();
	t_backpressure();
	t_tls();
	t_stats();
	t_close_iface();
	t_env();
	t_conn6();
	t_env6();
	t_tos6();
	t_tap(SIP_STREAM_TCP);
	t_tap(SIP_STREAM_TLS);
	t_tap_refused();

	sip_tap_set(NULL);

	uloop_done();

	return tu_summary("test_sip_stream");
}
