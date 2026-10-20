// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the transaction layer (src/proto/sip/txn.c) and the UA core
 * (src/proto/sip/ua.c).
 *
 * The layer takes its send, its clock and its timers from struct sip_txn_env,
 * so that a test can script all three. In this suite, the test advances a
 * virtual clock by hand, a capture array replaces the socket, and a timer table
 * replaces uloop. Every RFC 3261 section 17 timer is therefore observable to
 * the millisecond, and no case in the suite sleeps.
 */
#include <stdlib.h>

#include "proto/sip/ua.h"
#include "test_util.h"

#define FAKE_TIMER_MAX		256
#define FAKE_SEND_MAX		64
#define FAKE_WIRE_MAX		4096

/*
 * A request may be far larger than any response the UA builds from it: the
 * Require overflow case and the oversized Via chain both need several
 * kilobytes.
 */
#define MSG_TEXT_MAX		16384
#define MSG_STORE_MAX		8

struct fake_send {
	char buf[FAKE_WIRE_MAX + 1];
	size_t len;
	char host[64];
	uint16_t port;
};

static struct {
	struct sip_txn_env env;
	uint64_t now;

	struct sip_timer *timer[FAKE_TIMER_MAX];
	unsigned int timers;
	unsigned int fired;

	struct fake_send send[FAKE_SEND_MAX];
	unsigned int sends;
	bool send_fail;

	unsigned int resp_calls;
	unsigned int done_calls;
	unsigned int trace_calls;
	uint32_t resp_owner;
	uint32_t resp_txn;
	uint32_t resp_code;
	uint32_t done_owner;
	uint32_t done_txn;
	enum sip_txn_reason done_reason;

	bool auto_ack;
	const char *ack_wire;
	uint32_t abort_on_resp;		/* handle to abort from inside resp() */

	unsigned int ack_missing;
	unsigned int retry_after;
	unsigned int glare_retry;
	uint32_t cb_owner;
	uint32_t cb_secs;
	bool cb_present;
} fk;

static struct sip_txn_mgr mgr;
static struct ua ua;

#include "test_capture.h"
#include "test_clock.h"

/* ------------------------------------------------------------- environment */

static unsigned int trace_total;

static void fake_trace(struct proto_timer_env *env, const char *msg)
{
	fk.trace_calls++;
	trace_total++;
	(void)msg;
}

static void fake_resp(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
		      const struct sip_msg *msg)
{
	fk.resp_calls++;
	fk.resp_owner = owner;
	fk.resp_txn = txn;
	fk.resp_code = msg->status;

	if (fk.abort_on_resp == txn)
		sip_txn_abort(sip_txn_get(&mgr, txn));

	if (!fk.auto_ack || msg->status < 300)
		return;

	sip_txn_ack_set(sip_txn_get(&mgr, txn), fk.ack_wire, strlen(fk.ack_wire));
}

static void fake_done(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
		      enum sip_txn_reason reason)
{
	fk.done_calls++;
	fk.done_owner = owner;
	fk.done_txn = txn;
	fk.done_reason = reason;
}

static void fake_ack_missing(struct ua *u, uint32_t owner, void *priv)
{
	fk.ack_missing++;
	fk.cb_owner = owner;
}

static void fake_retry_after(struct ua *u, uint32_t owner, uint32_t secs, bool present,
			     void *priv)
{
	fk.retry_after++;
	fk.cb_owner = owner;
	fk.cb_secs = secs;
	fk.cb_present = present;
}

static void fake_glare_retry(struct ua *u, uint32_t owner, void *priv)
{
	fk.glare_retry++;
	fk.cb_owner = owner;
}

static const struct ua_cb ua_cb = {
	.ack_missing = fake_ack_missing,
	.retry_after = fake_retry_after,
	.glare_retry = fake_glare_retry,
};

static void fake_reset(void)
{
	struct sip_timer_cfg tc;

	sip_txn_mgr_done(&mgr);
	ua_done(&ua);

	memset(&fk, 0, sizeof(fk));
	fk.now = 100000;
	fk.env.send = fake_send_fn;
	fake_clock_bind(&fk.env);
	fk.env.base.trace = fake_trace;
	fk.env.resp = fake_resp;
	fk.env.done = fake_done;

	sip_timer_cfg_default(&tc);
	sip_txn_mgr_init(&mgr, &fk.env, &tc);
	ua_init(&ua, &mgr, &fk.env, NULL, &ua_cb, NULL);
}

/* ---------------------------------------------------------------- messages */

static char store[MSG_STORE_MAX][MSG_TEXT_MAX];
static struct sip_msg stored[MSG_STORE_MAX];
static unsigned int store_next;

/*
 * marker, when not 0, becomes a NUL byte in the stored copy. It is the only
 * way to put an embedded NUL into a test message, and an embedded NUL is what
 * a key comparator has to survive.
 */
static struct sip_msg *msg_take_mark(const char *text, char marker)
{
	unsigned int i = store_next++ % MSG_STORE_MAX;
	struct sip_msg *m = &stored[i];
	size_t len = strlen(text), n;

	memcpy(store[i], text, len);
	for (n = 0; marker && n < len; n++) {
		if (store[i][n] == marker)
			store[i][n] = 0;
	}

	if (msg_parse(m, store[i], len) != SIP_PARSE_OK)
		return NULL;

	return m;
}

static struct sip_msg *msg_take(const char *text)
{
	return msg_take_mark(text, 0);
}

/*
 * Eight rotating buffers per builder, so a test can hold on to one text while
 * it builds the next one.
 */
static char text_buf[8][MSG_TEXT_MAX];
static unsigned int text_next;

static char *text_slot(void)
{
	return text_buf[text_next++ % 8];
}

static const char *req_text(const char *method, const char *branch, const char *sentby,
			    unsigned int cseq, const char *to_tag, const char *extra)
{
	char *buf = text_slot();

	snprintf(buf, MSG_TEXT_MAX,
		 "%s sip:5552000@example.net SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP %s;branch=%s\r\n"
		 "Max-Forwards: 70\r\n"
		 "From: \"Caller\" <sip:5551000@carrier.net>;tag=remotetag\r\n"
		 "To: <sip:5552000@example.net>%s%s\r\n"
		 "Call-ID: call-abc@10.0.0.9\r\n"
		 "CSeq: %u %s\r\n"
		 "%s"
		 "Content-Length: 0\r\n"
		 "\r\n",
		 method, sentby, branch, to_tag ? ";tag=" : "", to_tag ? to_tag : "",
		 cseq, method, extra ? extra : "");

	return buf;
}

static const char *resp_text(uint32_t code, const char *branch, const char *sentby,
			     unsigned int cseq, const char *method, const char *to_tag)
{
	char *buf = text_slot();

	snprintf(buf, MSG_TEXT_MAX,
		 "SIP/2.0 %u %s\r\n"
		 "Via: SIP/2.0/UDP %s;branch=%s\r\n"
		 "From: \"Caller\" <sip:5551000@carrier.net>;tag=remotetag\r\n"
		 "To: <sip:5552000@example.net>%s%s\r\n"
		 "Call-ID: call-abc@10.0.0.9\r\n"
		 "CSeq: %u %s\r\n"
		 "Content-Length: 0\r\n"
		 "\r\n",
		 code, sip_status_reason(code), sentby, branch, to_tag ? ";tag=" : "",
		 to_tag ? to_tag : "", cseq, method);

	return buf;
}

static enum sip_txn_rx rx_mark(const char *text, char marker)
{
	struct sip_msg *m = msg_take_mark(text, marker);

	if (!m)
		return SIP_RX_IGNORED;

	return sip_txn_msg_recv(&mgr, m, NULL);
}

static enum sip_txn_rx rx(const char *text)
{
	return rx_mark(text, 0);
}

static uint32_t send_status(unsigned int idx)
{
	struct sip_msg m;

	if (!send_parse(idx, &m) || m.is_request)
		return 0;

	return m.status;
}

static bool send_has(unsigned int idx, const char *needle)
{
	return strstr(send_text(idx), needle) != NULL;
}

static struct sip_txn_dst *dst_make(const char *host, uint16_t port)
{
	static struct sip_txn_dst dst;

	sip_dst_set(&dst, host, port);

	return &dst;
}

static const char *invite_wire = "INVITE sip:5552000@carrier.net SIP/2.0\r\n"
				 "Via: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bK-c1\r\n"
				 "From: <sip:5551000@carrier.net>;tag=localtag\r\n"
				 "To: <sip:5552000@carrier.net>\r\n"
				 "Call-ID: out-1@10.0.0.1\r\n"
				 "CSeq: 1 INVITE\r\n"
				 "Content-Length: 0\r\n\r\n";

static const char *ack_wire = "ACK sip:5552000@carrier.net SIP/2.0\r\n"
			      "Via: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bK-c1\r\n"
			      "From: <sip:5551000@carrier.net>;tag=localtag\r\n"
			      "To: <sip:5552000@carrier.net>;tag=peer\r\n"
			      "Call-ID: out-1@10.0.0.1\r\n"
			      "CSeq: 1 ACK\r\n"
			      "Content-Length: 0\r\n\r\n";

static struct sip_txn *ict_start(uint32_t owner)
{
	return sip_txn_client_start(&mgr, SIP_METHOD_INVITE, sip_str_cstr("z9hG4bK-c1"),
				    invite_wire, strlen(invite_wire),
				    dst_make("192.0.2.10", 5060), owner);
}

static struct sip_txn *nict_start(uint32_t owner)
{
	static const char *wire = "OPTIONS sip:carrier.net SIP/2.0\r\n"
				  "Via: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bK-c2\r\n"
				  "CSeq: 4 OPTIONS\r\n\r\n";

	return sip_txn_client_start(&mgr, SIP_METHOD_OPTIONS, sip_str_cstr("z9hG4bK-c2"),
				    wire, strlen(wire), dst_make("192.0.2.10", 5060), owner);
}

static struct sip_txn *server_start_mark(const char *text, char marker, const char *host,
					 uint16_t port)
{
	struct sip_msg *m = msg_take_mark(text, marker);
	struct sip_txn_dst src;

	if (!m)
		return NULL;

	sip_dst_set(&src, host, port);

	return sip_txn_server_start(&mgr, m, &src, 0x00010001);
}

static struct sip_txn *server_start(const char *text, const char *host, uint16_t port)
{
	return server_start_mark(text, 0, host, port);
}

static const char *fill_text(char *out, size_t olen, char c, size_t n)
{
	if (n >= olen)
		n = olen - 1;

	memset(out, c, n);
	out[n] = 0;

	return out;
}

/* -------------------------------------------------------------- timer cfg */

static void timer_cfg_tests(void)
{
	struct sip_timer_cfg tc;

	CASE("timer defaults are the RFC 3261 values");
	sip_timer_cfg_default(&tc);
	CHECK_UINT(tc.t1, 500);
	CHECK_UINT(tc.t2, 4000);
	CHECK_UINT(tc.t4, 5000);
	CHECK_UINT(tc.a, 500);
	CHECK_UINT(tc.b, 32000);
	CHECK_UINT(tc.d, 32000);
	CHECK_UINT(tc.e, 500);
	CHECK_UINT(tc.f, 32000);
	CHECK_UINT(tc.g, 500);
	CHECK_UINT(tc.h, 32000);
	CHECK_UINT(tc.i, 5000);
	CHECK_UINT(tc.j, 32000);
	CHECK_UINT(tc.k, 5000);

	CASE("a config that sets T1 alone derives the whole set");
	memset(&tc, 0, sizeof(tc));
	tc.t1 = 250;
	sip_timer_cfg_fixup(&tc);
	CHECK_UINT(tc.a, 250);
	CHECK_UINT(tc.b, 16000);
	CHECK_UINT(tc.f, 16000);
	CHECK_UINT(tc.h, 16000);
	CHECK_UINT(tc.j, 16000);
	CHECK_UINT(tc.t2, 4000);

	CASE("explicit values survive the fixup");
	memset(&tc, 0, sizeof(tc));
	tc.t1 = 500;
	tc.b = 6000;
	tc.k = 1000;
	sip_timer_cfg_fixup(&tc);
	CHECK_UINT(tc.b, 6000);
	CHECK_UINT(tc.k, 1000);
	CHECK_UINT(tc.f, 32000);

	CASE("nonsense values are clamped, not accepted");
	memset(&tc, 0, sizeof(tc));
	tc.t1 = 1;
	tc.t2 = 5;
	tc.d = 9999999;
	sip_timer_cfg_fixup(&tc);
	CHECK_UINT(tc.t1, 10);
	CHECK(tc.t2 >= tc.t1);
	CHECK_UINT(tc.d, 600000);
}

/* ---------------------------------------------------------------- handles */

static void handle_tests(void)
{
	struct sip_txn *txn;
	uint32_t h1, h2;

	CASE("a handle carries index and generation");
	CHECK_UINT(sip_handle_index(sip_handle_make(7, 3)), 7);
	CHECK_UINT(sip_handle_gen(sip_handle_make(7, 3)), 3);
	CHECK(sip_handle_make(0, 1) != SIP_HANDLE_NONE);

	fake_reset();

	CASE("a live handle resolves to its transaction");
	txn = ict_start(0x00010001);
	CHECK(txn != NULL);
	h1 = sip_txn_handle(txn);
	CHECK(sip_txn_get(&mgr, h1) == txn);
	CHECK_UINT(sip_txn_count(&mgr), 1);

	CASE("a handle of a finished transaction resolves to NULL");
	sip_txn_abort(txn);
	CHECK(sip_txn_get(&mgr, h1) == NULL);
	CHECK_UINT(sip_txn_count(&mgr), 0);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_ABORT);

	CASE("a reused pool slot gets a new generation");
	txn = ict_start(0x00010001);
	h2 = sip_txn_handle(txn);
	CHECK(h1 != h2);
	CHECK(sip_txn_get(&mgr, h1) == NULL);
	CHECK(sip_txn_get(&mgr, h2) == txn);

	CASE("a garbage handle resolves to NULL");
	CHECK(sip_txn_get(&mgr, 0xffffffffu) == NULL);
	CHECK(sip_txn_get(&mgr, SIP_HANDLE_NONE) == NULL);
}

/* ----------------------------------------------------------- destinations */

static void dst_tests(void)
{
	struct sip_txn_dst a, b;
	struct sip_msg *m;
	char host[64];

	CASE("an IPv4 destination round trips");
	CHECK(sip_dst_set(&a, "192.0.2.10", 5062));
	CHECK(sip_dst_host(&a, host, sizeof(host)));
	CHECK(!strcmp(host, "192.0.2.10"));
	CHECK_UINT(sip_dst_port(&a), 5062);

	CASE("an IPv6 destination round trips");
	CHECK(sip_dst_set(&b, "2001:db8::1", 5060));
	CHECK(sip_dst_host(&b, host, sizeof(host)));
	CHECK(!strcmp(host, "2001:db8::1"));
	CHECK(!sip_dst_eq(&a, &b));

	CASE("a host name is refused: this layer has no resolver");
	CHECK(!sip_dst_set(&a, "proxy.carrier.net", 5060));
	CHECK(!sip_dst_set(&a, NULL, 5060));

	CASE("equal destinations compare equal, a port change does not");
	CHECK(sip_dst_set(&a, "192.0.2.10", 5060));
	CHECK(sip_dst_set(&b, "192.0.2.10", 5060));
	CHECK(sip_dst_eq(&a, &b));
	CHECK(sip_dst_set(&b, "192.0.2.10", 5061));
	CHECK(!sip_dst_eq(&a, &b));

	CASE("rport sends the response to the source port (RFC 3581)");
	m = msg_take(req_text("OPTIONS", "z9hG4bK-r1", "10.0.0.9:5060;rport", 1, NULL, NULL));
	CHECK(m != NULL);
	CHECK(sip_dst_set(&a, "10.0.0.9", 41234));
	CHECK(sip_dst_from_via(m, &a, &b));
	CHECK_UINT(sip_dst_port(&b), 41234);

	CASE("without rport the response goes to the sent-by port");
	m = msg_take(req_text("OPTIONS", "z9hG4bK-r2", "10.0.0.9:5070", 1, NULL, NULL));
	CHECK(m != NULL);
	CHECK(sip_dst_from_via(m, &a, &b));
	CHECK_UINT(sip_dst_port(&b), 5070);
	CHECK(sip_dst_host(&b, host, sizeof(host)));
	CHECK(!strcmp(host, "10.0.0.9"));

	CASE("a sent-by without a port means 5060");
	m = msg_take(req_text("OPTIONS", "z9hG4bK-r3", "10.0.0.9", 1, NULL, NULL));
	CHECK(m != NULL);
	CHECK(sip_dst_from_via(m, &a, &b));
	CHECK_UINT(sip_dst_port(&b), 5060);

	CASE("rport with a value still means the source port");
	m = msg_take(req_text("OPTIONS", "z9hG4bK-r4", "10.0.0.9:5060;rport=9", 1, NULL,
			      NULL));
	CHECK(m != NULL);
	CHECK(sip_dst_from_via(m, &a, &b));
	CHECK_UINT(sip_dst_port(&b), 41234);
}

static void branch_tests(void)
{
	char b1[SIP_BRANCH_MAX], b2[SIP_BRANCH_MAX];
	char small[8];

	fake_reset();

	CASE("a generated branch carries the RFC 3261 magic cookie");
	CHECK(sip_branch_new(&mgr, b1, sizeof(b1)));
	CHECK(!strncmp(b1, SIP_BRANCH_MAGIC, strlen(SIP_BRANCH_MAGIC)));
	CHECK(strlen(b1) > strlen(SIP_BRANCH_MAGIC) + 8);

	CASE("two branches differ");
	CHECK(sip_branch_new(&mgr, b2, sizeof(b2)));
	CHECK(strcmp(b1, b2) != 0);

	CASE("a short buffer is refused, never truncated");
	CHECK(!sip_branch_new(&mgr, small, sizeof(small)));
}

/* --------------------------------------------------- INVITE client (17.1.1) */

static void ict_tests(void)
{
	struct sip_txn *txn;
	uint32_t handle;

	fake_reset();

	CASE("the request goes out once when the transaction starts");
	txn = ict_start(0x00010001);
	CHECK(txn != NULL);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(txn->state, SIP_TXN_S_CALLING);
	CHECK_UINT(txn->kind, SIP_TXN_ICT);

	CASE("timer A doubles: 500, 1000, 2000, 4000, 8000, 16000");
	fake_advance(499);
	CHECK_UINT(fk.sends, 1);
	fake_advance(1);
	CHECK_UINT(fk.sends, 2);
	fake_advance(999);
	CHECK_UINT(fk.sends, 2);
	fake_advance(1);
	CHECK_UINT(fk.sends, 3);
	fake_advance(2000);
	CHECK_UINT(fk.sends, 4);
	fake_advance(4000);
	CHECK_UINT(fk.sends, 5);
	fake_advance(8000);
	CHECK_UINT(fk.sends, 6);
	fake_advance(16000);
	CHECK_UINT(fk.sends, 7);

	CASE("timer B ends the transaction at 64*T1 with a timeout");
	CHECK_UINT(fk.done_calls, 0);
	fake_advance(500);
	CHECK_UINT(fk.sends, 7);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_TIMEOUT);
	CHECK_UINT(fk.done_owner, 0x00010001);
	CHECK_UINT(sip_txn_count(&mgr), 0);
	CHECK_UINT(mgr.stat.timeout, 1);

	fake_reset();

	CASE("a provisional stops the retransmissions and the timeout");
	txn = ict_start(0x00010001);
	fake_advance(600);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(rx(resp_text(180, "z9hG4bK-c1", "10.0.0.9:5060", 1, "INVITE", "peer")),
		   SIP_RX_DELIVERED);
	CHECK_UINT(txn->state, SIP_TXN_S_PROCEEDING);
	CHECK_UINT(fk.resp_calls, 1);
	CHECK_UINT(fk.resp_code, 180);
	fake_advance(60000);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(fk.done_calls, 0);

	CASE("a second provisional is delivered too");
	CHECK_UINT(rx(resp_text(183, "z9hG4bK-c1", "10.0.0.9:5060", 1, "INVITE", "peer")),
		   SIP_RX_DELIVERED);
	CHECK_UINT(fk.resp_calls, 2);
	CHECK_UINT(txn->state, SIP_TXN_S_PROCEEDING);

	CASE("a 2xx terminates the client transaction at once");
	handle = sip_txn_handle(txn);
	CHECK_UINT(rx(resp_text(200, "z9hG4bK-c1", "10.0.0.9:5060", 1, "INVITE", "peer")),
		   SIP_RX_DELIVERED);
	CHECK_UINT(fk.resp_calls, 3);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_OK);
	CHECK(sip_txn_get(&mgr, handle) == NULL);

	CASE("a retransmitted 2xx no longer matches: it is the dialog's problem");
	CHECK_UINT(rx(resp_text(200, "z9hG4bK-c1", "10.0.0.9:5060", 1, "INVITE", "peer")),
		   SIP_RX_RESPONSE_STRAY);
	CHECK_UINT(mgr.stat.stray, 1);

	fake_reset();
	fk.auto_ack = true;
	fk.ack_wire = ack_wire;

	CASE("a non-2xx final moves to completed and the TU supplies the ACK");
	txn = ict_start(0x00010001);
	CHECK_UINT(rx(resp_text(486, "z9hG4bK-c1", "10.0.0.9:5060", 1, "INVITE", "peer")),
		   SIP_RX_DELIVERED);
	CHECK_UINT(txn->state, SIP_TXN_S_COMPLETED);
	CHECK_UINT(fk.sends, 2);
	CHECK(!strncmp(last_text(), "ACK ", 4));
	CHECK_UINT(txn->code, 486);

	CASE("a retransmitted final is absorbed and answered with the same ACK");
	CHECK_UINT(rx(resp_text(486, "z9hG4bK-c1", "10.0.0.9:5060", 1, "INVITE", "peer")),
		   SIP_RX_ABSORBED);
	CHECK_UINT(fk.resp_calls, 1);
	CHECK_UINT(fk.sends, 3);
	CHECK(!strncmp(last_text(), "ACK ", 4));

	CASE("no request retransmission happens while completed");
	fake_advance(31000);
	CHECK_UINT(fk.sends, 3);
	CHECK_UINT(fk.done_calls, 0);

	CASE("timer D ends the completed state at 32 s");
	fake_advance(1000);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_OK);
	CHECK_UINT(sip_txn_count(&mgr), 0);

	fake_reset();

	CASE("an ACK cannot be handed to a transaction that is not completed");
	txn = ict_start(0x00010001);
	CHECK(!sip_txn_ack_set(txn, ack_wire, strlen(ack_wire)));

	CASE("a transport error on the first send yields no transaction");
	fake_reset();
	fk.send_fail = true;
	CHECK(ict_start(0x00010001) == NULL);
	CHECK_UINT(sip_txn_count(&mgr), 0);
	CHECK_UINT(fk.done_calls, 0);

	CASE("a transport error while retransmitting terminates the transaction");
	fake_reset();
	txn = ict_start(0x00010001);
	CHECK(txn != NULL);
	fk.send_fail = true;
	fake_advance(600);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_TRANSPORT);
	CHECK_UINT(mgr.stat.transport, 1);

	fake_reset();

	CASE("the TU may abort the transaction from inside the response upcall");
	txn = ict_start(0x00010001);
	fk.abort_on_resp = sip_txn_handle(txn);
	CHECK_UINT(rx(resp_text(200, "z9hG4bK-c1", "10.0.0.9:5060", 1, "INVITE", "peer")),
		   SIP_RX_DELIVERED);
	CHECK_UINT(sip_txn_count(&mgr), 0);
	CHECK_UINT(fk.done_calls, 1);
}

/* ----------------------------------------------- non-INVITE client (17.1.2) */

static void nict_tests(void)
{
	struct sip_txn *txn;

	fake_reset();

	CASE("a non-INVITE client transaction starts in trying");
	txn = nict_start(0x00020001);
	CHECK(txn != NULL);
	CHECK_UINT(txn->kind, SIP_TXN_NICT);
	CHECK_UINT(txn->state, SIP_TXN_S_TRYING);
	CHECK_UINT(fk.sends, 1);

	CASE("timer E doubles up to T2: 500, 1000, 2000, 4000, 4000");
	fake_advance(500);
	CHECK_UINT(fk.sends, 2);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 3);
	fake_advance(2000);
	CHECK_UINT(fk.sends, 4);
	fake_advance(4000);
	CHECK_UINT(fk.sends, 5);
	fake_advance(3999);
	CHECK_UINT(fk.sends, 5);
	fake_advance(1);
	CHECK_UINT(fk.sends, 6);

	CASE("timer F ends the transaction at 64*T1");
	fake_advance(32000 - 11500);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_TIMEOUT);
	CHECK_UINT(fk.sends, 11);

	fake_reset();

	CASE("a provisional moves to proceeding and pins timer E at T2");
	txn = nict_start(0x00020001);
	fake_advance(400);
	CHECK_UINT(rx(resp_text(100, "z9hG4bK-c2", "10.0.0.9:5060", 4, "OPTIONS", "peer")),
		   SIP_RX_DELIVERED);
	CHECK_UINT(txn->state, SIP_TXN_S_PROCEEDING);
	fake_advance(100);
	CHECK_UINT(fk.sends, 2);
	fake_advance(3999);
	CHECK_UINT(fk.sends, 2);
	fake_advance(1);
	CHECK_UINT(fk.sends, 3);

	CASE("a final response completes the transaction and starts timer K");
	CHECK_UINT(rx(resp_text(200, "z9hG4bK-c2", "10.0.0.9:5060", 4, "OPTIONS", "peer")),
		   SIP_RX_DELIVERED);
	CHECK_UINT(txn->state, SIP_TXN_S_COMPLETED);
	CHECK_UINT(fk.resp_calls, 2);

	CASE("a retransmitted final is absorbed, never delivered twice");
	CHECK_UINT(rx(resp_text(200, "z9hG4bK-c2", "10.0.0.9:5060", 4, "OPTIONS", "peer")),
		   SIP_RX_ABSORBED);
	CHECK_UINT(fk.resp_calls, 2);
	CHECK_UINT(mgr.stat.absorbed, 1);

	CASE("nothing more goes on the wire while completed");
	fake_advance(4999);
	CHECK_UINT(fk.sends, 3);
	CHECK_UINT(fk.done_calls, 0);

	CASE("timer K ends the completed state at T4");
	fake_advance(1);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_OK);
	CHECK_UINT(sip_txn_count(&mgr), 0);
}

/* --------------------------------------------------- INVITE server (17.2.1) */

static void ist_tests(void)
{
	struct sip_txn *txn;
	uint32_t handle;
	const char *invite;

	fake_reset();
	invite = req_text("INVITE", "z9hG4bK-s1", "10.0.0.9:5060;rport", 1, NULL, NULL);

	CASE("an INVITE server transaction starts in proceeding");
	txn = server_start(invite, "10.0.0.9", 41234);
	CHECK(txn != NULL);
	CHECK_UINT(txn->kind, SIP_TXN_IST);
	CHECK_UINT(txn->state, SIP_TXN_S_PROCEEDING);
	CHECK_UINT(fk.sends, 0);

	CASE("the response destination follows the rport rule");
	CHECK_UINT(sip_dst_port(sip_txn_dst_get(txn)), 41234);

	CASE("a retransmitted INVITE before any response sends nothing");
	CHECK_UINT(rx(invite), SIP_RX_ABSORBED);
	CHECK_UINT(fk.sends, 0);

	CASE("a provisional goes out and answers the next retransmission");
	CHECK(sip_txn_response_send(txn, 180, "SIP/2.0 180 Ringing\r\n\r\n", 23));
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(rx(invite), SIP_RX_ABSORBED);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(txn->state, SIP_TXN_S_PROCEEDING);

	CASE("a non-2xx final completes the transaction");
	CHECK(sip_txn_response_send(txn, 486, "SIP/2.0 486 Busy Here\r\n\r\n", 25));
	CHECK_UINT(txn->state, SIP_TXN_S_COMPLETED);
	CHECK_UINT(fk.sends, 3);

	CASE("timer G retransmits the final, doubling up to T2");
	fake_advance(500);
	CHECK_UINT(fk.sends, 4);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 5);
	fake_advance(2000);
	CHECK_UINT(fk.sends, 6);
	fake_advance(4000);
	CHECK_UINT(fk.sends, 7);
	fake_advance(4000);
	CHECK_UINT(fk.sends, 8);

	CASE("a retransmitted INVITE while completed repeats the final");
	CHECK_UINT(rx(invite), SIP_RX_ABSORBED);
	CHECK_UINT(fk.sends, 9);

	CASE("the ACK confirms the transaction and stops timers G and H");
	CHECK_UINT(rx(req_text("ACK", "z9hG4bK-s1", "10.0.0.9:5060;rport", 1, NULL, NULL)),
		   SIP_RX_ABSORBED);
	CHECK_UINT(txn->state, SIP_TXN_S_CONFIRMED);
	fake_advance(4999);
	CHECK_UINT(fk.sends, 9);
	CHECK_UINT(fk.done_calls, 0);

	CASE("a retransmitted ACK is absorbed while confirmed");
	CHECK_UINT(rx(req_text("ACK", "z9hG4bK-s1", "10.0.0.9:5060;rport", 1, NULL, NULL)),
		   SIP_RX_ABSORBED);
	CHECK_UINT(fk.sends, 9);

	CASE("timer I ends the confirmed state at T4");
	fake_advance(5000);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_OK);
	CHECK_UINT(sip_txn_count(&mgr), 0);

	fake_reset();
	invite = req_text("INVITE", "z9hG4bK-s2", "10.0.0.9:5060", 1, NULL, NULL);

	CASE("timer H reports the missing ACK at 64*T1");
	txn = server_start(invite, "10.0.0.9", 41234);
	CHECK(sip_txn_response_send(txn, 500, "SIP/2.0 500 Server Error\r\n\r\n", 28));
	fake_advance(31999);
	CHECK_UINT(fk.done_calls, 0);
	fake_advance(1);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_TIMEOUT);

	CASE("the retransmission count over 64*T1 matches the G series");
	CHECK_UINT(fk.sends, 11);

	fake_reset();
	invite = req_text("INVITE", "z9hG4bK-s3", "10.0.0.9:5060", 1, NULL, NULL);

	CASE("a 2xx terminates the server transaction, ua.c takes over");
	txn = server_start(invite, "10.0.0.9", 41234);
	handle = sip_txn_handle(txn);
	CHECK(sip_txn_response_send(txn, 200, "SIP/2.0 200 OK\r\n\r\n", 18));
	CHECK(sip_txn_get(&mgr, handle) == NULL);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_OK);

	CASE("the ACK for that 2xx matches no transaction");
	CHECK_UINT(rx(req_text("ACK", "z9hG4bK-s3", "10.0.0.9:5060", 1, NULL, NULL)),
		   SIP_RX_ACK_2XX);

	CASE("an ACK with its own branch matches no transaction either");
	CHECK_UINT(rx(req_text("ACK", "z9hG4bK-s3ack", "10.0.0.9:5060", 1, NULL, NULL)),
		   SIP_RX_ACK_2XX);

	fake_reset();
	invite = req_text("INVITE", "z9hG4bK-s4", "10.0.0.9:5060", 1, NULL, NULL);

	CASE("a second final response is refused");
	txn = server_start(invite, "10.0.0.9", 41234);
	CHECK(sip_txn_response_send(txn, 486, "SIP/2.0 486 Busy Here\r\n\r\n", 25));
	CHECK(!sip_txn_response_send(txn, 603, "SIP/2.0 603 Decline\r\n\r\n", 23));
	CHECK_UINT(fk.sends, 1);

	CASE("an ACK that arrives while proceeding is absorbed, not confirmed");
	fake_reset();
	invite = req_text("INVITE", "z9hG4bK-s5", "10.0.0.9:5060", 1, NULL, NULL);
	txn = server_start(invite, "10.0.0.9", 41234);
	CHECK_UINT(rx(req_text("ACK", "z9hG4bK-s5", "10.0.0.9:5060", 1, NULL, NULL)),
		   SIP_RX_ABSORBED);
	CHECK_UINT(txn->state, SIP_TXN_S_PROCEEDING);
}

/* ------------------------------------------ reliable provisional (RFC 3262) */

/*
 * sip_txn_response_resend() repeats a reliable provisional for a transaction
 * user that owns the RFC 3262 3 schedule. The guard and the transport error
 * are therefore the whole contract. Only a proceeding INVITE server
 * transaction holds a provisional to repeat, and a refused send ends that
 * transaction.
 */
static void resend_tests(void)
{
	struct sip_timer_cfg tc;
	const struct sip_timer_cfg *timers;
	struct sip_txn *txn;
	uint32_t handle;
	const char *invite, *bye;

	fake_reset();
	invite = req_text("INVITE", "z9hG4bK-r1", "10.0.0.9:5060", 1,
			  NULL, NULL);

	CASE("a resend puts the retained provisional on the wire again");
	txn = server_start(invite, "10.0.0.9", 41234);
	CHECK(txn != NULL);
	CHECK(sip_txn_response_send(txn, 180, "SIP/2.0 180 Ringing\r\n\r\n",
				    23));
	CHECK_UINT(fk.sends, 1);
	CHECK(sip_txn_response_resend(txn));
	CHECK_UINT(fk.sends, 2);
	CHECK(!strcmp(fk.send[0].buf, fk.send[1].buf));
	CHECK_UINT(send_status(1), 180);
	CHECK_UINT(txn->state, SIP_TXN_S_PROCEEDING);

	CASE("a resend after the final response is refused and sends nothing");
	CHECK(sip_txn_response_send(txn, 486, "SIP/2.0 486 Busy Here\r\n\r\n",
				    25));
	CHECK_UINT(fk.sends, 3);
	CHECK(!sip_txn_response_resend(txn));
	CHECK_UINT(fk.sends, 3);

	CASE("a resend on a non-INVITE server transaction is refused");
	fake_reset();
	bye = req_text("BYE", "z9hG4bK-r2", "10.0.0.9:5060", 7, NULL, NULL);
	txn = server_start(bye, "10.0.0.9", 5060);
	CHECK(txn != NULL);
	CHECK(sip_txn_response_send(txn, 100, "SIP/2.0 100 Trying\r\n\r\n",
				    22));
	CHECK_UINT(txn->state, SIP_TXN_S_PROCEEDING);
	CHECK_UINT(fk.sends, 1);
	CHECK(!sip_txn_response_resend(txn));
	CHECK_UINT(fk.sends, 1);

	CASE("a resend on an INVITE client transaction is refused");
	fake_reset();
	txn = ict_start(0x00010001);
	CHECK(txn != NULL);
	CHECK_UINT(fk.sends, 1);
	CHECK(!sip_txn_response_resend(txn));
	CHECK_UINT(fk.sends, 1);

	CASE("a resend without a transaction is refused");
	CHECK(!sip_txn_response_resend(NULL));

	CASE("a refused send ends the transaction with a transport error");
	fake_reset();
	invite = req_text("INVITE", "z9hG4bK-r3", "10.0.0.9:5060", 1,
			  NULL, NULL);
	txn = server_start(invite, "10.0.0.9", 41234);
	CHECK(txn != NULL);
	handle = sip_txn_handle(txn);
	CHECK(sip_txn_response_send(txn, 180, "SIP/2.0 180 Ringing\r\n\r\n",
				    23));
	CHECK_UINT(fk.sends, 1);
	fk.send_fail = true;
	CHECK(!sip_txn_response_resend(txn));
	CHECK(sip_txn_get(&mgr, handle) == NULL);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_txn, handle);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_TRANSPORT);
	fk.send_fail = false;

	CASE("the reliable provisional timer carries its own name");
	CHECK(!strcmp(sip_timer_name(SIP_TIMER_CALL_REL), "call-rel"));

	CASE("the manager reports the timer values it holds");
	memset(&tc, 0, sizeof(tc));
	tc.t1 = 250;
	sip_txn_mgr_timers_set(&mgr, &tc);
	timers = sip_txn_mgr_timers_get(&mgr);
	CHECK(timers != NULL);
	CHECK_UINT(timers->t1, 250);
	CHECK_UINT(timers->b, 16000);
	CHECK_UINT(timers->t2, 4000);

	fake_reset();
}

/* ----------------------------------------------- non-INVITE server (17.2.2) */

static void nist_tests(void)
{
	struct sip_txn *txn;
	const char *bye;

	fake_reset();
	bye = req_text("BYE", "z9hG4bK-s6", "10.0.0.9:5060", 7, NULL, NULL);

	CASE("a non-INVITE server transaction starts in trying");
	txn = server_start(bye, "10.0.0.9", 5060);
	CHECK(txn != NULL);
	CHECK_UINT(txn->kind, SIP_TXN_NIST);
	CHECK_UINT(txn->state, SIP_TXN_S_TRYING);

	CASE("a retransmission while trying sends nothing");
	CHECK_UINT(rx(bye), SIP_RX_ABSORBED);
	CHECK_UINT(fk.sends, 0);

	CASE("a provisional moves to proceeding and answers retransmissions");
	CHECK(sip_txn_response_send(txn, 100, "SIP/2.0 100 Trying\r\n\r\n", 22));
	CHECK_UINT(txn->state, SIP_TXN_S_PROCEEDING);
	CHECK_UINT(rx(bye), SIP_RX_ABSORBED);
	CHECK_UINT(fk.sends, 2);

	CASE("a final response completes the transaction and starts timer J");
	CHECK(sip_txn_response_send(txn, 200, "SIP/2.0 200 OK\r\n\r\n", 18));
	CHECK_UINT(txn->state, SIP_TXN_S_COMPLETED);
	CHECK_UINT(fk.sends, 3);

	CASE("a retransmission while completed repeats the final");
	CHECK_UINT(rx(bye), SIP_RX_ABSORBED);
	CHECK_UINT(fk.sends, 4);
	CHECK(send_status(3) == 200);

	CASE("timer J ends the completed state at 64*T1");
	fake_advance(31999);
	CHECK_UINT(fk.done_calls, 0);
	fake_advance(1);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_OK);
	CHECK_UINT(sip_txn_count(&mgr), 0);

	CASE("a server transaction is never created for an ACK");
	fake_reset();
	CHECK(server_start(req_text("ACK", "z9hG4bK-s7", "10.0.0.9:5060", 1, NULL, NULL),
			   "10.0.0.9", 5060) == NULL);
}

/* --------------------------------------------------------------- matching */

static void match_tests(void)
{
	struct sip_txn *a, *b;

	fake_reset();

	CASE("the same branch from a different sent-by is a different transaction");
	a = server_start(req_text("OPTIONS", "z9hG4bK-m1", "10.0.0.9:5060", 1, NULL, NULL),
			 "10.0.0.9", 5060);
	b = server_start(req_text("OPTIONS", "z9hG4bK-m1", "10.0.0.8:5060", 1, NULL, NULL),
			 "10.0.0.8", 5060);
	CHECK(a != NULL);
	CHECK(b != NULL);
	CHECK(a != b);
	CHECK_UINT(sip_txn_count(&mgr), 2);

	CASE("the same branch and sent-by with another method is a different transaction");
	CHECK(server_start(req_text("INFO", "z9hG4bK-m1", "10.0.0.9:5060", 2, NULL, NULL),
			   "10.0.0.9", 5060) != NULL);
	CHECK_UINT(sip_txn_count(&mgr), 3);

	CASE("the identical request finds the existing transaction");
	CHECK(server_start(req_text("OPTIONS", "z9hG4bK-m1", "10.0.0.9:5060", 1, NULL, NULL),
			   "10.0.0.9", 5060) == NULL);
	CHECK_UINT(sip_txn_count(&mgr), 3);

	CASE("the sent-by host is matched case insensitively");
	CHECK(server_start(req_text("OPTIONS", "z9hG4bK-m2", "Host.Example:5060", 3, NULL,
				    NULL), "10.0.0.9", 5060) != NULL);
	CHECK(server_start(req_text("OPTIONS", "z9hG4bK-m2", "host.example:5060", 3, NULL,
				    NULL), "10.0.0.9", 5060) == NULL);

	fake_reset();

	CASE("a request without Via gets no transaction and no match");
	CHECK_UINT(rx("OPTIONS sip:a@b SIP/2.0\r\nCSeq: 1 OPTIONS\r\n\r\n"), SIP_RX_IGNORED);

	CASE("a request with a branch that predates RFC 3261 still matches itself");
	CHECK(server_start(req_text("OPTIONS", "legacy-1", "10.0.0.9:5060", 5, NULL, NULL),
			   "10.0.0.9", 5060) != NULL);
	CHECK_UINT(rx(req_text("OPTIONS", "legacy-1", "10.0.0.9:5060", 5, NULL, NULL)),
		   SIP_RX_ABSORBED);

	CASE("a pre-RFC 3261 request with another CSeq is a different "
	     "transaction");
	CHECK_UINT(rx(req_text("OPTIONS", "legacy-1", "10.0.0.9:5060", 6, NULL, NULL)),
		   SIP_RX_REQUEST_NEW);

	fake_reset();

	CASE("a response matches on branch and CSeq method together");
	CHECK(ict_start(0x00010001) != NULL);
	CHECK_UINT(rx(resp_text(200, "z9hG4bK-c1", "10.0.0.9:5060", 1, "CANCEL", "peer")),
		   SIP_RX_RESPONSE_STRAY);
	CHECK_UINT(fk.resp_calls, 0);
	CHECK_UINT(rx(resp_text(200, "z9hG4bK-c1", "10.0.0.9:5060", 1, "INVITE", "peer")),
		   SIP_RX_DELIVERED);

	CASE("a response with an unknown branch is stray");
	fake_reset();
	CHECK(ict_start(0x00010001) != NULL);
	CHECK_UINT(rx(resp_text(200, "z9hG4bK-other", "10.0.0.9:5060", 1, "INVITE", "peer")),
		   SIP_RX_RESPONSE_STRAY);

	CASE("a response without a Via branch is ignored");
	CHECK_UINT(rx("SIP/2.0 200 OK\r\nVia: SIP/2.0/UDP 10.0.0.9\r\n"
		      "CSeq: 1 INVITE\r\n\r\n"), SIP_RX_IGNORED);

	CASE("a response with a CSeq method of ACK is ignored");
	CHECK_UINT(rx(resp_text(200, "z9hG4bK-c1", "10.0.0.9:5060", 1, "ACK", "peer")),
		   SIP_RX_IGNORED);

	CASE("the same branch may serve an INVITE and its CANCEL as client");
	fake_reset();
	CHECK(ict_start(0x00010001) != NULL);
	CHECK(sip_txn_client_start(&mgr, SIP_METHOD_CANCEL, sip_str_cstr("z9hG4bK-c1"),
				   "CANCEL sip:x SIP/2.0\r\n\r\n", 24,
				   dst_make("192.0.2.10", 5060), 0x00010001) != NULL);
	CHECK_UINT(sip_txn_count(&mgr), 2);

	CASE("the CANCEL response reaches the CANCEL transaction only");
	CHECK_UINT(rx(resp_text(200, "z9hG4bK-c1", "10.0.0.9:5060", 1, "CANCEL", "peer")),
		   SIP_RX_DELIVERED);
	CHECK_UINT(fk.resp_calls, 1);
	CHECK_UINT(sip_txn_count(&mgr), 2);

	CASE("a client transaction is never started for an ACK");
	CHECK(sip_txn_client_start(&mgr, SIP_METHOD_ACK, sip_str_cstr("z9hG4bK-c9"),
				   ack_wire, strlen(ack_wire), dst_make("192.0.2.10", 5060),
				   0) == NULL);
}

/* ----------------------------------------------------------------- CANCEL */

static void cancel_tests(void)
{
	struct sip_txn *ist, *cancel_txn;
	const char *invite, *cancel;

	fake_reset();
	invite = req_text("INVITE", "z9hG4bK-x1", "10.0.0.9:5060", 1, NULL, NULL);

	CASE("a CANCEL is a new server transaction of its own");
	ist = server_start(invite, "10.0.0.9", 5060);
	CHECK(ist != NULL);
	cancel = req_text("CANCEL", "z9hG4bK-x1", "10.0.0.9:5060", 1, NULL, NULL);
	CHECK_UINT(rx(cancel), SIP_RX_REQUEST_NEW);

	CASE("the CANCEL finds the INVITE server transaction by branch");
	cancel_txn = server_start(cancel, "10.0.0.9", 5060);
	CHECK(cancel_txn != NULL);
	CHECK(cancel_txn != ist);
	CHECK_UINT(sip_txn_cancel_target(&mgr, msg_take(cancel)), sip_txn_handle(ist));

	CASE("ua_cancel_handle answers 200 to the CANCEL and 487 to the INVITE");
	CHECK(ua_cancel_handle(&ua, msg_take(cancel), cancel_txn, "localtag"));
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(send_status(0), 200);
	CHECK_UINT(send_status(1), 487);
	CHECK(send_has(0, "CSeq: 1 CANCEL"));
	CHECK(send_has(1, "CSeq: 1 INVITE"));
	CHECK(send_has(1, ";tag=localtag"));

	CASE("both transactions end up in their completed state");
	CHECK_UINT(cancel_txn->state, SIP_TXN_S_COMPLETED);
	CHECK_UINT(ist->state, SIP_TXN_S_COMPLETED);

	fake_reset();

	CASE("a CANCEL with no target is answered 481");
	cancel = req_text("CANCEL", "z9hG4bK-x2", "10.0.0.9:5060", 1, NULL, NULL);
	cancel_txn = server_start(cancel, "10.0.0.9", 5060);
	CHECK(!ua_cancel_handle(&ua, msg_take(cancel), cancel_txn, NULL));
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(send_status(0), 481);

	fake_reset();

	CASE("a CANCEL after the INVITE was answered gets 200 but no 487");
	invite = req_text("INVITE", "z9hG4bK-x3", "10.0.0.9:5060", 1, NULL, NULL);
	ist = server_start(invite, "10.0.0.9", 5060);
	CHECK(sip_txn_response_send(ist, 200, "SIP/2.0 200 OK\r\n\r\n", 18));
	cancel = req_text("CANCEL", "z9hG4bK-x3", "10.0.0.9:5060", 1, NULL, NULL);
	cancel_txn = server_start(cancel, "10.0.0.9", 5060);
	CHECK(!ua_cancel_handle(&ua, msg_take(cancel), cancel_txn, NULL));
	CHECK_UINT(send_status(1), 481);

	fake_reset();

	CASE("a CANCEL for an INVITE that already got a non-2xx final gets 200 only");
	invite = req_text("INVITE", "z9hG4bK-x4", "10.0.0.9:5060", 1, NULL, NULL);
	ist = server_start(invite, "10.0.0.9", 5060);
	CHECK(sip_txn_response_send(ist, 486, "SIP/2.0 486 Busy Here\r\n\r\n", 25));
	cancel = req_text("CANCEL", "z9hG4bK-x4", "10.0.0.9:5060", 1, NULL, NULL);
	cancel_txn = server_start(cancel, "10.0.0.9", 5060);
	CHECK(!ua_cancel_handle(&ua, msg_take(cancel), cancel_txn, "localtag"));
	CHECK_UINT(send_status(1), 200);
	CHECK(send_has(1, "CSeq: 1 CANCEL"));
	CHECK_UINT(fk.sends, 2);
}

/* ---------------------------------------------------------- inbound policy */

static enum ua_policy policy_run(const char *text)
{
	struct sip_msg *m = msg_take(text);
	struct sip_txn_dst src;
	struct sip_txn *txn;

	sip_dst_set(&src, "10.0.0.9", 5060);
	txn = sip_txn_server_start(&mgr, m, &src, 0x00030001);
	if (!txn)
		return UA_POLICY_PASS;

	return ua_request_policy(&ua, m, txn);
}

static void policy_tests(void)
{
	fake_reset();

	CASE("a rogue inbound REGISTER is refused with 403");
	CHECK_UINT(policy_run(req_text("REGISTER", "z9hG4bK-p1", "10.0.0.9:5060", 1, NULL,
				       NULL)), UA_POLICY_ANSWERED);
	CHECK_UINT(send_status(0), 403);

	CASE("an unknown method gets 405 with the Allow list");
	fake_reset();
	CHECK_UINT(policy_run(req_text("FROBNICATE", "z9hG4bK-p2", "10.0.0.9:5060", 1, NULL,
				       NULL)), UA_POLICY_ANSWERED);
	CHECK_UINT(send_status(0), 405);
	CHECK(send_has(0, "Allow: INVITE, ACK, CANCEL, BYE, OPTIONS, INFO, "
			  "NOTIFY, REFER, PRACK\r\n"));

	CASE("and the 405 carries the event packages beside the method list");
	CHECK(send_has(0, "Allow-Events: message-summary, refer"));

	CASE("UPDATE is not served");
	fake_reset();
	CHECK_UINT(policy_run(req_text("UPDATE", "z9hG4bK-p3", "10.0.0.9:5060", 1, NULL,
				       NULL)), UA_POLICY_ANSWERED);
	CHECK_UINT(send_status(0), 405);
	fake_reset();

	/*
	 * REFER is served: the list carries the method with the handler that
	 * answers it, so the policy passes it to the call layer instead of
	 * swallowing it with a 405.
	 */
	CASE("REFER is passed on, because a call leg serves it");
	CHECK_UINT(policy_run(req_text("REFER", "z9hG4bK-p4", "10.0.0.9:5060", 1, NULL,
				       NULL)), UA_POLICY_PASS);
	CHECK_UINT(fk.sends, 0);

	/*
	 * PRACK is served the same way: the leg that sent the reliable
	 * provisional answers it 200 or 481 out of its early dialog.
	 */
	CASE("PRACK is passed on, because a call leg serves it in the early "
	     "dialog (RFC 3262 3)");
	fake_reset();
	CHECK_UINT(policy_run(req_text("PRACK", "z9hG4bK-p5", "10.0.0.9:5060", 1, NULL,
				       NULL)), UA_POLICY_PASS);
	CHECK_UINT(fk.sends, 0);

	CASE("SUBSCRIBE is not served either: uvoice subscribes, it does not accept");
	fake_reset();
	CHECK_UINT(policy_run(req_text("SUBSCRIBE", "z9hG4bK-p6", "10.0.0.9:5060", 1, NULL,
				       NULL)), UA_POLICY_ANSWERED);
	CHECK_UINT(send_status(0), 405);

	CASE("OPTIONS is answered 200 with Allow, Accept and Supported");
	fake_reset();
	CHECK_UINT(policy_run(req_text("OPTIONS", "z9hG4bK-p7", "10.0.0.9:5060", 1, NULL,
				       NULL)), UA_POLICY_ANSWERED);
	CHECK_UINT(send_status(0), 200);
	CHECK(send_has(0, "Allow: INVITE"));
	CHECK(send_has(0, "Accept: application/sdp"));
	CHECK(send_has(0, "Supported: timer"));

	CASE("and it names the option tags and the methods of RFC 3262");
	CHECK(send_has(0, "Supported: timer, replaces, 100rel\r\n"));
	CHECK(send_has(0, "NOTIFY, REFER, PRACK\r\n"));

	/*
	 * Allow says NOTIFY is served; Allow-Events says which package makes
	 * that useful, and the body type of that package rides in Accept
	 * (message-waiting, RFC 6665 4.4.4). A switch that reads the capability set
	 * therefore learns it may send a message summary here.
	 */
	CASE("the 200 to OPTIONS advertises the packages a NOTIFY may carry");
	CHECK(send_has(0, "Allow-Events: message-summary, refer"));
	CHECK(send_has(0, "application/simple-message-summary"));
	CHECK(send_has(0, "message/sipfrag"));

	CASE("the 200 to OPTIONS carries a To tag and Content-Length 0");
	CHECK(send_has(0, ";tag="));
	CHECK(send_has(0, "Content-Length: 0"));

	CASE("Require: 100rel passes, reliable provisionals are served "
	     "(RFC 3262 3)");
	fake_reset();
	CHECK_UINT(policy_run(req_text("INVITE", "z9hG4bK-p8", "10.0.0.9:5060", 1, NULL,
				       "Require: 100rel\r\n")), UA_POLICY_PASS);
	CHECK_UINT(fk.sends, 0);

	CASE("Require: timer passes: session timers are in scope");
	fake_reset();
	CHECK_UINT(policy_run(req_text("INVITE", "z9hG4bK-p9", "10.0.0.9:5060", 1, NULL,
				       "Require: timer\r\n")), UA_POLICY_PASS);
	CHECK_UINT(fk.sends, 0);

	CASE("a Require list reports only the tags we do not have");
	fake_reset();
	CHECK_UINT(policy_run(req_text("INVITE", "z9hG4bK-p10", "10.0.0.9:5060", 1, NULL,
				       "Require: timer, 100rel, foo\r\n")),
		   UA_POLICY_ANSWERED);
	CHECK_UINT(send_status(0), 420);
	/*
	 * Neither "timer" nor "100rel" is in this list: call.c serves session
	 * timers and the reliable provisional responses of RFC 3262 (sip/ua.h),
	 * so both tags are part of UA_SUPPORTED_DEFAULT and the 420 reports the
	 * one tag left. The pin is kept from both ends, so a list that quietly
	 * loses a tag fails here whichever direction it moves.
	 */
	CHECK(send_has(0, "Unsupported: foo\r\n"));
	CHECK(!send_has(0, "Unsupported: timer"));
	CHECK(!send_has(0, "Unsupported: 100rel"));
	CHECK(send_has(0, "Supported: timer, replaces, 100rel\r\n"));

	CASE("option tags are compared case insensitively");
	fake_reset();
	CHECK_UINT(policy_run(req_text("INVITE", "z9hG4bK-p11", "10.0.0.9:5060", 1, NULL,
				       "Require: TIMER\r\n")), UA_POLICY_PASS);

	CASE("a Require on an OPTIONS is answered before the 200");
	fake_reset();
	CHECK_UINT(policy_run(req_text("OPTIONS", "z9hG4bK-p12",
				       "10.0.0.9:5060", 1, NULL,
				       "Require: foo\r\n")),
		   UA_POLICY_ANSWERED);
	CHECK_UINT(send_status(0), 420);

	CASE("a CANCEL is never answered 420");
	fake_reset();
	CHECK_UINT(policy_run(req_text("CANCEL", "z9hG4bK-p13", "10.0.0.9:5060", 1, NULL,
				       "Require: foo\r\n")), UA_POLICY_PASS);

	CASE("INVITE, BYE, INFO and NOTIFY pass through");
	fake_reset();
	CHECK_UINT(policy_run(req_text("INVITE", "z9hG4bK-p14", "10.0.0.9:5060", 1, NULL,
				       NULL)), UA_POLICY_PASS);
	CHECK_UINT(policy_run(req_text("BYE", "z9hG4bK-p15", "10.0.0.9:5060", 2, NULL, NULL)),
		   UA_POLICY_PASS);
	CHECK_UINT(policy_run(req_text("INFO", "z9hG4bK-p16", "10.0.0.9:5060", 3, NULL,
				       NULL)), UA_POLICY_PASS);
	CHECK_UINT(policy_run(req_text("NOTIFY", "z9hG4bK-p17", "10.0.0.9:5060", 4, NULL,
				       NULL)), UA_POLICY_PASS);
	CHECK_UINT(fk.sends, 0);

	CASE("an ACK is dropped, never answered");
	fake_reset();
	{
		struct sip_txn *t;

		t = server_start(req_text("OPTIONS", "z9hG4bK-p18", "10.0.0.9:5060", 1, NULL,
					  NULL), "10.0.0.9", 5060);
		CHECK(t != NULL);
		CHECK_UINT(ua_request_policy(&ua, msg_take(req_text("ACK", "z9hG4bK-p19",
								    "10.0.0.9:5060", 1, NULL,
								    NULL)), t),
			   UA_POLICY_DROP);
		CHECK_UINT(fk.sends, 0);
	}
}

/* ------------------------------------------- 2xx retransmission (13.3.1.4) */

static bool accept_setup(uint32_t owner)
{
	const char *text = resp_text(200, "z9hG4bK-a1", "10.0.0.9:5060", 1, "INVITE",
				     "localtag");
	struct sip_msg *m = msg_take(text);
	struct sip_txn_dst dst;

	sip_dst_set(&dst, "10.0.0.9", 5060);

	return ua_accept_add(&ua, m, text, strlen(text), &dst, owner);
}

/* One accepted re-INVITE of the same dialog: CSeq n, its own branch. */
static bool accept_add_cseq(unsigned int cseq, uint32_t owner)
{
	char branch[32];
	const char *text;
	struct sip_txn_dst dst;

	snprintf(branch, sizeof(branch), "z9hG4bK-c%u", cseq);
	text = resp_text(200, branch, "10.0.0.9:5060", cseq, "INVITE", "localtag");
	sip_dst_set(&dst, "10.0.0.9", 5060);

	return ua_accept_add(&ua, msg_take(text), text, strlen(text), &dst, owner);
}

static enum ua_rx accept_ack_cseq(unsigned int cseq)
{
	char branch[32];

	snprintf(branch, sizeof(branch), "z9hG4bK-k%u", cseq);

	return ua_msg_recv(&ua, msg_take(req_text("ACK", branch, "10.0.0.9:5060", cseq,
						  "localtag", NULL)));
}

/*
 * A full pool. A peer that holds and resumes about once a second, or that
 * bridges every answered call with a re-INVITE, accepts more INVITEs inside
 * 64*T1 than the pool holds. The record whose ACK came first gives way; a
 * record still waiting for its ACK never does.
 */
static void accept_pool_tests(void)
{
	unsigned int i;

	CASE("a full pool of ACKed records takes a new 2xx in the oldest one");
	fake_reset();
	for (i = 1; i <= UA_ACCEPT_MAX; i++) {
		CHECK(accept_add_cseq(i, 0x00040001));
		CHECK_UINT(accept_ack_cseq(i), UA_RX_ACK);
	}
	CHECK(accept_add_cseq(UA_ACCEPT_MAX + 1, 0x00040001));
	CHECK_UINT(ua_accept_count(&ua), UA_ACCEPT_MAX);
	CHECK_UINT(ua.stat.denied, 0);
	CHECK_UINT(ua.stat.accept_reused, 1);
	/* The first record is gone, the second still absorbs its ACK, and the
	 * new one takes its first ACK. */
	CHECK_UINT(accept_ack_cseq(1), UA_RX_PASS);
	CHECK_UINT(accept_ack_cseq(2), UA_RX_ABSORBED);
	CHECK_UINT(accept_ack_cseq(UA_ACCEPT_MAX + 1), UA_RX_ACK);

	CASE("a hold and resume every second keeps every 2xx for a minute");
	fake_reset();
	for (i = 1; i <= 60; i++) {
		CHECK(accept_add_cseq(i, 0x00040001));
		fake_advance(10);
		CHECK_UINT(accept_ack_cseq(i), UA_RX_ACK);
		fake_advance(990);
	}
	CHECK_UINT(ua.stat.denied, 0);
	CHECK_UINT(fk.ack_missing, 0);

	CASE("a full pool waiting for its ACKs refuses a new 2xx");
	fake_reset();
	for (i = 1; i <= UA_ACCEPT_MAX; i++)
		CHECK(accept_add_cseq(i, 0x00040001));
	CHECK(!accept_add_cseq(UA_ACCEPT_MAX + 1, 0x00040001));
	CHECK_UINT(ua.stat.denied, 1);
	CHECK_UINT(ua.stat.accept_reused, 0);

	CASE("a full pool gives up only its ACKed record, the others keep "
	     "repeating their 2xx");
	fake_reset();
	for (i = 1; i <= UA_ACCEPT_MAX; i++)
		CHECK(accept_add_cseq(i, 0x00040001));
	CHECK_UINT(accept_ack_cseq(3), UA_RX_ACK);
	CHECK(accept_add_cseq(UA_ACCEPT_MAX + 1, 0x00040001));
	CHECK_UINT(ua.stat.accept_reused, 1);
	CHECK_UINT(accept_ack_cseq(3), UA_RX_PASS);
	/* Seven old records and the new one repeat at T1. */
	fake_advance(500);
	CHECK_UINT(fk.sends, UA_ACCEPT_MAX);
	CHECK(!accept_add_cseq(UA_ACCEPT_MAX + 2, 0x00040001));
	CHECK_UINT(ua.stat.denied, 1);
}

static void accept_tests(void)
{
	fake_reset();

	CASE("a 2xx is taken over for retransmission");
	CHECK(accept_setup(0x00040001));
	CHECK_UINT(ua_accept_count(&ua), 1);
	CHECK(ua_accept_active(&ua, 0x00040001));
	CHECK_UINT(fk.sends, 0);

	CASE("the 2xx cadence is T1 doubling to T2");
	fake_advance(499);
	CHECK_UINT(fk.sends, 0);
	fake_advance(1);
	CHECK_UINT(fk.sends, 1);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 2);
	fake_advance(2000);
	CHECK_UINT(fk.sends, 3);
	fake_advance(4000);
	CHECK_UINT(fk.sends, 4);
	fake_advance(4000);
	CHECK_UINT(fk.sends, 5);
	fake_advance(4000);
	CHECK_UINT(fk.sends, 6);

	CASE("the retransmissions stop at 64*T1 and the TU is told");
	fake_advance(32000 - 15500);
	CHECK_UINT(fk.sends, 10);
	CHECK_UINT(fk.ack_missing, 1);
	CHECK_UINT(fk.cb_owner, 0x00040001);
	CHECK_UINT(ua_accept_count(&ua), 0);
	CHECK_UINT(ua.stat.ack_missing, 1);

	fake_reset();

	CASE("the ACK stops the retransmissions");
	CHECK(accept_setup(0x00040001));
	fake_advance(1600);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(ua_msg_recv(&ua, msg_take(req_text("ACK", "z9hG4bK-a2", "10.0.0.9:5060",
						      1, "localtag", NULL))), UA_RX_ACK);
	fake_advance(10000);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(fk.ack_missing, 0);

	CASE("a retransmitted ACK is absorbed, never a second confirmation");
	CHECK_UINT(ua_msg_recv(&ua, msg_take(req_text("ACK", "z9hG4bK-a2", "10.0.0.9:5060",
						      1, "localtag", NULL))),
		   UA_RX_ABSORBED);
	CHECK_UINT(ua.stat.ack_absorbed, 2);

	CASE("the accepted state ends at 64*T1 without an ack_missing report");
	fake_advance(22000);
	CHECK_UINT(ua_accept_count(&ua), 0);
	CHECK_UINT(fk.ack_missing, 0);

	fake_reset();

	CASE("a retransmitted INVITE in the accepted state repeats the 2xx");
	CHECK(accept_setup(0x00040001));
	CHECK_UINT(ua_msg_recv(&ua, msg_take(req_text("INVITE", "z9hG4bK-a1",
						      "10.0.0.9:5060", 1, NULL, NULL))),
		   UA_RX_ABSORBED);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(ua.stat.invite_absorbed, 1);

	CASE("an unrelated INVITE is passed on");
	CHECK_UINT(ua_msg_recv(&ua, msg_take(req_text("INVITE", "z9hG4bK-zz",
						      "10.0.0.9:5060", 1, NULL, NULL))),
		   UA_RX_PASS);

	CASE("an ACK for another dialog is passed on");
	CHECK_UINT(ua_msg_recv(&ua, msg_take(req_text("ACK", "z9hG4bK-a2", "10.0.0.9:5060",
						      1, "othertag", NULL))), UA_RX_PASS);

	CASE("a response is never taken by the UA receive path");
	CHECK_UINT(ua_msg_recv(&ua, msg_take(resp_text(200, "z9hG4bK-a1", "10.0.0.9:5060",
						       1, "INVITE", "localtag"))),
		   UA_RX_PASS);

	CASE("a non-2xx is refused by the accepted state");
	fake_reset();
	{
		const char *text = resp_text(486, "z9hG4bK-a3", "10.0.0.9:5060", 1, "INVITE",
					     "localtag");
		struct sip_txn_dst dst;

		sip_dst_set(&dst, "10.0.0.9", 5060);
		CHECK(!ua_accept_add(&ua, msg_take(text), text, strlen(text), &dst, 1));
	}

	CASE("the same 2xx is taken over once");
	fake_reset();
	CHECK(accept_setup(0x00040001));
	CHECK(!accept_setup(0x00040001));
	CHECK_UINT(ua_accept_count(&ua), 1);
}

/* -------------------------------------------------------- 491 glare, 503 */

static void glare_tests(void)
{
	unsigned int i, low = 0, high = 0;
	uint32_t ms, min = 0xffffffffu, max = 0;

	fake_reset();

	CASE("the UAC range is 2.1 to 4.0 s in units of 10 ms");
	for (i = 0; i < 2000; i++) {
		ms = ua_glare_delay_ms(&ua, true);
		if (ms < 2100 || ms > 4000 || ms % 10)
			low++;
		if (ms < min)
			min = ms;
		if (ms > max)
			max = ms;
	}
	CHECK_UINT(low, 0);
	CHECK(min < 2200);
	CHECK(max > 3900);

	CASE("the UAS range is 0 to 2.0 s in units of 10 ms");
	min = 0xffffffffu;
	max = 0;
	for (i = 0; i < 2000; i++) {
		ms = ua_glare_delay_ms(&ua, false);
		if (ms > 2000 || ms % 10)
			high++;
		if (ms < min)
			min = ms;
		if (ms > max)
			max = ms;
	}
	CHECK_UINT(high, 0);
	CHECK(min < 100);
	CHECK(max > 1900);

	CASE("an armed glare timer calls back when it expires");
	CHECK(ua_glare_arm(&ua, 0x00050001, true, &ms));
	CHECK(ms >= 2100 && ms <= 4000);
	fake_advance(ms - 1);
	CHECK_UINT(fk.glare_retry, 0);
	fake_advance(1);
	CHECK_UINT(fk.glare_retry, 1);
	CHECK_UINT(fk.cb_owner, 0x00050001);

	CASE("a cancelled glare timer never calls back");
	CHECK(ua_glare_arm(&ua, 0x00050002, false, &ms));
	ua_glare_cancel(&ua, 0x00050002);
	fake_advance(5000);
	CHECK_UINT(fk.glare_retry, 1);

	CASE("arming twice for one owner leaves one timer");
	CHECK(ua_glare_arm(&ua, 0x00050003, true, &ms));
	CHECK(ua_glare_arm(&ua, 0x00050003, true, &ms));
	fake_advance(5000);
	CHECK_UINT(fk.glare_retry, 2);
}

static void retry_after_tests(void)
{
	struct sip_msg *m;
	uint32_t secs = 0;
	char text[512];

	fake_reset();

	CASE("Retry-After is extracted from a 503");
	snprintf(text, sizeof(text),
		 "SIP/2.0 503 Service Unavailable\r\nVia: SIP/2.0/UDP 10.0.0.1;branch=z9hG4bK-q\r\n"
		 "From: <sip:a@b>;tag=x\r\nTo: <sip:c@d>;tag=y\r\nCall-ID: q\r\n"
		 "CSeq: 1 REGISTER\r\nRetry-After: 120\r\nContent-Length: 0\r\n\r\n");
	m = msg_take(text);
	CHECK(m != NULL);
	CHECK(ua_503_handle(&ua, 0x00060001, m));
	CHECK_UINT(fk.retry_after, 1);
	CHECK_UINT(fk.cb_secs, 120);
	CHECK(fk.cb_present);
	CHECK_UINT(fk.cb_owner, 0x00060001);

	CASE("a 503 without Retry-After reports the absence");
	snprintf(text, sizeof(text),
		 "SIP/2.0 503 Service Unavailable\r\nVia: SIP/2.0/UDP 10.0.0.1;branch=z9hG4bK-q\r\n"
		 "From: <sip:a@b>;tag=x\r\nTo: <sip:c@d>;tag=y\r\nCall-ID: q\r\n"
		 "CSeq: 1 REGISTER\r\nContent-Length: 0\r\n\r\n");
	CHECK(!ua_503_handle(&ua, 0x00060001, msg_take(text)));
	CHECK_UINT(fk.retry_after, 2);
	CHECK(!fk.cb_present);
	CHECK_UINT(fk.cb_secs, 0);

	CASE("Retry-After with a comment and a duration still parses");
	snprintf(text, sizeof(text),
		 "SIP/2.0 503 Service Unavailable\r\nVia: SIP/2.0/UDP 10.0.0.1;branch=z9hG4bK-q\r\n"
		 "From: <sip:a@b>;tag=x\r\nTo: <sip:c@d>;tag=y\r\nCall-ID: q\r\n"
		 "CSeq: 1 REGISTER\r\nRetry-After: 30 (out of service);duration=600\r\n"
		 "Content-Length: 0\r\n\r\n");
	m = msg_take(text);
	CHECK(ua_retry_after_get(m, &secs));
	CHECK_UINT(secs, 30);

	CASE("a response that is not a 503 is not handled here");
	CHECK(!ua_503_handle(&ua, 0x00060001,
			     msg_take(resp_text(500, "z9hG4bK-c1", "10.0.0.9:5060", 1,
						"INVITE", "peer"))));
	CHECK_UINT(fk.retry_after, 2);
}

/* ------------------------------------------------------------------ owner */

static void owner_tests(void)
{
	struct sip_txn *txn;

	fake_reset();

	CASE("releasing an owner detaches its transactions");
	txn = ict_start(0x00070001);
	CHECK(txn != NULL);
	CHECK_UINT(sip_txn_owner_release(&mgr, 0x00070001), 1);
	CHECK(txn->detached);
	CHECK_UINT(txn->owner, SIP_HANDLE_NONE);

	CASE("a detached transaction delivers no response upcall");
	CHECK_UINT(rx(resp_text(180, "z9hG4bK-c1", "10.0.0.9:5060", 1, "INVITE", "peer")),
		   SIP_RX_DELIVERED);
	CHECK_UINT(fk.resp_calls, 0);

	CASE("a detached transaction still runs to its protocol end, silently");
	fake_advance(60000);
	CHECK_UINT(fk.done_calls, 0);
	CHECK_UINT(sip_txn_count(&mgr), 1);
	CHECK_UINT(txn->state, SIP_TXN_S_PROCEEDING);

	CASE("releasing an owner with no transactions is a no-op");
	CHECK_UINT(sip_txn_owner_release(&mgr, 0x00070002), 0);
	CHECK_UINT(sip_txn_owner_release(&mgr, SIP_HANDLE_NONE), 0);

	CASE("a detached non-INVITE transaction reports no timeout either");
	fake_reset();
	CHECK(nict_start(0x00070003) != NULL);
	CHECK_UINT(sip_txn_owner_release(&mgr, 0x00070003), 1);
	fake_advance(40000);
	CHECK_UINT(fk.done_calls, 0);
	CHECK_UINT(sip_txn_count(&mgr), 0);

	CASE("releasing an owner stops the UA callbacks as well");
	fake_reset();
	CHECK(accept_setup(0x00070004));
	CHECK_UINT(ua_owner_release(&ua, 0x00070004), 1);
	fake_advance(40000);
	CHECK_UINT(fk.ack_missing, 0);
	CHECK_UINT(ua_accept_count(&ua), 0);

	CASE("the 2xx still went out while detached: the peer sees a full answer");
	CHECK_UINT(fk.sends, 10);

	CASE("a released glare timer never calls back");
	fake_reset();
	{
		uint32_t ms;

		CHECK(ua_glare_arm(&ua, 0x00070005, true, &ms));
		CHECK_UINT(ua_owner_release(&ua, 0x00070005), 1);
		fake_advance(5000);
		CHECK_UINT(fk.glare_retry, 0);
	}
}

/* ----------------------------------------------------------------- limits */

static void limit_tests(void)
{
	char branch[SIP_BRANCH_MAX];
	unsigned int i;

	fake_reset();

	CASE("the pool is bounded and refuses the transaction that does not fit");
	for (i = 0; i < SIP_TXN_MAX; i++) {
		snprintf(branch, sizeof(branch), "z9hG4bK-pool-%u", i);
		CHECK(sip_txn_client_start(&mgr, SIP_METHOD_OPTIONS, sip_str_cstr(branch),
					   "OPTIONS sip:a SIP/2.0\r\n\r\n", 25,
					   dst_make("192.0.2.10", 5060), 1) != NULL);
	}
	CHECK_UINT(sip_txn_count(&mgr), SIP_TXN_MAX);
	snprintf(branch, sizeof(branch), "z9hG4bK-pool-over");
	CHECK(sip_txn_client_start(&mgr, SIP_METHOD_OPTIONS, sip_str_cstr(branch),
				   "OPTIONS sip:a SIP/2.0\r\n\r\n", 25,
				   dst_make("192.0.2.10", 5060), 1) == NULL);
	CHECK_UINT(mgr.stat.denied, 1);

	CASE("the pool drains when the transactions time out");
	fake_advance(40000);
	CHECK_UINT(sip_txn_count(&mgr), 0);

	CASE("the accepted record pool is bounded too");
	fake_reset();
	for (i = 0; i < UA_ACCEPT_MAX + 2; i++) {
		const char *text;
		struct sip_txn_dst dst;
		char tag[32];

		snprintf(branch, sizeof(branch), "z9hG4bK-acc-%u", i);
		snprintf(tag, sizeof(tag), "tag-%u", i);
		text = resp_text(200, branch, "10.0.0.9:5060", i + 1, "INVITE", tag);
		sip_dst_set(&dst, "10.0.0.9", 5060);
		ua_accept_add(&ua, msg_take(text), text, strlen(text), &dst, i + 1);
	}
	CHECK_UINT(ua_accept_count(&ua), UA_ACCEPT_MAX);
	CHECK_UINT(ua.stat.denied, 2);

	CASE("the trace hook was exercised throughout");
	CHECK(trace_total > 100);
	CHECK_UINT(fake_timer_dropped, 0);
}

/* ------------ limits, embedded NULs, send failures, CSeq and Allow edges */

/*
 * A request whose Via chain alone is larger than the response buffer. The
 * header count stays inside SIP_HDR_COUNT_MAX, so the message parses and only
 * the response fails to build.
 */
static const char *via_flood_text(unsigned int id, unsigned int vias)
{
	static const char pad[] = "padpadpadpadpadpadpadpadpadpadpadpadpadpadpadpad"
				  "padpadpadpadpadpadpadpadpadpadpadpadpadpadpadpad"
				  "padpadpadpadpadpadpadpadpadpadpadpadpadpadpadpad"
				  "padpadpadpadpadpadpadpadpadpadpadpadpadpadpadpad";
	char *buf = text_slot();
	size_t used;
	unsigned int i;

	used = (size_t)snprintf(buf, MSG_TEXT_MAX, "OPTIONS sip:5552000@example.net SIP/2.0\r\n");
	for (i = 0; i < vias; i++) {
		used += (size_t)snprintf(buf + used, MSG_TEXT_MAX - used,
					 "Via: SIP/2.0/UDP 10.0.0.9:5060;"
					 "branch=z9hG4bK-flood-%u-%u-%s\r\n", id, i, pad);
	}

	snprintf(buf + used, MSG_TEXT_MAX - used,
		 "From: <sip:5551000@carrier.net>;tag=remotetag\r\n"
		 "To: <sip:5552000@example.net>\r\n"
		 "Call-ID: flood-%u@10.0.0.9\r\n"
		 "CSeq: 1 OPTIONS\r\n"
		 "Content-Length: 0\r\n\r\n", id);

	return buf;
}

static const char *ack_cseq_text(const char *cseq_method)
{
	char *buf = text_slot();

	snprintf(buf, MSG_TEXT_MAX,
		 "ACK sip:5552000@example.net SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP 10.0.0.9:5060;branch=z9hG4bK-a2\r\n"
		 "From: \"Caller\" <sip:5551000@carrier.net>;tag=remotetag\r\n"
		 "To: <sip:5552000@example.net>;tag=localtag\r\n"
		 "Call-ID: call-abc@10.0.0.9\r\n"
		 "CSeq: 1 %s\r\n"
		 "Content-Length: 0\r\n\r\n", cseq_method);

	return buf;
}

static const char *big_ident_text(bool response, const char *call_id, const char *ftag,
				  const char *ttag)
{
	char *buf = text_slot();

	snprintf(buf, MSG_TEXT_MAX,
		 "%s"
		 "Via: SIP/2.0/UDP 10.0.0.9:5060;branch=z9hG4bK-big\r\n"
		 "From: <sip:5551000@carrier.net>;tag=%s\r\n"
		 "To: <sip:5552000@example.net>;tag=%s\r\n"
		 "Call-ID: %s\r\n"
		 "CSeq: 9 %s\r\n"
		 "Content-Length: 0\r\n\r\n",
		 response ? "SIP/2.0 200 OK\r\n" : "ACK sip:5552000@example.net SIP/2.0\r\n",
		 ftag, ttag, call_id, response ? "INVITE" : "ACK");

	return buf;
}

static bool accept_add_text(const char *text, uint32_t owner)
{
	struct sip_msg *m = msg_take(text);
	struct sip_txn_dst dst;

	sip_dst_set(&dst, "10.0.0.9", 5060);

	return ua_accept_add(&ua, m, text, strlen(text), &dst, owner);
}

static void h1_tests(void)
{
	unsigned int i;

	fake_reset();

	CASE("a response that does not fit discards the server transaction");
	CHECK_UINT(policy_run(via_flood_text(1, 50)), UA_POLICY_FAILED);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(sip_txn_count(&mgr), 0);

	CASE("a flood of unanswerable requests cannot fill the pool");
	for (i = 0; i < SIP_TXN_MAX + 8; i++)
		CHECK_UINT(policy_run(via_flood_text(i + 2, 50)), UA_POLICY_FAILED);
	CHECK_UINT(sip_txn_count(&mgr), 0);
	CHECK_UINT(mgr.stat.denied, 0);

	CASE("a transport failure on an automatic response discards it too");
	fake_reset();
	fk.send_fail = true;
	CHECK_UINT(policy_run(req_text("OPTIONS", "z9hG4bK-h1", "10.0.0.9:5060", 1, NULL,
				       NULL)), UA_POLICY_FAILED);
	CHECK_UINT(sip_txn_count(&mgr), 0);
	CHECK_UINT(fk.done_calls, 0);

	CASE("a CANCEL whose 200 cannot be sent leaves no transaction behind");
	fake_reset();
	{
		const char *invite = req_text("INVITE", "z9hG4bK-h2", "10.0.0.9:5060", 1,
					      NULL, NULL);
		const char *cancel;
		struct sip_txn *ctxn;

		CHECK(server_start(invite, "10.0.0.9", 5060) != NULL);
		cancel = req_text("CANCEL", "z9hG4bK-h2", "10.0.0.9:5060", 1, NULL, NULL);
		ctxn = server_start(cancel, "10.0.0.9", 5060);
		CHECK(ctxn != NULL);
		fk.send_fail = true;
		CHECK(!ua_cancel_handle(&ua, msg_take(cancel), ctxn, "localtag"));
		/* the INVITE transaction belongs to the call layer and stays */
		CHECK_UINT(sip_txn_count(&mgr), 1);
	}
}

static void h2_tests(void)
{
	fake_reset();

	CASE("a branch with an embedded NUL matches no client transaction");
	CHECK(ict_start(0x000a0001) != NULL);
	CHECK_UINT(rx_mark(resp_text(200, "z9hG4bK-c1|INVITE#zzz", "10.0.0.9:5060", 1, "BYE",
				     "peer"), '#'), SIP_RX_IGNORED);
	CHECK_UINT(fk.resp_calls, 0);
	CHECK_UINT(fk.done_calls, 0);
	CHECK_UINT(sip_txn_count(&mgr), 1);

	CASE("the same response without the NUL is stray, not a match either");
	CHECK_UINT(rx(resp_text(200, "z9hG4bK-c1|INVITE", "10.0.0.9:5060", 1, "BYE", "peer")),
		   SIP_RX_RESPONSE_STRAY);
	CHECK_UINT(sip_txn_count(&mgr), 1);

	fake_reset();

	CASE("a branch with an embedded NUL matches no server transaction");
	CHECK(server_start(req_text("INVITE", "z9hG4bK-s1", "10.0.0.9:5060", 1, NULL, NULL),
			   "10.0.0.9", 5060) != NULL);
	CHECK_UINT(rx_mark(req_text("OPTIONS", "z9hG4bK-s1|10.0.0.9:5060|INVITE#z",
				    "10.0.0.9:5060", 1, NULL, NULL), '#'), SIP_RX_IGNORED);
	CHECK_UINT(sip_txn_count(&mgr), 1);

	CASE("a NUL bearing request gets no server transaction of its own");
	CHECK(server_start_mark(req_text("OPTIONS", "z9hG4bK-n1#AAA", "10.0.0.9:5060", 1,
					 NULL, NULL), '#', "10.0.0.9", 5060) == NULL);
	CHECK(server_start_mark(req_text("OPTIONS", "z9hG4bK-n1#BBB", "10.0.0.9:5060", 1,
					 NULL, NULL), '#', "10.0.0.9", 5060) == NULL);
	CHECK_UINT(sip_txn_count(&mgr), 1);

	CASE("a NUL in the sent-by is refused as well");
	CHECK(server_start_mark(req_text("OPTIONS", "z9hG4bK-n2", "10.0.0.9#x:5060", 1, NULL,
					 NULL), '#', "10.0.0.9", 5060) == NULL);
	CHECK_UINT(sip_txn_count(&mgr), 1);

	CASE("a NUL in the Call-ID of a pre-RFC 3261 request is refused");
	CHECK_UINT(rx_mark("OPTIONS sip:a@b SIP/2.0\r\n"
			   "Via: SIP/2.0/UDP 10.0.0.9:5060;branch=legacy-2\r\n"
			   "From: <sip:a@b>;tag=x\r\nTo: <sip:c@d>\r\n"
			   "Call-ID: aaa#bbb\r\nCSeq: 1 OPTIONS\r\n\r\n", '#'),
		   SIP_RX_IGNORED);
}

static void h3_tests(void)
{
	fake_reset();

	CASE("a refused 2xx retransmission keeps the RFC schedule");
	CHECK(accept_setup(0x00080001));
	fk.send_fail = true;
	fake_advance(500);
	CHECK_UINT(fk.sends, 0);
	fk.send_fail = false;
	fake_advance(1000);
	CHECK_UINT(fk.sends, 1);
	fake_advance(2000);
	CHECK_UINT(fk.sends, 2);

	CASE("the give-up still lands at 64*T1");
	CHECK_UINT(fk.ack_missing, 0);
	fake_advance(32000 - 3500);
	CHECK_UINT(fk.ack_missing, 1);
	CHECK_UINT(ua_accept_count(&ua), 0);
}

static void h4_tests(void)
{
	fake_reset();
	fk.auto_ack = true;
	fk.ack_wire = ack_wire;

	CASE("a first ACK that cannot be sent drops the client transaction");
	CHECK(ict_start(0x00090001) != NULL);
	fk.send_fail = true;
	CHECK_UINT(rx(resp_text(486, "z9hG4bK-c1", "10.0.0.9:5060", 1, "INVITE", "peer")),
		   SIP_RX_DELIVERED);
	CHECK_UINT(sip_txn_count(&mgr), 0);
	CHECK_UINT(fk.done_calls, 0);

	CASE("timer D never reports a completion for it afterwards");
	fk.send_fail = false;
	fake_advance(40000);
	CHECK_UINT(fk.done_calls, 0);
	CHECK_UINT(fk.sends, 1);
}

static void h5_tests(void)
{
	char host[300], branch[800];

	fake_reset();

	CASE("a long but valid cookie key still keys a transaction");
	fill_text(host, sizeof(host), 'h', 253);
	snprintf(branch, sizeof(branch), "z9hG4bK-%.*s", 52, "aaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		 "aaaaaaaaaaaaaaaaaaaaaaaa");
	CHECK(server_start(req_text("INVITE", branch, host, 1, NULL, NULL), "10.0.0.9",
			   5060) != NULL);
	CHECK_UINT(sip_txn_count(&mgr), 1);

	CASE("its retransmission matches the same transaction");
	CHECK_UINT(rx(req_text("INVITE", branch, host, 1, NULL, NULL)), SIP_RX_ABSORBED);
	CHECK_UINT(sip_txn_count(&mgr), 1);

	fake_reset();

	CASE("an overlong cookie key is refused, never demoted to pre-RFC 3261 "
	     "matching");
	fill_text(branch, sizeof(branch), 'b', 700);
	memcpy(branch, SIP_BRANCH_MAGIC, strlen(SIP_BRANCH_MAGIC));
	CHECK(server_start(req_text("INVITE", branch, "10.0.0.9:5060", 1, NULL, NULL),
			   "10.0.0.9", 5060) == NULL);
	CHECK_UINT(sip_txn_count(&mgr), 0);

	CASE("a second overlong branch of the same dialog does not alias the first");
	branch[100] = 'c';
	CHECK_UINT(rx(req_text("INVITE", branch, "10.0.0.9:5060", 1, NULL, NULL)),
		   SIP_RX_IGNORED);
	CHECK_UINT(sip_txn_count(&mgr), 0);
}

static void h6_tests(void)
{
	fake_reset();

	CASE("a 2xx whose CSeq is not INVITE is not an accepted INVITE");
	CHECK(!accept_add_text(resp_text(200, "z9hG4bK-a1", "10.0.0.9:5060", 1, "BYE",
					 "localtag"), 1));
	CHECK_UINT(ua_accept_count(&ua), 0);

	CASE("an ACK whose CSeq says BYE does not stop the retransmission");
	CHECK(accept_setup(0x000b0001));
	CHECK_UINT(ua_msg_recv(&ua, msg_take(ack_cseq_text("BYE"))), UA_RX_PASS);
	fake_advance(600);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(ua.stat.ack_absorbed, 0);

	CASE("the ACK with the right CSeq does stop it");
	CHECK_UINT(ua_msg_recv(&ua, msg_take(ack_cseq_text("ACK"))), UA_RX_ACK);
	fake_advance(10000);
	CHECK_UINT(fk.sends, 1);

	CASE("an INVITE whose CSeq says BYE is not a retransmission");
	fake_reset();
	CHECK(accept_setup(0x000b0002));
	{
		char *buf = text_slot();

		snprintf(buf, MSG_TEXT_MAX,
			 "INVITE sip:5552000@example.net SIP/2.0\r\n"
			 "Via: SIP/2.0/UDP 10.0.0.9:5060;branch=z9hG4bK-a1\r\n"
			 "From: <sip:5551000@carrier.net>;tag=remotetag\r\n"
			 "To: <sip:5552000@example.net>\r\n"
			 "Call-ID: call-abc@10.0.0.9\r\n"
			 "CSeq: 1 BYE\r\nContent-Length: 0\r\n\r\n");
		CHECK_UINT(ua_msg_recv(&ua, msg_take(buf)), UA_RX_PASS);
	}
	CHECK_UINT(fk.sends, 0);
}

static void h7_tests(void)
{
	char tag[6000], require[6100], line[6100];

	fake_reset();

	CASE("a single oversized option tag is preserved in Unsupported");
	fill_text(tag, sizeof(tag), 'q', 300);
	snprintf(require, sizeof(require), "Require: %s\r\n", tag);
	CHECK_UINT(policy_run(req_text("INVITE", "z9hG4bK-h7a", "10.0.0.9:5060", 1, NULL,
				       require)), UA_POLICY_ANSWERED);
	CHECK_UINT(send_status(0), 420);
	CHECK(send_has(0, tag));

	CASE("every tag of a long list survives");
	fake_reset();
	fill_text(tag, sizeof(tag), 'w', 900);
	snprintf(require, sizeof(require), "Require: timer, %s, 100rel\r\n", tag);
	CHECK_UINT(policy_run(req_text("INVITE", "z9hG4bK-h7b", "10.0.0.9:5060", 1, NULL,
				       require)), UA_POLICY_ANSWERED);
	CHECK_UINT(send_status(0), 420);
	/* the Unsupported line is pinned whole: "100rel" alone would pass on
	 * the Supported line of the same response */
	snprintf(line, sizeof(line), "Unsupported: %s\r\n", tag);
	CHECK(send_has(0, line));
	CHECK(!send_has(0, "Unsupported: timer"));
	CHECK(!send_has(0, "Unsupported: 100rel"));

	CASE("a list too long to enumerate is answered 400, never an empty 420");
	fake_reset();
	fill_text(tag, sizeof(tag), 'z', 5000);
	snprintf(require, sizeof(require), "Require: %s\r\n", tag);
	CHECK_UINT(policy_run(req_text("INVITE", "z9hG4bK-h7c", "10.0.0.9:5060", 1, NULL,
				       require)), UA_POLICY_ANSWERED);
	CHECK_UINT(send_status(0), 400);
	CHECK(!send_has(0, "Unsupported"));
}

static void h8_tests(void)
{
	char call_id[400], ftag[300], ttag[300];

	fake_reset();

	CASE("long identifiers do not stop a 2xx from being retransmitted");
	fill_text(call_id, sizeof(call_id), 'i', 300);
	fill_text(ftag, sizeof(ftag), 'f', 200);
	fill_text(ttag, sizeof(ttag), 't', 200);
	CHECK(accept_add_text(big_ident_text(true, call_id, ftag, ttag), 0x000c0001));
	CHECK_UINT(ua_accept_count(&ua), 1);
	fake_advance(600);
	CHECK_UINT(fk.sends, 1);

	CASE("the ACK with those identifiers still matches");
	CHECK_UINT(ua_msg_recv(&ua, msg_take(big_ident_text(false, call_id, ftag, ttag))),
		   UA_RX_ACK);
	fake_advance(10000);
	CHECK_UINT(fk.sends, 1);

	CASE("a different Call-ID of the same length does not match");
	fake_reset();
	CHECK(accept_add_text(big_ident_text(true, call_id, ftag, ttag), 0x000c0001));
	call_id[10] = 'X';
	CHECK_UINT(ua_msg_recv(&ua, msg_take(big_ident_text(false, call_id, ftag, ttag))),
		   UA_RX_PASS);
}

static void h9_tests(void)
{
	struct ua_cfg cfg;

	fake_reset();

	CASE("an Allow list that names REGISTER is refused at init");
	memset(&cfg, 0, sizeof(cfg));
	cfg.allow = "INVITE, ACK, REGISTER, BYE";
	ua_init(&ua, &mgr, &fk.env, &cfg, &ua_cb, NULL);
	CHECK(ua.allow_conflict);
	CHECK(!strcmp(ua.cfg.allow, UA_ALLOW_DEFAULT));

	CASE("REGISTER is still answered 403 after the conflict");
	CHECK_UINT(policy_run(req_text("REGISTER", "z9hG4bK-h9a", "10.0.0.9:5060", 1, NULL,
				       NULL)), UA_POLICY_ANSWERED);
	CHECK_UINT(send_status(0), 403);

	CASE("a custom list without REGISTER is kept and drives both branches");
	fake_reset();
	memset(&cfg, 0, sizeof(cfg));
	cfg.allow = "INVITE, ACK, BYE";
	ua_init(&ua, &mgr, &fk.env, &cfg, &ua_cb, NULL);
	CHECK(!ua.allow_conflict);
	CHECK_UINT(policy_run(req_text("OPTIONS", "z9hG4bK-h9b", "10.0.0.9:5060", 1, NULL,
				       NULL)), UA_POLICY_ANSWERED);
	CHECK_UINT(send_status(0), 405);
	CHECK(send_has(0, "Allow: INVITE, ACK, BYE"));

	CASE("the same list still answers a REGISTER with 403, not 405");
	CHECK_UINT(policy_run(req_text("REGISTER", "z9hG4bK-h9c", "10.0.0.9:5060", 1, NULL,
				       NULL)), UA_POLICY_ANSWERED);
	CHECK_UINT(send_status(1), 403);
	CHECK(!send_has(1, "Allow:"));

	fake_reset();
}

/* --------------------------------------------------------- automatic 100 */

static struct sip_txn *ist_for_trying(const char *branch)
{
	return server_start(req_text("INVITE", branch, "10.0.0.9:5060", 1, NULL, NULL),
			    "10.0.0.9", 5060);
}

static void trying_tests(void)
{
	struct sip_txn *txn;
	unsigned int i;
	uint32_t handle;

	CASE("the 100 goes out 200 ms after the INVITE server transaction starts");
	fake_reset();
	txn = ist_for_trying("z9hG4bK-t1");
	CHECK(txn != NULL);
	CHECK(ua_trying_arm(&ua, txn));
	fake_advance(199);
	CHECK_UINT(fk.sends, 0);
	fake_advance(1);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(send_status(0), 100);
	CHECK_UINT(ua.stat.trying_sent, 1);

	CASE("the automatic 100 carries no To tag (RFC 3261 8.2.6.1)");
	CHECK(send_has(0, "To: <sip:5552000@example.net>\r\n"));
	CHECK(!send_has(0, "To: <sip:5552000@example.net>;tag="));
	CHECK(send_has(0, "Via: SIP/2.0/UDP 10.0.0.9:5060;branch=z9hG4bK-t1"));

	CASE("the transaction is still proceeding and takes a 180 afterwards");
	CHECK_UINT(txn->state, SIP_TXN_S_PROCEEDING);
	CHECK(ua_response_send(&ua, txn, msg_take(req_text("INVITE", "z9hG4bK-t1",
							   "10.0.0.9:5060", 1, NULL, NULL)),
			       &(struct ua_resp){ .code = 180 }));
	CHECK_UINT(send_status(1), 180);

	CASE("a response inside the 200 ms keeps the 100 off the wire");
	fake_reset();
	txn = ist_for_trying("z9hG4bK-t2");
	CHECK(txn != NULL);
	CHECK(ua_trying_arm(&ua, txn));
	CHECK(ua_response_send(&ua, txn, msg_take(req_text("INVITE", "z9hG4bK-t2",
							   "10.0.0.9:5060", 1, NULL, NULL)),
			       &(struct ua_resp){ .code = 180 }));
	CHECK_UINT(fk.sends, 1);
	fake_advance(500);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(ua.stat.trying_sent, 0);
	CHECK_UINT(ua.stat.trying_skipped, 1);

	CASE("a transaction that ended before the timer produces nothing");
	fake_reset();
	txn = ist_for_trying("z9hG4bK-t3");
	CHECK(txn != NULL);
	CHECK(ua_trying_arm(&ua, txn));
	sip_txn_abort(txn);
	fake_advance(500);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ua.stat.trying_sent, 0);
	CHECK_UINT(ua.stat.trying_skipped, 0);

	CASE("a non INVITE server transaction is never armed");
	fake_reset();
	txn = server_start(req_text("OPTIONS", "z9hG4bK-t4", "10.0.0.9:5060", 1, NULL, NULL),
			   "10.0.0.9", 5060);
	CHECK(txn != NULL);
	CHECK(!ua_trying_arm(&ua, txn));
	CHECK(!ua_trying_arm(&ua, NULL));
	fake_advance(500);
	CHECK_UINT(fk.sends, 0);

	CASE("an explicit cancel drops the pending 100");
	fake_reset();
	txn = ist_for_trying("z9hG4bK-t5");
	CHECK(txn != NULL);
	handle = sip_txn_handle(txn);
	CHECK(ua_trying_arm(&ua, txn));
	ua_trying_cancel(&ua, handle);
	fake_advance(500);
	CHECK_UINT(fk.sends, 0);

	CASE("an owner that went away takes its pending 100 with it");
	fake_reset();
	txn = ist_for_trying("z9hG4bK-t6");
	CHECK(txn != NULL);
	CHECK(ua_trying_arm(&ua, txn));
	CHECK(ua_owner_release(&ua, 0x00010001) >= 1);
	fake_advance(500);
	CHECK_UINT(fk.sends, 0);

	CASE("the pool refuses more than UA_TRYING_MAX pending answers");
	fake_reset();
	for (i = 0; i < UA_TRYING_MAX; i++) {
		char branch[32];

		snprintf(branch, sizeof(branch), "z9hG4bK-tp%u", i);
		txn = ist_for_trying(branch);
		CHECK(txn != NULL);
		CHECK(ua_trying_arm(&ua, txn));
	}
	txn = ist_for_trying("z9hG4bK-tpx");
	CHECK(txn != NULL);
	CHECK(!ua_trying_arm(&ua, txn));
	CHECK_UINT(ua.stat.denied, 1);
	fake_advance(500);
	CHECK_UINT(fk.sends, UA_TRYING_MAX);

	CASE("the pending 100 carries its own timer id");
	fake_reset();
	txn = ist_for_trying("z9hG4bK-t0");
	CHECK(txn != NULL);
	CHECK(ua_trying_arm(&ua, txn));
	CHECK_UINT(ua.trying[0].timer.id, SIP_TIMER_TRYING);
	CHECK(ua.trying[0].timer.armed);
	CHECK(!strcmp(sip_timer_name(SIP_TIMER_TRYING), "trying"));
	fake_advance(200);
	CHECK_UINT(send_status(0), 100);

	CASE("ua_done cancels every pending 100");
	fake_reset();
	txn = ist_for_trying("z9hG4bK-t7");
	CHECK(txn != NULL);
	CHECK(ua_trying_arm(&ua, txn));
	ua_done(&ua);
	fake_advance(500);
	CHECK_UINT(fk.sends, 0);

	fake_reset();
}

/* ------------------------------------------------- reliable transport gate */

/*
 * The purpose of the gate. With SIP_TXN_ENV_RELIABLE set, the retransmit
 * timers A, E and G never arm, and the wait timers D, K, I and J collapse to
 * 0 ms. The transaction timeouts B, F and H are unchanged, and the server
 * answers the arriving source verbatim (RFC 3261 section 17, 18.2.2). Every
 * reliable case sets fk.env.flags after fake_reset. Reason: fake_reset zeroes
 * the flags back to the UDP default that the rest of the suite relies on.
 */
static void reliable_tests(void)
{
	struct sip_txn *txn;
	uint32_t handle;
	const char *invite;
	const char *bye;

	CASE("ICT reliable: the request goes out once and no timer A retransmits it");
	fake_reset();
	fk.env.flags = SIP_TXN_ENV_RELIABLE;
	txn = ict_start(0x00010001);
	CHECK(txn != NULL);
	CHECK_UINT(fk.sends, 1);
	fake_advance(500);
	CHECK_UINT(fk.sends, 1);
	fake_advance(1500);
	CHECK_UINT(fk.sends, 1);
	fake_advance(6000);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(fk.done_calls, 0);

	CASE("ICT reliable: timer B still times out at 64*T1");
	fake_advance(32000 - 8000 - 1);
	CHECK_UINT(fk.done_calls, 0);
	fake_advance(1);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_TIMEOUT);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(sip_txn_count(&mgr), 0);

	CASE("ICT reliable: a 2xx still terminates the transaction at once");
	fake_reset();
	fk.env.flags = SIP_TXN_ENV_RELIABLE;
	txn = ict_start(0x00010001);
	handle = sip_txn_handle(txn);
	CHECK_UINT(rx(resp_text(200, "z9hG4bK-c1", "10.0.0.9:5060", 1, "INVITE", "peer")),
		   SIP_RX_DELIVERED);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_OK);
	CHECK(sip_txn_get(&mgr, handle) == NULL);

	CASE("ICT reliable: the ACK for a non-2xx final is sent once, timer D waits 0 ms");
	fake_reset();
	fk.env.flags = SIP_TXN_ENV_RELIABLE;
	fk.auto_ack = true;
	fk.ack_wire = ack_wire;
	txn = ict_start(0x00010001);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(rx(resp_text(486, "z9hG4bK-c1", "10.0.0.9:5060", 1, "INVITE", "peer")),
		   SIP_RX_DELIVERED);
	CHECK_UINT(txn->state, SIP_TXN_S_COMPLETED);
	CHECK_UINT(fk.sends, 2);
	CHECK(!strncmp(last_text(), "ACK ", 4));
	CHECK_UINT(fk.done_calls, 0);
	fake_advance(0);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_OK);
	CHECK_UINT(sip_txn_count(&mgr), 0);

	CASE("NICT reliable: no timer E retransmits and timer F still times out");
	fake_reset();
	fk.env.flags = SIP_TXN_ENV_RELIABLE;
	txn = nict_start(0x00020001);
	CHECK(txn != NULL);
	CHECK_UINT(fk.sends, 1);
	fake_advance(500);
	CHECK_UINT(fk.sends, 1);
	fake_advance(3500);
	CHECK_UINT(fk.sends, 1);
	fake_advance(32000 - 4000 - 1);
	CHECK_UINT(fk.done_calls, 0);
	fake_advance(1);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_TIMEOUT);
	CHECK_UINT(fk.sends, 1);

	CASE("NICT reliable: a final response completes with timer K at 0 ms");
	fake_reset();
	fk.env.flags = SIP_TXN_ENV_RELIABLE;
	txn = nict_start(0x00020001);
	CHECK_UINT(rx(resp_text(200, "z9hG4bK-c2", "10.0.0.9:5060", 4, "OPTIONS", "peer")),
		   SIP_RX_DELIVERED);
	CHECK_UINT(txn->state, SIP_TXN_S_COMPLETED);
	CHECK_UINT(fk.done_calls, 0);
	fake_advance(0);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_OK);
	CHECK_UINT(sip_txn_count(&mgr), 0);

	CASE("IST reliable: a non-2xx final is sent once and timer G never retransmits it");
	fake_reset();
	fk.env.flags = SIP_TXN_ENV_RELIABLE;
	invite = req_text("INVITE", "z9hG4bK-r-s1", "10.0.0.9:5060", 1, NULL, NULL);
	txn = server_start(invite, "10.0.0.9", 41234);
	CHECK(txn != NULL);
	CHECK(sip_txn_response_send(txn, 486, "SIP/2.0 486 Busy Here\r\n\r\n", 25));
	CHECK_UINT(txn->state, SIP_TXN_S_COMPLETED);
	CHECK_UINT(fk.sends, 1);
	fake_advance(500);
	CHECK_UINT(fk.sends, 1);
	fake_advance(4000);
	CHECK_UINT(fk.sends, 1);
	fake_advance(4000);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(fk.done_calls, 0);

	CASE("IST reliable: the ACK confirms and timer I waits 0 ms");
	CHECK_UINT(rx(req_text("ACK", "z9hG4bK-r-s1", "10.0.0.9:5060", 1, NULL, NULL)),
		   SIP_RX_ABSORBED);
	CHECK_UINT(txn->state, SIP_TXN_S_CONFIRMED);
	CHECK_UINT(fk.done_calls, 0);
	fake_advance(0);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_OK);
	CHECK_UINT(sip_txn_count(&mgr), 0);

	CASE("IST reliable: timer H still reports the missing ACK at 64*T1");
	fake_reset();
	fk.env.flags = SIP_TXN_ENV_RELIABLE;
	invite = req_text("INVITE", "z9hG4bK-r-s2", "10.0.0.9:5060", 1, NULL, NULL);
	txn = server_start(invite, "10.0.0.9", 41234);
	CHECK(sip_txn_response_send(txn, 500, "SIP/2.0 500 Server Error\r\n\r\n", 28));
	fake_advance(31999);
	CHECK_UINT(fk.done_calls, 0);
	fake_advance(1);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_TIMEOUT);
	CHECK_UINT(fk.sends, 1);

	CASE("NIST reliable: timer J waits 0 ms after a final response");
	fake_reset();
	fk.env.flags = SIP_TXN_ENV_RELIABLE;
	bye = req_text("BYE", "z9hG4bK-r-s3", "10.0.0.9:5060", 7, NULL, NULL);
	txn = server_start(bye, "10.0.0.9", 5060);
	CHECK(txn != NULL);
	CHECK(sip_txn_response_send(txn, 200, "SIP/2.0 200 OK\r\n\r\n", 18));
	CHECK_UINT(txn->state, SIP_TXN_S_COMPLETED);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(fk.done_calls, 0);
	fake_advance(0);
	CHECK_UINT(fk.done_calls, 1);
	CHECK_UINT(fk.done_reason, SIP_TXN_DONE_OK);
	CHECK_UINT(sip_txn_count(&mgr), 0);

	CASE("reliable server response takes the arriving src verbatim and keeps NO_CONNECT");
	fake_reset();
	fk.env.flags = SIP_TXN_ENV_RELIABLE;
	{
		struct sip_msg *m = msg_take(req_text("OPTIONS", "z9hG4bK-r-dst1", "10.0.0.9:5070",
						      1, NULL, NULL));
		struct sip_txn_dst src;

		CHECK(m != NULL);
		CHECK(sip_dst_set(&src, "10.0.0.9", 41234));
		src.flags = SIP_DST_F_NO_CONNECT;
		txn = sip_txn_server_start(&mgr, m, &src, 0x00030001);
		CHECK(txn != NULL);
		CHECK_UINT(sip_dst_port(sip_txn_dst_get(txn)), 41234);
		CHECK(sip_txn_dst_get(txn)->flags & SIP_DST_F_NO_CONNECT);
	}

	CASE("the unreliable twin answers on the Via sent-by port and drops NO_CONNECT");
	fake_reset();
	{
		struct sip_msg *m = msg_take(req_text("OPTIONS", "z9hG4bK-r-dst2", "10.0.0.9:5070",
						      1, NULL, NULL));
		struct sip_txn_dst src;

		CHECK(m != NULL);
		CHECK(sip_dst_set(&src, "10.0.0.9", 41234));
		src.flags = SIP_DST_F_NO_CONNECT;
		txn = sip_txn_server_start(&mgr, m, &src, 0x00030001);
		CHECK(txn != NULL);
		CHECK_UINT(sip_dst_port(sip_txn_dst_get(txn)), 5070);
		CHECK(!(sip_txn_dst_get(txn)->flags & SIP_DST_F_NO_CONNECT));
	}

	CASE("reliable ICT: an ACK that cannot be sent drops the transaction, no upcall");
	fake_reset();
	fk.env.flags = SIP_TXN_ENV_RELIABLE;
	fk.auto_ack = true;
	fk.ack_wire = ack_wire;
	CHECK(ict_start(0x000d0001) != NULL);
	fk.send_fail = true;
	CHECK_UINT(rx(resp_text(486, "z9hG4bK-c1", "10.0.0.9:5060", 1, "INVITE", "peer")),
		   SIP_RX_DELIVERED);
	CHECK_UINT(sip_txn_count(&mgr), 0);
	CHECK_UINT(fk.done_calls, 0);
	fk.send_fail = false;
	fake_advance(40000);
	CHECK_UINT(fk.done_calls, 0);

	CASE("reliable IST: a final response whose send fails drops the transaction, no upcall");
	fake_reset();
	fk.env.flags = SIP_TXN_ENV_RELIABLE;
	{
		struct sip_msg *m = msg_take(req_text("INVITE", "z9hG4bK-r-sf", "10.0.0.9:5060", 1,
						      NULL, NULL));
		struct sip_txn_dst src;
		struct sip_txn *stxn;

		CHECK(m != NULL);
		CHECK(sip_dst_set(&src, "10.0.0.9", 41234));
		stxn = sip_txn_server_start(&mgr, m, &src, 0x000d0002);
		CHECK(stxn != NULL);
		fk.send_fail = true;
		CHECK(!sip_txn_response_send(stxn, 486, "SIP/2.0 486 Busy Here\r\n\r\n", 25));
		CHECK_UINT(sip_txn_count(&mgr), 0);
		CHECK_UINT(fk.done_calls, 0);
	}

	CASE("ICT UDP regression: timer A still doubles 500,1000,2000,4000 with flags 0");
	fake_reset();
	fk.env.flags = 0;
	txn = ict_start(0x00010001);
	CHECK(txn != NULL);
	CHECK_UINT(fk.sends, 1);
	fake_advance(499);
	CHECK_UINT(fk.sends, 1);
	fake_advance(1);
	CHECK_UINT(fk.sends, 2);
	fake_advance(999);
	CHECK_UINT(fk.sends, 2);
	fake_advance(1);
	CHECK_UINT(fk.sends, 3);
	fake_advance(2000);
	CHECK_UINT(fk.sends, 4);
	fake_advance(4000);
	CHECK_UINT(fk.sends, 5);

	fake_reset();
}

int main(void)
{
	struct sip_timer_cfg tc;

	sip_timer_cfg_default(&tc);
	sip_txn_mgr_init(&mgr, &fk.env, &tc);
	ua_init(&ua, &mgr, &fk.env, NULL, &ua_cb, NULL);

	timer_cfg_tests();
	handle_tests();
	dst_tests();
	branch_tests();
	ict_tests();
	nict_tests();
	ist_tests();
	resend_tests();
	nist_tests();
	match_tests();
	cancel_tests();
	policy_tests();
	accept_tests();
	accept_pool_tests();
	glare_tests();
	retry_after_tests();
	owner_tests();
	limit_tests();

	h1_tests();
	h2_tests();
	h3_tests();
	h4_tests();
	h5_tests();
	h6_tests();
	h7_tests();
	h8_tests();
	h9_tests();

	trying_tests();
	reliable_tests();

	sip_txn_mgr_done(&mgr);
	ua_done(&ua);

	return tu_summary("test_sip_txn");
}
