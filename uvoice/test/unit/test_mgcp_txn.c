// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the MGCP transaction layer (src/proto/mgcp/txn.c).
 *
 * The layer takes its clock, its timers and its entropy from struct
 * proto_timer_env and its transport from struct mgcp_txn_env. In this suite
 * the test moves a virtual clock by hand, a capture array replaces the
 * socket and a fixed rand32 makes every random draw known. Each RFC 3435
 * section 3.5, 4.3 and 4.4.7 timer is therefore observable to the
 * millisecond, and no case sleeps.
 */
#include <stdarg.h>
#include <stdlib.h>

#include "proto/mgcp/txn.h"
#include "test_util.h"

#define FAKE_TIMER_MAX		256
#define FAKE_SEND_MAX		128
#define FAKE_WIRE_MAX		4200
#define LOG_MAX			512

struct fake_send {
	char buf[FAKE_WIRE_MAX + 1];
	size_t len;
	char host[64];
	uint16_t port;
	uint64_t at_ms;
};

static struct {
	struct mgcp_txn_env env;
	uint64_t now;

	struct proto_timer *timer[FAKE_TIMER_MAX];
	unsigned int timers;
	unsigned int fired;

	struct fake_send send[FAKE_SEND_MAX];
	unsigned int sends;
	bool send_fail;

	uint32_t rand_value;

	unsigned int cmd_calls;
	unsigned int resp_calls;
	unsigned int done_calls;
	unsigned int disc_calls;
	unsigned int max1_calls;
	unsigned int max2_calls;
	unsigned int attempt_calls;
	unsigned int disc_ended_calls;
	unsigned int disc_open_max;	/* DISC transactions at an attempt */
	uint32_t cmd_txid;
	uint32_t resp_owner;
	uint32_t resp_txid;
	uint32_t resp_code;
	bool resp_left_pool;
	uint32_t done_owner;
	uint32_t done_txid;
	enum mgcp_txn_reason done_reason;
	uint64_t done_at;
	uint64_t disc_at;
	uint64_t max1_at[4];
	uint64_t max2_at;
	uint64_t attempt_at[16];
	uint32_t watch_handle;

	/* how the command upcall answers: 0 leaves the command running */
	uint32_t answer_code;
	const char *answer_extra;

	/* the redirect each max1 call applies, by call number, and max2's */
	const char *max1_redirect[4];
	const char *max2_redirect;
	bool max1_abort;

	/* disc_attempt sends an RSIP of the procedure */
	bool attempt_sends;

	char log[LOG_MAX];
} fk;

static struct mgcp_txn_mgr mgr;

#include "test_mgcp_clock.h"

static struct sockaddr_storage ca_sa, ca2_sa, gw_peer;
static socklen_t ca_len, ca2_len, peer_len;

/* ------------------------------------------------------------- environment */

static void log_add(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

static void log_add(const char *fmt, ...)
{
	size_t used = strlen(fk.log);
	va_list ap;

	if (used >= sizeof(fk.log) - 1)
		return;

	va_start(ap, fmt);
	vsnprintf(fk.log + used, sizeof(fk.log) - used, fmt, ap);
	va_end(ap);
}

static uint32_t fake_rand(struct proto_timer_env *env)
{
	return fk.rand_value;
}

static void fake_trace(struct proto_timer_env *env, const char *msg)
{
	(void)msg;
}

static void answer(uint32_t txid, uint32_t code, const char *extra)
{
	char text[512];
	int n;

	n = snprintf(text, sizeof(text), "%03u %u OK\r\n%s", code, txid,
		     extra ? extra : "");
	mgcp_txn_respond(&mgr, txid, (struct sockaddr *)&ca_sa, ca_len, code,
			 text, (size_t)n);
}

static void fake_command(struct mgcp_txn_env *env, const struct mgcp_msg *msg,
			 const struct sockaddr *src, socklen_t src_len)
{
	fk.cmd_calls++;
	fk.cmd_txid = msg->txid;
	log_add("cmd %u;", msg->txid);

	if (fk.answer_code)
		answer(msg->txid, fk.answer_code, fk.answer_extra);
}

static void fake_response(struct mgcp_txn_env *env, uint32_t owner,
			  uint32_t txid, const struct mgcp_msg *msg)
{
	fk.resp_calls++;
	fk.resp_owner = owner;
	fk.resp_txid = txid;
	fk.resp_code = msg->code;
	fk.resp_left_pool = mgcp_txn_get(&mgr, fk.watch_handle) == NULL &&
			    mgcp_txn_find(&mgr, txid) == MGCP_TXN_HANDLE_NONE;
	log_add("resp %u;", txid);
}

static void fake_done(struct mgcp_txn_env *env, uint32_t owner, uint32_t txid,
		      enum mgcp_txn_reason reason)
{
	fk.done_calls++;
	fk.done_owner = owner;
	fk.done_txid = txid;
	fk.done_reason = reason;
	fk.done_at = fk.now;
	log_add("done %u %s;", txid, mgcp_txn_reason_name(reason));
}

static void fake_disconnected(struct mgcp_txn_env *env)
{
	fk.disc_calls++;
	fk.disc_at = fk.now;
	log_add("disconnected;");
}

static void redirect_to(const char *host)
{
	struct sockaddr_storage sa;
	socklen_t len;

	len = sa_v4(&sa, host, 2727);
	mgcp_txn_redirect(&mgr, MGCP_TXN_OWNER_ANY, (struct sockaddr *)&sa,
			  len);
}

static void fake_max1(struct mgcp_txn_env *env, uint32_t owner, uint32_t txid,
		      uint32_t handle)
{
	unsigned int n = fk.max1_calls++;

	if (n < 4)
		fk.max1_at[n] = fk.now;
	log_add("max1 %u;", txid);
	if (fk.max1_abort)
		mgcp_txn_abort(&mgr, handle);
	else if (n < 4 && fk.max1_redirect[n])
		redirect_to(fk.max1_redirect[n]);
}

static void fake_max2(struct mgcp_txn_env *env, uint32_t owner, uint32_t txid,
		      uint32_t handle)
{
	fk.max2_calls++;
	fk.max2_at = fk.now;
	log_add("max2 %u;", txid);
	if (fk.max2_redirect)
		redirect_to(fk.max2_redirect);
}

static uint32_t ntfy_send_acks(uint32_t owner, const struct mgcp_ack_list *acks,
			       uint32_t flags);

static unsigned int disc_open(void)
{
	unsigned int i, n = 0;

	for (i = 0; i < mgr.cfg.pool_max; i++) {
		if (mgr.pool[i].in_use && mgr.pool[i].disc)
			n++;
	}

	return n;
}

static void fake_attempt(struct mgcp_txn_env *env)
{
	unsigned int open;

	if (fk.attempt_calls < 16)
		fk.attempt_at[fk.attempt_calls] = fk.now;
	fk.attempt_calls++;
	if (!fk.attempt_sends)
		return;

	ntfy_send_acks(0, NULL, MGCP_TXN_F_DISC);
	open = disc_open();
	if (open > fk.disc_open_max)
		fk.disc_open_max = open;
}

static void fake_disc_ended(struct mgcp_txn_env *env)
{
	fk.disc_ended_calls++;
	log_add("disc_ended;");
}

static uint64_t t0;

static void fake_reset_cfg(const struct mgcp_txn_cfg *cfg)
{
	mgcp_txn_mgr_done(&mgr);

	memset(&fk, 0, sizeof(fk));
	fk.now = 100000;
	t0 = fk.now;
	fk.env.send = fake_send_fn;
	fake_clock_bind(&fk.env.base);
	fk.env.base.rand32 = fake_rand;
	fk.env.base.trace = fake_trace;
	fk.env.command = fake_command;
	fk.env.response = fake_response;
	fk.env.done = fake_done;
	fk.env.disconnected = fake_disconnected;
	fk.env.max1 = fake_max1;
	fk.env.max2 = fake_max2;
	fk.env.disc_attempt = fake_attempt;
	fk.env.disc_ended = fake_disc_ended;

	ca_len = sa_v4(&ca_sa, "192.0.2.10", 2727);
	ca2_len = sa_v4(&ca2_sa, "192.0.2.11", 2727);
	peer_len = sa_v4(&gw_peer, "198.51.100.7", 2727);

	mgcp_txn_mgr_init(&mgr, &fk.env, cfg);
}

static void fake_reset(void)
{
	fake_reset_cfg(NULL);
}

/* ---------------------------------------------------------------- traffic */

static uint32_t last_txid;

static uint32_t ntfy_send_acks(uint32_t owner, const struct mgcp_ack_list *acks,
			       uint32_t flags)
{
	char text[512];
	struct sip_buf b;
	struct mgcp_ntfy n = {
		.endpoint = "aaln/1@gw.example.net",
		.version = MGCP_VERSION_RFC3435,
		.acks = acks,
		.request_id = "0",
		.observed = "L/hd",
	};
	struct mgcp_txn_req req = {
		.dst = (struct sockaddr *)&ca_sa,
		.dst_len = ca_len,
		.owner = owner,
		.acks = acks,
		.flags = flags,
	};
	uint32_t handle;

	n.txid = mgcp_txn_txid_next(&mgr);
	sip_buf_init(&b, text, sizeof(text));
	if (!mgcp_ntfy_build(&b, &n))
		return MGCP_TXN_HANDLE_NONE;

	req.txid = n.txid;
	req.wire = text;
	req.len = sip_buf_len(&b);
	last_txid = n.txid;
	handle = mgcp_txn_send(&mgr, &req);
	fk.watch_handle = handle;

	return handle;
}

static uint32_t ntfy_send(uint32_t owner)
{
	return ntfy_send_acks(owner, NULL, 0);
}

static unsigned int rx_from(const struct sockaddr_storage *src, socklen_t len,
			    const char *text)
{
	return mgcp_txn_recv(&mgr, text, strlen(text),
			     (const struct sockaddr *)src, len);
}

static unsigned int rx(const char *text)
{
	return rx_from(&ca_sa, ca_len, text);
}

static void rx_fmt(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

static void rx_fmt(const char *fmt, ...)
{
	char text[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);

	rx(text);
}

static uint64_t send_at(unsigned int idx)
{
	if (idx >= fk.sends || idx >= FAKE_SEND_SLOTS)
		return UINT64_MAX;

	return fk.send[idx].at_ms - t0;
}

static bool send_has(unsigned int idx, const char *needle)
{
	return strstr(send_text(idx), needle) != NULL;
}

/* a gone transaction reads as an empty one, so a failing case cannot crash */
static const struct mgcp_txn *txn_at(uint32_t handle)
{
	static const struct mgcp_txn gone;
	const struct mgcp_txn *txn = mgcp_txn_get(&mgr, handle);

	return txn ? txn : &gone;
}

static uint64_t retx_due(uint32_t handle)
{
	struct mgcp_txn *txn = mgcp_txn_get(&mgr, handle);

	if (!txn)
		return 0;

	return fake_timer_due(&txn->retx) - fk.now;
}

/* ---------------------------------------------------------------- ladder */

static void ladder_tests(void)
{
	static const uint64_t at[] = {
		0, 200, 400, 800, 1600, 3200, 6400, 10400,
	};
	struct mgcp_txn_cfg cfg;
	unsigned int i, bad;
	uint32_t h;

	/*
	 * RFC 3435 3.5.3 with rand32 fixed at 0, so each draw is the low end
	 * T-DELAY / 2, and ADEV 0 before any sample. The first timer is
	 * RTO-INIT, 200 ms. Retransmission k doubles T-DELAY to 200 * 2^k and
	 * arms 200 * 2^(k-1): 200, 400, 800, 1600, 3200, then 6400 and 12800
	 * capped at RTO-MAX 4000. 4.3: the expiry after retransmission 5 is
	 * Max1, the expiry after retransmission 7 is Max2.
	 */
	CASE("the retransmission ladder of one NTFY to Max2 (3.5.3, 4.3)");
	fake_reset();
	h = ntfy_send(1);
	CHECK(h != MGCP_TXN_HANDLE_NONE);
	CHECK_UINT(retx_due(h), 200);
	fake_advance(14399);
	CHECK_UINT(fk.sends, 8);
	bad = 0;
	for (i = 0; i < 8; i++) {
		if (send_at(i) != at[i]) {
			printf("    send %u at %llu, want %llu\n", i,
			       (unsigned long long)send_at(i),
			       (unsigned long long)at[i]);
			bad++;
		}
		if (strcmp(send_text(i), send_text(0)))
			bad++;
	}
	CHECK_UINT(bad, 0);
	CHECK_UINT(fk.done_calls, 0);
	CHECK(!strcmp(fk.send[0].host, "192.0.2.10"));
	CHECK_UINT(fk.send[0].port, 2727);
	fake_advance(1);
	CHECK_UINT(fk.done_calls, 1);
	CHECK(fk.done_reason == MGCP_TXN_DONE_TIMEOUT);
	CHECK_UINT(fk.done_at - t0, 14400);
	CHECK_UINT(fk.done_owner, 1);
	CHECK_UINT(mgr.stat.retransmitted, 7);
	CHECK_UINT(mgr.stat.timeout, 1);
	CHECK_UINT(mgcp_txn_count(&mgr), 0);

	CASE("Max1 is reached once and max1 runs once (4.3)");
	CHECK_UINT(fk.max1_calls, 1);
	CHECK_UINT(fk.max1_at[0] - t0, 6400);

	CASE("Max2 is reached and disconnected runs once (4.3)");
	CHECK_UINT(fk.max2_calls, 1);
	CHECK_UINT(fk.max2_at - t0, 14400);
	CHECK_UINT(fk.disc_calls, 1);
	CHECK_UINT(fk.disc_at - t0, 14400);
	CHECK(!strcmp(fk.log, "max1 1;max2 1;disconnected;done 1 timeout;"));
	fake_advance(120000);
	CHECK_UINT(fk.sends, 8);
	CHECK_UINT(fk.disc_calls, 1);

	CASE("the ladder to T-MAX: retransmissions cease at 20 s (4.3)");
	mgcp_txn_cfg_default(&cfg);
	cfg.max2 = 20;
	fake_reset_cfg(&cfg);
	h = ntfy_send(2);
	fake_advance(22399);
	/* 0 .. 10400 as above, then 14400 and 18400; 22400 is past T-MAX */
	CHECK_UINT(fk.sends, 10);
	CHECK_UINT(send_at(8), 14400);
	CHECK_UINT(send_at(9), 18400);
	fake_advance(1);
	CHECK_UINT(fk.sends, 10);
	CHECK(mgcp_txn_get(&mgr, h) != NULL);
	CHECK(txn_at(h)->quiet);
	CHECK_UINT(fk.done_calls, 0);

	CASE("after T-MAX the transaction waits for 2 * T-HIST (3.5.6, 4.3)");
	fake_advance(60000 - 22400 - 1);
	CHECK_UINT(fk.done_calls, 0);
	CHECK_UINT(fk.disc_calls, 0);
	fake_advance(1);
	CHECK_UINT(fk.done_calls, 1);
	CHECK(fk.done_reason == MGCP_TXN_DONE_TIMEOUT);
	CHECK_UINT(fk.done_at - t0, 60000);
	CHECK_UINT(fk.disc_calls, 1);
	CHECK_UINT(fk.sends, 10);

	CASE("a final response inside the quiet wait is still delivered");
	fake_reset_cfg(&cfg);
	h = ntfy_send(2);
	fake_advance(40000);
	rx_fmt("200 %u OK\r\n", last_txid);
	CHECK_UINT(fk.resp_calls, 1);
	CHECK(fk.done_reason == MGCP_TXN_DONE_OK);
	CHECK_UINT(fk.disc_calls, 0);

	CASE("Max2 with MGCP_TXN_CFG_MAX2_QUIET waits for 2 * T-HIST (NCS)");
	mgcp_txn_cfg_default(&cfg);
	cfg.flags = MGCP_TXN_CFG_MAX2_QUIET;
	fake_reset_cfg(&cfg);
	h = ntfy_send(3);
	fake_advance(14400);
	CHECK_UINT(fk.sends, 8);
	CHECK_UINT(fk.done_calls, 0);
	CHECK_UINT(fk.disc_calls, 0);
	CHECK(txn_at(h)->quiet);
	fake_advance(60000 - 14400);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.disc_calls, 1);
	CHECK_UINT(fk.done_at - t0, 60000);

	CASE("a redirect from max1 moves the retransmissions (4.3)");
	fake_reset();
	fk.max1_redirect[0] = "192.0.2.11";
	h = ntfy_send(4);
	fake_advance(6400);
	CHECK_UINT(fk.max1_calls, 1);
	CHECK_UINT(fk.sends, 7);
	CHECK(!strcmp(fk.send[5].host, "192.0.2.10"));
	CHECK(!strcmp(fk.send[6].host, "192.0.2.11"));
	CHECK_UINT(send_at(6), 6400);
	CHECK_UINT(txn_at(h)->reps, 0);
	/*
	 * N restarts at 0 on the new address; T-MAX ends the series after
	 * 10400, 14400 and 18400, before Max1 of the new address, and the
	 * end waits for 2 * T-HIST.
	 */
	fake_advance(60000 - 6400 - 1);
	CHECK_UINT(fk.sends, 10);
	CHECK(!strcmp(fk.send[9].host, "192.0.2.11"));
	CHECK_UINT(send_at(9), 18400);
	CHECK_UINT(txn_at(h)->reps, 3);
	CHECK_UINT(fk.max1_calls, 1);
	CHECK_UINT(fk.disc_calls, 0);
	fake_advance(1);
	CHECK_UINT(fk.disc_calls, 1);

	/*
	 * T-MAX and T-HIST at 100 s, so three addresses fit. A: sends at 0 ..
	 * 3200, Max1 at 6400 moves to B. B: 6400, then 4000 ms steps; the
	 * expiry after its fifth retransmission, at 30400, is Max1 again and
	 * moves to C. C: 30400, Max1 at 54400 with no redirect, seven
	 * retransmissions to 58400, Max2 at 62400 ends it.
	 */
	CASE("max1 runs once per address: three addresses are tried (4.3)");
	mgcp_txn_cfg_default(&cfg);
	cfg.t_max_ms = 100000;
	cfg.t_hist_ms = 100000;
	fake_reset_cfg(&cfg);
	fk.max1_redirect[0] = "192.0.2.11";
	fk.max1_redirect[1] = "192.0.2.12";
	h = ntfy_send(4);
	fake_advance(62400);
	CHECK_UINT(fk.max1_calls, 3);
	CHECK_UINT(fk.max1_at[0] - t0, 6400);
	CHECK_UINT(fk.max1_at[1] - t0, 30400);
	CHECK_UINT(fk.max1_at[2] - t0, 54400);
	CHECK(!strcmp(fk.send[5].host, "192.0.2.10"));
	CHECK(!strcmp(fk.send[6].host, "192.0.2.11"));
	CHECK(!strcmp(fk.send[11].host, "192.0.2.11"));
	CHECK_UINT(send_at(11), 26400);
	CHECK(!strcmp(fk.send[12].host, "192.0.2.12"));
	CHECK_UINT(send_at(12), 30400);
	CHECK_UINT(fk.sends, 20);
	CHECK(!strcmp(fk.send[19].host, "192.0.2.12"));
	CHECK_UINT(send_at(19), 58400);
	CHECK_UINT(fk.max2_calls, 1);
	CHECK_UINT(fk.max2_at - t0, 62400);
	CHECK_UINT(fk.done_calls, 1);
	CHECK(fk.done_reason == MGCP_TXN_DONE_TIMEOUT);
	CHECK_UINT(fk.disc_calls, 1);

	CASE("a redirect inside max2 keeps the transaction alive (4.3)");
	fake_reset();
	fk.max2_redirect = "192.0.2.11";
	h = ntfy_send(4);
	fake_advance(14400);
	CHECK_UINT(fk.max2_calls, 1);
	CHECK_UINT(fk.done_calls, 0);
	CHECK_UINT(fk.disc_calls, 0);
	CHECK_UINT(fk.sends, 9);
	CHECK(!strcmp(fk.send[8].host, "192.0.2.11"));
	CHECK_UINT(send_at(8), 14400);
	CHECK(mgcp_txn_get(&mgr, h) != NULL);
	fake_advance(60000 - 14400);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_at - t0, 60000);

	CASE("an abort inside max1 is safe");
	fake_reset();
	fk.max1_abort = true;
	h = ntfy_send(4);
	fake_advance(6400);
	CHECK_UINT(fk.max1_calls, 1);
	CHECK_UINT(fk.done_calls, 1);
	CHECK(fk.done_reason == MGCP_TXN_DONE_ABORT);
	CHECK(mgcp_txn_get(&mgr, h) == NULL);
	fake_advance(60000);
	CHECK_UINT(fk.sends, 6);
	CHECK_UINT(fk.disc_calls, 0);

	CASE("mgcp_txn_redirect() outside an upcall sends from a timer");
	fake_reset();
	h = ntfy_send(5);
	fake_advance(250);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(mgcp_txn_redirect(&mgr, 5, (struct sockaddr *)&ca2_sa,
				     ca2_len), 1);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(mgcp_txn_redirect(&mgr, 6, (struct sockaddr *)&ca2_sa,
				     ca2_len), 0);
	fake_advance(0);
	CHECK_UINT(fk.sends, 3);
	CHECK(!strcmp(fk.send[2].host, "192.0.2.11"));
	CHECK_UINT(txn_at(h)->reps, 0);

	CASE("mgcp_txn_redirect_one() moves one command and leaves the other "
	     "of its owner (4.3)");
	{
		uint32_t h2;
		unsigned int reps2;

		fake_reset();
		h = ntfy_send(5);
		h2 = ntfy_send(5);
		fake_advance(250);
		CHECK_UINT(fk.sends, 4);
		reps2 = txn_at(h2)->reps;
		CHECK_UINT(reps2, 1);
		CHECK(mgcp_txn_redirect_one(&mgr, h, (struct sockaddr *)&ca2_sa,
					    ca2_len));
		CHECK(!mgcp_txn_redirect_one(&mgr, MGCP_TXN_HANDLE_NONE,
					     (struct sockaddr *)&ca2_sa,
					     ca2_len));
		fake_advance(0);
		CHECK_UINT(fk.sends, 5);
		CHECK(!strcmp(fk.send[4].host, "192.0.2.11"));
		CHECK_UINT(txn_at(h)->reps, 0);
		CHECK_UINT(txn_at(h2)->reps, reps2);
		CHECK(!txn_at(h2)->redirected);
		CHECK(!memcmp(&txn_at(h2)->dst, &ca_sa, ca_len));
	}
}

/* ------------------------------------------------------------ estimator */

static void estimator_tests(void)
{
	uint32_t h;

	/*
	 * Golden numbers from 3.5.3 with the gains of RFC 6298 2.2 and 2.3,
	 * in microseconds, each division rounding down:
	 *   sample 1, R = 100 ms: AAD = 100000, ADEV = R / 2 = 50000,
	 *     RTO = (100000 + 4 * 50000) / 1000 = 300;
	 *   sample 2, R = 60 ms: ADEV = (3 * 50000 + |100000 - 60000|) / 4
	 *     = 47500, AAD = (7 * 100000 + 60000) / 8 = 95000,
	 *     RTO = (95000 + 190000) / 1000 = 285;
	 *   sample 3, R = 200 ms: ADEV = (3 * 47500 + 105000) / 4 = 61875,
	 *     AAD = (7 * 95000 + 200000) / 8 = 108125,
	 *     RTO = (108125 + 247500) / 1000 = 355.
	 */
	CASE("the RTO estimator against golden numbers (3.5.3)");
	fake_reset();
	CHECK_UINT(mgcp_txn_rto_ms(&mgr), 200);
	ntfy_send(1);
	fake_advance(100);
	rx_fmt("200 %u OK\r\n", last_txid);
	CHECK_UINT(mgr.aad_us, 100000);
	CHECK_UINT(mgr.adev_us, 50000);
	CHECK_UINT(mgcp_txn_rto_ms(&mgr), 300);
	ntfy_send(1);
	fake_advance(60);
	rx_fmt("200 %u OK\r\n", last_txid);
	CHECK_UINT(mgr.adev_us, 47500);
	CHECK_UINT(mgr.aad_us, 95000);
	CHECK_UINT(mgcp_txn_rto_ms(&mgr), 285);
	ntfy_send(1);
	fake_advance(200);
	rx_fmt("200 %u OK\r\n", last_txid);
	CHECK_UINT(mgr.adev_us, 61875);
	CHECK_UINT(mgr.aad_us, 108125);
	CHECK_UINT(mgcp_txn_rto_ms(&mgr), 355);

	/*
	 * The backoff on those estimators, rand32 at 0: T-DELAY starts at the
	 * first RTO, 355. Retransmission 1 doubles it to 710 and arms
	 * 355 + 4 * 61875 / 1000 = 355 + 247 = 602; retransmission 2 doubles
	 * it to 1420 and arms 710 + 247 = 957.
	 */
	CASE("the backoff draws on T-DELAY and ADEV (3.5.3)");
	t0 = fk.now;
	fk.sends = 0;
	h = ntfy_send(1);
	CHECK_UINT(retx_due(h), 355);
	fake_advance(355 + 602 + 1);
	CHECK_UINT(fk.sends, 3);
	CHECK_UINT(send_at(1), 355);
	CHECK_UINT(send_at(2), 957);
	CHECK_UINT(retx_due(h), 957 - 1);

	CASE("a retransmitted transaction leaves the estimators alone (Karn)");
	rx_fmt("200 %u OK\r\n", last_txid);
	CHECK_UINT(mgr.aad_us, 108125);
	CHECK_UINT(mgr.adev_us, 61875);
	CHECK_UINT(mgr.rto_carry_ms, 957);
	CHECK_UINT(mgr.tdelay_carry_ms, 1420);

	/*
	 * 3.5.3: the next command starts with the RTO of that last
	 * retransmission, 957. It succeeds at once with R = 50 ms:
	 * ADEV = (3 * 61875 + 58125) / 4 = 60937, AAD = (7 * 108125 + 50000)
	 * / 8 = 100859, RTO = (100859 + 243748) / 1000 = 344.
	 */
	CASE("the next command uses the last RTO until a clean sample (3.5.3)");
	CHECK_UINT(mgcp_txn_rto_ms(&mgr), 957);
	h = ntfy_send(1);
	CHECK_UINT(retx_due(h), 957);
	fake_advance(50);
	rx_fmt("200 %u OK\r\n", last_txid);
	CHECK_UINT(mgr.adev_us, 60937);
	CHECK_UINT(mgr.aad_us, 100859);
	CHECK_UINT(mgr.rto_carry_ms, 0);
	CHECK_UINT(mgcp_txn_rto_ms(&mgr), 344);

	/*
	 * 4 * 60937 / 1000 = 243. A command arms 344, T-DELAY 344; its
	 * retransmission at 344 doubles T-DELAY to 688 and arms
	 * 344 + 243 = 587; the response at 400 leaves RTO 587 and T-DELAY 688
	 * to the next command. That command arms 587; retransmission 1
	 * doubles T-DELAY to 1376 and arms 688 + 243 = 931, so its second
	 * retransmission comes at 587 + 931 = 1518, later than the first gap.
	 * A T-DELAY restarted at 587 would arm 830 instead.
	 */
	CASE("the carried T-DELAY keeps the backoff exponential (3.5.3)");
	ntfy_send(1);
	fake_advance(400);
	rx_fmt("200 %u OK\r\n", last_txid);
	CHECK_UINT(mgr.rto_carry_ms, 587);
	CHECK_UINT(mgr.tdelay_carry_ms, 688);
	t0 = fk.now;
	fk.sends = 0;
	h = ntfy_send(1);
	CHECK_UINT(retx_due(h), 587);
	fake_advance(1518);
	CHECK_UINT(fk.sends, 3);
	CHECK_UINT(send_at(1), 587);
	CHECK_UINT(send_at(2), 1518);
	CHECK(send_at(2) - send_at(1) > send_at(1));

	/*
	 * Ten clean 1 ms samples: AAD 1000 us, ADEV 500, then 375, 281, 210,
	 * 157, 117, 87, 65, 48, 36 us. RTO = (1000 + 144) / 1000 = 1, raised
	 * to RTO-MIN 100, and T-DELAY starts there. rand32 0 draws the low end
	 * T-DELAY / 2: 100, 100, 200, 400, 800, 1600, 3200, then 6400 capped
	 * at 4000, so Max2 comes at 10400 ms.
	 */
	CASE("the backoff doubles above the RTO floor after tiny RTTs (3.5.3)");
	fake_reset();
	{
		static const uint64_t gaps[] = {
			100, 100, 200, 400, 800, 1600, 3200,
		};
		unsigned int i, bad = 0;

		for (i = 0; i < 10; i++) {
			ntfy_send(1);
			fake_advance(1);
			rx_fmt("200 %u OK\r\n", last_txid);
		}
		CHECK_UINT(mgr.adev_us, 36);
		CHECK_UINT(mgcp_txn_rto_ms(&mgr), 100);
		t0 = fk.now;
		fk.sends = 0;
		h = ntfy_send(1);
		fake_advance(10400);
		CHECK_UINT(fk.sends, 8);
		for (i = 0; i < 7; i++) {
			if (send_at(i + 1) - send_at(i) != gaps[i])
				bad++;
		}
		CHECK_UINT(bad, 0);
		CHECK_UINT(fk.max2_calls, 1);
		CHECK_UINT(fk.max2_at - t0, 10400);
		CHECK_UINT(fk.done_at - t0, 10400);
	}

	/*
	 * rand32 at 0xffffffff: each draw of the ladder lies inside
	 * [T-DELAY / 2, T-DELAY], T-DELAY = 200 * 2^k, bounded by RTO-MAX;
	 * the first disconnected delay is 1000 + 0xffffffff % 14001 = 7534,
	 * inside 1 s to Tdinit.
	 */
	CASE("rand32 at its top keeps every draw inside its range");
	fake_reset();
	fk.rand_value = 0xffffffffu;
	t0 = fk.now;
	h = ntfy_send(1);
	fake_advance(14399 + 4000);
	{
		uint64_t td = 200, gap, lo, hi;
		unsigned int i, bad = 0;

		for (i = 1; i + 1 < fk.sends && i < 8; i++) {
			td *= 2;
			gap = send_at(i + 1) - send_at(i);
			lo = td / 2 > 4000 ? 4000 : td / 2;
			hi = td > 4000 ? 4000 : td;
			if (gap < lo || gap > hi)
				bad++;
		}
		CHECK_UINT(bad, 0);
		CHECK(fk.sends >= 7);
	}
	mgcp_txn_disc_enter(&mgr);
	CHECK_UINT(mgr.disc.td_ms, 7534);
	CHECK(mgr.disc.td_ms <= 15000);

	/* R = 1 ms: (1000 + 4 * 500) / 1000 = 3, raised to RTO-MIN */
	CASE("the RTO is bounded below by RTO-MIN (3.5.3)");
	fake_reset();
	ntfy_send(1);
	fake_advance(1);
	rx_fmt("200 %u OK\r\n", last_txid);
	CHECK_UINT(mgr.aad_us, 1000);
	CHECK_UINT(mgcp_txn_rto_ms(&mgr), MGCP_TXN_RTO_MIN_DEFAULT);

	/* R = 3000 ms with RTO-INIT 4000: (3000000 + 4 * 1500000) / 1000 =
	 * 9000, lowered to RTO-MAX */
	CASE("the RTO is bounded above by RTO-MAX (3.5.3)");
	{
		struct mgcp_txn_cfg cfg;

		mgcp_txn_cfg_default(&cfg);
		cfg.rto_init_ms = 4000;
		fake_reset_cfg(&cfg);
	}
	h = ntfy_send(1);
	CHECK_UINT(retx_due(h), 4000);
	fake_advance(3000);
	rx_fmt("200 %u OK\r\n", last_txid);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(mgr.aad_us, 3000000);
	CHECK_UINT(mgcp_txn_rto_ms(&mgr), MGCP_TXN_RTO_MAX_DEFAULT);
}

/* ------------------------------------------------- commands from the CA */

static const char crcx_500[] =
	"CRCX 500 aaln/1@gw.example.net MGCP 1.0\r\n"
	"C: A3C47F21456789F0\r\n"
	"M: recvonly\r\n";

static void hist_tests(void)
{
	struct mgcp_txn_cfg cfg;
	char first[FAKE_WIRE_MAX + 1];

	CASE("a new command reaches the caller once (3.5.1)");
	fake_reset();
	fk.answer_code = 200;
	fk.answer_extra = "I: FDE234C8\r\n";
	CHECK_UINT(rx(crcx_500), 1);
	CHECK_UINT(fk.cmd_calls, 1);
	CHECK_UINT(fk.cmd_txid, 500);
	CHECK_UINT(fk.sends, 1);
	CHECK(!strcmp(send_text(0), "200 500 OK\r\nI: FDE234C8\r\n"));
	CHECK(!strcmp(fk.send[0].host, "192.0.2.10"));
	snprintf(first, sizeof(first), "%s", send_text(0));

	CASE("a repetition inside T-HIST gets the same bytes, no upcall");
	fake_advance(1000);
	rx(crcx_500);
	CHECK_UINT(fk.cmd_calls, 1);
	CHECK_UINT(fk.sends, 2);
	CHECK(!strcmp(send_text(1), first));
	CHECK_UINT(mgr.stat.absorbed, 1);
	fake_advance(30000 - 1000 - 1);
	rx(crcx_500);
	CHECK_UINT(fk.cmd_calls, 1);
	CHECK_UINT(fk.sends, 3);
	CHECK(!strcmp(send_text(2), first));

	CASE("a repetition after T-HIST is a new command (3.5.1)");
	fake_advance(1);
	rx(crcx_500);
	CHECK_UINT(fk.cmd_calls, 2);
	CHECK_UINT(mgr.stat.absorbed, 2);

	CASE("repetitions match on the transaction id alone (3.5.2)");
	fake_reset();
	fk.answer_code = 200;
	rx(crcx_500);
	rx_from(&gw_peer, peer_len, crcx_500);
	CHECK_UINT(fk.cmd_calls, 1);
	CHECK_UINT(fk.sends, 2);
	CHECK(!strcmp(fk.send[1].host, "198.51.100.7"));
	CHECK(!strcmp(send_text(1), send_text(0)));

	CASE("a repetition while the command runs gets no second upcall");
	fake_reset();
	rx(crcx_500);
	CHECK_UINT(fk.cmd_calls, 1);
	CHECK_UINT(fk.sends, 0);
	rx(crcx_500);
	CHECK_UINT(fk.cmd_calls, 1);
	CHECK_UINT(fk.sends, 1);
	CHECK(!strcmp(send_text(0), "100 500\r\n"));
	rx(crcx_500);
	CHECK_UINT(fk.cmd_calls, 1);
	CHECK_UINT(fk.sends, 2);
	CHECK(!strcmp(send_text(1), "100 500\r\n"));

	CASE("prov_verbs limits the provisional to CRCX and MDCX (NCS 8.8)");
	mgcp_txn_cfg_default(&cfg);
	cfg.prov_verbs = (1u << MGCP_VERB_CRCX) | (1u << MGCP_VERB_MDCX);
	fake_reset_cfg(&cfg);
	rx("RQNT 501 aaln/1@gw.example.net MGCP 1.0\r\nX: 1\r\n");
	rx("RQNT 501 aaln/1@gw.example.net MGCP 1.0\r\nX: 1\r\n");
	CHECK_UINT(fk.cmd_calls, 1);
	CHECK_UINT(fk.sends, 0);
	rx(crcx_500);
	rx(crcx_500);
	CHECK_UINT(fk.cmd_calls, 2);
	CHECK_UINT(fk.sends, 1);
	CHECK(!strcmp(send_text(0), "100 500\r\n"));

	CASE("a command answered late keeps its record");
	fake_reset();
	rx(crcx_500);
	fake_advance(5000);
	answer(500, 200, NULL);
	CHECK_UINT(fk.sends, 1);
	CHECK(!mgcp_txn_respond(&mgr, 500, (struct sockaddr *)&ca_sa, ca_len,
				200, "200 500 OK\r\n", 12));
	rx(crcx_500);
	CHECK_UINT(fk.sends, 2);
	CHECK(!strcmp(send_text(1), "200 500 OK\r\n"));
	CHECK_UINT(fk.cmd_calls, 1);

	CASE("the record evicts the oldest entry when it is full");
	mgcp_txn_cfg_default(&cfg);
	cfg.hist_max = 2;
	fake_reset_cfg(&cfg);
	fk.answer_code = 200;
	rx("AUEP 601 aaln/1@gw.example.net MGCP 1.0\r\n");
	fake_advance(10);
	rx("AUEP 602 aaln/1@gw.example.net MGCP 1.0\r\n");
	fake_advance(10);
	rx("AUEP 603 aaln/1@gw.example.net MGCP 1.0\r\n");
	CHECK_UINT(mgr.stat.evicted, 1);
	rx("AUEP 602 aaln/1@gw.example.net MGCP 1.0\r\n");
	CHECK_UINT(fk.cmd_calls, 3);
	rx("AUEP 601 aaln/1@gw.example.net MGCP 1.0\r\n");
	CHECK_UINT(fk.cmd_calls, 4);

	CASE("a full record evicts an answered entry before a running one");
	mgcp_txn_cfg_default(&cfg);
	cfg.hist_max = 2;
	fake_reset_cfg(&cfg);
	fk.answer_code = 0;
	rx("CRCX 601 aaln/1@gw.example.net MGCP 1.0\r\nC: 1\r\n");
	fake_advance(10);
	fk.answer_code = 200;
	rx("AUEP 602 aaln/1@gw.example.net MGCP 1.0\r\n");
	fake_advance(10);
	rx("AUEP 603 aaln/1@gw.example.net MGCP 1.0\r\n");
	CHECK_UINT(mgr.stat.evicted, 1);
	CHECK_UINT(fk.cmd_calls, 3);
	fk.answer_code = 0;
	rx("CRCX 601 aaln/1@gw.example.net MGCP 1.0\r\nC: 1\r\n");
	CHECK_UINT(fk.cmd_calls, 3);
	CHECK(!strcmp(last_text(), "100 601\r\n"));
	rx("AUEP 602 aaln/1@gw.example.net MGCP 1.0\r\n");
	CHECK_UINT(fk.cmd_calls, 4);

	CASE("a 3000 byte final fits the default record (3.5.4)");
	fake_reset();
	rx(crcx_500);
	{
		static char big[4200];
		size_t head = 12;

		memcpy(big, "200 500 OK\r\n", head);
		memset(big + head, 'a', 3000 - head - 2);
		memcpy(big + 3000 - 2, "\r\n", 2);
		CHECK(mgcp_txn_respond(&mgr, 500, (struct sockaddr *)&ca_sa,
				       ca_len, 200, big, 3000));
		CHECK_UINT(fk.sends, 1);
		CHECK_UINT(fk.send[0].len, 3000);
	}

	CASE("a 4096 byte final after a provisional still takes its K:");
	fake_reset();
	rx(crcx_500);
	answer(500, 100, NULL);
	{
		static char big[4200];
		size_t head = 12;

		memcpy(big, "200 500 OK\r\n", head);
		memset(big + head, 'a', 4096 - head - 2);
		memcpy(big + 4096 - 2, "\r\n", 2);
		CHECK(mgcp_txn_respond(&mgr, 500, (struct sockaddr *)&ca_sa,
				       ca_len, 200, big, 4096));
		CHECK_UINT(fk.sends, 2);
		CHECK_UINT(fk.send[1].len, 4100);
		CHECK(!strncmp(send_text(1), "200 500 OK\r\nK:\r\naaa", 19));
		CHECK_UINT(mgr.stat.oversize, 0);
	}

	CASE("respond refuses a response that does not fit wire_max");
	mgcp_txn_cfg_default(&cfg);
	cfg.wire_max = 64;
	fake_reset_cfg(&cfg);
	rx(crcx_500);
	{
		char big[100];

		memset(big, 'x', sizeof(big));
		memcpy(big, "200 500 OK\r\n", 12);
		CHECK(!mgcp_txn_respond(&mgr, 500, (struct sockaddr *)&ca_sa,
					ca_len, 200, big, sizeof(big)));
		CHECK_UINT(mgr.stat.oversize, 1);
		CHECK_UINT(fk.sends, 0);
	}
	CHECK(!mgcp_txn_respond(&mgr, 500, (struct sockaddr *)&ca_sa, ca_len,
				0, "000 500\r\n", 9));
	answer(500, 200, NULL);
	CHECK_UINT(fk.sends, 1);
}

static void handshake_tests(void)
{
	unsigned int n;

	CASE("a final after a provisional carries an empty K: (3.5.6)");
	fake_reset();
	rx(crcx_500);
	answer(500, 100, "I: FDE234C8\r\n");
	CHECK_UINT(fk.sends, 1);
	CHECK(!strcmp(send_text(0), "100 500 OK\r\nI: FDE234C8\r\n"));
	fake_advance(3000);
	answer(500, 200, "I: FDE234C8\r\n");
	CHECK_UINT(fk.sends, 2);
	CHECK(!strcmp(send_text(1), "200 500 OK\r\nK:\r\nI: FDE234C8\r\n"));

	CASE("the final goes out again until the 000 arrives (3.5.6)");
	t0 = fk.now;
	fake_advance(1600);
	/* rand32 0: intervals 200, 200, 400, 800 after the final */
	CHECK_UINT(fk.sends, 6);
	CHECK_UINT(send_at(2), 200);
	CHECK_UINT(send_at(3), 400);
	CHECK_UINT(send_at(4), 800);
	CHECK_UINT(send_at(5), 1600);
	CHECK(!strcmp(send_text(5), send_text(1)));
	rx("000 500\r\n");
	CHECK_UINT(mgr.stat.acked, 1);
	fake_advance(20000);
	CHECK_UINT(fk.sends, 6);

	CASE("a confirmed command is discarded on repetition (3.5.2)");
	rx(crcx_500);
	CHECK_UINT(fk.sends, 6);
	CHECK_UINT(fk.cmd_calls, 1);

	CASE("a 000 from another entity is not accepted (3.5.2)");
	fake_reset();
	rx(crcx_500);
	answer(500, 100, NULL);
	answer(500, 200, NULL);
	rx_from(&gw_peer, peer_len, "000 500\r\n");
	CHECK_UINT(mgr.stat.stray, 1);
	CHECK_UINT(mgr.stat.acked, 0);
	fake_advance(250);
	CHECK_UINT(fk.sends, 3);

	CASE("a K: range in a later command confirms the final (3.5.2)");
	fake_reset();
	rx(crcx_500);
	answer(500, 100, NULL);
	answer(500, 200, NULL);
	rx("RQNT 501 aaln/1@gw.example.net MGCP 1.0\r\nK: 499-500\r\n"
	   "X: 1\r\n");
	CHECK_UINT(mgr.stat.acked, 1);
	CHECK_UINT(fk.cmd_calls, 2);
	n = fk.sends;
	fake_advance(20000);
	CHECK_UINT(fk.sends, n);

	CASE("a K: from another source does not confirm the final (3.5.2)");
	fake_reset();
	rx(crcx_500);
	answer(500, 100, NULL);
	answer(500, 200, NULL);
	rx_from(&gw_peer, peer_len,
		"RQNT 501 aaln/1@gw.example.net MGCP 1.0\r\n"
		"K: 500\r\nX: 1\r\n");
	CHECK_UINT(fk.cmd_calls, 2);
	CHECK_UINT(mgr.stat.acked, 0);
	fake_advance(250);
	CHECK_UINT(fk.sends, 3);
	CHECK(!strcmp(send_text(2), send_text(1)));

	CASE("the final stops at T-MAX without an acknowledgment (3.5.6)");
	fake_reset();
	rx(crcx_500);
	answer(500, 100, NULL);
	answer(500, 200, NULL);
	fake_advance(20000);
	/* the 100, the final and the resends at 200 .. 18400 */
	n = fk.sends;
	CHECK_UINT(n, 11);
	fake_advance(9000);
	CHECK_UINT(fk.sends, n);
	CHECK_UINT(fk.disc_calls, 0);
	CHECK_UINT(fk.done_calls, 0);
	rx(crcx_500);
	CHECK_UINT(fk.sends, n + 1);
	CHECK(send_has(n, "K:"));
	CHECK_UINT(fk.cmd_calls, 1);

	CASE("a final already carrying K: is kept as written");
	fake_reset();
	rx(crcx_500);
	answer(500, 100, NULL);
	answer(500, 200, "K:\r\n");
	CHECK(!strcmp(send_text(1), "200 500 OK\r\nK:\r\n"));

	CASE("K: goes into the response behind a piggybacked RSIP (4.4.7)");
	fake_reset();
	rx(crcx_500);
	answer(500, 100, NULL);
	{
		static const char dgram[] =
			"RSIP 900 aaln/1@gw.example.net MGCP 1.0\r\n"
			"RM: disconnected\r\n"
			".\r\n"
			"200 500 OK\r\n";

		CHECK(mgcp_txn_respond(&mgr, 500, (struct sockaddr *)&ca_sa,
				       ca_len, 200, dgram, strlen(dgram)));
		CHECK(!strcmp(send_text(1),
			      "RSIP 900 aaln/1@gw.example.net MGCP 1.0\r\n"
			      "RM: disconnected\r\n"
			      ".\r\n"
			      "200 500 OK\r\nK:\r\n"));
	}

	CASE("a final after a provisional must name its command");
	fake_reset();
	rx(crcx_500);
	answer(500, 100, NULL);
	CHECK(!mgcp_txn_respond(&mgr, 500, (struct sockaddr *)&ca_sa, ca_len,
				200, "200 499 OK\r\n", 12));
	CHECK_UINT(fk.sends, 1);
	answer(500, 200, NULL);
	CHECK_UINT(fk.sends, 2);

	CASE("a final with no provisional before it asks for nothing");
	fake_reset();
	rx(crcx_500);
	answer(500, 200, NULL);
	CHECK(!send_has(0, "K:"));
	fake_advance(10000);
	CHECK_UINT(fk.sends, 1);

	CASE("a K: in a command also confirms a plain final (3.5.2)");
	rx("RQNT 502 aaln/1@gw.example.net MGCP 1.0\r\nK: 500\r\nX: 1\r\n");
	CHECK_UINT(mgr.stat.acked, 1);
	rx(crcx_500);
	CHECK_UINT(fk.sends, 1);
}

/* ---------------------------------------------------- gateway responses */

static void response_tests(void)
{
	struct mgcp_ack_list acks;
	uint32_t h, first;

	CASE("a final response is delivered after the transaction left the "
	     "pool");
	fake_reset();
	h = ntfy_send(9);
	fake_advance(20);
	rx_fmt("200 %u OK\r\n", last_txid);
	CHECK_UINT(fk.resp_calls, 1);
	CHECK_UINT(fk.resp_owner, 9);
	CHECK_UINT(fk.resp_txid, last_txid);
	CHECK_UINT(fk.resp_code, 200);
	CHECK(fk.resp_left_pool);
	CHECK(mgcp_txn_get(&mgr, h) == NULL);
	CHECK(!strcmp(fk.log, "resp 1;done 1 ok;"));

	CASE("the gateway K: list goes onto the next command and clears "
	     "(3.5.2)");
	first = last_txid;
	CHECK_UINT(mgcp_txn_acks_get(&mgr, 9, (struct sockaddr *)&ca_sa,
				     ca_len, &acks), 1);
	CHECK_UINT(acks.txid[0], first);
	CHECK_UINT(mgcp_txn_acks_get(&mgr, 8, (struct sockaddr *)&ca_sa,
				     ca_len, &acks), 0);
	CHECK_UINT(mgcp_txn_acks_get(&mgr, 9, (struct sockaddr *)&ca2_sa,
				     ca2_len, &acks), 0);
	CHECK_UINT(mgcp_txn_acks_get(&mgr, MGCP_TXN_OWNER_ANY,
				     (struct sockaddr *)&ca_sa, ca_len, &acks),
		   1);
	h = ntfy_send_acks(9, &acks, 0);
	CHECK(h != MGCP_TXN_HANDLE_NONE);
	CHECK(send_has(fk.sends - 1, "\r\nK: 1\r\n"));
	CHECK_UINT(mgcp_txn_acks_get(&mgr, 9, (struct sockaddr *)&ca_sa,
				     ca_len, &acks), 0);

	CASE("a K: entry expires after T-HIST (3.5.2)");
	rx_fmt("200 %u OK\r\n", last_txid);
	CHECK_UINT(mgcp_txn_acks_get(&mgr, 9, (struct sockaddr *)&ca_sa,
				     ca_len, &acks), 1);
	fake_advance(30001);
	CHECK_UINT(mgcp_txn_acks_get(&mgr, 9, (struct sockaddr *)&ca_sa,
				     ca_len, &acks), 0);

	CASE("a final with an empty K: gets a 000 and stays off the list");
	fake_reset();
	ntfy_send(9);
	rx_fmt("200 %u OK\r\nK:\r\n", last_txid);
	CHECK_UINT(fk.resp_calls, 1);
	CHECK_UINT(fk.sends, 2);
	CHECK(!strcmp(send_text(1), "000 1\r\n"));
	CHECK_UINT(mgcp_txn_acks_get(&mgr, 9, (struct sockaddr *)&ca_sa,
				     ca_len, &acks), 0);

	CASE("a repeated final of a completed transaction is absorbed");
	rx_fmt("200 %u OK\r\nK:\r\n", last_txid);
	CHECK_UINT(fk.resp_calls, 1);
	CHECK_UINT(fk.sends, 3);
	CHECK(!strcmp(send_text(2), "000 1\r\n"));
	CHECK_UINT(mgr.stat.stray, 0);
	CHECK_UINT(mgr.stat.absorbed, 1);

	CASE("a response for an unknown transaction is stray, not delivered");
	rx("200 4242 OK\r\n");
	CHECK_UINT(fk.resp_calls, 1);
	CHECK_UINT(mgr.stat.stray, 1);
	CHECK_UINT(fk.done_calls, 1);

	CASE("a provisional switches to LONGTRAN-TIMER (3.5.6)");
	fake_reset();
	h = ntfy_send(9);
	fake_advance(100);
	rx_fmt("100 %u Pending\r\n", last_txid);
	CHECK_UINT(fk.resp_calls, 0);
	CHECK_UINT(mgr.aad_us, 100000);
	CHECK_UINT(retx_due(h), 5000);
	fake_advance(19900);
	/* 0, then 5100, 10100, 15100; 20100 is past T-MAX */
	CHECK_UINT(fk.sends, 4);
	CHECK_UINT(send_at(1), 5100);
	CHECK_UINT(send_at(3), 15100);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 4);
	CHECK_UINT(fk.disc_calls, 0);
	rx_fmt("200 %u OK\r\n", last_txid);
	CHECK_UINT(fk.resp_calls, 1);
	CHECK(fk.done_reason == MGCP_TXN_DONE_OK);

	CASE("a piggybacked datagram runs each message in order (3.5.5)");
	fake_reset();
	ntfy_send(9);
	rx_fmt("200 %u OK\r\n.\r\nRQNT 77 aaln/1@gw.example.net MGCP 1.0\r\n"
	       "X: 5\r\n.\r\n200 4243 OK\r\n", last_txid);
	CHECK(!strcmp(fk.log, "resp 1;done 1 ok;cmd 77;"));
	CHECK_UINT(mgr.stat.stray, 1);

	CASE("a malformed message is counted and skipped (3.5.5)");
	fake_reset();
	CHECK_UINT(rx("CRCX 0 aaln/1@gw MGCP 1.0\r\n.\r\n"
		      "AUEP 78 aaln/1@gw MGCP 1.0\r\n"), 1);
	CHECK_UINT(mgr.stat.malformed, 1);
	CHECK_UINT(fk.cmd_calls, 1);
	CHECK_UINT(fk.cmd_txid, 78);

	CASE("a released owner gets no upcall");
	fake_reset();
	ntfy_send(9);
	CHECK_UINT(mgcp_txn_owner_release(&mgr, 9), 1);
	rx_fmt("200 %u OK\r\n", last_txid);
	CHECK_UINT(fk.resp_calls, 0);
	CHECK_UINT(fk.done_calls, 0);
	CHECK_UINT(mgcp_txn_count(&mgr), 0);
}

/* ---------------------------------------------------------- pool, ids */

static void pool_tests(void)
{
	struct mgcp_txn_cfg cfg;
	uint32_t h, a, b;

	CASE("transaction ids start at 1 and wrap at 999999999 (3.2.1.2)");
	fake_reset();
	CHECK_UINT(mgcp_txn_txid_next(&mgr), 1);
	CHECK_UINT(mgcp_txn_txid_next(&mgr), 2);
	mgcp_txn_cfg_default(&cfg);
	cfg.txid_first = 999999998;
	fake_reset_cfg(&cfg);
	CHECK_UINT(mgcp_txn_txid_next(&mgr), 999999998);
	CHECK_UINT(mgcp_txn_txid_next(&mgr), 999999999);
	CHECK_UINT(mgcp_txn_txid_next(&mgr), 1);
	CHECK_UINT(mgcp_txn_txid_next(&mgr), 2);

	CASE("an id in use is skipped");
	fake_reset();
	ntfy_send(1);
	CHECK_UINT(last_txid, 1);
	mgr.txid_next = 1;
	CHECK_UINT(mgcp_txn_txid_next(&mgr), 2);

	CASE("a full pool refuses and counts denied");
	mgcp_txn_cfg_default(&cfg);
	cfg.pool_max = 2;
	fake_reset_cfg(&cfg);
	a = ntfy_send(1);
	b = ntfy_send(2);
	CHECK(a != MGCP_TXN_HANDLE_NONE && b != MGCP_TXN_HANDLE_NONE);
	CHECK(ntfy_send(3) == MGCP_TXN_HANDLE_NONE);
	CHECK_UINT(mgr.stat.denied, 1);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(mgcp_txn_count(&mgr), 2);
	rx("200 1 OK\r\n");
	h = ntfy_send(3);
	CHECK(h != MGCP_TXN_HANDLE_NONE);
	CHECK(h != a);
	CHECK(mgcp_txn_get(&mgr, a) == NULL);

	CASE("a duplicate id or a bad request is refused");
	fake_reset();
	ntfy_send(1);
	{
		struct mgcp_txn_req req = {
			.txid = last_txid, .wire = "x\r\n", .len = 3,
			.dst = (struct sockaddr *)&ca_sa, .dst_len = ca_len,
		};

		CHECK(mgcp_txn_send(&mgr, &req) == MGCP_TXN_HANDLE_NONE);
		req.txid = 0;
		CHECK(mgcp_txn_send(&mgr, &req) == MGCP_TXN_HANDLE_NONE);
		req.txid = 50;
		req.dst = NULL;
		CHECK(mgcp_txn_send(&mgr, &req) == MGCP_TXN_HANDLE_NONE);
	}

	CASE("a transport failure ends the transaction with that reason");
	fake_reset();
	h = ntfy_send(4);
	fk.send_fail = true;
	fake_advance(200);
	CHECK_UINT(fk.done_calls, 1);
	CHECK(fk.done_reason == MGCP_TXN_DONE_TRANSPORT);
	CHECK_UINT(fk.done_owner, 4);
	CHECK_UINT(mgr.stat.transport, 1);
	CHECK(mgcp_txn_get(&mgr, h) == NULL);
	CHECK_UINT(fk.disc_calls, 0);

	CASE("a refused first send returns no handle and runs no upcall");
	fake_reset();
	fk.send_fail = true;
	CHECK(ntfy_send(4) == MGCP_TXN_HANDLE_NONE);
	CHECK_UINT(fk.done_calls, 0);
	CHECK_UINT(mgcp_txn_count(&mgr), 0);
	CHECK_UINT(mgr.stat.transport, 1);

	CASE("an abort ends the transaction and reports it");
	fake_reset();
	h = ntfy_send(5);
	mgcp_txn_abort(&mgr, h);
	CHECK_UINT(fk.done_calls, 1);
	CHECK(fk.done_reason == MGCP_TXN_DONE_ABORT);
	CHECK(mgcp_txn_get(&mgr, h) == NULL);
	fake_advance(60000);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(fk.disc_calls, 0);
	mgcp_txn_abort(&mgr, h);
	CHECK_UINT(fk.done_calls, 1);

	CASE("the manager shutdown ends everything without an upcall");
	fake_reset();
	ntfy_send(6);
	rx(crcx_500);
	mgcp_txn_disc_enter(&mgr);
	mgcp_txn_mgr_done(&mgr);
	fake_advance(600000);
	CHECK_UINT(fk.done_calls, 0);
	CHECK_UINT(fk.attempt_calls, 0);
	CHECK_UINT(fk.timers, 0);
}

/* --------------------------------------------------------- disconnected */

static void disc_tests(void)
{
	static const uint32_t ladder[] = {
		8000, 16000, 32000, 64000, 128000, 256000, 512000, 600000,
		600000,
	};
	static const uint32_t draws[] = { 0, 1, 7000, 13999, 14000, 14001,
					  0xffffffffu };
	struct mgcp_txn_cfg cfg;
	uint64_t at;
	unsigned int i, bad;

	/* 4.4.7 step 1: 1000 + rand % (15000 - 1000 + 1) */
	CASE("the first disconnected delay lies between 1 s and Tdinit");
	bad = 0;
	for (i = 0; i < sizeof(draws) / sizeof(draws[0]); i++) {
		fake_reset();
		fk.rand_value = draws[i];
		mgcp_txn_disc_enter(&mgr);
		if (mgr.disc.td_ms < 1000 || mgr.disc.td_ms > 15000)
			bad++;
		if (mgr.disc.td_ms != 1000 + draws[i] % 14001)
			bad++;
	}
	CHECK_UINT(bad, 0);

	/*
	 * Each attempt fails at once: the RSIP's first send is refused, which
	 * ends the attempt on the transport, so the next delay starts at the
	 * attempt: 8000, then 16000 .. 512000, then Tdmax 600000.
	 */
	CASE("the disconnected delay doubles up to Tdmax (4.4.7)");
	fake_reset();
	fk.rand_value = 7000;
	fk.attempt_sends = true;
	fk.send_fail = true;
	mgcp_txn_disc_enter(&mgr);
	CHECK(mgcp_txn_disc_active(&mgr));
	CHECK_UINT(mgr.disc.td_ms, 8000);
	at = 0;
	bad = 0;
	for (i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
		at += ladder[i];
		fake_advance(ladder[i]);
		if (fk.attempt_calls != i + 1)
			bad++;
		if (i < 16 && fk.attempt_at[i] - t0 != at)
			bad++;
	}
	CHECK_UINT(bad, 0);
	CHECK_UINT(mgr.disc.td_ms, 600000);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(mgr.stat.transport, 9);

	CASE("leave ends the procedure");
	mgcp_txn_disc_leave(&mgr);
	CHECK(!mgcp_txn_disc_active(&mgr));
	fake_advance(10000000);
	CHECK_UINT(fk.attempt_calls, 9);

	CASE("disc_fail ends an attempt that never reached the layer");
	fake_reset();
	fk.rand_value = 7000;
	mgcp_txn_disc_enter(&mgr);
	fake_advance(8000);
	CHECK_UINT(fk.attempt_calls, 1);
	CHECK_UINT(fake_timer_due(&mgr.disc.timer), 0);
	mgcp_txn_disc_fail(&mgr);
	CHECK_UINT(mgr.disc.td_ms, 16000);
	CHECK_UINT(fake_timer_due(&mgr.disc.timer), fk.now + 16000);
	fake_advance(16000);
	CHECK_UINT(fk.attempt_calls, 2);
	mgcp_txn_disc_leave(&mgr);
	mgcp_txn_disc_fail(&mgr);
	CHECK_UINT(fake_timer_due(&mgr.disc.timer), 0);

	CASE("MGCP_TXN_CFG_TD_RANDOM draws from 1.5 to 2 times (NCS 7.4.3.6)");
	mgcp_txn_cfg_default(&cfg);
	cfg.flags = MGCP_TXN_CFG_TD_RANDOM;
	fake_reset_cfg(&cfg);
	fk.attempt_sends = true;
	fk.send_fail = true;
	fk.rand_value = 7000;
	mgcp_txn_disc_enter(&mgr);
	fk.rand_value = 0;
	fake_advance(8000);
	CHECK_UINT(mgr.disc.td_ms, 12000);
	fk.rand_value = 6000;
	fake_advance(12000);
	CHECK_UINT(mgr.disc.td_ms, 24000);
	/* 36000 + 6001 % (48000 - 36000 + 1) */
	fk.rand_value = 6001;
	fake_advance(24000);
	CHECK_UINT(mgr.disc.td_ms, 42001);

	/*
	 * RFC defaults, rand32 0: the first delay is 1000. Each RSIP runs the
	 * whole ladder and fails at Max2 14400 ms after its send, and the next
	 * delay doubles from that end: attempts at 1000, 15400 + 2000 = 17400,
	 * 31800 + 4000 = 35800 and 50200 + 8000 = 58200.
	 */
	CASE("one RSIP at a time, the next delay from its failed end (4.4.7)");
	fake_reset();
	fk.attempt_sends = true;
	mgcp_txn_disc_enter(&mgr);
	CHECK_UINT(mgr.disc.td_ms, 1000);
	fake_advance(58200);
	CHECK_UINT(fk.attempt_calls, 4);
	CHECK_UINT(fk.attempt_at[0] - t0, 1000);
	CHECK_UINT(fk.attempt_at[1] - t0, 17400);
	CHECK_UINT(fk.attempt_at[2] - t0, 35800);
	CHECK_UINT(fk.attempt_at[3] - t0, 58200);
	CHECK_UINT(fk.disc_open_max, 1);
	CHECK_UINT(fk.done_calls, 3);
	CHECK(fk.done_reason == MGCP_TXN_DONE_TIMEOUT);
	CHECK_UINT(mgr.disc.end_ms - t0, 50200);
	CHECK_UINT(fk.disc_calls, 0);

	CASE("a success response to the RSIP ends the procedure (4.4.7)");
	fake_reset();
	fk.attempt_sends = true;
	mgcp_txn_disc_enter(&mgr);
	fake_advance(1000);
	CHECK_UINT(fk.attempt_calls, 1);
	fake_advance(150);
	rx_fmt("200 %u OK\r\n", last_txid);
	CHECK_UINT(fk.disc_ended_calls, 1);
	CHECK(!mgcp_txn_disc_active(&mgr));
	CHECK_UINT(fake_timer_due(&mgr.disc.timer), 0);
	CHECK(!strcmp(fk.log, "disc_ended;resp 1;done 1 ok;"));
	fake_advance(600000);
	CHECK_UINT(fk.attempt_calls, 1);

	/* rand32 0: first delay 1000; the 400 at 1100 doubles it to 2000 */
	CASE("an error response to the RSIP is a failed attempt (4.4.7)");
	fake_reset();
	fk.attempt_sends = true;
	mgcp_txn_disc_enter(&mgr);
	fake_advance(1100);
	rx_fmt("400 %u Busy\r\n", last_txid);
	CHECK(mgcp_txn_disc_active(&mgr));
	CHECK_UINT(fk.disc_ended_calls, 0);
	CHECK_UINT(fk.resp_calls, 1);
	CHECK_UINT(fk.resp_code, 400);
	CHECK_UINT(mgr.disc.td_ms, 2000);
	CHECK_UINT(mgr.disc.end_ms - t0, 1100);
	CHECK_UINT(fake_timer_due(&mgr.disc.timer) - t0, 3100);
	fake_advance(2000);
	CHECK_UINT(fk.attempt_calls, 2);
	CHECK_UINT(fk.attempt_at[1] - t0, 3100);

	/*
	 * The 521 at 1100 is a failed attempt: delay 2000. Its 100 ms is the
	 * first RTT sample: AAD 100 ms, ADEV 50 ms, RTO 300, and each draw
	 * adds 4 * ADEV = 200. The caller kicks at once and sends its own
	 * RSIP; with rand32 0 its gaps are 300, 500, 800, 1400, 2600, 4000,
	 * 4000, 4000, so it fails at Max2 at 1100 + 17600 = 18700. The next
	 * delay doubles the accumulated 2000, not the first draw.
	 */
	CASE("a kick after a 521 starts an attempt and keeps the delay");
	fake_reset();
	fk.attempt_sends = true;
	mgcp_txn_disc_enter(&mgr);
	fake_advance(1100);
	rx_fmt("521 %u Redirect\r\nN: ca@ca2.example.net\r\n", last_txid);
	CHECK_UINT(mgr.disc.td_ms, 2000);
	CHECK_UINT(mgcp_txn_rto_ms(&mgr), 300);
	CHECK(mgcp_txn_disc_kick(&mgr, false));
	CHECK_UINT(fake_timer_due(&mgr.disc.timer), 0);
	fk.attempt_sends = false;
	CHECK(ntfy_send_acks(0, NULL, MGCP_TXN_F_DISC) != MGCP_TXN_HANDLE_NONE);
	fake_advance(17600);
	CHECK(mgcp_txn_disc_active(&mgr));
	CHECK_UINT(fk.done_calls, 2);
	CHECK(fk.done_reason == MGCP_TXN_DONE_TIMEOUT);
	CHECK_UINT(mgr.disc.end_ms - t0, 18700);
	CHECK_UINT(mgr.disc.td_ms, 4000);
	CHECK_UINT(fake_timer_due(&mgr.disc.timer) - t0, 22700);
	CHECK_UINT(fk.attempt_calls, 1);

	CASE("local activity re-triggers only after Tdmin (4.4.7 step 3)");
	fake_reset();
	fk.rand_value = 14000;
	mgcp_txn_disc_enter(&mgr);
	CHECK(!mgcp_txn_disc_kick(&mgr, true));
	fake_advance(14999);
	CHECK(!mgcp_txn_disc_kick(&mgr, true));
	CHECK_UINT(fk.attempt_calls, 0);
	fake_advance(1);
	CHECK_UINT(fk.attempt_calls, 1);
	CHECK(mgcp_txn_disc_kick(&mgr, true));
	CHECK_UINT(mgr.disc.td_ms, 15000);

	CASE("a command re-triggers at once, the delay waits for its end");
	fake_reset();
	fk.rand_value = 7000;
	mgcp_txn_disc_enter(&mgr);
	fake_advance(10);
	CHECK(mgcp_txn_disc_kick(&mgr, false));
	CHECK_UINT(fake_timer_due(&mgr.disc.timer), 0);
	fake_advance(100000);
	CHECK_UINT(fk.attempt_calls, 0);
	fk.send_fail = true;
	CHECK(ntfy_send_acks(0, NULL, MGCP_TXN_F_DISC) == MGCP_TXN_HANDLE_NONE);
	CHECK_UINT(mgr.disc.td_ms, 16000);
	fake_advance(16000);
	CHECK_UINT(fk.attempt_calls, 1);
	CHECK(mgcp_txn_disc_kick(&mgr, false));
	mgcp_txn_disc_leave(&mgr);
	CHECK(!mgcp_txn_disc_kick(&mgr, false));

	CASE("an RSIP of the procedure moves the Tdmin start to its end");
	fake_reset();
	fk.rand_value = 7000;
	mgcp_txn_disc_enter(&mgr);
	fk.rand_value = 0;
	fake_advance(20000);
	ntfy_send_acks(1, NULL, MGCP_TXN_F_DISC);
	fake_advance(14400);
	CHECK_UINT(fk.done_calls, 1);
	CHECK(fk.done_reason == MGCP_TXN_DONE_TIMEOUT);
	CHECK_UINT(fk.disc_calls, 0);
	CHECK_UINT(mgr.disc.end_ms - t0, 34400);
	CHECK_UINT(mgr.disc.td_ms, 16000);
	fake_advance(14999);
	CHECK(!mgcp_txn_disc_kick(&mgr, true));
	fake_advance(1);
	CHECK(mgcp_txn_disc_kick(&mgr, true));

	CASE("disconnected does not run while the procedure is active");
	fake_reset();
	mgcp_txn_disc_enter(&mgr);
	ntfy_send(1);
	fake_advance(14400);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.disc_calls, 0);
	mgcp_txn_disc_leave(&mgr);
	ntfy_send(1);
	fake_advance(14400);
	CHECK_UINT(fk.disc_calls, 1);
}

static void cfg_tests(void)
{
	struct mgcp_txn_cfg cfg;

	CASE("the configuration defaults are the RFC values");
	mgcp_txn_cfg_default(&cfg);
	CHECK_UINT(cfg.max1, 5);
	CHECK_UINT(cfg.max2, 7);
	CHECK_UINT(cfg.rto_max_ms, 4000);
	CHECK_UINT(cfg.t_max_ms, 20000);
	CHECK_UINT(cfg.t_hist_ms, 30000);
	CHECK_UINT(cfg.longtran_ms, 5000);
	CHECK_UINT(cfg.td_init_ms, 15000);
	CHECK_UINT(cfg.td_min_ms, 15000);
	CHECK_UINT(cfg.td_max_ms, 600000);
	CHECK_UINT(cfg.rto_init_ms, 200);

	CASE("the init keeps the relations of the text (4.3, 3.5.6)");
	memset(&cfg, 0, sizeof(cfg));
	cfg.max1 = 9;
	cfg.max2 = 3;
	cfg.t_max_ms = 40000;
	cfg.t_hist_ms = 10000;
	cfg.longtran_ms = 50000;
	fake_reset_cfg(&cfg);
	CHECK_UINT(mgr.cfg.max2, 10);
	CHECK_UINT(mgr.cfg.t_hist_ms, 40000);
	CHECK_UINT(mgr.cfg.longtran_ms, 20000);
	CHECK_UINT(mgr.cfg.rto_max_ms, 4000);

	CASE("the init bounds T-HIST and Tdmax by one hour");
	mgcp_txn_cfg_default(&cfg);
	cfg.t_hist_ms = 5000000;
	cfg.td_max_ms = 9999999;
	cfg.t_max_ms = 4000000;
	fake_reset_cfg(&cfg);
	CHECK_UINT(mgr.cfg.t_hist_ms, MGCP_TXN_LONG_MAX_MS);
	CHECK_UINT(mgr.cfg.td_max_ms, MGCP_TXN_LONG_MAX_MS);
	CHECK_UINT(mgr.cfg.t_max_ms, MGCP_TXN_LONG_MAX_MS);
	CHECK(2ull * mgr.cfg.t_hist_ms <= UINT32_MAX);
	CHECK_UINT(mgr.cfg.wire_max, 4096);

	CASE("the init refuses a missing manager or a missing required hook");
	CHECK(mgcp_txn_mgr_init(NULL, &fk.env, NULL) == -EINVAL);
	{
		struct mgcp_txn_mgr spare;
		struct mgcp_txn_env env = fk.env;

		env.send = NULL;
		CHECK(mgcp_txn_mgr_init(&spare, &env, NULL) == -EINVAL);
		env = fk.env;
		env.base.timer_set = NULL;
		CHECK(mgcp_txn_mgr_init(&spare, &env, NULL) == -EINVAL);
		env = fk.env;
		CHECK(mgcp_txn_mgr_init(&spare, &env, NULL) == 0);
		mgcp_txn_mgr_done(&spare);
	}
}

int main(void)
{
	ladder_tests();
	estimator_tests();
	hist_tests();
	handshake_tests();
	response_tests();
	pool_tests();
	disc_tests();
	cfg_tests();

	mgcp_txn_mgr_done(&mgr);
	SUITE_CHECK_UINT(fake_timer_dropped, 0);

	return tu_summary("test_mgcp_txn");
}
