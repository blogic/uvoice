// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the keepalive client (src/proto/sip/keepalive.c).
 *
 * The suite uses the scripted environment of test_sip_reg.c. The test advances
 * a virtual clock by hand, a capture array replaces the socket, and a timer
 * table replaces uloop. The OPTIONS ping runs on the real transaction layer. A
 * ping, a skip and a timeout are therefore observable to the millisecond
 * without a sleep.
 *
 * The account is fictitious. No provider credential appears in this file.
 */
#include <stdlib.h>

#include "proto/sip/keepalive.h"
#include "proto/shared/hdr.h"
#include "test_util.h"

#define FAKE_TIMER_MAX	256
#define FAKE_SEND_MAX	64
#define WIRE_MAX	4096
#define TEXT_MAX	4096

#define K_AOR		"sip:uvoice-test@sip.example.net"
#define K_REGISTRAR	"sip:sip.example.net"
#define K_LOCAL_HOST	"10.0.0.5"
#define K_LOCAL_PORT	5060
#define K_HOST		"192.0.2.10"
#define K_PORT		5060

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

	unsigned int dst_calls;
} fk;

static struct sip_txn_mgr mgr;
static struct sip_ka ka;
static struct sip_ka_cfg cfg;
static struct sip_timer_cfg tc_cur;

static struct sip_txn_dst target;
static bool dst_valid;

/*
 * The destinations after the sticky one, in the order the round asks for
 * them; dst_hole names an index that answers NULL ahead of the rest, so the
 * end-of-list rule is observable. dst_update makes the next index 0 call run
 * a config update from inside the callback, the account's re-read path, and
 * dst_update_secs is the CRLF interval that update carries when nonzero.
 */
#define EXTRA_MAX	3
static struct sip_txn_dst extra[EXTRA_MAX];
static unsigned int extra_count;
static unsigned int dst_hole;
static bool dst_update;
static uint32_t dst_update_secs;
static uint16_t fail_port;

#include "test_capture.h"
#include "test_clock.h"

/* ------------------------------------------------------------- environment */

/* The refusal comes before the capture, so a send to fail_port is not
 * counted, the same as a send under send_fail. */
static int ka_send_fn(struct sip_txn_env *env, const void *buf, size_t len,
		      const struct sip_txn_dst *dst)
{
	if (fail_port && sip_dst_port(dst) == fail_port)
		return -1;

	return fake_send_fn(env, buf, len, dst);
}

static void fake_trace(struct proto_timer_env *env, const char *msg)
{
}

/* a stand-in for the OS entropy the daemon installs; a counter is enough to
 * prove that two managers stop drawing the same stream */
static uint32_t fake_rand32(struct proto_timer_env *env)
{
	static uint32_t n;

	return ++n * 2654435761u;
}

static void fake_resp(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
		      const struct sip_msg *msg)
{
	sip_ka_txn_resp(&ka, txn, msg);
}

static void fake_done(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
		      enum sip_txn_reason reason)
{
	sip_ka_txn_done(&ka, txn, reason);
}

static const struct sip_txn_dst *fake_dst_at(struct sip_ka *k, void *priv,
					     unsigned int idx)
{
	fk.dst_calls++;

	if (!idx && dst_update) {
		dst_update = false;
		if (dst_update_secs)
			cfg.crlf_interval_s = dst_update_secs;
		sip_ka_cfg_update(k, &cfg);
	}

	if (!idx)
		return dst_valid ? &target : NULL;
	if (dst_hole && idx == dst_hole)
		return NULL;
	if (idx <= extra_count)
		return &extra[idx - 1];

	return NULL;
}

/* the extra destinations: K_HOST on the ports after K_PORT */
static void extra_set(unsigned int count)
{
	unsigned int i;

	extra_count = count;
	for (i = 0; i < count && i < EXTRA_MAX; i++)
		sip_dst_set(&extra[i], K_HOST, (uint16_t)(K_PORT + 1 + i));
}

/* ------------------------------------------------------------------ config */

static void cfg_default(void)
{
	memset(&cfg, 0, sizeof(cfg));
	cfg.aor = K_AOR;
	cfg.registrar = K_REGISTRAR;
	cfg.local_host = K_LOCAL_HOST;
	cfg.local_port = K_LOCAL_PORT;
	cfg.user_agent = "uvoice-test/1";
	cfg.crlf_enabled = true;
	cfg.crlf_interval_s = 30;
	cfg.options_interval_s = 60;
}

/* only the CRLF side; the OPTIONS side would add datagrams to every count */
static void cfg_crlf_only(uint32_t secs)
{
	cfg_default();
	cfg.crlf_interval_s = secs;
	cfg.options_interval_s = 0;
}

static void cfg_options_only(uint32_t secs)
{
	cfg_default();
	cfg.crlf_enabled = false;
	cfg.crlf_interval_s = 0;
	cfg.options_interval_s = secs;
}

static void tc_default(void)
{
	sip_timer_cfg_default(&tc_cur);
}

/*
 * The NICT retransmission of an OPTIONS and its transaction timeout are not
 * what the cadence cases measure, and a timer E inside a ping interval would
 * add a datagram to every send count. Both move outside the windows those
 * cases use; the timeout case advances the clock onto timer F on purpose.
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
	sip_ka_done(&ka);
	sip_txn_mgr_done(&mgr);

	memset(&fk, 0, sizeof(fk));
	fk.now = 100000;
	fk.env.send = ka_send_fn;
	fake_clock_bind(&fk.env);
	fk.env.base.trace = fake_trace;
	fk.env.resp = fake_resp;
	fk.env.done = fake_done;

	sip_dst_set(&target, K_HOST, K_PORT);
	dst_valid = true;
	extra_count = 0;
	dst_hole = 0;
	dst_update = false;
	dst_update_secs = 0;
	fail_port = 0;

	sip_txn_mgr_init(&mgr, &fk.env, &tc_cur);
	sip_ka_init(&ka, &mgr, &fk.env, 0x40010001, &cfg, fake_dst_at, NULL);
}

/* --------------------------------------------------------------- messages */

struct req_info {
	char branch[80];
	char call_id[128];
	char from_tag[64];
	char to_tag[64];
	char method[16];
	char ruri[160];
	uint32_t cseq;
	uint32_t max_forwards;
	uint32_t clen;
	bool rport;
};

static bool has(const char *needle)
{
	return strstr(last_text(), needle) != NULL;
}

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
	if (hdr_via_top(&m, &via)) {
		sip_str_copy(ri->branch, sizeof(ri->branch), via.branch);
		ri->rport = (via.flags & SIP_VIA_F_RPORT) != 0;
	}
	if (hdr_call_id_get(&m, &s))
		sip_str_copy(ri->call_id, sizeof(ri->call_id), s);
	if (hdr_addr_get(&m, SIP_HDR_FROM, &a))
		sip_str_copy(ri->from_tag, sizeof(ri->from_tag), a.tag);
	if (hdr_addr_get(&m, SIP_HDR_TO, &a))
		sip_str_copy(ri->to_tag, sizeof(ri->to_tag), a.tag);
	hdr_cseq_get(&m, &ri->cseq, NULL);
	hdr_uint_get(&m, SIP_HDR_MAX_FORWARDS, &ri->max_forwards);
	hdr_uint_get(&m, SIP_HDR_CONTENT_LENGTH, &ri->clen);

	return true;
}

static bool last_req(struct req_info *ri)
{
	return fk.sends ? req_info_at(fk.sends - 1, ri) : false;
}

/* a response to the OPTIONS that went out last */
static void reply(uint32_t code)
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
			       "To: <%s>;tag=far-end-tag\r\n"
			       "Call-ID: %s\r\n"
			       "CSeq: %u OPTIONS\r\n"
			       "Content-Length: 0\r\n\r\n",
			       code, sip_status_reason(code), K_LOCAL_HOST, K_LOCAL_PORT,
			       ri.branch, K_AOR, ri.from_tag, K_AOR, ri.call_id, ri.cseq);

	if (len >= sizeof(text))
		return;

	reply_recv(&mgr, text, len);
}

/* ------------------------------------------------------- CRLF NAT keepalive */

static void crlf_tests(void)
{
	unsigned int i;

	CASE("crlf: nothing goes out before the first full interval");
	tc_default();
	cfg_crlf_only(30);
	fake_reset();
	sip_ka_start(&ka);
	CHECK_UINT(fk.sends, 0);
	fake_advance(29 * 1000);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ka.stat.crlf_sent, 0);

	CASE("crlf: the interval elapses and exactly four bytes leave");
	fake_advance(1000);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(fk.send[0].len, SIP_KA_PING_LEN);
	CHECK(!memcmp(fk.send[0].buf, SIP_KA_PING, SIP_KA_PING_LEN));
	CHECK_UINT(ka.stat.crlf_sent, 1);
	CHECK_UINT(ka.stat.crlf_skipped, 0);
	CHECK_UINT(ka.stat.crlf_failed, 0);

	CASE("crlf: the ping goes to the destination dst_at reports "
	     "at index 0");
	CHECK(!strcmp(fk.send[0].host, K_HOST));
	CHECK_UINT(fk.send[0].port, K_PORT);

	CASE("crlf: the timer re-arms for every interval that follows");
	for (i = 2; i <= 5; i++) {
		fake_advance(30 * 1000);
		CHECK_UINT(fk.sends, i);
	}
	CHECK_UINT(ka.stat.crlf_sent, 5);
	CHECK_UINT(ka.stat.crlf_skipped, 0);
	CHECK_UINT(ka.stat.crlf_failed, 0);

	CASE("crlf: a tick with no destination sends nothing and is counted");
	tc_default();
	cfg_crlf_only(30);
	fake_reset();
	sip_ka_start(&ka);
	dst_valid = false;
	fake_advance(30 * 1000);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ka.stat.crlf_skipped, 1);
	CHECK_UINT(ka.stat.crlf_sent, 0);

	CASE("crlf: the timer still re-armed, so the next tick sends");
	dst_valid = true;
	fake_advance(30 * 1000);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(ka.stat.crlf_sent, 1);
	CHECK_UINT(ka.stat.crlf_skipped, 1);

	CASE("crlf: a transport refusal is counted apart from a skip");
	tc_default();
	cfg_crlf_only(30);
	fake_reset();
	sip_ka_start(&ka);
	fk.send_fail = true;
	fake_advance(30 * 1000);
	CHECK_UINT(ka.stat.crlf_failed, 1);
	CHECK_UINT(ka.stat.crlf_sent, 0);
	CHECK_UINT(ka.stat.crlf_skipped, 0);

	CASE("crlf: a refusal keeps the cadence, so the next tick sends");
	fk.send_fail = false;
	fake_advance(30 * 1000);
	CHECK_UINT(ka.stat.crlf_sent, 1);
	CHECK_UINT(ka.stat.crlf_failed, 1);
	CHECK_UINT(ka.stat.crlf_skipped, 0);

	CASE("crlf: every refused tick is counted, one for one");
	fk.send_fail = true;
	fake_advance(30 * 1000);
	CHECK_UINT(ka.stat.crlf_failed, 2);
	fake_advance(30 * 1000);
	CHECK_UINT(ka.stat.crlf_failed, 3);
	fk.send_fail = false;
	fake_advance(30 * 1000);
	CHECK_UINT(ka.stat.crlf_failed, 3);
	CHECK_UINT(ka.stat.crlf_sent, 2);

	CASE("crlf: a tick with no destination never counts a refusal");
	tc_default();
	cfg_crlf_only(30);
	fake_reset();
	sip_ka_start(&ka);
	dst_valid = false;
	fk.send_fail = true;
	fake_advance(30 * 1000);
	CHECK_UINT(ka.stat.crlf_skipped, 1);
	CHECK_UINT(ka.stat.crlf_failed, 0);
	dst_valid = true;
	fk.send_fail = false;

	CASE("crlf: the flag off arms no timer at all");
	tc_default();
	cfg_crlf_only(30);
	cfg.crlf_enabled = false;
	fake_reset();
	sip_ka_start(&ka);
	CHECK(!ka.t_crlf.armed);
	fake_advance(3600 * 1000);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ka.stat.crlf_sent, 0);
	CHECK_UINT(ka.stat.crlf_skipped, 0);
	CHECK_UINT(ka.stat.crlf_failed, 0);
	CHECK_UINT(fk.dst_calls, 0);

	CASE("crlf: the flag on with interval 0 arms no timer either");
	tc_default();
	cfg_crlf_only(0);
	fake_reset();
	sip_ka_start(&ka);
	CHECK(!ka.t_crlf.armed);
	fake_advance(3600 * 1000);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ka.stat.crlf_sent, 0);

	CASE("crlf: an interval of one second is the smallest one that runs");
	tc_default();
	cfg_crlf_only(1);
	fake_reset();
	sip_ka_start(&ka);
	CHECK_UINT(ka.t_crlf.due_ms - fk.now, 1000);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 1);
}

/* --------------------------------------------------------------- the round */

/*
 * One round pings every destination the callback lists, index 0 first, until
 * the first NULL (sip/keepalive.h). The OPTIONS side reads index 0 alone, and
 * the prime sends the round outside the cadence without moving it.
 */
static void round_tests(void)
{
	CASE("crlf: one ping per resolved target");
	tc_default();
	cfg_crlf_only(30);
	fake_reset();
	extra_set(2);
	sip_ka_start(&ka);
	fake_advance(30 * 1000);
	CHECK_UINT(fk.sends, 3);
	CHECK_UINT(ka.stat.crlf_sent, 3);
	CHECK_UINT(ka.stat.crlf_skipped, 0);
	CHECK_UINT(ka.stat.crlf_failed, 0);
	CHECK_UINT(fk.send[0].port, K_PORT);
	CHECK_UINT(fk.send[1].port, K_PORT + 1);
	CHECK_UINT(fk.send[2].port, K_PORT + 2);
	CHECK_UINT(fk.send[2].len, SIP_KA_PING_LEN);
	CHECK(!memcmp(fk.send[2].buf, SIP_KA_PING, SIP_KA_PING_LEN));
	/* the three destinations and the NULL that ends the list */
	CHECK_UINT(fk.dst_calls, 4);

	CASE("crlf: the list ends at the first NULL");
	tc_default();
	cfg_crlf_only(30);
	fake_reset();
	extra_set(2);
	dst_hole = 1;
	sip_ka_start(&ka);
	fake_advance(30 * 1000);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(ka.stat.crlf_sent, 1);
	CHECK_UINT(ka.stat.crlf_skipped, 0);
	CHECK_UINT(fk.dst_calls, 2);

	CASE("crlf: a tick whose first destination is NULL is one skip");
	tc_default();
	cfg_crlf_only(30);
	fake_reset();
	extra_set(2);
	dst_valid = false;
	sip_ka_start(&ka);
	fake_advance(30 * 1000);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ka.stat.crlf_skipped, 1);
	CHECK_UINT(ka.stat.crlf_sent, 0);
	CHECK_UINT(fk.dst_calls, 1);
	dst_valid = true;

	CASE("crlf: a refused datagram counts one failure and the round "
	     "goes on");
	tc_default();
	cfg_crlf_only(30);
	fake_reset();
	extra_set(2);
	fail_port = K_PORT + 1;
	sip_ka_start(&ka);
	fake_advance(30 * 1000);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(ka.stat.crlf_sent, 2);
	CHECK_UINT(ka.stat.crlf_failed, 1);
	CHECK_UINT(ka.stat.crlf_skipped, 0);
	CHECK_UINT(fk.send[1].port, K_PORT + 2);
	fail_port = 0;

	CASE("options: the ping goes to the sticky target only");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	extra_set(2);
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(ka.stat.options_sent, 1);
	CHECK_UINT(fk.send[0].port, K_PORT);
	CHECK_UINT(fk.dst_calls, 1);

	CASE("ping now: sends the round and re-arms a full interval");
	tc_default();
	cfg_crlf_only(30);
	fake_reset();
	extra_set(1);
	sip_ka_start(&ka);
	fake_advance(10 * 1000);
	sip_ka_ping_now(&ka);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(ka.stat.crlf_sent, 2);
	CHECK_UINT(ka.t_crlf.due_ms - fk.now, 30000);
	fake_advance(29 * 1000);
	CHECK_UINT(fk.sends, 2);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 4);

	CASE("ping now: a stopped instance sends nothing");
	sip_ka_stop(&ka);
	sip_ka_ping_now(&ka);
	CHECK_UINT(fk.sends, 4);
	CHECK(!ka.t_crlf.armed);

	CASE("ping now: a disabled CRLF side sends nothing either");
	tc_default();
	cfg_crlf_only(30);
	cfg.crlf_enabled = false;
	fake_reset();
	sip_ka_start(&ka);
	sip_ka_ping_now(&ka);
	CHECK_UINT(fk.sends, 0);
	CHECK(!ka.t_crlf.armed);
	CHECK_UINT(fk.dst_calls, 0);
}

/* ------------------------------------------------------- OPTIONS wire shape */

static void options_wire_tests(void)
{
	struct req_info ri, ri2;

	CASE("options: nothing goes out before the first full interval");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	CHECK_UINT(fk.sends, 0);
	fake_advance(59 * 1000);
	CHECK_UINT(fk.sends, 0);

	CASE("options: the interval elapses and one OPTIONS leaves");
	fake_advance(1000);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(ka.stat.options_sent, 1);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "OPTIONS"));

	CASE("options: the request URI is the configured registrar");
	CHECK(!strcmp(ri.ruri, K_REGISTRAR));
	CHECK(has("OPTIONS " K_REGISTRAR " SIP/2.0"));

	CASE("options: the To carries no tag and the From carries one");
	CHECK_UINT(strlen(ri.to_tag), 0);
	CHECK(ri.from_tag[0] != 0);
	CHECK(has("To: <" K_AOR ">"));
	CHECK(!has("To: <" K_AOR ">;tag="));

	CASE("options: Max-Forwards, Accept and an empty body");
	CHECK_UINT(ri.max_forwards, 70);
	CHECK_UINT(ri.clen, 0);
	CHECK(has("Max-Forwards: 70"));
	CHECK(has("Accept: application/sdp"));
	CHECK(has("Content-Length: 0"));

	CASE("options: the Via names the local sent-by and always asks for rport");
	CHECK(ri.rport);
	CHECK(has("Via: SIP/2.0/UDP " K_LOCAL_HOST ":5060;branch=z9hG4bK"));
	CHECK(has(";rport"));

	CASE("options: the User-Agent is written when the config carries one");
	CHECK(has("User-Agent: uvoice-test/1"));

	CASE("options: the ping goes to the destination dst_at reports "
	     "at index 0");
	CHECK(!strcmp(fk.send[0].host, K_HOST));
	CHECK_UINT(fk.send[0].port, K_PORT);

	CASE("options: the CSeq starts at one and names the method");
	CHECK_UINT(ri.cseq, 1);
	CHECK(has("CSeq: 1 OPTIONS"));

	CASE("options: the next ping is a new dialogue, not a second leg of this one");
	reply(200);
	fake_advance(60 * 1000);
	CHECK_UINT(fk.sends, 2);
	CHECK(last_req(&ri2));
	CHECK(strcmp(ri2.call_id, ri.call_id) != 0);
	CHECK(strcmp(ri2.from_tag, ri.from_tag) != 0);
	CHECK(strcmp(ri2.branch, ri2.call_id) != 0);
	CHECK(strcmp(ri2.branch, ri.branch) != 0);

	CASE("options: the CSeq only ever grows");
	CHECK_UINT(ri2.cseq, 2);
	reply(200);
	fake_advance(60 * 1000);
	CHECK(last_req(&ri));
	CHECK_UINT(ri.cseq, 3);

	/*
	 * The ping measures the next hop and the NAT binding toward it
	 * (sip/keepalive.h), so no route of any kind rides it. There is no
	 * configuration field to carry one: the registration keeps the service
	 * route of RFC 3608 and never hands it here.
	 */
	CASE("options: the ping carries no Route header, whatever the "
	     "registrar routed");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK_UINT(fk.sends, 1);
	CHECK(!has("\r\nRoute:"));
	CHECK(!has("Service-Route"));

	CASE("options: a config without a user agent writes no header");
	tc_quiet();
	cfg_options_only(60);
	cfg.user_agent = NULL;
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK_UINT(fk.sends, 1);
	CHECK(!has("User-Agent:"));

	CASE("options: a display name reaches the From");
	tc_quiet();
	cfg_options_only(60);
	cfg.display = "Front Desk";
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK(has("From: \"Front Desk\" <" K_AOR ">;tag="));

	CASE("options: an interval of 0 arms no timer at all");
	tc_quiet();
	cfg_options_only(0);
	fake_reset();
	sip_ka_start(&ka);
	CHECK(!ka.t_options.armed);
	fake_advance(3600 * 1000);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ka.stat.options_sent, 0);
	CHECK_UINT(fk.dst_calls, 0);

	CASE("options: a config with no address of record never arms the ping");
	tc_quiet();
	cfg_options_only(60);
	cfg.aor = NULL;
	fake_reset();
	sip_ka_start(&ka);
	CHECK(!ka.t_options.armed);
	fake_advance(3600 * 1000);
	CHECK_UINT(fk.sends, 0);

	CASE("options: nor one without a registrar or a local host");
	tc_quiet();
	cfg_options_only(60);
	cfg.registrar = NULL;
	fake_reset();
	sip_ka_start(&ka);
	CHECK(!ka.t_options.armed);
	cfg_options_only(60);
	cfg.local_host = NULL;
	fake_reset();
	sip_ka_start(&ka);
	CHECK(!ka.t_options.armed);
}

/* ------------------------------------------------------ the transport token */

/*
 * The Via carries the token after "SIP/2.0/" (sip/keepalive.h). The OPTIONS
 * ping carries no Contact, so this side has no URI parameter to write.
 */
static void transport_tests(void)
{
	CASE("transport: a NULL token keeps the UDP Via the ping always had");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK_UINT(fk.sends, 1);
	CHECK(has("Via: SIP/2.0/UDP " K_LOCAL_HOST ":5060;branch=z9hG4bK"));
	CHECK(!has(";transport="));

	CASE("transport: TCP reaches the Via of the OPTIONS");
	tc_quiet();
	cfg_options_only(60);
	cfg.transport = "TCP";
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK_UINT(fk.sends, 1);
	CHECK(has("Via: SIP/2.0/TCP " K_LOCAL_HOST ":5060;branch=z9hG4bK"));
	CHECK(has(";rport"));

	CASE("transport: the ping still carries no Contact at all");
	CHECK(!has("Contact:"));
	CHECK(!has(";transport="));

	CASE("transport: TLS is one more token");
	tc_quiet();
	cfg_options_only(60);
	cfg.transport = "TLS";
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK(has("Via: SIP/2.0/TLS " K_LOCAL_HOST ":5060;branch=z9hG4bK"));
	CHECK(!has("Contact:"));
	cfg_default();
}

/* ------------------------------------------------------- response policy */

static void options_policy_tests(void)
{
	CASE("policy: a 200 proves the path alive");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	reply(200);
	CHECK_UINT(ka.stat.options_alive, 1);
	CHECK_UINT(ka.stat.options_failed, 0);
	CHECK_UINT(ka.stat.options_skipped, 0);

	CASE("policy: a 404 proves it too, because the answer travelled");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	reply(404);
	CHECK_UINT(ka.stat.options_alive, 1);
	CHECK_UINT(ka.stat.options_failed, 0);

	CASE("policy: a 401 counts alive and is NEVER answered with credentials");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK_UINT(fk.sends, 1);
	reply(401);
	CHECK_UINT(ka.stat.options_alive, 1);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(ka.stat.options_sent, 1);
	CHECK_UINT(ka.stat.options_failed, 0);

	CASE("policy: a 407 is the same answer");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	reply(407);
	CHECK_UINT(ka.stat.options_alive, 1);
	CHECK_UINT(fk.sends, 1);

	CASE("policy: a 503 is alive as well, no failover of any kind follows");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	reply(503);
	CHECK_UINT(ka.stat.options_alive, 1);
	CHECK_UINT(ka.stat.options_failed, 0);
	CHECK_UINT(fk.sends, 1);

	CASE("policy: a provisional is ignored, only the final counts");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	reply(100);
	CHECK_UINT(ka.stat.options_alive, 0);
	reply(180);
	CHECK_UINT(ka.stat.options_alive, 0);
	reply(200);
	CHECK_UINT(ka.stat.options_alive, 1);

	CASE("policy: a retransmitted final is absorbed and counts once");
	reply(200);
	CHECK_UINT(ka.stat.options_alive, 1);

	CASE("policy: the transaction ends with OK and counts nothing");
	fake_advance(5 * 1000);
	CHECK_UINT(ka.txn, SIP_HANDLE_NONE);
	CHECK_UINT(ka.stat.options_failed, 0);
	CHECK_UINT(ka.stat.options_alive, 1);

	CASE("policy: a response to a transaction this instance does not own");
	sip_ka_txn_resp(&ka, 0x00ff00ff, &reply_stored);
	CHECK_UINT(ka.stat.options_alive, 1);

	CASE("policy: a done for a transaction it does not own");
	sip_ka_txn_done(&ka, 0x00ff00ff, SIP_TXN_DONE_TIMEOUT);
	CHECK_UINT(ka.stat.options_failed, 0);
}

/* --------------------------------------------------------- failure paths */

static void options_failure_tests(void)
{
	static char huge[SIP_KA_WIRE_MAX * 2];

	CASE("failure: a timeout counts failed and clears the handle");
	tc_quiet();
	cfg_options_only(600);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(600 * 1000);
	CHECK_UINT(ka.stat.options_sent, 1);
	CHECK(ka.txn != SIP_HANDLE_NONE);
	fake_advance(300 * 1000);
	CHECK_UINT(ka.stat.options_failed, 1);
	CHECK_UINT(ka.stat.options_alive, 0);
	CHECK_UINT(ka.txn, SIP_HANDLE_NONE);
	CHECK_UINT(fk.sends, 1);

	CASE("failure: the next tick sends again");
	fake_advance(300 * 1000);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(ka.stat.options_sent, 2);
	CHECK_UINT(ka.stat.options_skipped, 0);

	CASE("failure: a transport refusal counts failed and starts no transaction");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fk.send_fail = true;
	fake_advance(60 * 1000);
	CHECK_UINT(ka.stat.options_failed, 1);
	CHECK_UINT(ka.stat.options_sent, 0);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ka.txn, SIP_HANDLE_NONE);

	CASE("failure: the tick after it sends normally");
	fk.send_fail = false;
	fake_advance(60 * 1000);
	CHECK_UINT(ka.stat.options_sent, 1);
	CHECK_UINT(ka.stat.options_failed, 1);

	CASE("failure: a request that does not fit the buffer counts failed");
	memset(huge, 'x', sizeof(huge) - 1);
	huge[sizeof(huge) - 1] = 0;
	memcpy(huge, "sip:", 4);
	tc_quiet();
	cfg_options_only(60);
	cfg.registrar = huge;
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK_UINT(ka.stat.options_failed, 1);
	CHECK_UINT(ka.stat.options_sent, 0);
	CHECK_UINT(fk.sends, 0);

	CASE("failure: a tick with no destination is skipped and counted");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	dst_valid = false;
	fake_advance(60 * 1000);
	CHECK_UINT(ka.stat.options_skipped, 1);
	CHECK_UINT(ka.stat.options_sent, 0);
	CHECK_UINT(ka.stat.options_failed, 0);
	CHECK_UINT(fk.sends, 0);

	CASE("failure: the timer re-armed, so a destination later sends");
	dst_valid = true;
	fake_advance(60 * 1000);
	CHECK_UINT(ka.stat.options_sent, 1);
	CHECK_UINT(ka.stat.options_skipped, 1);

	CASE("in flight: a tick while the previous ping lives is skipped");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK_UINT(ka.stat.options_sent, 1);
	fake_advance(60 * 1000);
	CHECK_UINT(ka.stat.options_skipped, 1);
	CHECK_UINT(fk.sends, 1);
	fake_advance(60 * 1000);
	CHECK_UINT(ka.stat.options_skipped, 2);
	CHECK_UINT(ka.stat.options_sent, 1);
	CHECK_UINT(fk.sends, 1);

	CASE("in flight: the answer frees the slot and the next tick pings again");
	reply(200);
	CHECK_UINT(ka.stat.options_alive, 1);
	fake_advance(60 * 1000);
	CHECK_UINT(ka.stat.options_sent, 2);
	CHECK_UINT(ka.stat.options_skipped, 2);
	CHECK_UINT(fk.sends, 2);
}

/* ------------------------------------------------------------ start, stop */

static void lifetime_tests(void)
{
	CASE("start: a second start keeps the cadence of the first");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(20 * 1000);
	sip_ka_start(&ka);
	fake_advance(10 * 1000);
	CHECK_UINT(ka.stat.crlf_sent, 1);

	CASE("stop: both timers are cancelled");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_ka_start(&ka);
	CHECK(ka.t_crlf.armed);
	CHECK(ka.t_options.armed);
	sip_ka_stop(&ka);
	CHECK(!ka.t_crlf.armed);
	CHECK(!ka.t_options.armed);
	fake_advance(3600 * 1000);
	CHECK_UINT(fk.sends, 0);

	CASE("stop: an OPTIONS in flight is aborted and counts nothing");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK(ka.txn != SIP_HANDLE_NONE);
	sip_ka_stop(&ka);
	CHECK_UINT(ka.txn, SIP_HANDLE_NONE);
	CHECK_UINT(ka.stat.options_failed, 0);
	CHECK_UINT(ka.stat.options_alive, 0);
	CHECK_UINT(sip_txn_count(&mgr), 0);

	CASE("stop: nothing runs after it");
	fake_advance(3600 * 1000);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(ka.stat.options_sent, 1);

	CASE("stop: a stopped instance starts again from a full interval");
	sip_ka_start(&ka);
	fake_advance(59 * 1000);
	CHECK_UINT(fk.sends, 1);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 2);

	CASE("stop: a stop of an instance that never ran is quiet");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_ka_stop(&ka);
	CHECK(!ka.t_crlf.armed);
	CHECK(!ka.t_options.armed);
	CHECK_UINT(fk.sends, 0);

	CASE("done: every timer stops and the transaction detaches");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	sip_ka_done(&ka);
	CHECK_UINT(ka.txn, SIP_HANDLE_NONE);
	fake_advance(7200 * 1000);
	CHECK_UINT(ka.stat.options_sent, 1);
	CHECK_UINT(ka.stat.crlf_sent, 2);

	CASE("timers: both sides carry their own timer id and name");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_ka_start(&ka);
	CHECK_UINT(ka.t_crlf.id, SIP_TIMER_KA_CRLF);
	CHECK_UINT(ka.t_options.id, SIP_TIMER_KA_OPTIONS);
	CHECK(!strcmp(sip_timer_name(SIP_TIMER_KA_CRLF), "ka-crlf"));
	CHECK(!strcmp(sip_timer_name(SIP_TIMER_KA_OPTIONS), "ka-options"));

	CASE("no timer was lost during the suite");
	CHECK_UINT(fake_timer_dropped, 0);
}

/* ---------------------------------------------------------- cfg_update */

static void cfg_update_tests(void)
{
	CASE("update: a new interval keeps the time already waited");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(30 * 1000);
	cfg.options_interval_s = 45;
	sip_ka_cfg_update(&ka, &cfg);
	CHECK_UINT(ka.t_options.due_ms - fk.now, 15000);
	fake_advance(14 * 1000);
	CHECK_UINT(fk.sends, 0);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(ka.stat.options_sent, 1);

	CASE("update: a newly disabled OPTIONS side is cancelled");
	cfg.options_interval_s = 0;
	sip_ka_cfg_update(&ka, &cfg);
	CHECK(!ka.t_options.armed);
	fake_advance(3600 * 1000);
	CHECK_UINT(ka.stat.options_sent, 1);

	CASE("update: a newly enabled side starts a full fresh interval");
	cfg.options_interval_s = 20;
	sip_ka_cfg_update(&ka, &cfg);
	CHECK(ka.t_options.armed);
	fake_advance(19 * 1000);
	CHECK_UINT(ka.stat.options_sent, 1);
	fake_advance(1000);
	CHECK_UINT(ka.stat.options_sent, 2);

	CASE("update: a disable while a ping is in flight aborts the ping");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK_UINT(ka.stat.options_sent, 1);
	CHECK(ka.txn != SIP_HANDLE_NONE);
	cfg.options_interval_s = 0;
	sip_ka_cfg_update(&ka, &cfg);
	CHECK_UINT(ka.txn, SIP_HANDLE_NONE);
	CHECK_UINT(sip_txn_count(&mgr), 0);
	CHECK_UINT(ka.stat.options_failed, 0);
	CHECK_UINT(ka.stat.options_alive, 0);

	CASE("update: the aborted ping never times out afterwards");
	fake_advance(3600 * 1000);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(ka.stat.options_failed, 0);
	CHECK_UINT(ka.stat.options_sent, 1);

	CASE("update: a re-enable after the abort pings fresh");
	cfg.options_interval_s = 20;
	sip_ka_cfg_update(&ka, &cfg);
	CHECK(ka.t_options.armed);
	fake_advance(19 * 1000);
	CHECK_UINT(ka.stat.options_sent, 1);
	fake_advance(1000);
	CHECK_UINT(ka.stat.options_sent, 2);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(ka.stat.options_failed, 0);

	/*
	 * The retransmission timers of a NICT belong to the transaction, not to
	 * the cadence timer, so a disable that only cancelled the tick would
	 * still put datagrams on the wire for another 32 seconds. The default
	 * timer set is used here on purpose: timer E fires inside the window.
	 */
	CASE("update: a disable stops the retransmissions the ping still owed");
	tc_default();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK_UINT(fk.sends, 1);
	cfg.options_interval_s = 0;
	sip_ka_cfg_update(&ka, &cfg);
	fake_advance(60 * 1000);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(ka.stat.options_failed, 0);
	CHECK_UINT(ka.stat.options_sent, 1);

	CASE("update: a config that can no longer build a ping aborts it too");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK(ka.txn != SIP_HANDLE_NONE);
	cfg.aor = NULL;
	sip_ka_cfg_update(&ka, &cfg);
	CHECK(!ka.t_options.armed);
	CHECK_UINT(ka.txn, SIP_HANDLE_NONE);
	CHECK_UINT(sip_txn_count(&mgr), 0);
	CHECK_UINT(ka.stat.options_failed, 0);
	cfg_default();

	CASE("update: an enabled side keeps the ping it already has in flight");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK(ka.txn != SIP_HANDLE_NONE);
	cfg.options_interval_s = 30;
	sip_ka_cfg_update(&ka, &cfg);
	CHECK(ka.txn != SIP_HANDLE_NONE);
	CHECK_UINT(sip_txn_count(&mgr), 1);
	reply(200);
	CHECK_UINT(ka.stat.options_alive, 1);

	CASE("update: the CRLF side follows the same rule");
	tc_quiet();
	cfg_crlf_only(30);
	fake_reset();
	sip_ka_start(&ka);
	cfg.crlf_interval_s = 5;
	sip_ka_cfg_update(&ka, &cfg);
	fake_advance(5 * 1000);
	CHECK_UINT(ka.stat.crlf_sent, 1);
	cfg.crlf_enabled = false;
	sip_ka_cfg_update(&ka, &cfg);
	CHECK(!ka.t_crlf.armed);
	fake_advance(3600 * 1000);
	CHECK_UINT(ka.stat.crlf_sent, 1);
	cfg.crlf_enabled = true;
	sip_ka_cfg_update(&ka, &cfg);
	fake_advance(5 * 1000);
	CHECK_UINT(ka.stat.crlf_sent, 2);

	CASE("update: the new strings reach the next request");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	cfg.registrar = "sip:other.example.net";
	sip_ka_cfg_update(&ka, &cfg);
	fake_advance(60 * 1000);
	CHECK(has("OPTIONS sip:other.example.net SIP/2.0"));

	CASE("update: an update before the start arms nothing");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_ka_cfg_update(&ka, &cfg);
	CHECK(!ka.t_crlf.armed);
	CHECK(!ka.t_options.armed);
	fake_advance(3600 * 1000);
	CHECK_UINT(fk.sends, 0);

	CASE("update: an update after a stop arms nothing either");
	tc_quiet();
	cfg_default();
	fake_reset();
	sip_ka_start(&ka);
	sip_ka_stop(&ka);
	sip_ka_cfg_update(&ka, &cfg);
	CHECK(!ka.t_crlf.armed);
	CHECK(!ka.t_options.armed);

	/*
	 * The elapsed time survives an update (sip/keepalive.h): a reload burst
	 * that re-armed from now on every update pushed the next ping out for
	 * as long as the burst lasted, and a conntrack pinhole died under it.
	 */
	CASE("cfg update: keeps the elapsed time");
	tc_quiet();
	cfg_crlf_only(30);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(10 * 1000);
	cfg.crlf_interval_s = 20;
	sip_ka_cfg_update(&ka, &cfg);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ka.t_crlf.due_ms - fk.now, 10000);
	fake_advance(9 * 1000);
	CHECK_UINT(fk.sends, 0);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 1);

	CASE("cfg update: a repeated update does not push the ping out");
	fake_advance(5 * 1000);
	sip_ka_cfg_update(&ka, &cfg);
	fake_advance(5 * 1000);
	sip_ka_cfg_update(&ka, &cfg);
	CHECK_UINT(ka.t_crlf.due_ms - fk.now, 10000);
	fake_advance(10 * 1000);
	CHECK_UINT(fk.sends, 2);

	CASE("cfg update: an elapsed interval fires now and re-arms a full "
	     "one");
	tc_quiet();
	cfg_crlf_only(30);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(25 * 1000);
	cfg.crlf_interval_s = 10;
	sip_ka_cfg_update(&ka, &cfg);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(ka.stat.crlf_sent, 1);
	CHECK_UINT(ka.t_crlf.due_ms - fk.now, 10000);
	fake_advance(9 * 1000);
	CHECK_UINT(fk.sends, 1);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 2);

	CASE("cfg update: a side turned on starts a fresh full interval");
	tc_quiet();
	cfg_options_only(60);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(40 * 1000);
	cfg.crlf_enabled = true;
	cfg.crlf_interval_s = 30;
	sip_ka_cfg_update(&ka, &cfg);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(ka.t_crlf.due_ms - fk.now, 30000);
	/* the OPTIONS side kept its own 40 s, so it fires 20 s from here */
	CHECK_UINT(ka.t_options.due_ms - fk.now, 20000);
	fake_advance(30 * 1000);
	CHECK_UINT(ka.stat.crlf_sent, 1);
	CHECK_UINT(ka.stat.options_sent, 1);

	/*
	 * The account updates the config from inside the round when the
	 * conntrack bound moved under it. The side that is sending is left to
	 * its own re-arm, which reads the new interval, and no second round
	 * runs from the update.
	 */
	CASE("cfg update: safe from inside the destination callback");
	tc_quiet();
	cfg_crlf_only(30);
	fake_reset();
	extra_set(1);
	sip_ka_start(&ka);
	cfg.crlf_interval_s = 5;
	dst_update = true;
	fake_advance(30 * 1000);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(ka.stat.crlf_sent, 2);
	CHECK(!dst_update);
	CHECK_UINT(ka.t_crlf.due_ms - fk.now, 5000);
	fake_advance(5 * 1000);
	CHECK_UINT(fk.sends, 4);

	/*
	 * The round an update fires itself can re-enter the update from the
	 * callback, the account's bound re-read. The arm after that round reads
	 * the interval the callback set, not the one the outer update started
	 * with.
	 */
	CASE("cfg update: a round it fires re-arms from the interval the "
	     "callback set");
	tc_quiet();
	cfg_crlf_only(30);
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(25 * 1000);
	cfg.crlf_interval_s = 10;
	dst_update = true;
	dst_update_secs = 5;
	sip_ka_cfg_update(&ka, &cfg);
	CHECK_UINT(fk.sends, 1);
	CHECK(!dst_update);
	CHECK_UINT(ka.t_crlf.due_ms - fk.now, 5000);
	fake_advance(5 * 1000);
	CHECK_UINT(fk.sends, 2);
	dst_update_secs = 0;
}

/* --------------------------------------------------------------- clamps */

static void clamp_tests(void)
{
	CASE("clamp: an interval above the maximum arms at the maximum");
	tc_quiet();
	cfg_default();
	cfg.crlf_interval_s = 100000;
	cfg.options_interval_s = 4294967;
	fake_reset();
	sip_ka_start(&ka);
	CHECK_UINT(ka.t_crlf.due_ms - fk.now, (uint64_t)SIP_KA_INTERVAL_MAX * 1000);
	CHECK_UINT(ka.t_options.due_ms - fk.now, (uint64_t)SIP_KA_INTERVAL_MAX * 1000);

	CASE("clamp: and it fires there instead of wrapping the millisecond timer");
	fake_advance((uint64_t)(SIP_KA_INTERVAL_MAX - 1) * 1000);
	CHECK_UINT(ka.stat.crlf_sent, 0);
	CHECK_UINT(ka.stat.options_sent, 0);
	fake_advance(1000);
	CHECK_UINT(ka.stat.crlf_sent, 1);
	CHECK_UINT(ka.stat.options_sent, 1);

	CASE("clamp: the maximum itself is not clamped away");
	tc_quiet();
	cfg_crlf_only(SIP_KA_INTERVAL_MAX);
	fake_reset();
	sip_ka_start(&ka);
	CHECK_UINT(ka.t_crlf.due_ms - fk.now, (uint64_t)SIP_KA_INTERVAL_MAX * 1000);

	CASE("clamp: the minimum is not clamped away either");
	tc_quiet();
	cfg_crlf_only(SIP_KA_INTERVAL_MIN);
	fake_reset();
	sip_ka_start(&ka);
	CHECK_UINT(ka.t_crlf.due_ms - fk.now, (uint64_t)SIP_KA_INTERVAL_MIN * 1000);

	CASE("clamp: an update also clamps before the conversion");
	tc_quiet();
	cfg_crlf_only(30);
	fake_reset();
	sip_ka_start(&ka);
	cfg.crlf_interval_s = 4000000;
	sip_ka_cfg_update(&ka, &cfg);
	CHECK_UINT(ka.t_crlf.due_ms - fk.now, (uint64_t)SIP_KA_INTERVAL_MAX * 1000);
}

/* ------------------------------------------------------------- entropy */

/*
 * A defect demonstration, not a requirement. The fallback generator of the
 * transaction layer seeds from the millisecond clock. Two managers created at
 * one instant with no rand32 hook therefore draw the same first branch, and
 * the same Call-ID and From tag streams. uvoiced does not take that path:
 * account_build() installs an OS entropy hook on every account environment.
 * This case checks the fallback that the unit suites rely on, so a change to
 * it shows here and not in a shared-socket routing bug.
 */
static void entropy_tests(void)
{
	struct sip_txn_mgr m1, m2;
	char b1[SIP_BRANCH_MAX], b2[SIP_BRANCH_MAX];
	uint32_t v1, v2;

	CASE("entropy: two managers on one clock with no rand32 share a branch");
	tc_default();
	fake_reset();
	CHECK(fk.env.base.rand32 == NULL);
	sip_txn_mgr_init(&m1, &fk.env, &tc_cur);
	sip_txn_mgr_init(&m2, &fk.env, &tc_cur);
	CHECK(sip_branch_new(&m1, b1, sizeof(b1)));
	CHECK(sip_branch_new(&m2, b2, sizeof(b2)));
	CHECK(!strcmp(b1, b2));

	CASE("entropy: their identifier streams stay in step after that");
	v1 = sip_txn_rand(&m1);
	v2 = sip_txn_rand(&m2);
	CHECK_UINT(v1, v2);
	sip_txn_mgr_done(&m1);
	sip_txn_mgr_done(&m2);

	CASE("entropy: a rand32 hook on the environment is what breaks the tie");
	fake_reset();
	fk.env.base.rand32 = fake_rand32;
	sip_txn_mgr_init(&m1, &fk.env, &tc_cur);
	sip_txn_mgr_init(&m2, &fk.env, &tc_cur);
	CHECK(sip_branch_new(&m1, b1, sizeof(b1)));
	CHECK(sip_branch_new(&m2, b2, sizeof(b2)));
	CHECK(strcmp(b1, b2) != 0);
	sip_txn_mgr_done(&m1);
	sip_txn_mgr_done(&m2);
	fk.env.base.rand32 = NULL;
}

/* ------------------------------------------------------------ both sides */

static void combined_tests(void)
{
	CASE("both: the two sides run on their own intervals");
	tc_quiet();
	cfg_default();
	cfg.crlf_interval_s = 30;
	cfg.options_interval_s = 45;
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(30 * 1000);
	CHECK_UINT(ka.stat.crlf_sent, 1);
	CHECK_UINT(ka.stat.options_sent, 0);
	fake_advance(15 * 1000);
	CHECK_UINT(ka.stat.crlf_sent, 1);
	CHECK_UINT(ka.stat.options_sent, 1);
	fake_advance(15 * 1000);
	CHECK_UINT(ka.stat.crlf_sent, 2);
	CHECK_UINT(ka.stat.options_sent, 1);

	CASE("both: only the CRLF side puts a four byte datagram on the wire");
	CHECK_UINT(fk.sends, 3);
	CHECK_UINT(fk.send[0].len, SIP_KA_PING_LEN);
	CHECK(fk.send[1].len > SIP_KA_PING_LEN);
	CHECK_UINT(fk.send[2].len, SIP_KA_PING_LEN);

	CASE("both: one side disabled leaves the other alone");
	tc_quiet();
	cfg_default();
	cfg.crlf_enabled = false;
	fake_reset();
	sip_ka_start(&ka);
	fake_advance(60 * 1000);
	CHECK_UINT(ka.stat.crlf_sent, 0);
	CHECK_UINT(ka.stat.options_sent, 1);

	CASE("both: the pong is two bytes and the ping four");
	CHECK_UINT(SIP_KA_PING_LEN, strlen(SIP_KA_PING));
	CHECK_UINT(SIP_KA_PONG_LEN, strlen(SIP_KA_PONG));
	CHECK(!memcmp(SIP_KA_PING, "\r\n\r\n", SIP_KA_PING_LEN));
	CHECK(!memcmp(SIP_KA_PONG, "\r\n", SIP_KA_PONG_LEN));
}

int main(void)
{
	cfg_default();
	tc_default();
	sip_txn_mgr_init(&mgr, &fk.env, &tc_cur);
	sip_ka_init(&ka, &mgr, &fk.env, 0x40010001, &cfg, fake_dst_at, NULL);

	crlf_tests();
	round_tests();
	options_wire_tests();
	transport_tests();
	options_policy_tests();
	options_failure_tests();
	lifetime_tests();
	cfg_update_tests();
	clamp_tests();
	entropy_tests();
	combined_tests();

	sip_ka_done(&ka);
	sip_txn_mgr_done(&mgr);

	return tu_summary("test_sip_keepalive");
}
