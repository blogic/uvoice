// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * UA core duties outside the transaction layer. See sip/ua.h.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/sip/ua.h"

/*
 * One Require header value reaches SIP_HDR_LEN_MAX before the parser even
 * flags it, and the unsupported subset is never longer than the input. The
 * slack covers the ", " separators when several header lines contribute.
 */
#define UA_REQUIRE_LIST_MAX	(SIP_HDR_LEN_MAX + 64)

/* RFC 3261 14.1 draws the delay in units of 10 ms from two ranges. */
#define GLARE_OWNER_BASE_MS	2100
#define GLARE_OWNER_STEPS	191
#define GLARE_PEER_STEPS	201
#define GLARE_STEP_MS		10

const char *ua_policy_name(enum ua_policy policy)
{
	switch (policy) {
	case UA_POLICY_PASS:
		return "pass";
	case UA_POLICY_ANSWERED:
		return "answered";
	case UA_POLICY_DROP:
		return "drop";
	case UA_POLICY_FAILED:
		return "failed";
	}

	return "?";
}

const char *ua_rx_name(enum ua_rx rx)
{
	switch (rx) {
	case UA_RX_PASS:
		return "pass";
	case UA_RX_ABSORBED:
		return "absorbed";
	case UA_RX_ACK:
		return "ack";
	}

	return "?";
}

static void ua_trace(struct ua *ua, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void ua_trace(struct ua *ua, const char *fmt, ...)
{
	struct sip_txn_env *env = ua->env;
	va_list ap;

	if (!env->base.trace)
		return;

	va_start(ap, fmt);
	sip_txn_env_trace(env, fmt, ap, "%s", "");
	va_end(ap);
}

/* ------------------------------------------------------------- token lists */

static bool span_eq(struct sip_str a, struct sip_str b, bool ci)
{
	size_t i;

	if (a.len != b.len || !a.p || !b.p)
		return false;
	if (!ci)
		return !memcmp(a.p, b.p, a.len);

	for (i = 0; i < a.len; i++) {
		if (tolower((unsigned char)a.p[i]) != tolower((unsigned char)b.p[i]))
			return false;
	}

	return true;
}

static bool token_list_has(const char *list, struct sip_str tok, bool ci)
{
	struct sip_str rest, item;

	if (!list || !tok.len)
		return false;

	rest = sip_str_cstr(list);
	while (sip_csv_next(&rest, &item)) {
		if (span_eq(sip_str_trim(item), tok, ci))
			return true;
	}

	return false;
}

static bool span_eq_cstr(struct sip_str s, const char *lit, bool ci)
{
	return lit && span_eq(s, sip_str_cstr(lit), ci);
}

/*
 * A stored identifier is NUL free, so a span that carries an embedded NUL can
 * never equal one (sip_str_nul_free(), sip/txn.h).
 */
static char *str_dup(struct sip_str s)
{
	char *p;

	if (!s.len || !sip_str_nul_free(s))
		return NULL;

	p = malloc(s.len + 1);
	if (!p)
		return NULL;

	memcpy(p, s.p, s.len);
	p[s.len] = 0;

	return p;
}

static bool cseq_method_is(const struct sip_msg *msg, enum sip_method want)
{
	enum sip_method method;
	uint32_t num;

	return hdr_cseq_get(msg, &num, &method) && method == want;
}

/* --------------------------------------------------------------- lifetime */

void ua_init(struct ua *ua, struct sip_txn_mgr *mgr, struct sip_txn_env *env,
	     const struct ua_cfg *cfg, const struct ua_cb *cb, void *priv)
{
	memset(ua, 0, sizeof(*ua));
	ua->mgr = mgr;
	ua->env = env;
	ua->priv = priv;

	if (cfg)
		ua->cfg = *cfg;
	if (cb)
		ua->cb = *cb;

	if (!ua->cfg.allow)
		ua->cfg.allow = UA_ALLOW_DEFAULT;
	if (!ua->cfg.allow_events)
		ua->cfg.allow_events = UA_ALLOW_EVENTS_DEFAULT;
	if (!ua->cfg.supported)
		ua->cfg.supported = UA_SUPPORTED_DEFAULT;
	if (!ua->cfg.accept)
		ua->cfg.accept = UA_ACCEPT_DEFAULT;

	/*
	 * The Allow list and the policy read one served set, so a list that
	 * names REGISTER would advertise a method the policy answers 403. The
	 * conflict is resolved here, once, rather than in the policy branch.
	 */
	if (!token_list_has(ua->cfg.allow, sip_str_cstr("REGISTER"), false))
		return;

	ua->cfg.allow = UA_ALLOW_DEFAULT;
	ua->allow_conflict = true;
}

static void accept_ident_free(struct ua_accept *ac)
{
	free(ac->call_id);
	free(ac->from_tag);
	free(ac->to_tag);
	free(ac->branch);
	free(ac->host);
	ac->call_id = NULL;
	ac->from_tag = NULL;
	ac->to_tag = NULL;
	ac->branch = NULL;
	ac->host = NULL;
}

static void accept_free(struct ua_accept *ac)
{
	struct sip_txn_env *env = ac->ua->env;

	sip_timer_cancel(env, &ac->retx);
	sip_timer_cancel(env, &ac->life);
	accept_ident_free(ac);
	free(ac->wire);
	ac->wire = NULL;
	ac->len = 0;
	ac->in_use = false;
}

void ua_done(struct ua *ua)
{
	unsigned int i;

	for (i = 0; i < UA_ACCEPT_MAX; i++) {
		if (ua->accept[i].in_use)
			accept_free(&ua->accept[i]);
	}

	for (i = 0; i < UA_GLARE_MAX; i++) {
		if (!ua->glare[i].in_use)
			continue;

		sip_timer_cancel(ua->env, &ua->glare[i].timer);
		ua->glare[i].in_use = false;
	}

	for (i = 0; i < UA_TRYING_MAX; i++) {
		if (!ua->trying[i].in_use)
			continue;

		sip_timer_cancel(ua->env, &ua->trying[i].timer);
		ua->trying[i].in_use = false;
	}
}

bool ua_tag_new(struct ua *ua, char *out, size_t olen)
{
	if (!ua || !out || olen < UA_TAG_MAX)
		return false;

	ua->tag_seq++;
	snprintf(out, olen, "%08x%04x", sip_txn_rand(ua->mgr), ua->tag_seq & 0xffffu);

	return true;
}

/* --------------------------------------------------------- response sending */

bool ua_response_send(struct ua *ua, struct sip_txn *txn, const struct sip_msg *req,
		      const struct ua_resp *r)
{
	char tag[UA_TAG_MAX];
	const char *to_tag;
	struct sip_buf b;

	if (!ua || !txn || !req || !r)
		return false;

	to_tag = r->to_tag;
	if (!to_tag && r->code > 100) {
		if (!ua_tag_new(ua, tag, sizeof(tag)))
			return false;
		to_tag = tag;
	}

	sip_buf_init(&b, ua->out, sizeof(ua->out));
	if (!msg_response_build(&b, req, r->code, r->reason, to_tag, 0))
		return false;

	if (r->contact)
		hdr_addr_write(&b, SIP_HDR_CONTACT, NULL, r->contact, NULL);
	if (r->allow)
		hdr_write(&b, SIP_HDR_ALLOW, "%s", r->allow);
	if (r->allow_events)
		hdr_write(&b, SIP_HDR_ALLOW_EVENTS, "%s", r->allow_events);
	if (r->supported)
		hdr_write(&b, SIP_HDR_SUPPORTED, "%s", r->supported);
	if (r->unsupported)
		hdr_write(&b, SIP_HDR_UNSUPPORTED, "%s", r->unsupported);
	if (r->accept)
		hdr_write(&b, SIP_HDR_ACCEPT, "%s", r->accept);
	if (r->has_retry_after)
		hdr_write(&b, SIP_HDR_RETRY_AFTER, "%u", r->retry_after);

	if (!msg_body_write(&b, NULL, NULL, 0))
		return false;

	return sip_txn_response_send(txn, r->code, sip_buf_str(&b), sip_buf_len(&b));
}

/* --------------------------------------------------------- inbound policy */

enum ua_require {
	UA_REQUIRE_OK = 0,
	UA_REQUIRE_UNSUPPORTED,
	UA_REQUIRE_OVERFLOW,
};

/*
 * RFC 3261 8.2.2.3. CANCEL and ACK carry no Require processing: a CANCEL is
 * always answered 200 and an ACK is never answered at all.
 *
 * The list must be complete or not used at all. A truncated Unsupported header
 * claims support for the tags it drops, and an empty one (the first tag alone
 * overflowed) tells the peer nothing. A list that does not fit is reported
 * separately, so the caller can refuse the request instead of answering it
 * with a lie.
 */
static enum ua_require require_check(struct ua *ua, const struct sip_msg *req, char *out,
				     size_t olen)
{
	struct sip_val_iter it;
	struct sip_str val, head;
	struct sip_buf b;
	bool any = false;

	sip_buf_init(&b, out, olen);
	if (!hdr_val_first(req, SIP_HDR_REQUIRE, &it, &val))
		return UA_REQUIRE_OK;

	do {
		sip_value_split(val, &head, NULL);
		head = sip_str_trim(head);
		if (!head.len || token_list_has(ua->cfg.supported, head, true))
			continue;

		if (any)
			sip_buf_add(&b, ", ");
		sip_buf_add_str(&b, head);
		any = true;
	} while (hdr_val_next(&it, &val));

	if (!any)
		return UA_REQUIRE_OK;

	return sip_buf_ok(&b) ? UA_REQUIRE_UNSUPPORTED : UA_REQUIRE_OVERFLOW;
}

/*
 * Send an automatic response and report what became of the transaction. A
 * response that cannot be built (an oversized Via chain) or cannot be sent
 * leaves a server transaction that answered nothing, and such a transaction
 * holds no timer that would ever free it, so it is discarded here. The handle
 * round trip covers the case where sip_txn_response_send() already discarded
 * it on a transport error.
 */
static enum ua_policy policy_answer(struct ua *ua, struct sip_txn *txn,
				    const struct sip_msg *req, const struct ua_resp *r)
{
	uint32_t handle = sip_txn_handle(txn);

	if (ua_response_send(ua, txn, req, r))
		return UA_POLICY_ANSWERED;

	ua_trace(ua, "response %u refused, transaction discarded", r->code);
	txn = sip_txn_get(ua->mgr, handle);
	if (txn)
		sip_txn_discard(txn);

	return UA_POLICY_FAILED;
}

enum ua_policy ua_request_policy(struct ua *ua, const struct sip_msg *req, struct sip_txn *txn)
{
	char unsupported[UA_REQUIRE_LIST_MAX];
	enum ua_require rq;
	struct ua_resp r;

	if (!ua || !req || !req->is_request || !txn)
		return UA_POLICY_PASS;

	/* An ACK is never answered (RFC 3261 section 17). */
	if (req->method == SIP_METHOD_ACK)
		return UA_POLICY_DROP;

	memset(&r, 0, sizeof(r));

	/*
	 * The Allow list is the served set, and both branches below read it,
	 * so what uvoice advertises and what it answers cannot drift apart. A
	 * rogue REGISTER differs only in the code: 405 invites the sender to
	 * retry with another method, 403 does not.
	 */
	if (!token_list_has(ua->cfg.allow, req->method_name, false)) {
		r.code = req->method == SIP_METHOD_REGISTER ? 403 : 405;
		if (r.code == 405) {
			r.allow = ua->cfg.allow;
			r.allow_events = ua->cfg.allow_events;
		}

		return policy_answer(ua, txn, req, &r);
	}

	rq = req->method == SIP_METHOD_CANCEL ? UA_REQUIRE_OK :
	     require_check(ua, req, unsupported, sizeof(unsupported));

	if (rq == UA_REQUIRE_UNSUPPORTED) {
		r.code = 420;
		r.unsupported = unsupported;
		r.supported = ua->cfg.supported;
		ua_trace(ua, "420 unsupported: %s", unsupported);

		return policy_answer(ua, txn, req, &r);
	}

	if (rq == UA_REQUIRE_OVERFLOW) {
		/* The option tags do not fit any response we could build, so
		 * the request is unusable rather than merely unsupported. */
		r.code = 400;
		ua_trace(ua, "Require list too long to enumerate");

		return policy_answer(ua, txn, req, &r);
	}

	if (req->method == SIP_METHOD_OPTIONS) {
		/* RFC 3261 11.2 wants the capability set in the 200. */
		r.code = 200;
		r.allow = ua->cfg.allow;
		r.allow_events = ua->cfg.allow_events;
		r.supported = ua->cfg.supported;
		r.accept = ua->cfg.accept;

		return policy_answer(ua, txn, req, &r);
	}

	return UA_POLICY_PASS;
}

bool ua_cancel_handle(struct ua *ua, const struct sip_msg *cancel, struct sip_txn *cancel_txn,
		      const char *to_tag)
{
	struct sip_msg invite;
	struct sip_txn *target;
	struct ua_resp r;
	uint32_t handle;

	if (!ua || !cancel || !cancel_txn)
		return false;

	memset(&r, 0, sizeof(r));
	handle = sip_txn_cancel_target(ua->mgr, cancel);
	target = sip_txn_get(ua->mgr, handle);

	/* RFC 3261 9.2: no matching transaction is a 481. */
	if (!target) {
		r.code = 481;
		policy_answer(ua, cancel_txn, cancel, &r);
		return false;
	}

	/*
	 * The CANCEL transaction belongs to the UA core, so a refused 200
	 * discards it. The INVITE transaction below belongs to the call layer
	 * and is left alone.
	 */
	r.code = 200;
	if (policy_answer(ua, cancel_txn, cancel, &r) != UA_POLICY_ANSWERED)
		return false;

	target = sip_txn_get(ua->mgr, handle);
	if (!target || !sip_txn_request_get(target, &invite))
		return false;

	/*
	 * The 487 fails when the INVITE already has a final response, which is
	 * the RFC 3261 9.2 case where the CANCEL has no effect and only the
	 * 200 above is owed.
	 */
	r.code = 487;
	r.to_tag = to_tag;

	return ua_response_send(ua, target, &invite, &r);
}

/* ------------------------------------------------------- automatic 100 */

/*
 * RFC 3261 17.2.1. The INVITE server transaction owes a 100 Trying 200 ms
 * after it starts, unless the transaction user answered by itself first. The
 * test is the state of the transaction, not a flag this record keeps: a
 * provisional or a final that went out through any path sets txn->code, and a
 * transaction that is already gone resolves to NULL.
 */
static void trying_expire(struct sip_timer *t)
{
	struct ua_trying *tr = t->owner;
	struct ua *ua = tr->ua;
	struct sip_txn *txn = sip_txn_get(ua->mgr, tr->txn);
	struct sip_msg req;
	struct ua_resp r;

	tr->in_use = false;

	if (!txn || txn->kind != SIP_TXN_IST || txn->state != SIP_TXN_S_PROCEEDING)
		return;

	if (txn->code) {
		ua->stat.trying_skipped++;
		return;
	}

	if (!sip_txn_request_get(txn, &req))
		return;

	memset(&r, 0, sizeof(r));
	r.code = 100;
	ua->stat.trying_sent++;
	ua_trace(ua, "100 Trying after %u ms", UA_TRYING_MS);

	/* A refused 100 costs nothing: the peer retransmits the INVITE and the
	 * transaction answers that retransmission by itself. */
	ua_response_send(ua, txn, &req, &r);
}

bool ua_trying_arm(struct ua *ua, struct sip_txn *txn)
{
	struct ua_trying *tr = NULL;
	unsigned int i;

	if (!ua || !txn || txn->kind != SIP_TXN_IST || txn->code)
		return false;

	for (i = 0; i < UA_TRYING_MAX && !tr; i++) {
		if (!ua->trying[i].in_use)
			tr = &ua->trying[i];
	}
	if (!tr) {
		ua->stat.denied++;
		return false;
	}

	memset(tr, 0, sizeof(*tr));
	tr->ua = ua;
	tr->txn = sip_txn_handle(txn);
	tr->owner = txn->owner;
	tr->in_use = true;
	sip_timer_init(&tr->timer, tr, SIP_TIMER_TRYING, trying_expire);
	sip_timer_arm(ua->env, &tr->timer, UA_TRYING_MS);

	return true;
}

void ua_trying_cancel(struct ua *ua, uint32_t txn)
{
	unsigned int i;

	if (!ua || txn == SIP_HANDLE_NONE)
		return;

	for (i = 0; i < UA_TRYING_MAX; i++) {
		struct ua_trying *tr = &ua->trying[i];

		if (!tr->in_use || tr->txn != txn)
			continue;

		sip_timer_cancel(ua->env, &tr->timer);
		tr->in_use = false;
	}
}

/* ------------------------------------------- 2xx retransmission (13.3.1.4) */

/*
 * The identity of the accepted INVITE, taken from the 2xx: the dialog fields
 * match its ACK, the Via fields match a retransmission of the INVITE itself.
 * A 2xx whose CSeq does not say INVITE is not an INVITE answer at all, so the
 * record is refused.
 */
static bool accept_ident_set(struct ua_accept *ac, const struct sip_msg *resp)
{
	struct sip_addr from, to;
	struct sip_str call_id;
	struct sip_via via;
	enum sip_method method;
	uint32_t cseq;

	if (!hdr_call_id_get(resp, &call_id))
		return false;
	if (!hdr_cseq_get(resp, &cseq, &method) || method != SIP_METHOD_INVITE)
		return false;
	if (!hdr_addr_get(resp, SIP_HDR_FROM, &from) || !hdr_addr_get(resp, SIP_HDR_TO, &to))
		return false;
	if (!hdr_via_top(resp, &via))
		return false;
	if (!(via.flags & SIP_VIA_F_BRANCH) || !(via.flags & SIP_VIA_F_MAGIC))
		return false;

	ac->cseq = cseq;
	ac->port = (via.flags & SIP_VIA_F_PORT) ? (uint16_t)via.port : SIP_PORT_DEFAULT;
	ac->call_id = str_dup(call_id);
	ac->from_tag = str_dup(from.tag);
	ac->to_tag = str_dup(to.tag);
	ac->branch = str_dup(via.branch);
	ac->host = str_dup(via.host);

	return ac->call_id && ac->from_tag && ac->to_tag && ac->branch && ac->host;
}

static bool accept_dialog_match(const struct ua_accept *ac, const struct sip_msg *msg)
{
	struct sip_addr from, to;
	struct sip_str call_id;
	uint32_t cseq;

	if (!hdr_call_id_get(msg, &call_id) || !hdr_cseq_get(msg, &cseq, NULL))
		return false;
	if (cseq != ac->cseq)
		return false;
	if (!hdr_addr_get(msg, SIP_HDR_FROM, &from) || !hdr_addr_get(msg, SIP_HDR_TO, &to))
		return false;

	return span_eq_cstr(call_id, ac->call_id, false) &&
	       span_eq_cstr(from.tag, ac->from_tag, false) &&
	       span_eq_cstr(to.tag, ac->to_tag, false);
}

static bool accept_via_match(const struct ua_accept *ac, const struct sip_msg *msg)
{
	struct sip_via via;
	uint16_t port;

	if (!hdr_via_top(msg, &via))
		return false;
	if (!(via.flags & SIP_VIA_F_BRANCH) || !(via.flags & SIP_VIA_F_MAGIC))
		return false;

	port = (via.flags & SIP_VIA_F_PORT) ? (uint16_t)via.port : SIP_PORT_DEFAULT;

	return port == ac->port && span_eq_cstr(via.branch, ac->branch, false) &&
	       span_eq_cstr(via.host, ac->host, true);
}

static struct ua_accept *accept_find(struct ua *ua, const struct sip_msg *msg, bool via)
{
	unsigned int i;

	for (i = 0; i < UA_ACCEPT_MAX; i++) {
		struct ua_accept *ac = &ua->accept[i];

		if (!ac->in_use)
			continue;
		if (via ? accept_via_match(ac, msg) : accept_dialog_match(ac, msg))
			return ac;
	}

	return NULL;
}

static bool accept_wire_send(struct ua_accept *ac)
{
	struct sip_txn_env *env = ac->ua->env;

	ac->tx_count++;
	ac->ua->stat.rtx_2xx++;

	return env->send(env, ac->wire, ac->len, &ac->dst) >= 0;
}

static void accept_retx(struct sip_timer *t)
{
	struct ua_accept *ac = t->owner;
	struct ua *ua = ac->ua;
	uint32_t t2 = ua->mgr->tc.t2;

	/*
	 * A refused send is transient on UDP (a full queue, a route in flux),
	 * so the schedule continues. Giving up here would abandon the call on
	 * one lost datagram; the 64*T1 timer is the only thing that ends it.
	 */
	if (!accept_wire_send(ac))
		ua_trace(ua, "2xx retransmission refused, owner %08x", ac->owner);

	ac->retx_ms = ac->retx_ms * 2 > t2 ? t2 : ac->retx_ms * 2;
	sip_timer_arm(ua->env, &ac->retx, ac->retx_ms);
}

static void accept_expire(struct sip_timer *t)
{
	struct ua_accept *ac = t->owner;
	struct ua *ua = ac->ua;
	uint32_t owner = ac->owner;
	bool up = !ac->acked && !ac->detached && ua->cb.ack_missing;

	if (!ac->acked) {
		ua->stat.ack_missing++;
		ua_trace(ua, "no ACK after %u sends of the 2xx, owner %08x", ac->tx_count,
			 owner);
	}

	accept_free(ac);
	if (up)
		ua->cb.ack_missing(ua, owner, ua->priv);
}

/*
 * The record a full pool gives up for a new 2xx: the one whose ACK came
 * first. An acknowledged record repeats nothing any more; for the rest of its
 * 64*T1 it only absorbs late copies of its ACK and INVITE (RFC 6026). A record
 * still waiting for its ACK is never given up, because the 2xx it repeats is
 * what keeps the call. Reason: a record per accepted INVITE that stands 32 s
 * fills the pool of eight from a peer that re-INVITEs about once a second, or
 * that bridges every answered call with a re-INVITE, and the refused 2xx then
 * ends a call that is up.
 */
static struct ua_accept *accept_oldest_acked(struct ua *ua)
{
	struct ua_accept *best = NULL;
	unsigned int i;

	for (i = 0; i < UA_ACCEPT_MAX; i++) {
		struct ua_accept *ac = &ua->accept[i];

		if (!ac->in_use || !ac->acked)
			continue;
		if (!best || ac->acked_seq < best->acked_seq)
			best = ac;
	}

	return best;
}

bool ua_accept_add(struct ua *ua, const struct sip_msg *resp, const void *wire, size_t len,
		   const struct sip_txn_dst *dst, uint32_t owner)
{
	struct ua_accept *ac = NULL;
	unsigned int i;

	if (!ua || !resp || resp->is_request || !wire || !len || !dst)
		return false;
	if (resp->status < 200 || resp->status > 299)
		return false;
	if (accept_find(ua, resp, false))
		return false;

	for (i = 0; i < UA_ACCEPT_MAX && !ac; i++) {
		if (!ua->accept[i].in_use)
			ac = &ua->accept[i];
	}
	if (!ac) {
		ac = accept_oldest_acked(ua);
		if (!ac) {
			ua->stat.denied++;
			return false;
		}
		ua_trace(ua, "accepted record of owner %08x gives way to a new 2xx",
			 ac->owner);
		ua->stat.accept_reused++;
		accept_free(ac);
	}

	memset(ac, 0, sizeof(*ac));
	ac->ua = ua;
	if (!accept_ident_set(ac, resp)) {
		accept_ident_free(ac);
		return false;
	}

	ac->wire = malloc(len);
	if (!ac->wire) {
		accept_ident_free(ac);
		return false;
	}

	memcpy(ac->wire, wire, len);
	ac->len = len;
	ac->dst = *dst;
	ac->owner = owner;
	ac->in_use = true;

	sip_timer_init(&ac->retx, ac, SIP_TIMER_2XX, accept_retx);
	sip_timer_init(&ac->life, ac, SIP_TIMER_ACCEPT, accept_expire);

	ac->retx_ms = ua->mgr->tc.t1;
	sip_timer_arm(ua->env, &ac->retx, ac->retx_ms);
	sip_timer_arm(ua->env, &ac->life, 64 * ua->mgr->tc.t1);

	return true;
}

bool ua_accept_active(struct ua *ua, uint32_t owner)
{
	unsigned int i;

	for (i = 0; i < UA_ACCEPT_MAX; i++) {
		if (ua->accept[i].in_use && ua->accept[i].owner == owner)
			return true;
	}

	return false;
}

unsigned int ua_accept_count(const struct ua *ua)
{
	unsigned int i, n = 0;

	for (i = 0; i < UA_ACCEPT_MAX; i++) {
		if (ua->accept[i].in_use)
			n++;
	}

	return n;
}

static enum ua_rx accept_ack(struct ua_accept *ac)
{
	struct ua *ua = ac->ua;

	ua->stat.ack_absorbed++;
	if (ac->acked)
		return UA_RX_ABSORBED;

	/*
	 * RFC 6026: the record outlives the ACK on purpose. Retransmissions of
	 * the ACK keep arriving after the first one and must not reach the
	 * call layer as a second confirmation.
	 */
	ac->acked = true;
	ac->acked_seq = ++ua->accept_acks;
	sip_timer_cancel(ua->env, &ac->retx);

	return UA_RX_ACK;
}

enum ua_rx ua_msg_recv(struct ua *ua, const struct sip_msg *msg)
{
	struct ua_accept *ac;

	if (!ua || !msg || !msg->is_request)
		return UA_RX_PASS;

	/*
	 * The CSeq method has to agree with the request line. Without that
	 * test an ACK that says "CSeq: n BYE" would match on the dialog fields
	 * alone and stop a retransmission it has nothing to do with.
	 */
	if (msg->method == SIP_METHOD_ACK) {
		if (!cseq_method_is(msg, SIP_METHOD_ACK))
			return UA_RX_PASS;

		ac = accept_find(ua, msg, false);
		if (!ac)
			return UA_RX_PASS;

		return accept_ack(ac);
	}

	if (msg->method != SIP_METHOD_INVITE)
		return UA_RX_PASS;
	if (!cseq_method_is(msg, SIP_METHOD_INVITE))
		return UA_RX_PASS;

	ac = accept_find(ua, msg, true);
	if (!ac)
		return UA_RX_PASS;

	/* RFC 6026: a retransmitted INVITE in the accepted state is answered
	 * with the 2xx again, not passed up as a new call. */
	ua->stat.invite_absorbed++;
	accept_wire_send(ac);

	return UA_RX_ABSORBED;
}

/* ---------------------------------------------------------- 491 glare, 503 */

uint32_t ua_glare_delay_ms(struct ua *ua, bool call_id_owner)
{
	uint32_t r = sip_txn_rand(ua->mgr);

	if (call_id_owner)
		return GLARE_OWNER_BASE_MS + GLARE_STEP_MS * (r % GLARE_OWNER_STEPS);

	return GLARE_STEP_MS * (r % GLARE_PEER_STEPS);
}

static void glare_expire(struct sip_timer *t)
{
	struct ua_glare *g = t->owner;
	struct ua *ua = g->ua;
	uint32_t owner = g->owner;
	bool up = !g->detached && ua->cb.glare_retry;

	g->in_use = false;
	if (up)
		ua->cb.glare_retry(ua, owner, ua->priv);
}

bool ua_glare_arm(struct ua *ua, uint32_t owner, bool call_id_owner, uint32_t *delay_ms)
{
	struct ua_glare *g = NULL;
	unsigned int i;
	uint32_t ms;

	if (!ua)
		return false;

	ua_glare_cancel(ua, owner);

	for (i = 0; i < UA_GLARE_MAX && !g; i++) {
		if (!ua->glare[i].in_use)
			g = &ua->glare[i];
	}
	if (!g) {
		ua->stat.denied++;
		return false;
	}

	memset(g, 0, sizeof(*g));
	g->ua = ua;
	g->owner = owner;
	g->in_use = true;
	sip_timer_init(&g->timer, g, SIP_TIMER_GLARE, glare_expire);

	ms = ua_glare_delay_ms(ua, call_id_owner);
	sip_timer_arm(ua->env, &g->timer, ms);
	if (delay_ms)
		*delay_ms = ms;

	return true;
}

void ua_glare_cancel(struct ua *ua, uint32_t owner)
{
	unsigned int i;

	if (owner == SIP_HANDLE_NONE)
		return;

	for (i = 0; i < UA_GLARE_MAX; i++) {
		struct ua_glare *g = &ua->glare[i];

		if (!g->in_use || g->owner != owner)
			continue;

		sip_timer_cancel(ua->env, &g->timer);
		g->in_use = false;
	}
}

bool ua_retry_after_get(const struct sip_msg *resp, uint32_t *secs)
{
	return hdr_retry_after_get(resp, secs, NULL);
}

bool ua_503_handle(struct ua *ua, uint32_t owner, const struct sip_msg *resp)
{
	uint32_t secs = 0;
	bool present;

	if (!ua || !resp || resp->is_request || resp->status != 503)
		return false;

	present = ua_retry_after_get(resp, &secs);
	if (!present)
		secs = 0;

	if (ua->cb.retry_after)
		ua->cb.retry_after(ua, owner, secs, present, ua->priv);

	return present;
}

unsigned int ua_owner_release(struct ua *ua, uint32_t owner)
{
	unsigned int i, n = 0;

	if (!ua || owner == SIP_HANDLE_NONE)
		return 0;

	for (i = 0; i < UA_ACCEPT_MAX; i++) {
		struct ua_accept *ac = &ua->accept[i];

		if (!ac->in_use || ac->owner != owner)
			continue;

		ac->detached = true;
		ac->owner = SIP_HANDLE_NONE;
		n++;
	}

	for (i = 0; i < UA_GLARE_MAX; i++) {
		struct ua_glare *g = &ua->glare[i];

		if (!g->in_use || g->owner != owner)
			continue;

		g->detached = true;
		g->owner = SIP_HANDLE_NONE;
		n++;
	}

	/*
	 * An owner that vanished has no call to answer, so its pending 100 is
	 * dropped rather than detached: unlike the 2xx retransmission it owes
	 * the peer nothing that the transaction cannot do by itself.
	 */
	for (i = 0; i < UA_TRYING_MAX; i++) {
		struct ua_trying *tr = &ua->trying[i];

		if (!tr->in_use || tr->owner != owner)
			continue;

		sip_timer_cancel(ua->env, &tr->timer);
		tr->in_use = false;
		n++;
	}

	return n;
}
