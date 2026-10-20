// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * INVITE dialogs. See sip/dialog.h for the contracts.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/sip/dialog.h"

/*
 * The request URI of an in-dialog request. A remote target longer than this is
 * refused rather than truncated, because a truncated request URI reaches the
 * wrong host.
 */
#define DLG_URI_MAX	512

/* an ACK with an SDP answer is the largest message this layer builds */
#define DLG_BUF_MAX	4096

static char dlg_out[DLG_BUF_MAX];

const char *sip_dialog_state_name(enum sip_dialog_state state)
{
	switch (state) {
	case SIP_DLG_EARLY:
		return "early";
	case SIP_DLG_CONFIRMED:
		return "confirmed";
	case SIP_DLG_TERMINATED:
		return "terminated";
	}

	return "?";
}

static void dlg_trace(const struct sip_dialog *d, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void dlg_trace(const struct sip_dialog *d, const char *fmt, ...)
{
	struct sip_txn_env *env = d->set->env;
	va_list ap;

	if (!env->base.trace)
		return;

	va_start(ap, fmt);
	sip_txn_env_trace(env, fmt, ap, "dialog %u %s %s: ", d->index,
			  d->uac ? "uac" : "uas",
			  sip_dialog_state_name((enum sip_dialog_state)
						d->state));
	va_end(ap);
}

/* ------------------------------------------------------------------ strings */

static char *str_dup_len(const char *p, size_t len)
{
	char *mem;

	if (!p || !len)
		return NULL;

	mem = malloc(len + 1);
	if (!mem)
		return NULL;

	memcpy(mem, p, len);
	mem[len] = 0;

	return mem;
}

static char *str_dup_str(struct sip_str s)
{
	return str_dup_len(s.p, s.len);
}

static char *str_dup_cstr(const char *s)
{
	return s ? str_dup_len(s, strlen(s)) : NULL;
}

static bool str_eq_cstr(const char *stored, struct sip_str s)
{
	if (!stored)
		return false;

	return sip_str_eq(s, stored);
}

/* an absent tag equals an absent tag: RFC 3261 predates the mandatory To tag */
static bool tag_eq(const char *stored, struct sip_str s)
{
	if (!stored)
		return s.len == 0;

	return sip_str_eq(s, stored);
}

/* --------------------------------------------------------------- lifetime */

void sip_dialog_set_init(struct sip_dialog_set *set, struct sip_txn_env *env)
{
	memset(set, 0, sizeof(*set));
	set->env = env;
}

static void dlg_strings_free(struct sip_dialog *d)
{
	unsigned int i;

	free(d->call_id);
	free(d->local_tag);
	free(d->remote_tag);
	free(d->local_uri);
	free(d->remote_uri);
	free(d->remote_target);
	free(d->ack);
	d->call_id = NULL;
	d->local_tag = NULL;
	d->remote_tag = NULL;
	d->local_uri = NULL;
	d->remote_uri = NULL;
	d->remote_target = NULL;
	d->ack = NULL;
	d->ack_len = 0;

	for (i = 0; i < SIP_ROUTE_MAX; i++) {
		free(d->route[i]);
		d->route[i] = NULL;
	}
	d->routes = 0;
}

static void dlg_release(struct sip_dialog *d)
{
	dlg_strings_free(d);
	d->in_use = false;
	d->refs = 0;
	d->state = SIP_DLG_TERMINATED;
	d->gen++;
	if (!d->gen)
		d->gen = 1;
	d->set->active--;
}

void sip_dialog_set_done(struct sip_dialog_set *set)
{
	unsigned int i;

	for (i = 0; i < SIP_DIALOG_MAX; i++) {
		if (set->pool[i].in_use)
			dlg_release(&set->pool[i]);
	}
}

unsigned int sip_dialog_count(const struct sip_dialog_set *set)
{
	return set->active;
}

static struct sip_dialog *dlg_alloc(struct sip_dialog_set *set)
{
	unsigned int i;

	for (i = 0; i < SIP_DIALOG_MAX; i++) {
		struct sip_dialog *d = &set->pool[i];
		uint16_t gen = d->gen;

		if (d->in_use)
			continue;

		memset(d, 0, sizeof(*d));
		d->set = set;
		d->index = i;
		d->gen = gen ? gen : 1;
		d->refs = 1;
		d->in_use = true;
		set->active++;
		set->stat.created++;

		return d;
	}

	set->stat.denied++;

	return NULL;
}

uint32_t sip_dialog_handle(const struct sip_dialog *d)
{
	if (!d || !d->in_use)
		return SIP_HANDLE_NONE;

	return sip_handle_make(d->index, d->gen);
}

struct sip_dialog *sip_dialog_get(struct sip_dialog_set *set, uint32_t handle)
{
	unsigned int index = sip_handle_index(handle);
	struct sip_dialog *d;

	if (handle == SIP_HANDLE_NONE || index >= SIP_DIALOG_MAX)
		return NULL;

	d = &set->pool[index];
	if (!d->in_use || d->gen != sip_handle_gen(handle))
		return NULL;

	return d;
}

void sip_dialog_hold(struct sip_dialog *d)
{
	if (d && d->in_use)
		d->refs++;
}

void sip_dialog_put(struct sip_dialog *d)
{
	if (!d || !d->in_use || !d->refs)
		return;

	if (--d->refs)
		return;

	dlg_release(d);
}

void sip_dialog_terminate(struct sip_dialog *d)
{
	if (!d || !d->in_use || d->state == SIP_DLG_TERMINATED)
		return;

	d->state = SIP_DLG_TERMINATED;
	d->set->stat.terminated++;
	dlg_trace(d, "terminated");
}

enum sip_dialog_state sip_dialog_state(const struct sip_dialog *d)
{
	return (enum sip_dialog_state)d->state;
}

const struct sip_txn_dst *sip_dialog_dst(const struct sip_dialog *d)
{
	return d->dst_set ? &d->dst : NULL;
}

void sip_dialog_dst_set(struct sip_dialog *d, const struct sip_txn_dst *dst)
{
	if (!d || !dst)
		return;

	d->dst = *dst;
	d->dst_set = true;
}

void sip_dialog_confirm(struct sip_dialog *d)
{
	if (!d || d->state != SIP_DLG_EARLY)
		return;

	d->state = SIP_DLG_CONFIRMED;
	d->set->stat.confirmed++;
	dlg_trace(d, "confirmed");
}

/* ------------------------------------------------------- target and route */

static bool route_store(struct sip_dialog *d, const struct sip_route_set *rs)
{
	unsigned int i;

	for (i = 0; i < SIP_ROUTE_MAX; i++) {
		free(d->route[i]);
		d->route[i] = NULL;
	}
	d->routes = 0;
	d->route_overflow = rs->overflow;

	/*
	 * A route set longer than SIP_ROUTE_MAX rejects the dialog. A truncated
	 * one sends every in-dialog request down a path that skips proxies the
	 * peer put in the chain, and a BYE that never arrives leaves a call up
	 * on the other side.
	 */
	if (rs->overflow)
		return false;

	for (i = 0; i < rs->count; i++) {
		char *hop = str_dup_str(rs->hop[i]);

		if (!hop)
			return false;

		d->route[d->routes++] = hop;
	}

	return true;
}

/*
 * RFC 3261 12.2.1.2: the route set is fixed when the dialog is created and
 * never changes; only the remote target is refreshed, by a Contact in a later
 * response or in an in-dialog request (12.2.2).
 */
bool sip_dialog_target_refresh(struct sip_dialog *d, const struct sip_msg *msg)
{
	struct sip_addr c;
	char *target;

	if (!d || !d->in_use || !msg)
		return false;
	if (!hdr_addr_get(msg, SIP_HDR_CONTACT, &c) || !c.uri.raw.len)
		return false;

	target = str_dup_str(c.uri.raw);
	if (!target)
		return false;

	free(d->remote_target);
	d->remote_target = target;

	return true;
}

static void target_update(struct sip_dialog *d, const struct sip_msg *msg)
{
	sip_dialog_target_refresh(d, msg);
}

static bool route_update(struct sip_dialog *d, const struct sip_msg *msg, bool reverse)
{
	struct sip_route_set rs;

	route_set_collect(msg, SIP_HDR_RECORD_ROUTE, &rs);
	if (reverse)
		route_set_reverse(&rs);

	return route_store(d, &rs);
}

/* ------------------------------------------------------------------- UAC */

/*
 * The dialog a response belongs to. A response echoes the request it answers,
 * so its From tag is our tag and its To tag is the peer's in both orientations:
 * the search reads the stored tags and never the uac flag.
 *
 * DLG_F_ANY_REMOTE ignores the peer tag, which is how a 2xx that carries no To
 * tag finds the dialog an earlier 18x established.
 *
 * DLG_F_TERMINATED keeps a dialog the owner has already left matchable. The
 * RFC 6026 duty outlives the call: a peer that missed our ACK retransmits its
 * 2xx for 64*T1, and answering it needs the record that holds the ACK. The
 * record disappears when the last reference is dropped, not when the dialog
 * ends.
 *
 * DLG_F_ANY_ROLE takes a dialog the peer created as well, for the ACK of a 2xx
 * to a re-INVITE this side placed inside it. A 2xx that creates or updates a
 * dialog cannot come from one: it answers the INVITE that opened the dialog,
 * and that INVITE was ours.
 */
#define DLG_F_ANY_REMOTE	(1u << 0)
#define DLG_F_TERMINATED	(1u << 1)
#define DLG_F_ANY_ROLE		(1u << 2)

static struct sip_dialog *dlg_resp_find(struct sip_dialog_set *set, struct sip_str call_id,
					struct sip_str local_tag, struct sip_str remote_tag,
					uint32_t flags)
{
	unsigned int i;

	for (i = 0; i < SIP_DIALOG_MAX; i++) {
		struct sip_dialog *d = &set->pool[i];

		if (!d->in_use)
			continue;
		if (!d->uac && !(flags & DLG_F_ANY_ROLE))
			continue;
		if (d->state == SIP_DLG_TERMINATED && !(flags & DLG_F_TERMINATED))
			continue;
		if (!str_eq_cstr(d->call_id, call_id))
			continue;
		if (!tag_eq(d->local_tag, local_tag))
			continue;
		if (!(flags & DLG_F_ANY_REMOTE) && !tag_eq(d->remote_tag, remote_tag))
			continue;

		return d;
	}

	return NULL;
}

static bool uac_fill(struct sip_dialog *d, const struct sip_msg *resp, struct sip_str call_id,
		     const struct sip_addr *from, const struct sip_addr *to, uint32_t cseq)
{
	d->uac = true;
	d->call_id = str_dup_str(call_id);
	d->local_tag = str_dup_str(from->tag);
	d->remote_tag = str_dup_str(to->tag);
	d->local_uri = str_dup_str(from->uri.raw);
	d->remote_uri = str_dup_str(to->uri.raw);
	d->local_cseq = cseq;

	target_update(d, resp);
	if (!route_update(d, resp, true))
		return false;

	return d->call_id && d->local_tag && d->local_uri && d->remote_uri;
}

struct sip_dialog *sip_dialog_uac_update(struct sip_dialog_set *set, const struct sip_msg *resp,
					 const struct sip_txn_dst *dst,
					 const struct sip_dialog_local *local)
{
	struct sip_dialog *d, *early;
	struct sip_addr from, to;
	struct sip_str call_id;
	enum sip_method method;
	uint32_t cseq;
	bool final;

	if (!set || !resp || resp->is_request)
		return NULL;
	if (resp->status < 101 || resp->status > 299)
		return NULL;
	if (!hdr_cseq_get(resp, &cseq, &method) || method != SIP_METHOD_INVITE)
		return NULL;
	if (!hdr_call_id_get(resp, &call_id))
		return NULL;
	if (!hdr_addr_get(resp, SIP_HDR_FROM, &from) || !hdr_addr_get(resp, SIP_HDR_TO, &to))
		return NULL;
	if (!from.tag.len)
		return NULL;

	final = resp->status >= 200;

	/* RFC 3261 12.1.2: a provisional without a To tag creates no dialog. */
	if (!final && !to.tag.len)
		return NULL;

	d = dlg_resp_find(set, call_id, from.tag, to.tag, 0);

	/*
	 * A 2xx without a To tag is tolerated (sip/dialog.h): it confirms the
	 * early dialog when there is one, so the call keeps the tag the 18x
	 * established, and builds a tagless dialog otherwise.
	 */
	if (!d && !to.tag.len)
		d = dlg_resp_find(set, call_id, from.tag, to.tag, DLG_F_ANY_REMOTE);

	if (d) {
		if (dst)
			sip_dialog_dst_set(d, dst);
		if (local)
			d->local = *local;

		target_update(d, resp);
		if (final)
			sip_dialog_confirm(d);

		return d;
	}

	/*
	 * A 200 with a To tag that differs from the early dialog is a different
	 * dialog. The early one is discarded and this one is adopted.
	 */
	early = final ? dlg_resp_find(set, call_id, from.tag, to.tag, DLG_F_ANY_REMOTE) : NULL;
	if (early && early->state == SIP_DLG_EARLY) {
		/*
		 * The early dialog stops matching at once, but its reference
		 * belongs to the caller: the caller compares the handle this
		 * call returns with the one it held and drops the old one.
		 * Dropping it here would free memory the caller still names.
		 */
		set->stat.replaced++;
		dlg_trace(early, "replaced by a 2xx with another To tag");
		sip_dialog_terminate(early);
	}

	d = dlg_alloc(set);
	if (!d)
		return NULL;

	if (local)
		d->local = *local;
	if (dst)
		sip_dialog_dst_set(d, dst);

	if (!uac_fill(d, resp, call_id, &from, &to, cseq)) {
		set->stat.denied++;
		dlg_release(d);
		return NULL;
	}

	d->state = final ? SIP_DLG_CONFIRMED : SIP_DLG_EARLY;
	if (final)
		set->stat.confirmed++;

	dlg_trace(d, "created from %u", resp->status);

	return d;
}

/* ------------------------------------------------------------------- UAS */

struct sip_dialog *sip_dialog_uas_create(struct sip_dialog_set *set, const struct sip_msg *req,
					 const char *local_tag, const struct sip_txn_dst *dst,
					 const struct sip_dialog_local *local)
{
	struct sip_dialog *d;
	struct sip_addr from, to;
	struct sip_str call_id;
	uint32_t cseq;

	if (!set || !req || !req->is_request || req->method != SIP_METHOD_INVITE)
		return NULL;
	if (!local_tag || !*local_tag)
		return NULL;
	if (!hdr_call_id_get(req, &call_id) || !hdr_cseq_get(req, &cseq, NULL))
		return NULL;
	if (!hdr_addr_get(req, SIP_HDR_FROM, &from) || !hdr_addr_get(req, SIP_HDR_TO, &to))
		return NULL;
	if (!from.tag.len)
		return NULL;

	d = dlg_alloc(set);
	if (!d)
		return NULL;

	if (local)
		d->local = *local;
	if (dst)
		sip_dialog_dst_set(d, dst);

	d->uac = false;
	d->call_id = str_dup_str(call_id);
	d->local_tag = str_dup_cstr(local_tag);
	d->remote_tag = str_dup_str(from.tag);
	d->local_uri = str_dup_str(to.uri.raw);
	d->remote_uri = str_dup_str(from.uri.raw);
	d->remote_cseq = cseq;
	d->remote_cseq_set = true;
	d->state = SIP_DLG_EARLY;

	target_update(d, req);

	if (!route_update(d, req, false) || !d->call_id || !d->local_tag || !d->remote_tag ||
	    !d->local_uri || !d->remote_uri) {
		set->stat.denied++;
		dlg_release(d);
		return NULL;
	}

	dlg_trace(d, "created from an INVITE");

	return d;
}

/* -------------------------------------------------------------- matching */

struct sip_dialog *sip_dialog_find(struct sip_dialog_set *set, const struct sip_msg *req)
{
	struct sip_addr from, to;
	struct sip_str call_id;
	unsigned int i;

	if (!set || !req || !req->is_request)
		return NULL;
	if (!hdr_call_id_get(req, &call_id))
		return NULL;
	if (!hdr_addr_get(req, SIP_HDR_FROM, &from) || !hdr_addr_get(req, SIP_HDR_TO, &to))
		return NULL;

	for (i = 0; i < SIP_DIALOG_MAX; i++) {
		struct sip_dialog *d = &set->pool[i];

		if (!d->in_use || d->state == SIP_DLG_TERMINATED)
			continue;
		if (!str_eq_cstr(d->call_id, call_id))
			continue;
		if (!tag_eq(d->local_tag, to.tag) || !tag_eq(d->remote_tag, from.tag))
			continue;

		return d;
	}

	return NULL;
}

enum sip_dialog_cseq sip_dialog_remote_cseq_take(struct sip_dialog *d, const struct sip_msg *req)
{
	enum sip_method method;
	uint32_t cseq;

	if (!d || !req || !req->is_request)
		return SIP_DLG_CSEQ_BAD;
	if (!hdr_cseq_get(req, &cseq, &method))
		return SIP_DLG_CSEQ_BAD;

	if (d->remote_cseq_set && cseq < d->remote_cseq)
		return SIP_DLG_CSEQ_OLD;

	/* An ACK or a CANCEL carries the number of the request it belongs to,
	 * so it must never move the dialog on (RFC 3261 12.2.2). */
	if (method == SIP_METHOD_ACK || method == SIP_METHOD_CANCEL)
		return SIP_DLG_CSEQ_OK;

	if (d->remote_cseq_set && cseq == d->remote_cseq)
		return SIP_DLG_CSEQ_OLD;

	d->remote_cseq = cseq;
	d->remote_cseq_set = true;

	return SIP_DLG_CSEQ_OK;
}

/* ----------------------------------------------------------- request build */

static bool hop_uri(const char *hop, char *out, size_t olen)
{
	struct sip_addr a;

	if (!hdr_addr_parse(sip_str_cstr(hop), &a) || !a.uri.raw.len)
		return false;

	return sip_str_copy(out, olen, a.uri.raw);
}

/*
 * RFC 3261 12.2.1.1. A first hop without ;lr is a strict router: it takes the
 * request URI and the remote target moves to the end of the Route list.
 */
static bool dlg_strict(const struct sip_dialog *d)
{
	return d->routes && !route_hop_lr(sip_str_cstr(d->route[0]));
}

static bool ruri_build(const struct sip_dialog *d, char *out, size_t olen)
{
	if (dlg_strict(d))
		return hop_uri(d->route[0], out, olen);

	if (!d->remote_target)
		return false;

	return snprintf(out, olen, "%s", d->remote_target) < (int)olen;
}

static void route_write(const struct sip_dialog *d, struct sip_buf *b, bool strict)
{
	unsigned int i;

	for (i = strict ? 1 : 0; i < d->routes; i++)
		hdr_write_str(b, SIP_HDR_ROUTE, sip_str_cstr(d->route[i]));

	if (strict)
		hdr_write(b, SIP_HDR_ROUTE, "<%s>", d->remote_target);
}

static bool head_write(struct sip_dialog *d, struct sip_buf *b, const char *name, uint32_t cseq,
		       const char *branch)
{
	char ruri[DLG_URI_MAX];

	if (!ruri_build(d, ruri, sizeof(ruri)))
		return false;

	msg_req_line_write(b, name, ruri);
	hdr_via_write(b, d->local.transport, d->local.host, d->local.port,
		      branch, true);
	hdr_write(b, SIP_HDR_MAX_FORWARDS, "70");
	hdr_addr_tag_write(b, SIP_HDR_FROM, NULL, d->local_uri, d->local_tag);
	hdr_addr_tag_write(b, SIP_HDR_TO, NULL, d->remote_uri, d->remote_tag);
	hdr_write(b, SIP_HDR_CALL_ID, "%s", d->call_id);
	hdr_cseq_write(b, cseq, name);
	route_write(d, b, dlg_strict(d));

	return true;
}

bool sip_dialog_req_build(struct sip_dialog *d, struct sip_dialog_req *r, struct sip_buf *b)
{
	const char *name;
	uint32_t cseq;

	if (!d || !r || !b || !d->in_use || !r->branch)
		return false;
	if (d->state == SIP_DLG_TERMINATED || !d->local.host)
		return false;

	/* The ACK of a 2xx is not an ordinary in-dialog request: it takes the
	 * CSeq of its INVITE and this layer sends and retains it itself. */
	if (r->method == SIP_METHOD_ACK)
		return false;

	name = r->method_name ? r->method_name : sip_method_name(r->method);
	if (!name || !*name)
		return false;

	cseq = d->local_cseq + 1;
	if (!head_write(d, b, name, cseq, r->branch))
		return false;

	if (r->with_contact && d->local.contact)
		hdr_addr_write(b, SIP_HDR_CONTACT, NULL, d->local.contact, NULL);
	if (d->local.user_agent)
		hdr_write(b, SIP_HDR_USER_AGENT, "%s", d->local.user_agent);

	msg_body_write(b, r->ctype, r->body, r->body_len);
	if (!sip_buf_ok(b))
		return false;

	d->local_cseq = cseq;
	r->cseq = cseq;

	return true;
}

/* ---------------------------------------------------------------- the ACK */

bool sip_dialog_ack_stored(const struct sip_dialog *d)
{
	return d && d->ack && d->ack_len;
}

static bool ack_wire_send(struct sip_dialog *d)
{
	struct sip_txn_env *env = d->set->env;

	if (!d->dst_set || !env->send)
		return false;

	return env->send(env, d->ack, d->ack_len, &d->dst) >= 0;
}

bool sip_dialog_ack_send(struct sip_dialog *d, const struct sip_msg *resp, const char *branch,
			 const char *ctype, const char *body, size_t body_len)
{
	enum sip_method method;
	struct sip_buf b;
	uint32_t cseq;
	char *wire;

	/*
	 * Both orientations. Who created the dialog decides nothing about the
	 * ACK: a re-INVITE this side placed inside a dialog the peer created
	 * owes exactly the same ACK, and head_write() already writes our own
	 * identity in the From and the peer in the To for either one.
	 */
	if (!d || !d->in_use || !resp || resp->is_request || !branch)
		return false;
	if (resp->status < 200 || resp->status > 299)
		return false;
	if (!hdr_cseq_get(resp, &cseq, &method) || method != SIP_METHOD_INVITE)
		return false;
	if (!d->local.host || !d->dst_set)
		return false;

	sip_buf_init(&b, dlg_out, sizeof(dlg_out));
	if (!head_write(d, &b, "ACK", cseq, branch))
		return false;

	msg_body_write(&b, ctype, body, body_len);
	if (!sip_buf_ok(&b))
		return false;

	wire = str_dup_len(sip_buf_str(&b), sip_buf_len(&b));
	if (!wire)
		return false;

	free(d->ack);
	d->ack = wire;
	d->ack_len = sip_buf_len(&b);
	d->ack_cseq = cseq;

	/*
	 * The bytes are retained even when the send fails: RFC 6026 makes the
	 * retransmitted 2xx the recovery path, and answering it needs exactly
	 * these bytes.
	 */
	return ack_wire_send(d);
}

enum sip_dialog_rx sip_dialog_resp_recv(struct sip_dialog_set *set, const struct sip_msg *resp)
{
	struct sip_dialog *d;
	struct sip_addr from, to;
	struct sip_str call_id;
	enum sip_method method;
	uint32_t cseq;

	if (!set || !resp || resp->is_request)
		return SIP_DLG_RX_PASS;
	if (resp->status < 200 || resp->status > 299)
		return SIP_DLG_RX_PASS;
	if (!hdr_cseq_get(resp, &cseq, &method) || method != SIP_METHOD_INVITE)
		return SIP_DLG_RX_PASS;
	if (!hdr_call_id_get(resp, &call_id))
		return SIP_DLG_RX_PASS;
	if (!hdr_addr_get(resp, SIP_HDR_FROM, &from) || !hdr_addr_get(resp, SIP_HDR_TO, &to))
		return SIP_DLG_RX_PASS;

	/*
	 * A dialog the owner already left still owes the ACK (RFC 6026), so the
	 * terminated ones are searched too, in either orientation: the 2xx of a
	 * re-INVITE we placed inside a dialog the peer created is retransmitted
	 * exactly like the 2xx of a call we placed. A tagless retransmission
	 * falls back on the tag the 18x established, as the first copy did.
	 */
	d = dlg_resp_find(set, call_id, from.tag, to.tag,
			  DLG_F_TERMINATED | DLG_F_ANY_ROLE);
	if (!d && !to.tag.len)
		d = dlg_resp_find(set, call_id, from.tag, to.tag,
				  DLG_F_ANY_REMOTE | DLG_F_TERMINATED | DLG_F_ANY_ROLE);
	if (!d)
		return SIP_DLG_RX_PASS;

	/*
	 * The ACK belongs to one CSeq. A 2xx of an older INVITE cannot be
	 * answered with the ACK of a later one, and rebuilding it would need a
	 * branch this layer no longer owns, so it is absorbed and counted.
	 */
	if (!sip_dialog_ack_stored(d) || d->ack_cseq != cseq) {
		set->stat.absorbed++;
		dlg_trace(d, "2xx retransmission absorbed, no ACK for cseq %u", cseq);
		return SIP_DLG_RX_ABSORBED;
	}

	set->stat.reack++;
	dlg_trace(d, "2xx retransmission answered with the stored ACK");
	ack_wire_send(d);

	return SIP_DLG_RX_ABSORBED;
}
