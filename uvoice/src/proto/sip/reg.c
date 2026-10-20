// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Per account registration client. See sip/reg.h for the contracts.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "str.h"
#include "proto/sip/reg.h"

/*
 * The Contact ";transport=" parameter and its terminator. The prefix is 11
 * characters, so a token of 12 characters is the longest one this writes; a
 * longer one is refused and never truncated.
 */
#define REG_TRANSPORT_MAX	24

const char *sip_reg_state_name(enum sip_reg_state state)
{
	switch (state) {
	case SIP_REG_UNREGISTERED:
		return "unregistered";
	case SIP_REG_RESOLVING:
		return "resolving";
	case SIP_REG_TRYING:
		return "trying";
	case SIP_REG_REGISTERED:
		return "registered";
	case SIP_REG_REFRESHING:
		return "refreshing";
	case SIP_REG_BACKOFF:
		return "backoff";
	}

	return "?";
}

static void reg_trace(struct sip_reg *reg, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void reg_trace(struct sip_reg *reg, const char *fmt, ...)
{
	struct sip_txn_env *env = reg->env;
	va_list ap;

	if (!env->base.trace)
		return;

	va_start(ap, fmt);
	sip_txn_env_trace(env, fmt, ap, "reg %s %s: ",
			  reg->cfg.aor ? reg->cfg.aor : "?",
			  sip_reg_state_name((enum sip_reg_state)reg->state));
	va_end(ap);
}

static void state_set(struct sip_reg *reg, enum sip_reg_state state)
{
	if (reg->state == (uint8_t)state)
		return;

	reg->state = (uint8_t)state;
	reg_trace(reg, "entered");
}

static uint64_t reg_now(const struct sip_reg *reg)
{
	struct sip_txn_env *env = reg->env;

	return env->base.now_ms ? env->base.now_ms(&env->base) : 0;
}

static void attempt_start(struct sip_reg *reg);
static void reg_fail(struct sip_reg *reg, uint32_t code, uint32_t delay_s, bool credential);
static void rebind_giveup(struct sip_reg *reg);

/* the removal a Contact correction deferred, which no longer has an owner */
static void remove_pending_clear(struct sip_reg *reg)
{
	memset(&reg->remove_dst, 0, sizeof(reg->remove_dst));
	reg->remove_contact[0] = 0;
}

/*
 * The live binding is gone. binding_dst and binding_contact name the server
 * that granted it and the URI it was granted with, so both go with it: nothing
 * may read a binding no server holds. A removal the correction deferred was
 * owed by that binding and goes too, and so does the service route, which the
 * same server granted (RFC 3608).
 */
static void binding_drop(struct sip_reg *reg)
{
	reg->granted = 0;
	memset(&reg->binding_dst, 0, sizeof(reg->binding_dst));
	reg->binding_contact[0] = 0;
	route_hdrs_clear(&reg->service_route);
	remove_pending_clear(reg);
}

/* ----------------------------------------------------------------- targets */

unsigned int sip_reg_target_count(const struct sip_reg *reg)
{
	return reg ? reg->targets : 0;
}

unsigned int sip_reg_target_current(const struct sip_reg *reg)
{
	return reg ? reg->cur : 0;
}

const struct sip_reg_target *sip_reg_target_get(const struct sip_reg *reg, unsigned int idx)
{
	if (!reg || idx >= reg->targets)
		return NULL;

	return &reg->target[idx];
}

/*
 * Identity, not index and not name: two SRV records of one zone carry the same
 * owner name and different addresses, and one address serves several ports.
 * sip_dst_eq() compares the address and the port of the sockaddr.
 */
static bool target_find(const struct sip_reg *reg, const struct sip_txn_dst *dst,
			unsigned int *idx)
{
	unsigned int i;

	for (i = 0; i < reg->targets; i++) {
		if (!sip_dst_eq(&reg->target[i].dst, dst))
			continue;

		*idx = i;

		return true;
	}

	return false;
}

bool sip_reg_target_add(struct sip_reg *reg, const char *host, uint16_t port)
{
	return sip_reg_target_add_prio(reg, host, port, 0);
}

bool sip_reg_target_add_prio(struct sip_reg *reg, const char *host,
			     uint16_t port, uint16_t prio)
{
	struct sip_reg_target *t;
	struct sip_txn_dst dst;
	unsigned int idx;

	if (!reg)
		return false;

	port = port ? port : SIP_PORT_DEFAULT;
	if (!sip_dst_set(&dst, host, port))
		return false;

	/*
	 * One endpoint, one entry. A zone may carry two SRV records that name
	 * the same host and port, and each of them collects the same address,
	 * so an answer legitimately offers one identity twice. A second entry
	 * for it would take a share of the penalty state of the first across a
	 * reload, and a failover would "advance" from an endpoint to itself.
	 * The add is absorbed and reported as taken, so a caller that walks a
	 * resolver answer needs no duplicate test of its own. The entry keeps
	 * the lower priority, because one host under two records is reachable
	 * at its best one.
	 */
	if (target_find(reg, &dst, &idx)) {
		if (prio < reg->target[idx].prio)
			reg->target[idx].prio = prio;
		reg_trace(reg, "target %s:%u is already %u, absorbed", host, port, idx);
		return true;
	}

	if (reg->targets >= SIP_REG_TARGET_MAX)
		return false;

	t = &reg->target[reg->targets];
	memset(t, 0, sizeof(*t));
	t->dst = dst;
	t->prio = prio;

	reg->targets++;
	reg_trace(reg, "target %u added, %s:%u", reg->targets - 1, host, port);

	/* The account was waiting for exactly this. */
	if (reg->running && reg->state == SIP_REG_RESOLVING)
		attempt_start(reg);

	return true;
}

void sip_reg_targets_clear(struct sip_reg *reg)
{
	if (!reg)
		return;

	memset(reg->target, 0, sizeof(reg->target));
	reg->targets = 0;
	reg->cur = 0;
}

static struct sip_reg_target *target_cur(struct sip_reg *reg)
{
	if (!reg->targets)
		return NULL;
	if (reg->cur >= reg->targets)
		reg->cur = 0;

	return &reg->target[reg->cur];
}

/*
 * The target the attempt that just ended ran against, found by the identity
 * the attempt captured at its start. It is not the current one: a reload can
 * move the selection to another server while a REGISTER is in flight, and
 * neither the penalty nor the clear of that exchange may land on a target that
 * never carried it. A reload that dropped the attempt's server from the answer
 * leaves it owned by no entry at all, and NULL says so.
 */
static struct sip_reg_target *attempt_target(struct sip_reg *reg)
{
	unsigned int idx;

	if (!target_find(reg, &reg->attempt_dst, &idx))
		return NULL;

	return &reg->target[idx];
}

/*
 * The target the failure rule chooses, scanning forward from `from`: the first
 * one that is out of its penalty box, else the one whose penalty ends first,
 * because refusing to send at all is worse than an early retry.
 */
static unsigned int target_pick(const struct sip_reg *reg, unsigned int from)
{
	uint64_t now = reg_now(reg);
	unsigned int i, pick = from;

	for (i = 1; i <= reg->targets; i++) {
		unsigned int idx = (from + i) % reg->targets;

		if (reg->target[idx].penalty_until > now)
			continue;

		return idx;
	}

	for (i = 0; i < reg->targets; i++) {
		if (i && reg->target[i].penalty_until >= reg->target[pick].penalty_until)
			continue;

		pick = i;
	}

	return pick;
}

/*
 * Sticky selection: the current target keeps the traffic while it answers.
 * This runs on a failure only.
 */
static void target_advance(struct sip_reg *reg)
{
	unsigned int pick;

	if (reg->targets < 2)
		return;

	pick = target_pick(reg, reg->cur);
	if (pick == reg->cur)
		return;

	reg->stat.switched++;
	reg_trace(reg, "target %u to %u", reg->cur, pick);
	reg->cur = pick;
}

static void target_penalize_one(struct sip_reg *reg, struct sip_reg_target *t)
{
	t->failures++;
	t->penalty_until = reg_now(reg) + (uint64_t)reg->cfg.penalty_s * 1000;
}

static void target_penalize(struct sip_reg *reg)
{
	struct sip_reg_target *t = attempt_target(reg);

	if (!t)
		return;

	target_penalize_one(reg, t);

	/* The failover belongs to the target the line uses now. A failure of a
	 * target a reload moved away from penalizes it and nothing else. */
	if (t == &reg->target[reg->cur])
		target_advance(reg);
}

static void target_clear(struct sip_reg *reg)
{
	struct sip_reg_target *t = attempt_target(reg);

	if (!t)
		return;

	t->failures = 0;
	t->penalty_until = 0;
}

static bool stash_find(const struct sip_reg *reg, const struct sip_txn_dst *dst)
{
	unsigned int j;

	for (j = 0; j < reg->stashed; j++) {
		if (sip_dst_eq(&reg->stash[j].dst, dst))
			return true;
	}

	return false;
}

/* The penalty box the stash holds for this target, when it held one. */
static void target_penalty_take(struct sip_reg *reg, unsigned int idx)
{
	unsigned int j;

	for (j = 0; j < reg->stashed; j++) {
		if (!sip_dst_eq(&reg->target[idx].dst, &reg->stash[j].dst))
			continue;

		reg->target[idx].penalty_until = reg->stash[j].penalty_until;
		reg->target[idx].failures = reg->stash[j].failures;

		return;
	}
}

void sip_reg_targets_reload(struct sip_reg *reg)
{
	if (!reg)
		return;

	/* A second reload inside one window would stash the empty list the
	 * first one left and lose the state the pair exists to carry. */
	if (!reg->reloading) {
		memcpy(reg->stash, reg->target, sizeof(reg->stash));
		reg->stashed = reg->targets;
		reg->stash_cur = reg->cur < reg->targets ? reg->cur : 0;
		reg->reloading = true;
	}

	sip_reg_targets_clear(reg);
}

/*
 * The lowest priority that holds a target with an empty penalty box. False
 * when every target is penalized, which is the case where the sticky selection
 * keeps the line: refusing to send is worse than an early retry.
 */
static bool reload_best_prio(const struct sip_reg *reg, uint16_t *best)
{
	uint64_t now = reg_now(reg);
	unsigned int i;
	bool found = false;

	for (i = 0; i < reg->targets; i++) {
		if (reg->target[i].penalty_until > now)
			continue;
		if (found && reg->target[i].prio >= *best)
			continue;

		*best = reg->target[i].prio;
		found = true;
	}

	return found;
}

/*
 * The sticky selection keeps the line only inside the best priority group. A
 * selection that is penalized or outranked hands the line to the first
 * unpenalized target of that group, in list order: the fresh RFC 2782 draw and
 * the family sort decide the order inside one priority.
 */
static void reload_select(struct sip_reg *reg, unsigned int idx)
{
	const struct sip_reg_target *cur = &reg->target[idx];
	uint64_t now = reg_now(reg);
	uint16_t best = 0;
	unsigned int i;

	reg->cur = idx;
	if (!reload_best_prio(reg, &best))
		return;
	if (cur->penalty_until <= now && cur->prio == best)
		return;

	for (i = 0; i < reg->targets; i++) {
		const struct sip_reg_target *t = &reg->target[i];

		if (t->penalty_until > now || t->prio != best)
			continue;

		reg_trace(reg,
			  "reload: priority moved the selection from %u to %u",
			  idx, i);
		reg->cur = i;

		return;
	}
}

bool sip_reg_targets_reload_end(struct sip_reg *reg)
{
	unsigned int i, idx = 0;
	bool kept, changed, moved;

	if (!reg || !reg->reloading)
		return false;

	reg->reloading = false;

	/* Both lists are duplicate free, so equal counts plus every new
	 * endpoint found in the stash is set equality. */
	changed = reg->targets != reg->stashed;
	for (i = 0; !changed && i < reg->targets; i++)
		changed = !stash_find(reg, &reg->target[i].dst);

	for (i = 0; i < reg->targets; i++)
		target_penalty_take(reg, i);

	kept = reg->stashed && target_find(reg, &reg->stash[reg->stash_cur].dst, &idx);
	if (kept)
		reload_select(reg, idx);
	else
		/* Index 0 is the fresh RFC 2782 draw, so taking it would be
		 * the re-randomization this pair exists to stop.
		 * target_pick() walks the list from index 0, and the answer
		 * is in ascending priority order, so it prefers the best
		 * group by itself. */
		reg->cur = reg->targets ? target_pick(reg, reg->targets - 1) : 0;

	moved = false;
	if (reg->stashed && reg->targets)
		moved = !sip_dst_eq(&reg->stash[reg->stash_cur].dst,
				    &reg->target[reg->cur].dst);

	reg->stashed = 0;
	reg_trace(reg, "reload: %u target(s), current %u, %s%s", reg->targets, reg->cur,
		  kept ? "kept" : "reselected", changed ? ", the set moved" : "");

	return changed || moved;
}

/* ------------------------------------------------------------- identifiers */

static bool cfg_ok(const struct sip_reg *reg)
{
	return reg->cfg.aor && reg->cfg.registrar && reg->cfg.local_host;
}

static void ident_make(struct sip_reg *reg)
{
	const char *host = reg->cfg.local_host ? reg->cfg.local_host : "uvoice";

	snprintf(reg->call_id, sizeof(reg->call_id), "%08x%08x@%s", sip_txn_rand(reg->mgr),
		 sip_txn_rand(reg->mgr), host);
	snprintf(reg->from_tag, sizeof(reg->from_tag), "%08x%04x", sip_txn_rand(reg->mgr),
		 sip_txn_rand(reg->mgr) & 0xffffu);
}

/*
 * The transport parameter of the Contact URI. It belongs inside the angle
 * brackets hdr_addr_write() emits, so it is part of the URI string and not a
 * header parameter (RFC 3261 19.1.1). sip/reg.h hands the token in its upper
 * case Via form and makes the lower case spelling this writer's job.
 */
static bool transport_param(const char *token, char *out, size_t olen)
{
	int i, n;

	out[0] = 0;
	if (!token)
		return true;

	n = snprintf(out, olen, ";transport=%s", token);
	if (n < 0 || (size_t)n >= olen)
		return false;

	for (i = 0; i < n; i++)
		out[i] = (char)tolower((unsigned char)out[i]);

	return true;
}

/*
 * The standing URI survives a refusal, so the build runs in a local buffer and
 * reaches reg->contact_uri only once it fits. nat_check() leaves the Contact
 * alone rather than reverting it, and a partial write would leave a truncated
 * address in the buffer with contact_dirty false: resp_ok() would then report
 * the account registered on a corrupt binding.
 */
static bool contact_build(struct sip_reg *reg)
{
	const char *user = reg->cfg.contact_user;
	char uri[SIP_REG_URI_MAX];
	char param[REG_TRANSPORT_MAX];
	bool brk = sip_host_bracket_needed(reg->contact_host);
	int n;

	if (!transport_param(reg->cfg.transport, param, sizeof(param))) {
		reg_trace(reg, "contact refused, the transport token does not fit");
		return false;
	}

	if (user && *user)
		n = snprintf(uri, sizeof(uri), "sip:%s@%s%s%s:%u%s",
			     user, brk ? "[" : "", reg->contact_host, brk ? "]" : "",
			     reg->contact_port, param);
	else
		n = snprintf(uri, sizeof(uri), "sip:%s%s%s:%u%s",
			     brk ? "[" : "", reg->contact_host, brk ? "]" : "",
			     reg->contact_port, param);

	if (n <= 0 || (size_t)n >= sizeof(uri)) {
		reg_trace(reg, "contact refused, %s:%u does not fit", reg->contact_host,
			  reg->contact_port);
		return false;
	}

	memcpy(reg->contact_uri, uri, (size_t)n + 1);

	return true;
}

/*
 * RFC 3261 section 10.3 step 7 refuses a REGISTER that refreshes and removes
 * one Contact, and a registrar that accepts it removes the binding the line
 * needs. Each site that changes the Contact or fills the removal slot runs this
 * behind the change. True while the slot names a binding to remove.
 */
static bool old_contact_settle(struct sip_reg *reg)
{
	if (!reg->old_contact_uri[0])
		return false;

	if (strcmp(reg->old_contact_uri, reg->contact_uri))
		return true;

	reg->old_contact_uri[0] = 0;

	return false;
}

/*
 * Answer the address family for an IPv4 or IPv6 literal.
 * Accept brackets around cfg.local_host for an IPv6 literal.
 * Answer AF_UNSPEC for a name, a v4-mapped literal or text that does not parse.
 */
static int host_family(const char *host)
{
	struct sip_str text = sip_str_cstr(host);
	struct sip_txn_dst dst;
	const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&dst.sa;

	if (text.len >= 2 && text.p[0] == '[' && text.p[text.len - 1] == ']') {
		text.p++;
		text.len -= 2;
	}

	if (!sip_dst_set_str(&dst, text, 0))
		return AF_UNSPEC;
	if (dst.sa.ss_family == AF_INET6 &&
	    IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr))
		return AF_UNSPEC;

	return dst.sa.ss_family;
}

/*
 * RFC 3581: the registrar tells us how it saw us. The Contact must carry that
 * address, or the binding points at a private address the carrier cannot
 * reach. A response that carries neither parameter leaves the Contact alone
 * rather than reverting it, because reverting would flap the binding between
 * two servers that disagree.
 */
static void nat_check(struct sip_reg *reg, const struct sip_msg *msg)
{
	char host[SIP_REG_HOST_MAX];
	struct sip_via via;
	uint16_t port = reg->contact_port;
	bool fits = true;
	int family;

	if (!hdr_via_top(msg, &via))
		return;

	str_copy(host, sizeof(host), reg->contact_host);
	if ((via.flags & SIP_VIA_F_RPORT_VAL) && via.rport && via.rport <= 65535)
		port = (uint16_t)via.rport;
	if (via.flags & SIP_VIA_F_RECEIVED) {
		if (via.received.len >= 2 && via.received.p[0] == '[' &&
		    via.received.p[via.received.len - 1] == ']') {
			via.received.p++;
			via.received.len -= 2;
		}
		fits = sip_str_copy(host, sizeof(host), via.received);
	}

	/*
	 * RFC 3261 20.42 requires a bare literal in received.
	 * We tolerate brackets around that literal.
	 * The gate rejects the other family, a v4-mapped literal or a value
	 * that does not parse.
	 * It drops the whole observation because received and rport describe
	 * one mapping (RFC 3581 section 4).
	 * A port without its address would name a binding nobody listens on.
	 * The gate is off when the local host is a name because there is no
	 * address family to compare.
	 */
	family = host_family(reg->cfg.local_host);
	if ((via.flags & SIP_VIA_F_RECEIVED) && family != AF_UNSPEC &&
	    (!fits || host_family(host) != family)) {
		reg_trace(reg, "received %s is not a bare literal in the "
			  "family of %s, kept %s port %u", host,
			  reg->cfg.local_host,
			  reg->contact_host, reg->contact_port);
		return;
	}
	/* The name-local-host path also needs a received value that fits. */
	if (!fits)
		return;

	if (port == reg->contact_port && !strcmp(host, reg->contact_host))
		return;

	reg_trace(reg, "contact %s:%u to %s:%u", reg->contact_host, reg->contact_port, host,
		  port);
	str_copy(reg->contact_host, sizeof(reg->contact_host), host);
	reg->contact_port = port;
	reg->stat.contact_rebuilt++;
	reg->contact_dirty = contact_build(reg);

	/* Behind NAT the mapping the registrar reports after a local host move
	 * is often the one it reported before it, which the slot names. */
	if (reg->old_contact_uri[0] && !old_contact_settle(reg))
		reg_trace(reg, "the mapping is the removal Contact, "
			  "nothing to remove");
}

/* ------------------------------------------------------------------- wire */

static uint32_t expires_want(const struct sip_reg *reg)
{
	uint32_t want = reg->cfg.expires;

	/* A registrar that answered 423 once keeps its minimum for every later
	 * refresh; asking for the old value again would earn the same 423. */
	return reg->min_expires > want ? reg->min_expires : want;
}

static void cred_write(struct sip_reg *reg, struct sip_buf *b, enum sip_auth_kind kind)
{
	enum sip_auth_status status;

	status = sip_auth_cred_write(b, reg->auth, kind, reg->cfg.auth_user,
				     reg->cfg.auth_pass, "REGISTER",
				     reg->cfg.registrar);
	if (status != SIP_AUTH_OK)
		reg_trace(reg, "%s credentials refused (%s)", sip_auth_kind_name(kind),
			  sip_auth_status_name(status));
}

static bool wire_build(struct sip_reg *reg)
{
	struct sip_buf b;
	unsigned int k;

	if (!sip_branch_new(reg->mgr, reg->branch, sizeof(reg->branch)))
		return false;

rebuild:
	sip_buf_init(&b, reg->wire, sizeof(reg->wire));

	msg_req_line_write(&b, "REGISTER", reg->cfg.registrar);
	hdr_via_write(&b, reg->cfg.transport, reg->cfg.local_host,
		      reg->cfg.local_port, reg->branch, true);
	hdr_write(&b, SIP_HDR_MAX_FORWARDS, "70");
	hdr_addr_tag_write(&b, SIP_HDR_FROM, reg->cfg.display, reg->cfg.aor,
			   reg->from_tag);
	hdr_addr_write(&b, SIP_HDR_TO, NULL, reg->cfg.aor, NULL);
	hdr_write(&b, SIP_HDR_CALL_ID, "%s", reg->call_id);
	hdr_cseq_write(&b, reg->cseq, "REGISTER");
	hdr_addr_write(&b, SIP_HDR_CONTACT, NULL, reg->contact_uri, NULL);
	if (reg->old_contact_uri[0])
		hdr_addr_write(&b, SIP_HDR_CONTACT, NULL, reg->old_contact_uri,
			       ";expires=0");
	hdr_write(&b, SIP_HDR_EXPIRES, "%u", reg->req_expires);
	if (reg->cfg.user_agent)
		hdr_write(&b, SIP_HDR_USER_AGENT, "%s", reg->cfg.user_agent);

	for (k = 0; k < SIP_AUTH_KIND_MAX; k++)
		cred_write(reg, &b, (enum sip_auth_kind)k);

	msg_body_write(&b, NULL, NULL, 0);
	if (!sip_buf_ok(&b) && reg->old_contact_uri[0]) {
		reg->old_contact_uri[0] = 0;
		reg_trace(reg, "the removal Contact does not fit, "
			  "the old binding expires on its own");
		goto rebuild;
	}
	if (!sip_buf_ok(&b))
		return false;

	reg->wire_len = sip_buf_len(&b);

	return true;
}

/* ----------------------------------------------------------------- timers */

static void timer_stop(struct sip_reg *reg)
{
	sip_timer_cancel(reg->env, &reg->timer);
}

enum sip_reg_timer sip_reg_timer_kind(const struct sip_reg *reg)
{
	if (!reg->timer.armed)
		return SIP_REG_T_NONE;

	if (reg->timer.id == SIP_TIMER_REG_REFRESH)
		return SIP_REG_T_REFRESH;

	return SIP_REG_T_RETRY;
}

/*
 * A delay arrives in seconds from a Retry-After header or from an Expires the
 * registrar chose, and it goes to the timer in milliseconds. Without this
 * clamp a Retry-After of a few million seconds would overflow the conversion
 * and fire the timer almost at once, which is the opposite of what the header
 * asked for.
 */
static uint32_t delay_clamp(uint32_t secs)
{
	return secs > SIP_REG_DELAY_MAX ? SIP_REG_DELAY_MAX : secs;
}

static enum sip_timer_id timer_id(enum sip_reg_timer kind)
{
	return kind == SIP_REG_T_REFRESH ? SIP_TIMER_REG_REFRESH : SIP_TIMER_REG_RETRY;
}

static void timer_arm(struct sip_reg *reg, enum sip_reg_timer kind, uint32_t secs)
{
	secs = delay_clamp(secs);
	reg->timer.id = (uint8_t)timer_id(kind);
	sip_timer_arm(reg->env, &reg->timer, secs * 1000);

	/* The rollback of a failed move re-arms the refresh of the binding it
	 * keeps, so that deadline has to survive the move. */
	if (kind == SIP_REG_T_REFRESH)
		reg->refresh_due_ms = reg_now(reg) + (uint64_t)secs * 1000;

	reg_trace(reg, "timer %s armed %u s", sip_timer_name(timer_id(kind)), secs);
}

/* ------------------------------------------------------------- attempts */

static void txn_drop(struct sip_reg *reg)
{
	struct sip_txn *txn;

	if (reg->txn == SIP_HANDLE_NONE)
		return;

	txn = sip_txn_get(reg->mgr, reg->txn);
	reg->txn = SIP_HANDLE_NONE;
	if (txn)
		sip_txn_abort(txn);
}

/*
 * Every send of one exchange goes to attempt_dst, the destination the attempt
 * started against, never to the live selection: a reload under a 401 must not
 * re-aim the credential resend, because one server's nonce answered to
 * another reads as a wrong password there and earns the credential backoff.
 */
static bool attempt_send(struct sip_reg *reg)
{
	char host[SIP_REG_HOST_MAX];
	struct sip_txn *txn;

	if (!reg->attempt_dst.sa_len)
		return false;

	/* RFC 3261 10.2: one Call-ID, one From tag, a CSeq that only grows. */
	reg->cseq++;
	if (!wire_build(reg))
		return false;

	txn = sip_txn_client_start(reg->mgr, SIP_METHOD_REGISTER, sip_str_cstr(reg->branch),
				   reg->wire, reg->wire_len, &reg->attempt_dst, reg->owner);
	if (!txn)
		return false;

	reg->txn = sip_txn_handle(txn);
	reg->contact_dirty = false;
	reg->old_contact_sent = reg->old_contact_uri[0] != 0;
	reg->stat.sent++;

	/* The destination, not the index: a removal runs against the server
	 * the binding left, and a resend keeps its own server across a reload,
	 * so the selection names neither of them. */
	host[0] = 0;
	sip_dst_host(&reg->attempt_dst, host, sizeof(host));
	reg_trace(reg, "REGISTER cseq %u expires %u to %s:%u", reg->cseq,
		  reg->req_expires, host, sip_dst_port(&reg->attempt_dst));

	return true;
}

/* a retry inside one exchange: the auth and 423 counters keep their values */
static void attempt_resend(struct sip_reg *reg)
{
	if (attempt_send(reg))
		return;

	reg_fail(reg, 503, 0, false);
}

/*
 * One REGISTER per account at a time. txn_drop() clears reg->txn before it
 * aborts, so a late response or completion of the dropped transaction is
 * discarded by the guards in sip_reg_txn_resp() and sip_reg_txn_done().
 * A refresh after a local host move drops a live transaction on purpose:
 * it carried the old Via and Contact.
 */
static void attempt_prepare(struct sip_reg *reg)
{
	timer_stop(reg);
	txn_drop(reg);
	memset(reg->auth_retry, 0, sizeof(reg->auth_retry));
	reg->min_expires_retry = 0;
	/*
	 * A fresh exchange abandons a removal in flight; the old binding lapses
	 * on its own. The move flag goes too, because the exchange this
	 * prepares decides for itself whether it is one, and an empty target
	 * list leaves it with no exchange at all.
	 */
	reg->removing = false;
	reg->removal_slot = false;
	reg->moving = false;
	reg->req_expires = reg->unregistering ? 0 : expires_want(reg);
}

static void attempt_start(struct sip_reg *reg)
{
	/* the Contact correction belongs to the exchange this call starts */
	bool rebinding = reg->rebinding;

	reg->rebinding = false;
	attempt_prepare(reg);

	/* A fresh exchange runs against the live selection, which is what a
	 * pending retarget asked for. */
	reg->retarget_pending = false;

	if (!reg->targets) {
		state_set(reg, SIP_REG_RESOLVING);
		reg_trace(reg, "no target, waiting for the resolver");
		return;
	}

	/*
	 * The capture happens once per exchange, before the first send. Every
	 * send of the exchange goes there, and its outcome lands on the entry
	 * that carries the same identity. Two exchanges name the server that
	 * holds the binding and not the selection: an unregister, because that
	 * is the server it asks to remove the binding, and the correction of a
	 * Contact the answer moved, because it is that same binding it
	 * corrects.
	 */
	if ((reg->unregistering || rebinding) && reg->binding_dst.sa_len)
		reg->attempt_dst = reg->binding_dst;
	else
		reg->attempt_dst = target_cur(reg)->dst;

	/*
	 * A REGISTER toward a server other than the binding's, with a binding
	 * live, is a move. The binding stays where it is until this exchange
	 * succeeds, so a failure rolls back to it.
	 */
	reg->moving = reg->granted && !reg->unregistering &&
		      !sip_dst_eq(&reg->attempt_dst, &reg->binding_dst);

	if (reg->unregistering)
		state_set(reg, SIP_REG_TRYING);
	else
		state_set(reg, reg->granted ? SIP_REG_REFRESHING : SIP_REG_TRYING);

	attempt_resend(reg);
}

/* ------------------------------------------------------------- completion */

/*
 * The owner is allowed to call sip_reg_stop() from inside this callback, which
 * is what a shutdown path does. The guard makes that call a no-op instead of a
 * second callback: without it the pair recurses until the stack is gone.
 */
static void unregistered_notify(struct sip_reg *reg)
{
	if (reg->notifying || !reg->cb.unregistered)
		return;

	reg->notifying = true;
	reg->cb.unregistered(reg, reg->priv);
	reg->notifying = false;
}

static void stop_finish(struct sip_reg *reg)
{
	/* Report the transition only. A stop of an account that is already
	 * stopped changes nothing and owes the owner nothing. */
	bool notify = reg->running || reg->state != SIP_REG_UNREGISTERED;

	reg->running = false;
	reg->unregistering = false;
	reg->retarget_pending = false;
	reg->moving = false;
	reg->removing = false;
	reg->removal_slot = false;
	reg->rebinding = false;
	binding_drop(reg);
	timer_stop(reg);
	state_set(reg, SIP_REG_UNREGISTERED);

	if (notify)
		unregistered_notify(reg);
}

static uint32_t backoff_secs(const struct sip_reg *reg)
{
	uint32_t base = reg->cfg.retry_s;
	uint32_t shift = reg->fail_run ? reg->fail_run - 1 : 0;
	uint64_t v;

	if (shift > SIP_REG_BACKOFF_SHIFT)
		shift = SIP_REG_BACKOFF_SHIFT;

	v = (uint64_t)base << shift;

	return v > SIP_REG_BACKOFF_MAX ? SIP_REG_BACKOFF_MAX : (uint32_t)v;
}

/*
 * The Expires 0 to the server the binding left has failed. It costs the line
 * nothing: the old binding lapses on its own, so the server is penalized and
 * the registration the line holds now keeps its state and its refresh.
 */
static void removal_finish(struct sip_reg *reg)
{
	reg->removing = false;

	/* The slot is the removal's own only when the removal filled it; a
	 * local host move that owns it keeps it for its next REGISTER. */
	if (reg->removal_slot) {
		reg->old_contact_uri[0] = 0;
		reg->removal_slot = false;
	}

	/* The retarget rule reads attempt_dst as the destination of the last
	 * registration attempt, and a removal is not one. */
	reg->attempt_dst = reg->binding_dst;
}

static void removal_fail(struct sip_reg *reg, uint32_t code)
{
	struct sip_reg_target *t = attempt_target(reg);

	if (t)
		target_penalize_one(reg, t);

	removal_finish(reg);
	reg_trace(reg, "the removal of the old binding failed with %u", code);
}

/*
 * A move has failed. The binding at the old server is still live, so the line
 * goes back to it and reports nothing: the failure ladder, the retry delay and
 * the owner's view all belong to a registration that never went down. False
 * when the old server left the answer, which leaves nothing to roll back to
 * and makes the failure an ordinary one.
 */
static bool move_fail(struct sip_reg *reg, uint32_t code)
{
	struct sip_reg_target *t = attempt_target(reg);
	uint64_t now = reg_now(reg);
	unsigned int idx;
	uint32_t secs;

	reg->moving = false;
	if (!target_find(reg, &reg->binding_dst, &idx))
		return false;

	if (t)
		target_penalize_one(reg, t);

	/*
	 * The cached nonce belongs to the new server. The old one challenges
	 * again at the refresh, which costs one exchange and cannot read as a
	 * wrong password there.
	 */
	sip_auth_state_reset(&reg->auth[SIP_AUTH_WWW]);
	sip_auth_state_reset(&reg->auth[SIP_AUTH_PROXY]);

	reg->cur = idx;
	reg->attempt_dst = reg->binding_dst;
	state_set(reg, SIP_REG_REGISTERED);

	secs = reg->refresh_due_ms > now ?
	       (uint32_t)((reg->refresh_due_ms - now) / 1000) : 0;
	if (secs < SIP_REG_REFRESH_FLOOR)
		secs = SIP_REG_REFRESH_FLOOR;
	timer_arm(reg, SIP_REG_T_REFRESH, secs);

	reg_trace(reg, "the move failed with %u, target %u keeps the binding",
		  code, idx);

	/*
	 * Last, with the state and the refresh already restored: an owner that
	 * followed the new server into another family moves back from here and
	 * may send, and the caller does nothing after this that such a send
	 * could invalidate.
	 */
	if (reg->cb.rolled_back)
		reg->cb.rolled_back(reg, reg->priv);

	return true;
}

/*
 * The removal is best effort. A registrar that refuses a REGISTER with two
 * Contacts refuses each retry of it the same way, so the retry carries the new
 * Contact alone and the old binding expires at the registrar.
 */
static void old_contact_giveup(struct sip_reg *reg, uint32_t code)
{
	reg->old_contact_uri[0] = 0;
	reg_trace(reg, "%u to a REGISTER with a removal Contact, "
		  "the old binding expires on its own", code);
}

static void reg_fail(struct sip_reg *reg, uint32_t code, uint32_t delay_s, bool credential)
{
	/* A delay that arrived with the failure is one the server named, and
	 * the credential backoff protects the account; the ladder is ours. */
	bool fixed = credential || delay_s;

	/* The refusal belongs to this failure alone: the rollback below lets
	 * the owner send, and a failure of that send is not an answer. */
	bool refused = reg->old_contact_refused;

	reg->txn = SIP_HANDLE_NONE;
	reg->old_contact_refused = false;

	if (reg->removing) {
		removal_fail(reg, code);
		return;
	}

	/* A move that rolls back keeps the slot: the server that refused is not
	 * the one that holds the binding, and the owner settles the slot when it
	 * follows the selection back. */
	if (reg->moving && move_fail(reg, code))
		return;

	if (refused)
		old_contact_giveup(reg, code);

	reg->stat.failed++;
	reg->fail_run++;
	binding_drop(reg);
	target_penalize(reg);

	/*
	 * A rejected credential starts the next exchange from scratch: the
	 * cached nonce is what the registrar refused, and sending it again is
	 * how an account walks into a lockout.
	 */
	if (credential) {
		sip_auth_state_reset(&reg->auth[SIP_AUTH_WWW]);
		sip_auth_state_reset(&reg->auth[SIP_AUTH_PROXY]);
	}

	if (reg->unregistering) {
		reg_trace(reg, "unregister failed with %u", code);
		stop_finish(reg);
		return;
	}

	if (!reg->running) {
		state_set(reg, SIP_REG_UNREGISTERED);
		return;
	}

	if (!delay_s)
		delay_s = credential ? SIP_REG_BACKOFF_AUTH : backoff_secs(reg);

	delay_s = delay_clamp(delay_s);

	if (!reg->targets) {
		state_set(reg, SIP_REG_RESOLVING);
		delay_s = 0;
	} else {
		state_set(reg, SIP_REG_BACKOFF);
		timer_arm(reg, SIP_REG_T_RETRY, delay_s);
	}

	reg->retry_fixed = fixed;
	reg->next_retry_s = delay_s;
	reg_trace(reg, "failed with %u, next attempt in %u s", code, delay_s);
	if (reg->cb.failed)
		reg->cb.failed(reg, code, delay_s, reg->priv);
}

/*
 * The address the registrar reports keeps moving, so every answer would start
 * another REGISTER with no delay between them. The chase stops here: the
 * account goes down and the ordinary retry timer decides when to try again.
 * The target is not penalized, because it answered every time; what failed is
 * the path in between.
 */
static void rebind_giveup(struct sip_reg *reg)
{
	uint32_t delay;

	binding_drop(reg);
	reg->fail_run++;
	reg->stat.rebind_capped++;
	delay = backoff_secs(reg);

	state_set(reg, SIP_REG_BACKOFF);
	timer_arm(reg, SIP_REG_T_RETRY, delay);
	reg->retry_fixed = false;
	reg->next_retry_s = delay;
	reg_trace(reg, "contact moved %u times in a row, backing off %u s", reg->rebind_run,
		  delay);

	if (reg->cb.failed)
		reg->cb.failed(reg, 200, delay, reg->priv);
}

static void timer_fire(struct sip_timer *t)
{
	struct sip_reg *reg = t->owner;

	if (!reg->running)
		return;

	attempt_start(reg);
}

/* ---------------------------------------------------------------- refresh */

uint32_t sip_reg_refresh_secs(uint32_t expires)
{
	uint32_t half = expires / 2;
	uint32_t lead = expires > SIP_REG_REFRESH_LEAD ? expires - SIP_REG_REFRESH_LEAD : 0;
	uint32_t v = half < lead ? half : lead;

	if (v < SIP_REG_REFRESH_MIN)
		v = SIP_REG_REFRESH_MIN;

	if (v < expires)
		return v;

	/*
	 * The 15 s floor lands at or past the expiry, so honouring it would
	 * refresh a binding that has already lapsed. Half the granted expiry
	 * refreshes inside it instead, and SIP_REG_REFRESH_FLOOR keeps a
	 * nonsense expiry from turning into a timer that fires at once.
	 */
	return half > SIP_REG_REFRESH_FLOOR ? half : SIP_REG_REFRESH_FLOOR;
}

/*
 * Our own binding among the Contact headers of the 200 (RFC 3261 10.2.4). The
 * registrar returns every binding of the address of record, and the expiry of
 * somebody else's phone is not ours to refresh on.
 */
static bool contact_is_ours(const struct sip_reg *reg, const struct sip_addr *a)
{
	const char *user = reg->cfg.contact_user;
	uint32_t port;

	if (a->flags & SIP_ADDR_F_STAR)
		return false;
	if (!a->uri.host.len || !sip_str_eq_ci(a->uri.host, reg->contact_host))
		return false;

	port = (a->uri.flags & SIP_URI_F_PORT) ? a->uri.port : SIP_PORT_DEFAULT;
	if (port != reg->contact_port)
		return false;

	if (!user || !*user)
		return true;

	return sip_str_eq(a->uri.user, user);
}

static uint32_t accepted_expires(struct sip_reg *reg, const struct sip_msg *msg)
{
	struct sip_val_iter it;
	struct sip_addr a;
	uint32_t v;

	if (hdr_addr_first(msg, SIP_HDR_CONTACT, &it, &a)) {
		do {
			if (!contact_is_ours(reg, &a))
				continue;
			if (a.flags & SIP_ADDR_F_EXPIRES)
				return a.expires;

			break;
		} while (hdr_addr_next(&it, &a));
	}

	if (hdr_uint_get(msg, SIP_HDR_EXPIRES, &v))
		return v;

	return reg->req_expires;
}

/* --------------------------------------------------------------- responses */

/*
 * The Expires 0 that takes the old binding away, sent once the new server
 * granted one. It rides the same Call-ID and From tag with a grown CSeq, so
 * both registrars see one registration (RFC 3261 10.2). A family the line no
 * longer serves has no transport to reach the old server on, and that binding
 * lapses on its own instead.
 *
 * A registrar holds one binding per Contact, so the request has to name the URI
 * the old server granted. When that differs from the current one the removal
 * slot carries it as a second Contact with expires=0 (RFC 3261 10.2.2), which
 * is the same mechanism a local host move uses; a slot that move already owns
 * names the very URI the old server holds and is left alone.
 */
static void binding_remove(struct sip_reg *reg, const struct sip_txn_dst *old,
			   const char *old_contact)
{
	if (!reg->cfg.move_unregister || !old->sa_len)
		return;
	if (old->sa.ss_family != reg->binding_dst.sa.ss_family)
		return;

	reg->removing = true;
	if (!reg->old_contact_uri[0]) {
		memcpy(reg->old_contact_uri, old_contact,
		       sizeof(reg->old_contact_uri));
		reg->removal_slot = old_contact_settle(reg);
	}

	reg->attempt_dst = *old;
	reg->req_expires = 0;
	memset(reg->auth_retry, 0, sizeof(reg->auth_retry));
	reg->min_expires_retry = 0;
	reg_trace(reg, "removing the binding the move left behind");
	attempt_resend(reg);
}

/*
 * The route the registrar grants replaces the one it granted before, and a 200
 * without the header clears it (RFC 3608 section 6.1). A refused route leaves
 * the line with none and one trace line, because a partial route reaches the
 * wrong proxy and the call still works without any.
 */
static void service_route_take(struct sip_reg *reg, const struct sip_msg *msg)
{
	if (!route_hdrs_take(&reg->service_route, msg, SIP_HDR_SERVICE_ROUTE)) {
		reg_trace(reg, "service route refused: a strict first hop, a "
			  "control byte, or a route that does not fit");
		return;
	}

	if (reg->service_route.hops)
		reg_trace(reg, "service route stored, %u hops",
			  reg->service_route.hops);
}

static void resp_ok(struct sip_reg *reg, const struct sip_msg *msg)
{
	char old_contact[SIP_REG_URI_MAX];
	struct sip_txn_dst old;
	uint32_t expires;
	bool moved;

	/*
	 * The removal owes the owner nothing: the binding it took away is the
	 * one the move replaced. Everything below belongs to a registration,
	 * and accepted_expires() answers 0 for a request that asked for 0.
	 */
	if (reg->removing) {
		removal_finish(reg);
		reg_trace(reg, "old binding removed");
		return;
	}

	expires = accepted_expires(reg, msg);

	reg->stat.ok++;
	/* A dirty Contact means the answered request predates the move. */
	if (!reg->contact_dirty)
		reg->old_contact_uri[0] = 0;
	reg->fail_run = 0;
	reg->next_retry_s = 0;
	target_clear(reg);
	sip_auth_state_accept(&reg->auth[SIP_AUTH_WWW]);
	sip_auth_state_accept(&reg->auth[SIP_AUTH_PROXY]);

	if (reg->unregistering) {
		reg_trace(reg, "unregistered");
		stop_finish(reg);
		return;
	}

	/*
	 * Expiry 0 in a 200 to a REGISTER that asked for more is the registrar
	 * removing the binding. There is nothing to refresh, so the account
	 * goes down and the retry timer decides when to ask again.
	 */
	if (!expires) {
		/* The new server of a move granted nothing, so the move did
		 * not happen and the old binding is still the line's. */
		if (reg->moving && move_fail(reg, 200))
			return;

		binding_drop(reg);
		reg->fail_run++;
		state_set(reg, SIP_REG_BACKOFF);
		timer_arm(reg, SIP_REG_T_RETRY, backoff_secs(reg));
		reg->retry_fixed = false;
		reg_trace(reg, "registrar granted no expiry");
		unregistered_notify(reg);
		return;
	}

	old = reg->binding_dst;
	memcpy(old_contact, reg->binding_contact, sizeof(old_contact));
	moved = reg->moving;
	reg->moving = false;
	reg->granted = expires;
	reg->binding_dst = reg->attempt_dst;
	service_route_take(reg, msg);
	memcpy(reg->binding_contact, reg->contact_uri,
	       sizeof(reg->binding_contact));

	/*
	 * The response itself moved the Contact, so the binding the registrar
	 * just confirmed points at the old address. Register again with the new
	 * one before telling the owner the account is up. The correction runs
	 * against that same server, never against a selection a reload moved
	 * under it, so it is no move; the removal a move owes is remembered and
	 * runs from the clean answer that ends the chase.
	 */
	if (reg->contact_dirty) {
		reg->rebind_run++;
		if (reg->rebind_run > SIP_REG_REBIND_MAX) {
			rebind_giveup(reg);
			return;
		}

		if (moved) {
			reg->remove_dst = old;
			memcpy(reg->remove_contact, old_contact,
			       sizeof(reg->remove_contact));
		}

		reg->rebinding = true;
		reg_trace(reg, "contact changed under the answer, registering again");
		attempt_start(reg);
		return;
	}

	reg->rebind_run = 0;
	state_set(reg, SIP_REG_REGISTERED);
	timer_arm(reg, SIP_REG_T_REFRESH, sip_reg_refresh_secs(expires));
	reg_trace(reg, "registered for %u s", expires);
	if (reg->cb.registered)
		reg->cb.registered(reg, expires, reg->priv);

	if (!moved && reg->remove_dst.sa_len) {
		old = reg->remove_dst;
		memcpy(old_contact, reg->remove_contact, sizeof(old_contact));
		moved = true;
	}
	remove_pending_clear(reg);

	if (moved)
		binding_remove(reg, &old, old_contact);
}

/*
 * The retry allowance is per kind. A registrar that challenges first (401) and
 * a proxy in front of it that challenges next (407) are two exchanges, not two
 * attempts at one password: counting them together would turn the second,
 * legal challenge into a credential backoff.
 */
static void resp_auth(struct sip_reg *reg, const struct sip_msg *msg, uint32_t code)
{
	enum sip_auth_status status[SIP_AUTH_KIND_MAX];
	unsigned int k;
	bool resend;

	reg->stat.challenged++;
	resend = sip_auth_challenges_take(reg->auth, msg, reg->auth_retry,
					  sip_txn_rand, reg->mgr, status);

	for (k = 0; k < SIP_AUTH_KIND_MAX; k++) {
		const char *kind = sip_auth_kind_name((enum sip_auth_kind)k);

		if (status[k] == SIP_AUTH_ERR_NONE)
			continue;
		if (status[k] != SIP_AUTH_OK)
			reg_trace(reg, "%s challenge refused (%s)", kind,
				  sip_auth_status_name(status[k]));
		else if (reg->auth[k].stale)
			reg_trace(reg, "%s stale nonce, fresh challenge taken",
				  kind);
	}

	if (!resend) {
		reg_fail(reg, code, 0, true);
		return;
	}

	attempt_resend(reg);
}

static void resp_min_expires(struct sip_reg *reg, const struct sip_msg *msg)
{
	uint32_t min = 0;

	/*
	 * A resend that carries Min-Expires asks the server for a binding, and
	 * this exchange asks it to take one away. The 423 ends the exchange
	 * instead (RFC 3261 10.2.8 answers a registration, not a removal).
	 */
	if (reg->unregistering || reg->removing) {
		reg_trace(reg, "423 to a REGISTER that removes a binding");
		reg_fail(reg, 423, 0, false);
		return;
	}

	if (!hdr_uint_get(msg, SIP_HDR_MIN_EXPIRES, &min) || !min) {
		reg_trace(reg, "423 without a usable Min-Expires");
		reg_fail(reg, 423, 0, false);
		return;
	}

	if (min <= reg->req_expires || reg->min_expires_retry >= SIP_REG_MIN_EXPIRES_MAX) {
		reg_trace(reg, "423 makes no progress at %u s", min);
		reg_fail(reg, 423, 0, false);
		return;
	}

	reg->min_expires_retry++;
	reg->min_expires = min;
	reg->req_expires = min;
	reg_trace(reg, "423, asking for %u s", min);
	attempt_resend(reg);
}

static void resp_unavailable(struct sip_reg *reg, const struct sip_msg *msg)
{
	uint32_t secs = 0;

	/* RFC 3261 21.5.4: Retry-After names the delay; without it the next
	 * target is tried on the ordinary backoff. */
	if (!hdr_retry_after_get(msg, &secs, NULL))
		secs = 0;

	reg_fail(reg, 503, secs, false);
}

/*
 * The deferred half of sip_reg_retarget(). It runs when an exchange truly
 * ended: a resend inside the exchange re-set reg->txn and skips it. Only a
 * success sends at once, because it would otherwise wait out the refresh
 * timer; a failure armed its own retry, that delay honours the ladder, a
 * Retry-After and the credential backoff, and its attempt reads the new
 * selection anyway.
 */
static void retarget_settle(struct sip_reg *reg)
{
	const struct sip_reg_target *t;

	if (!reg->retarget_pending || reg->txn != SIP_HANDLE_NONE)
		return;

	reg->retarget_pending = false;
	if (!reg->running || reg->unregistering)
		return;

	/*
	 * The exchange that ended may have satisfied the retarget by itself:
	 * the rollback of a failed move puts the selection back on the server
	 * the last attempt ran against. Sending again would drop the refresh
	 * that rollback restored, so the same test sip_reg_retarget() makes is
	 * made once more here.
	 */
	t = target_cur(reg);
	if (t && sip_dst_eq(&t->dst, &reg->attempt_dst))
		return;

	if (reg->state == SIP_REG_REGISTERED)
		attempt_start(reg);
}

/*
 * Whether a final answer refused the removal Contact with the request. A
 * challenge and a 423 ask for the same request again and judge nothing in it.
 * An answer to a request that carried no removal Contact says nothing of one
 * that a local host move recorded since.
 */
static bool old_contact_judged(const struct sip_reg *reg, uint32_t code)
{
	if (code < 300 || code == 401 || code == 407 || code == 423)
		return false;

	return reg->old_contact_sent && reg->old_contact_uri[0];
}

void sip_reg_txn_resp(struct sip_reg *reg, uint32_t txn, const struct sip_msg *msg)
{
	uint32_t code;

	if (!reg || !msg || msg->is_request)
		return;
	if (reg->txn == SIP_HANDLE_NONE || txn != reg->txn)
		return;

	code = msg->status;
	if (code < 200)
		return;

	/*
	 * Every final response can carry the address the registrar saw, with
	 * two exceptions. A REGISTER that removes a binding takes none of it: a
	 * rebuilt Contact would make the authenticated resend ask the server to
	 * remove a binding it never held. A move takes it from a 2xx alone: the
	 * new server grants nothing before that, so a challenge or a failure
	 * would move the Contact of a binding the old server still holds, and
	 * the rollback would leave the new server's mapping on it.
	 */
	if (!reg->unregistering && !reg->removing &&
	    (!reg->moving || code < 300))
		nat_check(reg, msg);
	reg->txn = SIP_HANDLE_NONE;
	reg->old_contact_refused = old_contact_judged(reg, code);

	if (code < 300)
		resp_ok(reg, msg);
	else if (code == 401 || code == 407)
		resp_auth(reg, msg, code);
	else if (code == 423)
		resp_min_expires(reg, msg);
	else if (code == 503)
		resp_unavailable(reg, msg);
	else
		reg_fail(reg, code, 0, code == 403);

	retarget_settle(reg);
}

void sip_reg_txn_done(struct sip_reg *reg, uint32_t txn, enum sip_txn_reason reason)
{
	if (!reg || reg->txn == SIP_HANDLE_NONE || txn != reg->txn)
		return;

	reg->txn = SIP_HANDLE_NONE;
	if (reason == SIP_TXN_DONE_TIMEOUT)
		reg->stat.timeout++;

	reg_fail(reg, reason == SIP_TXN_DONE_TRANSPORT ? 503 : 408, 0, false);
	retarget_settle(reg);
}

/* ------------------------------------------------------------------- API */

void sip_reg_init(struct sip_reg *reg, struct sip_txn_mgr *mgr, struct sip_txn_env *env,
		  uint32_t owner, const struct sip_reg_cfg *cfg, const struct sip_reg_cb *cb,
		  void *priv)
{
	memset(reg, 0, sizeof(*reg));
	reg->mgr = mgr;
	reg->env = env;
	reg->owner = owner;
	reg->priv = priv;

	if (cfg)
		reg->cfg = *cfg;
	if (cb)
		reg->cb = *cb;

	if (!reg->cfg.expires)
		reg->cfg.expires = SIP_REG_EXPIRES_DEFAULT;
	if (!reg->cfg.retry_s)
		reg->cfg.retry_s = SIP_REG_RETRY_DEFAULT;
	if (!reg->cfg.penalty_s)
		reg->cfg.penalty_s = SIP_REG_PENALTY_DEFAULT;
	if (!reg->cfg.local_port)
		reg->cfg.local_port = SIP_PORT_DEFAULT;

	sip_timer_init(&reg->timer, reg, SIP_TIMER_REG_RETRY, timer_fire);
	ident_make(reg);

	str_copy(reg->contact_host, sizeof(reg->contact_host),
		 reg->cfg.local_host);
	reg->contact_port = reg->cfg.local_port;
	contact_build(reg);
}

void sip_reg_done(struct sip_reg *reg)
{
	if (!reg)
		return;

	timer_stop(reg);
	txn_drop(reg);
	sip_txn_owner_release(reg->mgr, reg->owner);
	reg->running = false;
	reg->unregistering = false;
	reg->moving = false;
	reg->removing = false;
	reg->removal_slot = false;
	reg->rebinding = false;
	binding_drop(reg);
	reg->state = SIP_REG_UNREGISTERED;
}

void sip_reg_start(struct sip_reg *reg)
{
	if (!reg || reg->running)
		return;

	if (!cfg_ok(reg)) {
		reg_trace(reg, "incomplete configuration, not starting");
		return;
	}

	reg->running = true;
	reg->unregistering = false;
	reg->fail_run = 0;
	reg->rebind_run = 0;
	sip_auth_state_reset(&reg->auth[SIP_AUTH_WWW]);
	sip_auth_state_reset(&reg->auth[SIP_AUTH_PROXY]);
	attempt_start(reg);
}

void sip_reg_stop(struct sip_reg *reg, bool unregister)
{
	if (!reg)
		return;

	if (unregister)
		reg->old_contact_uri[0] = 0;
	timer_stop(reg);
	txn_drop(reg);

	if (!unregister || !reg->granted || !reg->targets) {
		stop_finish(reg);
		return;
	}

	reg->running = true;
	reg->unregistering = true;
	reg->fail_run = 0;
	attempt_start(reg);
}

void sip_reg_refresh_now(struct sip_reg *reg)
{
	if (!reg || !reg->running || reg->unregistering)
		return;

	/* A move starts a new Contact chase. */
	reg->fail_run = 0;
	reg->next_retry_s = 0;
	reg->rebind_run = 0;
	attempt_start(reg);
}

/*
 * The failure ladder stands: the outage the line met was real, and a restart
 * of the ladder would hide a registrar that is still dead. A delay a server
 * named and the credential backoff are not ours to cut short, and retry_fixed
 * marks both. A line with no target waits for the resolver instead.
 */
bool sip_reg_retry_now(struct sip_reg *reg)
{
	if (!reg || !reg->running || reg->unregistering)
		return false;

	if (sip_reg_timer_kind(reg) != SIP_REG_T_RETRY || reg->retry_fixed)
		return false;

	if (!reg->targets)
		return false;

	attempt_start(reg);

	return true;
}

void sip_reg_retarget(struct sip_reg *reg)
{
	const struct sip_txn_dst *last;
	struct sip_reg_target *t;

	if (!reg || !reg->running || reg->unregistering)
		return;

	t = target_cur(reg);
	if (!t)
		return;

	/*
	 * The selection still matches the last attempt's destination: the
	 * binding path did not move and every armed timer stands. A removal in
	 * flight is no registration attempt, and it runs against the server the
	 * binding left, so the binding's own server is what to compare against.
	 */
	last = reg->removing ? &reg->binding_dst : &reg->attempt_dst;
	if (sip_dst_eq(&t->dst, last))
		return;

	if (reg->txn != SIP_HANDLE_NONE) {
		reg->retarget_pending = true;
		reg_trace(reg, "retarget waits behind the request in flight");
		return;
	}

	/* A delay the server named runs out first; its attempt reads the new
	 * selection when the timer fires. */
	if (sip_reg_timer_kind(reg) == SIP_REG_T_RETRY && reg->retry_fixed) {
		reg_trace(reg, "retarget waits out the server delay");
		return;
	}

	attempt_start(reg);
}

enum sip_reg_state sip_reg_state(const struct sip_reg *reg)
{
	return (enum sip_reg_state)reg->state;
}

uint32_t sip_reg_expires(const struct sip_reg *reg)
{
	return reg->granted;
}

const char *sip_reg_contact(const struct sip_reg *reg)
{
	return reg->contact_uri;
}

const struct sip_txn_dst *sip_reg_binding_dst(const struct sip_reg *reg)
{
	if (!reg || !reg->granted || !reg->binding_dst.sa_len)
		return NULL;

	return &reg->binding_dst;
}

const struct sip_route_hdrs *sip_reg_service_route(const struct sip_reg *reg)
{
	return &reg->service_route;
}

/*
 * An unchanged Contact records nothing and changes nothing.
 * Otherwise, an empty removal slot takes the standing URI.
 * An occupied slot keeps the first binding the registrar still has.
 * A move back to the slot's URI clears it.
 * RFC 3261 section 10.3 step 7 refuses removal and refresh of the same Contact.
 */
bool sip_reg_local_host_set(struct sip_reg *reg)
{
	char old_host[SIP_REG_HOST_MAX];
	char old_uri[SIP_REG_URI_MAX];
	const char *host;
	uint16_t old_port;
	size_t len;

	if (!reg)
		return false;

	host = reg->cfg.local_host ? reg->cfg.local_host : "";
	len = strlen(host);
	if (!len) {
		reg_trace(reg, "local host move refused, no local host, "
			  "kept %s port %u", reg->contact_host,
			  reg->contact_port);
		return false;
	}
	if (len >= sizeof(reg->contact_host))
		goto refused;

	old_port = reg->contact_port;
	memcpy(old_host, reg->contact_host, sizeof(old_host));
	memcpy(old_uri, reg->contact_uri, sizeof(old_uri));
	memcpy(reg->contact_host, host, len + 1);
	reg->contact_port = reg->cfg.local_port;
	if (!contact_build(reg)) {
		memcpy(reg->contact_host, old_host, sizeof(old_host));
		reg->contact_port = old_port;
		goto refused;
	}

	if (!strcmp(reg->contact_uri, old_uri)) {
		memcpy(reg->contact_host, old_host, sizeof(old_host));
		reg->contact_port = old_port;
		reg_trace(reg, "local host move: the Contact is unchanged");
		return true;
	}

	if (!reg->old_contact_uri[0])
		memcpy(reg->old_contact_uri, old_uri, sizeof(old_uri));
	old_contact_settle(reg);
	reg->contact_dirty = true;
	reg->stat.contact_rebuilt++;
	reg_trace(reg, "contact %s port %u to %s port %u on a local host move",
		  old_host, old_port, reg->contact_host, reg->contact_port);

	return true;

refused:
	reg_trace(reg, "local host move refused, %s does not fit, "
		  "kept %s port %u",
		  host, reg->contact_host, reg->contact_port);
	return false;
}

const char *sip_reg_call_id(const struct sip_reg *reg)
{
	return reg->call_id;
}

uint32_t sip_reg_cseq(const struct sip_reg *reg)
{
	return reg->cseq;
}
