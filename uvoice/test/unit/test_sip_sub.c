// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the message waiting intake and the subscription client
 * (src/proto/sip/sub.c).
 *
 * The suite uses the scripted environment of test_sip_reg.c and
 * test_sip_keepalive.c. The test advances a virtual clock by hand, a capture
 * array replaces the socket, and a timer table replaces uloop.
 *
 * Every fixture is an exact length heap buffer. A body is a span and not a C
 * string. A parser that reads one byte past its length therefore reads heap
 * that the test never wrote. malloc(len) with no terminator turns that read
 * into an ASan report instead of a pass. That is the only reason why the
 * fixtures are not literals.
 *
 * The account is fictitious. No provider credential appears in this file.
 */
#include <stdlib.h>

#include "proto/shared/hdr.h"
#include "proto/sip/reg.h"
#include "proto/sip/sub.h"
#include "test_util.h"

#define FAKE_TIMER_MAX	256
#define FAKE_SEND_MAX	64
#define WIRE_MAX	4096
#define TEXT_MAX	4096

#define S_AOR		"sip:uvoice-test@sip.example.net"
#define S_REGISTRAR	"sip:sip.example.net"
#define S_CONTACT	"sip:uvoice-test@203.0.113.9:5060"
#define S_LOCAL_HOST	"10.0.0.5"
#define S_LOCAL_PORT	5060
#define S_HOST		"192.0.2.10"
#define S_PORT		5060
#define S_EXPIRES	1800

struct fake_send {
	char buf[WIRE_MAX + 1];
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
} fk;

static struct sip_txn_mgr mgr;
static struct sip_sub sub;
static struct sip_sub_cfg cfg;
static struct sip_timer_cfg tc_cur;

static struct sip_txn_dst target;
static bool dst_valid;

#include "test_capture.h"
#include "test_clock.h"

/* ------------------------------------------------------------- environment */

static void fake_trace(struct proto_timer_env *env, const char *msg)
{
}

static uint32_t fake_rand32(struct proto_timer_env *env)
{
	static uint32_t n;

	return ++n * 2654435761u;
}

static void fake_resp(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
		      const struct sip_msg *msg)
{
	sip_sub_txn_resp(&sub, txn, msg);
}

static void fake_done(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
		      enum sip_txn_reason reason)
{
	sip_sub_txn_done(&sub, txn, reason);
}

static const struct sip_txn_dst *fake_dst(struct sip_sub *s, void *priv)
{
	return dst_valid ? &target : NULL;
}

/* ------------------------------------------------------------------ config */

static void cfg_default(void)
{
	memset(&cfg, 0, sizeof(cfg));
	cfg.aor = S_AOR;
	cfg.registrar = S_REGISTRAR;
	cfg.contact = S_CONTACT;
	cfg.local_host = S_LOCAL_HOST;
	cfg.local_port = S_LOCAL_PORT;
	cfg.user_agent = "uvoice-test/1";
	cfg.auth_user = "uvoice-test";
	cfg.auth_pass = "not-a-real-secret";
	cfg.expires = S_EXPIRES;
}

/*
 * The NICT retransmission of a SUBSCRIBE and its transaction timeout are not
 * what the cadence cases measure, so both move outside the windows they use.
 * The timeout case advances the clock onto timer F on purpose.
 */
static void tc_quiet(void)
{
	memset(&tc_cur, 0, sizeof(tc_cur));
	tc_cur.t1 = 500;
	tc_cur.e = 600000;
	tc_cur.f = 300000;
	sip_timer_cfg_fixup(&tc_cur);
}

static void fake_reset(void)
{
	sip_sub_done(&sub);
	sip_txn_mgr_done(&mgr);

	memset(&fk, 0, sizeof(fk));
	fk.now = 100000;
	fk.env.send = fake_send_fn;
	fake_clock_bind(&fk.env);
	fk.env.base.trace = fake_trace;
	fk.env.base.rand32 = fake_rand32;
	fk.env.resp = fake_resp;
	fk.env.done = fake_done;

	sip_dst_set(&target, S_HOST, S_PORT);
	dst_valid = true;

	sip_txn_mgr_init(&mgr, &fk.env, &tc_cur);
	sip_sub_init(&sub, &mgr, &fk.env, 0x50010001, &cfg, fake_dst, NULL);
}

/* ------------------------------------------------------- message fixtures */

/*
 * An exact length copy of text on the heap: the parser under test must never
 * read the byte behind it. The caller owns nothing; the previous fixture is
 * released here, and main() releases the last one.
 */
static char *fixture;
static size_t fixture_len;

static struct sip_str fixture_take(const char *text)
{
	size_t len = strlen(text);

	free(fixture);
	fixture = malloc(len ? len : 1);
	fixture_len = len;
	if (len)
		memcpy(fixture, text, len);

	return sip_str_init(fixture, len);
}

static bool body_parse(const char *text, struct sip_mwi *out)
{
	return sip_mwi_body_parse(fixture_take(text), out);
}

static struct sip_msg parsed;

/* the same exact length rule for a whole message */
static struct sip_msg *msg_take(const char *text)
{
	struct sip_str s = fixture_take(text);

	if (msg_parse(&parsed, fixture, s.len) != SIP_PARSE_OK)
		return NULL;

	return &parsed;
}

static bool has(const char *needle)
{
	return strstr(last_text(), needle) != NULL;
}

/* how many times a header name opens a line of the last request */
static unsigned int hdr_count(const char *name)
{
	const char *msg = last_text();
	size_t n = strlen(name);
	unsigned int c = 0;
	const char *p;

	for (p = msg; *p; p++) {
		if (p != msg && p[-1] != '\n')
			continue;
		if (!strncmp(p, name, n))
			c++;
	}

	return c;
}

/* the service route of a registrar, as it arrives and as it is re-emitted */
#define SR_HDRS		"Service-Route: <sip:p1.example.net;lr>\r\n" \
			"Service-Route: <sip:p2.example.net;lr>\r\n"
#define SR_TEXT		"Route: <sip:p1.example.net;lr>\r\n" \
			"Route: <sip:p2.example.net;lr>\r\n"

/*
 * The block the registration stores, built by the production collector out of
 * a real 200 OK. A hand-filled block could hold hops route_hdrs_take()
 * refuses, and the request would then carry a route no registrar could grant.
 */
static const struct sip_route_hdrs *sr_take(const char *hdrs)
{
	static struct sip_route_hdrs sr;
	static char buf[TEXT_MAX];
	struct sip_msg m;
	size_t len;

	len = (size_t)snprintf(buf, sizeof(buf),
			       "SIP/2.0 200 OK\r\n%sContent-Length: 0\r\n\r\n",
			       hdrs);
	route_hdrs_clear(&sr);
	if (len < sizeof(buf) && msg_parse(&m, buf, len) == SIP_PARSE_OK)
		route_hdrs_take(&sr, &m, SIP_HDR_SERVICE_ROUTE);

	return &sr;
}

struct req_info {
	char branch[80];
	char call_id[128];
	char from_tag[64];
	char method[16];
	char ruri[160];
	uint32_t cseq;
	uint32_t expires;
};

static bool req_info_at(unsigned int idx, struct req_info *ri)
{
	struct sip_msg m;
	struct sip_addr a;
	struct sip_via via;
	struct sip_str s;

	memset(ri, 0, sizeof(*ri));
	if (!send_parse(idx, &m) || !m.is_request)
		return false;

	sip_str_copy(ri->method, sizeof(ri->method), m.method_name);
	sip_str_copy(ri->ruri, sizeof(ri->ruri), m.ruri);
	if (hdr_via_top(&m, &via))
		sip_str_copy(ri->branch, sizeof(ri->branch), via.branch);
	if (hdr_call_id_get(&m, &s))
		sip_str_copy(ri->call_id, sizeof(ri->call_id), s);
	if (hdr_addr_get(&m, SIP_HDR_FROM, &a))
		sip_str_copy(ri->from_tag, sizeof(ri->from_tag), a.tag);
	hdr_cseq_get(&m, &ri->cseq, NULL);
	hdr_uint_get(&m, SIP_HDR_EXPIRES, &ri->expires);

	return true;
}

static bool last_req(struct req_info *ri)
{
	return fk.sends ? req_info_at(fk.sends - 1, ri) : false;
}

/* a response to the SUBSCRIBE that went out last */
static void reply_full(uint32_t code, const char *extra)
{
	struct req_info ri;
	char text[TEXT_MAX];
	size_t len;

	if (!last_req(&ri))
		return;

	len = (size_t)snprintf(text, sizeof(text),
			       "SIP/2.0 %u %s\r\n"
			       "Via: SIP/2.0/UDP %s:%u;branch=%s\r\n"
			       "From: <%s>;tag=%s\r\n"
			       "To: <%s>;tag=notifier-tag\r\n"
			       "Call-ID: %s\r\n"
			       "CSeq: %u SUBSCRIBE\r\n"
			       "%s"
			       "Content-Length: 0\r\n\r\n",
			       code, sip_status_reason(code), S_LOCAL_HOST, S_LOCAL_PORT,
			       ri.branch, S_AOR, ri.from_tag, S_AOR, ri.call_id, ri.cseq,
			       extra ? extra : "");

	if (len >= sizeof(text))
		return;

	reply_recv(&mgr, text, len);
}

static void reply(uint32_t code)
{
	reply_full(code, NULL);
}

/* a NOTIFY that belongs to the subscription this instance holds */
static struct sip_msg *notify_of_sub(const char *sub_state, const char *body)
{
	char text[TEXT_MAX];

	snprintf(text, sizeof(text),
		 "NOTIFY %s SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP %s:%u;branch=z9hG4bK-notify\r\n"
		 "From: <%s>;tag=notifier-tag\r\n"
		 "To: <%s>;tag=%s\r\n"
		 "Call-ID: %s\r\n"
		 "CSeq: 4 NOTIFY\r\n"
		 "Event: message-summary\r\n"
		 "Subscription-State: %s\r\n"
		 "Content-Type: application/simple-message-summary\r\n"
		 "Content-Length: %u\r\n\r\n%s",
		 S_CONTACT, S_HOST, S_PORT, S_AOR, S_AOR, sub.from_tag,
		 sip_sub_call_id(&sub), sub_state, (unsigned int)strlen(body), body);

	return msg_take(text);
}

/* ------------------------------------------------------------ body grammar */

static void body_tests(void)
{
	struct sip_mwi m;

	CASE("body: the RFC 3842 shape gives the flag and both counts");
	CHECK(body_parse("Messages-Waiting: yes\r\n"
			 "Message-Account: sip:uvoice-test@sip.example.net\r\n"
			 "Voice-Message: 2/1 (0/0)\r\n", &m));
	CHECK(m.waiting);
	CHECK(m.counts_valid);
	CHECK_UINT(m.new_msgs, 2);
	CHECK_UINT(m.old_msgs, 1);

	CASE("body: no clears the flag");
	CHECK(body_parse("Messages-Waiting: no\r\nVoice-Message: 0/0\r\n", &m));
	CHECK(!m.waiting);
	CHECK(m.counts_valid);
	CHECK_UINT(m.new_msgs, 0);

	CASE("body: the value is case insensitive");
	CHECK(body_parse("Messages-Waiting: YES\r\n", &m));
	CHECK(m.waiting);
	CHECK(body_parse("messages-waiting: No\r\n", &m));
	CHECK(!m.waiting);

	/* the Messages-Waiting line sets the flag, whatever the counts say */
	CASE("body: yes with 0/0 keeps the flag AND the counts");
	CHECK(body_parse("Messages-Waiting: yes\r\nVoice-Message: 0/0\r\n", &m));
	CHECK(m.waiting);
	CHECK(m.counts_valid);
	CHECK_UINT(m.new_msgs, 0);
	CHECK_UINT(m.old_msgs, 0);

	CASE("body: the counts are optional and their absence is not a zero count");
	CHECK(body_parse("Messages-Waiting: yes\r\n", &m));
	CHECK(m.waiting);
	CHECK(!m.counts_valid);
	CHECK_UINT(m.new_msgs, 0);

	CASE("body: a count line that does not parse leaves the flag standing");
	CHECK(body_parse("Messages-Waiting: yes\r\nVoice-Message: many/1\r\n", &m));
	CHECK(m.waiting);
	CHECK(!m.counts_valid);

	CASE("body: an unknown message class is ignored");
	CHECK(body_parse("Messages-Waiting: yes\r\n"
			 "Fax-Message: 3/4\r\n"
			 "Voice-Message: 5/6\r\n", &m));
	CHECK_UINT(m.new_msgs, 5);
	CHECK_UINT(m.old_msgs, 6);

	CASE("body: the counts may precede the flag");
	CHECK(body_parse("Voice-Message: 7/8\r\nMessages-Waiting: yes\r\n", &m));
	CHECK(m.waiting);
	CHECK_UINT(m.new_msgs, 7);

	CASE("body: bare LF terminators parse");
	CHECK(body_parse("Messages-Waiting: yes\nVoice-Message: 1/2\n", &m));
	CHECK(m.waiting);
	CHECK_UINT(m.new_msgs, 1);
	CHECK_UINT(m.old_msgs, 2);

	CASE("body: the last line needs no terminator");
	CHECK(body_parse("Messages-Waiting: yes\r\nVoice-Message: 9/9", &m));
	CHECK(m.counts_valid);
	CHECK_UINT(m.new_msgs, 9);

	/* the exact token rule: a substring scan of the body would pass this */
	CASE("body: a value that only contains yes is refused");
	CHECK(!body_parse("Messages-Waiting: yesterday\r\n", &m));
	CHECK(!m.waiting);
	CHECK(!m.counts_valid);

	CASE("body: the words inside another header are not the grammar");
	CHECK(!body_parse("Subject: messages-waiting: yes\r\n", &m));
	CHECK(!m.waiting);

	CASE("body: an empty body carries nothing");
	CHECK(!body_parse("", &m));

	CASE("body: a body with no Messages-Waiting line is refused whole");
	CHECK(!body_parse("Voice-Message: 2/1\r\n", &m));
	CHECK(!m.counts_valid);
	CHECK_UINT(m.new_msgs, 0);

	CASE("body: the first Messages-Waiting decides");
	CHECK(body_parse("Messages-Waiting: yes\r\nMessages-Waiting: no\r\n", &m));
	CHECK(m.waiting);

	CASE("body: a count above the 32 bit range is refused, the flag survives");
	CHECK(body_parse("Messages-Waiting: yes\r\nVoice-Message: 4294967296/1\r\n", &m));
	CHECK(m.waiting);
	CHECK(!m.counts_valid);

	CASE("body: whitespace around the value is trimmed");
	CHECK(body_parse("Messages-Waiting:   yes  \r\nVoice-Message:  3 / 4 \r\n", &m));
	CHECK(m.waiting);
	CHECK(m.counts_valid);
	CHECK_UINT(m.new_msgs, 3);
	CHECK_UINT(m.old_msgs, 4);
}

/* ---------------------------------------------------------- event routing */

static void event_tests(void)
{
	struct sip_msg *m;

	CASE("event: a message-summary NOTIFY is ours");
	m = msg_take("NOTIFY sip:5551000@10.0.0.5 SIP/2.0\r\n"
		     "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
		     "From: <sip:sw@example.net>;tag=a\r\n"
		     "To: <sip:5551000@example.net>\r\n"
		     "Call-ID: c1\r\n"
		     "CSeq: 1 NOTIFY\r\n"
		     "Event: message-summary\r\n"
		     "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(sip_sub_event_class(m) == SIP_SUB_EV_MWI);

	CASE("event: the package is case insensitive and its id parameter is ignored");
	m = msg_take("NOTIFY sip:5551000@10.0.0.5 SIP/2.0\r\n"
		     "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
		     "From: <sip:sw@example.net>;tag=a\r\n"
		     "To: <sip:5551000@example.net>\r\n"
		     "Call-ID: c1\r\n"
		     "CSeq: 1 NOTIFY\r\n"
		     "Event: Message-Summary;id=42\r\n"
		     "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(sip_sub_event_class(m) == SIP_SUB_EV_MWI);

	/*
	 * The one package this file names but does not serve. A REFER creates
	 * an implicit subscription the call layer holds, so the account layer
	 * has to tell it apart from a package nobody serves: 489 to a transfer
	 * NOTIFY would lose the outcome of every attended transfer before the
	 * leg ever saw it.
	 */
	CASE("event: a refer NOTIFY is named for the call layer, not answered 489");
	m = msg_take("NOTIFY sip:5551000@10.0.0.5 SIP/2.0\r\n"
		     "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
		     "From: <sip:sw@example.net>;tag=a\r\n"
		     "To: <sip:5551000@example.net>;tag=b\r\n"
		     "Call-ID: c1\r\n"
		     "CSeq: 4 NOTIFY\r\n"
		     "Event: refer\r\n"
		     "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(sip_sub_event_class(m) == SIP_SUB_EV_REFER);

	CASE("event: with its id parameter, which this layer does not read");
	m = msg_take("NOTIFY sip:5551000@10.0.0.5 SIP/2.0\r\n"
		     "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
		     "From: <sip:sw@example.net>;tag=a\r\n"
		     "To: <sip:5551000@example.net>;tag=b\r\n"
		     "Call-ID: c1\r\n"
		     "CSeq: 4 NOTIFY\r\n"
		     "Event: Refer;id=42\r\n"
		     "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(sip_sub_event_class(m) == SIP_SUB_EV_REFER);

	CASE("event: a package that merely starts with it is another package");
	m = msg_take("NOTIFY sip:5551000@10.0.0.5 SIP/2.0\r\n"
		     "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
		     "From: <sip:sw@example.net>;tag=a\r\n"
		     "To: <sip:5551000@example.net>;tag=b\r\n"
		     "Call-ID: c1\r\n"
		     "CSeq: 4 NOTIFY\r\n"
		     "Event: refer-to-nothing\r\n"
		     "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(sip_sub_event_class(m) == SIP_SUB_EV_OTHER);

	CASE("event: another package is answered 489, not taken");
	m = msg_take("NOTIFY sip:5551000@10.0.0.5 SIP/2.0\r\n"
		     "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
		     "From: <sip:sw@example.net>;tag=a\r\n"
		     "To: <sip:5551000@example.net>\r\n"
		     "Call-ID: c1\r\n"
		     "CSeq: 1 NOTIFY\r\n"
		     "Event: presence\r\n"
		     "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(sip_sub_event_class(m) == SIP_SUB_EV_OTHER);

	CASE("event: a NOTIFY with no Event header names no package we serve");
	m = msg_take("NOTIFY sip:5551000@10.0.0.5 SIP/2.0\r\n"
		     "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
		     "From: <sip:sw@example.net>;tag=a\r\n"
		     "To: <sip:5551000@example.net>\r\n"
		     "Call-ID: c1\r\n"
		     "CSeq: 1 NOTIFY\r\n"
		     "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(sip_sub_event_class(m) == SIP_SUB_EV_OTHER);

	CASE("event: another method is none of this file's business");
	m = msg_take("INVITE sip:5551000@10.0.0.5 SIP/2.0\r\n"
		     "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-1\r\n"
		     "From: <sip:sw@example.net>;tag=a\r\n"
		     "To: <sip:5551000@example.net>\r\n"
		     "Call-ID: c1\r\n"
		     "CSeq: 1 INVITE\r\n"
		     "Event: message-summary\r\n"
		     "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(sip_sub_event_class(m) == SIP_SUB_EV_NONE);
}

/* ------------------------------------------------------- unsolicited intake */

static struct sip_msg *notify_ctype(const char *ctype, const char *body)
{
	char text[TEXT_MAX];
	char ctype_hdr[128] = "";

	if (ctype)
		snprintf(ctype_hdr, sizeof(ctype_hdr), "Content-Type: %s\r\n", ctype);

	snprintf(text, sizeof(text),
		 "NOTIFY sip:5551000@10.0.0.5 SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-2\r\n"
		 "From: <sip:sw@example.net>;tag=switch\r\n"
		 "To: <sip:5551000@example.net>\r\n"
		 "Call-ID: unsolicited-1\r\n"
		 "CSeq: 1 NOTIFY\r\n"
		 "Event: message-summary\r\n"
		 "%s"
		 "Content-Length: %u\r\n\r\n%s",
		 ctype_hdr, (unsigned int)strlen(body), body);

	return msg_take(text);
}

static void intake_tests(void)
{
	const char *yes = "Messages-Waiting: yes\r\nVoice-Message: 2/1\r\n";
	struct sip_msg *m;
	struct sip_mwi mwi;

	tc_quiet();
	cfg_default();
	fake_reset();

	CASE("intake: the RFC content type is taken with no subscription at all");
	m = notify_ctype("application/simple-message-summary", yes);
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(NULL, m, &mwi) == SIP_SUB_RX_STORE);
	CHECK(mwi.waiting);
	CHECK_UINT(mwi.new_msgs, 2);
	CHECK_UINT(mwi.old_msgs, 1);

	/* Sylantro switches send this content type */
	CASE("intake: application/message-summary is taken too");
	m = notify_ctype("application/message-summary", yes);
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(NULL, m, &mwi) == SIP_SUB_RX_STORE);
	CHECK(mwi.waiting);

	CASE("intake: a content type parameter does not spoil the match");
	m = notify_ctype("application/simple-message-summary;charset=UTF-8", yes);
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(NULL, m, &mwi) == SIP_SUB_RX_STORE);
	CHECK(mwi.waiting);

	CASE("intake: a missing content type leaves the grammar to decide");
	m = notify_ctype(NULL, yes);
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(NULL, m, &mwi) == SIP_SUB_RX_STORE);
	CHECK(mwi.waiting);

	CASE("intake: another content type stores nothing");
	m = notify_ctype("application/sdp", yes);
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(NULL, m, &mwi) == SIP_SUB_RX_BAD_BODY);
	CHECK(!mwi.waiting);

	CASE("intake: a body that does not parse stores nothing");
	m = notify_ctype("application/simple-message-summary", "not a summary\r\n");
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(NULL, m, &mwi) == SIP_SUB_RX_BAD_BODY);
	CHECK(!mwi.waiting);

	CASE("intake: an empty body stores nothing");
	m = notify_ctype("application/simple-message-summary", "");
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(NULL, m, &mwi) == SIP_SUB_RX_BAD_BODY);

	CASE("intake: a NOTIFY that matches no subscription counts as unsolicited");
	m = notify_ctype("application/simple-message-summary", yes);
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(&sub, m, &mwi) == SIP_SUB_RX_STORE);
	CHECK_UINT(sub.stat.notify_unsolicited, 1);
	CHECK_UINT(sub.stat.notify_ours, 0);

	CASE("intake: a bad body is counted");
	m = notify_ctype("application/simple-message-summary", "Messages-Waiting: maybe\r\n");
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(&sub, m, &mwi) == SIP_SUB_RX_BAD_BODY);
	CHECK_UINT(sub.stat.notify_bad_body, 1);
}

/* --------------------------------------------------------- the client core */

static void client_tests(void)
{
	struct req_info ri, ri2;
	char first_call_id[128];

	tc_quiet();
	cfg_default();
	fake_reset();

	CASE("client: nothing leaves before the account starts it");
	CHECK_UINT(fk.sends, 0);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_OFF);

	CASE("client: the start sends one SUBSCRIBE at once");
	sip_sub_start(&sub);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(sub.stat.sent, 1);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_TRYING);

	CASE("client: the request URI is the registrar and the method is SUBSCRIBE");
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "SUBSCRIBE"));
	CHECK(!strcmp(ri.ruri, S_REGISTRAR));

	CASE("client: From and To are the address of record");
	CHECK(has("From: <" S_AOR ">;tag="));
	CHECK(has("To: <" S_AOR ">"));

	CASE("client: the To carries no tag before the notifier gives one");
	CHECK(!has("To: <" S_AOR ">;tag="));

	CASE("client: the Contact is the registered one");
	CHECK(has("Contact: <" S_CONTACT ">"));

	CASE("client: the event package and the accepted body type are named");
	CHECK(has("Event: message-summary"));
	CHECK(has("Accept: application/simple-message-summary"));

	CASE("client: the expiry is the account's registration expiry");
	CHECK_UINT(ri.expires, S_EXPIRES);

	CASE("client: the Via asks for rport and Max-Forwards is 70");
	CHECK(has(";rport"));
	CHECK(has("Max-Forwards: 70"));

	CASE("client: a second start while one is running sends nothing");
	sip_sub_start(&sub);
	CHECK_UINT(fk.sends, 1);

	CASE("client: the 2xx makes the subscription active and arms the refresh");
	snprintf(first_call_id, sizeof(first_call_id), "%s", ri.call_id);
	reply_full(200, "Expires: 1800\r\n");
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_ACTIVE);
	CHECK_UINT(sip_sub_expires(&sub), 1800);
	CHECK_UINT(sub.stat.ok, 1);

	CASE("client: nothing leaves before the refresh is due");
	fake_advance(sip_reg_refresh_secs(1800) * 1000 - 1000);
	CHECK_UINT(fk.sends, 1);

	CASE("client: the refresh keeps the call-id and grows the cseq");
	fake_advance(1000);
	CHECK_UINT(fk.sends, 2);
	CHECK(last_req(&ri2));
	CHECK(!strcmp(ri2.call_id, first_call_id));
	CHECK(!strcmp(ri2.from_tag, ri.from_tag));
	CHECK(ri2.cseq > ri.cseq);

	CASE("client: the refresh carries the To tag the notifier gave");
	CHECK(has("To: <" S_AOR ">;tag=notifier-tag"));

	CASE("client: an expiry the notifier lowers moves the next refresh");
	reply_full(200, "Expires: 600\r\n");
	CHECK_UINT(sip_sub_expires(&sub), 600);
	fake_advance(sip_reg_refresh_secs(600) * 1000 - 1000);
	CHECK_UINT(fk.sends, 2);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 3);

	CASE("client: a 2xx with no Expires falls back to what was asked for");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	reply(200);
	CHECK_UINT(sip_sub_expires(&sub), S_EXPIRES);

	CASE("client: a 2xx granting nothing leaves no subscription to refresh");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	reply_full(200, "Expires: 0\r\n");
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_RETRY);
	CHECK_UINT(sip_sub_expires(&sub), 0);
	fake_advance(SIP_SUB_RETRY_S * 1000);
	CHECK_UINT(fk.sends, 2);

	CASE("client: no destination is a failure that retries, not a send");
	tc_quiet();
	cfg_default();
	fake_reset();
	dst_valid = false;
	sip_sub_start(&sub);
	CHECK_UINT(fk.sends, 0);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_RETRY);
	dst_valid = true;
	fake_advance(SIP_SUB_RETRY_S * 1000);
	CHECK_UINT(fk.sends, 1);

	CASE("client: a configuration with no address of record stays off");
	tc_quiet();
	cfg_default();
	cfg.aor = NULL;
	fake_reset();
	sip_sub_start(&sub);
	CHECK_UINT(fk.sends, 0);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_OFF);
	cfg_default();
}

/*
 * RFC 3608. The SUBSCRIBE that creates the subscription carries the route
 * that the registrar granted. A refresh and an unsubscribe carry the To tag
 * that the notifier gave. Both are therefore in-dialog requests, and this file
 * keeps no route set for them (sip/sub.h).
 */
static void service_route_tests(void)
{
	CASE("client: the SUBSCRIBE carries no Route without a service route");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	CHECK_UINT(fk.sends, 1);
	CHECK(!has("\r\nRoute:"));

	CASE("client: a stored service route rides the SUBSCRIBE in order");
	tc_quiet();
	cfg_default();
	cfg.service_route = sr_take(SR_HDRS);
	fake_reset();
	sip_sub_start(&sub);
	CHECK_UINT(fk.sends, 1);
	CHECK(has(SR_TEXT));
	CHECK_UINT(hdr_count("Route:"), 2);
	/* the position on the wire: the hops open the line after CSeq and the
	 * Contact follows them */
	CHECK(has("\r\nCSeq: 1 SUBSCRIBE\r\n" SR_TEXT "Contact:"));

	CASE("client: a refresh inside the dialog carries no service route");
	reply_full(200, "Expires: 1800\r\n");
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_ACTIVE);
	fake_advance(sip_reg_refresh_secs(1800) * 1000);
	CHECK_UINT(fk.sends, 2);
	CHECK(has("To: <" S_AOR ">;tag=notifier-tag"));
	CHECK(!has("\r\nRoute:"));

	CASE("client: an unsubscribe carries no service route");
	sip_sub_stop(&sub, true);
	CHECK_UINT(fk.sends, 3);
	CHECK(has("To: <" S_AOR ">;tag=notifier-tag"));
	CHECK(has("Expires: 0"));
	CHECK(!has("\r\nRoute:"));

	/*
	 * The notifier answered nothing, so this request opens no dialog and
	 * closes none: it is the first SUBSCRIBE again with Expires 0, toward
	 * the same registrar, and the route belongs on it.
	 */
	CASE("client: an unsubscribe before the first 2xx carries the service "
	     "route");
	tc_quiet();
	cfg_default();
	cfg.service_route = sr_take(SR_HDRS);
	fake_reset();
	sip_sub_start(&sub);
	CHECK_UINT(fk.sends, 1);
	sip_sub_stop(&sub, true);
	CHECK_UINT(fk.sends, 2);
	CHECK(!has("To: <" S_AOR ">;tag="));
	CHECK(has("Expires: 0"));
	CHECK(has(SR_TEXT));
	cfg_default();
}

/* ------------------------------------------------------ the transport token */

/*
 * The Via carries the token after "SIP/2.0/" (sip/sub.h). The Contact is the
 * registered one and reaches the wire verbatim. reg.c therefore writes the
 * ";transport=" parameter, and this file never does.
 */
static void transport_tests(void)
{
	CASE("transport: a NULL token keeps the UDP Via the SUBSCRIBE always had");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	CHECK_UINT(fk.sends, 1);
	CHECK(has("Via: SIP/2.0/UDP " S_LOCAL_HOST ":5060;branch=z9hG4bK"));
	CHECK(!has(";transport="));

	CASE("transport: TCP reaches the Via of the SUBSCRIBE");
	tc_quiet();
	cfg_default();
	cfg.transport = "TCP";
	fake_reset();
	sip_sub_start(&sub);
	CHECK_UINT(fk.sends, 1);
	CHECK(has("Via: SIP/2.0/TCP " S_LOCAL_HOST ":5060;branch=z9hG4bK"));
	CHECK(has(";rport"));

	CASE("transport: the registered Contact goes out verbatim, unchanged here");
	CHECK(has("Contact: <" S_CONTACT ">"));
	CHECK(!has(";transport="));

	CASE("transport: TLS is one more token");
	tc_quiet();
	cfg_default();
	cfg.transport = "TLS";
	fake_reset();
	sip_sub_start(&sub);
	CHECK(has("Via: SIP/2.0/TLS " S_LOCAL_HOST ":5060;branch=z9hG4bK"));
	CHECK(has("Contact: <" S_CONTACT ">"));

	CASE("transport: a registered Contact that carries the token passes through");
	tc_quiet();
	cfg_default();
	cfg.transport = "TCP";
	cfg.contact = "sip:uvoice-test@203.0.113.9:5060;transport=tcp";
	fake_reset();
	sip_sub_start(&sub);
	CHECK(has("Contact: <sip:uvoice-test@203.0.113.9:5060;transport=tcp>"));
	CHECK(!has(">;transport="));
	cfg_default();
}

/* ------------------------------------------------------------ the failures */

static void failure_tests(void)
{
	struct req_info ri, ri2;

	CASE("failure: a challenge is answered once with credentials");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	CHECK(last_req(&ri));
	reply_full(401, "WWW-Authenticate: Digest realm=\"example.net\", "
		   "nonce=\"abc123\"\r\n");
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(sub.stat.challenged, 1);
	CHECK(has("Authorization: Digest"));
	CHECK(has("uri=\"" S_REGISTRAR "\""));

	CASE("failure: the same challenge twice is not tried a third time");
	reply_full(401, "WWW-Authenticate: Digest realm=\"example.net\", "
		   "nonce=\"abc123\"\r\n");
	CHECK_UINT(fk.sends, 2);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_RETRY);

	CASE("failure: a package refusal stops the client instead of retrying");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	reply(489);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_REFUSED);
	CHECK_UINT(sub.stat.refused, 1);
	fake_advance(2 * SIP_SUB_RETRY_S * 1000);
	CHECK_UINT(fk.sends, 1);

	CASE("failure: a 403 is the same refusal");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	reply(403);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_REFUSED);

	CASE("failure: a 500 retries after the fixed delay");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	reply(500);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_RETRY);
	fake_advance(SIP_SUB_RETRY_S * 1000 - 1000);
	CHECK_UINT(fk.sends, 1);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 2);

	CASE("failure: a transaction timeout retries the same way");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	fake_advance(tc_cur.f + 1000);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_RETRY);
	CHECK_UINT(sub.stat.failed, 1);

	CASE("failure: a 423 asks again with the minimum the notifier named");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	CHECK(last_req(&ri));
	CHECK_UINT(ri.expires, S_EXPIRES);
	reply_full(423, "Min-Expires: 3600\r\n");
	CHECK_UINT(fk.sends, 2);
	CHECK(last_req(&ri2));
	CHECK_UINT(ri2.expires, 3600);

	CASE("failure: a second 423 is a loop and is refused");
	reply_full(423, "Min-Expires: 7200\r\n");
	CHECK_UINT(fk.sends, 2);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_RETRY);

	CASE("restart: a 481 mints a fresh call-id and subscribes again");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	CHECK(last_req(&ri));
	reply(481);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(sub.stat.restarted, 1);
	CHECK(last_req(&ri2));
	CHECK(strcmp(ri2.call_id, ri.call_id) != 0);
	CHECK(strcmp(ri2.from_tag, ri.from_tag) != 0);

	CASE("restart: a second 481 waits instead of spinning");
	reply(481);
	CHECK_UINT(fk.sends, 2);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_RETRY);
	fake_advance(SIP_SUB_RETRY_S * 1000);
	CHECK_UINT(fk.sends, 3);
}

/* ------------------------------------------- the subscription's own NOTIFYs */

static void notify_tests(void)
{
	const char *yes = "Messages-Waiting: yes\r\nVoice-Message: 4/2\r\n";
	struct req_info ri, ri2, ri3;
	struct sip_mwi mwi;
	struct sip_msg *m;

	CASE("notify: a NOTIFY of this subscription is recognised");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	reply_full(200, "Expires: 1800\r\n");
	m = notify_of_sub("active;expires=1800", yes);
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(&sub, m, &mwi) == SIP_SUB_RX_STORE);
	CHECK_UINT(sub.stat.notify_ours, 1);
	CHECK_UINT(sub.stat.notify_unsolicited, 0);
	CHECK(mwi.waiting);
	CHECK_UINT(mwi.new_msgs, 4);

	CASE("notify: the subscription state expiry moves the refresh");
	m = notify_of_sub("active;expires=120", yes);
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(&sub, m, &mwi) == SIP_SUB_RX_STORE);
	CHECK_UINT(sip_sub_expires(&sub), 120);
	fake_advance(sip_reg_refresh_secs(120) * 1000 - 1000);
	CHECK_UINT(fk.sends, 1);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 2);

	CASE("notify: a foreign call-id is unsolicited, not ours");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	reply_full(200, "Expires: 1800\r\n");
	m = notify_ctype("application/simple-message-summary", yes);
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(&sub, m, &mwi) == SIP_SUB_RX_STORE);
	CHECK_UINT(sub.stat.notify_ours, 0);
	CHECK_UINT(sub.stat.notify_unsolicited, 1);

	CASE("notify: an unsolicited NOTIFY still stores its state");
	CHECK(mwi.waiting);
	CHECK_UINT(mwi.new_msgs, 4);
	CHECK_UINT(mwi.old_msgs, 2);

	CASE("notify: a terminated subscription is taken up again");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	reply_full(200, "Expires: 1800\r\n");
	m = notify_of_sub("terminated;reason=timeout", yes);
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(&sub, m, &mwi) == SIP_SUB_RX_STORE);
	CHECK_UINT(sub.stat.restarted, 1);
	CHECK_UINT(fk.sends, 2);

	CASE("notify: the body of a terminated NOTIFY is still stored");
	CHECK(mwi.waiting);
	CHECK_UINT(mwi.new_msgs, 4);

	/*
	 * The dead identity is dropped before the delay. A subscription the
	 * notifier ended is dead whether the next attempt runs now or in a
	 * minute: anything carrying that Call-ID and To tag earns the same
	 * answer, so a delayed attempt that kept them would refresh a dead
	 * subscription and the pair would loop through the delay forever.
	 */
	CASE("restart: a termination before the 2xx takes a fresh identity at once");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	CHECK(last_req(&ri));
	m = notify_of_sub("terminated;reason=deactivated", yes);
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(&sub, m, &mwi) == SIP_SUB_RX_STORE);
	CHECK_UINT(fk.sends, 2);
	CHECK(last_req(&ri2));
	CHECK(strcmp(ri2.call_id, ri.call_id) != 0);

	CASE("restart: a second termination waits, and mints the identity anyway");
	m = notify_of_sub("terminated;reason=deactivated", yes);
	CHECK(m != NULL);
	CHECK(sip_sub_notify_recv(&sub, m, &mwi) == SIP_SUB_RX_STORE);
	CHECK_UINT(fk.sends, 2);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_RETRY);
	CHECK(strcmp(sip_sub_call_id(&sub), ri2.call_id) != 0);

	CASE("restart: so the delayed attempt is a NEW subscription, not the dead one");
	fake_advance(SIP_SUB_RETRY_S * 1000);
	CHECK_UINT(fk.sends, 3);
	CHECK(last_req(&ri3));
	CHECK(strcmp(ri3.call_id, ri2.call_id) != 0);
	CHECK(strcmp(ri3.from_tag, ri2.from_tag) != 0);
	CHECK(!has("To: <" S_AOR ">;tag="));
}

/* ------------------------------------------------------------ stop and stay */

static void stop_tests(void)
{
	struct req_info ri, ri2;

	CASE("stop: the unsubscribe carries expires 0 on the same subscription");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	CHECK(last_req(&ri));
	reply_full(200, "Expires: 1800\r\n");
	sip_sub_stop(&sub, true);
	CHECK_UINT(fk.sends, 2);
	CHECK(last_req(&ri2));
	CHECK_UINT(ri2.expires, 0);
	CHECK(!strcmp(ri2.call_id, ri.call_id));
	CHECK(ri2.cseq > ri.cseq);

	CASE("stop: its answer leaves the client off with no retry");
	reply(200);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_OFF);
	fake_advance(2 * SIP_SUB_RETRY_S * 1000);
	CHECK_UINT(fk.sends, 2);

	CASE("stop: an unsubscribe the notifier refuses still ends the client");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	reply_full(200, "Expires: 1800\r\n");
	sip_sub_stop(&sub, true);
	reply(481);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_OFF);
	fake_advance(2 * SIP_SUB_RETRY_S * 1000);
	CHECK_UINT(fk.sends, 2);

	CASE("stop: without the unsubscribe nothing goes on the wire");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	reply_full(200, "Expires: 1800\r\n");
	sip_sub_stop(&sub, false);
	CHECK_UINT(fk.sends, 1);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_OFF);

	CASE("stop: a refused client has no subscription to unsubscribe");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_sub_start(&sub);
	reply(489);
	sip_sub_stop(&sub, true);
	CHECK_UINT(fk.sends, 1);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_OFF);

	CASE("stop: a stopped client can be started again");
	sip_sub_start(&sub);
	CHECK_UINT(fk.sends, 2);
	CHECK(sip_sub_status_get(&sub) == SIP_SUB_TRYING);
}

int main(void)
{
	body_tests();
	event_tests();
	intake_tests();
	client_tests();
	service_route_tests();
	transport_tests();
	failure_tests();
	notify_tests();
	stop_tests();

	SUITE_CHECK_UINT(fake_timer_dropped, 0);

	sip_sub_done(&sub);
	sip_txn_mgr_done(&mgr);
	free(fixture);

	return tu_summary("test_sip_sub");
}
