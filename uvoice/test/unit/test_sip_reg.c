// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the registration client (src/proto/sip/reg.c).
 *
 * The suite reuses the shape of the scripted environment of test_sip_txn.c. The
 * test advances a virtual clock by hand, a capture array replaces the socket,
 * and a timer table replaces uloop. The registration runs on the real
 * transaction layer. A refresh, a retransmission and a timeout are therefore
 * observable to the millisecond without a sleep.
 *
 * The account is fictitious. No provider credential appears in this file.
 */
#include <stdlib.h>

#include "proto/sip/reg.h"
#include "test_util.h"

#define FAKE_TIMER_MAX	256
#define FAKE_SEND_MAX	64
#define WIRE_MAX	4096
#define TEXT_MAX	4096

#define R_USER		"uvoice-test"
#define R_PASS		"not-a-real-secret"
#define R_REALM		"sip.example.com"
#define R_AOR		"sip:uvoice-test@sip.example.com"
#define R_REGISTRAR	"sip:sip.example.com"
#define R_NONCE		"1f3870be274f6c49b3e31a0c6728957f"
#define R_OPAQUE	"9a8b7c6d5e4f"
#define R_LOCAL_HOST	"10.0.0.5"
#define R_LOCAL_PORT	5060

/* the RFC 2069 answer for this account, nonce and request URI */
#define R_RESPONSE	"a3a8dd76559ceaf5a361177f6a87a08f"

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
	char trace[TEXT_MAX];

	unsigned int registered;
	unsigned int unregistered;
	unsigned int failed;
	unsigned int rolled_back;
	bool stop_in_cb;
	uint32_t last_expires;
	uint32_t last_code;
	uint32_t last_retry;
} fk;

static struct sip_txn_mgr mgr;
static struct sip_reg reg;

#include "test_capture.h"
#include "test_clock.h"

/* ------------------------------------------------------------- environment */

static void fake_trace(struct proto_timer_env *env, const char *msg)
{
	size_t len = strlen(fk.trace);

	snprintf(fk.trace + len, sizeof(fk.trace) - len, "%s\n", msg);
}

static void fake_resp(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
		      const struct sip_msg *msg)
{
	sip_reg_txn_resp(&reg, txn, msg);
}

static void fake_done(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
		      enum sip_txn_reason reason)
{
	sip_reg_txn_done(&reg, txn, reason);
}

static void cb_registered(struct sip_reg *r, uint32_t expires_s, void *priv)
{
	fk.registered++;
	fk.last_expires = expires_s;
}

static void cb_unregistered(struct sip_reg *r, void *priv)
{
	fk.unregistered++;

	/* An owner is allowed to stop the account from inside this callback.
	 * Without the guard the pair recurses until the stack ends. */
	if (fk.stop_in_cb)
		sip_reg_stop(r, true);
}

static void cb_failed(struct sip_reg *r, uint32_t code, uint32_t retry_s, void *priv)
{
	fk.failed++;
	fk.last_code = code;
	fk.last_retry = retry_s;
}

static void cb_rolled_back(struct sip_reg *r, void *priv)
{
	fk.rolled_back++;
}

static const struct sip_reg_cb reg_cb = {
	.registered = cb_registered,
	.unregistered = cb_unregistered,
	.failed = cb_failed,
	.rolled_back = cb_rolled_back,
};

static struct sip_reg_cfg cfg;

static void cfg_default(void)
{
	memset(&cfg, 0, sizeof(cfg));
	cfg.aor = R_AOR;
	cfg.registrar = R_REGISTRAR;
	cfg.contact_user = R_USER;
	cfg.auth_user = R_USER;
	cfg.auth_pass = R_PASS;
	cfg.local_host = R_LOCAL_HOST;
	cfg.local_port = R_LOCAL_PORT;
	cfg.expires = 3600;
	cfg.retry_s = 5;
	cfg.penalty_s = 60;
	cfg.user_agent = "uvoice-test/1";
}

static void fake_reset(void)
{
	struct sip_timer_cfg tc;

	sip_reg_done(&reg);
	sip_txn_mgr_done(&mgr);

	memset(&fk, 0, sizeof(fk));
	fk.now = 100000;
	fk.env.send = fake_send_fn;
	fake_clock_bind(&fk.env);
	fk.env.base.trace = fake_trace;
	fk.env.resp = fake_resp;
	fk.env.done = fake_done;

	sip_timer_cfg_default(&tc);
	sip_txn_mgr_init(&mgr, &fk.env, &tc);
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
}

/* --------------------------------------------------------------- messages */

struct req_info {
	char branch[80];
	char call_id[128];
	char from_tag[64];
	char contact[160];
	char method[16];
	uint32_t cseq;
	uint32_t expires;
	bool has_expires;
	bool auth;
	bool proxy_auth;
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
	if (hdr_via_top(&m, &via))
		sip_str_copy(ri->branch, sizeof(ri->branch), via.branch);
	if (hdr_call_id_get(&m, &s))
		sip_str_copy(ri->call_id, sizeof(ri->call_id), s);
	if (hdr_addr_get(&m, SIP_HDR_FROM, &a))
		sip_str_copy(ri->from_tag, sizeof(ri->from_tag), a.tag);
	if (hdr_addr_get(&m, SIP_HDR_CONTACT, &a))
		sip_str_copy(ri->contact, sizeof(ri->contact), a.uri.raw);
	hdr_cseq_get(&m, &ri->cseq, NULL);
	ri->has_expires = hdr_uint_get(&m, SIP_HDR_EXPIRES, &ri->expires);
	ri->auth = msg_hdr_first(&m, SIP_HDR_AUTHORIZATION) != NULL;
	ri->proxy_auth = msg_hdr_first(&m, SIP_HDR_PROXY_AUTHORIZATION) != NULL;

	return true;
}

static bool last_req(struct req_info *ri)
{
	return fk.sends ? req_info_at(fk.sends - 1, ri) : false;
}

/*
 * A response to the REGISTER that went out last. The Via is rebuilt rather
 * than copied, so a test can add received and rport without the ;rport of the
 * request getting in the way.
 */
static void reply(uint32_t code, const char *via_params, const char *extra)
{
	struct req_info ri;
	char text[TEXT_MAX];
	size_t len;

	if (!last_req(&ri))
		return;

	len = (size_t)snprintf(text, sizeof(text),
			       "SIP/2.0 %u %s\r\n"
			       "Via: SIP/2.0/UDP %s:%u;branch=%s%s\r\n"
			       "From: <%s>;tag=%s\r\n"
			       "To: <%s>;tag=registrar-tag\r\n"
			       "Call-ID: %s\r\n"
			       "CSeq: %u REGISTER\r\n"
			       "%s"
			       "Content-Length: 0\r\n\r\n",
			       code, sip_status_reason(code), R_LOCAL_HOST, R_LOCAL_PORT,
			       ri.branch, via_params ? via_params : "", R_AOR, ri.from_tag,
			       R_AOR, ri.call_id, ri.cseq, extra ? extra : "");

	if (len >= sizeof(text))
		return;

	reply_recv(&mgr, text, len);
}

static const char *chal_2069(void)
{
	static char buf[256];

	snprintf(buf, sizeof(buf),
		 "WWW-Authenticate: Digest realm=\"%s\",nonce=\"%s\",opaque=\"%s\"\r\n",
		 R_REALM, R_NONCE, R_OPAQUE);

	return buf;
}

static const char *chal_nonce(const char *nonce, const char *extra)
{
	static char buf[256];

	snprintf(buf, sizeof(buf), "WWW-Authenticate: Digest realm=\"%s\",nonce=\"%s\"%s\r\n",
		 R_REALM, nonce, extra ? extra : "");

	return buf;
}

static bool has(const char *needle)
{
	return strstr(last_text(), needle) != NULL;
}

/* register once, ending in SIP_REG_REGISTERED with one target */
static void register_ok(void)
{
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, NULL, chal_2069());
	reply(200, NULL, "Expires: 3600\r\n");
}

/* ------------------------------------------------------------ basic flow */

static void flow_tests(void)
{
	struct req_info ri;
	char call_id[128];

	CASE("a start with one target sends a REGISTER at once");
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5060));
	sip_reg_start(&reg);
	CHECK_UINT(fk.sends, 1);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "REGISTER"));
	CHECK_UINT(ri.cseq, 1);
	CHECK(!ri.auth && !ri.proxy_auth);
	CHECK(has("REGISTER sip:sip.example.com SIP/2.0"));
	CHECK(has("Via: SIP/2.0/UDP 10.0.0.5:5060;branch=z9hG4bK"));
	CHECK(has(";rport"));
	CHECK(has("To: <" R_AOR ">"));
	CHECK(!has("To: <" R_AOR ">;tag="));
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060>"));
	CHECK(has("Expires: 3600"));
	CHECK(has("User-Agent: uvoice-test/1"));
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_TRYING);
	CHECK_UINT(fk.send[0].port, 5060);
	CHECK(!strcmp(fk.send[0].host, "192.0.2.10"));

	CASE("the 401 is answered with credentials on a new CSeq");
	snprintf(call_id, sizeof(call_id), "%s", ri.call_id);
	reply(401, NULL, chal_2069());
	CHECK_UINT(fk.sends, 2);
	CHECK(last_req(&ri));
	CHECK_UINT(ri.cseq, 2);
	CHECK(ri.auth);
	CHECK(!strcmp(ri.call_id, call_id));
	CHECK(has("response=\"" R_RESPONSE "\""));
	CHECK(has("opaque=\"" R_OPAQUE "\""));
	CHECK(has("uri=\"" R_REGISTRAR "\""));
	CHECK(!has("qop="));
	CHECK(!has("nc="));
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_TRYING);
	CHECK_UINT(fk.failed, 0);

	/*
	 * Neither attempt carries a Route header. An outbound proxy is a next
	 * hop address, not a route.
	 */
	CASE("a REGISTER never carries a Route header");
	CHECK_UINT(fk.sends, 2);
	CHECK(strstr(send_text(0), "\r\nRoute:") == NULL);
	CHECK(strstr(send_text(1), "\r\nRoute:") == NULL);

	CASE("the 200 registers the account and reports the accepted expiry");
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	CHECK_UINT(fk.registered, 1);
	CHECK_UINT(fk.last_expires, 3600);
	CHECK_UINT(sip_reg_expires(&reg), 3600);

	CASE("the refresh runs at half the accepted expiry");
	fake_advance(1799 * 1000);
	CHECK_UINT(fk.sends, 2);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 3);
	CHECK(last_req(&ri));
	CHECK_UINT(ri.cseq, 3);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REFRESHING);

	CASE("the refresh carries the credentials without a new challenge");
	CHECK(ri.auth);
	CHECK(has("response=\"" R_RESPONSE "\""));

	CASE("the Call-ID and the From tag never change");
	CHECK(!strcmp(ri.call_id, call_id));
	CHECK(!strcmp(sip_reg_call_id(&reg), call_id));

	CASE("the refresh 200 registers again");
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 2);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);

	CASE("the CSeq only ever grows");
	CHECK_UINT(sip_reg_cseq(&reg), 3);
	fake_advance(1800 * 1000);
	CHECK(last_req(&ri));
	CHECK_UINT(ri.cseq, 4);

	CASE("a response to a transaction the account does not own is ignored");
	sip_reg_txn_resp(&reg, 0x00ff00ff, &reply_stored);
	CHECK_UINT(fk.registered, 2);

	CASE("a provisional response changes nothing");
	reply(100, NULL, NULL);
	CHECK_UINT(fk.registered, 2);
	CHECK_UINT(fk.failed, 0);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REFRESHING);
}

/* --------------------------------------------------------- refresh clamp */

static void refresh_tests(void)
{
	CASE("the ordinary expiry refreshes at half");
	CHECK_UINT(sip_reg_refresh_secs(3600), 1800);
	CHECK_UINT(sip_reg_refresh_secs(1800), 900);
	CHECK_UINT(sip_reg_refresh_secs(120), 60);

	CASE("the 30 second lead wins once half is the larger value");
	CHECK_UINT(sip_reg_refresh_secs(100), 50);
	CHECK_UINT(sip_reg_refresh_secs(70), 35);
	CHECK_UINT(sip_reg_refresh_secs(65), 32);
	CHECK_UINT(sip_reg_refresh_secs(60), 30);

	CASE("expires 30 falls on the 15 second floor");
	CHECK_UINT(sip_reg_refresh_secs(30), 15);
	CHECK_UINT(sip_reg_refresh_secs(31), 15);
	CHECK_UINT(sip_reg_refresh_secs(40), 15);

	/*
	 * The floor may never land at or past the expiry: a refresh after the
	 * binding lapsed leaves the account unregistered for the gap.
	 */
	CASE("expires 16 is the smallest one the 15 second floor still fits");
	CHECK_UINT(sip_reg_refresh_secs(16), 15);

	CASE("an expiry at or below the floor refreshes at half instead");
	CHECK_UINT(sip_reg_refresh_secs(15), 7);
	CHECK_UINT(sip_reg_refresh_secs(10), 5);
	CHECK_UINT(sip_reg_refresh_secs(6), 3);

	CASE("an absurdly small expiry still produces a usable timer");
	CHECK_UINT(sip_reg_refresh_secs(5), 2);
	CHECK_UINT(sip_reg_refresh_secs(1), 2);
	CHECK_UINT(sip_reg_refresh_secs(0), 2);

	CASE("every refresh lands strictly inside the expiry it came from");
	{
		uint32_t e;

		for (e = 1; e <= 400; e++)
			CHECK(sip_reg_refresh_secs(e) < e || e <= SIP_REG_REFRESH_FLOOR);
	}

	CASE("a 60 second binding refreshes 30 seconds later on the wire");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, NULL, "Expires: 60\r\n");
	CHECK_UINT(fk.last_expires, 60);
	fake_advance(29 * 1000);
	CHECK_UINT(fk.sends, 1);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 2);

	CASE("a 30 second binding refreshes after 15 seconds");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, NULL, "Expires: 30\r\n");
	fake_advance(14 * 1000);
	CHECK_UINT(fk.sends, 1);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 2);

	CASE("a 10 second binding refreshes after 5 seconds, before it lapses");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, NULL, "Expires: 10\r\n");
	CHECK_UINT(fk.last_expires, 10);
	fake_advance(4 * 1000);
	CHECK_UINT(fk.sends, 1);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 2);
}

/* ------------------------------------------------------- accepted expiry */

static void expiry_tests(void)
{
	CASE("our own binding among several Contacts decides the expiry");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, NULL,
	      "Contact: <sip:other@198.51.100.7:5060>;expires=90\r\n"
	      "Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=1200\r\n"
	      "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 1);
	CHECK_UINT(fk.last_expires, 1200);

	CASE("the Contact expires parameter beats the Expires header");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, NULL,
	      "Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=600\r\n"
	      "Expires: 3600\r\n");
	CHECK_UINT(fk.last_expires, 600);

	CASE("a Contact that is not ours is skipped, the Expires header wins");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, NULL,
	      "Contact: <sip:someone@198.51.100.7:5060>;expires=90\r\n"
	      "Expires: 2400\r\n");
	CHECK_UINT(fk.last_expires, 2400);

	CASE("our binding without an expires parameter falls back to Expires");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, NULL,
	      "Contact: <sip:uvoice-test@10.0.0.5:5060>\r\n"
	      "Expires: 1500\r\n");
	CHECK_UINT(fk.last_expires, 1500);

	CASE("without a Contact and without Expires the request value stands");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, NULL, NULL);
	CHECK_UINT(fk.last_expires, 3600);

	CASE("a Contact list holding a star is not mistaken for our binding");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, NULL, "Contact: *\r\nExpires: 900\r\n");
	CHECK_UINT(fk.last_expires, 900);

	CASE("expiry 0 in a 200 takes the account down and schedules a retry");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, NULL, "Expires: 0\r\n");
	CHECK_UINT(fk.registered, 0);
	CHECK_UINT(fk.unregistered, 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_BACKOFF);
	CHECK_UINT(sip_reg_expires(&reg), 0);
	fake_advance(5 * 1000);
	CHECK_UINT(fk.sends, 2);
}

/* -------------------------------------------------------------- 423 path */

static void min_expires_tests(void)
{
	struct req_info ri;

	CASE("423 raises the requested expiry and retries");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	CHECK(last_req(&ri));
	CHECK_UINT(ri.expires, 3600);
	reply(423, NULL, "Min-Expires: 7200\r\n");
	CHECK_UINT(fk.sends, 2);
	CHECK(last_req(&ri));
	CHECK_UINT(ri.expires, 7200);
	CHECK_UINT(ri.cseq, 2);
	CHECK_UINT(fk.failed, 0);

	CASE("the 200 that follows registers on the raised expiry");
	reply(200, NULL, "Expires: 7200\r\n");
	CHECK_UINT(fk.registered, 1);
	CHECK_UINT(fk.last_expires, 7200);

	CASE("the raised minimum survives into the next refresh");
	fake_advance(3600 * 1000);
	CHECK(last_req(&ri));
	CHECK_UINT(ri.expires, 7200);

	CASE("a 423 without Min-Expires fails the attempt");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(423, NULL, NULL);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 423);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_BACKOFF);

	CASE("a 423 that asks for no more than we offered fails instead of looping");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(423, NULL, "Min-Expires: 3600\r\n");
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.sends, 1);

	CASE("a registrar that keeps raising the minimum is given up on");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(423, NULL, "Min-Expires: 4000\r\n");
	reply(423, NULL, "Min-Expires: 5000\r\n");
	CHECK_UINT(fk.sends, 3);
	reply(423, NULL, "Min-Expires: 6000\r\n");
	CHECK_UINT(fk.sends, 3);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 423);
}

/* ---------------------------------------------------------- authentication */

static void auth_tests(void)
{
	struct req_info ri;

	CASE("the same nonce twice is a credential failure, not a third attempt");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, NULL, chal_2069());
	CHECK_UINT(fk.sends, 2);
	reply(401, NULL, chal_2069());
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 401);
	CHECK_UINT(fk.last_retry, SIP_REG_BACKOFF_AUTH);

	CASE("a credential failure drops the cached challenge");
	CHECK(!sip_auth_state_valid(&reg.auth[SIP_AUTH_WWW]));
	fake_advance(SIP_REG_BACKOFF_AUTH * 1000);
	CHECK_UINT(fk.sends, 3);
	CHECK(last_req(&ri));
	CHECK(!ri.auth);

	CASE("a fresh nonce with no stale flag is still only one retry");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, NULL, chal_nonce("nonce-a", NULL));
	CHECK_UINT(fk.sends, 2);
	reply(401, NULL, chal_nonce("nonce-b", NULL));
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(fk.failed, 1);

	CASE("stale=true with a fresh nonce buys another try");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, NULL, chal_nonce("nonce-a", NULL));
	CHECK_UINT(fk.sends, 2);
	reply(401, NULL, chal_nonce("nonce-b", ",stale=true"));
	CHECK_UINT(fk.sends, 3);
	CHECK_UINT(fk.failed, 0);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 1);

	CASE("a registrar that only ever says stale is given up on");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, NULL, chal_nonce("s1", ",stale=true"));
	reply(401, NULL, chal_nonce("s2", ",stale=true"));
	reply(401, NULL, chal_nonce("s3", ",stale=true"));
	CHECK_UINT(fk.sends, 4);
	reply(401, NULL, chal_nonce("s4", ",stale=true"));
	CHECK_UINT(fk.sends, 4);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 401);

	CASE("a 407 is answered with Proxy-Authorization");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(407, NULL, "Proxy-Authenticate: Digest realm=\"proxy.example.net\","
			 "nonce=\"proxy-nonce\"\r\n");
	CHECK_UINT(fk.sends, 2);
	CHECK(last_req(&ri));
	CHECK(ri.proxy_auth && !ri.auth);
	CHECK(has("realm=\"proxy.example.net\""));

	CASE("a challenge of both kinds produces both credential headers");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, NULL,
	      "WWW-Authenticate: Digest realm=\"" R_REALM "\",nonce=\"" R_NONCE "\"\r\n"
	      "Proxy-Authenticate: Digest realm=\"proxy.example.net\",nonce=\"pn\"\r\n");
	CHECK(last_req(&ri));
	CHECK(ri.auth && ri.proxy_auth);
	CHECK(has("Authorization: Digest"));
	CHECK(has("Proxy-Authorization: Digest"));

	CASE("qop=auth counts up across refreshes");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, NULL, chal_nonce("qop-nonce", ",qop=\"auth\""));
	CHECK(has("nc=00000001"));
	CHECK(has("qop=auth"));
	CHECK(has("cnonce="));
	reply(200, NULL, "Expires: 3600\r\n");
	fake_advance(1800 * 1000);
	CHECK(has("nc=00000002"));

	CASE("a challenge we cannot answer fails the attempt at once");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, NULL, chal_nonce("ai", ",qop=\"auth-int\""));
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 401);

	CASE("a 401 with no challenge at all fails the attempt");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, NULL, NULL);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(fk.failed, 1);

	CASE("a 403 backs off as a credential failure");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(403, NULL, NULL);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 403);
	CHECK_UINT(fk.last_retry, SIP_REG_BACKOFF_AUTH);

	/* the production value, pinned: the line refuses every call while it
	 * waits (proto/sip/reg.h SIP_REG_BACKOFF_AUTH) */
	CASE("a credential failure retries after 30 s");
	CHECK_UINT(fk.last_retry, 30);
}

/* ------------------------------------------------------- contact and NAT */

static void contact_tests(void)
{
	struct req_info ri;

	CASE("without received or rport the Contact keeps the configured address");
	register_ok();
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.contact, "sip:uvoice-test@10.0.0.5:5060"));
	CHECK(!strcmp(sip_reg_contact(&reg), "sip:uvoice-test@10.0.0.5:5060"));

	CASE("received and rport on the 401 rebuild the Contact for the retry");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, ";received=203.0.113.9;rport=41234", chal_2069());
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.contact, "sip:uvoice-test@203.0.113.9:41234"));
	CHECK_UINT(reg.stat.contact_rebuilt, 1);

	CASE("the binding that follows is reported without a second REGISTER");
	reply(200, ";received=203.0.113.9;rport=41234", "Expires: 3600\r\n");
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(fk.registered, 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);

	CASE("a Contact that moves under the 200 registers again before reporting");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, ";received=203.0.113.9;rport=50001", "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 0);
	CHECK_UINT(fk.sends, 2);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.contact, "sip:uvoice-test@203.0.113.9:50001"));
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REFRESHING);
	reply(200, ";received=203.0.113.9;rport=50001", "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 1);
	CHECK_UINT(fk.sends, 2);

	CASE("a later rebinding to a new port re-registers");
	fake_advance(1800 * 1000);
	CHECK_UINT(fk.sends, 3);
	reply(200, ";received=203.0.113.9;rport=50002", "Expires: 3600\r\n");
	CHECK_UINT(fk.sends, 4);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.contact, "sip:uvoice-test@203.0.113.9:50002"));

	CASE("received alone moves the host and keeps the port");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, ";received=203.0.113.9", chal_2069());
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.contact, "sip:uvoice-test@203.0.113.9:5060"));

	CASE("a response without the parameters leaves an adopted Contact alone");
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 1);
	CHECK(!strcmp(sip_reg_contact(&reg), "sip:uvoice-test@203.0.113.9:5060"));

	CASE("a bare rport with no value changes nothing");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, ";rport", "Expires: 3600\r\n");
	CHECK_UINT(reg.stat.contact_rebuilt, 0);
	CHECK_UINT(fk.registered, 1);

	CASE("an account without a contact user still builds a Contact");
	fake_reset();
	cfg.contact_user = NULL;
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	CHECK(has("Contact: <sip:10.0.0.5:5060>"));
	cfg_default();

	CASE("contact: a v6 contact host is bracketed exactly once");
	cfg.local_host = "2001:db8::1";
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "2001:db8::10", 5060));
	sip_reg_start(&reg);
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::1]:5060>"));
	CHECK(!strcmp(sip_reg_contact(&reg),
		      "sip:uvoice-test@[2001:db8::1]:5060"));

	CASE("a bracketed received value is stored bare and bracketed once "
	     "on the wire");
	fk.trace[0] = 0;
	reply(401, ";received=[2001:db8::99];rport=41234", chal_2069());
	CHECK(!strcmp(reg.contact_host, "2001:db8::99"));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.contact, "sip:uvoice-test@[2001:db8::99]:41234"));
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::99]:41234>\r\n"));
	CHECK_UINT(reg.stat.contact_rebuilt, 1);
	fk.trace[0] = 0;
	reply(200, ";received=[2001:db8::99];rport=41234",
	      "Contact: <sip:uvoice-test@[2001:db8::99]:41234>;expires=1200\r\n"
	      "Expires: 3600\r\n");
	CHECK_UINT(sip_reg_expires(&reg), 1200);
	CHECK_UINT(fk.registered, 1);
	CHECK_UINT(reg.stat.contact_rebuilt, 1);
	cfg_default();
}

/* ------------------------------------------------------ the transport token */

/*
 * The Via carries the token after "SIP/2.0/", and the Contact URI carries it
 * as ";transport=", lower cased (sip/reg.h). reg.c takes it verbatim; the
 * account layer owns the vocabulary.
 */
static void transport_tests(void)
{
	/* 220 characters: the URI fits at 10.0.0.5:5060 and not at the longer
	 * address the RFC 3581 rebuild adopts below */
	static char long_user[221];
	char uri[SIP_REG_URI_MAX];
	char needle[SIP_REG_URI_MAX + 32];
	struct req_info ri;

	CASE("transport: a NULL token keeps the UDP wire the account always had");
	register_ok();
	CHECK(has("Via: SIP/2.0/UDP 10.0.0.5:5060;branch=z9hG4bK"));
	CHECK(!has(";transport="));
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060>"));
	CHECK(!strcmp(sip_reg_contact(&reg), "sip:uvoice-test@10.0.0.5:5060"));

	CASE("transport: TCP reaches the Via and the Contact URI");
	fake_reset();
	cfg.transport = "TCP";
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	CHECK(has("Via: SIP/2.0/TCP 10.0.0.5:5060;branch=z9hG4bK"));
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060;transport=tcp>"));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.contact, "sip:uvoice-test@10.0.0.5:5060;transport=tcp"));
	CHECK(!strcmp(sip_reg_contact(&reg), "sip:uvoice-test@10.0.0.5:5060;transport=tcp"));

	/* inside the angle brackets: a header parameter would sit behind the '>' */
	CASE("transport: the parameter belongs to the URI, not to the header");
	CHECK(!has(">;transport="));

	CASE("transport: the auth retry and the refresh carry the token as well");
	reply(401, NULL, chal_2069());
	CHECK_UINT(fk.sends, 2);
	CHECK(has("Via: SIP/2.0/TCP 10.0.0.5:5060;branch=z9hG4bK"));
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060;transport=tcp>"));
	reply(200, NULL, "Expires: 3600\r\n");
	fake_advance(1800 * 1000);
	CHECK_UINT(fk.sends, 3);
	CHECK(has("Via: SIP/2.0/TCP 10.0.0.5:5060;branch=z9hG4bK"));
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060;transport=tcp>"));

	CASE("transport: TLS is one more token, and never a sips: URI");
	fake_reset();
	cfg.transport = "TLS";
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	CHECK(has("Via: SIP/2.0/TLS 10.0.0.5:5060;branch=z9hG4bK"));
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060;transport=tls>"));
	CHECK(!has("sips:"));

	CASE("transport: the RFC 3581 rebuild keeps the token at the new address");
	fake_reset();
	cfg.transport = "TLS";
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, ";received=203.0.113.9;rport=41234", chal_2069());
	CHECK_UINT(reg.stat.contact_rebuilt, 1);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.contact, "sip:uvoice-test@203.0.113.9:41234;transport=tls"));
	CHECK(has("Contact: <sip:uvoice-test@203.0.113.9:41234;transport=tls>"));

	CASE("transport: the registrar still finds our binding among the Contacts");
	reply(200, ";received=203.0.113.9;rport=41234",
	      "Contact: <sip:uvoice-test@203.0.113.9:41234;transport=tls>;expires=1200\r\n");
	CHECK_UINT(fk.registered, 1);
	CHECK_UINT(fk.last_expires, 1200);
	CHECK_UINT(fk.sends, 2);

	CASE("transport: an account without a contact user carries the token too");
	fake_reset();
	cfg.contact_user = NULL;
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	CHECK(has("Contact: <sip:10.0.0.5:5060;transport=tls>"));
	cfg_default();

	CASE("transport: a bracketed IPv6 host keeps the token behind the port");
	fake_reset();
	cfg.transport = "TCP";
	cfg.local_host = "2001:db8::1";
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	CHECK(has("Via: SIP/2.0/TCP [2001:db8::1]:5060;branch=z9hG4bK"));
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::1]:5060;transport=tcp>"));
	cfg_default();

	/*
	 * ";transport=" is 11 characters of the 24 byte parameter buffer, so 12
	 * is the longest token the Contact carries and a longer one is refused,
	 * never truncated.
	 */
	CASE("transport: a 12 character token is the longest one that reaches the wire");
	fake_reset();
	cfg.transport = "TCPTCPTCPTCP";
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	CHECK(has("Via: SIP/2.0/TCPTCPTCPTCP 10.0.0.5:5060;branch=z9hG4bK"));
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060;transport=tcptcptcptcp>"));
	CHECK(!strcmp(sip_reg_contact(&reg),
		      "sip:uvoice-test@10.0.0.5:5060;transport=tcptcptcptcp"));

	CASE("transport: a 13 character token is refused instead of truncated");
	fake_reset();
	cfg.transport = "TCPTCPTCPTCPT";
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	CHECK(has("Via: SIP/2.0/TCPTCPTCPTCPT 10.0.0.5:5060;branch=z9hG4bK"));
	CHECK(!has(";transport="));
	CHECK_UINT(strlen(sip_reg_contact(&reg)), 0);

	CASE("transport: the refusal at the rebuild leaves the URI alone and never loops");
	reply(200, ";received=203.0.113.9;rport=41234", "Expires: 3600\r\n");
	CHECK_UINT(reg.stat.contact_rebuilt, 1);
	CHECK(!reg.contact_dirty);
	CHECK_UINT(strlen(sip_reg_contact(&reg)), 0);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(fk.registered, 1);
	CHECK_UINT(reg.rebind_run, 0);

	/*
	 * The URI buffer is the other refusal. A build that does not fit must
	 * leave the address the account already holds standing: a truncated one
	 * would go on the wire as a live binding with contact_dirty false.
	 */
	CASE("transport: a rebuild that does not fit leaves the previous Contact standing");
	memset(long_user, 'u', sizeof(long_user) - 1);
	long_user[sizeof(long_user) - 1] = 0;
	fake_reset();
	cfg.transport = "TLS";
	cfg.contact_user = long_user;
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	snprintf(uri, sizeof(uri), "sip:%s@10.0.0.5:5060;transport=tls", long_user);
	CHECK(!strcmp(sip_reg_contact(&reg), uri));
	reply(200, ";received=203.0.113.9;rport=41234", "Expires: 3600\r\n");
	CHECK_UINT(reg.stat.contact_rebuilt, 1);
	CHECK(!reg.contact_dirty);
	CHECK(!strcmp(sip_reg_contact(&reg), uri));
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(fk.registered, 1);

	CASE("transport: and the refresh puts that same address back on the wire");
	snprintf(needle, sizeof(needle), "Contact: <%s>", uri);
	fake_advance(1800 * 1000);
	CHECK_UINT(fk.sends, 2);
	CHECK(has(needle));
	cfg_default();

	/*
	 * The token is taken verbatim: reg.c neither validates nor maps it, so an
	 * empty one reaches the wire as an empty one. This pins the current
	 * contract, not a wire shape anybody wants; the account layer owns the
	 * vocabulary and is where a guard belongs.
	 */
	CASE("transport: an empty token passes through unvalidated");
	fake_reset();
	cfg.transport = "";
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	CHECK(has("Via: SIP/2.0/ 10.0.0.5:5060;branch=z9hG4bK"));
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060;transport=>"));

	cfg_default();
	fake_reset();
}

/* -------------------------------------------------------------- targets */

static bool target_host_is(unsigned int idx, const char *host)
{
	const struct sip_reg_target *t = sip_reg_target_get(&reg, idx);
	char buf[64];

	if (!t || !sip_dst_host(&t->dst, buf, sizeof(buf)))
		return false;

	return !strcmp(buf, host);
}

static void target_tests(void)
{
	const struct sip_reg_target *t;
	unsigned int i;

	CASE("a start without a target waits for the resolver");
	fake_reset();
	sip_reg_start(&reg);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_RESOLVING);
	CHECK_UINT(fk.sends, 0);

	CASE("the first target that arrives starts the attempt");
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5060));
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_TRYING);

	CASE("the selection is sticky while the target answers");
	reply(200, NULL, "Expires: 3600\r\n");
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	fake_advance(1800 * 1000);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.10"));

	CASE("a timeout penalizes the target and moves to the next one");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	CHECK(!strcmp(fk.send[0].host, "192.0.2.10"));
	fake_advance(32 * 1000 + 100);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 408);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && t->failures == 1 && t->penalty_until > fk.now);
	CHECK_UINT(reg.stat.switched, 1);

	CASE("the retry goes to the second target");
	fake_advance(6 * 1000);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.11"));

	CASE("a success clears the penalty of the target that answered");
	reply(200, NULL, "Expires: 3600\r\n");
	t = sip_reg_target_get(&reg, 1);
	CHECK(t && !t->failures && !t->penalty_until);
	CHECK_UINT(fk.registered, 1);

	CASE("with every target penalized the one that clears first is taken");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	reply(500, NULL, NULL);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	fake_advance(6 * 1000);
	reply(500, NULL, NULL);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && t->failures == 1);

	CASE("the target list refuses more than SIP_REG_TARGET_MAX entries");
	fake_reset();
	for (i = 0; i < SIP_REG_TARGET_MAX; i++)
		CHECK(sip_reg_target_add(&reg, "192.0.2.10", (uint16_t)(5060 + i)));
	CHECK(!sip_reg_target_add(&reg, "192.0.2.10", 6000));
	CHECK_UINT(sip_reg_target_count(&reg), SIP_REG_TARGET_MAX);

	CASE("targets: the sixteenth target is taken and the seventeenth "
	     "refused");
	fake_reset();
	for (i = 0; i < 16; i++)
		CHECK(sip_reg_target_add(&reg, "192.0.2.10",
					 (uint16_t)(5060 + i)));
	CHECK_UINT(sip_reg_target_count(&reg), 16);
	t = sip_reg_target_get(&reg, 15);
	CHECK(t && sip_dst_port(&t->dst) == 5075);
	CHECK(sip_reg_target_get(&reg, 16) == NULL);
	CHECK(!sip_reg_target_add(&reg, "192.0.2.10", 5076));
	CHECK_UINT(sip_reg_target_count(&reg), 16);
	sip_reg_targets_reload(&reg);
	CHECK_UINT(reg.stashed, 16);
	for (i = 0; i < 16; i++)
		CHECK(sip_reg_target_add(&reg, "192.0.2.10",
					 (uint16_t)(5075 - i)));
	sip_reg_targets_reload_end(&reg);
	CHECK_UINT(sip_reg_target_count(&reg), 16);
	CHECK_UINT(sip_reg_target_current(&reg), 15);

	CASE("a target already in the list is absorbed, not added twice");
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5060));
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5060));
	CHECK_UINT(sip_reg_target_count(&reg), 1);
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5080));
	CHECK_UINT(sip_reg_target_count(&reg), 2);
	CHECK(sip_reg_target_add(&reg, "192.0.2.11", 5060));
	CHECK_UINT(sip_reg_target_count(&reg), 3);

	CASE("the identity compares the port the default filled in");
	CHECK(sip_reg_target_add(&reg, "192.0.2.11", 0));
	CHECK_UINT(sip_reg_target_count(&reg), 3);
	CHECK(target_host_is(2, "192.0.2.11"));

	CASE("a duplicate is absorbed even with the list full");
	fake_reset();
	for (i = 0; i < SIP_REG_TARGET_MAX; i++)
		CHECK(sip_reg_target_add(&reg, "192.0.2.10", (uint16_t)(5060 + i)));
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5060));
	CHECK_UINT(sip_reg_target_count(&reg), SIP_REG_TARGET_MAX);
	CHECK(!sip_reg_target_add(&reg, "192.0.2.99", 5060));

	/*
	 * Two SRV rows of a zone can name one host and port, and each collects
	 * the same address. Two entries for it would make the failover step
	 * from an endpoint to itself and retry the server that just failed.
	 */
	CASE("a duplicated answer cannot fail over to the same endpoint");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	CHECK_UINT(sip_reg_target_count(&reg), 2);
	sip_reg_start(&reg);
	CHECK(!strcmp(fk.send[0].host, "192.0.2.10"));
	reply(500, NULL, NULL);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	CHECK(target_host_is(1, "192.0.2.11"));
	fake_advance(6 * 1000);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.11"));

	/*
	 * A reload can move the selection while a REGISTER is in flight. The
	 * outcome belongs to the server the attempt ran against, and the
	 * failover belongs to the server the line uses now, so the two are
	 * different targets and only the first one is penalized.
	 */
	CASE("a failed non-current target penalizes without advancing");
	fake_reset();
	sip_reg_target_add_prio(&reg, "192.0.2.10", 5060, 10);
	sip_reg_target_add_prio(&reg, "192.0.2.11", 5060, 20);
	sip_reg_start(&reg);
	CHECK(!strcmp(fk.send[0].host, "192.0.2.10"));
	sip_reg_targets_reload(&reg);
	sip_reg_target_add_prio(&reg, "192.0.2.10", 5060, 20);
	sip_reg_target_add_prio(&reg, "192.0.2.11", 5060, 10);
	sip_reg_targets_reload_end(&reg);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	fake_advance(32 * 1000 + 100);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 408);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	CHECK(target_host_is(1, "192.0.2.11"));
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && t->failures == 1 && t->penalty_until > fk.now);
	t = sip_reg_target_get(&reg, 1);
	CHECK(t && !t->failures && !t->penalty_until);
	CHECK_UINT(reg.stat.switched, 0);

	CASE("a target that is not an address is refused");
	fake_reset();
	CHECK(!sip_reg_target_add(&reg, "registrar.example.net", 5060));
	CHECK_UINT(sip_reg_target_count(&reg), 0);

	CASE("clearing the list while nothing is in flight goes back to resolving");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	sip_reg_targets_clear(&reg);
	reply(500, NULL, NULL);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_RESOLVING);
	CHECK_UINT(fk.last_retry, 0);
}

/* --------------------------------------------------------- the DNS reload */

/*
 * A TTL re-resolution asks for the same name and gets the same servers back in
 * a fresh RFC 2782 draw, so the same server arrives at another index. The
 * reload pair carries the sticky selection and the penalty boxes across that,
 * by identity. A plain clear plus re-add re-randomizes both once per TTL cycle.
 */

/* the answer a re-resolution delivers, as account.c applies it */
static void reload_answer(const char *a, const char *b)
{
	sip_reg_targets_reload(&reg);
	if (a)
		sip_reg_target_add(&reg, a, 5060);
	if (b)
		sip_reg_target_add(&reg, b, 5060);
	sip_reg_targets_reload_end(&reg);
}

/* the same answer with an SRV priority per target */
static bool reload_answer_prio(const char *a, uint16_t pa,
			       const char *b, uint16_t pb)
{
	sip_reg_targets_reload(&reg);
	if (a)
		sip_reg_target_add_prio(&reg, a, 5060, pa);
	if (b)
		sip_reg_target_add_prio(&reg, b, 5060, pb);

	return sip_reg_targets_reload_end(&reg);
}

static void reload_tests(void)
{
	const struct sip_reg_target *t;

	CASE("reload: the first answer of all is an ordinary load");
	fake_reset();
	sip_reg_start(&reg);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_RESOLVING);
	reload_answer("192.0.2.10", "192.0.2.11");
	CHECK_UINT(sip_reg_target_count(&reg), 2);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	CHECK_UINT(fk.sends, 1);
	CHECK(!strcmp(fk.send[0].host, "192.0.2.10"));

	CASE("reload: the current target follows its identity to a new index");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	reload_answer("192.0.2.11", "192.0.2.10");
	CHECK_UINT(sip_reg_target_count(&reg), 2);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	CHECK(target_host_is(1, "192.0.2.10"));

	CASE("reload: the refresh after it goes to the same server");
	fake_advance(1800 * 1000);
	CHECK_UINT(fk.sends, 2);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.10"));

	CASE("reload: a port that moved is another target, not the same one");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, NULL, "Expires: 3600\r\n");
	reload_answer("192.0.2.10", NULL);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	sip_reg_targets_reload(&reg);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_target_add(&reg, "192.0.2.10", 5080);
	sip_reg_targets_reload_end(&reg);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	CHECK(target_host_is(0, "192.0.2.11"));

	CASE("reload: the penalty state follows the identity, not the index");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	reply(500, NULL, NULL);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && t->failures == 1 && t->penalty_until > fk.now);
	reload_answer("192.0.2.11", "192.0.2.10");
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	CHECK(target_host_is(0, "192.0.2.11"));
	t = sip_reg_target_get(&reg, 1);
	CHECK(t && t->failures == 1 && t->penalty_until > fk.now);
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && !t->failures && !t->penalty_until);

	CASE("reload: the retry after it still avoids the penalized server");
	fake_advance(6 * 1000);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.11"));

	CASE("reload: a server that left the answer re-selects, not index 0");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	reply(500, NULL, NULL);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	/* .11 is gone; .10 is back at index 0 and still in its penalty box */
	reload_answer("192.0.2.10", "192.0.2.12");
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	CHECK(target_host_is(1, "192.0.2.12"));
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && t->failures == 1 && t->penalty_until > fk.now);

	CASE("reload: with every survivor penalized the soonest free one wins");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_target_add(&reg, "192.0.2.12", 5060);
	sip_reg_start(&reg);
	reply(500, NULL, NULL);				/* .10 penalized first */
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	fake_advance(6 * 1000);
	reply(500, NULL, NULL);				/* then .11 */
	CHECK_UINT(sip_reg_target_current(&reg), 2);
	fake_advance(11 * 1000);
	reply(500, NULL, NULL);				/* then .12 */
	CHECK_UINT(sip_reg_target_current(&reg), 0);	/* .10 clears first */
	/* .10 leaves the answer, and the two survivors are both penalized */
	reload_answer("192.0.2.12", "192.0.2.11");
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	CHECK(target_host_is(1, "192.0.2.11"));

	CASE("reload: an attempt in flight lands on its own target afterwards");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	CHECK_UINT(fk.sends, 1);
	CHECK(!strcmp(fk.send[0].host, "192.0.2.10"));
	reload_answer("192.0.2.11", "192.0.2.10");
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	reply(500, NULL, NULL);				/* the answer of .10 */
	CHECK_UINT(fk.failed, 1);
	t = sip_reg_target_get(&reg, 1);
	CHECK(t && t->failures == 1 && t->penalty_until > fk.now);
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && !t->failures && !t->penalty_until);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	CHECK(target_host_is(0, "192.0.2.11"));

	CASE("reload: an attempt whose target left the answer penalizes nothing");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	CHECK(!strcmp(fk.send[0].host, "192.0.2.10"));
	reload_answer("192.0.2.20", "192.0.2.21");
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	reply(500, NULL, NULL);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 500);
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && !t->failures && !t->penalty_until);
	t = sip_reg_target_get(&reg, 1);
	CHECK(t && !t->failures && !t->penalty_until);
	CHECK_UINT(reg.stat.switched, 0);

	CASE("reload: the retry after that runs on the new answer");
	fake_advance(6 * 1000);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.20"));
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 1);

	/*
	 * The orphan belongs to one completion. An attempt that fails before it
	 * reaches the wire, a build or a transaction start that fails, must not
	 * inherit it: that failure is the current target's and owes a penalty
	 * and a failover.
	 */
	CASE("reload: an orphan does not survive into the next failed attempt");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reload_answer("192.0.2.20", "192.0.2.21");
	reply(500, NULL, NULL);
	CHECK_UINT(fk.failed, 1);
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && !t->failures && !t->penalty_until);
	fk.send_fail = true;
	fake_advance(6 * 1000);
	CHECK_UINT(fk.failed, 2);
	CHECK_UINT(fk.last_code, 503);
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && t->failures == 1 && t->penalty_until > fk.now);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	CHECK_UINT(reg.stat.switched, 1);
	fk.send_fail = false;

	CASE("reload: a 200 from a target that left the answer clears nothing");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	reply(500, NULL, NULL);				/* .10 penalized, cur is .11 */
	fake_advance(6 * 1000);				/* the retry goes to .11 */
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.11"));
	reload_answer("192.0.2.10", "192.0.2.30");	/* .11 leaves under it */
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 1);
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && t->failures == 1 && t->penalty_until > fk.now);

	CASE("reload: a duplicated answer keeps one entry with its penalty");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	reply(500, NULL, NULL);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	/* the zone answers with two SRV rows that collect the same address */
	sip_reg_targets_reload(&reg);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_targets_reload_end(&reg);
	CHECK_UINT(sip_reg_target_count(&reg), 2);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	CHECK(target_host_is(0, "192.0.2.11"));
	t = sip_reg_target_get(&reg, 1);
	CHECK(t && t->failures == 1 && t->penalty_until > fk.now);

	CASE("reload: an answer with nothing in it empties the list");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reload_answer(NULL, NULL);
	CHECK_UINT(sip_reg_target_count(&reg), 0);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	CHECK(sip_reg_target_get(&reg, 0) == NULL);

	CASE("reload: an end without a reload leaves the selection alone");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	reply(500, NULL, NULL);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	sip_reg_targets_reload_end(&reg);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && t->failures == 1 && t->penalty_until > fk.now);

	CASE("reload: a full clear still resets the selection and the penalties");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	reply(500, NULL, NULL);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	sip_reg_targets_clear(&reg);
	CHECK_UINT(sip_reg_target_count(&reg), 0);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && !t->failures && !t->penalty_until);
	t = sip_reg_target_get(&reg, 1);
	CHECK(t && !t->failures && !t->penalty_until);

	CASE("reload: a clear inside a reload window is still a full reset");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	reply(500, NULL, NULL);
	sip_reg_targets_reload(&reg);
	sip_reg_targets_clear(&reg);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_targets_reload_end(&reg);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	CHECK(target_host_is(0, "192.0.2.11"));
	t = sip_reg_target_get(&reg, 1);
	CHECK(t && t->failures == 1);

	CASE("reload: the end reports whether the endpoint set moved");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	/* a swap of the same endpoints is a redraw, not a move */
	sip_reg_targets_reload(&reg);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	CHECK(!sip_reg_targets_reload_end(&reg));
	/* a grown answer moved the set */
	sip_reg_targets_reload(&reg);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_target_add(&reg, "192.0.2.12", 5060);
	CHECK(sip_reg_targets_reload_end(&reg));
	/* a moved port is another endpoint */
	sip_reg_targets_reload(&reg);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_target_add(&reg, "192.0.2.12", 5061);
	CHECK(sip_reg_targets_reload_end(&reg));
	/* a shrink moved it too */
	sip_reg_targets_reload(&reg);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	CHECK(sip_reg_targets_reload_end(&reg));
	/* an end without a reload reports nothing */
	CHECK(!sip_reg_targets_reload_end(&reg));

	CASE("reload: the first answer after an empty list reports a move");
	fake_reset();
	sip_reg_start(&reg);
	sip_reg_targets_reload(&reg);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	CHECK(sip_reg_targets_reload_end(&reg));

	/*
	 * A target that left one answer and came back in the next owns its
	 * attempt again. The outcome is matched by identity, so a server that
	 * failed is penalized whether or not it blinked out of the answer.
	 */
	CASE("reload: a target that left and returned owns its attempt again");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	CHECK(!strcmp(fk.send[0].host, "192.0.2.10"));
	reload_answer("192.0.2.20", "192.0.2.11");
	reload_answer("192.0.2.10", "192.0.2.11");
	reply(500, NULL, NULL);
	CHECK_UINT(fk.failed, 1);
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && t->failures == 1 && t->penalty_until > fk.now);
	CHECK(target_host_is(0, "192.0.2.10"));

	/* ------------------------------------------------- the SRV priority */

	CASE("reload: a priority swap moves the selection to the new best "
	     "server");
	fake_reset();
	sip_reg_target_add_prio(&reg, "192.0.2.10", 5060, 10);
	sip_reg_target_add_prio(&reg, "192.0.2.11", 5060, 20);
	sip_reg_start(&reg);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	reload_answer_prio("192.0.2.10", 20, "192.0.2.11", 10);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	CHECK(target_host_is(1, "192.0.2.11"));

	CASE("reload: a new server at a better priority takes the line");
	fake_reset();
	sip_reg_target_add_prio(&reg, "192.0.2.11", 5060, 20);
	sip_reg_start(&reg);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	reload_answer_prio("192.0.2.10", 10, "192.0.2.11", 20);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	CHECK(target_host_is(0, "192.0.2.10"));

	/*
	 * A primary in its penalty box holds no group of its own: the best
	 * group is the one the secondary sits in, so the line stays there.
	 */
	CASE("reload: a penalized primary keeps the line on the secondary");
	fake_reset();
	sip_reg_target_add_prio(&reg, "192.0.2.10", 5060, 10);
	sip_reg_target_add_prio(&reg, "192.0.2.11", 5060, 20);
	sip_reg_start(&reg);
	reply(500, NULL, NULL);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	reload_answer_prio("192.0.2.11", 20, "192.0.2.10", 10);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	CHECK(target_host_is(0, "192.0.2.11"));

	CASE("reload: a recovered primary is re-taken once its penalty ends");
	fake_reset();
	sip_reg_target_add_prio(&reg, "192.0.2.10", 5060, 10);
	sip_reg_target_add_prio(&reg, "192.0.2.11", 5060, 20);
	sip_reg_start(&reg);
	reply(500, NULL, NULL);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	fake_advance(6 * 1000);
	reply(200, NULL, "Expires: 3600\r\n");
	reload_answer_prio("192.0.2.10", 10, "192.0.2.11", 20);
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	fake_advance(61 * 1000);
	reload_answer_prio("192.0.2.10", 10, "192.0.2.11", 20);
	CHECK_UINT(sip_reg_target_current(&reg), 0);

	CASE("reload: the end reports a selection the priority moved");
	fake_reset();
	sip_reg_target_add_prio(&reg, "192.0.2.10", 5060, 10);
	sip_reg_target_add_prio(&reg, "192.0.2.11", 5060, 20);
	sip_reg_start(&reg);
	/* the same endpoints at the same priorities move nothing */
	CHECK(!reload_answer_prio("192.0.2.10", 10, "192.0.2.11", 20));
	/* the swap moves the selection, and the set did not change */
	CHECK(reload_answer_prio("192.0.2.10", 20, "192.0.2.11", 10));
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	/* and the answer that puts the priorities back reports the move back */
	CHECK(reload_answer_prio("192.0.2.10", 10, "192.0.2.11", 20));
	CHECK_UINT(sip_reg_target_current(&reg), 0);

	CASE("reload: an absorbed duplicate keeps the lower priority");
	fake_reset();
	sip_reg_targets_reload(&reg);
	sip_reg_target_add_prio(&reg, "192.0.2.10", 5060, 20);
	sip_reg_target_add_prio(&reg, "192.0.2.10", 5060, 10);
	sip_reg_target_add_prio(&reg, "192.0.2.11", 5060, 15);
	sip_reg_targets_reload_end(&reg);
	CHECK_UINT(sip_reg_target_count(&reg), 2);
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && t->prio == 10);
	t = sip_reg_target_get(&reg, 1);
	CHECK(t && t->prio == 15);
	/* a later record of a worse priority does not raise it back */
	CHECK(sip_reg_target_add_prio(&reg, "192.0.2.10", 5060, 30));
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && t->prio == 10);
}

/* -------------------------------------------------------------- retarget */

static void retarget_tests(void)
{
	struct req_info first, ri;
	unsigned int sends;

	CASE("retarget: nothing in flight sends at once to the new sticky target");
	register_ok();
	CHECK_UINT(fk.registered, 1);
	sends = fk.sends;
	reload_answer("192.0.2.20", NULL);
	sip_reg_retarget(&reg);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.20"));
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REFRESHING);

	CASE("retarget: the Call-ID and the From tag stay and the CSeq grows");
	CHECK(req_info_at(0, &first));
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.call_id, first.call_id));
	CHECK(!strcmp(ri.from_tag, first.from_tag));
	CHECK_UINT(ri.cseq, 3);

	CASE("retarget: no unregistered report and no state through UNREGISTERED");
	CHECK_UINT(fk.unregistered, 0);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 2);
	CHECK_UINT(fk.unregistered, 0);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);

	CASE("retarget: a sticky that did not move sends nothing");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	reply(401, NULL, chal_2069());
	reply(200, NULL, "Expires: 3600\r\n");
	sends = fk.sends;
	/* the set moved, the selection stayed on .10 */
	sip_reg_targets_reload(&reg);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.12", 5060);
	CHECK(sip_reg_targets_reload_end(&reg));
	sip_reg_retarget(&reg);
	CHECK_UINT(fk.sends, sends);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	/* an emptied list is refused too */
	reload_answer(NULL, NULL);
	sip_reg_retarget(&reg);
	CHECK_UINT(fk.sends, sends);

	CASE("retarget: a REGISTER in flight is not dropped, the refresh waits");
	register_ok();
	fake_advance(1800 * 1000);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.10"));
	sends = fk.sends;
	reload_answer("192.0.2.20", NULL);
	sip_reg_retarget(&reg);
	CHECK_UINT(fk.sends, sends);
	CHECK(reg.retarget_pending);

	CASE("retarget: the deferred refresh follows the 200 of the exchange");
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 2);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.20"));
	CHECK(!reg.retarget_pending);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 3);
	CHECK_UINT(fk.unregistered, 0);

	CASE("retarget: the credential resend goes to the attempt's server");
	register_ok();
	fake_advance(1800 * 1000);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.10"));
	reload_answer("192.0.2.20", NULL);
	sip_reg_retarget(&reg);
	reply(401, NULL, chal_nonce("fresh-after-reload", ",stale=true"));
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.10"));
	CHECK(reg.retarget_pending);

	CASE("retarget: the challenge finishes first and the retarget follows");
	sends = fk.sends;
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.20"));
	CHECK(!reg.retarget_pending);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.unregistered, 0);

	CASE("retarget: a failed deferred exchange keeps its retry delay");
	register_ok();
	fake_advance(1800 * 1000);
	reload_answer("192.0.2.20", NULL);
	sip_reg_retarget(&reg);
	CHECK(reg.retarget_pending);
	sends = fk.sends;
	reply(500, NULL, NULL);
	CHECK_UINT(fk.sends, sends);
	CHECK(!reg.retarget_pending);
	CHECK_UINT(fk.last_retry, 5);
	/* to the retry exactly: one step further fires the retransmission */
	fake_advance(5 * 1000);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.20"));

	CASE("retarget: a 503 with Retry-After keeps the server's delay");
	register_ok();
	fake_advance(1800 * 1000);
	reply(503, NULL, "Retry-After: 120\r\n");
	CHECK_UINT(fk.last_retry, 120);
	sends = fk.sends;
	reload_answer("192.0.2.20", NULL);
	sip_reg_retarget(&reg);
	CHECK_UINT(fk.sends, sends);
	fake_advance(119 * 1000);
	CHECK_UINT(fk.sends, sends);
	fake_advance(1 * 1000);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.20"));

	CASE("retarget: a credential backoff is never cut short");
	register_ok();
	fake_advance(1800 * 1000);
	reply(401, NULL, chal_nonce(R_NONCE, NULL));
	reply(401, NULL, chal_nonce(R_NONCE, NULL));
	CHECK_UINT(fk.last_retry, SIP_REG_BACKOFF_AUTH);
	sends = fk.sends;
	reload_answer("192.0.2.20", NULL);
	sip_reg_retarget(&reg);
	CHECK_UINT(fk.sends, sends);
	fake_advance(SIP_REG_BACKOFF_AUTH * 1000);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.20"));

	CASE("retarget: during plain backoff it sends now and keeps the ladder");
	register_ok();
	fake_advance(1800 * 1000);
	reply(500, NULL, NULL);
	CHECK_UINT(fk.last_retry, 5);
	sends = fk.sends;
	reload_answer("192.0.2.20", NULL);
	sip_reg_retarget(&reg);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.20"));
	reply(500, NULL, NULL);
	CHECK_UINT(fk.last_retry, 10);	/* the ladder continued doubling */

	CASE("retarget: a stop forgets the deferred refresh");
	register_ok();
	fake_advance(1800 * 1000);
	reload_answer("192.0.2.20", NULL);
	sip_reg_retarget(&reg);
	CHECK(reg.retarget_pending);
	sip_reg_stop(&reg, false);
	CHECK(!reg.retarget_pending);

	CASE("retarget: an unregister in progress refuses it");
	register_ok();
	sip_reg_stop(&reg, true);
	CHECK(reg.unregistering);
	sends = fk.sends;
	reload_answer("192.0.2.20", NULL);
	sip_reg_retarget(&reg);
	CHECK(!reg.retarget_pending);
	reply(200, NULL, NULL);
	CHECK_UINT(fk.unregistered, 1);
	CHECK_UINT(fk.sends, sends);
}

/* ----------------------------------------------------------- server move */

/*
 * A REGISTER toward a server other than the one that holds the binding. The
 * binding stays live until the new server grants one, so a failed move costs
 * the line nothing; a successful one takes the old binding away when
 * cfg.move_unregister says so.
 *
 * The frame every case starts from: registered at .10 after a challenge, with
 * .11 in the list and the priority swapped so the selection sits on .11. The
 * move is one sip_reg_retarget() away, and fk.sends is 2.
 */
static void move_ready(void)
{
	fake_reset();
	sip_reg_target_add_prio(&reg, "192.0.2.10", 5060, 10);
	sip_reg_target_add_prio(&reg, "192.0.2.11", 5060, 20);
	sip_reg_start(&reg);
	reply(401, NULL, chal_2069());
	reply(200, NULL, "Expires: 3600\r\n");
	reload_answer_prio("192.0.2.10", 20, "192.0.2.11", 10);
}

static void move_tests(void)
{
	static char move_host[SIP_REG_HOST_MAX + 1] = R_LOCAL_HOST;
	const struct sip_reg_target *t;
	struct req_info ri;
	char call_id[128];
	char contact[192];
	char slot[SIP_REG_URI_MAX];
	unsigned int sends;
	uint64_t due;

	cfg.move_unregister = true;

	CASE("move: the REGISTER to the new server goes first and the binding "
	     "stays");
	move_ready();
	CHECK_UINT(fk.sends, 2);
	CHECK(last_req(&ri));
	snprintf(call_id, sizeof(call_id), "%s", ri.call_id);
	sip_reg_retarget(&reg);
	CHECK_UINT(fk.sends, 3);
	CHECK(!strcmp(fk.send[2].host, "192.0.2.11"));
	CHECK(reg.moving);
	CHECK_UINT(sip_reg_expires(&reg), 3600);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REFRESHING);
	CHECK_UINT(fk.unregistered, 0);

	CASE("move: the 2xx moves the binding and sends Expires 0 to the old "
	     "server on the same Call-ID, CSeq grows by two");
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.sends, 4);
	CHECK(!strcmp(fk.send[3].host, "192.0.2.10"));
	CHECK(req_info_at(3, &ri));
	CHECK_UINT(ri.expires, 0);
	CHECK(!strcmp(ri.call_id, call_id));
	CHECK_UINT(ri.cseq, 4);
	CHECK(reg.removing);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	CHECK_UINT(fk.unregistered, 0);

	CASE("move: no unregistered report, the state reads REGISTERED "
	     "throughout");
	reply(200, NULL, NULL);
	CHECK(!reg.removing);
	CHECK_UINT(fk.unregistered, 0);
	CHECK_UINT(fk.failed, 0);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	CHECK_UINT(sip_reg_expires(&reg), 3600);
	CHECK(strstr(fk.trace, "old binding removed") != NULL);

	CASE("move: a timeout at the new server rolls back: penalized, "
	     "selection back, no report, state REGISTERED, refresh re-armed "
	     "where it was");
	move_ready();
	due = reg.refresh_due_ms;
	sip_reg_retarget(&reg);
	fake_advance(32 * 1000 + 100);
	CHECK_UINT(fk.failed, 0);
	CHECK_UINT(fk.unregistered, 0);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	CHECK_UINT(sip_reg_expires(&reg), 3600);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	CHECK(target_host_is(0, "192.0.2.10"));
	t = sip_reg_target_get(&reg, 1);
	CHECK(t && t->failures == 1 && t->penalty_until > fk.now);
	CHECK_UINT(sip_reg_timer_kind(&reg), SIP_REG_T_REFRESH);
	CHECK(reg.timer.due_ms <= due && due - reg.timer.due_ms < 1000);
	CHECK_UINT(reg.next_retry_s, 0);

	/*
	 * The owner may have followed the new server, with a socket or an
	 * address family of its own, so the rollback tells it once. It is no
	 * failure report: the registration never went down.
	 */
	CASE("move: a rollback reports itself once");
	CHECK_UINT(fk.rolled_back, 1);
	CHECK_UINT(fk.failed, 0);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	move_ready();
	CHECK_UINT(fk.rolled_back, 0);
	sip_reg_retarget(&reg);
	reply(500, NULL, NULL);
	CHECK_UINT(fk.rolled_back, 1);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	/* an ordinary failure reports nothing of the kind */
	move_ready();
	sip_reg_retarget(&reg);
	reload_answer("192.0.2.11", "192.0.2.12");
	reply(500, NULL, NULL);
	CHECK_UINT(fk.rolled_back, 0);
	CHECK_UINT(fk.failed, 1);

	/*
	 * A 200 that grants no expiry is the registrar refusing the binding,
	 * so the move did not happen: the line goes back to the server that
	 * still holds one instead of reporting itself down.
	 */
	CASE("move: a 2xx that grants no expiry rolls back");
	move_ready();
	sip_reg_retarget(&reg);
	reply(200, NULL, "Expires: 0\r\n");
	CHECK_UINT(fk.unregistered, 0);
	CHECK_UINT(fk.failed, 0);
	CHECK_UINT(fk.rolled_back, 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	CHECK_UINT(sip_reg_expires(&reg), 3600);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	CHECK(target_host_is(0, "192.0.2.10"));
	CHECK_UINT(sip_reg_timer_kind(&reg), SIP_REG_T_REFRESH);
	/* with the old server gone it is an ordinary refusal again */
	move_ready();
	sip_reg_retarget(&reg);
	reload_answer("192.0.2.11", "192.0.2.12");
	reply(200, NULL, "Expires: 0\r\n");
	CHECK_UINT(fk.unregistered, 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_BACKOFF);

	CASE("move: a 403 at the new server rolls back and resets the "
	     "credentials");
	move_ready();
	CHECK(sip_auth_state_valid(&reg.auth[SIP_AUTH_WWW]));
	sip_reg_retarget(&reg);
	reply(403, NULL, NULL);
	CHECK_UINT(fk.failed, 0);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	CHECK(!sip_auth_state_valid(&reg.auth[SIP_AUTH_WWW]));
	CHECK(!sip_auth_state_valid(&reg.auth[SIP_AUTH_PROXY]));

	/* the delay names the new server, and the line is not waiting on it */
	CASE("move: a 503 with Retry-After at the new server rolls back "
	     "without waiting");
	move_ready();
	sip_reg_retarget(&reg);
	reply(503, NULL, "Retry-After: 600\r\n");
	CHECK_UINT(fk.failed, 0);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	CHECK_UINT(sip_reg_timer_kind(&reg), SIP_REG_T_REFRESH);
	CHECK(!reg.retry_fixed);
	CHECK_UINT(reg.next_retry_s, 0);

	CASE("move: a challenge at the new server is answered there");
	move_ready();
	sip_reg_retarget(&reg);
	sends = fk.sends;
	reply(401, NULL, chal_nonce("7b1d3e5f7a9c1e3b5d7f9a1c3e5b7d9f", NULL));
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.11"));
	CHECK(last_req(&ri));
	CHECK(ri.auth);
	CHECK(reg.moving);

	CASE("move: a received from the new server moves the Contact as any "
	     "refresh does");
	move_ready();
	sip_reg_retarget(&reg);
	reply(200, ";received=203.0.113.9", "Expires: 3600\r\n");
	CHECK(strstr(sip_reg_contact(&reg), "203.0.113.9") != NULL);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REFRESHING);
	CHECK(!reg.removing);

	/*
	 * The new server has granted nothing yet, so its mapping belongs to no
	 * binding. Taking it would move the Contact of the binding the old
	 * server still holds, and the rollback would leave it there.
	 */
	CASE("move: a received in the challenge of a move leaves the Contact "
	     "alone");
	move_ready();
	snprintf(contact, sizeof(contact), "%s", sip_reg_contact(&reg));
	sip_reg_retarget(&reg);
	reply(401, ";received=203.0.113.9;rport=40000",
	      chal_nonce("4c6e8a0b2d4f6a8c0e2b4d6f8a0c2e4b", NULL));
	CHECK(!strcmp(sip_reg_contact(&reg), contact));
	CHECK(last_req(&ri));
	CHECK(ri.auth);
	CHECK(!strcmp(ri.contact, contact));
	reply(403, NULL, NULL);
	CHECK(!strcmp(sip_reg_contact(&reg), contact));
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	CHECK_UINT(sip_reg_target_current(&reg), 0);

	/*
	 * A registrar holds one binding per Contact. The old server was granted
	 * the URI the line carried then, so the Expires 0 has to name that one
	 * and not the URI a move since gave the line.
	 */
	CASE("move: the removal names the Contact the old binding was granted "
	     "with");
	cfg.local_host = move_host;
	snprintf(move_host, sizeof(move_host), R_LOCAL_HOST);
	move_ready();
	snprintf(contact, sizeof(contact), "%s", sip_reg_contact(&reg));
	snprintf(move_host, sizeof(move_host), "10.0.0.6");
	CHECK(sip_reg_local_host_set(&reg));
	CHECK(strcmp(sip_reg_contact(&reg), contact) != 0);
	sip_reg_retarget(&reg);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK(reg.removing);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.10"));
	CHECK(last_req(&ri));
	CHECK_UINT(ri.expires, 0);
	CHECK(has("@10.0.0.6:5060>"));
	CHECK(has("@10.0.0.5:5060>;expires=0"));
	/* and the slot the removal filled is empty again once it ends */
	reply(200, NULL, NULL);
	CHECK(!reg.removing);
	CHECK(!reg.old_contact_uri[0]);
	/* cfg_default() memsets the shared config, the option with it */
	cfg_default();
	cfg.move_unregister = true;

	/*
	 * The correction of a Contact the answer moved belongs to the server
	 * that just granted the binding, and the removal the move owes waits
	 * for the clean answer that ends the chase.
	 */
	CASE("move: a dirty 2xx corrects the Contact at the new server, then "
	     "removes the old binding");
	move_ready();
	snprintf(contact, sizeof(contact), "%s", sip_reg_contact(&reg));
	sip_reg_retarget(&reg);
	sends = fk.sends;
	reply(200, ";received=203.0.113.9", "Expires: 3600\r\n");
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.11"));
	CHECK(!reg.moving);
	CHECK(!reg.removing);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK(reg.removing);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.10"));
	CHECK(last_req(&ri));
	CHECK_UINT(ri.expires, 0);
	CHECK(has("@203.0.113.9:"));
	CHECK(has(contact));

	CASE("move: the correction goes to the binding's server, not a "
	     "re-selected old one");
	move_ready();
	sip_reg_retarget(&reg);
	reload_answer_prio("192.0.2.10", 10, "192.0.2.11", 20);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	sends = fk.sends;
	reply(200, ";received=203.0.113.9", "Expires: 3600\r\n");
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.11"));
	CHECK(!reg.moving);

	/*
	 * The rollback puts the selection back on the server the attempt ran
	 * against, which is what the retarget asked for. Sending again would
	 * drop the refresh the rollback restored.
	 */
	CASE("move: a rollback satisfies a pending retarget");
	move_ready();
	sip_reg_retarget(&reg);
	reload_answer_prio("192.0.2.10", 10, "192.0.2.11", 20);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	sip_reg_retarget(&reg);
	CHECK(reg.retarget_pending);
	sends = fk.sends;
	reply(500, NULL, NULL);
	CHECK(!reg.retarget_pending);
	CHECK_UINT(fk.sends, sends);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	CHECK_UINT(sip_reg_timer_kind(&reg), SIP_REG_T_REFRESH);
	CHECK_UINT(sip_reg_target_current(&reg), 0);

	CASE("move: an emptied list during a move clears the move flag");
	move_ready();
	sip_reg_retarget(&reg);
	CHECK(reg.moving);
	reload_answer(NULL, NULL);
	sip_reg_refresh_now(&reg);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_RESOLVING);
	CHECK(!reg.moving);
	CHECK(!reg.removing);

	CASE("move: the old server gone from the answer makes a failed move an "
	     "ordinary failure");
	move_ready();
	sip_reg_retarget(&reg);
	reload_answer("192.0.2.11", "192.0.2.12");
	reply(500, NULL, NULL);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 500);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_BACKOFF);
	CHECK_UINT(sip_reg_expires(&reg), 0);
	CHECK(!reg.moving);

	CASE("move: the removal's timeout penalizes the old server without "
	     "advancing and reports nothing");
	move_ready();
	sip_reg_retarget(&reg);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK(reg.removing);
	sends = sip_reg_target_current(&reg);
	fake_advance(32 * 1000 + 100);
	CHECK(!reg.removing);
	CHECK_UINT(fk.failed, 0);
	CHECK_UINT(fk.unregistered, 0);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	CHECK_UINT(sip_reg_target_current(&reg), sends);
	t = sip_reg_target_get(&reg, 0);
	CHECK(t && t->failures == 1 && t->penalty_until > fk.now);

	CASE("move: a 423 on the removal ends it");
	move_ready();
	sip_reg_retarget(&reg);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK(reg.removing);
	sends = fk.sends;
	reply(423, NULL, "Min-Expires: 7200\r\n");
	CHECK_UINT(fk.sends, sends);
	CHECK(!reg.removing);
	CHECK_UINT(fk.failed, 0);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);

	CASE("move: the removal keeps its Contact under a received");
	move_ready();
	sip_reg_retarget(&reg);
	reply(200, NULL, "Expires: 3600\r\n");
	snprintf(contact, sizeof(contact), "%s", sip_reg_contact(&reg));
	reply(401, ";received=203.0.113.9;rport=40000",
	      chal_nonce("9d7f5b3a1c9e7d5b3f1a9c7e5d3b1f9a", NULL));
	CHECK(!strcmp(sip_reg_contact(&reg), contact));
	CHECK(last_req(&ri));
	CHECK_UINT(ri.expires, 0);
	CHECK(!strcmp(ri.contact, contact));
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.10"));

	/* the answer of a removal runs none of the binding logic, so the slot
	 * stands exactly as the exchange found it */
	CASE("move: the removal leaves the removal slot alone");
	cfg.local_host = move_host;
	snprintf(move_host, sizeof(move_host), R_LOCAL_HOST);
	move_ready();
	sip_reg_retarget(&reg);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK(reg.removing);
	snprintf(move_host, sizeof(move_host), "10.0.0.6");
	CHECK(sip_reg_local_host_set(&reg));
	snprintf(slot, sizeof(slot), "%s", reg.old_contact_uri);
	CHECK(slot[0] != 0);
	reply(200, NULL, NULL);
	CHECK(!reg.removing);
	CHECK(!strcmp(reg.old_contact_uri, slot));
	cfg_default();
	cfg.move_unregister = true;

	CASE("move: the option off skips the removal, the rollback still "
	     "applies");
	cfg.move_unregister = false;
	move_ready();
	sip_reg_retarget(&reg);
	sends = fk.sends;
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.sends, sends);
	CHECK(!reg.removing);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	CHECK(target_host_is(1, "192.0.2.11"));
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	move_ready();
	sip_reg_retarget(&reg);
	reply(500, NULL, NULL);
	CHECK_UINT(fk.failed, 0);
	CHECK_UINT(sip_reg_target_current(&reg), 0);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	cfg.move_unregister = true;

	CASE("move: stop with unregister during the move sends the Expires 0 "
	     "to the binding's server");
	move_ready();
	sip_reg_retarget(&reg);
	sip_reg_stop(&reg, true);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.10"));
	CHECK(last_req(&ri));
	CHECK_UINT(ri.expires, 0);
	reply(200, NULL, "Expires: 0\r\n");
	CHECK_UINT(fk.unregistered, 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_UNREGISTERED);

	CASE("move: stop during the removal drops it and unregisters at the "
	     "new server");
	move_ready();
	sip_reg_retarget(&reg);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK(reg.removing);
	sip_reg_stop(&reg, true);
	CHECK(!reg.removing);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.11"));
	CHECK(last_req(&ri));
	CHECK_UINT(ri.expires, 0);

	/*
	 * The refresh timer is stopped for the length of an exchange, so the
	 * attempt that follows a move under a live binding is started here the
	 * way the timer starts one. It reads the same selection and is a move
	 * again.
	 */
	CASE("move: a refresh during a move is a move again");
	move_ready();
	sip_reg_retarget(&reg);
	CHECK(reg.moving);
	sends = fk.sends;
	sip_reg_refresh_now(&reg);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.11"));
	CHECK(reg.moving);
	CHECK_UINT(sip_reg_expires(&reg), 3600);

	CASE("move: a refresh during the removal drops it");
	move_ready();
	sip_reg_retarget(&reg);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK(reg.removing);
	sends = fk.sends;
	sip_reg_refresh_now(&reg);
	CHECK(!reg.removing);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.11"));

	/* mode 0: the answer re-points the selection and sends nothing, so the
	 * refresh timer is what starts the move */
	CASE("move: the refresh timer under a re-pointed selection moves at "
	     "the refresh");
	move_ready();
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	sends = fk.sends;
	fake_advance(1800 * 1000);
	CHECK(fk.sends > sends);
	CHECK(!strcmp(fk.send[sends].host, "192.0.2.11"));
	CHECK(reg.moving);
	CHECK_UINT(sip_reg_expires(&reg), 3600);

	/* the old binding has no transport left in the family the line gave
	 * up, so it lapses on its own */
	CASE("move: a target of the other family skips the removal");
	fake_reset();
	sip_reg_target_add_prio(&reg, "2001:db8::10", 5060, 10);
	sip_reg_target_add_prio(&reg, "192.0.2.11", 5060, 20);
	sip_reg_start(&reg);
	reply(200, NULL, "Expires: 3600\r\n");
	reload_answer_prio("2001:db8::10", 20, "192.0.2.11", 10);
	sip_reg_retarget(&reg);
	sends = fk.sends;
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.sends, sends);
	CHECK(!reg.removing);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);

	CASE("move: no binding, no move flags");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	CHECK(!reg.moving && !reg.removing);
	reply(500, NULL, NULL);
	CHECK_UINT(fk.failed, 1);
	CHECK(!reg.moving && !reg.removing);
	fake_advance(6 * 1000);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.11"));
	CHECK(!reg.moving && !reg.removing);

	cfg_default();
	fake_reset();
}

/* ------------------------------------------- service route (RFC 3608) */

/* the granted route as the registrar sends it, and as reg.c renders it */
#define SR_HDRS		"Service-Route: <sip:p1.example.com;lr>\r\n" \
			"Service-Route: <sip:p2.example.com;lr>\r\n"
#define SR_TEXT		"Route: <sip:p1.example.com;lr>\r\n" \
			"Route: <sip:p2.example.com;lr>\r\n"

/* register once, ending in SIP_REG_REGISTERED with the two hop service route */
static void register_sr(void)
{
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, NULL, chal_2069());
	reply(200, NULL, "Expires: 3600\r\n" SR_HDRS);
}

static void service_route_tests(void)
{
	const struct sip_route_hdrs *sr;
	struct req_info ri;
	unsigned int i;

	CASE("service route: the 200 stores the hops in receive order");
	register_sr();
	sr = sip_reg_service_route(&reg);
	CHECK(sr != NULL);
	CHECK_UINT(sr->hops, 2);
	CHECK(!strcmp(sr->text, SR_TEXT));
	CHECK_UINT(sr->len, strlen(SR_TEXT));
	CHECK(strstr(fk.trace, "service route stored, 2 hops") != NULL);

	CASE("service route: a later 200 replaces the stored hops");
	fake_advance(1800 * 1000);
	reply(200, NULL, "Expires: 3600\r\n"
	      "Service-Route: <sip:p9.example.com;lr>\r\n");
	CHECK_UINT(fk.registered, 2);
	CHECK_UINT(sr->hops, 1);
	CHECK(!strcmp(sr->text, "Route: <sip:p9.example.com;lr>\r\n"));

	CASE("service route: a 200 without one clears it (RFC 3608 6.1)");
	fake_advance(1800 * 1000);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 3);
	CHECK(sip_reg_service_route(&reg) != NULL);
	CHECK_UINT(sr->hops, 0);
	CHECK_UINT(sr->len, 0);

	/* The registrar took the binding away, so the route it granted with
	 * that binding goes too and the route of this answer is no one's. */
	CASE("service route: a 200 with expiry 0 stores nothing and clears it");
	register_sr();
	fake_advance(1800 * 1000);
	reply(200, NULL, "Expires: 0\r\n"
	      "Service-Route: <sip:zero.example.com;lr>\r\n");
	CHECK_UINT(fk.unregistered, 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_BACKOFF);
	CHECK_UINT(sip_reg_service_route(&reg)->hops, 0);
	CHECK_UINT(sip_reg_service_route(&reg)->len, 0);

	CASE("service route: the refresh REGISTER carries no Route header");
	register_sr();
	fake_advance(1800 * 1000);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "REGISTER"));
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REFRESHING);
	CHECK(strstr(last_text(), "\r\nRoute:") == NULL);
	CHECK_UINT(sip_reg_service_route(&reg)->hops, 2);

	CASE("service route: the unregister REGISTER carries no Route and the "
	     "stop clears it");
	register_sr();
	sip_reg_stop(&reg, true);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.method, "REGISTER"));
	CHECK_UINT(ri.expires, 0);
	CHECK(strstr(last_text(), "\r\nRoute:") == NULL);
	CHECK_UINT(sip_reg_service_route(&reg)->hops, 2);
	reply(200, NULL, "Expires: 0\r\n");
	CHECK_UINT(fk.unregistered, 1);
	CHECK_UINT(sip_reg_service_route(&reg)->hops, 0);

	CASE("service route: a binding drop clears it");
	register_sr();
	fake_advance(1800 * 1000);
	reply(500, NULL, NULL);
	CHECK_UINT(fk.failed, 1);
	CHECK(sip_reg_binding_dst(&reg) == NULL);
	CHECK_UINT(sip_reg_service_route(&reg)->hops, 0);

	CASE("service route: the capped Contact chase clears it");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, ";received=203.0.113.9;rport=50001",
	      "Expires: 3600\r\n" SR_HDRS);
	CHECK_UINT(sip_reg_service_route(&reg)->hops, 2);
	for (i = 1; i <= SIP_REG_REBIND_MAX; i++) {
		char via[64];

		snprintf(via, sizeof(via), ";received=203.0.113.9;rport=%u",
			 50001 + i);
		reply(200, via, "Expires: 3600\r\n" SR_HDRS);
	}
	CHECK_UINT(reg.stat.rebind_capped, 1);
	CHECK_UINT(sip_reg_service_route(&reg)->hops, 0);
	CHECK_UINT(sip_reg_service_route(&reg)->len, 0);

	CASE("service route: a challenge leaves the stored hops alone");
	register_sr();
	fake_advance(1800 * 1000);
	reply(401, NULL, chal_nonce("another-nonce", NULL));
	CHECK(last_req(&ri));
	CHECK(ri.auth);
	CHECK_UINT(sip_reg_service_route(&reg)->hops, 2);
	CHECK(!strcmp(sip_reg_service_route(&reg)->text, SR_TEXT));

	CASE("service route: eleven hops overflow and store nothing");
	register_sr();
	fake_advance(1800 * 1000);
	fk.trace[0] = 0;
	reply(200, NULL, "Expires: 3600\r\n"
	      "Service-Route: <sip:h1.example.com;lr>,<sip:h2.example.com;lr>,"
	      "<sip:h3.example.com;lr>,<sip:h4.example.com;lr>,"
	      "<sip:h5.example.com;lr>,<sip:h6.example.com;lr>,"
	      "<sip:h7.example.com;lr>,<sip:h8.example.com;lr>,"
	      "<sip:h9.example.com;lr>,<sip:h10.example.com;lr>,"
	      "<sip:h11.example.com;lr>\r\n");
	CHECK_UINT(fk.registered, 2);
	CHECK_UINT(sip_reg_service_route(&reg)->hops, 0);
	CHECK_UINT(sip_reg_service_route(&reg)->len, 0);
	CHECK(strstr(fk.trace, "service route refused") != NULL);

	/* The removal runs against the server the binding left, and its answer
	 * belongs to a binding that is gone. The live route is the new
	 * server's and this 200 does not touch it. */
	CASE("service route: the 200 of a removal leaves the route of the new "
	     "binding alone");
	cfg.move_unregister = true;
	move_ready();
	sip_reg_retarget(&reg);
	CHECK(reg.moving);
	reply(200, NULL, "Expires: 3600\r\n" SR_HDRS);
	CHECK(reg.removing);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.10"));
	CHECK(!strcmp(sip_reg_service_route(&reg)->text, SR_TEXT));
	reply(200, NULL, "Service-Route: <sip:old.example.com;lr>\r\n");
	CHECK(!reg.removing);
	CHECK_UINT(sip_reg_service_route(&reg)->hops, 2);
	CHECK(!strcmp(sip_reg_service_route(&reg)->text, SR_TEXT));
	cfg.move_unregister = false;

	CASE("service route: a done registration reports none");
	register_sr();
	CHECK_UINT(sip_reg_service_route(&reg)->hops, 2);
	sip_reg_done(&reg);
	sr = sip_reg_service_route(&reg);
	CHECK(sr != NULL);
	CHECK_UINT(sr->hops, 0);
	CHECK_UINT(sr->len, 0);
	CHECK_UINT(sr->text[0], 0);
	cfg_default();
	fake_reset();
}

/* ------------------------------------------------------ failure handling */

static void failure_tests(void)
{
	CASE("the retry delay doubles per consecutive failure");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(500, NULL, NULL);
	CHECK_UINT(fk.last_retry, 5);
	fake_advance(6 * 1000);
	reply(500, NULL, NULL);
	CHECK_UINT(fk.last_retry, 10);
	fake_advance(11 * 1000);
	reply(500, NULL, NULL);
	CHECK_UINT(fk.last_retry, 20);
	fake_advance(21 * 1000);
	reply(500, NULL, NULL);
	CHECK_UINT(fk.last_retry, 40);

	CASE("a success resets the ladder");
	fake_advance(41 * 1000);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 1);
	fake_advance(1800 * 1000);
	reply(500, NULL, NULL);
	CHECK_UINT(fk.last_retry, 5);

	CASE("503 with Retry-After schedules exactly that delay");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(503, NULL, "Retry-After: 120\r\n");
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 503);
	CHECK_UINT(fk.last_retry, 120);
	fake_advance(119 * 1000);
	CHECK_UINT(fk.sends, 1);
	fake_advance(1000);
	CHECK_UINT(fk.sends, 2);

	CASE("an absurd Retry-After is clamped instead of wrapping the timer");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(503, NULL, "Retry-After: 4294967\r\n");
	CHECK_UINT(fk.last_retry, SIP_REG_DELAY_MAX);
	fake_advance(3600 * 1000);
	CHECK_UINT(fk.sends, 1);
	fake_advance((uint64_t)(SIP_REG_DELAY_MAX - 3600) * 1000);
	CHECK_UINT(fk.sends, 2);

	CASE("503 without Retry-After uses the ordinary backoff");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(503, NULL, NULL);
	CHECK_UINT(fk.last_retry, 5);

	CASE("a transport failure fails the attempt");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	fk.send_fail = true;
	sip_reg_start(&reg);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 503);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_BACKOFF);

	CASE("a 404 from the registrar is reported with its own code");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(404, NULL, NULL);
	CHECK_UINT(fk.last_code, 404);
	CHECK_UINT(fk.last_retry, 5);

	CASE("a 6xx is reported like any other refusal");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(603, NULL, NULL);
	CHECK_UINT(fk.last_code, 603);

	CASE("the binding is reported gone when a refresh fails");
	register_ok();
	CHECK_UINT(sip_reg_expires(&reg), 3600);
	fake_advance(1800 * 1000);
	reply(500, NULL, NULL);
	CHECK_UINT(sip_reg_expires(&reg), 0);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_BACKOFF);
}

/* ------------------------------------------------------- retry on recovery */

/*
 * The local transport came back after a WAN flap. The account layer rebound
 * the socket, and asks each line in its failure backoff to try now.
 * fk.send_fail stands for the down socket, because a send through one fails
 * exactly this way. The ladder must keep its value, and a delay that is not
 * ours must stand.
 */
static void retry_now_tests(void)
{
	unsigned int sends;

	CASE("retry now: a socket that came back sends the attempt the ladder was holding");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	fk.send_fail = true;
	sip_reg_start(&reg);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_BACKOFF);
	CHECK_UINT(fk.last_retry, 5);
	CHECK_UINT(fk.sends, 0);
	fk.send_fail = false;
	CHECK(sip_reg_retry_now(&reg));
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_TRYING);

	CASE("retry now: the failure run is not reset, so the next delay is still longer");
	reply(500, NULL, NULL);
	CHECK_UINT(fk.last_retry, 10);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_BACKOFF);
	/* and the attempt that delay holds runs when it ends, not before */
	sends = fk.sends;
	fake_advance(9 * 1000);
	CHECK_UINT(fk.sends, sends);
	fake_advance(1000);
	CHECK_UINT(fk.sends, sends + 1);

	CASE("retry now: the attempt goes to the target the timer fire would take");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_target_add(&reg, "192.0.2.11", 5060);
	sip_reg_start(&reg);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.10"));
	reply(500, NULL, NULL);
	/* the failure penalized .10 and the selection moved to .11 */
	CHECK_UINT(sip_reg_target_current(&reg), 1);
	sends = fk.sends;
	CHECK(sip_reg_retry_now(&reg));
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.11"));
	CHECK_UINT(sip_reg_target_current(&reg), 1);

	CASE("retry now: a send that fails again earns the next delay of the ladder");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	fk.send_fail = true;
	sip_reg_start(&reg);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_retry, 5);
	/* the socket is still down, so the failure path runs inside the call */
	CHECK(sip_reg_retry_now(&reg));
	CHECK_UINT(fk.failed, 2);
	CHECK_UINT(fk.last_retry, 10);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_BACKOFF);
	CHECK_UINT(fk.sends, 0);

	CASE("retry now: a Retry-After delay the server named still stands");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(503, NULL, "Retry-After: 120\r\n");
	CHECK_UINT(fk.last_retry, 120);
	sends = fk.sends;
	CHECK(!sip_reg_retry_now(&reg));
	CHECK_UINT(fk.sends, sends);
	fake_advance(119 * 1000);
	CHECK_UINT(fk.sends, sends);
	fake_advance(1000);
	CHECK_UINT(fk.sends, sends + 1);

	CASE("retry now: the credential backoff still stands");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, NULL, chal_nonce(R_NONCE, NULL));
	reply(401, NULL, chal_nonce(R_NONCE, NULL));
	CHECK_UINT(fk.last_retry, SIP_REG_BACKOFF_AUTH);
	sends = fk.sends;
	CHECK(!sip_reg_retry_now(&reg));
	CHECK_UINT(fk.sends, sends);
	fake_advance((SIP_REG_BACKOFF_AUTH - 1) * 1000);
	CHECK_UINT(fk.sends, sends);
	fake_advance(1000);
	CHECK_UINT(fk.sends, sends + 1);

	CASE("retry now: a registered line, a stopped one and an unregister in "
	     "progress do nothing");
	register_ok();
	sends = fk.sends;
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	CHECK(!sip_reg_retry_now(&reg));
	CHECK_UINT(fk.sends, sends);
	sip_reg_stop(&reg, false);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_UNREGISTERED);
	CHECK(!sip_reg_retry_now(&reg));
	CHECK_UINT(fk.sends, sends);
	register_ok();
	sip_reg_stop(&reg, true);
	CHECK(reg.unregistering);
	sends = fk.sends;
	CHECK(!sip_reg_retry_now(&reg));
	CHECK_UINT(fk.sends, sends);

	CASE("retry now: a line with no target waits for the resolver instead");
	fake_reset();
	sip_reg_start(&reg);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_RESOLVING);
	CHECK(!sip_reg_retry_now(&reg));
	CHECK_UINT(fk.sends, 0);
	/* the same answer for a backoff whose target list a reload emptied */
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	fk.send_fail = true;
	sip_reg_start(&reg);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_BACKOFF);
	fk.send_fail = false;
	sip_reg_targets_clear(&reg);
	CHECK(!sip_reg_retry_now(&reg));
	CHECK_UINT(fk.sends, 0);
	/* the armed retry stands, and its fire finds the empty list */
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_BACKOFF);
	fake_advance(4 * 1000);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_BACKOFF);
	fake_advance(1000);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_RESOLVING);
	CHECK_UINT(fk.sends, 0);
}

/* ------------------------------------------------------------ stop paths */

static void stop_tests(void)
{
	struct req_info ri;
	char call_id[128];
	char contact[192];

	CASE("stop with unregister sends Expires 0 on the same Call-ID");
	register_ok();
	CHECK(last_req(&ri));
	snprintf(call_id, sizeof(call_id), "%s", ri.call_id);
	sip_reg_stop(&reg, true);
	CHECK_UINT(fk.sends, 3);
	CHECK(last_req(&ri));
	CHECK_UINT(ri.expires, 0);
	CHECK(!strcmp(ri.call_id, call_id));
	CHECK(ri.contact[0] != 0);
	CHECK_UINT(ri.cseq, 3);
	CHECK_UINT(fk.unregistered, 0);

	CASE("the 200 to the unregister reports the account down");
	reply(200, NULL, "Expires: 0\r\n");
	CHECK_UINT(fk.unregistered, 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_UNREGISTERED);
	CHECK_UINT(sip_reg_expires(&reg), 0);

	CASE("nothing is scheduled after the unregister");
	fake_advance(3600 * 1000);
	CHECK_UINT(fk.sends, 3);

	CASE("stop without unregister sends nothing");
	register_ok();
	CHECK_UINT(fk.sends, 2);
	sip_reg_stop(&reg, false);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(fk.unregistered, 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_UNREGISTERED);

	CASE("stop while nothing is registered completes at once");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	sip_reg_stop(&reg, true);
	CHECK_UINT(fk.sends, 1);
	CHECK_UINT(fk.unregistered, 1);

	CASE("an unregister that fails still reports the account down");
	register_ok();
	sip_reg_stop(&reg, true);
	reply(500, NULL, NULL);
	CHECK_UINT(fk.unregistered, 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_UNREGISTERED);
	fake_advance(600 * 1000);
	CHECK_UINT(fk.sends, 3);

	/*
	 * A 423 asks for a longer expiry, and the Expires 0 asks the registrar
	 * to take the binding away. A resend with Min-Expires would register
	 * again, so the 423 ends the exchange.
	 */
	CASE("stop: a 423 to the Expires 0 finishes the stop");
	register_ok();
	sip_reg_stop(&reg, true);
	CHECK_UINT(fk.sends, 3);
	CHECK(last_req(&ri));
	CHECK_UINT(ri.expires, 0);
	reply(423, NULL, "Min-Expires: 3600\r\n");
	CHECK_UINT(fk.sends, 3);
	CHECK_UINT(fk.unregistered, 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_UNREGISTERED);

	/*
	 * A received in the challenge names the mapping of the binding the
	 * registrar holds. Rebuilding the Contact from it would make the
	 * authenticated resend ask the registrar to remove a binding it never
	 * held, and the real one would stay.
	 */
	CASE("stop: a received in the challenge does not move the Contact of "
	     "the Expires 0");
	register_ok();
	snprintf(contact, sizeof(contact), "%s", sip_reg_contact(&reg));
	sip_reg_stop(&reg, true);
	reply(401, ";received=203.0.113.9;rport=40000",
	      chal_nonce("2f9c1a4b6d8e0f3a5c7b9d1e2f4a6b8c", NULL));
	CHECK_UINT(fk.sends, 4);
	CHECK(last_req(&ri));
	CHECK_UINT(ri.expires, 0);
	CHECK(ri.auth);
	CHECK(!strcmp(ri.contact, contact));
	CHECK(!strcmp(sip_reg_contact(&reg), contact));
	reply(200, NULL, "Expires: 0\r\n");
	CHECK_UINT(fk.unregistered, 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_UNREGISTERED);

	/*
	 * The Expires 0 of a stop goes to the server that holds the binding and
	 * names the URI that server was granted, which after a move is the one
	 * the line still carries.
	 */
	CASE("stop: an unregister after a move names the binding's Contact");
	cfg.move_unregister = true;
	move_ready();
	sip_reg_retarget(&reg);
	reply(200, NULL, "Expires: 3600\r\n");
	reply(200, NULL, NULL);
	CHECK(!reg.removing);
	snprintf(contact, sizeof(contact), "%s", sip_reg_contact(&reg));
	CHECK(!strcmp(reg.binding_contact, contact));
	sip_reg_stop(&reg, true);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.11"));
	CHECK(last_req(&ri));
	CHECK_UINT(ri.expires, 0);
	CHECK(!strcmp(ri.contact, contact));
	cfg_default();

	CASE("a stopped account starts again with the same identifiers");
	register_ok();
	CHECK(last_req(&ri));
	snprintf(call_id, sizeof(call_id), "%s", ri.call_id);
	sip_reg_stop(&reg, false);
	sip_reg_start(&reg);
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.call_id, call_id));
	CHECK_UINT(ri.cseq, 3);
	CHECK(!ri.auth);
}

/* -------------------------------------------------------------- lifetime */

static void lifetime_tests(void)
{
	CASE("an account without an address of record does not start");
	fake_reset();
	memset(&cfg, 0, sizeof(cfg));
	cfg.registrar = R_REGISTRAR;
	cfg.local_host = R_LOCAL_HOST;
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	CHECK_UINT(fk.sends, 0);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_UNREGISTERED);
	cfg_default();

	CASE("the defaults fill in for an unset expiry and retry");
	fake_reset();
	memset(&cfg, 0, sizeof(cfg));
	cfg.aor = R_AOR;
	cfg.registrar = R_REGISTRAR;
	cfg.local_host = R_LOCAL_HOST;
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
	CHECK_UINT(reg.cfg.expires, SIP_REG_EXPIRES_DEFAULT);
	CHECK_UINT(reg.cfg.retry_s, SIP_REG_RETRY_DEFAULT);
	CHECK_UINT(reg.cfg.penalty_s, SIP_REG_PENALTY_DEFAULT);
	CHECK_UINT(reg.cfg.local_port, 5060);
	cfg_default();

	CASE("a second start is refused while one is running");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	sip_reg_start(&reg);
	CHECK_UINT(fk.sends, 1);

	CASE("done stops every timer and detaches the transaction");
	register_ok();
	sip_reg_done(&reg);
	fake_advance(7200 * 1000);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_UNREGISTERED);

	CASE("the registration timers carry their own timer ids");
	register_ok();
	CHECK_UINT(reg.timer.id, SIP_TIMER_REG_REFRESH);
	CHECK(reg.timer.armed);
	CHECK(!strcmp(sip_timer_name(SIP_TIMER_REG_REFRESH), "reg-refresh"));
	fake_advance(1800 * 1000);
	reply(500, NULL, NULL);
	CHECK_UINT(reg.timer.id, SIP_TIMER_REG_RETRY);
	CHECK(reg.timer.armed);
	CHECK(!strcmp(sip_timer_name(SIP_TIMER_REG_RETRY), "reg-retry"));

	CASE("state names exist for every state");
	CHECK(!strcmp(sip_reg_state_name(SIP_REG_RESOLVING), "resolving"));
	CHECK(!strcmp(sip_reg_state_name(SIP_REG_REFRESHING), "refreshing"));
	CHECK(!strcmp(sip_reg_state_name(SIP_REG_BACKOFF), "backoff"));

	CASE("no timer was lost during the suite");
	CHECK_UINT(fake_timer_dropped, 0);
}

/* ---------------- challenge pairs, stop reentrancy, contact chase, IPv6 */

static void sol_tests(void)
{
	struct req_info ri;
	unsigned int i;

	/*
	 * A registrar that challenges first and a proxy in front of it that
	 * challenges next are two exchanges, not two attempts at one password.
	 */
	CASE("a 401 and then a 407 both complete instead of backing off");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, NULL, chal_2069());
	CHECK_UINT(fk.sends, 2);
	reply(407, NULL, "Proxy-Authenticate: Digest realm=\"proxy.example.net\","
			 "nonce=\"proxy-nonce\"\r\n");
	CHECK_UINT(fk.sends, 3);
	CHECK_UINT(fk.failed, 0);
	CHECK(last_req(&ri));
	CHECK(ri.auth && ri.proxy_auth);
	CHECK_UINT(ri.cseq, 3);

	CASE("the 200 that follows registers the account");
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 1);
	CHECK_UINT(fk.failed, 0);

	CASE("the proxy kind keeps its own single retry");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(407, NULL, "Proxy-Authenticate: Digest realm=\"p\",nonce=\"p1\"\r\n");
	CHECK_UINT(fk.sends, 2);
	reply(407, NULL, "Proxy-Authenticate: Digest realm=\"p\",nonce=\"p2\"\r\n");
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 407);

	CASE("a server challenge after a proxy one is still answered");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(407, NULL, "Proxy-Authenticate: Digest realm=\"p\",nonce=\"p1\"\r\n");
	CHECK_UINT(fk.sends, 2);
	reply(401, NULL, chal_2069());
	CHECK_UINT(fk.sends, 3);
	CHECK_UINT(fk.failed, 0);
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 1);

	/*
	 * sip_reg_stop() from inside unregistered() must not recurse.
	 */
	CASE("a stop from inside the unregistered callback reports once");
	register_ok();
	fk.stop_in_cb = true;
	sip_reg_stop(&reg, false);
	CHECK_UINT(fk.unregistered, 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_UNREGISTERED);

	CASE("the same holds on the unregister answer path");
	fk.stop_in_cb = false;
	register_ok();
	sip_reg_stop(&reg, true);
	fk.stop_in_cb = true;
	reply(200, NULL, "Expires: 0\r\n");
	CHECK_UINT(fk.unregistered, 1);

	CASE("and on the registrar that grants no expiry");
	fk.stop_in_cb = false;
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	fk.stop_in_cb = true;
	reply(200, NULL, "Expires: 0\r\n");
	CHECK_UINT(fk.unregistered, 1);
	fk.stop_in_cb = false;

	CASE("a stop of an account that is already stopped stays quiet");
	fake_reset();
	sip_reg_stop(&reg, true);
	CHECK_UINT(fk.unregistered, 0);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	sip_reg_stop(&reg, false);
	CHECK_UINT(fk.unregistered, 1);
	sip_reg_stop(&reg, false);
	CHECK_UINT(fk.unregistered, 1);

	/*
	 * A NAT that hands out a new source port per datagram would keep
	 * the account in a REGISTER loop with no delay between the attempts.
	 */
	CASE("a Contact that keeps moving stops the chase at the cap");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	for (i = 0; i <= SIP_REG_REBIND_MAX; i++) {
		char via[64];

		snprintf(via, sizeof(via), ";received=203.0.113.9;rport=%u", 50001 + i);
		reply(200, via, "Expires: 3600\r\n");
	}
	CHECK_UINT(fk.sends, SIP_REG_REBIND_MAX + 1);
	CHECK_UINT(fk.registered, 0);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 200);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_BACKOFF);
	CHECK_UINT(reg.stat.rebind_capped, 1);
	CHECK_UINT(reg.stat.contact_rebuilt, SIP_REG_REBIND_MAX + 1);

	CASE("the target that answered every time is not penalized");
	CHECK(sip_reg_target_get(&reg, 0) != NULL);
	CHECK_UINT(sip_reg_target_get(&reg, 0)->failures, 0);

	CASE("the retry timer takes over from there");
	CHECK_UINT(fk.last_retry, 5);
	fake_advance(5 * 1000);
	CHECK_UINT(fk.sends, SIP_REG_REBIND_MAX + 2);

	CASE("one rebind that settles resets the run");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(200, ";received=203.0.113.9;rport=40001", "Expires: 3600\r\n");
	CHECK_UINT(reg.rebind_run, 1);
	reply(200, ";received=203.0.113.9;rport=40001", "Expires: 3600\r\n");
	CHECK_UINT(reg.rebind_run, 0);
	CHECK_UINT(fk.registered, 1);
	CHECK_UINT(fk.failed, 0);

	/*
	 * An IPv6 literal wears brackets inside a URI (RFC 3261 19.1.1).
	 */
	CASE("an IPv6 local address is bracketed in the Contact");
	fake_reset();
	cfg.local_host = "2001:db8::1";
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::1]:5060>"));
	CHECK(!strcmp(sip_reg_contact(&reg), "sip:uvoice-test@[2001:db8::1]:5060"));

	CASE("an account without a contact user brackets it too");
	fake_reset();
	cfg.contact_user = NULL;
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	CHECK(has("Contact: <sip:[2001:db8::1]:5060>"));
	cfg_default();

	CASE("an IPv6 address in received is bracketed after the rebuild");
	cfg.local_host = "2001:db8::1";
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	reply(401, ";received=2001:db8::99;rport=41234", chal_2069());
	CHECK(last_req(&ri));
	CHECK(!strcmp(ri.contact, "sip:uvoice-test@[2001:db8::99]:41234"));
	cfg_default();

	CASE("an IPv4 address keeps its plain form");
	fake_reset();
	sip_reg_target_add(&reg, "192.0.2.10", 5060);
	sip_reg_start(&reg);
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060>"));
	CHECK(!has("[10.0.0.5]"));

	fake_reset();
}

static void local_host_tests(void)
{
	struct sip_reg_target targets[SIP_REG_TARGET_MAX];
	static char local_host[SIP_REG_HOST_MAX + 1] = R_LOCAL_HOST;
	char long_user[221];
	char uri[SIP_REG_URI_MAX];
	char call_id[SIP_REG_ID_MAX];
	struct req_info before, after;
	unsigned int sends;
	uint64_t rebuilt;

	CASE("sip_reg_local_host_set moves the Contact to an IPv6 address");
	cfg.local_host = local_host;
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5060));
	sip_reg_start(&reg);
	CHECK(last_req(&before));
	snprintf(call_id, sizeof(call_id), "%s", sip_reg_call_id(&reg));
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	CHECK(!strcmp(reg.contact_host, local_host));
	CHECK_UINT(reg.contact_port, R_LOCAL_PORT);
	CHECK(reg.contact_dirty);
	CHECK_UINT(reg.stat.contact_rebuilt, 1);
	CHECK_UINT(fk.sends, 1);
	CHECK(strstr(fk.trace, "contact 10.0.0.5 port 5060 to "
		     "2001:db8::5 port 5060 on a local host move") != NULL);

	CASE("the moved Contact is bracketed and the Call-ID is unchanged");
	CHECK(!strcmp(sip_reg_contact(&reg),
		      "sip:uvoice-test@[2001:db8::5]:5060"));
	CHECK(!strcmp(sip_reg_call_id(&reg), call_id));
	sip_reg_stop(&reg, false);
	sip_reg_start(&reg);
	CHECK(last_req(&after));
	CHECK(!strcmp(after.call_id, before.call_id));
	CHECK(!strcmp(after.from_tag, before.from_tag));
	CHECK_UINT(after.cseq, before.cseq + 1);
	CHECK(!strcmp(after.contact, sip_reg_contact(&reg)));
	CHECK(has("Via: SIP/2.0/UDP [2001:db8::5]:5060;branch="));
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::5]:5060>"));
	CHECK(!reg.contact_dirty);

	CASE("a host that does not fit leaves the old Contact standing");
	snprintf(uri, sizeof(uri), "%s", sip_reg_contact(&reg));
	memset(local_host, 'h', sizeof(local_host) - 1);
	local_host[sizeof(local_host) - 1] = 0;
	fk.trace[0] = 0;
	CHECK(!sip_reg_local_host_set(&reg));
	CHECK(strstr(fk.trace, "local host move refused, ") != NULL);
	CHECK(strstr(fk.trace, " does not fit, kept "
		     "2001:db8::5 port 5060") != NULL);
	local_host[0] = 0;
	fk.trace[0] = 0;
	CHECK(!sip_reg_local_host_set(&reg));
	CHECK(strstr(fk.trace, "local host move refused, no local host, "
		     "kept 2001:db8::5 port 5060") != NULL);
	CHECK(!sip_reg_local_host_set(NULL));
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(!strcmp(sip_reg_contact(&reg), uri));
	CHECK(!strcmp(reg.contact_host, local_host));
	CHECK_UINT(reg.contact_port, R_LOCAL_PORT);
	CHECK_UINT(reg.stat.contact_rebuilt, 1);
	CHECK(!reg.contact_dirty);
	cfg_default();
	memset(long_user, 'u', sizeof(long_user) - 1);
	long_user[sizeof(long_user) - 1] = 0;
	cfg.contact_user = long_user;
	cfg.transport = "TLS";
	cfg.local_host = local_host;
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5060));
	sip_reg_start(&reg);
	reply(401, ";rport=41234", chal_2069());
	CHECK_UINT(reg.contact_port, 41234);
	snprintf(uri, sizeof(uri), "%s", sip_reg_contact(&reg));
	CHECK(uri[0]);
	rebuilt = reg.stat.contact_rebuilt;
	sends = fk.sends;
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	fk.trace[0] = 0;
	CHECK(!sip_reg_local_host_set(&reg));
	CHECK(strstr(fk.trace, "local host move refused, 2001:db8::5 "
		     "does not fit, kept 10.0.0.5 port 41234") != NULL);
	CHECK(!strcmp(sip_reg_contact(&reg), uri));
	CHECK(!strcmp(reg.contact_host, R_LOCAL_HOST));
	CHECK_UINT(reg.contact_port, 41234);
	CHECK_UINT(reg.stat.contact_rebuilt, rebuilt);
	CHECK(!reg.contact_dirty);
	CHECK_UINT(fk.sends, sends);
	reg.contact_dirty = true;
	CHECK(!sip_reg_local_host_set(&reg));
	CHECK(reg.contact_dirty);
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	cfg_default();

	CASE("the targets and the sticky selection survive a local "
	     "host move");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	cfg.local_host = local_host;
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5060));
	CHECK(sip_reg_target_add(&reg, "2001:db8::10", 5070));
	sip_reg_start(&reg);
	reply(500, NULL, NULL);
	CHECK_UINT(reg.cur, 1);
	CHECK_UINT(reg.target[0].failures, 1);
	CHECK(reg.target[0].penalty_until > fk.now);
	memcpy(targets, reg.target, sizeof(targets));
	sends = fk.sends;
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	CHECK_UINT(reg.cur, 1);
	CHECK_UINT(reg.targets, 2);
	CHECK(!memcmp(targets, reg.target, sizeof(targets)));
	CHECK_UINT(fk.sends, sends);
	fake_advance(5 * 1000);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "2001:db8::10"));
	CHECK_UINT(fk.send[fk.sends - 1].port, 5070);
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::5]:5060>"));
	cfg_default();

	CASE("a NAT-learned Contact port goes back to the local port "
	     "on the move");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	cfg.local_host = local_host;
	cfg.local_port = 5072;
	cfg.transport = "TCP";
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5060));
	sip_reg_start(&reg);
	reply(401, ";received=203.0.113.9;rport=41234", chal_2069());
	CHECK_UINT(reg.contact_port, 41234);
	CHECK_UINT(reg.stat.contact_rebuilt, 1);
	sends = fk.sends;
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	CHECK_UINT(reg.contact_port, 5072);
	CHECK(!strcmp(sip_reg_contact(&reg),
		      "sip:uvoice-test@[2001:db8::5]:5072;transport=tcp"));
	CHECK_UINT(reg.stat.contact_rebuilt, 2);
	CHECK(reg.contact_dirty);
	CHECK_UINT(fk.sends, sends);
	cfg_default();

	CASE("a received value in the other family moves neither the host "
	     "nor the port");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	cfg.local_host = local_host;
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "2001:db8::10", 5060));
	sip_reg_start(&reg);
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	sip_reg_stop(&reg, false);
	sip_reg_start(&reg);
	fk.trace[0] = 0;
	reply(200, ";received=203.0.113.9;rport=41234", "Expires: 3600\r\n");
	CHECK(!strcmp(reg.contact_host, local_host));
	CHECK_UINT(reg.contact_port, R_LOCAL_PORT);
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::5]:5060>"));
	CHECK(strstr(fk.trace, "received 203.0.113.9 is not a bare literal "
		     "in the family of 2001:db8::5, "
		     "kept 2001:db8::5 port 5060") != NULL);
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	CHECK(sip_reg_local_host_set(&reg));
	sip_reg_stop(&reg, false);
	sip_reg_start(&reg);
	fk.trace[0] = 0;
	reply(200, ";received=[2001:db8::99];rport=41234", "Expires: 3600\r\n");
	CHECK(!strcmp(reg.contact_host, local_host));
	CHECK_UINT(reg.contact_port, R_LOCAL_PORT);
	CHECK_UINT(fk.registered, 2);
	CHECK(strstr(fk.trace, "received 2001:db8::99 is not a bare literal "
		     "in the family of 10.0.0.5, "
		     "kept 10.0.0.5 port 5060") != NULL);
	cfg_default();

	CASE("a v4-mapped received value is refused on both families");
	cfg.local_host = "2001:db8::5";
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "2001:db8::10", 5060));
	sip_reg_start(&reg);
	reply(200, ";received=::ffff:203.0.113.9;rport=41234",
	      "Expires: 3600\r\n");
	CHECK(!strcmp(reg.contact_host, cfg.local_host));
	CHECK_UINT(reg.contact_port, R_LOCAL_PORT);
	CHECK_UINT(reg.stat.contact_rebuilt, 0);
	CHECK_UINT(fk.sends, 1);
	cfg_default();
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5060));
	sip_reg_start(&reg);
	reply(200, ";received=::ffff:203.0.113.9;rport=41234",
	      "Expires: 3600\r\n");
	CHECK(!strcmp(reg.contact_host, cfg.local_host));
	CHECK_UINT(reg.contact_port, R_LOCAL_PORT);
	CHECK_UINT(reg.stat.contact_rebuilt, 0);
	CHECK_UINT(fk.sends, 1);

	CASE("a zone-id received value is refused and traced");
	cfg.local_host = "2001:db8::5";
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "2001:db8::10", 5060));
	sip_reg_start(&reg);
	fk.trace[0] = 0;
	reply(401, ";received=fe80::99%eth0;rport=41234", chal_2069());
	CHECK(!strcmp(reg.contact_host, cfg.local_host));
	CHECK_UINT(reg.contact_port, R_LOCAL_PORT);
	CHECK_UINT(reg.stat.contact_rebuilt, 0);
	CHECK(strstr(fk.trace, "received fe80::99%eth0 is not a bare literal "
		     "in the family of 2001:db8::5, "
		     "kept 2001:db8::5 port 5060") != NULL);
	sip_reg_refresh_now(&reg);
	fk.trace[0] = 0;
	reply(401, ";received=phone.example.net;rport=41234", chal_2069());
	CHECK(!strcmp(reg.contact_host, cfg.local_host));
	CHECK_UINT(reg.contact_port, R_LOCAL_PORT);
	CHECK(strstr(fk.trace, "received phone.example.net is not a bare "
		     "literal in the family of 2001:db8::5") != NULL);
	sip_reg_refresh_now(&reg);
	fk.trace[0] = 0;
	reply(401, ";received=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa;rport=41234",
	      chal_2069());
	CHECK(!strcmp(reg.contact_host, cfg.local_host));
	CHECK_UINT(reg.contact_port, R_LOCAL_PORT);
	CHECK_UINT(reg.stat.contact_rebuilt, 0);
	CHECK(strstr(fk.trace, "received aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
		     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa is not a bare literal "
		     "in the family of 2001:db8::5") != NULL);
	cfg_default();

	CASE("a received value in the same family still rebuilds the "
	     "Contact");
	cfg.local_host = local_host;
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "2001:db8::10", 5060));
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	sip_reg_start(&reg);
	reply(200, ";received=2001:db8::99;rport=41234", "Expires: 3600\r\n");
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(reg.stat.contact_rebuilt, 2);
	CHECK_UINT(fk.registered, 0);
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::99]:41234>"));
	reply(200, ";received=2001:db8::99;rport=41234", "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 1);
	CHECK_UINT(fk.sends, 2);
	snprintf(local_host, sizeof(local_host), "phone.example.net");
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5060));
	sip_reg_start(&reg);
	reply(401, ";received=2001:db8::99;rport=41234", chal_2069());
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::99]:41234>"));
	reply(200, ";received=203.0.113.9;rport=41234", "Expires: 3600\r\n");
	CHECK(has("Contact: <sip:uvoice-test@203.0.113.9:41234>"));
	cfg_default();
	fake_reset();
}

static void local_refresh_tests(void)
{
	static char local_host[SIP_REG_HOST_MAX] = R_LOCAL_HOST;
	char large_agent[SIP_REG_WIRE_MAX];
	char old_uri[SIP_REG_URI_MAX];
	size_t agent_len;
	struct sip_reg_target targets[SIP_REG_TARGET_MAX];
	struct req_info before, after;
	struct sip_reg stopped;
	uint32_t txn;
	unsigned int sends, timers, selected;

	CASE("a move to the same host records no removal Contact");
	cfg.local_host = local_host;
	register_ok();
	memcpy(&stopped, &reg, sizeof(stopped));
	fk.trace[0] = 0;
	CHECK(sip_reg_local_host_set(&reg));
	CHECK(!memcmp(&stopped, &reg, sizeof(stopped)));
	CHECK(!reg.old_contact_uri[0]);
	CHECK(strstr(fk.trace, "local host move: "
		     "the Contact is unchanged") != NULL);
	sip_reg_refresh_now(&reg);
	CHECK(!has(";expires=0"));

	CASE("two moves before a 2xx keep the first removal Contact");
	register_ok();
	snprintf(old_uri, sizeof(old_uri), "%s", reg.contact_uri);
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	sip_reg_refresh_now(&reg);
	snprintf(local_host, sizeof(local_host), "2001:db8::6");
	CHECK(sip_reg_local_host_set(&reg));
	CHECK(!strcmp(reg.old_contact_uri, old_uri));
	memcpy(&stopped, &reg, sizeof(stopped));
	fk.trace[0] = 0;
	CHECK(sip_reg_local_host_set(&reg));
	CHECK(!memcmp(&stopped, &reg, sizeof(stopped)));
	CHECK(strstr(fk.trace, "local host move: "
		     "the Contact is unchanged") != NULL);
	sip_reg_refresh_now(&reg);
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::6]:5060>\r\n"
		  "Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));
	CHECK(!has("Contact: <sip:uvoice-test@[2001:db8::5]:5060>"));

	CASE("a move back to the remembered host clears the removal slot");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	CHECK(sip_reg_local_host_set(&reg));
	CHECK(!reg.old_contact_uri[0]);
	CHECK(reg.contact_dirty);
	sip_reg_refresh_now(&reg);
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060>\r\n"));
	CHECK(!has(";expires=0"));

	CASE("a 2xx to a request without the removal Contact keeps it");
	register_ok();
	sip_reg_refresh_now(&reg);
	CHECK(!has(";expires=0"));
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	CHECK(reg.contact_dirty);
	sends = fk.sends;
	fk.trace[0] = 0;
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(!strcmp(reg.old_contact_uri, old_uri));
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::5]:5060>\r\n"
		  "Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));
	fk.trace[0] = 0;
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK(!reg.old_contact_uri[0]);

	CASE("a removal Contact that does not fit is dropped and the "
	     "REGISTER goes out");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5060));
	sip_reg_start(&reg);
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	sip_reg_refresh_now(&reg);
	CHECK(has(";expires=0"));
	memset(large_agent, 'u', sizeof(large_agent));
	agent_len = sizeof(reg.wire) - reg.wire_len +
		    strlen(cfg.user_agent) + 1;
	large_agent[agent_len] = 0;
	reg.cfg.user_agent = large_agent;
	sends = fk.sends;
	fk.trace[0] = 0;
	sip_reg_refresh_now(&reg);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK_UINT(fk.failed, 0);
	CHECK(!reg.old_contact_uri[0]);
	CHECK(!has(";expires=0"));
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::5]:5060>\r\n"));
	CHECK(has("Content-Length: 0\r\n\r\n"));
	CHECK(last_req(&after));
	CHECK(strstr(fk.trace, "the removal Contact does not fit, "
		     "the old binding expires on its own") != NULL);
	fk.trace[0] = 0;
	reply(200, NULL, "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 1);

	CASE("the next REGISTER after a move removes the old Contact "
	     "with expires 0");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	cfg.local_host = local_host;
	register_ok();
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	sip_reg_refresh_now(&reg);
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::5]:5060>\r\n"
		  "Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));
	reply(401, NULL, chal_nonce("move-nonce", NULL));
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));
	reply(423, NULL, "Min-Expires: 7200\r\n");
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));
	sends = fk.sends;
	fake_advance(500);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));

	CASE("a 2xx forgets the old Contact and an unregister "
	     "forgets it too");
	reply(200, NULL, "Expires: 7200\r\n");
	sip_reg_refresh_now(&reg);
	CHECK(!has(";expires=0"));
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::5]:5060>\r\n"));
	reply(200, NULL, "Expires: 7200\r\n");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	CHECK(sip_reg_local_host_set(&reg));
	sip_reg_stop(&reg, true);
	CHECK(has("Expires: 0\r\n"));
	CHECK(!has(";expires=0"));
	reply(500, NULL, NULL);
	sip_reg_start(&reg);
	CHECK(!has(";expires=0"));
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	sip_reg_stop(&reg, true);
	sip_reg_start(&reg);
	CHECK(!has(";expires=0"));

	CASE("sip_reg_refresh_now drops the in-flight REGISTER "
	     "and sends a fresh one");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	register_ok();
	sip_reg_refresh_now(&reg);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REFRESHING);
	txn = reg.txn;
	CHECK(sip_txn_get(&mgr, txn) != NULL);
	CHECK(last_req(&before));
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	sends = fk.sends;
	sip_reg_refresh_now(&reg);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(sip_txn_get(&mgr, txn) == NULL);
	CHECK(reg.txn != txn);
	CHECK(sip_txn_get(&mgr, reg.txn) != NULL);
	CHECK(last_req(&after));
	CHECK_UINT(after.cseq, before.cseq + 1);
	CHECK(strcmp(after.branch, before.branch) != 0);
	CHECK(has("Via: SIP/2.0/UDP [2001:db8::5]:5060;branch="));
	CHECK(!strcmp(after.contact, "sip:uvoice-test@[2001:db8::5]:5060"));
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REFRESHING);
	CHECK_UINT(sip_reg_expires(&reg), 3600);
	CHECK_UINT(fk.registered, 1);
	CHECK_UINT(fk.unregistered, 0);
	CHECK_UINT(fk.failed, 0);
	fake_advance(500);
	CHECK_UINT(fk.sends, sends + 2);
	CHECK(has("Via: SIP/2.0/UDP [2001:db8::5]:5060;branch="));

	CASE("sip_reg_refresh_now keeps the targets, the selection "
	     "and the Call-ID");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5060));
	CHECK(sip_reg_target_add(&reg, "2001:db8::10", 5070));
	sip_reg_start(&reg);
	reply(500, NULL, NULL);
	CHECK(reg.timer.armed);
	CHECK(reg.fail_run && reg.next_retry_s);
	reg.rebind_run = SIP_REG_REBIND_MAX;
	CHECK(last_req(&before));
	memcpy(targets, reg.target, sizeof(targets));
	selected = sip_reg_target_current(&reg);
	CHECK_UINT(selected, 1);
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	sends = fk.sends;
	sip_reg_refresh_now(&reg);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK_UINT(sip_reg_target_count(&reg), 2);
	CHECK_UINT(sip_reg_target_current(&reg), selected);
	CHECK(!memcmp(targets, reg.target, sizeof(targets)));
	CHECK(last_req(&after));
	CHECK(!strcmp(after.call_id, before.call_id));
	CHECK(!strcmp(after.from_tag, before.from_tag));
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "2001:db8::10"));
	CHECK_UINT(fk.send[fk.sends - 1].port, 5070);
	CHECK_UINT(reg.fail_run, 0);
	CHECK_UINT(reg.next_retry_s, 0);
	CHECK_UINT(reg.rebind_run, 0);
	CHECK(!reg.timer.armed);
	CHECK_UINT(sip_reg_timer_kind(&reg), SIP_REG_T_NONE);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_TRYING);
	CHECK_UINT(fk.registered, 0);
	CHECK_UINT(fk.unregistered, 0);
	CHECK_UINT(fk.failed, 1);

	CASE("sip_reg_refresh_now on a stopped registration does nothing");
	sip_reg_stop(&reg, false);
	memcpy(&stopped, &reg, sizeof(stopped));
	sends = fk.sends;
	timers = fk.timers;
	sip_reg_refresh_now(&reg);
	CHECK(!memcmp(&stopped, &reg, sizeof(stopped)));
	CHECK_UINT(fk.sends, sends);
	CHECK_UINT(fk.timers, timers);
	CHECK_UINT(fk.unregistered, 1);
	CHECK_UINT(fk.failed, 1);
	fake_advance(60 * 1000);
	CHECK_UINT(fk.sends, sends);

	CASE("sip_reg_refresh_now keeps the cached credentials");
	register_ok();
	snprintf(local_host, sizeof(local_host), "2001:db8::6");
	CHECK(sip_reg_local_host_set(&reg));
	sends = fk.sends;
	sip_reg_refresh_now(&reg);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(last_req(&after));
	CHECK(after.auth);
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::6]:5060>\r\n"));

	CASE("sip_reg_refresh_now moves a registered line to refreshing");
	register_ok();
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);
	sends = fk.sends;
	sip_reg_refresh_now(&reg);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REFRESHING);
	CHECK_UINT(sip_reg_expires(&reg), 3600);
	CHECK_UINT(fk.registered, 1);
	CHECK_UINT(fk.unregistered, 0);
	CHECK_UINT(fk.failed, 0);

	CASE("sip_reg_refresh_now while unregistering does nothing");
	sip_reg_stop(&reg, true);
	CHECK(reg.unregistering);
	memcpy(&stopped, &reg, sizeof(stopped));
	sends = fk.sends;
	timers = fk.timers;
	sip_reg_refresh_now(&reg);
	CHECK(!memcmp(&stopped, &reg, sizeof(stopped)));
	CHECK_UINT(fk.sends, sends);
	CHECK_UINT(fk.timers, timers);
	CHECK_UINT(fk.unregistered, 0);

	CASE("sip_reg_refresh_now with no target resolves instead of "
	     "sending");
	register_ok();
	sip_reg_targets_clear(&reg);
	CHECK_UINT(reg.targets, 0);
	sends = fk.sends;
	sip_reg_refresh_now(&reg);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_RESOLVING);
	CHECK_UINT(fk.sends, sends);
	CHECK_UINT(reg.txn, SIP_HANDLE_NONE);
	CHECK(!reg.timer.armed);
	CHECK_UINT(fk.unregistered, 0);
	CHECK_UINT(fk.failed, 0);

	/*
	 * A line behind NAT registers with the mapping the registrar reported,
	 * and that URI is what a local host move records for removal. The
	 * answer to the REGISTER of the move reports the same mapping again, so
	 * the Contact goes back to the URI the slot names. A correction that
	 * carried both would ask the registrar to refresh and to remove one
	 * binding (RFC 3261 10.3 step 7).
	 */
	CASE("a NAT answer that names the removal Contact clears the "
	     "removal slot");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	fake_reset();
	CHECK(sip_reg_target_add(&reg, "192.0.2.10", 5060));
	sip_reg_start(&reg);
	reply(200, ";received=203.0.113.9;rport=41234", "Expires: 3600\r\n");
	reply(200, ";received=203.0.113.9;rport=41234", "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 1);
	snprintf(local_host, sizeof(local_host), "10.0.0.6");
	CHECK(sip_reg_local_host_set(&reg));
	CHECK(!strcmp(reg.old_contact_uri, "sip:uvoice-test@203.0.113.9:41234"));
	sip_reg_refresh_now(&reg);
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.6:5060>\r\n"
		  "Contact: <sip:uvoice-test@203.0.113.9:41234>;expires=0\r\n"));
	sends = fk.sends;
	fk.trace[0] = 0;
	reply(200, ";received=203.0.113.9;rport=41234", "Expires: 3600\r\n");
	CHECK(!reg.old_contact_uri[0]);
	CHECK(strstr(fk.trace, "the mapping is the removal Contact, "
		     "nothing to remove") != NULL);

	CASE("the correction REGISTER behind NAT carries one Contact and "
	     "no expires=0");
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(has("Contact: <sip:uvoice-test@203.0.113.9:41234>\r\n"));
	CHECK(!has("Contact: <sip:uvoice-test@10.0.0.6:5060>"));
	CHECK(!has(";expires=0"));
	reply(200, ";received=203.0.113.9;rport=41234", "Expires: 3600\r\n");
	CHECK_UINT(fk.registered, 2);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_REGISTERED);

	CASE("a refused REGISTER drops the removal Contact and the retry "
	     "carries the new one alone");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	register_ok();
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	sip_reg_refresh_now(&reg);
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));
	fk.trace[0] = 0;
	reply(500, NULL, NULL);
	CHECK_UINT(fk.failed, 1);
	CHECK(!reg.old_contact_uri[0]);
	CHECK(strstr(fk.trace, "500 to a REGISTER with a removal Contact, "
		     "the old binding expires on its own") != NULL);
	sends = fk.sends;
	fake_advance(5 * 1000);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::5]:5060>\r\n"));
	CHECK(!has(";expires=0"));

	CASE("a 403 to a REGISTER with a removal Contact does not repeat "
	     "the removal");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	register_ok();
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	sip_reg_refresh_now(&reg);
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));
	reply(403, NULL, NULL);
	CHECK_UINT(fk.last_retry, SIP_REG_BACKOFF_AUTH);
	CHECK(!reg.old_contact_uri[0]);
	sends = fk.sends;
	fake_advance(SIP_REG_BACKOFF_AUTH * 1000);
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::5]:5060>\r\n"));
	CHECK(!has(";expires=0"));

	/* no registrar judged the request, so the removal is still owed */
	CASE("a timeout keeps the removal Contact for the retry");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	register_ok();
	snprintf(old_uri, sizeof(old_uri), "%s", reg.contact_uri);
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	sip_reg_refresh_now(&reg);
	fake_advance(32 * 1000 + 100);
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_code, 408);
	CHECK(!strcmp(reg.old_contact_uri, old_uri));
	fake_advance(5 * 1000);
	CHECK_UINT(sip_reg_state(&reg), SIP_REG_TRYING);
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::5]:5060>\r\n"
		  "Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));

	/*
	 * A challenge asks for the same request with credentials and judges
	 * nothing in it. A challenge that ends the exchange as a credential
	 * failure is no verdict on the removal either, from the registrar (401)
	 * or from a proxy in front of it (407), and neither is a 423.
	 */
	CASE("a challenge keeps the removal Contact for the credential "
	     "retry");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	register_ok();
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	sip_reg_refresh_now(&reg);
	sends = fk.sends;
	reply(401, NULL, chal_nonce("removal-nonce", NULL));
	CHECK_UINT(fk.sends, sends + 1);
	CHECK(last_req(&after));
	CHECK(after.auth);
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));
	reply(401, NULL, chal_nonce("removal-nonce-2", NULL));
	CHECK_UINT(fk.failed, 1);
	CHECK_UINT(fk.last_retry, SIP_REG_BACKOFF_AUTH);
	CHECK(!strcmp(reg.old_contact_uri, old_uri));
	sip_reg_refresh_now(&reg);
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));
	reply(407, NULL, NULL);
	CHECK_UINT(fk.failed, 2);
	CHECK(!strcmp(reg.old_contact_uri, old_uri));
	sip_reg_refresh_now(&reg);
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));
	reply(423, NULL, NULL);
	CHECK_UINT(fk.failed, 3);
	CHECK(!strcmp(reg.old_contact_uri, old_uri));

	/*
	 * A family move behind a failed attempt records the slot and sends
	 * nothing, so a slot can stand while a request that left before it is
	 * still in flight. The refusal of that request judged one Contact, and
	 * the removal has not reached the registrar yet.
	 */
	CASE("a refusal of a REGISTER that carried no removal Contact keeps "
	     "the slot");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	register_ok();
	sip_reg_refresh_now(&reg);
	CHECK(!has(";expires=0"));
	snprintf(local_host, sizeof(local_host), "2001:db8::5");
	CHECK(sip_reg_local_host_set(&reg));
	CHECK(!strcmp(reg.old_contact_uri, old_uri));
	reply(500, NULL, NULL);
	CHECK_UINT(fk.failed, 1);
	CHECK(!strcmp(reg.old_contact_uri, old_uri));
	fake_advance(5 * 1000);
	CHECK(has("Contact: <sip:uvoice-test@[2001:db8::5]:5060>\r\n"
		  "Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));

	/*
	 * The server that refused is the new one of a server move, and the
	 * binding stands at the old one, which never saw the removal Contact.
	 * The rollback keeps the slot, so the refresh at the old server still
	 * removes the Contact it holds.
	 */
	CASE("a refused move that rolls back keeps the removal Contact");
	snprintf(local_host, sizeof(local_host), R_LOCAL_HOST);
	move_ready();
	snprintf(local_host, sizeof(local_host), "10.0.0.6");
	CHECK(sip_reg_local_host_set(&reg));
	sip_reg_refresh_now(&reg);
	CHECK(reg.moving);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.11"));
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));
	reply(500, NULL, NULL);
	CHECK_UINT(fk.rolled_back, 1);
	CHECK_UINT(fk.failed, 0);
	CHECK(!strcmp(reg.old_contact_uri, old_uri));
	sip_reg_refresh_now(&reg);
	CHECK(!strcmp(fk.send[fk.sends - 1].host, "192.0.2.10"));
	CHECK(has("Contact: <sip:uvoice-test@10.0.0.6:5060>\r\n"
		  "Contact: <sip:uvoice-test@10.0.0.5:5060>;expires=0\r\n"));
	cfg_default();
	fake_reset();
}

int main(void)
{
	struct sip_timer_cfg tc;

	cfg_default();
	sip_timer_cfg_default(&tc);
	sip_txn_mgr_init(&mgr, &fk.env, &tc);
	sip_reg_init(&reg, &mgr, &fk.env, 0x00010001, &cfg, &reg_cb, NULL);

	flow_tests();
	refresh_tests();
	expiry_tests();
	min_expires_tests();
	auth_tests();
	contact_tests();
	transport_tests();
	target_tests();
	reload_tests();
	retarget_tests();
	move_tests();
	service_route_tests();
	failure_tests();
	retry_now_tests();
	stop_tests();
	sol_tests();
	local_host_tests();
	local_refresh_tests();
	lifetime_tests();

	sip_reg_done(&reg);
	sip_txn_mgr_done(&mgr);

	return tu_summary("test_sip_reg");
}
