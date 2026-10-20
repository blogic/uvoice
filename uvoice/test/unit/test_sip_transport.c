// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the UDP transport (src/proto/sip/transport.c).
 *
 * The cases use real sockets on the loopback interface and a real uloop,
 * because transport.c owns the socket. Every layer above it runs on a virtual
 * clock in its own suite. Each case sends a datagram and pumps the loop until
 * the receive callback has what it waits for. Otherwise a guard timer ends the
 * loop, and the case fails.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#include "proto/sip/transport.h"
#include "proto/sip/tap.h"
#include "proto_host_fake.h"
#include "test_util.h"

#define RX_LOG_MAX	8

static struct sip_transport tp;

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

static void transport_done(void)
{
	sip_tap_set(NULL);
	sip_transport_done(&tp);
}

static void tap_check(bool tx, const char *wire, size_t len, int family,
		      const char *local, uint16_t local_port,
		      const char *peer, uint16_t peer_port)
{
	const struct sip_tap_endpoints *ep = &tap_log[tx].ep;
	uint8_t addr[16] = { 0 };
	unsigned int ipver = family == AF_INET6 ? 6 : 4;

	CHECK_UINT(tap_log[tx].count, 1);
	CHECK_UINT(tap_log[!tx].count, 0);
	CHECK_UINT(tap_log[tx].len, len);
	CHECK(!memcmp(tap_log[tx].wire, wire, len));
	CHECK_UINT(ep->kind, TRACE_PKT_SIP_UDP);
	CHECK_UINT(ep->local.ipver, ipver);
	CHECK_UINT(ep->local.port, local_port);
	if (local)
		CHECK(inet_pton(family, local, addr) == 1);
	CHECK(!memcmp(ep->local.addr, addr, sizeof(addr)));
	CHECK(inet_pton(family, peer, addr) == 1);
	CHECK_UINT(ep->peer.ipver, ipver);
	CHECK_UINT(ep->peer.port, peer_port);
	CHECK(!memcmp(ep->peer.addr, addr, sizeof(addr)));
}

static struct {
	unsigned int msgs;
	unsigned int pings;
	unsigned int want;

	uint32_t sock[RX_LOG_MAX];
	char method[RX_LOG_MAX][16];
	uint32_t status[RX_LOG_MAX];
	char src_host[RX_LOG_MAX][64];
	uint16_t src_port[RX_LOG_MAX];
	size_t len[RX_LOG_MAX];
	size_t ping_len[RX_LOG_MAX];
} rx;

static void rx_reset(unsigned int want)
{
	memset(&rx, 0, sizeof(rx));
	rx.want = want;
}

static void rx_done_check(void)
{
	if (rx.want && rx.msgs + rx.pings >= rx.want)
		uloop_end();
}

static void on_msg(struct sip_transport *t, uint32_t sock, struct sip_msg *msg,
		   const struct sip_txn_dst *src, void *priv)
{
	unsigned int i = rx.msgs;

	if (i < RX_LOG_MAX) {
		rx.sock[i] = sock;
		rx.len[i] = msg->len;
		rx.status[i] = msg->status;
		if (msg->is_request)
			sip_str_copy(rx.method[i], sizeof(rx.method[i]),
				     msg->method_name);
		sip_dst_host(src, rx.src_host[i], sizeof(rx.src_host[i]));
		rx.src_port[i] = sip_dst_port(src);
	}

	rx.msgs++;
	rx_done_check();
}

static void on_ping(struct sip_transport *t, uint32_t sock, size_t len,
		    const struct sip_txn_dst *src, void *priv)
{
	if (rx.pings < RX_LOG_MAX)
		rx.ping_len[rx.pings] = len;

	rx.pings++;
	rx_done_check();
}

static const struct sip_transport_cb cb = {
	.msg = on_msg,
	.ping = on_ping,
};

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

static uint32_t sock_open_af(int family, const char *addr, uint16_t port,
			     size_t rx_max)
{
	struct sip_sock_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.family = family;
	cfg.addr = addr;
	cfg.port = port;
	cfg.rx_max = rx_max;

	return sip_sock_open(&tp, &cfg);
}

static uint32_t sock_open(uint16_t port, size_t rx_max)
{
	return sock_open_af(AF_INET, "127.0.0.1", port, rx_max);
}

/*
 * A port nothing holds. The transport keys a socket on the configured port, so
 * a shared socket needs a port the caller names; an ephemeral bind is never
 * shared, which is what the port 0 case asserts.
 */
static uint16_t free_port(void)
{
	struct sockaddr_in sa;
	socklen_t slen = sizeof(sa);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	uint16_t port = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (fd >= 0 && !bind(fd, (struct sockaddr *)&sa, sizeof(sa)) &&
	    !getsockname(fd, (struct sockaddr *)&sa, &slen))
		port = ntohs(sa.sin_port);

	if (fd >= 0)
		close(fd);

	return port;
}

/*
 * Occupy a real loopback port and report it. The transport sets no
 * SO_REUSEADDR, so a socket pointed at this port fails its next bind with
 * EADDRINUSE: the failure is genuine and not a privilege artefact that would
 * pass silently as root. The caller closes *fd to give the port back.
 */
static uint16_t port_take(int *fd)
{
	struct sockaddr_in sa;
	socklen_t slen = sizeof(sa);

	*fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (*fd < 0)
		return 0;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(*fd, (struct sockaddr *)&sa, sizeof(sa)) ||
	    getsockname(*fd, (struct sockaddr *)&sa, &slen)) {
		close(*fd);
		*fd = -1;
		return 0;
	}

	return ntohs(sa.sin_port);
}

static uint16_t free_port6(void)
{
	struct sockaddr_in6 sa;
	socklen_t slen = sizeof(sa);
	int fd = socket(AF_INET6, SOCK_DGRAM, 0);
	uint16_t port = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sin6_family = AF_INET6;
	sa.sin6_addr = in6addr_loopback;

	if (fd >= 0 && !bind(fd, (struct sockaddr *)&sa, sizeof(sa)) &&
	    !getsockname(fd, (struct sockaddr *)&sa, &slen))
		port = ntohs(sa.sin6_port);

	if (fd >= 0)
		close(fd);

	return port;
}

static uint16_t sock_port(uint32_t handle)
{
	struct sip_sock *sk = sip_sock_get(&tp, handle);

	return sk ? sip_sock_port(sk) : 0;
}

static int tos_read(uint32_t handle)
{
	struct sip_sock *sk = sip_sock_get(&tp, handle);
	socklen_t len = sizeof(int);
	int val = -1;

	if (!sk)
		return -1;

	if (getsockopt(sk->ufd.fd, IPPROTO_IP, IP_TOS, &val, &len) < 0)
		return -1;

	return val;
}

static int tos6_read(uint32_t handle)
{
	struct sip_sock *sk = sip_sock_get(&tp, handle);
	socklen_t len = sizeof(int);
	int val = -1;

	if (!sk)
		return -1;

	if (getsockopt(sk->ufd.fd, IPPROTO_IPV6, IPV6_TCLASS, &val, &len) < 0)
		return -1;

	return val;
}

static int v6only_read(uint32_t handle)
{
	struct sip_sock *sk = sip_sock_get(&tp, handle);
	socklen_t len = sizeof(int);
	int val = -1;

	if (!sk)
		return -1;

	if (getsockopt(sk->ufd.fd, IPPROTO_IPV6, IPV6_V6ONLY, &val, &len) < 0)
		return -1;

	return val;
}

static const char *msg_register(void)
{
	return "REGISTER sip:example.com SIP/2.0\r\n"
	       "Via: SIP/2.0/UDP 127.0.0.1:5060;branch=z9hG4bK-uvoice-1;rport\r\n"
	       "From: <sip:a@example.com>;tag=t1\r\n"
	       "To: <sip:a@example.com>\r\n"
	       "Call-ID: c1@127.0.0.1\r\n"
	       "CSeq: 1 REGISTER\r\n"
	       "Content-Length: 0\r\n\r\n";
}

/* ------------------------------------------------------------------- init */

/*
 * The init leaves the receive buffer as it is, so this case runs first. Every
 * case after it receives into a buffer that holds 0xff. A byte read before the
 * datagram put it there therefore fails a parse below.
 */
static void test_init_dirty(void)
{
	static const struct sip_sock_stat zero_stat;
	unsigned int i;

	CASE("init: a transport initialised over garbage is clean around its buffer");
	memset(&tp, 0xff, sizeof(tp));
	sip_transport_init(&tp, &cb, NULL);
	CHECK(tp.cb.msg == cb.msg);
	CHECK(tp.cb.ping == cb.ping);
	CHECK(tp.priv == NULL);
	CHECK_UINT(tp.active, 0);
	CHECK_UINT(tp.denied, 0);
	CHECK(!memcmp(&tp.total, &zero_stat, sizeof(zero_stat)));
	for (i = 0; i < SIP_SOCK_MAX; i++) {
		const struct sip_sock *sk = &tp.sock[i];

		CHECK(sk->tp == NULL);
		CHECK_UINT(sk->index, i);
		CHECK_UINT(sk->gen, 1);
		CHECK(sk->ufd.fd == -1);
		CHECK(sk->ufd.cb == NULL);
		CHECK(!sk->in_use);
		CHECK(!sk->up);
		CHECK_UINT(sk->refs, 0);
		CHECK_UINT(sk->port, 0);
		CHECK_UINT(sk->tos_cur, SIP_TOS_UNSET);
		CHECK_UINT(sk->addr[0], 0);
		CHECK_UINT(sk->iface[0], 0);
		CHECK_UINT(sk->bound[0], 0);
		CHECK_UINT(sk->cfg.rx_max, 0);
		CHECK(!memcmp(&sk->stat, &zero_stat, sizeof(zero_stat)));
	}
	/* the buffer is what the init leaves alone */
	CHECK_UINT((unsigned char)tp.rx[0], 0xff);
	CHECK_UINT((unsigned char)tp.rx[SIP_RX_BUF_MAX - 1], 0xff);
}

/* ---------------------------------------------------------------- opening */

static void test_open(void)
{
	uint32_t a, b, c;
	struct sip_sock *sk;
	char host[64];
	uint16_t port;
	unsigned int count, i;

	CASE("open: a fresh transport owns no socket");
	sip_transport_init(&tp, &cb, NULL);
	CHECK_UINT(sip_sock_count(&tp), 0);
	CHECK_UINT(sip_transport_stat(&tp)->rx, 0);

	CASE("open: an ephemeral bind reports the real port through getsockname");
	a = sock_open(0, 0);
	CHECK(a != SIP_HANDLE_NONE);
	sk = sip_sock_get(&tp, a);
	CHECK(sk != NULL);
	CHECK(sip_sock_port(sk) != 0);
	CHECK(sip_sock_up(sk));
	CHECK(sip_sock_local_host(sk, host, sizeof(host)));
	CHECK(!strcmp(host, "127.0.0.1"));
	CHECK_UINT(sip_sock_count(&tp), 1);

	CASE("open: port 0 is not an identity, so a second one gets its own socket");
	b = sock_open(0, 0);
	CHECK(b != SIP_HANDLE_NONE);
	CHECK(b != a);
	CHECK(sock_port(a) != sock_port(b));
	CHECK_UINT(sip_sock_count(&tp), 2);

	CASE("open: a named port that two accounts share is one socket, two refs");
	port = free_port();
	CHECK(port != 0);
	c = sock_open(port, 0);
	CHECK(c != SIP_HANDLE_NONE);
	CHECK_UINT(sock_port(c), port);
	CHECK_UINT(sip_sock_count(&tp), 3);
	CHECK_UINT(sock_open(port, 0), c);
	CHECK_UINT(sip_sock_count(&tp), 3);

	CASE("open: an ephemeral socket is never shared by naming its port later");
	CHECK(sock_open(sock_port(a), 0) == SIP_HANDLE_NONE);
	CHECK_UINT(sip_sock_count(&tp), 3);

	CASE("open: a reference is needed for every close");
	sip_sock_close(&tp, c);
	CHECK(sip_sock_get(&tp, c) != NULL);
	CHECK_UINT(sip_sock_count(&tp), 3);
	sip_sock_close(&tp, c);
	CHECK(sip_sock_get(&tp, c) == NULL);
	CHECK_UINT(sip_sock_count(&tp), 2);

	CASE("open: a closed handle never resolves again");
	CHECK(sip_sock_get(&tp, c) == NULL);
	sip_sock_close(&tp, c);
	CHECK_UINT(sip_sock_count(&tp), 2);

	CASE("open: the pool refuses the ninth socket and counts it");
	CHECK_UINT(SIP_SOCK_MAX, 8);
	count = sip_sock_count(&tp);
	for (i = count; i < SIP_SOCK_MAX; i++)
		CHECK(sock_open(0, 0) != SIP_HANDLE_NONE);
	CHECK_UINT(sip_sock_count(&tp), SIP_SOCK_MAX);
	CHECK(sock_open(0, 0) == SIP_HANDLE_NONE);
	CHECK_UINT(errno, ENOSPC);
	CHECK_UINT(tp.denied, 1);

	CASE("open: an unusable bind address is refused");
	transport_done();
	sip_transport_init(&tp, &cb, NULL);
	CHECK(sip_sock_open(&tp, &(struct sip_sock_cfg){ .family = AF_INET,
							.addr = "not-an-address" }) ==
	      SIP_HANDLE_NONE);
	CHECK_UINT(sip_sock_count(&tp), 0);

	CASE("open: a port already taken is refused, never shared");
	a = sock_open(0, 0);
	CHECK(a != SIP_HANDLE_NONE);
	{
		int fd = socket(AF_INET, SOCK_DGRAM, 0);
		struct sockaddr_in sa;
		socklen_t slen = sizeof(sa);

		memset(&sa, 0, sizeof(sa));
		sa.sin_family = AF_INET;
		sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		CHECK(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
		CHECK(getsockname(fd, (struct sockaddr *)&sa, &slen) == 0);
		CHECK(sock_open(ntohs(sa.sin_port), 0) == SIP_HANDLE_NONE);
		close(fd);
	}

	transport_done();
}

/* ------------------------------------------------------------ send and rx */

static void test_roundtrip(void)
{
	struct sip_txn_dst dst;
	const char *reg = msg_register();
	uint32_t a, b;
	int n;

	sip_transport_init(&tp, &cb, NULL);
	a = sock_open(0, 0);
	b = sock_open(0, 0);
	CHECK(a != SIP_HANDLE_NONE && b != SIP_HANDLE_NONE);
	CHECK(sip_dst_set(&dst, "127.0.0.1", sock_port(b)));

	CASE("rx: a REGISTER arrives parsed, with the source address of the sender");
	rx_reset(1);
	n = sip_sock_send(&tp, a, 0, reg, strlen(reg), &dst);
	CHECK_UINT(n, strlen(reg));
	loop_pump(500);
	CHECK_UINT(rx.msgs, 1);
	CHECK(!strcmp(rx.method[0], "REGISTER"));
	CHECK_UINT(rx.len[0], strlen(reg));
	CHECK_UINT(rx.sock[0], b);
	CHECK(!strcmp(rx.src_host[0], "127.0.0.1"));

	CASE("rx: the source port is the port the sender listens on");
	CHECK_UINT(rx.src_port[0], sock_port(a));

	CASE("counters: one send and one receive, per socket and in the total");
	{
		const struct sip_sock_stat *total = sip_transport_stat(&tp);

		CHECK_UINT(sip_sock_get(&tp, a)->stat.tx, 1);
		CHECK_UINT(sip_sock_get(&tp, a)->stat.rx, 0);
		CHECK_UINT(sip_sock_get(&tp, b)->stat.rx, 1);
		CHECK_UINT(sip_sock_get(&tp, b)->stat.rx_bytes, strlen(reg));
		CHECK_UINT(total->tx, 1);
		CHECK_UINT(total->rx, 1);
		CHECK_UINT(total->tx_err, 0);
	}

	CASE("rx: a CRLF keepalive is a ping and not a parse error");
	rx_reset(1);
	CHECK(sip_sock_send(&tp, a, 0, "\r\n\r\n", 4, &dst) == 4);
	loop_pump(500);
	CHECK_UINT(rx.pings, 1);
	CHECK_UINT(rx.msgs, 0);
	CHECK_UINT(sip_sock_get(&tp, b)->stat.ping, 1);
	CHECK_UINT(sip_sock_get(&tp, b)->stat.parse_err, 0);

	/*
	 * RFC 5626 4.4.1 puts the whole difference between a ping and a pong in
	 * the length, so the receiver cannot tell them apart without it: four
	 * bytes is a ping and is answered, two bytes is a pong and is not.
	 */
	CASE("rx: the four byte ping reports its length to the callback");
	CHECK_UINT(rx.ping_len[0], 4);

	CASE("rx: a two byte pong is a ping callback carrying length two");
	rx_reset(1);
	CHECK(sip_sock_send(&tp, a, 0, "\r\n", 2, &dst) == 2);
	loop_pump(500);
	CHECK_UINT(rx.pings, 1);
	CHECK_UINT(rx.msgs, 0);
	CHECK_UINT(rx.ping_len[0], 2);
	CHECK_UINT(sip_sock_get(&tp, b)->stat.ping, 2);
	CHECK_UINT(sip_sock_get(&tp, b)->stat.parse_err, 0);

	CASE("rx: the two lengths arrive in order when both are sent");
	rx_reset(2);
	CHECK(sip_sock_send(&tp, a, 0, "\r\n\r\n", 4, &dst) == 4);
	CHECK(sip_sock_send(&tp, a, 0, "\r\n", 2, &dst) == 2);
	loop_pump(500);
	CHECK_UINT(rx.pings, 2);
	CHECK_UINT(rx.ping_len[0], 4);
	CHECK_UINT(rx.ping_len[1], 2);

	CASE("rx: an unparsable datagram is counted and never dispatched");
	rx_reset(0);
	{
		static const char junk[] = "\x01\x02 not sip\r\n\r\n";

		CHECK(sip_sock_send(&tp, a, 0, junk, sizeof(junk) - 1, &dst) > 0);
	}
	loop_pump(120);
	CHECK_UINT(rx.msgs, 0);
	CHECK_UINT(rx.pings, 0);
	CHECK_UINT(sip_sock_get(&tp, b)->stat.parse_err, 1);
	CHECK_UINT(sip_transport_stat(&tp)->parse_err, 1);

	CASE("rx: several datagrams in one readiness callback all arrive");
	rx_reset(3);
	CHECK(sip_sock_send(&tp, a, 0, reg, strlen(reg), &dst) > 0);
	CHECK(sip_sock_send(&tp, a, 0, reg, strlen(reg), &dst) > 0);
	CHECK(sip_sock_send(&tp, a, 0, reg, strlen(reg), &dst) > 0);
	loop_pump(500);
	CHECK_UINT(rx.msgs, 3);

	CASE("tx: a destination of the other family is refused and counted");
	{
		struct sip_txn_dst v6;
		uint64_t before = sip_sock_get(&tp, a)->stat.tx_err;

		CHECK(sip_dst_set(&v6, "::1", 5060));
		CHECK(sip_sock_send(&tp, a, 0, reg, strlen(reg), &v6) < 0);
		CHECK_UINT(sip_sock_get(&tp, a)->stat.tx_err, before + 1);
	}

	CASE("tx: a dead handle and an empty destination both refuse");
	{
		struct sip_txn_dst empty;

		memset(&empty, 0, sizeof(empty));
		CHECK(sip_sock_send(&tp, a, 0, reg, strlen(reg), &empty) < 0);
		CHECK(sip_sock_send(&tp, SIP_HANDLE_NONE, 0, reg, strlen(reg), &dst) < 0);
		CHECK(sip_sock_send(&tp, 0xdeadbeef, 0, reg, strlen(reg), &dst) < 0);
	}

	transport_done();
}

/* ------------------------------------------------------------------- tos */

static void test_tos(void)
{
	struct sip_udp_env ue_a, ue_b;
	struct sip_txn_dst dst;
	const char *reg = msg_register();
	uint32_t a, b;

	sip_transport_init(&tp, &cb, NULL);
	a = sock_open(0, 0);
	b = sock_open(0, 0);
	CHECK(sip_dst_set(&dst, "127.0.0.1", sock_port(b)));

	CASE("tos: the byte is on the socket before the datagram leaves");
	rx_reset(1);
	CHECK(sip_sock_send(&tp, a, 0x60, reg, strlen(reg), &dst) > 0);
	CHECK_UINT(tos_read(a), 0x60);
	loop_pump(500);
	CHECK_UINT(rx.msgs, 1);

	CASE("tos: a second send with the same byte leaves it alone");
	CHECK(sip_sock_send(&tp, a, 0x60, reg, strlen(reg), &dst) > 0);
	CHECK_UINT(tos_read(a), 0x60);
	CHECK_UINT(sip_sock_get(&tp, a)->tos_cur, 0x60);

	CASE("tos: two accounts on one socket each get their own byte per send");
	sip_udp_env_init(&ue_a, &tp, a, 0x60);
	sip_udp_env_init(&ue_b, &tp, a, 0xb8);
	CHECK(ue_a.env.send(&ue_a.env, reg, strlen(reg), &dst) > 0);
	CHECK_UINT(tos_read(a), 0x60);
	CHECK(ue_b.env.send(&ue_b.env, reg, strlen(reg), &dst) > 0);
	CHECK_UINT(tos_read(a), 0xb8);
	CHECK(ue_a.env.send(&ue_a.env, reg, strlen(reg), &dst) > 0);
	CHECK_UINT(tos_read(a), 0x60);

	CASE("tos: a changed byte follows the account, not the socket order");
	sip_udp_env_tos_set(&ue_a, 0x88);
	CHECK(ue_a.env.send(&ue_a.env, reg, strlen(reg), &dst) > 0);
	CHECK_UINT(tos_read(a), 0x88);

	CASE("env: the environment carries the uloop clock and timer implementation");
	CHECK(ue_a.env.base.now_ms != NULL);
	CHECK(ue_a.env.base.timer_set != NULL);
	CHECK(ue_a.env.base.timer_cancel != NULL);
	CHECK(ue_a.env.base.trace != NULL);
	CHECK(ue_a.env.base.now_ms(&ue_a.env.base) > 0);
	CHECK(ue_a.env.base.priv == &ue_a);

	CASE("env: a rebound socket handle is followed by the environment");
	sip_udp_env_sock_set(&ue_a, b);
	CHECK_UINT(ue_a.sock, b);

	transport_done();
}

/* ------------------------------------------------------------- truncation */

static void test_truncation(void)
{
	struct sip_txn_dst dst;
	char big[2048];
	char exact[600];
	uint32_t a, b;
	size_t pad;
	int n;

	sip_transport_init(&tp, &cb, NULL);
	a = sock_open(0, 0);
	b = sock_open(0, 512);
	CHECK(sip_dst_set(&dst, "127.0.0.1", sock_port(b)));

	CASE("truncation: a datagram over the limit is dropped and counted");
	rx_reset(0);
	memset(big, 'x', sizeof(big));
	memcpy(big, "OPTIONS sip:x SIP/2.0\r\n", 23);
	CHECK(sip_sock_send(&tp, a, 0, big, sizeof(big), &dst) > 0);
	loop_pump(120);
	CHECK_UINT(rx.msgs, 0);
	CHECK_UINT(sip_sock_get(&tp, b)->stat.truncated, 1);
	CHECK_UINT(sip_sock_get(&tp, b)->stat.rx, 0);
	CHECK_UINT(sip_transport_stat(&tp)->truncated, 1);

	CASE("truncation: the socket keeps working after a dropped datagram");
	rx_reset(1);
	CHECK(sip_sock_send(&tp, a, 0, msg_register(), strlen(msg_register()), &dst) > 0);
	loop_pump(500);
	CHECK_UINT(rx.msgs, 1);

	CASE("truncation: a datagram of exactly the limit is accepted");
	n = snprintf(exact, sizeof(exact),
		     "OPTIONS sip:x SIP/2.0\r\n"
		     "Via: SIP/2.0/UDP 127.0.0.1;branch=z9hG4bK1;rport\r\n"
		     "Call-ID: pad\r\n"
		     "CSeq: 1 OPTIONS\r\n"
		     "X-Pad: ");
	CHECK(n > 0 && (size_t)n < 512);
	pad = 512 - (size_t)n - 4;
	memset(exact + n, 'p', pad);
	memcpy(exact + n + pad, "\r\n\r\n", 4);
	rx_reset(1);
	CHECK(sip_sock_send(&tp, a, 0, exact, 512, &dst) == 512);
	loop_pump(500);
	CHECK_UINT(rx.msgs, 1);
	CHECK_UINT(rx.len[0], 512);
	CHECK_UINT(sip_sock_get(&tp, b)->stat.truncated, 1);

	CASE("truncation: one byte over the limit is dropped");
	rx_reset(0);
	CHECK(sip_sock_send(&tp, a, 0, exact, 513, &dst) == 513);
	loop_pump(120);
	CHECK_UINT(rx.msgs, 0);
	CHECK_UINT(sip_sock_get(&tp, b)->stat.truncated, 2);

	CASE("truncation: an rx_max above the buffer is clamped to the buffer");
	{
		uint32_t c = sip_sock_open(&tp, &(struct sip_sock_cfg){
						.family = AF_INET,
						.addr = "127.0.0.1",
						.rx_max = 1024 * 1024 });

		CHECK(c != SIP_HANDLE_NONE);
		CHECK_UINT(sip_sock_get(&tp, c)->cfg.rx_max, SIP_RX_BUF_MAX);
	}

	transport_done();
}

/* ---------------------------------------------------- via, rebind, device */

static void test_via_rebind(void)
{
	struct sip_txn_dst dst;
	struct sip_buf sb;
	char buf[256];
	char host[64];
	uint32_t a, b;
	uint16_t port, port_a, port_b;
	int hold_fd = -1;
	int hold_b = -1;

	sip_transport_init(&tp, &cb, NULL);
	a = sock_open(0, 0);
	b = sock_open(0, 0);
	CHECK(sip_dst_set(&dst, "127.0.0.1", sock_port(b)));

	CASE("via: the egress Via always carries rport");
	sip_buf_init(&sb, buf, sizeof(buf));
	CHECK(sip_sock_via_write(&tp, a, &sb, &dst, "z9hG4bK-1"));
	CHECK(strstr(sip_buf_str(&sb), ";rport") != NULL);
	CHECK(strstr(sip_buf_str(&sb), "SIP/2.0/UDP 127.0.0.1:") != NULL);
	CHECK(strstr(sip_buf_str(&sb), ";branch=z9hG4bK-1") != NULL);
	{
		char want[32];

		snprintf(want, sizeof(want), ":%u;", sock_port(a));
		CHECK(strstr(sip_buf_str(&sb), want) != NULL);
	}

	CASE("via: a dead handle writes nothing");
	sip_buf_init(&sb, buf, sizeof(buf));
	CHECK(!sip_sock_via_write(&tp, 0xdeadbeef, &sb, &dst, "z9hG4bK-1"));
	CHECK_UINT(sip_buf_len(&sb), 0);

	CASE("source: a socket bound to an address answers with that address");
	CHECK(sip_sock_source_host(sip_sock_get(&tp, a), &dst, host, sizeof(host)));
	CHECK(!strcmp(host, "127.0.0.1"));

	CASE("source: a wildcard socket asks the kernel which address a route takes");
	{
		uint32_t w = sip_sock_open(&tp, &(struct sip_sock_cfg){
						.family = AF_INET });

		CHECK(w != SIP_HANDLE_NONE);
		CHECK(sip_sock_local_host(sip_sock_get(&tp, w), host, sizeof(host)));
		CHECK(!strcmp(host, "0.0.0.0"));
		CHECK(sip_sock_source_host(sip_sock_get(&tp, w), &dst, host,
					   sizeof(host)));
		CHECK(!strcmp(host, "127.0.0.1"));
		sip_sock_close(&tp, w);
	}

	CASE("rebind: a named port survives, the handle survives, the socket receives");
	transport_done();
	sip_transport_init(&tp, &cb, NULL);
	a = sock_open(0, 0);
	b = sock_open(0, 0);
	port = sock_port(b);
	CHECK(sip_dst_set(&dst, "127.0.0.1", port));
	rx_reset(1);
	CHECK(sip_sock_send(&tp, a, 0, msg_register(), strlen(msg_register()), &dst) > 0);
	loop_pump(500);
	CHECK_UINT(rx.msgs, 1);

	{
		struct sip_sock *sk = sip_sock_get(&tp, b);
		struct sip_sock_cfg cfg = sk->cfg;

		/*
		 * The ephemeral bind is turned into a named one first: an
		 * interface address change reopens the socket, and a caller
		 * that wants its port back has to have named it.
		 */
		cfg.port = port;
		sk->cfg = cfg;
	}
	CHECK(sip_sock_rebind(&tp, b));
	CHECK(sip_sock_get(&tp, b) != NULL);
	CHECK_UINT(sock_port(b), port);
	CHECK_UINT(sip_sock_get(&tp, b)->stat.rebinds, 1);
	CHECK(sip_sock_up(sip_sock_get(&tp, b)));

	rx_reset(1);
	CHECK(sip_sock_send(&tp, a, 0, msg_register(), strlen(msg_register()), &dst) > 0);
	loop_pump(500);
	CHECK_UINT(rx.msgs, 1);

	CASE("rebind: rebind_all touches every open socket");
	CHECK_UINT(sip_transport_rebind_all(&tp), 0);
	CHECK_UINT(sip_sock_get(&tp, b)->stat.rebinds, 2);
	CHECK_UINT(sip_sock_get(&tp, a)->stat.rebinds, 1);
	CHECK_UINT(sip_transport_stat(&tp)->rebinds, 3);

	CASE("rebind: a port another socket holds leaves this one down, tx_err counted");
	{
		struct sip_sock *sk = sip_sock_get(&tp, b);
		uint16_t taken = port_take(&hold_fd);
		uint64_t tx_err0;

		CHECK(taken != 0);
		sk->cfg.port = taken;
		CHECK(!sip_sock_rebind(&tp, b));
		CHECK(!sip_sock_up(sip_sock_get(&tp, b)));

		tx_err0 = sip_sock_get(&tp, b)->stat.tx_err;
		CHECK(sip_sock_send(&tp, b, 0, "x", 1, &dst) < 0);
		CHECK_UINT(sip_sock_get(&tp, b)->stat.tx_err, tx_err0 + 1);
		close(hold_fd);
	}

	CASE("rebind: a dead handle is refused, and a dead-handle send counts a tx_err");
	CHECK(!sip_sock_rebind(&tp, 0xdeadbeef));
	{
		uint64_t total0 = sip_transport_stat(&tp)->tx_err;

		CHECK(sip_sock_send(&tp, 0xdeadbeef, 0, "x", 1, &dst) < 0);
		CHECK_UINT(sip_transport_stat(&tp)->tx_err, total0 + 1);
	}

	/*
	 * The recovery pass of the account layer. Each case here puts a socket
	 * down with the technique above, a configured port that another socket
	 * holds, and then watches which sockets the pass touches. A rebind of a
	 * working socket costs it the conntrack tuple of its flow, so the count
	 * of rebinds of the healthy socket is what these cases guard.
	 */
	transport_done();
	sip_transport_init(&tp, &cb, NULL);

	/* Both probes are held at once, so the kernel cannot answer twice with
	 * the same port and leave the two sockets sharing one key. */
	port_a = port_take(&hold_fd);
	port_b = port_take(&hold_b);
	CHECK(port_a != 0);
	CHECK(port_b != 0);
	close(hold_fd);
	close(hold_b);
	a = sock_open(port_a, 0);
	b = sock_open(port_b, 0);
	CHECK(a != SIP_HANDLE_NONE);
	CHECK(b != SIP_HANDLE_NONE);
	CHECK(a != b);

	CASE("rebind down: a healthy socket is left alone and the down one comes back");
	{
		uint64_t rebinds_a = sip_sock_get(&tp, a)->stat.rebinds;
		uint16_t taken = port_take(&hold_fd);

		CHECK(taken != 0);
		sip_sock_get(&tp, b)->cfg.port = taken;
		CHECK(!sip_sock_rebind(&tp, b));
		CHECK(!sip_sock_up(sip_sock_get(&tp, b)));

		/* the port is free again and the socket asks for its own */
		close(hold_fd);
		sip_sock_get(&tp, b)->cfg.port = port_b;

		CHECK_UINT(sip_transport_rebind_down(&tp), 0);
		CHECK(sip_sock_up(sip_sock_get(&tp, b)));
		CHECK_UINT(sock_port(b), port_b);
		CHECK(sip_sock_up(sip_sock_get(&tp, a)));
		CHECK_UINT(sip_sock_get(&tp, a)->stat.rebinds, rebinds_a);
		CHECK_UINT(sock_port(a), port_a);
	}

	CASE("rebind down: the count of the sockets still down is what it returns");
	{
		uint16_t held_a = port_take(&hold_fd);
		uint16_t held_b = port_take(&hold_b);

		CHECK(held_a != 0);
		CHECK(held_b != 0);

		/* two sockets down, and both ports are still held */
		sip_sock_get(&tp, a)->cfg.port = held_a;
		sip_sock_get(&tp, b)->cfg.port = held_b;
		CHECK(!sip_sock_rebind(&tp, a));
		CHECK(!sip_sock_rebind(&tp, b));
		CHECK_UINT(sip_transport_rebind_down(&tp), 2);
		CHECK(!sip_sock_up(sip_sock_get(&tp, a)));
		CHECK(!sip_sock_up(sip_sock_get(&tp, b)));

		/* one comes back, the other keeps its occupied port */
		close(hold_fd);
		sip_sock_get(&tp, a)->cfg.port = port_a;
		CHECK_UINT(sip_transport_rebind_down(&tp), 1);
		CHECK(sip_sock_up(sip_sock_get(&tp, a)));
		CHECK_UINT(sock_port(a), port_a);
		CHECK(!sip_sock_up(sip_sock_get(&tp, b)));

		close(hold_b);
		sip_sock_get(&tp, b)->cfg.port = port_b;
		CHECK_UINT(sip_transport_rebind_down(&tp), 0);
		CHECK(sip_sock_up(sip_sock_get(&tp, b)));
	}

	CASE("rebind down: a pool with nothing down rebinds nothing");
	{
		uint64_t total = sip_transport_stat(&tp)->rebinds;

		CHECK_UINT(sip_transport_down_count(&tp), 0);
		CHECK_UINT(sip_transport_rebind_down(&tp), 0);
		CHECK_UINT(sip_transport_stat(&tp)->rebinds, total);
		CHECK(sip_sock_up(sip_sock_get(&tp, a)));
		CHECK(sip_sock_up(sip_sock_get(&tp, b)));
	}

	CASE("down count: an open pool reports the sockets a failed rebind left down");
	{
		uint16_t taken = port_take(&hold_fd);

		CHECK(taken != 0);
		CHECK_UINT(sip_transport_down_count(&tp), 0);

		sip_sock_get(&tp, b)->cfg.port = taken;
		CHECK(!sip_sock_rebind(&tp, b));
		CHECK_UINT(sip_transport_down_count(&tp), 1);

		sip_sock_get(&tp, a)->cfg.port = taken;
		CHECK(!sip_sock_rebind(&tp, a));
		CHECK_UINT(sip_transport_down_count(&tp), 2);

		/* a closed socket leaves the pool and is counted no more */
		sip_sock_close(&tp, a);
		CHECK_UINT(sip_transport_down_count(&tp), 1);

		close(hold_fd);
		sip_sock_get(&tp, b)->cfg.port = port_b;
		CHECK_UINT(sip_transport_rebind_down(&tp), 0);
		CHECK_UINT(sip_transport_down_count(&tp), 0);
	}

	transport_done();
}

static void test_bindtodevice(void)
{
	struct sip_sock_cfg cfg;
	uint32_t a;

	CASE("iface: SO_BINDTODEVICE is applied, or the refusal is reported");
	sip_transport_init(&tp, &cb, NULL);
	memset(&cfg, 0, sizeof(cfg));
	cfg.family = AF_INET;
	cfg.addr = "127.0.0.1";
	cfg.iface = "lo";
	errno = 0;
	a = sip_sock_open(&tp, &cfg);

	if (a != SIP_HANDLE_NONE) {
		struct sip_sock *sk = sip_sock_get(&tp, a);
		char name[SIP_SOCK_IFACE_MAX] = "";
		socklen_t len = sizeof(name);

		CHECK(!strcmp(sip_sock_iface(sk), "lo"));
		CHECK(getsockopt(sk->ufd.fd, SOL_SOCKET, SO_BINDTODEVICE, name,
				 &len) == 0);
		CHECK(!strcmp(name, "lo"));
		printf("    note: SO_BINDTODEVICE is available in this environment\n");
	} else {
		CHECK(errno == EPERM || errno == EACCES || errno == ENODEV);
		CHECK_UINT(sip_sock_count(&tp), 0);
		printf("    note: SO_BINDTODEVICE refused with errno %d, the refusal "
		       "path is what this case covers\n", errno);
	}

	CASE("iface: an interface name that does not fit is refused before the socket");
	memset(&cfg, 0, sizeof(cfg));
	cfg.family = AF_INET;
	cfg.iface = "an-interface-name-far-too-long";
	CHECK(sip_sock_open(&tp, &cfg) == SIP_HANDLE_NONE);
	CHECK_UINT(errno, EINVAL);

	CASE("iface: the interface is part of the socket key");
	{
		uint32_t x, y;

		transport_done();
		sip_transport_init(&tp, &cb, NULL);
		memset(&cfg, 0, sizeof(cfg));
		cfg.family = AF_INET;
		cfg.addr = "127.0.0.1";
		cfg.port = free_port();
		CHECK(cfg.port != 0);
		x = sip_sock_open(&tp, &cfg);
		CHECK(x != SIP_HANDLE_NONE);

		y = sip_sock_open(&tp, &cfg);
		CHECK_UINT(y, x);

		cfg.iface = "lo";
		CHECK(sip_sock_open(&tp, &cfg) != y);
	}

	transport_done();
}

/*
 * The device that a socket scopes to must be replaceable. Reason: netifd can
 * move the logical interface onto another netdev, and a rebind reuses the
 * device that the socket was opened with. The cases run on real loopback
 * sockets. The setter writes the name, and the next rebind applies it. A name
 * that the kernel does not know makes that rebind fail and leaves the socket
 * down, as the header promises the caller.
 */
static void test_iface_set(void)
{
	struct sip_txn_dst dst;
	struct sip_sock_cfg cfg;
	struct sip_sock *sk;
	uint32_t peer, s;
	uint16_t port;

	sip_transport_init(&tp, &cb, NULL);
	peer = sock_open(0, 0);
	CHECK(peer != SIP_HANDLE_NONE);

	memset(&cfg, 0, sizeof(cfg));
	cfg.family = AF_INET;
	cfg.addr = "127.0.0.1";
	cfg.port = free_port();
	cfg.iface = "lo";

	s = sip_sock_open(&tp, &cfg);
	if (s == SIP_HANDLE_NONE) {
		/* SO_BINDTODEVICE is refused in this environment (the
		 * test_bindtodevice note covers it). Every promise below except
		 * the opening scope still holds on an unscoped socket. */
		cfg.iface = NULL;
		s = sip_sock_open(&tp, &cfg);
		printf("    note: SO_BINDTODEVICE refused, the setter runs unscoped\n");
	}
	CHECK(s != SIP_HANDLE_NONE);
	CHECK(sip_dst_set(&dst, "127.0.0.1", cfg.port));

	CASE("iface set: a dead handle and a name that does not fit are refused");
	CHECK(!sip_sock_iface_set(&tp, 0xdeadbeef, "lo"));
	CHECK(!sip_sock_iface_set(&tp, s, "an-interface-name-far-too-long"));

	CASE("iface set: clearing the device leaves the running socket alone");
	sk = sip_sock_get(&tp, s);
	CHECK(sip_sock_up(sk));
	CHECK(sip_sock_iface_set(&tp, s, ""));
	CHECK(!sip_sock_iface(sk)[0]);
	CHECK(sip_sock_up(sk));

	CASE("iface set: the next rebind takes it unscoped, keeps the port and receives");
	port = sock_port(s);
	CHECK(sip_sock_rebind(&tp, s));
	CHECK(sip_sock_up(sip_sock_get(&tp, s)));
	CHECK_UINT(sock_port(s), port);
	rx_reset(1);
	CHECK(sip_sock_send(&tp, peer, 0, msg_register(), strlen(msg_register()), &dst) > 0);
	loop_pump(500);
	CHECK_UINT(rx.msgs, 1);

	CASE("iface set: a device the kernel does not know fails the rebind, socket down");
	CHECK(sip_sock_iface_set(&tp, s, "uvoice-no-dev"));
	CHECK(sip_sock_up(sip_sock_get(&tp, s)));	/* still up, nothing rebound */
	CHECK(!sip_sock_rebind(&tp, s));
	CHECK(!sip_sock_up(sip_sock_get(&tp, s)));

	CASE("iface set: setting it back recovers the socket on the next rebind");
	CHECK(sip_sock_iface_set(&tp, s, NULL));
	CHECK(sip_sock_rebind(&tp, s));
	CHECK(sip_sock_up(sip_sock_get(&tp, s)));
	CHECK_UINT(sock_port(s), port);
	rx_reset(1);
	CHECK(sip_sock_send(&tp, peer, 0, msg_register(), strlen(msg_register()), &dst) > 0);
	loop_pump(500);
	CHECK_UINT(rx.msgs, 1);

	CASE("iface set: the same name again is a no-op that still reports success");
	CHECK(sip_sock_iface_set(&tp, s, ""));
	CHECK(sip_sock_up(sip_sock_get(&tp, s)));

	transport_done();
}

/*
 * This group uses real ::1 sockets. A host without IPv6 loopback skips every
 * case with the probe's reason. A run in a network namespace with IPv6
 * loopback runs these cases.
 */
static void test_ipv6(void)
{
	struct sip_udp_env ue_a, ue_b;
	struct sip_txn_dst dst;
	struct sip_sock *sk;
	struct sockaddr_in6 sa;
	socklen_t slen = sizeof(sa);
	const char *reg = msg_register();
	uint32_t a = SIP_HANDLE_NONE;
	uint32_t b = SIP_HANDLE_NONE;
	uint32_t c = SIP_HANDLE_NONE;
	char why[128];
	char host[64];
	uint16_t port = 0;
	bool v6 = tu_v6_loopback(why, sizeof(why));

	sip_transport_init(&tp, &cb, NULL);

	CASE("open6: an ephemeral ::1 bind reports the real port through "
	     "getsockname");
	if (!v6) {
		tu_skip(why);
	} else {
		a = sock_open_af(AF_INET6, "::1", 0, 0);
		CHECK(a != SIP_HANDLE_NONE);
		sk = sip_sock_get(&tp, a);
		CHECK(sk != NULL);
		if (sk) {
			CHECK(sip_sock_port(sk) != 0);
			CHECK(sip_sock_up(sk));
			CHECK(sip_sock_local_host(sk, host, sizeof(host)));
			CHECK(!strcmp(host, "::1"));
			CHECK(getsockname(sk->ufd.fd, (struct sockaddr *)&sa,
					  &slen) == 0);
			CHECK_UINT(sa.sin6_family, AF_INET6);
			CHECK_UINT(sip_sock_port(sk), ntohs(sa.sin6_port));
			CHECK(IN6_IS_ADDR_LOOPBACK(&sa.sin6_addr));
		}
		CHECK_UINT(sip_sock_count(&tp), 1);
	}

	CASE("open6: a named v6 port that two accounts share is one socket, "
	     "two refs");
	if (!v6) {
		tu_skip(why);
	} else {
		port = free_port6();
		CHECK(port != 0);
		b = sock_open_af(AF_INET6, "::1", port, 0);
		CHECK(b != SIP_HANDLE_NONE);
		CHECK_UINT(sock_port(b), port);
		CHECK_UINT(sock_open_af(AF_INET6, "::1", port, 0), b);
		CHECK_UINT(sip_sock_count(&tp), 2);
		sk = sip_sock_get(&tp, b);
		CHECK(sk != NULL);
		if (sk)
			CHECK_UINT(sk->refs, 2);
	}

	CASE("open6: a v4 and a v6 socket on the same port number are two "
	     "sockets");
	if (!v6) {
		tu_skip(why);
	} else {
		c = sock_open(port, 0);
		CHECK(c != SIP_HANDLE_NONE);
		CHECK(c != b);
		CHECK_UINT(sock_port(c), sock_port(b));
		CHECK_UINT(sip_sock_count(&tp), 3);
	}

	CASE("open6: a v4 and a v6 wildcard socket on one port are two "
	     "sockets");
	if (!v6) {
		tu_skip(why);
	} else {
		unsigned int count = sip_sock_count(&tp);
		uint16_t p = free_port();
		uint32_t w4, w6;

		CHECK(p != 0);
		w4 = sock_open_af(AF_INET, NULL, p, 0);
		CHECK(w4 != SIP_HANDLE_NONE);
		w6 = sock_open_af(AF_INET6, NULL, p, 0);
		CHECK(w6 != SIP_HANDLE_NONE);
		CHECK(w4 != w6);
		CHECK_UINT(sip_sock_count(&tp), count + 2);
		sip_sock_close(&tp, w6);
		sip_sock_close(&tp, w4);
	}

	CASE("open6: IPV6_V6ONLY is set on every v6 socket");
	if (!v6) {
		tu_skip(why);
	} else {
		uint32_t w = sock_open_af(AF_INET6, NULL, 0, 0);

		CHECK(w != SIP_HANDLE_NONE);
		/*
		 * The kernel implies IPV6_V6ONLY for a non-wildcard ::1 bind,
		 * regardless of sock_fd_open(). The wildcard assertion reads
		 * the option that sock_fd_open() sets.
		 */
		CHECK_UINT(v6only_read(a), 1);
		CHECK_UINT(v6only_read(b), 1);
		CHECK_UINT(v6only_read(w), 1);
		sip_sock_close(&tp, w);
	}

	CASE("rx6: a REGISTER arrives parsed, with ::1 as the source address");
	if (!v6) {
		tu_skip(why);
	} else {
		CHECK(sip_dst_set(&dst, "::1", sock_port(b)));
		rx_reset(1);
		CHECK_UINT(sip_sock_send(&tp, a, 0, reg, strlen(reg), &dst),
			   strlen(reg));
		loop_pump(500);
		CHECK_UINT(rx.msgs, 1);
		CHECK(!strcmp(rx.method[0], "REGISTER"));
		CHECK_UINT(rx.len[0], strlen(reg));
		CHECK_UINT(rx.sock[0], b);
		CHECK(!strcmp(rx.src_host[0], "::1"));
	}

	CASE("rx6: the source port is the port the v6 sender listens on");
	if (!v6) {
		tu_skip(why);
	} else {
		CHECK_UINT(rx.msgs, 1);
		CHECK(sock_port(a) != 0);
		CHECK_UINT(rx.src_port[0], sock_port(a));
	}

	CASE("tx6: a v4 destination on a v6 socket is refused and counted");
	if (!v6) {
		tu_skip(why);
	} else {
		struct sip_txn_dst v4;
		uint64_t total_err = sip_transport_stat(&tp)->tx_err;
		uint64_t total_tx = sip_transport_stat(&tp)->tx;

		CHECK(sip_dst_set(&v4, "127.0.0.1", sock_port(c)));
		sk = sip_sock_get(&tp, b);
		CHECK(sk != NULL);
		if (sk) {
			CHECK_UINT(sk->tos_cur, SIP_TOS_UNSET);
			CHECK_UINT(sk->stat.tx_err, 0);
			CHECK_UINT(tos6_read(b), 0);
			CHECK(sip_sock_send(&tp, b, 0x60, reg, strlen(reg),
					    &v4) < 0);
			CHECK_UINT(sk->stat.tx_err, 1);
			CHECK_UINT(sk->stat.tx, 0);
			CHECK_UINT(sk->stat.tx_bytes, 0);
			/*
			 * The kernel also refuses this send, so counters alone
			 * cannot tell the guard from the kernel. These two TOS
			 * assertions can: the guard runs before tos_apply(), and
			 * the kernel refuses the send after it.
			 */
			CHECK_UINT(sk->tos_cur, SIP_TOS_UNSET);
			CHECK_UINT(tos6_read(b), 0);
		}
		CHECK_UINT(sip_transport_stat(&tp)->tx_err, total_err + 1);
		CHECK_UINT(sip_transport_stat(&tp)->tx, total_tx);
	}

	CASE("tos6: the byte is on the v6 socket as IPV6_TCLASS before the "
	     "datagram leaves");
	if (!v6) {
		tu_skip(why);
	} else {
		rx_reset(1);
		CHECK(sip_sock_send(&tp, a, 0x60, reg, strlen(reg), &dst) > 0);
		CHECK_UINT(tos6_read(a), 0x60);
		loop_pump(500);
		CHECK_UINT(rx.msgs, 1);
	}

	CASE("tos6: two accounts on one v6 socket each get their own byte per "
	     "send");
	if (!v6) {
		tu_skip(why);
	} else {
		sip_udp_env_init(&ue_a, &tp, b, 0x60);
		sip_udp_env_init(&ue_b, &tp, b, 0xb8);
		CHECK(sip_dst_set(&dst, "::1", sock_port(a)));
		rx_reset(3);
		CHECK(ue_a.env.send(&ue_a.env, reg, strlen(reg), &dst) > 0);
		CHECK_UINT(tos6_read(b), 0x60);
		CHECK(ue_b.env.send(&ue_b.env, reg, strlen(reg), &dst) > 0);
		CHECK_UINT(tos6_read(b), 0xb8);
		CHECK(ue_a.env.send(&ue_a.env, reg, strlen(reg), &dst) > 0);
		CHECK_UINT(tos6_read(b), 0x60);
		loop_pump(500);
		CHECK_UINT(rx.msgs, 3);
	}

	CASE("via6: the egress Via of a v6 socket brackets the sent-by and "
	     "carries rport");
	if (!v6) {
		tu_skip(why);
	} else {
		struct sip_buf sb;
		char buf[256];
		char want[128];

		sip_buf_init(&sb, buf, sizeof(buf));
		CHECK(sip_sock_via_write(&tp, b, &sb, &dst, "z9hG4bK-6"));
		snprintf(want, sizeof(want), "Via: SIP/2.0/UDP [::1]:%u"
			 ";branch=z9hG4bK-6;rport\r\n", sock_port(b));
		CHECK(!strcmp(sip_buf_str(&sb), want));
	}

	CASE("source6: a wildcard v6 socket asks the kernel which address "
	     "a route takes");
	if (!v6) {
		tu_skip(why);
	} else {
		uint32_t w = sock_open_af(AF_INET6, NULL, 0, 0);

		CHECK(w != SIP_HANDLE_NONE);
		sk = sip_sock_get(&tp, w);
		CHECK(sk != NULL);
		if (sk) {
			CHECK(sip_sock_local_host(sk, host, sizeof(host)));
			CHECK(!strcmp(host, "::"));
			CHECK(sip_sock_source_host(sk, &dst, host,
					   sizeof(host)));
			CHECK(!strcmp(host, "::1"));
		}
		sip_sock_close(&tp, w);
	}

	CASE("rebind6: a named v6 port survives, the handle survives, the "
	     "socket receives");
	if (!v6) {
		tu_skip(why);
	} else {
		CHECK(sip_sock_rebind(&tp, b));
		sk = sip_sock_get(&tp, b);
		CHECK(sk != NULL);
		if (sk) {
			CHECK_UINT(sip_sock_handle(sk), b);
			CHECK_UINT(sip_sock_port(sk), port);
			CHECK(sip_sock_up(sk));
			CHECK_UINT(sk->refs, 2);
			CHECK_UINT(sk->stat.rebinds, 1);
			CHECK_UINT(sk->tos_cur, SIP_TOS_UNSET);
			CHECK_UINT(v6only_read(b), 1);
		}
		CHECK_UINT(sip_transport_stat(&tp)->rebinds, 1);
		CHECK(sip_dst_set(&dst, "::1", port));
		rx_reset(1);
		CHECK(sip_sock_send(&tp, a, 0, reg, strlen(reg), &dst) > 0);
		loop_pump(500);
		CHECK_UINT(rx.msgs, 1);
		CHECK_UINT(rx.sock[0], b);
		CHECK(!strcmp(rx.method[0], "REGISTER"));
		CHECK(!strcmp(rx.src_host[0], "::1"));
		CHECK_UINT(rx.src_port[0], sock_port(a));
	}

	CASE("iface6: SO_BINDTODEVICE on lo is applied to a v6 socket, or the "
	     "refusal is reported");
	if (!v6) {
		tu_skip(why);
	} else {
		struct sip_sock_cfg cfg = {
			.family = AF_INET6,
			.addr = "::1",
			.iface = "lo",
		};
		unsigned int count = sip_sock_count(&tp);
		uint32_t s;
		int err;

		errno = 0;
		s = sip_sock_open(&tp, &cfg);
		err = errno;
		if (s != SIP_HANDLE_NONE) {
			char name[SIP_SOCK_IFACE_MAX] = "";
			socklen_t len = sizeof(name);

			sk = sip_sock_get(&tp, s);
			CHECK(!strcmp(sip_sock_iface(sk), "lo"));
			CHECK(getsockopt(sk->ufd.fd, SOL_SOCKET,
					 SO_BINDTODEVICE, name, &len) == 0);
			CHECK(!strcmp(name, "lo"));
			CHECK_UINT(v6only_read(s), 1);
			CHECK_UINT(sip_sock_count(&tp), count + 1);
			printf("    note: IPv6 SO_BINDTODEVICE is available\n");
		} else {
			CHECK(err == EPERM || err == EACCES || err == ENODEV);
			CHECK_UINT(sip_sock_count(&tp), count);
			printf("    note: IPv6 SO_BINDTODEVICE refused with "
			       "errno %d, this case covers the refusal\n", err);
		}
	}

	transport_done();
}

static void test_tap_exchange(int family, const char *bound)
{
	const char *host = family == AF_INET6 ? "::1" : "127.0.0.1";
	const char *reg = msg_register();
	struct sip_txn_dst dst;
	char name[128], why[128];
	bool available = family != AF_INET6 ||
			 tu_v6_loopback(why, sizeof(why));
	uint32_t a = SIP_HANDLE_NONE, b = SIP_HANDLE_NONE;

	snprintf(name, sizeof(name), "tap udp%d tx: %s bind keeps bytes and "
		 "endpoints", family == AF_INET6 ? 6 : 4,
		 bound ? "loopback" : "wildcard");
	CASE(name);
	if (!available) {
		tu_skip(why);
	} else {
		sip_transport_init(&tp, &cb, NULL);
		a = sock_open_af(family, bound, 0, 0);
		b = sock_open_af(family, bound, 0, 0);
		CHECK(a != SIP_HANDLE_NONE && b != SIP_HANDLE_NONE);
		CHECK(sip_dst_set(&dst, host, sock_port(b)));
		rx_reset(1);
		tap_start();
		CHECK_UINT(sip_sock_send(&tp, a, 0, reg, strlen(reg), &dst),
			   strlen(reg));
		tap_check(true, reg, strlen(reg), family, bound, sock_port(a),
			  host, sock_port(b));
		sip_tap_set(NULL);
	}

	snprintf(name, sizeof(name), "tap udp%d rx: %s bind keeps bytes and "
		 "endpoints", family == AF_INET6 ? 6 : 4,
		 bound ? "loopback" : "wildcard");
	CASE(name);
	if (!available) {
		tu_skip(why);
		return;
	}

	tap_start();
	loop_pump(500);
	CHECK_UINT(rx.msgs, 1);
	tap_check(false, reg, strlen(reg), family, bound, sock_port(b),
		  host, sock_port(a));
	sip_tap_set(NULL);
	transport_done();
}

static void test_tap_refused(void)
{
	struct sip_txn_dst dst;
	struct sip_sock *sk;
	uint32_t a, b;

	CASE("tap udp tx: a down socket produces no tap call");
	sip_transport_init(&tp, &cb, NULL);
	a = sock_open(0, 0);
	b = sock_open(0, 0);
	CHECK(a != SIP_HANDLE_NONE && b != SIP_HANDLE_NONE);
	CHECK(sip_dst_set(&dst, "127.0.0.1", sock_port(a)));
	sk = sip_sock_get(&tp, b);
	CHECK(sk != NULL);
	if (!sk)
		goto out;

	sk->cfg.port = sock_port(a);
	CHECK(!sip_sock_rebind(&tp, b));
	CHECK(!sip_sock_up(sk));
	tap_start();
	CHECK(sip_sock_send(&tp, b, 0, "x", 1, &dst) < 0);
	CHECK_UINT(tap_log[true].count, 0);
	CHECK_UINT(tap_log[false].count, 0);
	sip_tap_set(NULL);
out:
	transport_done();
}

static void test_tap_ping(void)
{
	struct sip_txn_dst dst;
	uint32_t a, b;

	CASE("tap udp rx: a keepalive retains its wire bytes");
	sip_transport_init(&tp, &cb, NULL);
	a = sock_open(0, 0);
	b = sock_open(0, 0);
	CHECK(a != SIP_HANDLE_NONE && b != SIP_HANDLE_NONE);
	CHECK(sip_dst_set(&dst, "127.0.0.1", sock_port(b)));
	rx_reset(1);
	CHECK_UINT(sip_sock_send(&tp, a, 0, "\r\n\r\n", 4, &dst), 4);
	tap_start();
	loop_pump(500);
	CHECK_UINT(rx.pings, 1);
	CHECK_UINT(rx.msgs, 0);
	tap_check(false, "\r\n\r\n", 4, AF_INET, "127.0.0.1", sock_port(b),
		  "127.0.0.1", sock_port(a));
	sip_tap_set(NULL);
	transport_done();
}

int main(void)
{
	alarm(120);
	uloop_init();
	sip_wire_host = proto_host_fake();

	test_init_dirty();
	test_open();
	test_roundtrip();
	test_tos();
	test_truncation();
	test_via_rebind();
	test_bindtodevice();
	test_iface_set();
	test_ipv6();
	test_tap_exchange(AF_INET, "127.0.0.1");
	test_tap_exchange(AF_INET, NULL);
	test_tap_exchange(AF_INET6, "::1");
	test_tap_exchange(AF_INET6, NULL);
	test_tap_refused();
	test_tap_ping();

	sip_tap_set(NULL);

	uloop_done();

	return tu_summary("test_sip_transport");
}
