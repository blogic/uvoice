// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Message waiting indication: the NOTIFY intake and the SUBSCRIBE client. See
 * sip/sub.h for the contracts.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "proto/sip/reg.h"
#include "proto/sip/sub.h"

static void sub_trace(struct sip_sub *sub, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void sub_trace(struct sip_sub *sub, const char *fmt, ...)
{
	va_list ap;

	if (!sub || !sub->env || !sub->env->base.trace)
		return;

	va_start(ap, fmt);
	sip_txn_env_trace(sub->env, fmt, ap, "mwi %s: ",
			  sub->cfg.aor ? sub->cfg.aor : "?");
	va_end(ap);
}

const char *sip_sub_status_name(enum sip_sub_status status)
{
	switch (status) {
	case SIP_SUB_OFF:
		return "off";
	case SIP_SUB_TRYING:
		return "trying";
	case SIP_SUB_ACTIVE:
		return "active";
	case SIP_SUB_RETRY:
		return "retry";
	case SIP_SUB_REFUSED:
		return "refused";
	}

	return "?";
}

const char *sip_sub_rx_name(enum sip_sub_rx rx)
{
	return rx == SIP_SUB_RX_STORE ? "store" : "bad-body";
}

/* ------------------------------------------------------------ body grammar */

/*
 * One line of the body, CRLF or bare LF terminated (shared/msg.h "Tolerance
 * policy" takes the same view of the header block). The last line needs no
 * terminator at all, because a body is a span and not a C string.
 */
static bool body_line_next(struct sip_str *rest, struct sip_str *line)
{
	const char *nl;
	size_t len;

	if (!rest->len)
		return false;

	nl = memchr(rest->p, '\n', rest->len);
	len = nl ? (size_t)(nl - rest->p) : rest->len;
	*line = sip_str_init(rest->p, len);
	if (line->len && line->p[line->len - 1] == '\r')
		line->len--;

	if (!nl) {
		*rest = sip_str_init(rest->p + len, 0);
		return true;
	}

	*rest = sip_str_init(rest->p + len + 1, rest->len - len - 1);

	return true;
}

static bool body_hdr_split(struct sip_str line, struct sip_str *name, struct sip_str *val)
{
	const char *colon;
	size_t at;

	if (!line.len)
		return false;

	colon = memchr(line.p, ':', line.len);
	if (!colon)
		return false;

	at = (size_t)(colon - line.p);
	*name = sip_str_trim(sip_str_init(line.p, at));
	*val = sip_str_trim(sip_str_init(line.p + at + 1, line.len - at - 1));

	return name->len > 0;
}

/*
 * RFC 3842 3.1: the value is the token "yes" or the token "no" and nothing
 * else. A value that merely contains one of them ("yes, 2 waiting") is not
 * the grammar and must not turn an indication on.
 */
static bool waiting_take(struct sip_str val, bool *out)
{
	if (sip_str_eq_ci(val, "yes")) {
		*out = true;
		return true;
	}

	if (sip_str_eq_ci(val, "no")) {
		*out = false;
		return true;
	}

	return false;
}

/*
 * "<new>/<old>", with the RFC 3842 urgent pair in parentheses behind it when
 * the switch sends one. The urgent counts have no consumer on this product,
 * so everything from the first space or '(' is dropped.
 */
static bool counts_take(struct sip_str val, uint32_t *new_msgs, uint32_t *old_msgs)
{
	struct sip_str left, right;
	const char *slash;
	size_t at, i;

	if (!val.len)
		return false;

	slash = memchr(val.p, '/', val.len);
	if (!slash)
		return false;

	at = (size_t)(slash - val.p);
	left = sip_str_trim(sip_str_init(val.p, at));
	right = sip_str_trim(sip_str_init(val.p + at + 1, val.len - at - 1));

	for (i = 0; i < right.len; i++) {
		if (right.p[i] == ' ' || right.p[i] == '\t' || right.p[i] == '(')
			break;
	}
	right.len = i;

	return sip_str_uint(left, new_msgs) && sip_str_uint(right, old_msgs);
}

bool sip_mwi_body_parse(struct sip_str body, struct sip_mwi *out)
{
	struct sip_str rest = body, line, name, val;
	bool seen = false;

	memset(out, 0, sizeof(*out));
	if (!body.p)
		return false;

	while (body_line_next(&rest, &line)) {
		if (!body_hdr_split(line, &name, &val))
			continue;

		/*
		 * The first of each line decides. Taking the last one would let
		 * a trailer a switch appended overwrite the summary it just
		 * sent, and a body that carries two of either is malformed
		 * whichever one is believed.
		 */
		if (!seen && sip_str_eq_ci(name, "Messages-Waiting")) {
			if (!waiting_take(val, &out->waiting))
				break;

			seen = true;
			continue;
		}

		if (!out->counts_valid && sip_str_eq_ci(name, "Voice-Message"))
			out->counts_valid = counts_take(val, &out->new_msgs,
							&out->old_msgs);
	}

	if (!seen)
		memset(out, 0, sizeof(*out));

	return seen;
}

/* --------------------------------------------------------------- the intake */

enum sip_sub_event sip_sub_event_class(const struct sip_msg *msg)
{
	struct sip_event ev;

	if (!msg || !msg->is_request || msg->method != SIP_METHOD_NOTIFY)
		return SIP_SUB_EV_NONE;

	/* RFC 6665 8.2.1: an event type is a token compared case insensitively.
	 * A NOTIFY with no Event header names no package uvoice serves either,
	 * so it takes the same answer as one naming somebody else's. */
	if (!hdr_event_get(msg, &ev))
		return SIP_SUB_EV_OTHER;

	if (sip_str_eq_ci(ev.package, SIP_SUB_EVENT_MWI))
		return SIP_SUB_EV_MWI;

	/* hdr_event_get() splits the id parameter off the package, so a refer
	 * NOTIFY is named here with or without one (RFC 3515 2.4.4). */
	if (sip_str_eq_ci(ev.package, SIP_SUB_EVENT_REFER))
		return SIP_SUB_EV_REFER;

	return SIP_SUB_EV_OTHER;
}

static bool ctype_ok(const struct sip_msg *msg)
{
	struct sip_str type;

	/* No Content-Type at all: the grammar decides, because a body that
	 * parses as a message summary is one whatever a switch labelled it. */
	if (!hdr_content_type_get(msg, &type, NULL))
		return true;

	type = sip_str_trim(type);

	return sip_str_eq_ci(type, SIP_SUB_CTYPE_MWI) ||
	       sip_str_eq_ci(type, SIP_SUB_CTYPE_MWI_ALT);
}

/* ---------------------------------------------------------- the client core */

static void attempt_start(struct sip_sub *sub);

static void status_set(struct sip_sub *sub, enum sip_sub_status status)
{
	if (sub->status == (uint8_t)status)
		return;

	sub_trace(sub, "%s -> %s", sip_sub_status_name((enum sip_sub_status)sub->status),
		  sip_sub_status_name(status));
	sub->status = (uint8_t)status;
}

static bool cfg_ok(const struct sip_sub *sub)
{
	return sub->cfg.aor && sub->cfg.registrar && sub->cfg.local_host;
}

static uint32_t expires_want(const struct sip_sub *sub)
{
	uint32_t want = sub->cfg.expires ? sub->cfg.expires : SIP_REG_EXPIRES_DEFAULT;

	return sub->min_expires > want ? sub->min_expires : want;
}

static void ident_make(struct sip_sub *sub)
{
	const char *host = sub->cfg.local_host ? sub->cfg.local_host : "uvoice";

	snprintf(sub->call_id, sizeof(sub->call_id), "%08x%08x@%s",
		 sip_txn_rand(sub->mgr), sip_txn_rand(sub->mgr), host);
	snprintf(sub->from_tag, sizeof(sub->from_tag), "%08x%04x", sip_txn_rand(sub->mgr),
		 sip_txn_rand(sub->mgr) & 0xffffu);
	sub->to_tag[0] = 0;
}

static void cred_write(struct sip_sub *sub, struct sip_buf *b, enum sip_auth_kind kind)
{
	enum sip_auth_status status;

	status = sip_auth_cred_write(b, sub->auth, kind, sub->cfg.auth_user,
				     sub->cfg.auth_pass, "SUBSCRIBE",
				     sub->cfg.registrar);
	if (status != SIP_AUTH_OK)
		sub_trace(sub, "%s credentials refused (%s)", sip_auth_kind_name(kind),
			  sip_auth_status_name(status));
}

static bool wire_build(struct sip_sub *sub)
{
	struct sip_buf b;
	unsigned int k;

	if (!sip_branch_new(sub->mgr, sub->branch, sizeof(sub->branch)))
		return false;

	sip_buf_init(&b, sub->wire, sizeof(sub->wire));

	msg_req_line_write(&b, "SUBSCRIBE", sub->cfg.registrar);
	hdr_via_write(&b, sub->cfg.transport, sub->cfg.local_host,
		      sub->cfg.local_port, sub->branch, true);
	hdr_write(&b, SIP_HDR_MAX_FORWARDS, "70");
	hdr_addr_tag_write(&b, SIP_HDR_FROM, sub->cfg.display, sub->cfg.aor,
			   sub->from_tag);
	hdr_addr_tag_write(&b, SIP_HDR_TO, NULL, sub->cfg.aor,
			   sub->to_tag[0] ? sub->to_tag : NULL);
	hdr_write(&b, SIP_HDR_CALL_ID, "%s", sub->call_id);
	hdr_cseq_write(&b, sub->cseq, "SUBSCRIBE");

	/* RFC 3608 section 5: an out of dialog request takes the route the
	 * registrar granted. A To tag says the notifier put this subscription
	 * in a dialog, and this file keeps no dialog route set (sip/sub.h). */
	if (!sub->to_tag[0])
		route_hdrs_write(&b, sub->cfg.service_route);

	/* The NOTIFY has to reach the address the registrar already holds, so
	 * the Contact is the registered one and never a fresh build. */
	if (sub->cfg.contact && sub->cfg.contact[0])
		hdr_addr_write(&b, SIP_HDR_CONTACT, NULL, sub->cfg.contact, NULL);

	hdr_write(&b, SIP_HDR_EVENT, "%s", SIP_SUB_EVENT_MWI);
	hdr_write(&b, SIP_HDR_ACCEPT, "%s", SIP_SUB_CTYPE_MWI);
	hdr_write(&b, SIP_HDR_EXPIRES, "%u", sub->req_expires);
	if (sub->cfg.user_agent)
		hdr_write(&b, SIP_HDR_USER_AGENT, "%s", sub->cfg.user_agent);

	for (k = 0; k < SIP_AUTH_KIND_MAX; k++)
		cred_write(sub, &b, (enum sip_auth_kind)k);

	msg_body_write(&b, NULL, NULL, 0);
	if (!sip_buf_ok(&b))
		return false;

	sub->wire_len = sip_buf_len(&b);

	return true;
}

/* ----------------------------------------------------------------- timers */

static void timer_stop(struct sip_sub *sub)
{
	sip_timer_cancel(sub->env, &sub->timer);
}

static void timer_arm(struct sip_sub *sub, enum sip_timer_id id, uint32_t secs)
{
	/* A delay a notifier chose reaches a millisecond timer, so it takes the
	 * sip_reg ceiling: seconds times 1000 has to stay inside it. */
	if (secs > SIP_REG_DELAY_MAX)
		secs = SIP_REG_DELAY_MAX;

	sub->timer.id = (uint8_t)id;
	sip_timer_arm(sub->env, &sub->timer, secs * 1000);
	sub_trace(sub, "timer %s armed %u s", sip_timer_name(id), secs);
}

static void timer_fire(struct sip_timer *t)
{
	struct sip_sub *sub = t->owner;

	if (!sub->running)
		return;

	attempt_start(sub);
}

/* --------------------------------------------------------------- attempts */

static void txn_drop(struct sip_sub *sub)
{
	struct sip_txn *txn;

	if (sub->txn == SIP_HANDLE_NONE)
		return;

	txn = sip_txn_get(sub->mgr, sub->txn);
	sub->txn = SIP_HANDLE_NONE;
	if (txn)
		sip_txn_abort(txn);
}

/*
 * A refusal of the package itself. Each of these means the notifier answers
 * the next SUBSCRIBE exactly the same way, so the client stops instead of
 * asking again every minute for the life of the daemon.
 */
static bool code_is_refusal(uint32_t code)
{
	return code == 403 || code == 404 || code == 405 || code == 489 || code == 501;
}

static void sub_fail(struct sip_sub *sub, uint32_t code)
{
	sub->stat.failed++;

	if (sub->unsubscribing || !sub->running) {
		sub->running = false;
		sub->unsubscribing = false;
		status_set(sub, SIP_SUB_OFF);
		return;
	}

	if (code_is_refusal(code)) {
		sub->stat.refused++;
		sub_trace(sub, "the notifier refused message-summary with %u", code);
		status_set(sub, SIP_SUB_REFUSED);
		return;
	}

	sub_trace(sub, "attempt failed (%u)", code);
	status_set(sub, SIP_SUB_RETRY);
	timer_arm(sub, SIP_TIMER_SUB_RETRY, SIP_SUB_RETRY_S);
}

static bool attempt_send(struct sip_sub *sub)
{
	const struct sip_txn_dst *dst;
	struct sip_txn *txn;

	dst = sub->dst_get ? sub->dst_get(sub, sub->priv) : NULL;
	if (!dst)
		return false;

	/* RFC 6665 4.1.2.2: one Call-ID, one From tag, a CSeq that only grows,
	 * so a notifier sees one subscription and not a new one per refresh. */
	sub->cseq++;
	if (!wire_build(sub))
		return false;

	txn = sip_txn_client_start(sub->mgr, SIP_METHOD_SUBSCRIBE,
				   sip_str_cstr(sub->branch), sub->wire, sub->wire_len,
				   dst, sub->owner);
	if (!txn)
		return false;

	sub->txn = sip_txn_handle(txn);
	sub->stat.sent++;
	sub_trace(sub, "SUBSCRIBE cseq %u expires %u", sub->cseq, sub->req_expires);

	return true;
}

/* a retry inside one exchange: the auth and 423 counters keep their values */
static void attempt_resend(struct sip_sub *sub)
{
	if (attempt_send(sub))
		return;

	sub_fail(sub, 0);
}

static void attempt_start(struct sip_sub *sub)
{
	timer_stop(sub);

	/* One SUBSCRIBE per account at a time, the sip_reg rule: a second
	 * transaction would run with nobody watching its outcome. */
	txn_drop(sub);
	memset(sub->auth_retry, 0, sizeof(sub->auth_retry));
	sub->min_expires_retry = 0;
	sub->req_expires = sub->unsubscribing ? 0 : expires_want(sub);
	status_set(sub, SIP_SUB_TRYING);

	if (!attempt_send(sub))
		sub_fail(sub, 0);
}

/*
 * A subscription the notifier no longer knows. RFC 6665 makes a 481 and a
 * terminated state final for that subscription, and a client that stops
 * there never receives message-summary again. A fresh identity is minted
 * instead, bounded so a notifier that always answers 481 cannot spin.
 */
static void sub_restart(struct sip_sub *sub)
{
	sub->stat.restarted++;
	sub->granted = 0;
	txn_drop(sub);

	/*
	 * The identity is minted before the delay, not instead of it. The
	 * Call-ID and the To tag this subscription carried name a subscription
	 * the notifier has ended, so any later request carrying them earns the
	 * same 481, delayed or not: the retry timer would refresh a dead
	 * subscription and the pair would loop through the delay forever.
	 * ident_make() also clears the To tag, which is half of that identity.
	 */
	ident_make(sub);

	if (sub->restart_run >= SIP_SUB_RESTART_MAX) {
		sub_trace(sub, "the subscription keeps ending, waiting before the next");
		status_set(sub, SIP_SUB_RETRY);
		timer_arm(sub, SIP_TIMER_SUB_RETRY, SIP_SUB_RETRY_S);
		return;
	}

	sub->restart_run++;
	sub_trace(sub, "subscribing again with a fresh call-id");
	attempt_start(sub);
}

/* --------------------------------------------------------------- responses */

static void to_tag_take(struct sip_sub *sub, const struct sip_msg *msg,
			enum sip_hdr_id id)
{
	struct sip_addr a;

	if (sub->to_tag[0])
		return;
	if (!hdr_addr_get(msg, id, &a) || !a.tag.len)
		return;

	sip_str_copy(sub->to_tag, sizeof(sub->to_tag), a.tag);
}

static void resp_ok(struct sip_sub *sub, const struct sip_msg *msg)
{
	uint32_t expires;

	sub->stat.ok++;
	sub->restart_run = 0;
	sip_auth_state_accept(&sub->auth[SIP_AUTH_WWW]);
	sip_auth_state_accept(&sub->auth[SIP_AUTH_PROXY]);
	to_tag_take(sub, msg, SIP_HDR_TO);

	if (sub->unsubscribing) {
		sub->running = false;
		sub->unsubscribing = false;
		sub->granted = 0;
		status_set(sub, SIP_SUB_OFF);
		sub_trace(sub, "unsubscribed");
		return;
	}

	if (!hdr_uint_get(msg, SIP_HDR_EXPIRES, &expires))
		expires = sub->req_expires;

	sub->granted = expires;

	/* The notifier took the request and granted nothing, so there is no
	 * subscription to refresh and asking again is the only move left. */
	if (!expires) {
		status_set(sub, SIP_SUB_RETRY);
		timer_arm(sub, SIP_TIMER_SUB_RETRY, SIP_SUB_RETRY_S);
		return;
	}

	status_set(sub, SIP_SUB_ACTIVE);
	timer_arm(sub, SIP_TIMER_SUB_REFRESH, sip_reg_refresh_secs(expires));
	sub_trace(sub, "subscribed for %u s", expires);
}

/*
 * The retry allowance is per kind, for the reason reg.c states: a registrar
 * that challenges first and a proxy in front of it that challenges next are
 * two exchanges, not two attempts at one password.
 */
static void resp_auth(struct sip_sub *sub, const struct sip_msg *msg, uint32_t code)
{
	enum sip_auth_status status[SIP_AUTH_KIND_MAX];
	unsigned int k;
	bool resend;

	sub->stat.challenged++;
	resend = sip_auth_challenges_take(sub->auth, msg, sub->auth_retry,
					  sip_txn_rand, sub->mgr, status);

	for (k = 0; k < SIP_AUTH_KIND_MAX; k++) {
		if (status[k] != SIP_AUTH_OK && status[k] != SIP_AUTH_ERR_NONE)
			sub_trace(sub, "%s challenge refused (%s)",
				  sip_auth_kind_name((enum sip_auth_kind)k),
				  sip_auth_status_name(status[k]));
	}

	if (!resend) {
		sub_fail(sub, code);
		return;
	}

	attempt_resend(sub);
}

static void resp_min_expires(struct sip_sub *sub, const struct sip_msg *msg)
{
	uint32_t min = 0;

	if (!hdr_uint_get(msg, SIP_HDR_MIN_EXPIRES, &min) || !min ||
	    min <= sub->req_expires || sub->min_expires_retry >= SIP_SUB_MIN_EXPIRES_MAX) {
		sub_trace(sub, "423 makes no progress at %u s", min);
		sub_fail(sub, 423);
		return;
	}

	sub->min_expires_retry++;
	sub->min_expires = min;
	sub->req_expires = min;
	sub_trace(sub, "423, asking for %u s", min);
	attempt_resend(sub);
}

void sip_sub_txn_resp(struct sip_sub *sub, uint32_t txn, const struct sip_msg *msg)
{
	uint32_t code;

	if (!sub || !msg || msg->is_request)
		return;
	if (sub->txn == SIP_HANDLE_NONE || txn != sub->txn)
		return;

	code = msg->status;
	if (code < 200)
		return;

	sub->txn = SIP_HANDLE_NONE;

	if (code < 300) {
		resp_ok(sub, msg);
		return;
	}

	if (code == 401 || code == 407) {
		resp_auth(sub, msg, code);
		return;
	}

	if (code == 423) {
		resp_min_expires(sub, msg);
		return;
	}

	/* An unsubscribe of a subscription the notifier already dropped has
	 * reached the state it asked for, so it needs no fresh identity. */
	if (code == 481 && !sub->unsubscribing) {
		sub_restart(sub);
		return;
	}

	sub_fail(sub, code);
}

void sip_sub_txn_done(struct sip_sub *sub, uint32_t txn, enum sip_txn_reason reason)
{
	if (!sub || sub->txn == SIP_HANDLE_NONE || txn != sub->txn)
		return;

	sub->txn = SIP_HANDLE_NONE;
	if (reason != SIP_TXN_DONE_TIMEOUT && reason != SIP_TXN_DONE_TRANSPORT)
		return;

	sub_fail(sub, reason == SIP_TXN_DONE_TRANSPORT ? 503 : 408);
}

/* ------------------------------------------------------------ NOTIFY intake */

/*
 * Whether this NOTIFY belongs to the subscription this instance holds. No
 * sip_dialog stands behind it, so the identity is the one the SUBSCRIBE
 * carried with the roles swapped: the notifier's To tag is our From tag, and
 * its From tag is the To tag the subscription learned.
 */
static bool notify_is_ours(const struct sip_sub *sub, const struct sip_msg *msg)
{
	struct sip_str call_id;
	struct sip_addr a;

	if (!sub->call_id[0])
		return false;
	if (!hdr_call_id_get(msg, &call_id) || !sip_str_eq(call_id, sub->call_id))
		return false;
	if (!hdr_addr_get(msg, SIP_HDR_TO, &a) || !sip_str_eq(a.tag, sub->from_tag))
		return false;

	if (!sub->to_tag[0])
		return true;

	return hdr_addr_get(msg, SIP_HDR_FROM, &a) && sip_str_eq(a.tag, sub->to_tag);
}

/*
 * RFC 6665 4.1.3: the notifier's subscription state is authoritative and its
 * expires parameter is what the next refresh has to beat. A NOTIFY arrives
 * inside the exchange that establishes a subscription as often as after it,
 * so this is also where the To tag is learned.
 */
static void notify_state_take(struct sip_sub *sub, const struct sip_msg *msg)
{
	struct sip_sub_state st;

	to_tag_take(sub, msg, SIP_HDR_FROM);
	if (!hdr_sub_state_get(msg, &st))
		return;

	if (sip_str_eq_ci(st.state, "terminated")) {
		if (!sub->running || sub->unsubscribing)
			return;

		sub_trace(sub, "the notifier ended the subscription");
		sub_restart(sub);
		return;
	}

	if (!(st.flags & SIP_SUB_F_EXPIRES) || !st.expires)
		return;

	sub->granted = st.expires;

	/* A request in flight owns the timer: its own answer arms the refresh,
	 * and two sources arming it would race. */
	if (sub->txn != SIP_HANDLE_NONE || !sub->running)
		return;

	status_set(sub, SIP_SUB_ACTIVE);
	timer_arm(sub, SIP_TIMER_SUB_REFRESH, sip_reg_refresh_secs(st.expires));
}

enum sip_sub_rx sip_sub_notify_recv(struct sip_sub *sub, const struct sip_msg *msg,
				    struct sip_mwi *out)
{
	struct sip_str body;
	bool ours;

	memset(out, 0, sizeof(*out));
	if (!msg)
		return SIP_SUB_RX_BAD_BODY;

	ours = sub && notify_is_ours(sub, msg);
	if (sub && ours)
		sub->stat.notify_ours++;
	else if (sub)
		sub->stat.notify_unsolicited++;

	if (ours)
		notify_state_take(sub, msg);

	if (!ctype_ok(msg) || !msg_body_get(msg, &body) || !sip_mwi_body_parse(body, out)) {
		if (sub)
			sub->stat.notify_bad_body++;
		sub_trace(sub, "notify carries no message summary, nothing stored");

		return SIP_SUB_RX_BAD_BODY;
	}

	sub_trace(sub, "notify %s waiting %d new %u old %u", ours ? "ours" : "unsolicited",
		  out->waiting, out->new_msgs, out->old_msgs);

	return SIP_SUB_RX_STORE;
}

/* ------------------------------------------------------------------- API */

void sip_sub_init(struct sip_sub *sub, struct sip_txn_mgr *mgr, struct sip_txn_env *env,
		  uint32_t owner, const struct sip_sub_cfg *cfg, sip_sub_dst_get dst_get,
		  void *priv)
{
	memset(sub, 0, sizeof(*sub));
	sub->mgr = mgr;
	sub->env = env;
	sub->owner = owner;
	sub->dst_get = dst_get;
	sub->priv = priv;
	sub->txn = SIP_HANDLE_NONE;

	if (cfg)
		sub->cfg = *cfg;
	if (!sub->cfg.local_port)
		sub->cfg.local_port = SIP_PORT_DEFAULT;

	/* the id moves per arm, exactly as reg.c moves between its two */
	sip_timer_init(&sub->timer, sub, SIP_TIMER_SUB_REFRESH, timer_fire);
}

void sip_sub_start(struct sip_sub *sub)
{
	if (!sub || sub->running)
		return;

	if (!cfg_ok(sub)) {
		sub_trace(sub, "no address of record or registrar, staying off");
		return;
	}

	sub->running = true;
	sub->unsubscribing = false;
	sub->restart_run = 0;
	sub->min_expires = 0;
	ident_make(sub);
	attempt_start(sub);
}

void sip_sub_stop(struct sip_sub *sub, bool unsubscribe)
{
	if (!sub)
		return;

	timer_stop(sub);

	if (!unsubscribe || !sub->running || sub->status == SIP_SUB_REFUSED) {
		txn_drop(sub);
		sub->running = false;
		sub->unsubscribing = false;
		sub->granted = 0;
		status_set(sub, SIP_SUB_OFF);
		return;
	}

	sub->unsubscribing = true;
	attempt_start(sub);
}

void sip_sub_cfg_update(struct sip_sub *sub, const struct sip_sub_cfg *cfg)
{
	if (!sub || !cfg)
		return;

	sub->cfg = *cfg;
	if (!sub->cfg.local_port)
		sub->cfg.local_port = SIP_PORT_DEFAULT;
}

void sip_sub_done(struct sip_sub *sub)
{
	if (!sub)
		return;

	sip_sub_stop(sub, false);
	sip_txn_owner_release(sub->mgr, sub->owner);
	sub->txn = SIP_HANDLE_NONE;
}

enum sip_sub_status sip_sub_status_get(const struct sip_sub *sub)
{
	return sub ? (enum sip_sub_status)sub->status : SIP_SUB_OFF;
}

uint32_t sip_sub_expires(const struct sip_sub *sub)
{
	return sub ? sub->granted : 0;
}

const char *sip_sub_call_id(const struct sip_sub *sub)
{
	return sub ? sub->call_id : "";
}

uint32_t sip_sub_cseq(const struct sip_sub *sub)
{
	return sub ? sub->cseq : 0;
}
