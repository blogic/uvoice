// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the dialog layer (src/proto/sip/dialog.c).
 *
 * Like the transaction and registration suites, this suite uses a scripted
 * environment with a capture array in place of a socket. The layer sends one
 * message by itself, the ACK of a 2xx. A case can inspect that ACK byte for
 * byte and compare its retransmission with the original.
 */
#include <stdlib.h>

#include "proto/sip/dialog.h"
#include "test_msg.h"
#include "test_util.h"

#define FAKE_SEND_MAX	32
#define WIRE_MAX	4096
#define TEXT_MAX	4096

#define L_HOST		"10.0.0.5"
#define L_PORT		5060
#define L_CONTACT	"sip:5551000@10.0.0.5:5060"
#define L_URI		"sip:5551000@carrier.net"
#define R_URI		"sip:5552000@carrier.net"
#define L_TAG		"localtag"
#define R_TAG		"remotetag"
#define CALL_ID		"call-1@10.0.0.5"

struct fake_send {
	char buf[WIRE_MAX + 1];
	size_t len;
};

static struct {
	struct sip_txn_env env;
	struct fake_send send[FAKE_SEND_MAX];
	unsigned int sends;
	bool send_fail;
} fk;

static struct sip_dialog_set set;
static struct sip_dialog_local local;

static int fake_send_fn(struct sip_txn_env *env, const void *buf, size_t len,
			const struct sip_txn_dst *dst)
{
	struct fake_send *s;

	if (fk.send_fail)
		return -1;

	if (fk.sends < FAKE_SEND_MAX) {
		s = &fk.send[fk.sends];
		s->len = len < WIRE_MAX ? len : WIRE_MAX;
		memcpy(s->buf, buf, s->len);
		s->buf[s->len] = 0;
	}
	fk.sends++;

	return (int)len;
}

static uint64_t fake_now(struct proto_timer_env *env)
{
	return 100000;
}

static void fake_trace(struct proto_timer_env *env, const char *msg)
{
}

static void fake_reset(void)
{
	sip_dialog_set_done(&set);
	memset(&fk, 0, sizeof(fk));
	fk.env.send = fake_send_fn;
	fk.env.base.now_ms = fake_now;
	fk.env.base.trace = fake_trace;
	sip_dialog_set_init(&set, &fk.env);

	memset(&local, 0, sizeof(local));
	local.host = L_HOST;
	local.port = L_PORT;
	local.contact = L_CONTACT;
	local.user_agent = "uvoice-test/1";
}

/* --------------------------------------------------------------- messages */

static char text_buf[4][TEXT_MAX];
static unsigned int text_next;

/*
 * A response to an INVITE we sent, with both tags named. A response echoes the
 * request it answers, so the From tag is always ours and the To tag the peer's,
 * whoever created the dialog.
 */
static struct sip_msg *resp_tags_take(uint32_t code, const char *from_tag, const char *to_tag,
				      unsigned int cseq, const char *extra)
{
	char *buf = text_buf[text_next++ % 4];

	snprintf(buf, TEXT_MAX,
		 "SIP/2.0 %u %s\r\n"
		 "Via: SIP/2.0/UDP " L_HOST ":5060;branch=z9hG4bK-inv1\r\n"
		 "From: \"Line 1\" <" L_URI ">;tag=%s\r\n"
		 "To: <" R_URI ">%s%s\r\n"
		 "Call-ID: " CALL_ID "\r\n"
		 "CSeq: %u INVITE\r\n"
		 "%s"
		 "Content-Length: 0\r\n\r\n",
		 code, sip_status_reason(code), from_tag, to_tag ? ";tag=" : "",
		 to_tag ? to_tag : "", cseq, extra ? extra : "");

	return msg_take(buf);
}

/* a response to the INVITE we placed */
static struct sip_msg *resp_take(uint32_t code, const char *to_tag, unsigned int cseq,
				 const char *extra)
{
	return resp_tags_take(code, L_TAG, to_tag, cseq, extra);
}

/* a response to a re-INVITE we placed inside a dialog the peer created */
static struct sip_msg *uas_resp_take(uint32_t code, unsigned int cseq, const char *extra)
{
	return resp_tags_take(code, "uas-tag", R_TAG, cseq, extra);
}

/* an INVITE arriving from the network */
static struct sip_msg *invite_take(const char *extra)
{
	char *buf = text_buf[text_next++ % 4];

	snprintf(buf, TEXT_MAX,
		 "INVITE " L_CONTACT " SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-peer1\r\n"
		 "Max-Forwards: 70\r\n"
		 "From: \"Caller\" <" R_URI ">;tag=" R_TAG "\r\n"
		 "To: <" L_URI ">\r\n"
		 "Call-ID: " CALL_ID "\r\n"
		 "CSeq: 7 INVITE\r\n"
		 "Contact: <sip:5552000@192.0.2.10:5060>\r\n"
		 "%s"
		 "Content-Length: 0\r\n\r\n",
		 extra ? extra : "");

	return msg_take(buf);
}

/* an in-dialog request arriving from the peer */
static struct sip_msg *indialog_take(const char *method, const char *from_tag,
				     const char *to_tag, unsigned int cseq)
{
	char *buf = text_buf[text_next++ % 4];

	snprintf(buf, TEXT_MAX,
		 "%s " L_CONTACT " SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-in%u\r\n"
		 "From: <" R_URI ">;tag=%s\r\n"
		 "To: <" L_URI ">%s%s\r\n"
		 "Call-ID: " CALL_ID "\r\n"
		 "CSeq: %u %s\r\n"
		 "Content-Length: 0\r\n\r\n",
		 method, cseq, from_tag, to_tag ? ";tag=" : "", to_tag ? to_tag : "", cseq,
		 method);

	return msg_take(buf);
}

/* an in-dialog request that carries extra headers, a Contact for example */
static struct sip_msg *indialog_extra(const char *method, const char *from_tag,
				      const char *to_tag, unsigned int cseq, const char *extra)
{
	char *buf = text_buf[text_next++ % 4];

	snprintf(buf, TEXT_MAX,
		 "%s " L_CONTACT " SIP/2.0\r\n"
		 "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-ix%u\r\n"
		 "From: <" R_URI ">;tag=%s\r\n"
		 "To: <" L_URI ">%s%s\r\n"
		 "Call-ID: " CALL_ID "\r\n"
		 "CSeq: %u %s\r\n"
		 "%s"
		 "Content-Length: 0\r\n\r\n",
		 method, cseq, from_tag, to_tag ? ";tag=" : "", to_tag ? to_tag : "", cseq,
		 method, extra ? extra : "");

	return msg_take(buf);
}

/* n Record-Route header lines, hop 1 first */
static const char *record_routes(unsigned int n)
{
	static char buf[TEXT_MAX];
	unsigned int i, used = 0;

	buf[0] = 0;
	for (i = 0; i < n; i++)
		used += (unsigned int)snprintf(buf + used, sizeof(buf) - used,
					       "Record-Route: <sip:p%u.example.net;lr>\r\n",
					       i + 1);

	return buf;
}

static struct sip_txn_dst *dst_make(const char *host, uint16_t port)
{
	static struct sip_txn_dst dst;

	sip_dst_set(&dst, host, port);

	return &dst;
}

static char out[WIRE_MAX];

static const char *build(struct sip_dialog *d, enum sip_method method, const char *branch,
			 bool contact, const char *ctype, const char *body, uint32_t *cseq)
{
	struct sip_dialog_req r;
	struct sip_buf b;

	memset(&r, 0, sizeof(r));
	r.method = method;
	r.branch = branch;
	r.with_contact = contact;
	r.ctype = ctype;
	r.body = body;
	r.body_len = body ? strlen(body) : 0;

	sip_buf_init(&b, out, sizeof(out));
	if (!sip_dialog_req_build(d, &r, &b))
		return "";

	if (cseq)
		*cseq = r.cseq;

	return sip_buf_str(&b);
}

static const char *last_send(void)
{
	if (!fk.sends || fk.sends > FAKE_SEND_MAX)
		return "";

	return fk.send[fk.sends - 1].buf;
}

/* ------------------------------------------------------------ UAC dialogs */

static void uac_tests(void)
{
	struct sip_dialog *d, *dlg2;
	struct sip_msg *m;
	uint32_t h, cseq;

	CASE("a 180 with a To tag creates an early dialog");
	fake_reset();
	m = resp_take(180, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	CHECK(m != NULL);
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	CHECK_UINT(sip_dialog_state(d), SIP_DLG_EARLY);
	CHECK_UINT(sip_dialog_count(&set), 1);
	CHECK(!strcmp(d->call_id, CALL_ID));
	CHECK(!strcmp(d->local_tag, L_TAG));
	CHECK(!strcmp(d->remote_tag, R_TAG));
	CHECK(!strcmp(d->local_uri, L_URI));
	CHECK(!strcmp(d->remote_uri, R_URI));
	CHECK(!strcmp(d->remote_target, "sip:5552000@192.0.2.10:5060"));
	CHECK_UINT(d->local_cseq, 1);
	CHECK(d->uac);

	CASE("the 200 with the same tag confirms that dialog");
	h = sip_dialog_handle(d);
	m = resp_take(200, R_TAG, 1, "Contact: <sip:5552000@192.0.2.11:5060>\r\n");
	dlg2 = sip_dialog_uac_update(&set, m, NULL, &local);
	CHECK(dlg2 == d);
	CHECK_UINT(sip_dialog_handle(dlg2), h);
	CHECK_UINT(sip_dialog_state(d), SIP_DLG_CONFIRMED);
	CHECK_UINT(sip_dialog_count(&set), 1);

	CASE("the 200 refreshes the remote target (RFC 3261 12.2.1.2)");
	CHECK(!strcmp(d->remote_target, "sip:5552000@192.0.2.11:5060"));

	CASE("a 200 with another To tag adopts a new dialog and drops the early one");
	fake_reset();
	m = resp_take(180, "early-tag", 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	m = resp_take(200, "final-tag", 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	dlg2 = sip_dialog_uac_update(&set, m, NULL, &local);
	CHECK(dlg2 != NULL && dlg2 != d);
	CHECK_UINT(sip_dialog_state(d), SIP_DLG_TERMINATED);
	CHECK_UINT(sip_dialog_state(dlg2), SIP_DLG_CONFIRMED);
	CHECK(!strcmp(dlg2->remote_tag, "final-tag"));
	CHECK_UINT(set.stat.replaced, 1);

	CASE("the discarded early dialog belongs to its owner until it is put");
	CHECK_UINT(sip_dialog_count(&set), 2);
	sip_dialog_put(d);
	CHECK_UINT(sip_dialog_count(&set), 1);
	CHECK(sip_dialog_get(&set, sip_dialog_handle(dlg2)) == dlg2);

	CASE("a 200 without a To tag confirms the early dialog it belongs to");
	fake_reset();
	m = resp_take(183, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	m = resp_take(200, NULL, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	dlg2 = sip_dialog_uac_update(&set, m, NULL, &local);
	CHECK(dlg2 == d);
	CHECK_UINT(sip_dialog_state(d), SIP_DLG_CONFIRMED);
	CHECK(!strcmp(d->remote_tag, R_TAG));

	CASE("a 200 without a To tag and without an early dialog is still a dialog");
	fake_reset();
	m = resp_take(200, NULL, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	CHECK(d->remote_tag == NULL);
	CHECK_UINT(sip_dialog_state(d), SIP_DLG_CONFIRMED);

	CASE("its requests carry a To without a tag");
	CHECK(strstr(build(d, SIP_METHOD_BYE, "z9hG4bK-b1", false, NULL, NULL, NULL),
		     "To: <" R_URI ">\r\n") != NULL);

	CASE("a provisional without a To tag creates no dialog");
	fake_reset();
	m = resp_take(180, NULL, 1, NULL);
	CHECK(sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local) == NULL);
	CHECK_UINT(sip_dialog_count(&set), 0);

	CASE("100, 3xx and 4xx create no dialog");
	m = resp_take(100, NULL, 1, NULL);
	CHECK(sip_dialog_uac_update(&set, m, NULL, &local) == NULL);
	m = resp_take(302, R_TAG, 1, NULL);
	CHECK(sip_dialog_uac_update(&set, m, NULL, &local) == NULL);
	m = resp_take(486, R_TAG, 1, NULL);
	CHECK(sip_dialog_uac_update(&set, m, NULL, &local) == NULL);
	CHECK_UINT(sip_dialog_count(&set), 0);

	CASE("a response whose CSeq is not an INVITE creates no dialog");
	m = msg_take("SIP/2.0 200 OK\r\n"
		     "Via: SIP/2.0/UDP " L_HOST ":5060;branch=z9hG4bK-x\r\n"
		     "From: <" L_URI ">;tag=" L_TAG "\r\n"
		     "To: <" R_URI ">;tag=" R_TAG "\r\n"
		     "Call-ID: " CALL_ID "\r\n"
		     "CSeq: 2 OPTIONS\r\n\r\n");
	CHECK(sip_dialog_uac_update(&set, m, NULL, &local) == NULL);

	CASE("a request is not a response");
	m = invite_take(NULL);
	CHECK(sip_dialog_uac_update(&set, m, NULL, &local) == NULL);

	CASE("the local CSeq comes from the INVITE the response answers");
	fake_reset();
	m = resp_take(200, R_TAG, 41, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	CHECK(strstr(build(d, SIP_METHOD_BYE, "z9hG4bK-b1", false, NULL, NULL, &cseq),
		     "CSeq: 42 BYE") != NULL);
	CHECK_UINT(cseq, 42);
}

/* --------------------------------------------------------------- route set */

static void route_tests(void)
{
	struct sip_dialog *d;
	struct sip_msg *m;
	const char *wire;

	CASE("the UAC route set is the Record-Route chain reversed");
	fake_reset();
	m = resp_take(200, R_TAG, 1,
		      "Record-Route: <sip:p1.example.net;lr>\r\n"
		      "Record-Route: <sip:p2.example.net;lr>\r\n"
		      "Record-Route: <sip:p3.example.net;lr>\r\n"
		      "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	CHECK_UINT(d->routes, 3);
	CHECK(!strcmp(d->route[0], "<sip:p3.example.net;lr>"));
	CHECK(!strcmp(d->route[1], "<sip:p2.example.net;lr>"));
	CHECK(!strcmp(d->route[2], "<sip:p1.example.net;lr>"));

	CASE("a loose route set goes out in that order with the target as request URI");
	wire = build(d, SIP_METHOD_BYE, "z9hG4bK-b1", false, NULL, NULL, NULL);
	CHECK(strstr(wire, "BYE sip:5552000@192.0.2.10:5060 SIP/2.0") != NULL);
	CHECK(strstr(wire, "Route: <sip:p3.example.net;lr>\r\n"
			   "Route: <sip:p2.example.net;lr>\r\n"
			   "Route: <sip:p1.example.net;lr>\r\n") != NULL);

	CASE("several Record-Route values on one line count as several hops");
	fake_reset();
	m = resp_take(200, R_TAG, 1,
		      "Record-Route: <sip:p1.example.net;lr>, <sip:p2.example.net;lr>\r\n"
		      "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	CHECK_UINT(d->routes, 2);
	CHECK(!strcmp(d->route[0], "<sip:p2.example.net;lr>"));

	CASE("a first hop without lr is a strict router (RFC 3261 12.2.1.1)");
	fake_reset();
	m = resp_take(200, R_TAG, 1,
		      "Record-Route: <sip:p1.example.net;lr>\r\n"
		      "Record-Route: <sip:strict.example.net>\r\n"
		      "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	wire = build(d, SIP_METHOD_BYE, "z9hG4bK-b1", false, NULL, NULL, NULL);
	CHECK(strstr(wire, "BYE sip:strict.example.net SIP/2.0") != NULL);
	CHECK(strstr(wire, "Route: <sip:p1.example.net;lr>\r\n") != NULL);
	CHECK(strstr(wire, "Route: <sip:5552000@192.0.2.10:5060>\r\n") != NULL);

	CASE("the route set is fixed at creation and a later 200 does not move it");
	fake_reset();
	m = resp_take(180, R_TAG, 1,
		      "Record-Route: <sip:p1.example.net;lr>\r\n"
		      "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	m = resp_take(200, R_TAG, 1,
		      "Record-Route: <sip:other.example.net;lr>\r\n"
		      "Record-Route: <sip:second.example.net;lr>\r\n"
		      "Contact: <sip:5552000@192.0.2.99:5060>\r\n");
	CHECK(sip_dialog_uac_update(&set, m, NULL, &local) == d);
	CHECK_UINT(d->routes, 1);
	CHECK(!strcmp(d->route[0], "<sip:p1.example.net;lr>"));
	CHECK(!strcmp(d->remote_target, "sip:5552000@192.0.2.99:5060"));

	CASE("a dialog without a route set names the remote target directly");
	fake_reset();
	m = resp_take(200, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	CHECK_UINT(d->routes, 0);
	wire = build(d, SIP_METHOD_BYE, "z9hG4bK-b1", false, NULL, NULL, NULL);
	CHECK(strstr(wire, "BYE sip:5552000@192.0.2.10:5060 SIP/2.0") != NULL);
	CHECK(strstr(wire, "Route:") == NULL);

	CASE("a dialog with neither target nor route cannot build a request");
	fake_reset();
	m = resp_take(200, R_TAG, 1, NULL);
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	CHECK(d->remote_target == NULL);
	CHECK(!strcmp(build(d, SIP_METHOD_BYE, "z9hG4bK-b1", false, NULL, NULL, NULL), ""));
}

/* ------------------------------------------------------------ UAS dialogs */

static void uas_tests(void)
{
	struct sip_dialog *d;
	struct sip_msg *m;
	const char *wire;

	CASE("an accepted INVITE builds a UAS dialog");
	fake_reset();
	m = invite_take("Record-Route: <sip:p1.example.net;lr>\r\n"
			"Record-Route: <sip:p2.example.net;lr>\r\n");
	CHECK(m != NULL);
	d = sip_dialog_uas_create(&set, m, "uas-tag", dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	CHECK(!d->uac);
	CHECK(!strcmp(d->local_tag, "uas-tag"));
	CHECK(!strcmp(d->remote_tag, R_TAG));
	CHECK(!strcmp(d->local_uri, L_URI));
	CHECK(!strcmp(d->remote_uri, R_URI));
	CHECK(!strcmp(d->remote_target, "sip:5552000@192.0.2.10:5060"));
	CHECK_UINT(d->remote_cseq, 7);
	CHECK_UINT(d->local_cseq, 0);
	CHECK_UINT(sip_dialog_state(d), SIP_DLG_EARLY);

	CASE("the UAS route set keeps the received order");
	CHECK_UINT(d->routes, 2);
	CHECK(!strcmp(d->route[0], "<sip:p1.example.net;lr>"));
	CHECK(!strcmp(d->route[1], "<sip:p2.example.net;lr>"));

	CASE("confirm moves the dialog on");
	sip_dialog_confirm(d);
	CHECK_UINT(sip_dialog_state(d), SIP_DLG_CONFIRMED);
	CHECK_UINT(set.stat.confirmed, 1);
	sip_dialog_confirm(d);
	CHECK_UINT(set.stat.confirmed, 1);

	CASE("a BYE from the UAS side swaps the tags");
	wire = build(d, SIP_METHOD_BYE, "z9hG4bK-uas1", false, NULL, NULL, NULL);
	CHECK(strstr(wire, "BYE sip:5552000@192.0.2.10:5060 SIP/2.0") != NULL);
	CHECK(strstr(wire, "From: <" L_URI ">;tag=uas-tag") != NULL);
	CHECK(strstr(wire, "To: <" R_URI ">;tag=" R_TAG) != NULL);
	CHECK(strstr(wire, "CSeq: 1 BYE") != NULL);
	CHECK(strstr(wire, "Via: SIP/2.0/UDP 10.0.0.5:5060;branch=z9hG4bK-uas1;rport") != NULL);

	CASE("an INVITE without a From tag builds no dialog");
	fake_reset();
	m = msg_take("INVITE " L_CONTACT " SIP/2.0\r\n"
		     "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-p\r\n"
		     "From: <" R_URI ">\r\n"
		     "To: <" L_URI ">\r\n"
		     "Call-ID: " CALL_ID "\r\n"
		     "CSeq: 1 INVITE\r\n\r\n");
	CHECK(sip_dialog_uas_create(&set, m, "uas-tag", NULL, &local) == NULL);

	CASE("a UAS dialog needs a local tag and an INVITE");
	m = invite_take(NULL);
	CHECK(sip_dialog_uas_create(&set, m, NULL, NULL, &local) == NULL);
	CHECK(sip_dialog_uas_create(&set, m, "", NULL, &local) == NULL);
	m = indialog_take("BYE", R_TAG, "uas-tag", 8);
	CHECK(sip_dialog_uas_create(&set, m, "uas-tag", NULL, &local) == NULL);
	CHECK_UINT(sip_dialog_count(&set), 0);
}

/* --------------------------------------------------------------- matching */

static void match_tests(void)
{
	struct sip_dialog *d, *u;
	struct sip_msg *m;

	CASE("an in-dialog BYE finds the UAS dialog");
	fake_reset();
	m = invite_take(NULL);
	d = sip_dialog_uas_create(&set, m, "uas-tag", dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	m = indialog_take("BYE", R_TAG, "uas-tag", 8);
	CHECK(sip_dialog_find(&set, m) == d);

	CASE("a wrong tag or a wrong Call-ID does not match");
	m = indialog_take("BYE", "someone-else", "uas-tag", 8);
	CHECK(sip_dialog_find(&set, m) == NULL);
	m = indialog_take("BYE", R_TAG, "other-tag", 8);
	CHECK(sip_dialog_find(&set, m) == NULL);
	m = msg_take("BYE " L_CONTACT " SIP/2.0\r\n"
		     "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-in9\r\n"
		     "From: <" R_URI ">;tag=" R_TAG "\r\n"
		     "To: <" L_URI ">;tag=uas-tag\r\n"
		     "Call-ID: another-call@10.0.0.5\r\n"
		     "CSeq: 9 BYE\r\n\r\n");
	CHECK(sip_dialog_find(&set, m) == NULL);

	CASE("a terminated dialog stops matching at once");
	sip_dialog_terminate(d);
	m = indialog_take("BYE", R_TAG, "uas-tag", 8);
	CHECK(sip_dialog_find(&set, m) == NULL);
	CHECK_UINT(set.stat.terminated, 1);

	CASE("a UAC dialog matches an incoming BYE with the tags the other way round");
	fake_reset();
	m = resp_take(200, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	u = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(u != NULL);
	m = indialog_take("BYE", R_TAG, L_TAG, 4);
	CHECK(sip_dialog_find(&set, m) == u);

	CASE("a response is never matched as an in-dialog request");
	m = resp_take(200, R_TAG, 1, NULL);
	CHECK(sip_dialog_find(&set, m) == NULL);
}

/* --------------------------------------------------------------- sequence */

static void cseq_tests(void)
{
	struct sip_dialog *d;
	struct sip_msg *m;

	CASE("the remote sequence number moves forward");
	fake_reset();
	m = invite_take(NULL);
	d = sip_dialog_uas_create(&set, m, "uas-tag", dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	m = indialog_take("INFO", R_TAG, "uas-tag", 8);
	CHECK_UINT(sip_dialog_remote_cseq_take(d, m), SIP_DLG_CSEQ_OK);
	CHECK_UINT(d->remote_cseq, 8);

	CASE("a number below the highest one seen is refused (RFC 3261 12.2.2)");
	m = indialog_take("INFO", R_TAG, "uas-tag", 5);
	CHECK_UINT(sip_dialog_remote_cseq_take(d, m), SIP_DLG_CSEQ_OLD);
	CHECK_UINT(d->remote_cseq, 8);

	CASE("the same number again is refused for a request that is not an ACK");
	m = indialog_take("INFO", R_TAG, "uas-tag", 8);
	CHECK_UINT(sip_dialog_remote_cseq_take(d, m), SIP_DLG_CSEQ_OLD);

	CASE("an ACK carries the number of its INVITE and does not move the dialog");
	m = indialog_take("ACK", R_TAG, "uas-tag", 8);
	CHECK_UINT(sip_dialog_remote_cseq_take(d, m), SIP_DLG_CSEQ_OK);
	CHECK_UINT(d->remote_cseq, 8);

	CASE("a request without a CSeq is refused");
	m = msg_take("BYE " L_CONTACT " SIP/2.0\r\n"
		     "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-nocseq\r\n"
		     "From: <" R_URI ">;tag=" R_TAG "\r\n"
		     "To: <" L_URI ">;tag=uas-tag\r\n"
		     "Call-ID: " CALL_ID "\r\n\r\n");
	CHECK_UINT(sip_dialog_remote_cseq_take(d, m), SIP_DLG_CSEQ_BAD);

	CASE("a response is not an in-dialog request either");
	m = resp_take(200, R_TAG, 1, NULL);
	CHECK_UINT(sip_dialog_remote_cseq_take(d, m), SIP_DLG_CSEQ_BAD);
}

/* ---------------------------------------------------------- request build */

static void build_tests(void)
{
	struct sip_dialog_req r;
	struct sip_dialog *d;
	struct sip_msg *m;
	struct sip_buf b;
	const char *wire;
	uint32_t cseq;
	char small[64];

	CASE("a BYE carries the dialog identifiers and the next CSeq");
	fake_reset();
	m = resp_take(200, R_TAG, 4, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	wire = build(d, SIP_METHOD_BYE, "z9hG4bK-bye1", false, NULL, NULL, &cseq);
	CHECK(strstr(wire, "BYE sip:5552000@192.0.2.10:5060 SIP/2.0") != NULL);
	CHECK(strstr(wire, "From: <" L_URI ">;tag=" L_TAG) != NULL);
	CHECK(strstr(wire, "To: <" R_URI ">;tag=" R_TAG) != NULL);
	CHECK(strstr(wire, "Call-ID: " CALL_ID) != NULL);
	CHECK(strstr(wire, "CSeq: 5 BYE") != NULL);
	CHECK(strstr(wire, "Max-Forwards: 70") != NULL);
	CHECK(strstr(wire, "Content-Length: 0") != NULL);
	CHECK(strstr(wire, "User-Agent: uvoice-test/1") != NULL);
	CHECK(strstr(wire, "Contact:") == NULL);
	CHECK_UINT(cseq, 5);

	CASE("every request takes the next number");
	wire = build(d, SIP_METHOD_INFO, "z9hG4bK-info1", false, NULL, NULL, &cseq);
	CHECK_UINT(cseq, 6);
	CHECK(strstr(wire, "CSeq: 6 INFO") != NULL);

	CASE("a re-INVITE carries our Contact and the offer");
	wire = build(d, SIP_METHOD_INVITE, "z9hG4bK-rei1", true, "application/sdp", "v=0\r\n",
		     &cseq);
	CHECK_UINT(cseq, 7);
	CHECK(strstr(wire, "INVITE sip:5552000@192.0.2.10:5060 SIP/2.0") != NULL);
	CHECK(strstr(wire, "Contact: <" L_CONTACT ">") != NULL);
	CHECK(strstr(wire, "Content-Type: application/sdp") != NULL);
	CHECK(strstr(wire, "Content-Length: 5") != NULL);
	CHECK(strstr(wire, "\r\n\r\nv=0\r\n") != NULL);

	CASE("an INFO body goes out with its own content type");
	wire = build(d, SIP_METHOD_INFO, "z9hG4bK-info2", false, "application/dtmf-relay",
		     "Signal=5\r\nDuration=180\r\n", &cseq);
	CHECK(strstr(wire, "Content-Type: application/dtmf-relay") != NULL);
	CHECK(strstr(wire, "Signal=5") != NULL);

	CASE("the ACK is not an ordinary in-dialog request");
	CHECK(!strcmp(build(d, SIP_METHOD_ACK, "z9hG4bK-a", false, NULL, NULL, NULL), ""));

	CASE("a request without a branch is refused");
	CHECK(!strcmp(build(d, SIP_METHOD_BYE, NULL, false, NULL, NULL, NULL), ""));

	CASE("a build that does not fit consumes no sequence number");
	cseq = d->local_cseq;
	memset(&r, 0, sizeof(r));
	r.method = SIP_METHOD_BYE;
	r.branch = "z9hG4bK-small";
	sip_buf_init(&b, small, sizeof(small));
	CHECK(!sip_dialog_req_build(d, &r, &b));
	CHECK_UINT(d->local_cseq, cseq);

	CASE("a tag longer than any fixed buffer goes out whole");
	fake_reset();
	{
		char tag[220], expect[280];

		memset(tag, 'y', sizeof(tag) - 1);
		tag[sizeof(tag) - 1] = 0;
		m = resp_take(200, tag, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
		d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
		CHECK(d != NULL);
		snprintf(expect, sizeof(expect), "To: <" R_URI ">;tag=%s\r\n", tag);
		wire = build(d, SIP_METHOD_BYE, "z9hG4bK-long", false, NULL, NULL, NULL);
		CHECK(strstr(wire, expect) != NULL);
	}

	CASE("a remote target too long for a request URI is refused, not truncated");
	fake_reset();
	{
		char host[600], contact[700];

		memset(host, 'h', sizeof(host) - 1);
		host[sizeof(host) - 1] = 0;
		snprintf(contact, sizeof(contact), "Contact: <sip:5552000@%s>\r\n", host);
		m = resp_take(200, R_TAG, 1, contact);
		d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
		CHECK(d != NULL);
		CHECK(!strcmp(build(d, SIP_METHOD_BYE, "z9hG4bK-b8", false, NULL, NULL, NULL),
			      ""));
	}

	CASE("a terminated dialog builds nothing");
	sip_dialog_terminate(d);
	CHECK(!strcmp(build(d, SIP_METHOD_BYE, "z9hG4bK-b9", false, NULL, NULL, NULL), ""));

	CASE("a dialog without a local host builds nothing");
	fake_reset();
	memset(&local, 0, sizeof(local));
	m = resp_take(200, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	CHECK(!strcmp(build(d, SIP_METHOD_BYE, "z9hG4bK-b1", false, NULL, NULL, NULL), ""));
}

/* ---------------------------------------------------- the PRACK, RFC 3262 4 */

/*
 * RFC 3262 4: a PRACK is an ordinary in-dialog request, so this layer builds
 * it like a BYE. It writes no RAck, because sip_dialog_req_build() takes no
 * extra header block; call.c splices the RAck into the built request.
 */
static void prack_tests(void)
{
	struct sip_dialog *d;
	struct sip_msg *m;
	const char *wire;
	uint32_t cseq;

	CASE("prack: the early dialog of a 180 builds one, without the RAck");
	fake_reset();
	m = resp_take(180, R_TAG, 2,
		      "Record-Route: <sip:p1.example.net;lr>\r\n"
		      "Record-Route: <sip:p2.example.net;lr>\r\n"
		      "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	CHECK(m != NULL);
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060),
				  &local);
	CHECK(d != NULL);
	CHECK_UINT(sip_dialog_state(d), SIP_DLG_EARLY);
	wire = build(d, SIP_METHOD_PRACK, "z9hG4bK-pr1", false, NULL, NULL,
		     &cseq);
	CHECK(strstr(wire, "PRACK sip:5552000@192.0.2.10:5060 SIP/2.0")
	      != NULL);
	CHECK(strstr(wire, "Route: <sip:p2.example.net;lr>\r\n"
			   "Route: <sip:p1.example.net;lr>\r\n") != NULL);
	CHECK(strstr(wire, "From: <" L_URI ">;tag=" L_TAG "\r\n") != NULL);
	CHECK(strstr(wire, "To: <" R_URI ">;tag=" R_TAG "\r\n") != NULL);
	CHECK(strstr(wire, "Call-ID: " CALL_ID "\r\n") != NULL);
	CHECK(strstr(wire, "CSeq: 3 PRACK\r\n") != NULL);
	CHECK(strstr(wire, "Content-Length: 0\r\n") != NULL);
	CHECK(strstr(wire, "Contact:") == NULL);
	CHECK(strstr(wire, "RAck:") == NULL);
	CHECK_UINT(cseq, 3);

	CASE("prack: a 1xx to the PRACK leaves the dialog untouched");
	m = msg_take("SIP/2.0 183 Session Progress\r\n"
		     "Via: SIP/2.0/UDP " L_HOST ":5060;branch=z9hG4bK-pr1\r\n"
		     "From: \"Line 1\" <" L_URI ">;tag=" L_TAG "\r\n"
		     "To: <" R_URI ">;tag=" R_TAG "\r\n"
		     "Call-ID: " CALL_ID "\r\n"
		     "CSeq: 3 PRACK\r\n"
		     "Record-Route: <sip:p9.example.net;lr>\r\n"
		     "Contact: <sip:5552000@198.51.100.9:5060>\r\n"
		     "Content-Length: 0\r\n\r\n");
	CHECK(m != NULL);
	CHECK(sip_dialog_uac_update(&set, m, NULL, &local) == NULL);
	CHECK(!strcmp(d->remote_target, "sip:5552000@192.0.2.10:5060"));
	CHECK_UINT(d->routes, 2);
	CHECK(!strcmp(d->route[0], "<sip:p2.example.net;lr>"));
	CHECK(!strcmp(d->route[1], "<sip:p1.example.net;lr>"));
	CHECK(!strcmp(d->remote_tag, R_TAG));
	CHECK_UINT(sip_dialog_state(d), SIP_DLG_EARLY);

	CASE("prack: a 1xx without a To tag leaves no dialog to build one in");
	fake_reset();
	m = resp_take(180, NULL, 2,
		      "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	CHECK(m != NULL);
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060),
				  &local);
	CHECK(d == NULL);
	CHECK_UINT(sip_dialog_count(&set), 0);
	CHECK(!strcmp(build(d, SIP_METHOD_PRACK, "z9hG4bK-pr2", false, NULL,
			    NULL, NULL), ""));

	fake_reset();
}

/* ------------------------------------------------------- the ACK, RFC 6026 */

static void ack_tests(void)
{
	struct sip_dialog *d;
	struct sip_msg *m;
	char first[WIRE_MAX];
	size_t first_len;

	CASE("the ACK of a 2xx goes out with the CSeq of its INVITE");
	fake_reset();
	m = resp_take(200, R_TAG, 3,
		      "Record-Route: <sip:p1.example.net;lr>\r\n"
		      "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	CHECK(sip_dialog_ack_send(d, m, "z9hG4bK-ack1", NULL, NULL, 0));
	CHECK_UINT(fk.sends, 1);
	CHECK(strstr(last_send(), "ACK sip:5552000@192.0.2.10:5060 SIP/2.0") != NULL);
	CHECK(strstr(last_send(), "CSeq: 3 ACK") != NULL);
	CHECK(strstr(last_send(), "To: <" R_URI ">;tag=" R_TAG) != NULL);
	CHECK(strstr(last_send(), "From: <" L_URI ">;tag=" L_TAG) != NULL);
	CHECK(strstr(last_send(), "branch=z9hG4bK-ack1") != NULL);
	CHECK(strstr(last_send(), "Route: <sip:p1.example.net;lr>") != NULL);
	CHECK(sip_dialog_ack_stored(d));
	CHECK_UINT(d->ack_cseq, 3);

	CASE("the local CSeq is untouched by the ACK");
	CHECK_UINT(d->local_cseq, 3);

	CASE("a retransmitted 2xx is answered with the same bytes");
	first_len = fk.send[0].len;
	memcpy(first, fk.send[0].buf, first_len);
	m = resp_take(200, R_TAG, 3, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_ABSORBED);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(fk.send[1].len, first_len);
	CHECK(!memcmp(fk.send[1].buf, first, first_len));
	CHECK_UINT(set.stat.reack, 1);

	CASE("a third copy is answered again");
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_ABSORBED);
	CHECK_UINT(fk.sends, 3);
	CHECK_UINT(set.stat.reack, 2);

	CASE("a 2xx of a CSeq we have no ACK for is absorbed without a send");
	m = resp_take(200, R_TAG, 9, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_ABSORBED);
	CHECK_UINT(fk.sends, 3);
	CHECK_UINT(set.stat.absorbed, 1);

	CASE("a 2xx of a dialog we do not have passes to the caller");
	m = resp_take(200, "another-fork", 3, "Contact: <sip:5552000@192.0.2.77:5060>\r\n");
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_PASS);
	CHECK_UINT(fk.sends, 3);

	CASE("a non 2xx and a request both pass");
	m = resp_take(180, R_TAG, 3, NULL);
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_PASS);
	m = invite_take(NULL);
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_PASS);

	CASE("a 2xx whose CSeq is not an INVITE passes");
	m = msg_take("SIP/2.0 200 OK\r\n"
		     "Via: SIP/2.0/UDP " L_HOST ":5060;branch=z9hG4bK-x\r\n"
		     "From: <" L_URI ">;tag=" L_TAG "\r\n"
		     "To: <" R_URI ">;tag=" R_TAG "\r\n"
		     "Call-ID: " CALL_ID "\r\n"
		     "CSeq: 3 BYE\r\n\r\n");
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_PASS);

	CASE("the ACK of a re-INVITE replaces the retained one");
	m = resp_take(200, R_TAG, 4, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	CHECK(sip_dialog_ack_send(d, m, "z9hG4bK-ack2", NULL, NULL, 0));
	CHECK_UINT(d->ack_cseq, 4);
	CHECK(strstr(last_send(), "CSeq: 4 ACK") != NULL);
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_ABSORBED);
	CHECK(strstr(last_send(), "CSeq: 4 ACK") != NULL);

	CASE("an ACK can carry the answer to an offerless INVITE");
	fake_reset();
	m = resp_take(200, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	CHECK(sip_dialog_ack_send(d, m, "z9hG4bK-ack3", "application/sdp", "v=0\r\n", 5));
	CHECK(strstr(last_send(), "Content-Type: application/sdp") != NULL);
	CHECK(strstr(last_send(), "Content-Length: 5") != NULL);

	CASE("the retained bytes survive a send that failed");
	fake_reset();
	m = resp_take(200, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	fk.send_fail = true;
	CHECK(!sip_dialog_ack_send(d, m, "z9hG4bK-ack4", NULL, NULL, 0));
	CHECK(sip_dialog_ack_stored(d));
	fk.send_fail = false;
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_ABSORBED);
	CHECK_UINT(fk.sends, 1);

	CASE("a non 2xx never has an ACK, in either orientation");
	fake_reset();
	m = invite_take(NULL);
	d = sip_dialog_uas_create(&set, m, "uas-tag", dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	m = uas_resp_take(487, 1, NULL);
	CHECK(!sip_dialog_ack_send(d, m, "z9hG4bK-a", NULL, NULL, 0));
	CHECK(!sip_dialog_ack_stored(d));
	CHECK_UINT(fk.sends, 0);
	fake_reset();
	m = resp_take(200, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	m = resp_take(487, R_TAG, 1, NULL);
	CHECK(!sip_dialog_ack_send(d, m, "z9hG4bK-a", NULL, NULL, 0));
	CHECK(!sip_dialog_ack_stored(d));

	CASE("an ACK without a destination is refused");
	fake_reset();
	m = resp_take(200, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, NULL, &local);
	CHECK(d != NULL);
	CHECK(sip_dialog_dst(d) == NULL);
	m = resp_take(200, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	CHECK(!sip_dialog_ack_send(d, m, "z9hG4bK-a", NULL, NULL, 0));
	CHECK_UINT(fk.sends, 0);
}

/* ------------------------------- the ACK inside a dialog the peer created */

/*
 * A re-INVITE from this side inside a dialog that the peer created needs the
 * same 2xx ACK as one inside our own dialog. Without that ACK, every hold and
 * every resume stalls: the peer retransmits its 200 for 32 s and answers the
 * next re-INVITE with 500 Overlapping Requests.
 */
static void uas_ack_tests(void)
{
	struct sip_dialog *d;
	struct sip_msg *m;
	char first[WIRE_MAX];
	size_t first_len;
	uint32_t cseq, h;

	CASE("uas ack: the 2xx of our re-INVITE in a peer dialog is acknowledged");
	fake_reset();
	m = invite_take("Record-Route: <sip:p1.example.net;lr>\r\n"
			"Record-Route: <sip:p2.example.net;lr>\r\n");
	CHECK(m != NULL);
	d = sip_dialog_uas_create(&set, m, "uas-tag", dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	sip_dialog_confirm(d);
	CHECK(strstr(build(d, SIP_METHOD_INVITE, "z9hG4bK-uas-rei", true, "application/sdp",
			   "v=0\r\n", &cseq), "CSeq: 1 INVITE") != NULL);
	CHECK_UINT(cseq, 1);
	m = uas_resp_take(200, cseq, "Contact: <sip:5552000@192.0.2.11:5060>\r\n");
	CHECK(m != NULL);
	CHECK(sip_dialog_target_refresh(d, m));
	CHECK(sip_dialog_ack_send(d, m, "z9hG4bK-uas-ack1", NULL, NULL, 0));
	CHECK_UINT(fk.sends, 1);
	CHECK(sip_dialog_ack_stored(d));
	CHECK_UINT(d->ack_cseq, 1);

	CASE("uas ack: the request URI is the remote target the 2xx moved");
	CHECK(strstr(last_send(), "ACK sip:5552000@192.0.2.11:5060 SIP/2.0") != NULL);

	CASE("uas ack: the From is ours with our tag, the To the peer with its tag");
	CHECK(strstr(last_send(), "From: <" L_URI ">;tag=uas-tag\r\n") != NULL);
	CHECK(strstr(last_send(), "To: <" R_URI ">;tag=" R_TAG "\r\n") != NULL);
	CHECK(strstr(last_send(), "Call-ID: " CALL_ID "\r\n") != NULL);

	CASE("uas ack: the CSeq is the number of the re-INVITE with the ACK method");
	CHECK(strstr(last_send(), "CSeq: 1 ACK\r\n") != NULL);
	CHECK_UINT(d->local_cseq, 1);

	CASE("uas ack: the route set goes out in the order the INVITE recorded");
	CHECK(strstr(last_send(), "Route: <sip:p1.example.net;lr>\r\n"
				  "Route: <sip:p2.example.net;lr>\r\n") != NULL);
	CHECK(strstr(last_send(), "branch=z9hG4bK-uas-ack1") != NULL);

	CASE("uas ack: a retransmitted 2xx is answered with the same bytes");
	first_len = fk.send[0].len;
	memcpy(first, fk.send[0].buf, first_len);
	m = uas_resp_take(200, 1, "Contact: <sip:5552000@192.0.2.11:5060>\r\n");
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_ABSORBED);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(fk.send[1].len, first_len);
	CHECK(!memcmp(fk.send[1].buf, first, first_len));
	CHECK_UINT(set.stat.reack, 1);

	CASE("uas ack: a 2xx of a CSeq we have no ACK for is absorbed without a send");
	m = uas_resp_take(200, 9, "Contact: <sip:5552000@192.0.2.11:5060>\r\n");
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_ABSORBED);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(set.stat.absorbed, 1);

	CASE("uas ack: a 2xx that names another local tag is not this dialog");
	m = resp_tags_take(200, "someone-else", R_TAG, 1, NULL);
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_PASS);
	CHECK_UINT(fk.sends, 2);

	CASE("uas ack: a terminated peer dialog still answers the retransmission");
	h = sip_dialog_handle(d);
	sip_dialog_hold(d);
	sip_dialog_terminate(d);
	CHECK_UINT(sip_dialog_state(d), SIP_DLG_TERMINATED);
	m = uas_resp_take(200, 1, "Contact: <sip:5552000@192.0.2.11:5060>\r\n");
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_ABSORBED);
	CHECK_UINT(fk.sends, 3);
	CHECK_UINT(fk.send[2].len, first_len);
	CHECK(!memcmp(fk.send[2].buf, first, first_len));
	CHECK_UINT(set.stat.reack, 2);

	CASE("uas ack: once the last reference goes the 2xx passes again");
	sip_dialog_put(d);
	sip_dialog_put(d);
	CHECK(sip_dialog_get(&set, h) == NULL);
	m = uas_resp_take(200, 1, "Contact: <sip:5552000@192.0.2.11:5060>\r\n");
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_PASS);
	CHECK_UINT(fk.sends, 3);

	CASE("uas ack: the retained bytes survive a send that failed");
	fake_reset();
	m = invite_take(NULL);
	d = sip_dialog_uas_create(&set, m, "uas-tag", dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	m = uas_resp_take(200, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	fk.send_fail = true;
	CHECK(!sip_dialog_ack_send(d, m, "z9hG4bK-uas-ack2", NULL, NULL, 0));
	CHECK(sip_dialog_ack_stored(d));
	fk.send_fail = false;
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_ABSORBED);
	CHECK_UINT(fk.sends, 1);

	CASE("uas ack: an ACK can carry the answer to an offerless re-INVITE");
	fake_reset();
	m = invite_take(NULL);
	d = sip_dialog_uas_create(&set, m, "uas-tag", dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	m = uas_resp_take(200, 2, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	CHECK(sip_dialog_ack_send(d, m, "z9hG4bK-uas-ack3", "application/sdp", "v=0\r\n", 5));
	CHECK(strstr(last_send(), "CSeq: 2 ACK\r\n") != NULL);
	CHECK(strstr(last_send(), "Content-Type: application/sdp") != NULL);
	CHECK(strstr(last_send(), "Content-Length: 5") != NULL);

	fake_reset();
}

/* ------------------------------------------------------ handles, lifetime */

static void lifetime_tests(void)
{
	struct sip_dialog *d;
	struct sip_msg *m;
	uint32_t h;
	unsigned int i;

	CASE("a handle resolves to its dialog and dies with it");
	fake_reset();
	m = resp_take(200, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	h = sip_dialog_handle(d);
	CHECK(h != SIP_HANDLE_NONE);
	CHECK(sip_dialog_get(&set, h) == d);
	sip_dialog_put(d);
	CHECK(sip_dialog_get(&set, h) == NULL);
	CHECK_UINT(sip_dialog_count(&set), 0);

	CASE("a handle of the wrong generation resolves to nothing");
	m = resp_take(200, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	CHECK(sip_dialog_handle(d) != h);
	CHECK(sip_dialog_get(&set, h) == NULL);
	CHECK(sip_dialog_get(&set, SIP_HANDLE_NONE) == NULL);

	CASE("a transaction reference keeps the dialog alive past its owner");
	h = sip_dialog_handle(d);
	sip_dialog_hold(d);
	sip_dialog_hold(d);
	sip_dialog_put(d);
	CHECK(sip_dialog_get(&set, h) == d);
	sip_dialog_put(d);
	CHECK(sip_dialog_get(&set, h) == d);
	sip_dialog_put(d);
	CHECK(sip_dialog_get(&set, h) == NULL);

	CASE("a terminated dialog holds its memory until the last reference");
	m = resp_take(200, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	h = sip_dialog_handle(d);
	sip_dialog_hold(d);
	sip_dialog_terminate(d);
	CHECK(sip_dialog_get(&set, h) == d);
	CHECK_UINT(sip_dialog_count(&set), 1);
	sip_dialog_put(d);
	sip_dialog_put(d);
	CHECK(sip_dialog_get(&set, h) == NULL);

	CASE("an extra put on a released dialog is harmless");
	sip_dialog_put(d);
	CHECK_UINT(sip_dialog_count(&set), 0);

	CASE("the pool refuses more than SIP_DIALOG_MAX dialogs");
	fake_reset();
	for (i = 0; i < SIP_DIALOG_MAX; i++) {
		char tag[32];

		snprintf(tag, sizeof(tag), "peer-%u", i);
		m = resp_take(200, tag, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
		CHECK(sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060),
					    &local) != NULL);
	}
	CHECK_UINT(sip_dialog_count(&set), SIP_DIALOG_MAX);
	m = resp_take(200, "one-too-many", 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	CHECK(sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local) == NULL);
	CHECK_UINT(set.stat.denied, 1);

	CASE("the set frees every dialog it still holds");
	sip_dialog_set_done(&set);
	CHECK_UINT(sip_dialog_count(&set), 0);

	CASE("state names exist for every state");
	CHECK(!strcmp(sip_dialog_state_name(SIP_DLG_EARLY), "early"));
	CHECK(!strcmp(sip_dialog_state_name(SIP_DLG_CONFIRMED), "confirmed"));
	CHECK(!strcmp(sip_dialog_state_name(SIP_DLG_TERMINATED), "terminated"));
}

/* --------------- target refresh, tagless 2xx, dead dialogs, route limit */

static void sol_tests(void)
{
	struct sip_dialog *d, *dlg2;
	struct sip_msg *m;
	char first[WIRE_MAX];
	size_t first_len;
	uint32_t h;

	/*
	 * RFC 3261 12.2.2: an accepted INVITE or UPDATE is a target refresh
	 * request. Without it a peer that moves behind a re-INVITE keeps
	 * receiving our BYE at the address it left.
	 */
	CASE("an accepted in-dialog re-INVITE moves the remote target");
	fake_reset();
	m = invite_take(NULL);
	d = sip_dialog_uas_create(&set, m, "uas-tag", dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	CHECK(!strcmp(d->remote_target, "sip:5552000@192.0.2.10:5060"));
	m = indialog_extra("INVITE", R_TAG, "uas-tag", 8,
			   "Contact: <sip:5552000@192.0.2.55:5070>\r\n");
	CHECK_UINT(sip_dialog_remote_cseq_take(d, m), SIP_DLG_CSEQ_OK);
	CHECK(sip_dialog_target_refresh(d, m));
	CHECK(!strcmp(d->remote_target, "sip:5552000@192.0.2.55:5070"));

	CASE("the next request goes to the new target");
	CHECK(strstr(build(d, SIP_METHOD_BYE, "z9hG4bK-j4", false, NULL, NULL, NULL),
		     "BYE sip:5552000@192.0.2.55:5070 SIP/2.0") != NULL);

	CASE("a request that is not a target refresh leaves the target alone");
	m = indialog_extra("INFO", R_TAG, "uas-tag", 9,
			   "Contact: <sip:5552000@198.51.100.9:5060>\r\n");
	CHECK_UINT(sip_dialog_remote_cseq_take(d, m), SIP_DLG_CSEQ_OK);
	CHECK(!strcmp(d->remote_target, "sip:5552000@192.0.2.55:5070"));

	CASE("a re-INVITE the sequence check refuses moves nothing");
	m = indialog_extra("INVITE", R_TAG, "uas-tag", 5,
			   "Contact: <sip:5552000@198.51.100.9:5060>\r\n");
	CHECK_UINT(sip_dialog_remote_cseq_take(d, m), SIP_DLG_CSEQ_OLD);
	CHECK(!strcmp(d->remote_target, "sip:5552000@192.0.2.55:5070"));

	CASE("an UPDATE refreshes the target as well (RFC 3311)");
	m = indialog_extra("UPDATE", R_TAG, "uas-tag", 10,
			   "Contact: <sip:5552000@203.0.113.4:5060>\r\n");
	CHECK_UINT(sip_dialog_remote_cseq_take(d, m), SIP_DLG_CSEQ_OK);
	CHECK(sip_dialog_target_refresh(d, m));
	CHECK(!strcmp(d->remote_target, "sip:5552000@203.0.113.4:5060"));

	/*
	 * Passing the sequence check is not acceptance:
	 * RFC 6141 keeps the target of a target refresh request that was not
	 * answered 2xx, so the check moves nothing by itself.
	 */
	CASE("the sequence check alone does not move the target");
	m = indialog_extra("INVITE", R_TAG, "uas-tag", 11,
			   "Contact: <sip:5552000@198.51.100.77:5060>\r\n");
	CHECK_UINT(sip_dialog_remote_cseq_take(d, m), SIP_DLG_CSEQ_OK);
	CHECK(!strcmp(d->remote_target, "sip:5552000@203.0.113.4:5060"));

	CASE("a re-INVITE we refuse leaves the peer where it was");
	CHECK(strstr(build(d, SIP_METHOD_BYE, "z9hG4bK-j4b", false, NULL, NULL, NULL),
		     "BYE sip:5552000@203.0.113.4:5060 SIP/2.0") != NULL);

	CASE("accepting the same request afterwards moves it");
	CHECK(sip_dialog_target_refresh(d, m));
	CHECK(!strcmp(d->remote_target, "sip:5552000@198.51.100.77:5060"));

	CASE("a refresh without a Contact reports false and moves nothing");
	m = indialog_extra("INVITE", R_TAG, "uas-tag", 12, NULL);
	CHECK_UINT(sip_dialog_remote_cseq_take(d, m), SIP_DLG_CSEQ_OK);
	CHECK(!sip_dialog_target_refresh(d, m));
	CHECK(!strcmp(d->remote_target, "sip:5552000@198.51.100.77:5060"));

	CASE("a response Contact refreshes the target of a confirmed dialog");
	fake_reset();
	m = resp_take(200, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	m = resp_take(200, R_TAG, 2, "Contact: <sip:5552000@203.0.113.9:5080>\r\n");
	CHECK(sip_dialog_target_refresh(d, m));
	CHECK(!strcmp(d->remote_target, "sip:5552000@203.0.113.9:5080"));

	CASE("a refresh with no dialog or no message is refused");
	CHECK(!sip_dialog_target_refresh(NULL, m));
	CHECK(!sip_dialog_target_refresh(d, NULL));

	/*
	 * A peer that answers without a To tag retransmits the same tagless
	 * 200, which has to find the dialog its first copy confirmed.
	 */
	CASE("a retransmitted tagless 200 finds the dialog the 18x built");
	fake_reset();
	m = resp_take(180, R_TAG, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	m = resp_take(200, NULL, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	CHECK(sip_dialog_uac_update(&set, m, NULL, &local) == d);
	CHECK(sip_dialog_ack_send(d, m, "z9hG4bK-j8", NULL, NULL, 0));
	CHECK_UINT(fk.sends, 1);
	first_len = fk.send[0].len;
	memcpy(first, fk.send[0].buf, first_len);
	m = resp_take(200, NULL, 1, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_ABSORBED);
	CHECK_UINT(fk.sends, 2);
	CHECK_UINT(fk.send[1].len, first_len);
	CHECK(!memcmp(fk.send[1].buf, first, first_len));
	CHECK_UINT(set.stat.reack, 1);

	/*
	 * RFC 6026 outlives the call: the peer retransmits its 2xx for
	 * 64*T1 and the owner may already have left the dialog.
	 */
	CASE("a terminated dialog still answers a retransmitted 2xx");
	fake_reset();
	m = resp_take(200, R_TAG, 3, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
	CHECK(d != NULL);
	CHECK(sip_dialog_ack_send(d, m, "z9hG4bK-j9", NULL, NULL, 0));
	first_len = fk.send[0].len;
	memcpy(first, fk.send[0].buf, first_len);
	h = sip_dialog_handle(d);
	sip_dialog_hold(d);
	sip_dialog_terminate(d);
	CHECK_UINT(sip_dialog_state(d), SIP_DLG_TERMINATED);
	m = resp_take(200, R_TAG, 3, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_ABSORBED);
	CHECK_UINT(fk.sends, 2);
	CHECK(!memcmp(fk.send[1].buf, first, first_len));

	CASE("a terminated dialog is still not matched as a new one");
	dlg2 = sip_dialog_uac_update(&set, m, NULL, &local);
	CHECK(dlg2 != NULL && dlg2 != d);
	CHECK_UINT(set.stat.replaced, 0);
	sip_dialog_terminate(dlg2);
	sip_dialog_put(dlg2);
	CHECK_UINT(sip_dialog_count(&set), 1);

	CASE("once the last reference goes the record is gone and it passes");
	sip_dialog_put(d);
	sip_dialog_put(d);
	CHECK(sip_dialog_get(&set, h) == NULL);
	fk.sends = 0;
	m = resp_take(200, R_TAG, 3, "Contact: <sip:5552000@192.0.2.10:5060>\r\n");
	CHECK_UINT(sip_dialog_resp_recv(&set, m), SIP_DLG_RX_PASS);
	CHECK_UINT(fk.sends, 0);

	/*
	 * A truncated route set routes past proxies the peer put in the
	 * chain, and a BYE that never arrives leaves the call up on the far
	 * side.
	 */
	CASE("a route set at the limit is accepted whole");
	fake_reset();
	{
		char extra[TEXT_MAX];

		snprintf(extra, sizeof(extra), "%sContact: <sip:5552000@192.0.2.10:5060>\r\n",
			 record_routes(SIP_ROUTE_MAX));
		m = resp_take(200, R_TAG, 1, extra);
		d = sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060), &local);
		CHECK(d != NULL);
		CHECK_UINT(d->routes, SIP_ROUTE_MAX);
		CHECK(!d->route_overflow);
	}

	CASE("one hop more refuses the dialog instead of truncating it");
	fake_reset();
	{
		char extra[TEXT_MAX];

		snprintf(extra, sizeof(extra), "%sContact: <sip:5552000@192.0.2.10:5060>\r\n",
			 record_routes(SIP_ROUTE_MAX + 1));
		m = resp_take(200, R_TAG, 1, extra);
		CHECK(sip_dialog_uac_update(&set, m, dst_make("192.0.2.10", 5060),
					    &local) == NULL);
		CHECK_UINT(sip_dialog_count(&set), 0);
		CHECK_UINT(set.stat.denied, 1);
	}

	CASE("the UAS side refuses the same chain");
	fake_reset();
	{
		char extra[TEXT_MAX];

		snprintf(extra, sizeof(extra), "%s", record_routes(SIP_ROUTE_MAX + 1));
		m = invite_take(extra);
		CHECK(sip_dialog_uas_create(&set, m, "uas-tag", dst_make("192.0.2.10", 5060),
					    &local) == NULL);
		CHECK_UINT(sip_dialog_count(&set), 0);
		CHECK_UINT(set.stat.denied, 1);
	}

	CASE("and accepts the chain one hop shorter");
	fake_reset();
	{
		char extra[TEXT_MAX];

		snprintf(extra, sizeof(extra), "%s", record_routes(SIP_ROUTE_MAX));
		m = invite_take(extra);
		d = sip_dialog_uas_create(&set, m, "uas-tag", dst_make("192.0.2.10", 5060),
					  &local);
		CHECK(d != NULL);
		CHECK_UINT(d->routes, SIP_ROUTE_MAX);
	}

	fake_reset();
}

int main(void)
{
	fake_reset();

	uac_tests();
	route_tests();
	uas_tests();
	match_tests();
	cseq_tests();
	build_tests();
	prack_tests();
	ack_tests();
	uas_ack_tests();
	sol_tests();
	lifetime_tests();

	sip_dialog_set_done(&set);

	return tu_summary("test_sip_dialog");
}
