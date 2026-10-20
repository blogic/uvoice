// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The MGCP endpoints. See mgcp/ep.h for the contracts.
 *
 * Order of a command. A handler checks every parameter of the command and
 * of its encapsulated request into scratch state, and changes nothing until
 * all of them pass (RFC 3435 2.3.5, 4.4.2). It then commits, hands the
 * digits of S: to the host as the last calls that touch the line, sends the
 * response, and only then runs what the commit owes: the quarantined events,
 * a signal that failed at its start, the line report to the core. So a NTFY
 * that the commit triggers never overtakes the response.
 */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/mgcp/ep.h"
#include "proto/proto_trace.h"
#include "proto/shared/media_fill.h"
#include "str.h"

#define ARRAY_LEN(a)	(sizeof(a) / sizeof((a)[0]))

#define EP_TRACE(gw, fmt, ...) \
	PROTO_TRACE((gw)->host, TRACE_MGCP, "mgcp " fmt, ##__VA_ARGS__)

/* proto_timer ids */
enum {
	TID_RESTART = 1,
	TID_DIGIT,
	TID_SIG,			/* TID_SIG + the slot of the signal */
};

/* The least wait before an RSIP that did not go out is sent again, so a
 * zero restart_delay_max_ms cannot spin the loop. */
#define RESTART_RETRY_MIN_MS	1000

/* The situations whose code the two dialects answer differently. NCS Table 4
 * defines none of the codes in rfc[], so ncs[] takes the nearest code that
 * the table defines. */
enum ep_err {
	E_RESTARTING = 0,
	E_DIGIT_MAP,
	E_PTIME,
	E_CONN_LIMIT,
	E_RCD,
	E_VERB,
	E_NO_PORT,
	E_ANY_NONE,
	E_DIGIT_FULL,
};

static uint32_t err_code(const struct mgcp_gw *gw, enum ep_err e)
{
	static const uint32_t rfc[] = {
		405, 537, 535, 540, 509, 504, 403, 410, 403,
	};
	static const uint32_t ncs[] = {
		520, 510, 532, 502, 505, 510, 400, 400, 400,
	};

	return gw->cfg.dialect == CFG_MGCP_NCS ? ncs[e] : rfc[e];
}

static const char *version_of(const struct mgcp_gw *gw)
{
	return gw->cfg.dialect == CFG_MGCP_NCS ? MGCP_VERSION_NCS :
						 MGCP_VERSION_RFC3435;
}

static struct proto_timer_env *tenv(const struct mgcp_gw *gw)
{
	return &gw->mgr->env->base;
}

static uint32_t rand32(struct mgcp_gw *gw)
{
	uint32_t v;

	if (gw->host->random_bytes(gw->host->ctx, &v, sizeof(v)))
		return v;

	return mgcp_txn_rand(gw->mgr);
}

static uint32_t owner_of(const struct mgcp_ep *ep)
{
	return ep->index + 1;
}

static const struct cfg_channel *channel_of(const struct mgcp_ep *ep)
{
	const struct mgcp_gw *gw = ep->gw;
	const struct cfg *cfg = gw->host->cfg(gw->host->ctx);

	return cfg ? &cfg->channel[ep->index] : NULL;
}

/* -------------------------------------------------------- the wire out */

/* The arena that holds the values of one response. */
static const char *arena_printf(struct mgcp_gw *gw, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static const char *arena_printf(struct mgcp_gw *gw, const char *fmt, ...)
{
	size_t left = sizeof(gw->arena) - gw->arena_used;
	char *at = gw->arena + gw->arena_used;
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(at, left, fmt, ap);
	va_end(ap);

	if (n < 0 || (size_t)n >= left)
		return "";

	gw->arena_used += (size_t)n + 1;

	return at;
}

struct out_cmd {
	struct sockaddr_storage dst;
	socklen_t dst_len;
	struct mgcp_ack_list acks;
	uint32_t txid;
};

static bool out_prep(struct mgcp_gw *gw, uint32_t owner, const char *entity,
		     struct out_cmd *o)
{
	memset(o, 0, sizeof(*o));
	if (!gw->env.dst ||
	    !gw->env.dst(gw->env.priv, entity, &o->dst, &o->dst_len)) {
		EP_TRACE(gw, "no address for the notified entity %s", entity);
		return false;
	}

	mgcp_txn_acks_get(gw->mgr, owner, (struct sockaddr *)&o->dst,
			  o->dst_len, &o->acks);
	o->txid = mgcp_txn_txid_next(gw->mgr);

	return true;
}

static uint32_t out_send(struct mgcp_gw *gw, uint32_t owner,
			 const struct out_cmd *o, const struct sip_buf *b,
			 uint32_t flags)
{
	struct mgcp_txn_req req = {
		.txid = o->txid,
		.wire = sip_buf_str(b),
		.len = sip_buf_len(b),
		.dst = (const struct sockaddr *)&o->dst,
		.dst_len = o->dst_len,
		.owner = owner,
		.acks = &o->acks,
		.flags = flags,
	};

	return mgcp_txn_send(gw->mgr, &req);
}

struct rsip_out {
	uint32_t owner;
	const char *endpoint;
	const char *entity;
	const char *method;
	bool has_delay;
	uint32_t delay;
	uint32_t reason;
	uint32_t flags;
};

/*
 * The handle of the RSIP, MGCP_TXN_HANDLE_NONE when it did not go out.
 * *layer says whether the transaction layer saw it: a refusal there counts
 * as a failed attempt of the disconnected procedure, a missing address or a
 * build failure does not.
 */
static uint32_t rsip_send(struct mgcp_gw *gw, const struct rsip_out *r,
			  uint32_t *txid, bool *layer)
{
	struct out_cmd o;
	struct sip_buf b;
	struct mgcp_rsip m;
	uint32_t handle;

	*txid = 0;
	if (layer)
		*layer = false;
	if (!out_prep(gw, r->owner, r->entity, &o))
		return MGCP_TXN_HANDLE_NONE;

	memset(&m, 0, sizeof(m));
	m.txid = o.txid;
	m.endpoint = r->endpoint;
	m.version = version_of(gw);
	m.acks = &o.acks;
	m.method = r->method;
	m.has_delay = r->has_delay;
	m.delay = r->delay;
	m.reason = r->reason;
	m.reason_text = NULL;

	sip_buf_init(&b, gw->wire, sizeof(gw->wire));
	if (!mgcp_rsip_build(&b, &m))
		return MGCP_TXN_HANDLE_NONE;

	if (layer)
		*layer = true;
	handle = out_send(gw, r->owner, &o, &b, r->flags);
	if (handle != MGCP_TXN_HANDLE_NONE) {
		*txid = o.txid;
		gw->stat.rsip++;
	}
	EP_TRACE(gw, "RSIP %u %s %s", o.txid, r->endpoint, r->method);

	return handle;
}

/* ------------------------------------------------------------- naming */

bool mgcp_ep_name_expand(const char *pattern, int line_no, const char *host,
			 char *out, size_t olen)
{
	struct sip_buf b;
	const char *p = pattern;

	sip_buf_init(&b, out, olen);
	while (*p) {
		if (!strncmp(p, "{line}", 6)) {
			if (line_no < 0)
				sip_buf_add(&b, "*");
			else
				sip_buf_addf(&b, "%d", line_no);
			p += 6;
			continue;
		}
		if (!strncmp(p, "{host}", 6)) {
			sip_buf_add(&b, host);
			p += 6;
			continue;
		}
		sip_buf_add_len(&b, p, 1);
		p++;
	}

	return sip_buf_ok(&b);
}

static size_t local_len(const char *name)
{
	const char *at = strrchr(name, '@');

	return at ? (size_t)(at - name) : strlen(name);
}

const char *mgcp_ep_svc_name(enum mgcp_ep_svc svc)
{
	switch (svc) {
	case MGCP_EP_RESTARTING:
		return "restarting";
	case MGCP_EP_IN_SERVICE:
		return "in_service";
	case MGCP_EP_OUT_OF_SERVICE:
		return "out_of_service";
	}

	return "?";
}

/* ----------------------------------------------- the line, as the core sees */

static bool ep_ringing(const struct mgcp_ep *ep)
{
	unsigned int i;

	for (i = 0; i < MGCP_SIG_RUN_MAX; i++) {
		if (ep->sig[i].used && ep->sig[i].def->prim == MGCP_PRIM_RING)
			return true;
	}

	return false;
}

static bool ep_has_conn(const struct mgcp_ep *ep)
{
	unsigned int i;

	for (i = 0; i < MGCP_CONN_MAX; i++) {
		if (ep->conn[i].used)
			return true;
	}

	return false;
}

/* The state words of mgcp_ep_state_name, and the state id of
 * host->line_transition. */
enum ep_state {
	ST_IDLE = 0,
	ST_OFFHOOK,
	ST_RINGING,
	ST_CONNECTED,
	ST_RESTARTING,
	ST_OOS,
};

static enum ep_state ep_state_of(const struct mgcp_ep *ep)
{
	if (ep->svc == MGCP_EP_OUT_OF_SERVICE)
		return ST_OOS;
	if (ep->svc == MGCP_EP_RESTARTING)
		return ST_RESTARTING;
	if (ep->offhook && ep_has_conn(ep))
		return ST_CONNECTED;
	if (ep->offhook)
		return ST_OFFHOOK;
	if (ep_ringing(ep))
		return ST_RINGING;

	return ST_IDLE;
}

static const char *const ep_state_words[] = {
	[ST_IDLE] = "idle",
	[ST_OFFHOOK] = "offhook",
	[ST_RINGING] = "ringing",
	[ST_CONNECTED] = "connected",
	[ST_RESTARTING] = "restarting",
	[ST_OOS] = "out_of_service",
};

static uint32_t ep_flags_of(const struct mgcp_ep *ep)
{
	uint32_t flags = 0;

	if (ep->offhook)
		flags |= PROTO_LF_OFFHOOK;
	if (ep_has_conn(ep) || ep_ringing(ep))
		flags |= PROTO_LF_CALL;
	if (ep->offhook && !ep_has_conn(ep))
		flags |= PROTO_LF_SESSION;

	return flags;
}

uint32_t mgcp_ep_line_flags(const struct mgcp_gw *gw, unsigned int line)
{
	if (line >= gw->cfg.lines)
		return 0;

	return ep_flags_of(&gw->ep[line]);
}

bool mgcp_ep_digit_play_allowed(const struct mgcp_gw *gw, unsigned int line)
{
	return line < gw->cfg.lines &&
	       gw->ep[line].svc == MGCP_EP_IN_SERVICE;
}

const char *mgcp_ep_state_name(const struct mgcp_gw *gw, unsigned int line)
{
	if (line >= gw->cfg.lines)
		return NULL;

	return ep_state_words[ep_state_of(&gw->ep[line])];
}

/* --------------------------------------------------------- the requests */

/* A request staged from a command or an embedded request (RFC 3435
 * 2.3.3), applied only when the whole command passed. */
struct sig_new {
	const struct mgcp_sig_def *def;
	struct mgcp_sig_params p;
	char name[MGCP_SIG_NAME_MAX];
	char params[64];
};

struct rq {
	bool has_x;
	bool has_r;
	bool has_s;
	bool has_d;
	bool has_q;
	bool has_t;
	bool has_n;
	bool has_b;
	char rid[MGCP_ID_MAX];
	char ne[MGCP_ENTITY_MAX];
	struct mgcp_req_ent req[MGCP_REQ_MAX];
	unsigned int nreq;
	char req_text[MGCP_TEXT_MAX];
	struct mgcp_ev_pat detect[MGCP_REQ_MAX];
	unsigned int ndetect;
	char detect_text[MGCP_TEXT_MAX];
	bool q_loop;
	bool q_discard;
	struct dialplan dp;
	char dmap[CFG_DIGIT_MAP_MAX];
	struct sig_new sig[MGCP_SIG_RUN_MAX];
	unsigned int nsig;
	/* the digits toward the phone, in the order of S: */
	const struct mgcp_sig_def *br[MGCP_BR_MAX];
	unsigned int nbr;
	char sig_text[MGCP_TEXT_MAX];
	struct proto_cid cid;
	bool cid_set;
	char bearer[8];
};

static struct rq rq_stage;

static const struct sip_str pkg_line = { "L", 1 };

/*
 * RFC 3435 2.3.3 page 38: the pairs of actions that combine. Every other
 * pair, and S, which this gateway does not support, gets 523. N with E
 * needs a request that allows more than one NTFY (4.4.1).
 */
static bool act_combine_ok(uint32_t act, bool loop)
{
	static const uint32_t pairs[] = {
		MGCP_ACT_N | MGCP_ACT_K, MGCP_ACT_N | MGCP_ACT_E,
		MGCP_ACT_A | MGCP_ACT_K, MGCP_ACT_A | MGCP_ACT_E,
		MGCP_ACT_D | MGCP_ACT_K, MGCP_ACT_K | MGCP_ACT_E,
		MGCP_ACT_K | MGCP_ACT_I,
	};
	uint32_t a, b;

	if (act & MGCP_ACT_S)
		return false;
	if ((act & (MGCP_ACT_N | MGCP_ACT_E)) == (MGCP_ACT_N | MGCP_ACT_E) &&
	    !loop)
		return false;

	for (a = 1; a <= MGCP_ACT_E; a <<= 1) {
		for (b = a << 1; b <= MGCP_ACT_E; b <<= 1) {
			size_t i;
			bool ok = false;

			if ((act & (a | b)) != (a | b))
				continue;
			for (i = 0; i < ARRAY_LEN(pairs); i++)
				ok = ok || pairs[i] == (a | b);
			if (!ok)
				return false;
		}
	}

	return true;
}

/* 4.4.2 glare is about a hook event the request names; L/all names none. */
static bool pat_hook(const struct mgcp_ev_pat *p, uint8_t kind)
{
	if (p->pkg != MGCP_PKG_L || p->all)
		return false;

	return p->kind == kind;
}

static uint32_t rq_map_check(const struct mgcp_gw *gw, struct sip_str d,
			     struct rq *q);
static uint32_t rq_sig_take(struct mgcp_gw *gw, struct mgcp_ep *ep,
			    struct sip_str s, struct rq *q, bool embedded);

/*
 * The groups of an embedded request (RFC 3435 3.2.2.16). They are checked
 * now and run when the event fires; an embedded request holds no other one
 * (2.3.3 page 40).
 */
static uint32_t rq_embedded_check(struct mgcp_gw *gw, struct mgcp_ep *ep,
				  struct sip_str e);

static uint32_t rq_req_take(struct mgcp_gw *gw, struct mgcp_ep *ep,
			    struct sip_str r, struct rq *q, bool embedded)
{
	struct mgcp_req_iter it;
	struct mgcp_req_event re;
	bool more;
	uint32_t code;

	q->has_r = true;
	q->nreq = 0;
	if (!sip_str_copy(q->req_text, sizeof(q->req_text), r))
		return 510;

	for (more = mgcp_req_first(r, pkg_line, &it, &re); more;
	     more = mgcp_req_next(&it, &re)) {
		struct mgcp_req_ent *ent;

		if (q->nreq >= MGCP_REQ_MAX)
			return 510;
		ent = &q->req[q->nreq];
		memset(ent, 0, sizeof(*ent));

		code = mgcp_ev_pat_compile(gw->cfg.dialect, &re.ev, &ent->pat);
		if (code)
			return code;
		if (re.act & MGCP_ACT_EXT)
			return 518;
		if (!act_combine_ok(re.act, q->q_loop))
			return 523;
		if (re.act & MGCP_ACT_E) {
			if (embedded)
				return 523;
			code = rq_embedded_check(gw, ep, re.embedded);
			if (code)
				return code;
			if (!sip_str_copy(ent->embedded, sizeof(ent->embedded),
					  re.embedded))
				return 510;
		}
		ent->act = re.act;

		/* 4.4.2: glare, checked when the request arrives */
		if (!embedded && ep->offhook &&
		    pat_hook(&ent->pat, MGCP_EVK_HD))
			return 401;
		if (!embedded && !ep->offhook &&
		    (pat_hook(&ent->pat, MGCP_EVK_HU) ||
		     pat_hook(&ent->pat, MGCP_EVK_HF)))
			return 402;

		q->nreq++;
	}

	return it.bad ? 510 : 0;
}

static uint32_t rq_embedded_check(struct mgcp_gw *gw, struct mgcp_ep *ep,
				  struct sip_str e)
{
	static struct rq inner;
	struct mgcp_embedded em;
	uint32_t code;

	if (!mgcp_embedded_parse(e, &em))
		return 510;

	memset(&inner, 0, sizeof(inner));
	if (em.has_r) {
		code = rq_req_take(gw, ep, em.r, &inner, true);
		if (code)
			return code;
	}
	if (em.has_d) {
		code = rq_map_check(gw, em.d, &inner);
		if (code)
			return code;
	}
	if (em.has_s)
		return rq_sig_take(gw, ep, em.s, &inner, true);

	return 0;
}

static uint32_t rq_detect_take(const struct mgcp_gw *gw, struct sip_str t,
			       struct rq *q)
{
	struct mgcp_sig_iter it;
	struct mgcp_event ev;
	bool more;
	uint32_t code;

	q->has_t = true;
	q->ndetect = 0;
	if (!sip_str_copy(q->detect_text, sizeof(q->detect_text), t))
		return 510;

	for (more = mgcp_sig_first(t, pkg_line, &it, &ev); more;
	     more = mgcp_sig_next(&it, &ev)) {
		if (q->ndetect >= MGCP_REQ_MAX)
			return 510;
		code = mgcp_ev_pat_compile(gw->cfg.dialect, &ev,
					   &q->detect[q->ndetect]);
		if (code)
			return code;
		q->ndetect++;
	}

	return it.bad ? 510 : 0;
}

/*
 * NCS 7.1.5 page 24 allows a timer in the last position of a string alone,
 * so an ncs endpoint refuses a T with more symbols behind it.
 */
static bool map_mid_timer(const char *map)
{
	const char *p;

	for (p = map; *p; p++) {
		const char *q;

		if (*p != 'T' && *p != 't')
			continue;
		for (q = p + 1; *q == ' ' || *q == '\t'; q++)
			;
		if (*q && *q != '|')
			return true;
	}

	return false;
}

/* RFC 3435 Appendix A: DigitMap = DigitString / "(" DigitStringList ")" */
static uint32_t rq_map_check(const struct mgcp_gw *gw, struct sip_str d,
			     struct rq *q)
{
	d = sip_str_trim(d);
	if (d.len >= 2 && d.p[0] == '(' && d.p[d.len - 1] == ')')
		d = sip_str_trim(sip_str_init(d.p + 1, d.len - 2));

	q->has_d = true;
	if (!sip_str_copy(q->dmap, sizeof(q->dmap), d))
		return err_code(gw, E_DIGIT_MAP);
	if (gw->cfg.dialect == CFG_MGCP_NCS && map_mid_timer(q->dmap))
		return err_code(gw, E_DIGIT_MAP);
	if (!dialplan_compile(q->dmap, &q->dp))
		return err_code(gw, E_DIGIT_MAP);

	return 0;
}

static uint32_t rq_quarantine_take(struct sip_str v, struct rq *q)
{
	struct mgcp_quarantine qh;

	if (!mgcp_quarantine_parse(v, &qh))
		return 508;

	q->has_q = true;
	q->q_loop = qh.loop;
	q->q_discard = qh.discard;

	return 0;
}

static bool sig_name_write(const struct mgcp_sig_def *def, char *out,
			   size_t olen)
{
	int n = snprintf(out, olen, "%s/%s", mgcp_pkg_name(def->pkg),
			 def->name);

	return n > 0 && (size_t)n < olen;
}

/* The hook prerequisite of a signal (RFC 3435 4.4.2 page 133). */
static uint32_t sig_hook_check(const struct mgcp_ep *ep,
			       const struct mgcp_sig_def *def)
{
	if ((def->flags & MGCP_SIG_F_OFFHOOK) && !ep->offhook)
		return 402;
	if ((def->flags & MGCP_SIG_F_ONHOOK) && ep->offhook)
		return 401;

	return 0;
}

static const struct mgcp_conn *conn_find_id(const struct mgcp_ep *ep,
					    struct sip_str id)
{
	unsigned int i;

	for (i = 0; i < MGCP_CONN_MAX; i++) {
		if (ep->conn[i].used && sip_str_eq_ci(id, ep->conn[i].id))
			return &ep->conn[i];
	}

	return NULL;
}

/*
 * A signal on a connection. The connection must exist (515) and hold a
 * remote description (RFC 3435 2.3.3 page 41: 527). The HAL plays no tone
 * toward the far end, so every such signal then gets 513.
 */
static uint32_t sig_conn_check(const struct mgcp_ep *ep,
			       const struct mgcp_event *ev)
{
	const struct mgcp_conn *c;

	if (sip_str_eq(ev->conn, "*") || sip_str_eq(ev->conn, "$"))
		return 513;

	c = conn_find_id(ep, ev->conn);
	if (!c)
		return 515;
	if (!c->remote)
		return 527;

	return 513;
}

static uint32_t sig_one_take(struct mgcp_gw *gw, struct mgcp_ep *ep,
			     const struct mgcp_event *ev, struct rq *q,
			     bool embedded)
{
	const struct mgcp_sig_def *def;
	struct sig_new *s;
	uint32_t code = 0;

	def = mgcp_sig_lookup(gw->cfg.dialect, ev, &code);
	if (!def)
		return code;
	if (ev->conn.len)
		return sig_conn_check(ep, ev);
	if (def->prim == MGCP_PRIM_NONE)
		return 513;
	if (def->prim == MGCP_PRIM_LSA &&
	    !(gw->host->hal_features(gw->host->ctx) & HAL_FEAT_POLARITY))
		return 513;
	if (!embedded) {
		code = sig_hook_check(ep, def);
		if (code)
			return code;
	}

	if (def->prim == MGCP_PRIM_CID) {
		if (!ev->has_params || !mgcp_sig_cid_parse(ev->params, &q->cid))
			return 538;
		q->cid_set = true;
	}

	if (def->prim == MGCP_PRIM_DIGIT) {
		struct mgcp_sig_params p;

		if (!mgcp_sig_params_parse(def, ev, &p))
			return 538;
		if (q->nbr >= MGCP_BR_MAX)
			return 510;
		q->br[q->nbr++] = def;
		return 0;
	}

	if (q->nsig >= MGCP_SIG_RUN_MAX)
		return 510;
	s = &q->sig[q->nsig];
	memset(s, 0, sizeof(*s));
	s->def = def;
	if (!mgcp_sig_params_parse(def, ev, &s->p))
		return 538;
	sig_name_write(def, s->name, sizeof(s->name));
	if (ev->has_params)
		sip_str_copy(s->params, sizeof(s->params), ev->params);
	q->nsig++;

	return 0;
}

static uint32_t rq_sig_take(struct mgcp_gw *gw, struct mgcp_ep *ep,
			    struct sip_str s, struct rq *q, bool embedded)
{
	struct mgcp_sig_iter it;
	struct mgcp_event ev;
	bool more;
	uint32_t code;

	q->has_s = true;
	q->nsig = 0;
	q->nbr = 0;
	q->cid_set = false;
	if (!sip_str_copy(q->sig_text, sizeof(q->sig_text), s))
		return 510;

	for (more = mgcp_sig_first(s, pkg_line, &it, &ev); more;
	     more = mgcp_sig_next(&it, &ev)) {
		code = sig_one_take(gw, ep, &ev, q, embedded);
		if (code)
			return code;
	}

	return it.bad ? 510 : 0;
}

/* B: e:mu or e:A (RFC 3435 3.2.2.1); any other value gets 532. */
static uint32_t rq_bearer_take(const struct mgcp_msg *msg, struct rq *q)
{
	struct mgcp_bearer b;

	if (!mgcp_msg_bearer(msg, &b))
		return 510;
	if (!sip_str_eq_ci(b.e, "mu") && !sip_str_eq(b.e, "A"))
		return 532;

	q->has_b = true;
	sip_str_copy(q->bearer, sizeof(q->bearer), b.e);

	return 0;
}

/* Whether msg carries a parameter of an encapsulated request besides X:
 * RFC 3435 2.3.5 page 50, 2.3.6 page 53 and 2.3.7 page 57 require X with
 * them. */
static bool rq_params_present(const struct mgcp_msg *msg)
{
	static const enum mgcp_param codes[] = {
		MGCP_PARAM_R, MGCP_PARAM_S, MGCP_PARAM_D, MGCP_PARAM_Q,
		MGCP_PARAM_T,
	};
	size_t i;

	for (i = 0; i < ARRAY_LEN(codes); i++) {
		if (mgcp_msg_param(msg, codes[i]))
			return true;
	}

	return false;
}

/*
 * Stage the request parameters of msg (RFC 3435 2.3.3). The first parameter
 * that fails sets the return code. need_x says the command is an RQNT, where
 * X is mandatory.
 */
static uint32_t rq_take(struct mgcp_gw *gw, struct mgcp_ep *ep,
			const struct mgcp_msg *msg, struct rq *q, bool need_x)
{
	struct mgcp_entity ent;
	struct sip_str v;
	uint32_t code;

	memset(q, 0, sizeof(*q));
	q->q_loop = gw->cfg.quarantine_loop;

	if (mgcp_msg_param(msg, MGCP_PARAM_X)) {
		if (!mgcp_msg_request_id(msg, &v) ||
		    !sip_str_copy(q->rid, sizeof(q->rid), v))
			return 510;
		q->has_x = true;
	} else if (need_x) {
		return 510;
	}

	if (mgcp_msg_value(msg, MGCP_PARAM_N, &v)) {
		if (!mgcp_entity_parse(v, &ent) ||
		    !sip_str_copy(q->ne, sizeof(q->ne), v))
			return 510;
		q->has_n = true;
	}

	if (mgcp_msg_param(msg, MGCP_PARAM_B)) {
		code = rq_bearer_take(msg, q);
		if (code)
			return code;
	}

	/* the parameters below belong to a request, which needs X */
	if (!q->has_x)
		return rq_params_present(msg) ? 510 : 0;

	if (mgcp_msg_value(msg, MGCP_PARAM_Q, &v)) {
		code = rq_quarantine_take(v, q);
		if (code)
			return code;
	}
	if (mgcp_msg_value(msg, MGCP_PARAM_R, &v)) {
		code = rq_req_take(gw, ep, v, q, false);
		if (code)
			return code;
	}
	if (mgcp_msg_digit_map(msg, &v)) {
		code = rq_map_check(gw, v, q);
		if (code)
			return code;
	}
	if (mgcp_msg_value(msg, MGCP_PARAM_S, &v)) {
		code = rq_sig_take(gw, ep, v, q, false);
		if (code)
			return code;
	}
	if (mgcp_msg_value(msg, MGCP_PARAM_T, &v))
		return rq_detect_take(gw, v, q);

	return 0;
}

/* ------------------------------------------------------------- signals */

static void obs_later(struct mgcp_ep *ep, uint8_t kind, const char *sig)
{
	struct mgcp_obs *o;

	if (ep->nlater >= ARRAY_LEN(ep->later))
		return;

	o = &ep->later[ep->nlater++];
	memset(o, 0, sizeof(*o));
	o->kind = kind;
	str_copy(o->sig, sizeof(o->sig), sig);
}

/* The stream the digits toward the phone go on: the first connection of the
 * endpoint. The stream of a connection is its slot index. */
static unsigned int digit_stream(const struct mgcp_ep *ep)
{
	unsigned int i;

	for (i = 0; i < MGCP_CONN_MAX; i++) {
		if (ep->conn[i].used)
			return i;
	}

	return 0;
}

static void sig_prim_off(struct mgcp_ep *ep, const struct mgcp_sig_def *def)
{
	const struct proto_host *h = ep->gw->host;

	switch (def->prim) {
	case MGCP_PRIM_TONE:
		h->tone_stop(h->ctx, ep->index);
		break;
	case MGCP_PRIM_RING:
		h->ring_stop(h->ctx, ep->index);
		break;
	case MGCP_PRIM_CW:
		h->cw_alert(h->ctx, ep->index, false, 0, NULL);
		break;
	case MGCP_PRIM_OSI:
	case MGCP_PRIM_LSA:
		h->battery_set(h->ctx, ep->index, PROTO_BATT_ACTIVE);
		break;
	default:
		break;
	}
}

static void sig_run_clear(struct mgcp_ep *ep, struct mgcp_sig_run *run)
{
	proto_timer_cancel(tenv(ep->gw), &run->timer);
	run->used = false;
	run->def = NULL;
}

static void sig_expire(struct proto_timer *t);
static void ep_event(struct mgcp_ep *ep, const struct mgcp_obs *o);
static void ep_post(struct mgcp_ep *ep);
static void br_cancel(struct mgcp_ep *ep);

/*
 * RFC 3435 2.3.3 page 42 and 2.1.7 page 32: a requested event stops every
 * TO signal and the pending BR digits, unless its actions carry K. No oc
 * goes out for them.
 */
static void sig_stop_to(struct mgcp_ep *ep)
{
	unsigned int i;

	for (i = 0; i < MGCP_SIG_RUN_MAX; i++) {
		struct mgcp_sig_run *run = &ep->sig[i];

		if (!run->used || run->def->type != MGCP_SIG_TO)
			continue;
		sig_prim_off(ep, run->def);
		sig_run_clear(ep, run);
	}
	br_cancel(ep);
}

/* The ring template cycle of the line, for the rep of a ring signal. */
static uint32_t ring_cycle_ms(const struct mgcp_ep *ep, unsigned int tpl)
{
	const struct proto_host *h = ep->gw->host;
	const struct hal_region *r = h->region(h->ctx);
	uint32_t ms = 0;
	unsigned int i;

	if (!r || tpl >= HAL_REGION_RING_MAX)
		return 0;

	for (i = 0; i < r->ring[tpl].cadence_count && i < HAL_CADENCE_MAX;
	     i++)
		ms += r->ring[tpl].cadence[i].on_ms +
		      r->ring[tpl].cadence[i].off_ms;

	return ms;
}

/*
 * Start one signal. The answer of the host is the start result: a refusal
 * of a TO signal is its failure (RFC 3435 2.1.7 page 32).
 */
static enum hal_status sig_prim_on(struct mgcp_ep *ep,
				   const struct sig_new *s,
				   const struct rq *q, uint32_t *to_ms)
{
	struct mgcp_gw *gw = ep->gw;
	const struct proto_host *h = gw->host;
	const struct mgcp_sig_def *def = s->def;
	const struct proto_cid *cid = q->cid_set ? &q->cid : NULL;
	unsigned int tpl;

	*to_ms = mgcp_sig_timeout(def, &s->p, 0);

	switch (def->prim) {
	case MGCP_PRIM_TONE:
		return h->tone_play(h->ctx, ep->index, (enum hal_tone)def->arg,
				    def->type == MGCP_SIG_BR ?
				    h->region(h->ctx)->confirm_total_ms : 0);
	case MGCP_PRIM_RING:
		tpl = mgcp_sig_ring_template(h, ep->index, def,
					     cid ? cid->number : "");
		*to_ms = mgcp_sig_timeout(def, &s->p, ring_cycle_ms(ep, tpl));
		h->ring_start(h->ctx, ep->index, PROTO_RING_CALL, tpl, 0, cid);
		return HAL_OK;
	case MGCP_PRIM_SPLASH:
		h->ring_start(h->ctx, ep->index, PROTO_RING_CALL, 0,
			      MGCP_SIG_SPLASH_MS, NULL);
		return HAL_OK;
	case MGCP_PRIM_CW:
		h->cw_alert(h->ctx, ep->index, true, def->arg, cid);
		return HAL_OK;
	case MGCP_PRIM_OSI:
		return h->battery_set(h->ctx, ep->index, PROTO_BATT_DOWN);
	default:
		return HAL_OK;
	}
}

static struct mgcp_sig_run *sig_run_find(struct mgcp_ep *ep,
					 const struct mgcp_sig_def *def)
{
	unsigned int i;

	for (i = 0; i < MGCP_SIG_RUN_MAX; i++) {
		if (ep->sig[i].used && ep->sig[i].def == def)
			return &ep->sig[i];
	}

	return NULL;
}

static struct mgcp_sig_run *sig_run_alloc(struct mgcp_ep *ep)
{
	unsigned int i;

	for (i = 0; i < MGCP_SIG_RUN_MAX; i++) {
		if (!ep->sig[i].used)
			return &ep->sig[i];
	}

	return NULL;
}

static bool sig_listed(const struct rq *q, const struct mgcp_sig_def *def)
{
	unsigned int i;

	for (i = 0; i < q->nsig; i++) {
		if (q->sig[i].def == def)
			return true;
	}

	return false;
}

static bool sig_alert_listed(const struct rq *q)
{
	unsigned int i;

	for (i = 0; i < q->nsig; i++) {
		if (q->sig[i].def->prim == MGCP_PRIM_RING ||
		    q->sig[i].def->prim == MGCP_PRIM_CW)
			return true;
	}

	return false;
}

static void sig_oo_apply(struct mgcp_ep *ep, const struct sig_new *s)
{
	const struct proto_host *h = ep->gw->host;
	bool on = !s->p.off;

	if (s->def->prim == MGCP_PRIM_VMWI) {
		if (on == ep->vmwi_on)
			return;
		ep->vmwi_on = on;
		h->mwi_set(h->ctx, ep->index, on, 0, 0);
		return;
	}

	if (s->def->prim == MGCP_PRIM_LSA && on != ep->lsa_on) {
		ep->lsa_on = on;
		h->battery_set(h->ctx, ep->index,
			       on ? PROTO_BATT_REVERSE : PROTO_BATT_ACTIVE);
	}
}

static void sig_to_start(struct mgcp_ep *ep, const struct sig_new *s,
			 const struct rq *q)
{
	struct mgcp_sig_run *run;
	enum hal_status st;
	uint32_t to_ms;

	if (sig_run_find(ep, s->def))
		return;

	run = sig_run_alloc(ep);
	if (!run)
		return;

	st = sig_prim_on(ep, s, q, &to_ms);
	if (st != HAL_OK) {
		EP_TRACE(ep->gw, "line %u %s did not start", ep->index,
			 s->name);
		obs_later(ep, MGCP_EVK_OF, s->name);
		return;
	}

	run->used = true;
	run->def = s->def;
	str_copy(run->name, sizeof(run->name), s->name);
	str_copy(run->params, sizeof(run->params), s->params);
	proto_timer_init(&run->timer, ep,
			 TID_SIG + (unsigned int)(run - ep->sig), sig_expire);
	if (to_ms)
		proto_timer_arm(tenv(ep->gw), &run->timer, to_ms);
}

static void sig_br_start(struct mgcp_ep *ep, const struct sig_new *s,
			 const struct rq *q)
{
	const struct proto_host *h = ep->gw->host;
	uint32_t to_ms;

	if (s->def->prim == MGCP_PRIM_CID) {
		if (!sig_alert_listed(q))
			h->cid_send(h->ctx, ep->index,
				    ep->offhook ? HAL_CID_TYPE2 : HAL_CID_TYPE1,
				    &q->cid);
		return;
	}

	sig_prim_on(ep, s, q, &to_ms);
}

/* The pending BR digits go; the one that plays ends on its own (2.1.7). */
static void br_cancel(struct mgcp_ep *ep)
{
	const struct proto_host *h = ep->gw->host;

	if (!ep->br_pending[0])
		return;

	h->digit_play_cancel(h->ctx, ep->index, ep->br_stream);
	ep->br_pending[0] = '\0';
}

/*
 * The digits of a request go to the host on the stream of the connections
 * the command left, which queues them: the first plays, the next ones wait
 * behind it and an audit lists them (2.1.7, 2.3.10 page 62). The pending
 * digits of the request in force go first, because the new request replaces
 * the signal list. When the host refuses a digit, the digits this call
 * queued go too, and the answer is false. A digit that started already plays
 * to its end.
 */
static bool br_submit(struct mgcp_ep *ep, const struct rq *q)
{
	const struct proto_host *h = ep->gw->host;
	unsigned int stream = digit_stream(ep);
	struct sip_buf b;
	char name[MGCP_SIG_NAME_MAX];
	unsigned int i;

	br_cancel(ep);
	ep->br_stream = stream;
	sip_buf_init(&b, ep->br_pending, sizeof(ep->br_pending));
	for (i = 0; i < q->nbr; i++) {
		if (!h->digit_play(h->ctx, ep->index, stream,
				   (char)q->br[i]->arg, MGCP_SIG_DIGIT_MS,
				   false)) {
			if (i)
				h->digit_play_cancel(h->ctx, ep->index, stream);
			ep->br_pending[0] = '\0';
			EP_TRACE(ep->gw, "line %u digit %u refused", ep->index,
				 i);
			return false;
		}
		if (!i || !sig_name_write(q->br[i], name, sizeof(name)))
			continue;
		sip_buf_addf(&b, "%s%s", i > 1 ? "," : "", name);
	}
	if (!sip_buf_ok(&b))
		ep->br_pending[0] = '\0';

	return true;
}

/*
 * The digits of a committed request, as the last calls of the command that
 * touch the line: a tone that the request stops shares the playout with a
 * digit. A refused digit leaves the request in force, and only a CRCX takes
 * back its connection. The code reports a resource short at this time
 * (RFC 3661 p. 9: 403, and 400 under ncs, whose Table 4 holds no 403).
 */
static uint32_t rq_digits_play(struct mgcp_ep *ep, const struct rq *q)
{
	if (!q->has_x)
		return 0;

	return br_submit(ep, q) ? 0 : err_code(ep->gw, E_DIGIT_FULL);
}

/*
 * A new SignalRequests (RFC 3435 2.3.3 page 42, 2.1.7 page 32): the TO
 * signals it leaves out stop, the ones it names again keep their timer and
 * parameters, and an OO signal changes only on its own "+" or "-". The BR
 * digits go to the host after it, in rq_digits_play().
 */
static void sig_apply(struct mgcp_ep *ep, const struct rq *q)
{
	unsigned int i;

	for (i = 0; i < MGCP_SIG_RUN_MAX; i++) {
		struct mgcp_sig_run *run = &ep->sig[i];

		if (!run->used || run->def->type != MGCP_SIG_TO)
			continue;
		if (sig_listed(q, run->def))
			continue;
		sig_prim_off(ep, run->def);
		sig_run_clear(ep, run);
	}

	for (i = 0; i < q->nsig; i++) {
		const struct sig_new *s = &q->sig[i];

		switch (s->def->type) {
		case MGCP_SIG_TO:
			sig_to_start(ep, s, q);
			break;
		case MGCP_SIG_OO:
			sig_oo_apply(ep, s);
			break;
		default:
			sig_br_start(ep, s, q);
			break;
		}
	}
}

static void sig_expire(struct proto_timer *t)
{
	struct mgcp_ep *ep = t->owner;
	struct mgcp_sig_run *run = &ep->sig[t->id - TID_SIG];
	struct mgcp_obs o = { .kind = MGCP_EVK_OC };

	if (!run->used)
		return;

	str_copy(o.sig, sizeof(o.sig), run->name);
	sig_prim_off(ep, run->def);
	run->used = false;
	run->def = NULL;

	ep_event(ep, &o);
	ep_post(ep);
}

/* The TO signal of the class that the host reports as failed. */
static void sig_failed(struct mgcp_ep *ep, enum proto_signal sig)
{
	unsigned int i;

	for (i = 0; i < MGCP_SIG_RUN_MAX; i++) {
		struct mgcp_sig_run *run = &ep->sig[i];
		struct mgcp_obs o = { .kind = MGCP_EVK_OF };
		uint8_t prim;

		if (!run->used || run->def->type != MGCP_SIG_TO)
			continue;

		prim = run->def->prim;
		if (!((sig == PROTO_SIG_TONE && prim == MGCP_PRIM_TONE) ||
		      (sig == PROTO_SIG_RING && prim == MGCP_PRIM_RING) ||
		      (sig == PROTO_SIG_ALERT && prim == MGCP_PRIM_CW)))
			continue;

		str_copy(o.sig, sizeof(o.sig), run->name);
		sig_run_clear(ep, run);
		ep_event(ep, &o);
		return;
	}
}

/* ------------------------------------------------- observed events, NTFY */

static void obs_add(struct mgcp_ep *ep, const struct mgcp_obs *o)
{
	if (ep->nobs >= MGCP_OBS_MAX) {
		EP_TRACE(ep->gw, "line %u observed list full", ep->index);
		return;
	}

	ep->obs[ep->nobs++] = *o;
}

static void dial_reset(struct mgcp_ep *ep)
{
	ep->ndial = 0;
	ep->dial[0] = '\0';
	proto_timer_cancel(tenv(ep->gw), &ep->digit_timer);
}

/*
 * One NTFY with the observed events (RFC 3435 2.3.4). The endpoint then
 * stays in the notification state until a response arrives: it cannot put
 * two NTFY commands in one datagram, so it runs the lockstep of 4.4.1 step
 * f (page 130).
 */
static void ntfy_send(struct mgcp_ep *ep)
{
	struct mgcp_gw *gw = ep->gw;
	char observed[MGCP_TEXT_MAX];
	struct sip_buf ob, b;
	struct out_cmd o;
	struct mgcp_ntfy n;
	unsigned int i;

	sip_buf_init(&ob, observed, sizeof(observed));
	for (i = 0; i < ep->nobs; i++) {
		if (i)
			sip_buf_add(&ob, ",");
		mgcp_obs_write(gw->cfg.dialect, &ep->obs[i], &ob);
	}

	ep->notif = true;
	ep->notified = true;
	ep->ntfy_txid = 0;
	proto_timer_cancel(tenv(gw), &ep->digit_timer);

	if (!out_prep(gw, owner_of(ep), ep->entity, &o))
		return;

	memset(&n, 0, sizeof(n));
	n.txid = o.txid;
	n.endpoint = ep->name;
	n.version = version_of(gw);
	n.acks = &o.acks;
	n.notified_entity = ep->req_ne[0] ? ep->req_ne : NULL;
	n.request_id = ep->rid;
	n.observed = sip_buf_str(&ob);

	sip_buf_init(&b, gw->wire, sizeof(gw->wire));
	if (!mgcp_ntfy_build(&b, &n))
		return;

	if (out_send(gw, owner_of(ep), &o, &b, 0) != MGCP_TXN_HANDLE_NONE)
		ep->ntfy_txid = o.txid;
	gw->stat.ntfy++;
	gw->host->calltrace_event(gw->host->ctx, ep->index, "ntfy");
	EP_TRACE(gw, "line %u NTFY %u X %s O %s", ep->index, o.txid, ep->rid,
		 observed);
}

/* The state in which a detected event goes to the quarantine buffer
 * (RFC 3435 4.4.1 pages 127 and 128). */
static bool ep_quarantined(const struct mgcp_ep *ep)
{
	if (ep->svc != MGCP_EP_IN_SERVICE || ep->disc)
		return true;
	if (ep->notif)
		return true;

	return ep->notified && !ep->q_loop;
}

static const struct mgcp_req_ent *req_find(const struct mgcp_ep *ep,
					   const struct mgcp_obs *o)
{
	unsigned int i;

	for (i = 0; i < ep->nreq; i++) {
		if (mgcp_ev_pat_match(ep->gw->cfg.dialect, &ep->req[i].pat, o))
			return &ep->req[i];
	}

	return NULL;
}

static bool pat_list_match(const struct mgcp_ep *ep,
			   const struct mgcp_ev_pat *p, unsigned int n,
			   const struct mgcp_obs *o)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (mgcp_ev_pat_match(ep->gw->cfg.dialect, &p[i], o))
			return true;
	}

	return false;
}

static bool persist_match(const struct mgcp_ep *ep, const struct mgcp_obs *o)
{
	return pat_list_match(ep, ep->persist, ep->npersist, o);
}

/* RFC 3435 4.4.1 page 128: the events detected in the quarantine state. */
static bool ep_detectable(const struct mgcp_ep *ep, const struct mgcp_obs *o)
{
	if (req_find(ep, o) || persist_match(ep, o))
		return true;

	return ep->detect_set &&
	       pat_list_match(ep, ep->detect, ep->ndetect, o);
}

static void ep_handle(struct mgcp_ep *ep, const struct mgcp_obs *o);

/* Handle quarantined events in order until one sends a NTFY. */
static void quar_run(struct mgcp_ep *ep)
{
	while (ep->nquar && !ep_quarantined(ep)) {
		struct mgcp_obs o = ep->quar[0];

		ep->nquar--;
		memmove(&ep->quar[0], &ep->quar[1],
			ep->nquar * sizeof(ep->quar[0]));
		ep_handle(ep, &o);
	}
}

static void quar_add(struct mgcp_ep *ep, const struct mgcp_obs *o)
{
	if (ep->nquar >= MGCP_QUAR_MAX) {
		ep->quar_dropped++;
		ep->gw->stat.quar_dropped++;
		EP_TRACE(ep->gw, "line %u quarantine full, event discarded",
			 ep->index);
		return;
	}

	ep->quar[ep->nquar++] = *o;
}

/*
 * The endpoint leaves the notification state: the observed events and the
 * dial string reset, and under loop the quarantined events run (RFC 3435
 * 4.4.1 page 128).
 */
static void notif_leave(struct mgcp_ep *ep)
{
	ep->notif = false;
	ep->ntfy_txid = 0;
	ep->nobs = 0;
	dial_reset(ep);
	quar_run(ep);
}

static void embedded_run(struct mgcp_ep *ep, const struct mgcp_req_ent *ent);
static void dial_take(struct mgcp_ep *ep, const struct mgcp_obs *o);
static void dial_timer_take(struct mgcp_ep *ep, const struct mgcp_obs *o);

static void ep_handle(struct mgcp_ep *ep, const struct mgcp_obs *o)
{
	const struct mgcp_req_ent *ent = req_find(ep, o);
	uint32_t act;

	if (ent)
		act = ent->act;
	else if (persist_match(ep, o))
		act = MGCP_ACT_N;
	else
		return;

	if (!(act & MGCP_ACT_K))
		sig_stop_to(ep);

	if (act & MGCP_ACT_I)
		return;

	if ((act & MGCP_ACT_D) && o->kind == MGCP_EVK_T) {
		dial_timer_take(ep, o);
		return;
	}
	if (act & MGCP_ACT_D) {
		dial_take(ep, o);
		return;
	}

	if (act & (MGCP_ACT_A | MGCP_ACT_N))
		obs_add(ep, o);
	if ((act & MGCP_ACT_E) && ent)
		embedded_run(ep, ent);
	if (act & MGCP_ACT_N)
		ntfy_send(ep);
}

/* One detected event: quarantined, or handled now. */
static void ep_event(struct mgcp_ep *ep, const struct mgcp_obs *o)
{
	if (ep->svc == MGCP_EP_OUT_OF_SERVICE)
		return;

	if (ep_quarantined(ep)) {
		if (ep_detectable(ep, o))
			quar_add(ep, o);
		return;
	}

	ep_handle(ep, o);
}

/* ------------------------------------------------------ digit collection */

/*
 * The critical (short) or partial (long) interdigit timer, from the channel
 * options the SIP collector reads too. Their defaults are the RFC 3660
 * values of page 15, 4 s and 16 s.
 */
static uint32_t digit_timer_ms(const struct mgcp_ep *ep, bool partial)
{
	const struct cfg_channel *ch = channel_of(ep);
	unsigned int s;

	if (!ch)
		return partial ? MGCP_SIG_TPAR_MS : MGCP_SIG_TCRIT_MS;

	s = partial ? ch->interdigit_long_s : ch->interdigit_s;
	if (s > LINE_TIMER_MAX_S)
		s = LINE_TIMER_MAX_S;

	return s * 1000u;
}

static void digit_timer_arm(struct mgcp_ep *ep, enum dp_action act)
{
	if (!ep->dial_timer)
		return;

	if (act == DP_ACT_WAIT_SHORT)
		proto_timer_arm(tenv(ep->gw), &ep->digit_timer,
				digit_timer_ms(ep, false));
	else if (act == DP_ACT_WAIT_LONG)
		proto_timer_arm(tenv(ep->gw), &ep->digit_timer,
				digit_timer_ms(ep, true));
}

/* The verdict of the dial string: a perfect or an impossible match sends
 * the NTFY (RFC 3435 2.1.5 page 25); otherwise the timer runs. */
static void dial_verdict(struct mgcp_ep *ep)
{
	struct dp_match m;
	enum dp_action act;

	dialplan_match(&ep->dp, ep->dial, ep->ndial, &m);
	act = dp_action_of(&m);

	if (act == DP_ACT_DIAL || act == DP_ACT_REORDER) {
		ntfy_send(ep);
		return;
	}

	digit_timer_arm(ep, act);
}

static void dial_take(struct mgcp_ep *ep, const struct mgcp_obs *o)
{
	obs_add(ep, o);

	if (o->kind != MGCP_EVK_DIGIT ||
	    ep->ndial >= DIALPLAN_DIGITS_MAX) {
		/* an event that is no digit cannot match the map (2.3.3) */
		ntfy_send(ep);
		return;
	}

	ep->dial[ep->ndial++] = o->digit;
	ep->dial[ep->ndial] = '\0';
	dial_verdict(ep);
}

/*
 * The timer T under the digit map action (dialplan.h collector contract,
 * RFC 3435 2.1.5 page 25), from an expiry or from the quarantine buffer. A
 * DIAL verdict dials without a T. Else, when a timer completes or continues
 * an alternative, the T joins the dial string and the map runs again. Else
 * the expiry keeps its own meaning, and the NTFY goes. The T is an observed
 * event in each case but the first, at its place in O:.
 */
static void dial_timer_take(struct mgcp_ep *ep, const struct mgcp_obs *o)
{
	struct dp_match m;

	dialplan_match(&ep->dp, ep->dial, ep->ndial, &m);
	if (m.complete_now) {
		ntfy_send(ep);
		return;
	}

	obs_add(ep, o);
	if ((m.complete_on_timer || m.timer_next) &&
	    ep->ndial < DIALPLAN_DIGITS_MAX) {
		ep->dial[ep->ndial++] = 'T';
		ep->dial[ep->ndial] = '\0';
		dial_verdict(ep);
		return;
	}

	ntfy_send(ep);
}

static void digit_expire(struct proto_timer *t)
{
	struct mgcp_ep *ep = t->owner;
	struct mgcp_obs o = { .kind = MGCP_EVK_T };

	ep_event(ep, &o);
	ep_post(ep);
}

/* -------------------------------------------------------- request apply */

static void detect_arm(struct mgcp_ep *ep)
{
	const struct proto_host *h = ep->gw->host;
	uint32_t mask = 0;
	unsigned int i;

	for (i = 0; i < ep->nreq; i++)
		mask |= mgcp_ev_pat_detect_mask(ep->gw->cfg.dialect,
						&ep->req[i].pat);
	for (i = 0; ep->detect_set && i < ep->ndetect; i++)
		mask |= mgcp_ev_pat_detect_mask(ep->gw->cfg.dialect,
						&ep->detect[i]);

	if (!h->detector_arm(h->ctx, ep->index, mask))
		EP_TRACE(ep->gw, "line %u detector refused mask 0x%x",
			 ep->index, mask);
}

/* The timer T of a request without the digit map action, which runs at
 * once and stops at the first digit (RFC 3660 page 14). */
static void timer_plain_arm(struct mgcp_ep *ep)
{
	unsigned int i;

	ep->dial_timer = false;
	for (i = 0; i < ep->nreq; i++) {
		const struct mgcp_req_ent *ent = &ep->req[i];

		if (ent->pat.kind != MGCP_EVK_DIGIT || !ent->pat.timer)
			continue;
		if (ent->act & MGCP_ACT_D) {
			ep->dial_timer = true;
			continue;
		}
		proto_timer_arm(tenv(ep->gw), &ep->digit_timer,
				digit_timer_ms(ep, false));
	}
}

/* The part of a request that an embedded request replaces too. */
static void rq_events_apply(struct mgcp_ep *ep, const struct rq *q)
{
	if (q->has_r) {
		memcpy(ep->req, q->req, sizeof(q->req[0]) * q->nreq);
		ep->nreq = q->nreq;
		str_copy(ep->req_text, sizeof(ep->req_text), q->req_text);
	} else {
		ep->nreq = 0;
		ep->req_text[0] = '\0';
	}

	if (q->has_d) {
		ep->dp = q->dp;
		str_copy(ep->dmap, sizeof(ep->dmap), q->dmap);
	}

	dial_reset(ep);
	timer_plain_arm(ep);
	/* the detector first: a digit dialed into the dial tone must count */
	detect_arm(ep);
	sig_apply(ep, q);
}

/*
 * The observed events that no NTFY reported go in front of the quarantined
 * ones, which came later (RFC 3435 4.4.1). A full buffer keeps the
 * oldest events and discards the newest.
 */
static void obs_requeue(struct mgcp_ep *ep)
{
	unsigned int n = ep->nobs, keep = ep->nquar, lost;

	if (n > MGCP_QUAR_MAX)
		n = MGCP_QUAR_MAX;
	if (keep > MGCP_QUAR_MAX - n)
		keep = MGCP_QUAR_MAX - n;
	lost = (ep->nobs - n) + (ep->nquar - keep);

	memmove(&ep->quar[n], &ep->quar[0], keep * sizeof(ep->quar[0]));
	memcpy(&ep->quar[0], &ep->obs[0], n * sizeof(ep->obs[0]));
	ep->nquar = n + keep;

	if (!lost)
		return;
	ep->quar_dropped += lost;
	ep->gw->stat.quar_dropped += lost;
	EP_TRACE(ep->gw, "line %u quarantine full, %u events discarded",
		 ep->index, lost);
}

/*
 * Commit a request (RFC 3435 2.3.3, 4.4.1 pages 129 to 132). Each parameter
 * acts on its own. The quarantined events run after the response, in
 * ep_post().
 */
static void rq_apply(struct mgcp_ep *ep, const struct rq *q)
{
	if (q->has_n)
		str_copy(ep->entity, sizeof(ep->entity), q->ne);
	if (q->has_b)
		str_copy(ep->bearer, sizeof(ep->bearer), q->bearer);
	if (!q->has_x)
		return;

	str_copy(ep->rid, sizeof(ep->rid), q->rid);
	str_copy(ep->req_ne, sizeof(ep->req_ne), q->has_n ? q->ne : "");
	ep->q_loop = q->q_loop;
	ep->q_discard = q->q_discard;
	if (q->has_t) {
		memcpy(ep->detect, q->detect,
		       sizeof(q->detect[0]) * q->ndetect);
		ep->ndetect = q->ndetect;
		ep->detect_set = true;
		str_copy(ep->detect_text, sizeof(ep->detect_text),
			 q->detect_text);
	}

	/* a NTFY still out keeps the endpoint in the notification state
	 * (step f); one that was lost no longer counts as pending, and its
	 * events left with it */
	if (ep->notif && !ep->ntfy_txid) {
		ep->notif = false;
		ep->nobs = 0;
	}

	if (!ep->notif && !ep->q_discard && ep->nobs)
		obs_requeue(ep);
	if (ep->q_discard)
		ep->nquar = 0;
	ep->nobs = 0;
	ep->notified = false;

	rq_events_apply(ep, q);
}

/*
 * An embedded request runs as a new request with the same N, X, Q and T. The
 * dial string clears; the observed events and the quarantine buffer stay
 * (RFC 3435 2.3.3 page 40).
 */
static void embedded_run(struct mgcp_ep *ep, const struct mgcp_req_ent *ent)
{
	struct mgcp_gw *gw = ep->gw;
	struct mgcp_embedded em;
	static struct rq q;
	char text[MGCP_EMBED_MAX];
	uint32_t code = 0;

	str_copy(text, sizeof(text), ent->embedded);
	memset(&q, 0, sizeof(q));
	q.q_loop = ep->q_loop;

	if (!mgcp_embedded_parse(sip_str_cstr(text), &em))
		return;
	if (em.has_r)
		code = rq_req_take(gw, ep, em.r, &q, true);
	if (!code && em.has_d)
		code = rq_map_check(gw, em.d, &q);
	if (!code && em.has_s)
		code = rq_sig_take(gw, ep, em.s, &q, false);
	if (code) {
		EP_TRACE(gw, "line %u embedded request refused (%u)",
			 ep->index, code);
		return;
	}

	rq_events_apply(ep, &q);
	if (!br_submit(ep, &q))
		EP_TRACE(gw, "line %u embedded request runs without its digits",
			 ep->index);
}

/* ------------------------------------------------------- the connections */

static enum sdp_dir mode_dir(uint8_t mode)
{
	switch (mode) {
	case MGCP_MODE_SENDONLY:
		return SDP_SENDONLY;
	case MGCP_MODE_RECVONLY:
		return SDP_RECVONLY;
	case MGCP_MODE_INACTIVE:
		return SDP_INACTIVE;
	default:
		return SDP_SENDRECV;
	}
}

/* RFC 3435 2.3.5 page 49: the modes that need a remote description. */
static bool mode_needs_remote(uint8_t mode)
{
	return mode == MGCP_MODE_SENDRECV || mode == MGCP_MODE_SENDONLY ||
	       mode == MGCP_MODE_CONFRNCE || mode == MGCP_MODE_NETWLOOP ||
	       mode == MGCP_MODE_NETWTEST;
}

/*
 * 517 before 527: 527 answers a request that needs a remote description to
 * be supported (RFC 3661 page 16), and no description makes an unsupported
 * mode supported (517, page 13).
 */
static uint32_t mode_check(const struct mgcp_gw *gw, enum mgcp_mode mode)
{
	switch (mode) {
	case MGCP_MODE_SENDONLY:
	case MGCP_MODE_RECVONLY:
	case MGCP_MODE_SENDRECV:
	case MGCP_MODE_INACTIVE:
		return 0;
	case MGCP_MODE_CONFRNCE:
		return (gw->host->hal_features(gw->host->ctx) &
			HAL_FEAT_CONF) ? 0 : 517;
	case MGCP_MODE_EXTENSION:
		return 518;
	default:
		return 517;
	}
}

/* The L: of one command: text is its value, opt holds the fields it names,
 * and the others are unset as struct mgcp_lopt states. */
struct lco {
	bool has;
	struct sip_str text;
	struct mgcp_lco v;
	struct mgcp_lopt opt;
};

static void lopt_values_clear(struct mgcp_lopt *o)
{
	o->a[0] = '\0';
	o->ptime = 0;
	o->vad = -1;
	o->ec = -1;
	o->tos = -1;
}

static void lopt_init(struct mgcp_lopt *o)
{
	o->sup[0] = '\0';
	lopt_values_clear(o);
}

/* One "name:value" item of an L: list. */
static void lco_item_split(struct sip_str item, struct sip_str *name,
			   struct sip_str *value)
{
	const char *colon = memchr(item.p, ':', item.len);
	size_t at = colon ? (size_t)(colon - item.p) : item.len;

	*name = sip_str_trim(sip_str_init(item.p, at));
	*value = colon ? sip_str_trim(sip_str_init(colon + 1,
						   item.len - at - 1)) :
			 sip_str_init(item.p + item.len, 0);
}

/* The value of the last item of list named name. */
static bool lco_item_find(struct sip_str list, struct sip_str name,
			  struct sip_str *value)
{
	struct sip_str rest = list, item, n, v;
	bool found = false;

	while (mgcp_list_next(&rest, &item)) {
		lco_item_split(item, &n, &v);
		if (n.len != name.len || strncasecmp(n.p, name.p, n.len))
			continue;
		*value = v;
		found = true;
	}

	return found;
}

/*
 * The supplied options of sup with add merged in: a name that add carries
 * takes its value there, a new name joins at the end, and the name drop
 * leaves (an empty span drops none). false when the result does not fit.
 */
static bool lopt_sup_merge(char *sup, size_t len, struct sip_str add,
			   struct sip_str drop)
{
	char out[MGCP_TEXT_MAX];
	struct sip_str rest = sip_str_cstr(sup), item, n, v, nv;
	struct sip_buf b;

	sip_buf_init(&b, out, sizeof(out));
	while (mgcp_list_next(&rest, &item)) {
		lco_item_split(item, &n, &v);
		if (drop.len && n.len == drop.len &&
		    !strncasecmp(n.p, drop.p, n.len))
			continue;
		if (lco_item_find(add, n, &nv))
			v = nv;
		sip_buf_addf(&b, "%s%.*s:%.*s", sip_buf_len(&b) ? ", " : "",
			     (int)n.len, n.p, (int)v.len, v.p);
	}

	/* out holds every name kept so far, so a name of add joins once */
	rest = add;
	while (mgcp_list_next(&rest, &item)) {
		struct sip_str kept = sip_str_init(sip_buf_str(&b),
						   sip_buf_len(&b));

		lco_item_split(item, &n, &v);
		if (lco_item_find(kept, n, &nv))
			continue;
		if (lco_item_find(add, n, &nv))
			v = nv;
		sip_buf_addf(&b, "%s%.*s:%.*s", sip_buf_len(&b) ? ", " : "",
			     (int)n.len, n.p, (int)v.len, v.p);
	}

	if (!sip_buf_ok(&b) || sip_buf_len(&b) >= len)
		return false;
	memcpy(sup, out, sip_buf_len(&b) + 1);

	return true;
}

static uint32_t lco_values(const struct mgcp_ep *ep, const struct mgcp_lco *v,
			   struct mgcp_lopt *o);

/* The fields of o from its supplied options. */
static void lopt_derive(const struct mgcp_ep *ep, struct mgcp_lopt *o)
{
	struct mgcp_lco v;

	lopt_values_clear(o);
	if (mgcp_lco_parse(sip_str_cstr(o->sup), &v))
		lco_values(ep, &v, o);
}

/*
 * RFC 3435 2.3.6 page 53: an L: changes the options it names. drop names an
 * option that leaves. false when the options in force do not fit.
 */
static bool lopt_merge(const struct mgcp_ep *ep, struct mgcp_lopt *dst,
		       const struct lco *l, const char *drop)
{
	if (!lopt_sup_merge(dst->sup, sizeof(dst->sup),
			    l->has ? l->text : sip_str_init("", 0),
			    sip_str_cstr(drop ? drop : "")))
		return false;

	lopt_derive(ep, dst);

	return true;
}

/* The options in force as the L: of an audit (RFC 3435 2.3.11 page 66). */
static const char *lopt_text(struct mgcp_gw *gw, const struct mgcp_lopt *o)
{
	return arena_printf(gw, "%s", o->sup);
}

/* t: is the IPv4 type of service byte, 1*2 HEXDIG (RFC 3435 3.2.2.10). */
static bool tos_take(struct sip_str v, int16_t *out)
{
	char text[3];

	if (v.len > 2 || !mgcp_hex_id_valid(v))
		return false;

	memcpy(text, v.p, v.len);
	text[v.len] = '\0';
	*out = (int16_t)strtoul(text, NULL, 16);

	return true;
}

static bool on_off(struct sip_str v, bool *on)
{
	if (sip_str_eq_ci(v, "on")) {
		*on = true;
		return true;
	}
	if (sip_str_eq_ci(v, "off")) {
		*on = false;
		return true;
	}

	return false;
}

/*
 * p: names one packet time or a range (RFC 3435 3.2.2.10). The packet time
 * of the channel wins when the range holds it, else the shortest one the SDP
 * layer carries.
 */
static uint32_t ptime_take(const struct mgcp_gw *gw, struct sip_str v,
			   uint8_t channel, uint8_t *out)
{
	static const uint8_t carried[] = { 10, 20, 30 };
	uint32_t lo, hi;
	size_t dash, i;

	for (dash = 0; dash < v.len && v.p[dash] != '-'; dash++)
		;
	if (!sip_str_uint(sip_str_trim(sip_str_init(v.p, dash)), &lo))
		return 532;
	hi = lo;
	if (dash < v.len &&
	    !sip_str_uint(sip_str_trim(sip_str_init(v.p + dash + 1,
						    v.len - dash - 1)), &hi))
		return 532;
	if (hi < lo)
		return 532;

	if (channel >= lo && channel <= hi) {
		for (i = 0; i < ARRAY_LEN(carried); i++) {
			if (carried[i] == channel) {
				*out = channel;
				return 0;
			}
		}
	}

	for (i = 0; i < ARRAY_LEN(carried); i++) {
		if (carried[i] >= lo && carried[i] <= hi) {
			*out = carried[i];
			return 0;
		}
	}

	return err_code(gw, E_PTIME);
}

/*
 * The line echo canceller is a channel setting of the core, which the host
 * table does not switch per connection: e: must name the setting in effect.
 */
static bool lec_take(const struct cfg_channel *ch, struct sip_str v,
		     int8_t *out)
{
	bool on, ec, nlp;

	if (!on_off(v, &on))
		return false;
	if (ch) {
		cfg_channel_lec_effective(ch, &ec, &nlp);
		if (on != ec)
			return false;
	}
	*out = on;

	return true;
}

/*
 * t: as the type of service byte. NCS 7.3.3 page 37 keeps the two ECN bits
 * at zero: a value other than 0 must be a multiple of 4.
 */
static bool tos_check(const struct mgcp_gw *gw, struct sip_str v,
		      int16_t *out)
{
	if (!tos_take(v, out))
		return false;

	return gw->cfg.dialect != CFG_MGCP_NCS || !(*out & 3);
}

/*
 * The fields of o from the options of v, each on its own, and the code of
 * the first option in the order below that this gateway refuses: 510 for an
 * a: that does not fit, the packet time code of the dialect for a packet
 * time outside the carried set, 532 for a value it does not carry.
 */
static uint32_t lco_values(const struct mgcp_ep *ep, const struct mgcp_lco *v,
			   struct mgcp_lopt *o)
{
	const struct cfg_channel *ch = channel_of(ep);
	uint8_t channel_ptime = ch ? (uint8_t)ch->ptime_ms : 20;
	const struct mgcp_gw *gw = ep->gw;
	uint32_t code[9] = { 0 };
	unsigned int i;
	bool on;

	if (v->a.len && !sip_str_copy(o->a, sizeof(o->a), v->a))
		code[0] = 510;
	if (v->p.len)
		code[1] = ptime_take(gw, v->p, channel_ptime, &o->ptime);
	if (v->e.len && !lec_take(ch, v->e, &o->ec))
		code[2] = 532;
	if (v->s.len && on_off(v->s, &on))
		o->vad = on;
	else if (v->s.len)
		code[3] = 532;
	if (v->t.len && !tos_check(gw, v->t, &o->tos)) {
		o->tos = -1;
		code[4] = 532;
	}
	if (v->gc.len && !sip_str_eq_ci(v->gc, "auto") &&
	    !sip_str_eq(v->gc, "0"))
		code[5] = 532;
	if (v->r.len && !sip_str_eq_ci(v->r, "be"))
		code[6] = 532;
	if (v->k.len)
		code[7] = 532;
	if (v->nt.len && !sip_str_eq_ci(v->nt, "IN"))
		code[8] = 532;

	for (i = 0; i < ARRAY_LEN(code); i++) {
		if (code[i])
			return code[i];
	}

	return 0;
}

/*
 * The LocalConnectionOptions of a command (RFC 3435 3.2.2.10). An unknown
 * mandatory extension gets 525, the other refusals are those of
 * lco_values().
 */
static uint32_t lco_take(const struct mgcp_ep *ep, const struct mgcp_msg *msg,
			 struct lco *l)
{
	memset(l, 0, sizeof(*l));
	lopt_init(&l->opt);
	if (!mgcp_msg_value(msg, MGCP_PARAM_L, &l->text))
		return 0;

	l->has = true;
	if (!mgcp_lco_parse(l->text, &l->v))
		return 510;
	if (l->v.ext_mandatory.len)
		return 525;

	return lco_values(ep, &l->v, &l->opt);
}

/*
 * The approved list (RFC 3435 2.6 page 76): the codecs of the line that
 * a: names, in the order of a:. image/t38 in a: keeps T.38; an a: without
 * it drops T.38.
 */
static void codec_filter(struct sdp_local *loc, struct sip_str a)
{
	struct sdp_codec keep[SDP_CODEC_MAX];
	struct sip_str rest = a, name;
	unsigned int n = 0, i;
	bool t38 = false;

	while (mgcp_codec_next(&rest, &name)) {
		if (sip_str_eq_ci(name, "image/t38")) {
			t38 = true;
			continue;
		}
		for (i = 0; i < loc->codecs && n < SDP_CODEC_MAX; i++) {
			const char *cn = sdp_codec_name(
				(enum sdp_codec_id)loc->codec[i].id);
			unsigned int j;
			bool dup = false;

			if (!sip_str_eq_ci(name, cn))
				continue;
			for (j = 0; j < n; j++)
				dup = dup || keep[j].id == loc->codec[i].id;
			if (!dup)
				keep[n++] = loc->codec[i];
		}
	}

	memcpy(loc->codec, keep, n * sizeof(keep[0]));
	loc->codecs = n;
	loc->t38 = loc->t38 && t38;
}

/* A p: of the call agent wins over the packet time of an offer. */
static void lopt_apply(struct sdp_local *loc, const struct mgcp_lopt *o,
		       bool use_a)
{
	unsigned int i;

	if (use_a && o->a[0])
		codec_filter(loc, sip_str_cstr(o->a));
	if (o->ptime) {
		loc->ptime_ms = o->ptime;
		loc->ptime_pin = true;
	}
	for (i = 0; o->vad >= 0 && i < loc->codecs; i++)
		loc->codec[i].vad = o->vad;
}

/*
 * The local view of a connection: the codecs of the line, the T.38 and
 * Annex B gates of the backend, then the LocalConnectionOptions in force.
 * use_a false leaves the codecs of the line as they are.
 */
static void conn_local_build(struct mgcp_ep *ep, struct mgcp_conn *c,
			     bool use_a)
{
	const struct proto_host *h = ep->gw->host;
	const struct cfg *cfg = h->cfg(h->ctx);
	const struct cfg_channel *ch = channel_of(ep);
	uint32_t feat = h->hal_features(h->ctx);

	media_sdp_local_build(cfg, ch, h->hal_codecs(h->ctx),
			      ep->gw->cfg.media_addr, c->port, &c->loc);
	c->loc.t38 = ch && ch->fax_t38_enable && (feat & HAL_FEAT_T38);
	c->loc.g729b = (feat & HAL_FEAT_G729B) != 0;
	c->loc.ptime_omit = ep->gw->cfg.ptime_omit;
	lopt_apply(&c->loc, &c->lopt, use_a);
}

static uint64_t sess_id_new(struct mgcp_gw *gw)
{
	uint64_t id = 0;

	if (!gw->host->random_bytes(gw->host->ctx, &id, sizeof(id)))
		id = ((uint64_t)mgcp_txn_rand(gw->mgr) << 32) |
		     mgcp_txn_rand(gw->mgr);

	return (id & 0x7fffffffffffffffull) | 1;
}

/* A random id, the next value on a clash with the other connection. */
static void conn_id_new(struct mgcp_ep *ep, char *out)
{
	uint32_t v = rand32(ep->gw);

	for (;; v++) {
		if (!v)
			continue;
		snprintf(out, MGCP_ID_MAX, "%08X", v);
		if (!conn_find_id(ep, sip_str_cstr(out)))
			return;
	}
}

/* Copy the descriptor built in body into the connection's record. */
static void lcd_keep(struct mgcp_conn *c, const struct sip_buf *b)
{
	size_t len = sip_buf_len(b);

	if (len >= sizeof(c->lcd))
		len = sizeof(c->lcd) - 1;
	memcpy(c->lcd, sip_buf_str(b), len);
	c->lcd[len] = '\0';
	c->lcd_len = len;
}

/* The code of a refused remote description. */
static uint32_t sdp_refusal(const struct mgcp_gw *gw, enum sdp_verdict v,
			    const struct sdp_msg *offer)
{
	if (v == SDP_BAD)
		return err_code(gw, E_RCD);

	/* an image section alone that the gates refuse is a description
	 * this gateway does not support */
	if (offer && offer->image.present &&
	    (!offer->audio.present || !offer->audio.port))
		return 505;

	return 534;
}

/*
 * A remote description on a connection: the answer to the offer of this
 * side, or a new offer this side answers. The descriptor of an answer goes
 * to body.
 */
static uint32_t conn_remote_take(struct mgcp_ep *ep, struct mgcp_conn *c,
				 struct sip_str rcd, struct sip_buf *body,
				 bool *lcd_new)
{
	struct mgcp_gw *gw = ep->gw;
	struct sdp_msg msg;
	enum sdp_verdict v;

	*lcd_new = false;
	if (rcd.len >= sizeof(c->rcd))
		return 505;
	if (!sdp_parse(rcd, &msg))
		return err_code(gw, E_RCD);

	if (c->sdp.offer_pending) {
		v = sdp_answer_take(&c->sdp, &c->loc, &msg);
		if (v != SDP_OK)
			return sdp_refusal(gw, v, &msg);
		sdp_commit(&c->sdp);
	} else {
		v = sdp_answer_build(&c->sdp, &c->loc, &msg, body);
		if (v != SDP_OK)
			return sdp_refusal(gw, v, &msg);
		sdp_commit(&c->sdp);
		*lcd_new = true;
	}

	memcpy(c->rcd, rcd.p, rcd.len);
	c->rcd[rcd.len] = '\0';
	c->rcd_len = rcd.len;
	c->remote = true;

	return 0;
}

/*
 * The mode of the connection within what the peer allows: a zero connection
 * address holds the peer to sendonly, so this side can only receive.
 */
static enum sdp_dir conn_dir(const struct mgcp_conn *c,
			     const struct sdp_result *res)
{
	enum sdp_dir d = mode_dir(c->mode);

	if (!res->remote_hold)
		return d;

	return d == SDP_SENDRECV || d == SDP_RECVONLY ? SDP_RECVONLY :
							SDP_INACTIVE;
}

/*
 * Program the stream of a connection from its committed exchange, in the
 * mode of the connection. No stream starts before a remote tuple exists, and
 * inactive configures without a start. A T.38 result keeps the sendrecv of
 * the SDP layer (shared/sdp.h) unless the mode is inactive. *touched says
 * whether the stream changed on the host, so that a failure knows whether
 * the stream needs conn_restore().
 */
static enum hal_status conn_program(struct mgcp_ep *ep, struct mgcp_conn *c,
				    bool *touched)
{
	struct mgcp_gw *gw = ep->gw;
	const struct proto_host *h = gw->host;
	const struct cfg *cfg = h->cfg(h->ctx);
	const struct cfg_channel *ch = channel_of(ep);
	struct sdp_result res = c->sdp.committed;
	struct hal_media m;
	enum hal_status st;

	*touched = false;
	if (!res.valid || !ch || !cfg)
		return HAL_OK;

	if (!res.t38 || c->mode == MGCP_MODE_INACTIVE)
		res.dir = (uint8_t)conn_dir(c, &res);

	media_fill(ch, cfg->rtcp_enabled, cfg->rtcp_interval,
		   (h->hal_features(h->ctx) & HAL_FEAT_RTCP_ADDR) != 0, &res,
		   c->loc.addr, c->loc.port, gw->cfg.bind_dev, c->fax_kind,
		   h->net_ct_punch_interval_ms(h->ctx), &m);
	if (!m.remote.port)
		return HAL_FATAL;
	/* NCS 7.3.3 footnote 16: t: is for the RTP packets, not RTCP */
	if (c->lopt.tos >= 0)
		m.rtp_dscp = (uint8_t)(c->lopt.tos >> 2);

	/*
	 * A move between audio and T.38 stops the stream first. The host
	 * halts the stream whatever the stop answers (line/line.c media_stop(),
	 * the udsp stream_halt()), so the status is advisory and the program
	 * goes on from a stopped stream.
	 */
	if (c->media_started && c->media_set &&
	    (c->media.codec == HAL_CODEC_T38) != (m.codec == HAL_CODEC_T38)) {
		*touched = true;
		st = h->media.stop(h->media.priv, ep->index, c->stream);
		if (st != HAL_OK)
			EP_TRACE(gw, "line %u stream %u stop answered %d",
				 ep->index, c->stream, (int)st);
		c->media_started = false;
	}

	if (!c->media_set || memcmp(&m, &c->media, sizeof(m))) {
		*touched = true;
		st = h->media.config_set(h->media.priv, ep->index, c->stream,
					 &m);
		if (st != HAL_OK)
			return st;
	}
	c->media = m;
	c->media_set = true;

	if (!c->media_started && c->mode != MGCP_MODE_INACTIVE) {
		*touched = true;
		st = h->media.start(h->media.priv, ep->index, c->stream);
		if (st != HAL_OK)
			return st;
		c->media_started = true;
	}

	return HAL_OK;
}

/*
 * conn_program() changed the stream of old, the live connection, on s, its
 * scratch copy, and the command then failed. The stream takes the
 * configuration and the run state of old again; a stream that old does not
 * hold stops. When that fails, the record of old takes the run state the
 * host has, so the stream and the record agree when the error response goes
 * out.
 */
static void conn_restore(struct mgcp_ep *ep, struct mgcp_conn *old,
			 const struct mgcp_conn *s)
{
	const struct proto_host *h = ep->gw->host;
	bool running = s->media_started;
	enum hal_status st;

	if (!old->media_set) {
		if (s->media_set || running)
			h->media.stop(h->media.priv, ep->index, s->stream);
		return;
	}

	/* a move between audio and T.38 needs a stopped stream */
	if (running && (s->media.codec == HAL_CODEC_T38) !=
		       (old->media.codec == HAL_CODEC_T38)) {
		h->media.stop(h->media.priv, ep->index, old->stream);
		running = false;
	}

	st = h->media.config_set(h->media.priv, ep->index, old->stream,
				 &old->media);
	if (st == HAL_OK && old->media_started && !running) {
		st = h->media.start(h->media.priv, ep->index, old->stream);
		running = st == HAL_OK;
	} else if (st == HAL_OK && !old->media_started && running) {
		h->media.stop(h->media.priv, ep->index, old->stream);
		running = false;
	}

	old->media_started = running;
	if (st != HAL_OK)
		EP_TRACE(ep->gw, "line %u stream %u not restored (%d)",
			 ep->index, old->stream, (int)st);
}

/* RFC 3435 3.2.2.7 from the counters of the stream; false when the
 * backend has none (3.3 page 102 makes P: optional). */
static bool conn_stats(struct mgcp_ep *ep, const struct mgcp_conn *c,
		       struct mgcp_conn_params *cp)
{
	const struct proto_host *h = ep->gw->host;
	struct hal_media_stats s;

	memset(cp, 0, sizeof(*cp));
	if (h->stream_stats_get(h->ctx, ep->index, c->stream, &s) != HAL_OK)
		return false;

	cp->ps = (uint32_t)(s.packets_sent > UINT32_MAX ? UINT32_MAX :
			    s.packets_sent);
	cp->os = (uint32_t)(s.bytes_sent > UINT32_MAX ? UINT32_MAX :
			    s.bytes_sent);
	cp->pr = (uint32_t)(s.packets_recv > UINT32_MAX ? UINT32_MAX :
			    s.packets_recv);
	cp->or_ = (uint32_t)(s.bytes_recv > UINT32_MAX ? UINT32_MAX :
			     s.bytes_recv);
	cp->pl = (uint32_t)(s.packets_lost > UINT32_MAX ? UINT32_MAX :
			    s.packets_lost);
	cp->ji = (uint32_t)(s.jitter_avg > UINT32_MAX ? UINT32_MAX :
			    s.jitter_avg);
	/* 2.3.7 page 57: half the round trip without a shared clock */
	cp->la = (uint32_t)(s.rtt_avg / 2 > UINT32_MAX ? UINT32_MAX :
			    s.rtt_avg / 2);
	cp->present = MGCP_CP_PS | MGCP_CP_OS | MGCP_CP_PR | MGCP_CP_OR |
		      MGCP_CP_PL | MGCP_CP_JI | MGCP_CP_LA;

	return true;
}

/* The local mixer follows the two modes: on when both connections are in
 * confrnce, off when either leaves it. */
static void conf_settle(struct mgcp_ep *ep)
{
	const struct proto_host *h = ep->gw->host;
	bool both = ep->conn[0].used && ep->conn[1].used &&
		    ep->conn[0].mode == MGCP_MODE_CONFRNCE &&
		    ep->conn[1].mode == MGCP_MODE_CONFRNCE;

	if (both == ep->conf_on)
		return;

	h->conf_set(h->ctx, ep->index, both);
	ep->conf_on = both;
}

/* A configured stream holds the sockets and codec instances its config_set
 * made, started or not, so the stop releases them (hal.h stream_stop). */
static void conn_release(struct mgcp_ep *ep, struct mgcp_conn *c)
{
	const struct proto_host *h = ep->gw->host;

	if (c->media_set || c->media_started)
		h->media.stop(h->media.priv, ep->index, c->stream);
	if (c->port)
		h->net_port_free(h->ctx, c->port);
	if (c->sdp.offer_pending)
		sdp_rollback(&c->sdp);

	memset(c, 0, sizeof(*c));
}

/* ------------------------------------------------------- the call records */

static struct mgcp_rec *rec_find(struct mgcp_ep *ep, const char *call_id)
{
	unsigned int i;

	for (i = 0; i < MGCP_CONN_MAX; i++) {
		if (ep->rec[i].used && !strcasecmp(ep->rec[i].call_id, call_id))
			return &ep->rec[i];
	}

	return NULL;
}

/* The record opens on its stream and tells the call trace, as a SIP leg
 * does at its start. */
static void rec_start(struct mgcp_ep *ep, const struct mgcp_rec *rec)
{
	const struct proto_host *h = ep->gw->host;
	uint32_t seq;

	h->cdr_call_started(h->ctx, ep->index, rec->stream, rec->inbound);
	seq = h->cdr_call_seq(h->ctx, ep->index, rec->stream);
	h->calltrace_call_started(h->ctx, ep->index, seq);
	h->calltrace_event(h->ctx, ep->index, "leg_started");
}

/* A connection joined the endpoint: the first one of its call opens the
 * record. */
static void rec_open(struct mgcp_ep *ep, const struct mgcp_conn *c)
{
	struct mgcp_rec *rec;
	unsigned int i;

	if (rec_find(ep, c->call_id))
		return;
	for (i = 0; i < MGCP_CONN_MAX && ep->rec[i].used; i++)
		;
	if (i == MGCP_CONN_MAX)
		return;

	rec = &ep->rec[i];
	memset(rec, 0, sizeof(*rec));
	rec->used = true;
	rec->inbound = !ep->offhook;
	rec->stream = c->stream;
	str_copy(rec->call_id, sizeof(rec->call_id), c->call_id);
	rec_start(ep, rec);
}

/* A committed connection: a stream that runs marks the record, and the
 * first sendrecv answers it. */
static void rec_note(struct mgcp_ep *ep, const struct mgcp_conn *c)
{
	const struct proto_host *h = ep->gw->host;
	struct mgcp_rec *rec = rec_find(ep, c->call_id);

	if (!rec)
		return;
	if (c->media_started)
		rec->media_ran = true;
	if (rec->answered || c->mode != MGCP_MODE_SENDRECV)
		return;

	rec->answered = true;
	rec->answered_ms = tenv(ep->gw)->now_ms(tenv(ep->gw));
	h->cdr_call_answered(h->ctx, ep->index, rec->stream);
}

/* Another connection of the call of c, or NULL. */
static const struct mgcp_conn *call_other_conn(const struct mgcp_ep *ep,
					       const struct mgcp_conn *c)
{
	unsigned int i;

	for (i = 0; i < MGCP_CONN_MAX; i++) {
		const struct mgcp_conn *o = &ep->conn[i];

		if (o != c && o->used && !strcasecmp(o->call_id, c->call_id))
			return o;
	}

	return NULL;
}

/* The connection holds the open record of its call: it is on its stream. */
static bool rec_holds(struct mgcp_ep *ep, const struct mgcp_conn *c)
{
	const struct mgcp_rec *rec;

	if (!c->used)
		return false;
	rec = rec_find(ep, c->call_id);

	return rec && rec->stream == c->stream;
}

/*
 * The record ends with what c carried. The media numbers go first, while
 * the stream runs and the record is open (proto_media.h stats_snapshot).
 * moved says the call goes on in the record that names this one.
 */
static void rec_end(struct mgcp_ep *ep, const struct mgcp_rec *rec,
		    const struct mgcp_conn *c, bool moved)
{
	const struct proto_host *h = ep->gw->host;
	const struct sdp_result *res = &c->sdp.committed;
	struct proto_call_ended info;
	uint64_t now;

	if (c->media_started)
		h->media.stats_snapshot(h->media.priv, ep->index, c->stream);

	now = tenv(ep->gw)->now_ms(tenv(ep->gw));
	memset(&info, 0, sizeof(info));
	info.inbound = rec->inbound;
	info.answered = rec->answered;
	info.media_ran = rec->media_ran || c->media_started;
	info.number = "";
	info.remote_number = "";
	info.remote_name = "";
	info.t38 = res->valid && res->t38;
	info.codec = res->valid && !res->t38 ?
		     (uint8_t)media_codec_of((enum sdp_codec_id)res->codec.id) :
		     HAL_CODEC_NONE;
	info.ptime_ms = info.codec != HAL_CODEC_NONE ? res->ptime_ms : 0;
	info.rtp_port = c->port;
	info.transferred = moved;
	if (rec->answered)
		info.answered_ago_ms = (uint32_t)(now - rec->answered_ms);

	h->cdr_call_ended(h->ctx, ep->index, rec->stream, &info);
	h->calltrace_event(h->ctx, ep->index, "leg_ended");
}

/*
 * The core keys an open record by its stream (cdr.h), so the record of a
 * call follows a connection that stays: the connection on its stream goes,
 * a new record opens on the stream of o and names the old one
 * (host->cdr_call_correlate()), and the old one ends. The new record takes
 * the answer the call already had and the later edges.
 */
static void rec_move(struct mgcp_ep *ep, struct mgcp_rec *rec,
		     const struct mgcp_conn *c, const struct mgcp_conn *o)
{
	const struct proto_host *h = ep->gw->host;
	struct mgcp_rec old = *rec;

	rec->stream = o->stream;
	rec->media_ran = o->media_started;
	rec_start(ep, rec);
	h->cdr_call_correlate(h->ctx, ep->index, rec->stream, old.stream);
	if (rec->answered)
		h->cdr_call_answered(h->ctx, ep->index, rec->stream);
	rec_end(ep, &old, c, true);
}

/* The connection c goes. The last one of its call closes the record. */
static void rec_close(struct mgcp_ep *ep, const struct mgcp_conn *c)
{
	struct mgcp_rec *rec = rec_find(ep, c->call_id);
	const struct mgcp_conn *o = call_other_conn(ep, c);

	if (!rec)
		return;
	if (o) {
		if (rec->stream == c->stream)
			rec_move(ep, rec, c, o);
		return;
	}

	rec_end(ep, rec, c, false);
	memset(rec, 0, sizeof(*rec));
}

/* The slot order of a delete of several connections: a connection that
 * holds a record goes last, so the record closes once, on its own stream. */
static void conn_order(struct mgcp_ep *ep, unsigned int *order)
{
	unsigned int i, n = 0;

	for (i = 0; i < MGCP_CONN_MAX; i++) {
		if (!rec_holds(ep, &ep->conn[i]))
			order[n++] = i;
	}
	for (i = 0; i < MGCP_CONN_MAX; i++) {
		if (rec_holds(ep, &ep->conn[i]))
			order[n++] = i;
	}
}

/* A connection leaves the endpoint: its record first, then its stream. */
static void conn_drop(struct mgcp_ep *ep, struct mgcp_conn *c)
{
	rec_close(ep, c);
	conn_release(ep, c);
}

static void conn_delete(struct mgcp_ep *ep, struct mgcp_conn *c)
{
	EP_TRACE(ep->gw, "line %u connection %s deleted", ep->index, c->id);
	conn_drop(ep, c);
	conf_settle(ep);
}

/* ------------------------------------------------------------ responses */

struct cmd {
	const struct mgcp_msg *msg;
	const struct sockaddr *src;
	socklen_t src_len;
	struct mgcp_ep *ep;		/* the one endpoint the command names */
	uint32_t mask;			/* every endpoint it names */
	bool all;			/* the "all of" wildcard */
	bool any;			/* the "any of" wildcard */
	bool audit;
	uint32_t front;			/* an RSIP that rides in front */
};

struct reply {
	struct mgcp_param_kv kv[24];
	unsigned int nkv;
	const char *sdp;
	size_t sdp_len;
	const char *tail;		/* lines after the parameters */
};

static void reply_kv(struct reply *r, enum mgcp_param code,
		     const char *value)
{
	if (r->nkv >= ARRAY_LEN(r->kv))
		return;

	r->kv[r->nkv].code = code;
	r->kv[r->nkv].name = NULL;
	r->kv[r->nkv].value = value ? value : "";
	r->nkv++;
}

static const char *code_comment(uint32_t code)
{
	if (code == 250)
		return "Connection deleted";
	if (code >= 200 && code < 300)
		return "OK";

	return "Error";
}

/*
 * One response into the wire buffer. An RSIP of the restart or the
 * disconnected procedure goes in front of it in the same datagram when the
 * command needs it (RFC 3435 4.4.6 page 141, 4.4.7 page 143).
 */
static bool reply_build(struct mgcp_gw *gw, const struct cmd *c,
			uint32_t code, const struct reply *r, struct sip_buf *b)
{
	struct mgcp_txn *front = NULL;
	struct mgcp_resp resp;

	sip_buf_init(b, gw->wire, sizeof(gw->wire));
	if (c->front)
		front = mgcp_txn_get(gw->mgr, c->front);
	if (front) {
		sip_buf_add_len(b, front->wire, front->len);
		mgcp_sep_write(b);
	}

	memset(&resp, 0, sizeof(resp));
	resp.code = code;
	resp.txid = c->msg->txid;
	resp.comment = code_comment(code);
	if (r) {
		resp.params = r->kv;
		resp.param_count = r->nkv;
		resp.sdp = r->sdp;
		resp.sdp_len = r->sdp_len;
	}

	if (!mgcp_resp_build(b, &resp))
		return false;
	if (r && r->tail)
		sip_buf_add(b, r->tail);

	return sip_buf_ok(b);
}

/* One response, toward the source of the command. A response that does
 * not fit the datagram becomes 533 (RFC 3435 2.4). */
static void reply_send(struct mgcp_gw *gw, const struct cmd *c,
		       uint32_t code, const struct reply *r)
{
	struct sip_buf b;

	if (!reply_build(gw, c, code, r, &b)) {
		EP_TRACE(gw, "%s %u: response %u does not fit",
			 mgcp_verb_name(c->msg->verb), c->msg->txid, code);
		code = 533;
		if (!reply_build(gw, c, code, NULL, &b)) {
			EP_TRACE(gw, "%s %u: no response sent",
				 mgcp_verb_name(c->msg->verb), c->msg->txid);
			return;
		}
	}

	if (code >= 400)
		gw->stat.errors++;
	mgcp_txn_respond(gw->mgr, c->msg->txid, c->src, c->src_len, code,
			 sip_buf_str(&b), sip_buf_len(&b));
	EP_TRACE(gw, "%s %u answered %u", mgcp_verb_name(c->msg->verb),
		 c->msg->txid, code);
}

static void reply_code(struct mgcp_gw *gw, const struct cmd *c, uint32_t code)
{
	reply_send(gw, c, code, NULL);
}

/* The value of a P: line: the writer of mgcp/msg.h builds the whole line,
 * and the response builder takes the value after "P: ". */
static const char *cp_text(struct mgcp_gw *gw,
			   const struct mgcp_conn_params *cp)
{
	char line[160];
	struct sip_buf b;
	size_t n;

	sip_buf_init(&b, line, sizeof(line));
	if (!mgcp_conn_params_write(&b, cp) || sip_buf_len(&b) < 5)
		return "";

	n = sip_buf_len(&b) - 5;

	return arena_printf(gw, "%.*s", (int)n, line + 3);
}

/* What a command owes after its response: the events held behind it, the
 * quarantine, and the line report to the core. A move of the state reaches
 * the call trace by name, as a move of the SIP machine does. */
static void ep_post(struct mgcp_ep *ep)
{
	const struct proto_host *h = ep->gw->host;
	struct mgcp_obs later[ARRAY_LEN(ep->later)];
	unsigned int n = ep->nlater, i;
	enum ep_state st;

	memcpy(later, ep->later, n * sizeof(later[0]));
	ep->nlater = 0;
	for (i = 0; i < n; i++)
		ep_event(ep, &later[i]);

	quar_run(ep);

	st = ep_state_of(ep);
	if (st != ep->state_last) {
		h->calltrace_state(h->ctx, ep->index,
				   ep_state_words[ep->state_last],
				   ep_state_words[st]);
		ep->state_last = (uint8_t)st;
	}
	h->line_transition(h->ctx, ep->index, st, ep_flags_of(ep));
	h->line_settle(h->ctx, ep->index);
}

/* ----------------------------------------------------- restart, service */

static void restart_send(struct mgcp_gw *gw, uint32_t mask);

static uint32_t lines_mask(const struct mgcp_gw *gw)
{
	return (1u << gw->cfg.lines) - 1;
}

/* The restarting endpoints that the disconnected procedure does not hold. */
static uint32_t restarting_mask(const struct mgcp_gw *gw)
{
	uint32_t mask = 0;
	unsigned int i;

	for (i = 0; i < gw->cfg.lines; i++) {
		if (gw->ep[i].svc == MGCP_EP_RESTARTING && !gw->ep[i].disc)
			mask |= 1u << i;
	}

	return mask;
}

/* The endpoints of mask that share the notified entity of endpoint i. */
static uint32_t entity_group(const struct mgcp_gw *gw, uint32_t mask,
			     unsigned int i)
{
	uint32_t group = 0;
	unsigned int k;

	for (k = 0; k < gw->cfg.lines; k++) {
		if ((mask & (1u << k)) &&
		    !strcmp(gw->ep[k].entity, gw->ep[i].entity))
			group |= 1u << k;
	}

	return group;
}

static unsigned int mask_first(uint32_t mask)
{
	unsigned int i;

	for (i = 0; !(mask & (1u << i)); i++)
		;

	return i;
}

/* End the endpoint's own RSIP that is still out; a new one replaces it. */
static void rsip_abort(struct mgcp_gw *gw, struct mgcp_ep *ep)
{
	uint32_t handle;

	if (!ep->rsip_txid)
		return;

	handle = mgcp_txn_find(gw->mgr, ep->rsip_txid);
	ep->rsip_txid = 0;
	if (handle && mgcp_txn_get(gw->mgr, handle))
		mgcp_txn_abort(gw->mgr, handle);
}

/* RFC 3435 4.4.6 step 1: uniform between 0 and restart_delay_max_ms. */
static uint32_t restart_delay_draw(struct mgcp_gw *gw)
{
	if (!gw->cfg.restart_delay_max_ms)
		return 0;

	return rand32(gw) % (gw->cfg.restart_delay_max_ms + 1);
}

/*
 * An RSIP "restart" that did not go out keeps the procedure armed: the
 * delay, a command or an off-hook sends it again (RFC 3435 4.4.6).
 */
static void restart_retry_arm(struct mgcp_gw *gw)
{
	uint32_t delay = restart_delay_draw(gw);

	if (gw->stopping)
		return;
	if (delay < RESTART_RETRY_MIN_MS)
		delay = RESTART_RETRY_MIN_MS;

	gw->restart_stop = true;
	proto_timer_arm(tenv(gw), &gw->restart_timer, delay);
	EP_TRACE(gw, "restart again in %u ms", delay);
}

static void restart_expire(struct proto_timer *t)
{
	struct mgcp_gw *gw = t->owner;

	gw->restart_wait = false;
	restart_send(gw, restarting_mask(gw));
}

/* The endpoint's own RSIP "restart", toward its own notified entity. */
static bool restart_ep_send(struct mgcp_gw *gw, struct mgcp_ep *ep)
{
	struct rsip_out r = {
		.owner = owner_of(ep),
		.endpoint = ep->name,
		.entity = ep->entity,
		.method = "restart",
	};
	uint32_t txid;

	if (gw->stopping)
		return false;
	rsip_abort(gw, ep);
	if (rsip_send(gw, &r, &txid, NULL) == MGCP_TXN_HANDLE_NONE)
		return false;
	ep->rsip_txid = txid;

	return true;
}

/*
 * RSIP "restart" for the restarting endpoints of mask: one wildcard when
 * they are every endpoint and share one notified entity, else one RSIP per
 * endpoint toward its own entity (RFC 3435 4.4.6 page 141, 4.4.7 page 144).
 */
static void restart_send(struct mgcp_gw *gw, uint32_t mask)
{
	struct rsip_out r = { .owner = MGCP_EP_OWNER_GW, .method = "restart" };
	bool failed = false;
	uint32_t txid;
	unsigned int i;

	if (gw->stopping)
		return;

	/* a retry that waits covers every restarting endpoint */
	if (gw->restart_stop)
		mask |= restarting_mask(gw);
	proto_timer_cancel(tenv(gw), &gw->restart_timer);
	gw->restart_wait = false;
	gw->restart_stop = false;

	/* the wildcard still out names endpoints the new RSIP may not */
	if (gw->restart_handle && mgcp_txn_get(gw->mgr, gw->restart_handle)) {
		mask |= gw->restart_mask;
		mgcp_txn_abort(gw->mgr, gw->restart_handle);
	}
	gw->restart_handle = MGCP_TXN_HANDLE_NONE;
	gw->restart_txid = 0;
	gw->restart_mask = 0;

	mask &= restarting_mask(gw);
	if (!mask)
		return;

	if (mask == lines_mask(gw) &&
	    entity_group(gw, mask, 0) == mask) {
		for (i = 0; i < gw->cfg.lines; i++)
			rsip_abort(gw, &gw->ep[i]);
		r.endpoint = gw->wild;
		r.entity = gw->ep[0].entity;
		gw->restart_mask = mask;
		gw->restart_handle = rsip_send(gw, &r, &txid, NULL);
		gw->restart_txid = txid;
		failed = gw->restart_handle == MGCP_TXN_HANDLE_NONE;
	} else {
		for (i = 0; i < gw->cfg.lines; i++) {
			if ((mask & (1u << i)) &&
			    !restart_ep_send(gw, &gw->ep[i]))
				failed = true;
		}
	}

	if (failed)
		restart_retry_arm(gw);
}

/* The endpoints of the RSIP are in service now (RFC 3435 4.4.6). */
static void restart_done(struct mgcp_gw *gw, uint32_t mask,
			 const struct mgcp_msg *msg)
{
	struct sip_str ne;
	unsigned int i;
	bool has_ne = msg && mgcp_msg_value(msg, MGCP_PARAM_N, &ne);

	for (i = 0; i < gw->cfg.lines; i++) {
		struct mgcp_ep *ep = &gw->ep[i];

		if (!(mask & (1u << i)))
			continue;
		if (has_ne)
			sip_str_copy(ep->entity, sizeof(ep->entity), ne);
		ep->rsip_txid = 0;
		if (ep->svc != MGCP_EP_RESTARTING)
			continue;
		ep->svc = MGCP_EP_IN_SERVICE;
		ep->rm = MGCP_RM_RESTART;
		ep->reason = 0;
		EP_TRACE(gw, "line %u in service", ep->index);
		ep_post(ep);
	}
}

/* A local activity or a command starts the restart procedure at once
 * (RFC 3435 4.4.6 step 3). */
static void restart_now(struct mgcp_gw *gw)
{
	if (!gw->restart_wait && !gw->restart_stop)
		return;

	restart_send(gw, restarting_mask(gw));
}

static void restart_response(struct mgcp_gw *gw, uint32_t mask,
			     const struct mgcp_msg *msg)
{
	struct sip_str ne;
	unsigned int i;

	if (mgcp_rc_class_of(msg->code) == MGCP_RC_SUCCESS) {
		restart_done(gw, mask, msg);
		return;
	}

	/* 4.4.6 page 141: 521 with N: redirects, 4xx tries again, any other
	 * 5xx stops the procedure until a command arrives */
	if (msg->code == 521 && mgcp_msg_value(msg, MGCP_PARAM_N, &ne)) {
		for (i = 0; i < gw->cfg.lines; i++) {
			if (mask & (1u << i))
				sip_str_copy(gw->ep[i].entity,
					     sizeof(gw->ep[i].entity), ne);
		}
		restart_send(gw, mask);
		return;
	}
	if (mgcp_rc_class_of(msg->code) == MGCP_RC_TRANSIENT) {
		restart_send(gw, mask);
		return;
	}

	gw->restart_stop = true;
	EP_TRACE(gw, "restart stopped by %u", msg->code);
}

/* A DLCX of the gateway for one connection, with its counters when the
 * backend has them (RFC 3435 2.3.8, p. 58). */
static void dlcx_send(struct mgcp_ep *ep, struct mgcp_conn *c,
		      uint32_t reason)
{
	struct mgcp_gw *gw = ep->gw;
	struct mgcp_conn_params cp;
	struct out_cmd o;
	struct sip_buf b;
	struct mgcp_dlcx d;
	bool stats = conn_stats(ep, c, &cp);

	if (!out_prep(gw, owner_of(ep), ep->entity, &o))
		return;

	memset(&d, 0, sizeof(d));
	d.txid = o.txid;
	d.endpoint = ep->name;
	d.version = version_of(gw);
	d.acks = &o.acks;
	d.call_id = c->call_id;
	d.conn_id = c->id;
	d.reason = reason;
	d.stats = stats ? &cp : NULL;

	sip_buf_init(&b, gw->wire, sizeof(gw->wire));
	if (mgcp_dlcx_build(&b, &d))
		out_send(gw, owner_of(ep), &o, &b, 0);
}

static void ep_clean(struct mgcp_ep *ep)
{
	const struct proto_host *h = ep->gw->host;
	unsigned int i;

	for (i = 0; i < MGCP_SIG_RUN_MAX; i++) {
		if (!ep->sig[i].used)
			continue;
		sig_prim_off(ep, ep->sig[i].def);
		sig_run_clear(ep, &ep->sig[i]);
	}
	if (ep->lsa_on)
		h->battery_set(h->ctx, ep->index, PROTO_BATT_ACTIVE);
	dial_reset(ep);
	ep->nreq = 0;
	ep->req_text[0] = '\0';
	ep->ndetect = 0;
	ep->detect_set = false;
	ep->detect_text[0] = '\0';
	str_copy(ep->rid, sizeof(ep->rid), "0");
	ep->req_ne[0] = '\0';
	ep->q_loop = ep->gw->cfg.quarantine_loop;
	ep->q_discard = false;
	ep->nobs = 0;
	ep->nquar = 0;
	ep->nlater = 0;
	ep->notif = false;
	ep->notified = false;
	ep->ntfy_txid = 0;
	br_cancel(ep);
	ep->lsa_on = false;
	/* RFC 3660 page 31: after a restart vmwi is off, with no record */
	ep->vmwi_on = false;
}

void mgcp_ep_service(struct mgcp_gw *gw, unsigned int line, bool in_service,
		     uint32_t reason)
{
	struct rsip_out r = { 0 };
	unsigned int i, order[MGCP_CONN_MAX];
	struct mgcp_ep *ep;
	uint32_t txid;

	if (line >= gw->cfg.lines)
		return;
	ep = &gw->ep[line];

	if (!gw->started) {
		ep->svc = in_service ? MGCP_EP_RESTARTING :
				       MGCP_EP_OUT_OF_SERVICE;
		ep->state_last = (uint8_t)ep_state_of(ep);
		return;
	}

	if (!in_service) {
		if (ep->svc == MGCP_EP_OUT_OF_SERVICE)
			return;
		br_cancel(ep);
		conn_order(ep, order);
		for (i = 0; i < MGCP_CONN_MAX; i++) {
			struct mgcp_conn *c = &ep->conn[order[i]];

			if (!c->used)
				continue;
			dlcx_send(ep, c, reason);
			conn_drop(ep, c);
		}
		conf_settle(ep);
		ep_clean(ep);
		rsip_abort(gw, ep);
		ep->disc = false;
		ep->svc = MGCP_EP_OUT_OF_SERVICE;
		ep->rm = MGCP_RM_FORCED;
		ep->reason = reason;
		r.owner = owner_of(ep);
		r.endpoint = ep->name;
		r.entity = ep->entity;
		r.method = "forced";
		r.reason = reason;
		rsip_send(gw, &r, &txid, NULL);
	} else {
		if (ep->svc != MGCP_EP_OUT_OF_SERVICE)
			return;
		ep_clean(ep);
		ep->svc = MGCP_EP_RESTARTING;
		ep->rm = MGCP_RM_RESTART;
		ep->reason = 0;
		if (!restart_ep_send(gw, ep))
			restart_retry_arm(gw);
	}

	EP_TRACE(gw, "line %u %s service", line,
		 in_service ? "into" : "out of");
	ep_post(ep);
}

/* ------------------------------------------------- disconnected procedure */

/* The endpoints in the disconnected procedure. */
static uint32_t disc_mask_of(const struct mgcp_gw *gw)
{
	uint32_t mask = 0;
	unsigned int i;

	for (i = 0; i < gw->cfg.lines; i++) {
		if (gw->ep[i].disc && gw->ep[i].svc != MGCP_EP_OUT_OF_SERVICE)
			mask |= 1u << i;
	}

	return mask;
}

/* The endpoint lost its notified entity (RFC 3435 4.3, 4.4.7). */
static void disc_mark(struct mgcp_gw *gw, uint32_t mask)
{
	unsigned int i;

	for (i = 0; i < gw->cfg.lines; i++) {
		struct mgcp_ep *ep = &gw->ep[i];

		if (!(mask & (1u << i)) || ep->svc == MGCP_EP_OUT_OF_SERVICE)
			continue;
		if (!ep->disc)
			EP_TRACE(gw, "line %u disconnected", i);
		ep->disc = true;
		ep->rm = MGCP_RM_DISCONNECTED;
	}
	mgcp_txn_disc_enter(gw->mgr);
}

/*
 * The method of an RSIP of the procedure: "disconnected" with the seconds
 * since the entry, or "restart" when the restart procedure of an endpoint it
 * names is not complete under rfc3435 (4.4.6 page 141). NCS 7.4.3.5 page 62
 * sends "disconnected" in both cases.
 */
static void disc_method_fill(struct mgcp_gw *gw, uint32_t mask,
			     struct rsip_out *r)
{
	uint64_t now = tenv(gw)->now_ms(tenv(gw));

	if (gw->cfg.dialect == CFG_MGCP_RFC3435) {
		unsigned int i;

		for (i = 0; i < gw->cfg.lines; i++) {
			if ((mask & (1u << i)) &&
			    gw->ep[i].svc == MGCP_EP_RESTARTING) {
				r->method = "restart";
				return;
			}
		}
	}

	r->method = "disconnected";
	r->has_delay = true;
	r->delay = (uint32_t)((now - gw->mgr->disc.entered_ms) / 1000);
}

/* The endpoint's own RSIP of the procedure, toward its own entity. */
static void disc_ep_send(struct mgcp_gw *gw, struct mgcp_ep *ep)
{
	struct rsip_out r = {
		.owner = owner_of(ep),
		.endpoint = ep->name,
		.entity = ep->entity,
	};
	uint32_t txid;

	if (gw->stopping)
		return;
	rsip_abort(gw, ep);
	disc_method_fill(gw, 1u << ep->index, &r);
	if (rsip_send(gw, &r, &txid, NULL) != MGCP_TXN_HANDLE_NONE)
		ep->rsip_txid = txid;
}

/*
 * One attempt of the disconnected procedure (RFC 3435 4.4.7): an RSIP for
 * each disconnected endpoint, toward its own notified entity (page 144). The
 * transaction layer times the RSIP of the first endpoint, which names every
 * endpoint when they all share that entity. Each other endpoint sends its
 * own RSIP.
 */
static void disc_rsip_send(struct mgcp_gw *gw)
{
	struct rsip_out r = { .owner = MGCP_EP_OWNER_GW,
			      .flags = MGCP_TXN_F_DISC };
	uint32_t mask = disc_mask_of(gw), txid;
	unsigned int first, i;
	bool layer;

	if (gw->stopping)
		return;
	if (gw->disc_handle && mgcp_txn_get(gw->mgr, gw->disc_handle))
		mgcp_txn_abort(gw->mgr, gw->disc_handle);
	gw->disc_handle = MGCP_TXN_HANDLE_NONE;
	gw->disc_txid = 0;
	gw->disc_mask = 0;
	if (!mask) {
		mgcp_txn_disc_leave(gw->mgr);
		return;
	}

	first = mask_first(mask);
	gw->disc_mask = entity_group(gw, mask, first) == lines_mask(gw) ?
			lines_mask(gw) : 1u << first;
	for (i = 0; i < gw->cfg.lines; i++) {
		if (gw->disc_mask & (1u << i))
			rsip_abort(gw, &gw->ep[i]);
	}

	r.endpoint = gw->disc_mask == lines_mask(gw) ? gw->wild :
		     gw->ep[first].name;
	r.entity = gw->ep[first].entity;
	disc_method_fill(gw, gw->disc_mask, &r);
	gw->disc_handle = rsip_send(gw, &r, &txid, &layer);
	gw->disc_txid = txid;

	/* a missing address never reaches the transaction layer */
	if (gw->disc_handle == MGCP_TXN_HANDLE_NONE && !layer)
		mgcp_txn_disc_fail(gw->mgr);

	for (i = 0; i < gw->cfg.lines; i++) {
		if ((mask & ~gw->disc_mask) & (1u << i))
			disc_ep_send(gw, &gw->ep[i]);
	}
}

/* No endpoint is marked here: mgcp_ep_txn_done() does it, because only that
 * upcall names the command that failed. */
void mgcp_ep_disconnected(struct mgcp_gw *gw)
{
	EP_TRACE(gw, "disconnected");
	mgcp_txn_disc_enter(gw->mgr);
}

void mgcp_ep_disc_all(struct mgcp_gw *gw)
{
	EP_TRACE(gw, "socket lost, every endpoint disconnected");
	disc_mark(gw, lines_mask(gw));
}

void mgcp_ep_disc_attempt(struct mgcp_gw *gw)
{
	disc_rsip_send(gw);
}

/*
 * The endpoints of mask are connected again, with the N: of the response
 * when it has one. A restart the RSIP carried completes under rfc3435, and a
 * NTFY lost on the way leaves the notification state (RFC 3435 4.4.1 page
 * 128). The other endpoints stay in the procedure, which starts again for
 * them. Under ncs the restart still owes its own RSIP (NCS 7.4.3.5 page 62).
 */
static void disc_done(struct mgcp_gw *gw, uint32_t mask,
		      const struct mgcp_msg *msg)
{
	struct sip_str ne;
	bool has_ne = msg && mgcp_msg_value(msg, MGCP_PARAM_N, &ne);
	uint32_t restart;
	unsigned int i;

	EP_TRACE(gw, "connected again, mask 0x%x", mask);
	for (i = 0; i < gw->cfg.lines; i++) {
		struct mgcp_ep *ep = &gw->ep[i];

		if (!(mask & (1u << i)) || !ep->disc)
			continue;
		ep->disc = false;
		if (has_ne)
			sip_str_copy(ep->entity, sizeof(ep->entity), ne);
		if (ep->svc == MGCP_EP_RESTARTING &&
		    gw->cfg.dialect == CFG_MGCP_RFC3435) {
			ep->svc = MGCP_EP_IN_SERVICE;
			ep->reason = 0;
		}
		ep->rm = MGCP_RM_RESTART;
		if (ep->notif && !ep->ntfy_txid)
			notif_leave(ep);
		ep_post(ep);
	}

	if (!disc_mask_of(gw)) {
		mgcp_txn_disc_leave(gw->mgr);
	} else {
		mgcp_txn_disc_enter(gw->mgr);
	}

	restart = mask & restarting_mask(gw);
	if (restart)
		restart_send(gw, restart);
}

/*
 * The layer calls this before response() delivers the success, so the
 * endpoints drain only after the N: of that response took effect.
 */
void mgcp_ep_disc_ended(struct mgcp_gw *gw)
{
	gw->disc_end = true;
}

/* The response to the RSIP the transaction layer times. */
static void disc_response(struct mgcp_gw *gw, const struct mgcp_msg *msg)
{
	uint32_t mask = gw->disc_mask;
	struct sip_str ne;
	unsigned int i;

	gw->disc_txid = 0;
	if (gw->disc_end ||
	    mgcp_rc_class_of(msg->code) == MGCP_RC_SUCCESS) {
		gw->disc_end = false;
		gw->disc_mask = 0;
		disc_done(gw, mask, msg);
		return;
	}

	/* 4.4.6: a 521 with N: redirects the RSIP at once */
	if (msg->code != 521 || !mgcp_msg_value(msg, MGCP_PARAM_N, &ne))
		return;
	for (i = 0; i < gw->cfg.lines; i++) {
		if (mask & (1u << i))
			sip_str_copy(gw->ep[i].entity,
				     sizeof(gw->ep[i].entity), ne);
	}
	if (mgcp_txn_disc_kick(gw->mgr, false))
		disc_rsip_send(gw);
}

/* The response to an endpoint's own RSIP of the procedure. */
static void disc_ep_response(struct mgcp_gw *gw, struct mgcp_ep *ep,
			     const struct mgcp_msg *msg)
{
	struct sip_str ne;

	if (mgcp_rc_class_of(msg->code) == MGCP_RC_SUCCESS) {
		disc_done(gw, 1u << ep->index, msg);
		return;
	}
	if (msg->code == 521 && mgcp_msg_value(msg, MGCP_PARAM_N, &ne)) {
		sip_str_copy(ep->entity, sizeof(ep->entity), ne);
		disc_ep_send(gw, ep);
		return;
	}

	/* the next attempt of the procedure sends it again */
	mgcp_txn_disc_enter(gw->mgr);
}

/*
 * A command for a disconnected endpoint starts an attempt at once (4.4.7
 * step 3).
 */
static void disc_command(struct mgcp_gw *gw, const struct cmd *c)
{
	if (!(c->mask & disc_mask_of(gw)))
		return;

	if (mgcp_txn_disc_kick(gw->mgr, false))
		disc_rsip_send(gw);
}

/* The RSIP still out that names the endpoint, or MGCP_TXN_HANDLE_NONE. */
static uint32_t rsip_handle_of(struct mgcp_gw *gw, const struct mgcp_ep *ep)
{
	uint32_t bit = 1u << ep->index;

	if (ep->rsip_txid)
		return mgcp_txn_find(gw->mgr, ep->rsip_txid);
	if (ep->disc)
		return (gw->disc_mask & bit) ? gw->disc_handle :
					       MGCP_TXN_HANDLE_NONE;

	return (gw->restart_mask & bit) ? gw->restart_handle :
					  MGCP_TXN_HANDLE_NONE;
}

/*
 * The RSIP that rides in front of the response (RFC 3435 4.4.6 page 141,
 * 4.4.7 page 143; NCS 7.4.3.5 page 62 and 7.4.3.6): the one that names the
 * first endpoint of the command. Under rfc3435 an audit is exempt, and so
 * is a restarting endpoint that the procedure does not hold (4.4.6 page
 * 140). Under ncs every response carries it.
 */
static void front_take(struct mgcp_gw *gw, struct cmd *c)
{
	bool ncs = gw->cfg.dialect == CFG_MGCP_NCS;
	unsigned int i;

	for (i = 0; i < gw->cfg.lines && !c->front; i++) {
		const struct mgcp_ep *ep = &gw->ep[i];

		if (!(c->mask & (1u << i)))
			continue;
		if ((ep->disc && (ncs || !c->audit)) ||
		    (ep->svc == MGCP_EP_RESTARTING && ncs))
			c->front = rsip_handle_of(gw, ep);
	}
}

/* ------------------------------------------------------------- commands */

/*
 * The endpoints a command names (RFC 3435 2.1.2). A local name that ends
 * in "*" is the "all of" wildcard. "$" in its place is the "any of"
 * wildcard, which names the first endpoint in service with no connection.
 */
static uint32_t target_take(struct mgcp_gw *gw, struct cmd *c)
{
	struct sip_str name = c->msg->endpoint, local, domain;
	bool named = false;
	size_t at, i, pre;

	for (at = name.len; at > 0 && name.p[at - 1] != '@'; at--)
		;
	if (!at)
		return 500;

	local = sip_str_init(name.p, at - 1);
	domain = sip_str_init(name.p + at, name.len - at);
	if (!local.len || !sip_str_eq_ci(domain, gw->cfg.host))
		return 500;

	c->all = local.p[local.len - 1] == '*';
	c->any = local.p[local.len - 1] == '$';
	pre = c->all || c->any ? local.len - 1 : local.len;

	for (i = 0; i < gw->cfg.lines; i++) {
		const struct mgcp_ep *ep = &gw->ep[i];
		size_t own_len = local_len(ep->name);

		if (c->all || c->any ?
		    pre > own_len || strncasecmp(ep->name, local.p, pre) :
		    own_len != local.len ||
		    strncasecmp(ep->name, local.p, local.len))
			continue;
		named = true;
		if (c->any && (ep->svc != MGCP_EP_IN_SERVICE ||
			       ep_has_conn(ep)))
			continue;
		c->mask |= 1u << i;
		if (c->any)
			break;
	}

	/* E_ANY_NONE is for endpoints that exist and are all busy */
	if (!c->mask)
		return c->any && named ? err_code(gw, E_ANY_NONE) : 500;
	for (i = 0; !c->all && i < gw->cfg.lines; i++) {
		if (c->mask == (1u << i))
			c->ep = &gw->ep[i];
	}

	return 0;
}

static void cmd_rqnt(struct mgcp_gw *gw, struct cmd *c)
{
	uint32_t code;

	if (!c->ep) {
		reply_code(gw, c, 503);
		return;
	}

	code = rq_take(gw, c->ep, c->msg, &rq_stage, true);
	if (code) {
		reply_code(gw, c, code);
		return;
	}

	rq_apply(c->ep, &rq_stage);
	code = rq_digits_play(c->ep, &rq_stage);
	reply_code(gw, c, code ? code : 200);
}

static void cmd_epcf(struct mgcp_gw *gw, struct cmd *c)
{
	uint32_t code;
	unsigned int i;

	if (!mgcp_msg_param(c->msg, MGCP_PARAM_B)) {
		reply_code(gw, c, 510);
		return;
	}

	code = rq_bearer_take(c->msg, &rq_stage);
	if (code) {
		reply_code(gw, c, code);
		return;
	}

	for (i = 0; i < gw->cfg.lines; i++) {
		if (c->mask & (1u << i))
			str_copy(gw->ep[i].bearer, sizeof(gw->ep[i].bearer),
				 rq_stage.bearer);
	}
	reply_code(gw, c, 200);
}

static struct mgcp_conn *conn_slot_free(struct mgcp_ep *ep)
{
	unsigned int i;

	for (i = 0; i < MGCP_CONN_MAX; i++) {
		if (!ep->conn[i].used)
			return &ep->conn[i];
	}

	return NULL;
}

/* C: as 1*32 HEXDIG, or 510. */
static bool call_id_take(const struct mgcp_msg *msg, char *out)
{
	struct sip_str v;

	return mgcp_msg_call_id(msg, &v) && sip_str_copy(out, MGCP_ID_MAX, v);
}

static void reply_lcd(struct reply *r, const struct mgcp_conn *c)
{
	r->sdp = c->lcd;
	r->sdp_len = c->lcd_len;
}

/*
 * CreateConnection (RFC 3435 2.3.5). The connection is built in scratch and
 * joins the endpoint when the whole command passed.
 */
static void cmd_crcx(struct mgcp_gw *gw, struct cmd *c)
{
	const struct proto_host *h = gw->host;
	struct mgcp_ep *ep = c->ep;
	struct mgcp_conn *s = &gw->scratch, *slot;
	const struct cfg_channel *ch;
	struct sip_buf body;
	enum mgcp_mode mode;
	struct lco l;
	uint16_t min, max;
	uint32_t code;
	struct reply r = { 0 };
	bool lcd_new, touched;

	if (!ep) {
		reply_code(gw, c, 510);
		return;
	}
	ch = channel_of(ep);

	memset(s, 0, sizeof(*s));
	if (!call_id_take(c->msg, s->call_id) ||
	    !mgcp_msg_mode(c->msg, &mode)) {
		reply_code(gw, c, 510);
		return;
	}

	code = mode_check(gw, mode);
	if (!code && !conn_slot_free(ep))
		code = err_code(gw, E_CONN_LIMIT);
	if (!code && !c->msg->sdp.len && mode_needs_remote(mode))
		code = 527;
	if (!code)
		code = lco_take(ep, c->msg, &l);
	if (!code)
		code = rq_take(gw, ep, c->msg, &rq_stage, false);
	lopt_init(&s->lopt);
	if (!code && !lopt_merge(ep, &s->lopt, &l, NULL))
		code = 510;
	if (code) {
		reply_code(gw, c, code);
		return;
	}

	h->net_port_range(h->ctx, ch, &min, &max);
	if (!h->net_port_alloc(h->ctx, min, max, &s->port)) {
		reply_code(gw, c, err_code(gw, E_NO_PORT));
		return;
	}

	s->mode = (uint8_t)mode;
	slot = conn_slot_free(ep);
	s->stream = (unsigned int)(slot - ep->conn);
	conn_local_build(ep, s, true);
	if (!s->loc.codecs) {
		h->net_port_free(h->ctx, s->port);
		reply_code(gw, c, 534);
		return;
	}
	sdp_state_init(&s->sdp, sess_id_new(gw));
	sdp_dir_attr_enable(&s->sdp, false);
	sdp_dir_set(&s->sdp, mode_dir(s->mode));

	sip_buf_init(&body, gw->body, sizeof(gw->body));
	if (c->msg->sdp.len) {
		code = conn_remote_take(ep, s, c->msg->sdp, &body, &lcd_new);
	} else if (!sdp_offer_build(&s->sdp, &s->loc, &body)) {
		code = 534;
	}
	if (!code && conn_program(ep, s, &touched) != HAL_OK)
		code = 400;
	if (code) {
		conn_release(ep, s);
		reply_code(gw, c, code);
		return;
	}

	lcd_keep(s, &body);
	s->used = true;
	conn_id_new(ep, s->id);
	*slot = *s;
	conf_settle(ep);
	rq_apply(ep, &rq_stage);
	EP_TRACE(gw, "line %u connection %s mode %s", ep->index, slot->id,
		 mgcp_mode_name(mode));

	code = rq_digits_play(ep, &rq_stage);
	if (code) {
		conn_delete(ep, slot);
		reply_code(gw, c, code);
		return;
	}

	rec_open(ep, slot);
	rec_note(ep, slot);
	reply_kv(&r, MGCP_PARAM_I, slot->id);
	if (c->any)
		reply_kv(&r, MGCP_PARAM_Z, ep->name);
	reply_lcd(&r, slot);
	reply_send(gw, c, 200, &r);
}

/*
 * The connection a command names by I:. need_call says the command must
 * carry C: (RFC 3435 2.3.6 and 2.3.7); AUCX need not (2.3.11). A C: that is
 * present must be 1*32 HEXDIG in both cases.
 */
static struct mgcp_conn *conn_of_cmd(struct mgcp_ep *ep,
				     const struct mgcp_msg *msg,
				     bool need_call, uint32_t *code)
{
	struct mgcp_conn *conn;
	struct sip_str id, call;
	bool has_call = mgcp_msg_param(msg, MGCP_PARAM_C) != NULL;

	if ((need_call || has_call) && !mgcp_msg_call_id(msg, &call)) {
		*code = 510;
		return NULL;
	}
	if (!mgcp_msg_conn_id(msg, &id)) {
		*code = 510;
		return NULL;
	}

	conn = (struct mgcp_conn *)conn_find_id(ep, id);
	if (!conn) {
		*code = 515;
		return NULL;
	}

	if (has_call && !sip_str_eq_ci(call, conn->call_id)) {
		*code = 516;
		return NULL;
	}

	*code = 0;

	return conn;
}

/*
 * The remote description of an MDCX, or a new LocalConnectionOptions on a
 * connection that has none: the stored remote description is answered
 * again, or the offer is built again (RFC 3435 2.3.6 page 53).
 */
static uint32_t mdcx_media_take(struct mgcp_ep *ep, struct mgcp_conn *s,
				const struct mgcp_msg *msg, const struct lco *l,
				struct sip_buf *body, bool *lcd_new)
{
	struct sdp_msg stored;
	enum sdp_verdict v;

	*lcd_new = false;
	if (msg->sdp.len)
		return conn_remote_take(ep, s, msg->sdp, body, lcd_new);
	if (!l->has)
		return 0;

	if (s->sdp.offer_pending || !s->remote) {
		*lcd_new = true;
		return sdp_offer_build(&s->sdp, &s->loc, body) ? 0 : 534;
	}

	if (!sdp_parse(sip_str_init(s->rcd, s->rcd_len), &stored))
		return err_code(ep->gw, E_RCD);
	v = sdp_answer_build(&s->sdp, &s->loc, &stored, body);
	if (v != SDP_OK)
		return sdp_refusal(ep->gw, v, &stored);
	sdp_commit(&s->sdp);
	*lcd_new = true;

	return 0;
}

/* ModifyConnection (RFC 3435 2.3.6), on a scratch copy of the connection. */
static void cmd_mdcx(struct mgcp_gw *gw, struct cmd *c)
{
	struct mgcp_ep *ep = c->ep;
	struct mgcp_conn *s = &gw->scratch, *conn;
	struct sip_buf body;
	enum mgcp_mode mode;
	struct lco l;
	uint32_t code;
	struct reply r = { 0 };
	bool lcd_new = false, has_mode, new_offer, touched;

	if (!ep) {
		reply_code(gw, c, 510);
		return;
	}

	conn = conn_of_cmd(ep, c->msg, true, &code);
	if (!conn) {
		reply_code(gw, c, code);
		return;
	}

	has_mode = mgcp_msg_param(c->msg, MGCP_PARAM_M) != NULL;
	mode = (enum mgcp_mode)conn->mode;
	if (has_mode && !mgcp_msg_mode(c->msg, &mode)) {
		reply_code(gw, c, 510);
		return;
	}

	code = mode_check(gw, mode);
	if (!code && !conn->remote && !c->msg->sdp.len &&
	    mode_needs_remote(mode))
		code = 527;
	if (!code)
		code = lco_take(ep, c->msg, &l);
	if (!code)
		code = rq_take(gw, ep, c->msg, &rq_stage, false);
	if (code) {
		reply_code(gw, c, code);
		return;
	}

	*s = *conn;
	s->mode = (uint8_t)mode;
	sdp_dir_set(&s->sdp, mode_dir(s->mode));
	/*
	 * RFC 3435 2.3.6 page 53: an L: changes the options it names and the
	 * others stay. A new remote offer meets the a: of this command, or
	 * else the codecs of the line, and that list is the a: in force from
	 * now on. An answer meets the view its offer was built from.
	 */
	new_offer = c->msg->sdp.len && !s->sdp.offer_pending;
	if (l.has || new_offer) {
		if (!lopt_merge(ep, &s->lopt, &l,
				new_offer && !l.v.a.len ? "a" : NULL)) {
			reply_code(gw, c, 510);
			return;
		}
		conn_local_build(ep, s, true);
		if (!s->loc.codecs) {
			reply_code(gw, c, 534);
			return;
		}
	}

	sip_buf_init(&body, gw->body, sizeof(gw->body));
	code = mdcx_media_take(ep, s, c->msg, &l, &body, &lcd_new);
	touched = false;
	if (!code && conn_program(ep, s, &touched) != HAL_OK)
		code = 400;
	if (code) {
		if (touched)
			conn_restore(ep, conn, s);
		reply_code(gw, c, code);
		return;
	}

	if (lcd_new)
		lcd_keep(s, &body);
	*conn = *s;
	conf_settle(ep);
	rec_note(ep, conn);
	rq_apply(ep, &rq_stage);

	code = rq_digits_play(ep, &rq_stage);
	if (code) {
		reply_code(gw, c, code);
		return;
	}

	if (lcd_new)
		reply_lcd(&r, conn);
	reply_send(gw, c, 200, &r);
}

/* DeleteConnection from the call agent (RFC 3435 2.3.7, 2.3.9). */
static void cmd_dlcx(struct mgcp_gw *gw, struct cmd *c)
{
	struct mgcp_conn_params cp;
	struct mgcp_conn *conn;
	struct sip_str call;
	struct reply r = { 0 };
	unsigned int i, e, order[MGCP_CONN_MAX];
	uint32_t code;
	bool by_call;

	if (mgcp_msg_param(c->msg, MGCP_PARAM_I)) {
		if (!c->ep) {
			reply_code(gw, c, 510);
			return;
		}
		conn = conn_of_cmd(c->ep, c->msg, true, &code);
		if (!code)
			code = rq_take(gw, c->ep, c->msg, &rq_stage, false);
		if (code) {
			reply_code(gw, c, code);
			return;
		}

		if (conn_stats(c->ep, conn, &cp))
			reply_kv(&r, MGCP_PARAM_P, cp_text(gw, &cp));
		/* the new request replaces the pending digits, which name the
		 * stream they went to */
		if (rq_stage.has_x)
			br_cancel(c->ep);
		conn_delete(c->ep, conn);
		rq_apply(c->ep, &rq_stage);
		code = rq_digits_play(c->ep, &rq_stage);
		if (code) {
			reply_code(gw, c, code);
			return;
		}
		reply_send(gw, c, 250, &r);
		return;
	}

	/* RFC 3435 2.3.9 page 59: no request rides a DLCX of several
	 * connections */
	if (mgcp_msg_param(c->msg, MGCP_PARAM_X) || rq_params_present(c->msg)) {
		reply_code(gw, c, 510);
		return;
	}

	by_call = mgcp_msg_value(c->msg, MGCP_PARAM_C, &call);
	for (e = 0; e < gw->cfg.lines; e++) {
		struct mgcp_ep *ep = &gw->ep[e];

		if (!(c->mask & (1u << e)))
			continue;
		conn_order(ep, order);
		for (i = 0; i < MGCP_CONN_MAX; i++) {
			conn = &ep->conn[order[i]];
			if (!conn->used)
				continue;
			if (by_call && !sip_str_eq_ci(call, conn->call_id))
				continue;
			conn_delete(ep, conn);
		}
	}
	reply_code(gw, c, 250);
}

/* ----------------------------------------------------------- the audits */

static const char *quarantine_text(const struct mgcp_ep *ep)
{
	return ep->q_loop ? (ep->q_discard ? "discard,loop" : "process,loop") :
			    (ep->q_discard ? "discard,step" : "process,step");
}

/* A list, then the persistent events that it does not already name. */
static const char *events_text(struct mgcp_gw *gw, const struct mgcp_ep *ep,
			       const char *list)
{
	static const struct {
		uint8_t kind;
		const char *name;
	} hook[] = {
		{ MGCP_EVK_HD, "L/hd" }, { MGCP_EVK_HU, "L/hu" },
		{ MGCP_EVK_HF, "L/hf" },
	};
	char text[MGCP_TEXT_MAX];
	struct sip_buf b;
	unsigned int i;

	sip_buf_init(&b, text, sizeof(text));
	sip_buf_add(&b, list);
	for (i = 0; i < ARRAY_LEN(hook); i++) {
		struct mgcp_obs o = { .kind = hook[i].kind };

		if (!persist_match(ep, &o) || req_find(ep, &o))
			continue;
		sip_buf_addf(&b, "%s%s", sip_buf_len(&b) ? "," : "",
			     hook[i].name);
	}

	return arena_printf(gw, "%s", sip_buf_ok(&b) ? text : list);
}

/* SignalRequests in an audit (RFC 3435 2.3.10 page 62): the TO signals
 * that run, the OO signals that are on, the pending BR signals. */
static const char *signals_text(struct mgcp_gw *gw, const struct mgcp_ep *ep)
{
	char text[MGCP_TEXT_MAX];
	struct sip_buf b;
	unsigned int i;

	sip_buf_init(&b, text, sizeof(text));
	for (i = 0; i < MGCP_SIG_RUN_MAX; i++) {
		const struct mgcp_sig_run *run = &ep->sig[i];

		if (!run->used)
			continue;
		sip_buf_addf(&b, "%s%s", sip_buf_len(&b) ? "," : "",
			     run->name);
		if (run->params[0])
			sip_buf_addf(&b, "(%s)", run->params);
	}
	if (ep->vmwi_on)
		sip_buf_addf(&b, "%sL/vmwi(+)", sip_buf_len(&b) ? "," : "");
	if (ep->lsa_on)
		sip_buf_addf(&b, "%sL/lsa(+)", sip_buf_len(&b) ? "," : "");
	if (ep->br_pending[0])
		sip_buf_addf(&b, "%s%s", sip_buf_len(&b) ? "," : "",
			     ep->br_pending);

	return arena_printf(gw, "%s", sip_buf_ok(&b) ? text : "");
}

static const char *observed_text(struct mgcp_gw *gw, const struct mgcp_ep *ep)
{
	char text[MGCP_TEXT_MAX];
	struct sip_buf b;
	unsigned int i;

	sip_buf_init(&b, text, sizeof(text));
	for (i = 0; i < ep->nobs; i++) {
		if (i)
			sip_buf_add(&b, ",");
		mgcp_obs_write(gw->cfg.dialect, &ep->obs[i], &b);
	}

	return arena_printf(gw, "%s", sip_buf_ok(&b) ? text : "");
}

static const char *conn_ids_text(struct mgcp_gw *gw, const struct mgcp_ep *ep)
{
	const char *a = ep->conn[0].used ? ep->conn[0].id : "";
	const char *b = ep->conn[1].used ? ep->conn[1].id : "";

	return arena_printf(gw, "%s%s%s", a, a[0] && b[0] ? ", " : "", b);
}

/* Capabilities (RFC 3435 3.2.2.3): the codecs of the line. */
static const char *caps_text(struct mgcp_gw *gw, struct mgcp_ep *ep)
{
	struct mgcp_conn *s = &gw->scratch;
	char text[MGCP_TEXT_MAX];
	struct sip_buf b;
	unsigned int i;

	memset(s, 0, sizeof(*s));
	lopt_init(&s->lopt);
	conn_local_build(ep, s, false);

	sip_buf_init(&b, text, sizeof(text));
	sip_buf_add(&b, "a:");
	for (i = 0; i < s->loc.codecs; i++)
		sip_buf_addf(&b, "%s%s", i ? ";" : "",
			     sdp_codec_name((enum sdp_codec_id)
					    s->loc.codec[i].id));
	if (s->loc.t38)
		sip_buf_addf(&b, "%simage/t38", s->loc.codecs ? ";" : "");
	sip_buf_addf(&b, ", p:10-30, e:on, s:on, v:%s, "
		     "m:sendonly;recvonly;sendrecv;inactive%s",
		     gw->cfg.dialect == CFG_MGCP_NCS ? "L" : "L;G;D",
		     (gw->host->hal_features(gw->host->ctx) & HAL_FEAT_CONF) ?
		     ";confrnce" : "");

	return arena_printf(gw, "%s", sip_buf_ok(&b) ? text : "");
}

static void auep_info(struct mgcp_gw *gw, struct mgcp_ep *ep,
		      enum mgcp_param code, struct reply *r)
{
	switch (code) {
	case MGCP_PARAM_R:
		reply_kv(r, code, events_text(gw, ep, ep->req_text));
		break;
	case MGCP_PARAM_D:
		reply_kv(r, code, arena_printf(gw, "%s", ep->dmap));
		break;
	case MGCP_PARAM_S:
		reply_kv(r, code, signals_text(gw, ep));
		break;
	case MGCP_PARAM_X:
		reply_kv(r, code, ep->rid);
		break;
	case MGCP_PARAM_Q:
		reply_kv(r, code, quarantine_text(ep));
		break;
	case MGCP_PARAM_N:
		reply_kv(r, code, ep->entity);
		break;
	case MGCP_PARAM_I:
		reply_kv(r, code, conn_ids_text(gw, ep));
		break;
	case MGCP_PARAM_T:
		reply_kv(r, code, events_text(gw, ep, ep->detect_text));
		break;
	case MGCP_PARAM_O:
		reply_kv(r, code, observed_text(gw, ep));
		break;
	case MGCP_PARAM_ES:
		reply_kv(r, code, ep->offhook ? "L/hd" : "L/hu");
		break;
	case MGCP_PARAM_B:
		reply_kv(r, code, ep->bearer[0] ?
			 arena_printf(gw, "e:%s", ep->bearer) : "");
		break;
	case MGCP_PARAM_RM:
		reply_kv(r, code, mgcp_rm_name((enum mgcp_rm)ep->rm));
		break;
	case MGCP_PARAM_RD:
		reply_kv(r, code, "0");
		break;
	case MGCP_PARAM_E:
		reply_kv(r, code, arena_printf(gw, "%03u", ep->reason));
		break;
	case MGCP_PARAM_PL:
		reply_kv(r, code, gw->cfg.dialect == CFG_MGCP_NCS ? "L:1" :
			 "L:1,G:1,D:1");
		break;
	case MGCP_PARAM_MD:
		reply_kv(r, code, arena_printf(gw, "%zu",
					       gw->mgr->cfg.wire_max));
		break;
	case MGCP_PARAM_A:
		reply_kv(r, code, caps_text(gw, ep));
		break;
	default:
		/* 2.3.10 page 62: an unknown code is left out, no error */
		break;
	}
}

/* AuditEndpoint (RFC 3435 2.3.10). A wildcard lists the endpoints. */
static void cmd_auep(struct mgcp_gw *gw, struct cmd *c)
{
	struct reply r = { 0 };
	struct sip_str rest, name;
	enum mgcp_param code;
	unsigned int i;

	if (!c->ep) {
		char tail[MGCP_TEXT_MAX];
		struct sip_buf b;

		sip_buf_init(&b, tail, sizeof(tail));
		for (i = 0; i < gw->cfg.lines; i++) {
			if (c->mask & (1u << i))
				sip_buf_addf(&b, "Z: %s\r\n", gw->ep[i].name);
		}
		r.tail = arena_printf(gw, "%s", tail);
		reply_send(gw, c, 200, &r);
		return;
	}

	if (mgcp_msg_value(c->msg, MGCP_PARAM_F, &rest)) {
		while (mgcp_info_next(&rest, &code, &name))
			auep_info(gw, c->ep, code, &r);
	}
	reply_send(gw, c, 200, &r);
}

/* AuditConnection (RFC 3435 2.3.11); the local description goes before the
 * remote one, and a missing one is "v=0" alone (3.3 page 104). */
static void cmd_aucx(struct mgcp_gw *gw, struct cmd *c)
{
	struct mgcp_conn_params cp;
	struct mgcp_conn *conn;
	struct sip_str rest, name;
	enum mgcp_param code;
	struct reply r = { 0 };
	struct sip_buf body;
	bool lc = false, rc = false;
	const char *ltext;
	uint32_t err;

	if (!c->ep) {
		reply_code(gw, c, 510);
		return;
	}
	conn = conn_of_cmd(c->ep, c->msg, false, &err);
	if (!conn) {
		reply_code(gw, c, err);
		return;
	}
	ltext = lopt_text(gw, &conn->lopt);

	if (mgcp_msg_value(c->msg, MGCP_PARAM_F, &rest)) {
		while (mgcp_info_next(&rest, &code, &name)) {
			switch (code) {
			case MGCP_PARAM_C:
				reply_kv(&r, code, conn->call_id);
				break;
			case MGCP_PARAM_N:
				reply_kv(&r, code, c->ep->entity);
				break;
			case MGCP_PARAM_L:
				reply_kv(&r, code, ltext);
				break;
			case MGCP_PARAM_M:
				reply_kv(&r, code, mgcp_mode_name(
					(enum mgcp_mode)conn->mode));
				break;
			case MGCP_PARAM_P:
				if (conn_stats(c->ep, conn, &cp))
					reply_kv(&r, code, cp_text(gw, &cp));
				break;
			case MGCP_PARAM_LC:
				lc = true;
				break;
			case MGCP_PARAM_RC:
				rc = true;
				break;
			default:
				break;
			}
		}
	}

	sip_buf_init(&body, gw->body, sizeof(gw->body));
	if (lc) {
		if (conn->lcd_len)
			sip_buf_add_len(&body, conn->lcd, conn->lcd_len);
		else
			sip_buf_add(&body, "v=0\r\n");
	}
	if (rc) {
		if (lc)
			sip_buf_crlf(&body);
		if (conn->rcd_len)
			sip_buf_add_len(&body, conn->rcd, conn->rcd_len);
		else
			sip_buf_add(&body, "v=0\r\n");
	}
	if (sip_buf_len(&body) && sip_buf_ok(&body)) {
		r.sdp = sip_buf_str(&body);
		r.sdp_len = sip_buf_len(&body);
	}
	reply_send(gw, c, 200, &r);
}

static bool version_ok(const struct mgcp_msg *msg)
{
	if (msg->flags & MGCP_MSG_F_BAD_VERSION)
		return false;

	return mgcp_msg_version_is(msg, MGCP_VERSION_RFC3435) ||
	       mgcp_msg_version_is(msg, MGCP_VERSION_NCS);
}

/* The verbs a call agent sends to a gateway, under the dialect. */
static bool verb_known(const struct mgcp_gw *gw, enum mgcp_verb verb)
{
	switch (verb) {
	case MGCP_VERB_EPCF:
		return gw->cfg.dialect != CFG_MGCP_NCS;
	case MGCP_VERB_CRCX:
	case MGCP_VERB_MDCX:
	case MGCP_VERB_DLCX:
	case MGCP_VERB_RQNT:
	case MGCP_VERB_AUEP:
	case MGCP_VERB_AUCX:
		return true;
	default:
		return false;
	}
}

/*
 * The service state of the endpoints a non-audit command names (RFC 3435
 * 4.4.5 page 138): restarting gets the restarting code of the dialect, out
 * of service 501. front_take() chose the RSIP in front of it.
 */
static bool service_refuse(struct mgcp_gw *gw, struct cmd *c)
{
	unsigned int i;

	for (i = 0; i < gw->cfg.lines; i++) {
		const struct mgcp_ep *ep = &gw->ep[i];

		if (!(c->mask & (1u << i)))
			continue;

		if (ep->svc == MGCP_EP_OUT_OF_SERVICE) {
			reply_code(gw, c, 501);
			return true;
		}
		if (ep->svc != MGCP_EP_RESTARTING)
			continue;

		reply_code(gw, c, err_code(gw, E_RESTARTING));
		return true;
	}

	return false;
}

/* RFC 3435 3.2.2 page 86: a parameter "X+..." that the gateway does not
 * know. */
static bool ext_unknown(const struct mgcp_msg *msg)
{
	unsigned int i;

	for (i = 0; i < msg->param_count; i++) {
		const struct mgcp_param_line *p = &msg->param[i];

		if (p->code == MGCP_PARAM_UNKNOWN && p->name.len >= 2 &&
		    (p->name.p[0] == 'X' || p->name.p[0] == 'x') &&
		    p->name.p[1] == '+')
			return true;
	}

	return false;
}

/* The call trace reason of a command: its verb in lower case, as "crcx". */
static void verb_word(enum mgcp_verb verb, char *out, size_t olen)
{
	const char *name = mgcp_verb_name(verb);
	size_t i;

	for (i = 0; name[i] && i + 1 < olen; i++)
		out[i] = (char)tolower((unsigned char)name[i]);
	out[i] = '\0';
}

void mgcp_ep_command(struct mgcp_gw *gw, const struct mgcp_msg *msg,
		     const struct sockaddr *src, socklen_t src_len)
{
	struct cmd c = { .msg = msg, .src = src, .src_len = src_len };
	char word[8];
	uint32_t code;
	unsigned int i;

	gw->arena_used = 0;
	gw->stat.commands++;

	if (!version_ok(msg)) {
		reply_code(gw, &c, 528);
		return;
	}
	if ((msg->flags & MGCP_MSG_F_VERB_BAD) || !verb_known(gw, msg->verb)) {
		reply_code(gw, &c, err_code(gw, E_VERB));
		return;
	}

	code = target_take(gw, &c);
	if (code) {
		reply_code(gw, &c, code);
		return;
	}
	c.audit = msg->verb == MGCP_VERB_AUEP || msg->verb == MGCP_VERB_AUCX;

	restart_now(gw);
	disc_command(gw, &c);
	front_take(gw, &c);

	/* 511 changes no request state, but it carries the RSIP in front */
	if (ext_unknown(msg)) {
		reply_code(gw, &c, 511);
		return;
	}
	if (!c.audit && service_refuse(gw, &c))
		return;

	switch (msg->verb) {
	case MGCP_VERB_EPCF:
		cmd_epcf(gw, &c);
		break;
	case MGCP_VERB_RQNT:
		cmd_rqnt(gw, &c);
		break;
	case MGCP_VERB_CRCX:
		cmd_crcx(gw, &c);
		break;
	case MGCP_VERB_MDCX:
		cmd_mdcx(gw, &c);
		break;
	case MGCP_VERB_DLCX:
		cmd_dlcx(gw, &c);
		break;
	case MGCP_VERB_AUEP:
		cmd_auep(gw, &c);
		break;
	case MGCP_VERB_AUCX:
		cmd_aucx(gw, &c);
		break;
	default:
		break;
	}

	verb_word(msg->verb, word, sizeof(word));
	for (i = 0; i < gw->cfg.lines; i++) {
		if (!(c.mask & (1u << i)))
			continue;
		gw->host->calltrace_event(gw->host->ctx, i, word);
		ep_post(&gw->ep[i]);
	}
}

/* --------------------------------------------- responses to our commands */

void mgcp_ep_response(struct mgcp_gw *gw, uint32_t owner, uint32_t txid,
		      const struct mgcp_msg *msg)
{
	struct mgcp_ep *ep;

	if (owner == MGCP_EP_OWNER_GW) {
		if (txid && txid == gw->restart_txid) {
			gw->restart_txid = 0;
			restart_response(gw, gw->restart_mask, msg);
			return;
		}
		if (txid && txid == gw->disc_txid)
			disc_response(gw, msg);
		return;
	}

	if (!owner || owner > gw->cfg.lines)
		return;
	ep = &gw->ep[owner - 1];

	if (txid == ep->ntfy_txid && ep->notif) {
		notif_leave(ep);
		ep_post(ep);
		return;
	}
	if (!txid || txid != ep->rsip_txid)
		return;
	if (ep->disc)
		disc_ep_response(gw, ep, msg);
	else if (ep->svc == MGCP_EP_RESTARTING)
		restart_response(gw, 1u << ep->index, msg);
}

/*
 * The end of a gateway command. The layer reports a disconnection before
 * this call and does not name the command, so the endpoints of a command
 * that timed out join the disconnected procedure here.
 */
void mgcp_ep_txn_done(struct mgcp_gw *gw, uint32_t owner, uint32_t txid,
		      enum mgcp_txn_reason reason)
{
	struct mgcp_ep *ep;
	uint32_t mask;

	if (owner == MGCP_EP_OWNER_GW) {
		if (reason == MGCP_TXN_DONE_OK && gw->disc_end) {
			mask = gw->disc_mask;
			gw->disc_end = false;
			gw->disc_mask = 0;
			gw->disc_txid = 0;
			disc_done(gw, mask, NULL);
			return;
		}
		/* a send failure is no disconnection: the restart goes on */
		if (reason == MGCP_TXN_DONE_TRANSPORT && txid &&
		    txid == gw->restart_txid) {
			gw->restart_txid = 0;
			restart_retry_arm(gw);
			return;
		}
		if (reason != MGCP_TXN_DONE_TIMEOUT)
			return;
		if (txid && txid == gw->disc_txid) {
			gw->disc_txid = 0;
			return;
		}
		mask = lines_mask(gw);
		if (txid && txid == gw->restart_txid) {
			mask = gw->restart_mask;
			gw->restart_txid = 0;
		}
		disc_mark(gw, mask);
		return;
	}

	if (!owner || owner > gw->cfg.lines)
		return;
	ep = &gw->ep[owner - 1];
	if (txid && txid == ep->rsip_txid) {
		ep->rsip_txid = 0;
		if (reason == MGCP_TXN_DONE_TRANSPORT &&
		    ep->svc == MGCP_EP_RESTARTING && !ep->disc)
			restart_retry_arm(gw);
	}
	if (reason == MGCP_TXN_DONE_OK || reason == MGCP_TXN_DONE_ABORT)
		return;

	/* 4.4.1 page 128: a NTFY that got no response is lost and not
	 * pending, and the endpoint stays in the notification state */
	if (txid == ep->ntfy_txid)
		ep->ntfy_txid = 0;
	if (reason == MGCP_TXN_DONE_TIMEOUT)
		disc_mark(gw, 1u << ep->index);
}

/* ------------------------------------------------------------ line events */

/*
 * A fax or modem tone on a G.711 stream switches it to the passthrough
 * profile, once per connection. A stream on another codec waits for the
 * call agent's MDCX.
 */
static void fax_switch(struct mgcp_ep *ep, uint8_t kind)
{
	const struct proto_host *h = ep->gw->host;
	struct hal_fax_profile fax;
	unsigned int i;

	for (i = 0; i < MGCP_CONN_MAX; i++) {
		struct mgcp_conn *c = &ep->conn[i];
		uint8_t codec;

		if (!c->used || !c->media_started || c->fax_applied)
			continue;
		codec = c->media.codec;
		if (codec != HAL_CODEC_PCMU && codec != HAL_CODEC_PCMA)
			continue;

		media_fax_profile_build(&c->sdp.committed.codec, &fax);
		if (h->media.fax_apply(h->media.priv, ep->index, c->stream,
				       &fax) != HAL_OK)
			continue;
		c->fax_applied = true;
		c->fax_kind = kind == MGCP_EVK_FT ? HAL_PASSTHROUGH_FAX :
						    HAL_PASSTHROUGH_MODEM;
	}
}

static void hook_take(struct mgcp_ep *ep, uint8_t state)
{
	struct mgcp_gw *gw = ep->gw;

	if (state == HAL_HOOK_SEIZE)
		ep->offhook = true;
	else if (state == HAL_HOOK_RELEASE)
		ep->offhook = false;

	/* an off-hook is local activity (4.4.6 and 4.4.7 step 3) */
	if (state != HAL_HOOK_SEIZE)
		return;
	restart_now(gw);
	if (ep->disc && mgcp_txn_disc_kick(gw->mgr, true))
		disc_rsip_send(gw);
}

void mgcp_ep_line_event(struct mgcp_gw *gw, unsigned int line,
			const struct proto_line_event *ev)
{
	struct mgcp_ep *ep;
	struct mgcp_obs o;

	if (line >= gw->cfg.lines)
		return;
	ep = &gw->ep[line];

	if (ev->type == PROTO_EV_HOOK)
		hook_take(ep, ev->u.hook.state);

	if (!mgcp_obs_of_line_event(ev, &o))
		return;

	if (o.kind == MGCP_EVK_FT || o.kind == MGCP_EVK_MT)
		fax_switch(ep, o.kind);

	/* a digit stops the timer T that runs without the digit map
	 * action (RFC 3660 page 14) */
	if (o.kind == MGCP_EVK_DIGIT && !ep->dial_timer)
		proto_timer_cancel(tenv(gw), &ep->digit_timer);

	ep_event(ep, &o);
	ep_post(ep);
}

void mgcp_ep_signal_result(struct mgcp_gw *gw, unsigned int line,
			   enum proto_signal sig, bool ok)
{
	if (line >= gw->cfg.lines || ok)
		return;

	sig_failed(&gw->ep[line], sig);
	ep_post(&gw->ep[line]);
}

void mgcp_ep_ring_failed(struct mgcp_gw *gw, unsigned int line)
{
	mgcp_ep_signal_result(gw, line, PROTO_SIG_RING, false);
}

void mgcp_ep_line_reset(struct mgcp_gw *gw, unsigned int line)
{
	struct mgcp_ep *ep;
	unsigned int i;

	if (line >= gw->cfg.lines)
		return;
	ep = &gw->ep[line];

	proto_timer_cancel(tenv(gw), &ep->digit_timer);
	for (i = 0; i < MGCP_SIG_RUN_MAX; i++) {
		if (ep->sig[i].used)
			sig_run_clear(ep, &ep->sig[i]);
	}
}

/* --------------------------------------------------------------- lifetime */

static void persist_take(struct mgcp_gw *gw, struct mgcp_ep *ep)
{
	struct mgcp_sig_iter it;
	struct mgcp_event ev;
	bool more;

	ep->npersist = 0;
	for (more = mgcp_sig_first(sip_str_cstr(gw->cfg.persistent), pkg_line,
				   &it, &ev);
	     more && ep->npersist < MGCP_REQ_MAX;
	     more = mgcp_sig_next(&it, &ev)) {
		if (mgcp_ev_pat_compile(gw->cfg.dialect, &ev,
					&ep->persist[ep->npersist]))
			continue;
		ep->npersist++;
	}
}

static void map_default_take(struct mgcp_ep *ep)
{
	const struct cfg_channel *ch = channel_of(ep);

	if (ch && dialplan_compile(ch->digit_map, &ep->dp)) {
		str_copy(ep->dmap, sizeof(ep->dmap), ch->digit_map);
		return;
	}

	dialplan_compile(DIALPLAN_DEFAULT_MAP, &ep->dp);
	str_copy(ep->dmap, sizeof(ep->dmap), DIALPLAN_DEFAULT_MAP);
}

int mgcp_ep_init(struct mgcp_gw *gw, const struct proto_host *host,
		 struct mgcp_txn_mgr *mgr, const struct mgcp_ep_env *env,
		 const struct mgcp_ep_cfg *cfg)
{
	unsigned int i;

	if (!gw || !host || !mgr || !mgr->env || !env || !cfg ||
	    cfg->lines > MGCP_EP_MAX)
		return -EINVAL;

	memset(gw, 0, sizeof(*gw));
	gw->host = host;
	gw->mgr = mgr;
	gw->env = *env;
	gw->cfg = *cfg;
	if (!mgcp_ep_name_expand(cfg->pattern, -1, cfg->host, gw->wild,
				 sizeof(gw->wild)))
		return -EINVAL;

	proto_timer_init(&gw->restart_timer, gw, TID_RESTART, restart_expire);

	for (i = 0; i < cfg->lines; i++) {
		struct mgcp_ep *ep = &gw->ep[i];

		ep->gw = gw;
		ep->index = i;
		if (!mgcp_ep_name_expand(cfg->pattern,
					 (int)(cfg->line_base + i), cfg->host,
					 ep->name, sizeof(ep->name)))
			return -EINVAL;
		str_copy(ep->entity, sizeof(ep->entity), cfg->entity);
		proto_timer_init(&ep->digit_timer, ep, TID_DIGIT,
				 digit_expire);
		ep_clean(ep);
		persist_take(gw, ep);
		map_default_take(ep);
		ep->svc = MGCP_EP_RESTARTING;
		ep->rm = MGCP_RM_RESTART;
		ep->state_last = ST_RESTARTING;
	}

	return 0;
}

void mgcp_ep_start(struct mgcp_gw *gw)
{
	uint32_t delay = restart_delay_draw(gw);

	gw->started = true;
	gw->restart_wait = true;
	proto_timer_arm(tenv(gw), &gw->restart_timer, delay);
	EP_TRACE(gw, "restart in %u ms", delay);
}

void mgcp_ep_stop_begin(struct mgcp_gw *gw, uint32_t delay_s)
{
	unsigned int i;
	uint32_t txid;

	gw->stopping = true;
	proto_timer_cancel(tenv(gw), &gw->restart_timer);
	gw->restart_wait = false;
	gw->restart_stop = false;

	for (i = 0; i < gw->cfg.lines; i++) {
		struct mgcp_ep *ep = &gw->ep[i];
		struct rsip_out r = {
			.owner = owner_of(ep),
			.endpoint = ep->name,
			.entity = ep->entity,
			.method = "graceful",
			.has_delay = true,
			.delay = delay_s,
		};

		if (ep->svc != MGCP_EP_IN_SERVICE || !ep_has_conn(ep))
			continue;
		ep->rm = MGCP_RM_GRACEFUL;
		rsip_send(gw, &r, &txid, NULL);
	}
}

void mgcp_ep_done(struct mgcp_gw *gw)
{
	unsigned int i, k;

	proto_timer_cancel(tenv(gw), &gw->restart_timer);
	for (i = 0; i < gw->cfg.lines; i++) {
		struct mgcp_ep *ep = &gw->ep[i];

		proto_timer_cancel(tenv(gw), &ep->digit_timer);
		for (k = 0; k < MGCP_SIG_RUN_MAX; k++) {
			if (ep->sig[k].used)
				sig_run_clear(ep, &ep->sig[k]);
		}
		/* the backend closed before this call, so only the ports go
		 * back */
		for (k = 0; k < MGCP_CONN_MAX; k++) {
			struct mgcp_conn *c = &ep->conn[k];

			if (c->used && c->port)
				gw->host->net_port_free(gw->host->ctx,
							c->port);
			memset(c, 0, sizeof(*c));
		}
	}
}
