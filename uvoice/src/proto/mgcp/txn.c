// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * MGCP transaction layer. See mgcp/txn.h for the contracts.
 */
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto/mgcp/txn.h"

#define TXN_TRACE_MAX		240

#define TMR_RETX		1
#define TMR_LIFE		2
#define TMR_HIST		3
#define TMR_DISC		4

#define POOL_LIMIT		1024
#define HIST_LIMIT		1024
#define WIRE_MIN		64

/* the empty ResponseAck line of 3.5.6 */
#define HIST_K_LINE		"K:\r\n"
#define HIST_K_LEN		(sizeof(HIST_K_LINE) - 1)

/* T-DELAY doubles on every retransmission; the cap keeps it in 32 bits */
#define TDELAY_CAP_MS		(1u << 30)

/* the longest delay sample the estimator takes, so microseconds fit 32 bits */
#define RTT_SAMPLE_MAX_MS	4000000u

/* 4.4.7: the first delay is at least one second */
#define TD_FIRST_MIN_MS		1000u

/* ----------------------------------------------------------- environment */

static struct proto_timer_env *base_of(const struct mgcp_txn_mgr *mgr)
{
	return &mgr->env->base;
}

static uint64_t now_ms(const struct mgcp_txn_mgr *mgr)
{
	struct proto_timer_env *base = base_of(mgr);

	return base->now_ms(base);
}

static void txn_trace(const struct mgcp_txn_mgr *mgr, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void txn_trace(const struct mgcp_txn_mgr *mgr, const char *fmt, ...)
{
	struct proto_timer_env *base = base_of(mgr);
	char line[TXN_TRACE_MAX];
	va_list ap;

	if (!base->trace)
		return;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	base->trace(base, line);
}

uint32_t mgcp_txn_rand(struct mgcp_txn_mgr *mgr)
{
	struct proto_timer_env *base = base_of(mgr);
	uint32_t x;

	if (base->rand32)
		return base->rand32(base);

	/* xorshift32 seeded from the clock: timer.h leaves the generator to
	 * the layer when the environment has none. */
	if (!mgr->rand_state) {
		uint64_t now = now_ms(mgr);

		mgr->rand_state = (uint32_t)(now ^ (now >> 32)) | 1u;
	}

	x = mgr->rand_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	mgr->rand_state = x;

	return x;
}

static uint32_t uniform(struct mgcp_txn_mgr *mgr, uint32_t lo, uint32_t hi)
{
	if (hi <= lo)
		return lo;

	return lo + mgcp_txn_rand(mgr) % (hi - lo + 1);
}

static bool sa_store(struct sockaddr_storage *dst, socklen_t *dst_len,
		     const struct sockaddr *src, socklen_t src_len)
{
	if (!src || !src_len || src_len > sizeof(*dst))
		return false;

	memset(dst, 0, sizeof(*dst));
	memcpy(dst, src, src_len);
	*dst_len = src_len;

	return true;
}

static bool sa_same(const struct sockaddr_storage *a, socklen_t a_len,
		    const struct sockaddr_storage *b, socklen_t b_len)
{
	const struct sockaddr_in *a4 = (const struct sockaddr_in *)a;
	const struct sockaddr_in *b4 = (const struct sockaddr_in *)b;
	const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)a;
	const struct sockaddr_in6 *b6 = (const struct sockaddr_in6 *)b;

	if (a->ss_family != b->ss_family)
		return false;
	if (a->ss_family == AF_INET)
		return a4->sin_addr.s_addr == b4->sin_addr.s_addr &&
		       a4->sin_port == b4->sin_port;
	if (a->ss_family == AF_INET6)
		return !memcmp(&a6->sin6_addr, &b6->sin6_addr,
			       sizeof(a6->sin6_addr)) &&
		       a6->sin6_port == b6->sin6_port;

	return a_len == b_len && !memcmp(a, b, a_len);
}

static int wire_send(struct mgcp_txn_mgr *mgr, const void *buf, size_t len,
		     const struct sockaddr_storage *dst, socklen_t dst_len)
{
	struct mgcp_txn_env *env = mgr->env;
	int rc = env->send(env, buf, len, (const struct sockaddr *)dst,
			   dst_len);

	if (rc < 0)
		mgr->stat.transport++;

	return rc;
}

const char *mgcp_txn_reason_name(enum mgcp_txn_reason reason)
{
	switch (reason) {
	case MGCP_TXN_DONE_OK:
		return "ok";
	case MGCP_TXN_DONE_TIMEOUT:
		return "timeout";
	case MGCP_TXN_DONE_TRANSPORT:
		return "transport";
	case MGCP_TXN_DONE_ABORT:
		return "abort";
	}

	return "?";
}

const char *mgcp_txn_rx_name(enum mgcp_txn_rx rx)
{
	switch (rx) {
	case MGCP_RX_IGNORED:
		return "ignored";
	case MGCP_RX_ABSORBED:
		return "absorbed";
	case MGCP_RX_DELIVERED:
		return "delivered";
	case MGCP_RX_STRAY:
		return "stray";
	}

	return "?";
}

/* ------------------------------------------------------------------ config */

static uint32_t verbs_all(void)
{
	uint32_t mask = 0;
	unsigned int v;

	for (v = MGCP_VERB_UNKNOWN + 1; v < MGCP_VERB_MAX; v++)
		mask |= 1u << v;

	return mask;
}

void mgcp_txn_cfg_default(struct mgcp_txn_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->max1 = MGCP_TXN_MAX1_DEFAULT;
	cfg->max2 = MGCP_TXN_MAX2_DEFAULT;
	cfg->rto_init_ms = MGCP_TXN_RTO_INIT_DEFAULT;
	cfg->rto_min_ms = MGCP_TXN_RTO_MIN_DEFAULT;
	cfg->rto_max_ms = MGCP_TXN_RTO_MAX_DEFAULT;
	cfg->t_max_ms = MGCP_TXN_T_MAX_DEFAULT;
	cfg->t_hist_ms = MGCP_TXN_T_HIST_DEFAULT;
	cfg->longtran_ms = MGCP_TXN_LONGTRAN_DEFAULT;
	cfg->td_init_ms = MGCP_TXN_TD_INIT_DEFAULT;
	cfg->td_min_ms = MGCP_TXN_TD_MIN_DEFAULT;
	cfg->td_max_ms = MGCP_TXN_TD_MAX_DEFAULT;
	cfg->prov_verbs = verbs_all();
	cfg->txid_first = 1;
	cfg->pool_max = MGCP_TXN_POOL_DEFAULT;
	cfg->hist_max = MGCP_TXN_HIST_DEFAULT;
	cfg->wire_max = MGCP_TXN_WIRE_DEFAULT;
}

static void cfg_fixup(struct mgcp_txn_cfg *cfg)
{
	struct mgcp_txn_cfg def;

	mgcp_txn_cfg_default(&def);

#define CFG_ZERO_DEFAULT(f)	do { if (!cfg->f) cfg->f = def.f; } while (0)
	CFG_ZERO_DEFAULT(max1);
	CFG_ZERO_DEFAULT(max2);
	CFG_ZERO_DEFAULT(rto_init_ms);
	CFG_ZERO_DEFAULT(rto_min_ms);
	CFG_ZERO_DEFAULT(rto_max_ms);
	CFG_ZERO_DEFAULT(t_max_ms);
	CFG_ZERO_DEFAULT(t_hist_ms);
	CFG_ZERO_DEFAULT(longtran_ms);
	CFG_ZERO_DEFAULT(td_init_ms);
	CFG_ZERO_DEFAULT(td_min_ms);
	CFG_ZERO_DEFAULT(td_max_ms);
	CFG_ZERO_DEFAULT(prov_verbs);
	CFG_ZERO_DEFAULT(pool_max);
	CFG_ZERO_DEFAULT(hist_max);
	CFG_ZERO_DEFAULT(wire_max);
#undef CFG_ZERO_DEFAULT

	if (cfg->max2 <= cfg->max1)
		cfg->max2 = cfg->max1 + 1;
	if (cfg->rto_max_ms < cfg->rto_min_ms)
		cfg->rto_max_ms = cfg->rto_min_ms;
	if (cfg->rto_init_ms < cfg->rto_min_ms)
		cfg->rto_init_ms = cfg->rto_min_ms;
	if (cfg->rto_init_ms > cfg->rto_max_ms)
		cfg->rto_init_ms = cfg->rto_max_ms;
	if (cfg->t_max_ms > MGCP_TXN_LONG_MAX_MS)
		cfg->t_max_ms = MGCP_TXN_LONG_MAX_MS;
	if (cfg->t_hist_ms > MGCP_TXN_LONG_MAX_MS)
		cfg->t_hist_ms = MGCP_TXN_LONG_MAX_MS;
	if (cfg->td_max_ms > MGCP_TXN_LONG_MAX_MS)
		cfg->td_max_ms = MGCP_TXN_LONG_MAX_MS;
	if (cfg->t_hist_ms < cfg->t_max_ms)
		cfg->t_hist_ms = cfg->t_max_ms;
	if (cfg->longtran_ms >= cfg->t_hist_ms)
		cfg->longtran_ms = cfg->t_hist_ms / 2;
	if (cfg->td_init_ms < TD_FIRST_MIN_MS)
		cfg->td_init_ms = TD_FIRST_MIN_MS;
	if (cfg->td_init_ms > MGCP_TXN_LONG_MAX_MS)
		cfg->td_init_ms = MGCP_TXN_LONG_MAX_MS;
	if (cfg->td_max_ms < cfg->td_init_ms)
		cfg->td_max_ms = cfg->td_init_ms;
	if (!cfg->txid_first || cfg->txid_first > MGCP_TXID_MAX)
		cfg->txid_first = 1;
	if (cfg->pool_max > POOL_LIMIT)
		cfg->pool_max = POOL_LIMIT;
	if (cfg->hist_max > HIST_LIMIT)
		cfg->hist_max = HIST_LIMIT;
	if (cfg->wire_max < WIRE_MIN)
		cfg->wire_max = WIRE_MIN;
	if (cfg->wire_max > MGCP_MSG_LEN_MAX)
		cfg->wire_max = MGCP_MSG_LEN_MAX;
}

/* --------------------------------------------------------------- estimator */

static uint32_t rto_clamp(const struct mgcp_txn_mgr *mgr, uint64_t ms)
{
	if (ms < mgr->cfg.rto_min_ms)
		return mgr->cfg.rto_min_ms;
	if (ms > mgr->cfg.rto_max_ms)
		return mgr->cfg.rto_max_ms;

	return (uint32_t)ms;
}

/* 3.5.3: RTO = AAD + N * ADEV */
static uint32_t rto_base(const struct mgcp_txn_mgr *mgr)
{
	uint64_t us;

	if (!mgr->rtt_valid)
		return rto_clamp(mgr, mgr->cfg.rto_init_ms);

	us = (uint64_t)mgr->aad_us + (uint64_t)MGCP_TXN_RTO_K * mgr->adev_us;

	return rto_clamp(mgr, us / 1000);
}

uint32_t mgcp_txn_rto_ms(const struct mgcp_txn_mgr *mgr)
{
	if (mgr->rto_carry_ms)
		return mgr->rto_carry_ms;

	return rto_base(mgr);
}

/*
 * 3.5.3, after a retransmission: double T-DELAY, draw a value uniformly
 * between T-DELAY / 2 and T-DELAY, add N * ADEV, bound it by RTO-MAX.
 * T-DELAY starts at the first RTO the transaction armed, after its bounds,
 * so the floor cannot flatten the exponential backoff that 3.5.3 requires.
 */
static uint32_t rto_backoff(struct mgcp_txn_mgr *mgr, uint32_t *tdelay)
{
	uint32_t td = *tdelay ? *tdelay : 1, half, r;

	td = td >= TDELAY_CAP_MS ? TDELAY_CAP_MS : td * 2;
	*tdelay = td;
	half = td / 2;
	r = uniform(mgr, half, td);

	return rto_clamp(mgr, (uint64_t)r +
			 (uint64_t)MGCP_TXN_RTO_K * mgr->adev_us / 1000);
}

/*
 * 3.5.3 names the TCP estimator. The gains are those of RFC 6298 2.2 and
 * 2.3: the first sample sets AAD to R and ADEV to R / 2; each later one
 * moves ADEV a quarter and AAD an eighth of the way to the sample.
 */
static void rtt_sample(struct mgcp_txn_mgr *mgr, uint64_t ms)
{
	uint32_t r, diff;

	if (ms > RTT_SAMPLE_MAX_MS)
		ms = RTT_SAMPLE_MAX_MS;
	r = (uint32_t)ms * 1000;

	if (!mgr->rtt_valid) {
		mgr->aad_us = r;
		mgr->adev_us = r / 2;
		mgr->rtt_valid = true;
		return;
	}

	diff = mgr->aad_us > r ? mgr->aad_us - r : r - mgr->aad_us;
	mgr->adev_us = (uint32_t)(((uint64_t)mgr->adev_us * 3 + diff) / 4);
	mgr->aad_us = (uint32_t)(((uint64_t)mgr->aad_us * 7 + r) / 8);
}

/* ------------------------------------------------------------------- pool */

static void txn_retx_fire(struct proto_timer *t);
static void txn_life_fire(struct proto_timer *t);
static void hist_fire(struct proto_timer *t);
static void disc_fire(struct proto_timer *t);

uint32_t mgcp_txn_handle(const struct mgcp_txn *txn)
{
	if (!txn || !txn->in_use)
		return MGCP_TXN_HANDLE_NONE;

	return ((uint32_t)txn->index << 16) | txn->gen;
}

struct mgcp_txn *mgcp_txn_get(struct mgcp_txn_mgr *mgr, uint32_t handle)
{
	unsigned int index = handle >> 16;
	struct mgcp_txn *txn;

	if (!mgr || !mgr->pool || handle == MGCP_TXN_HANDLE_NONE ||
	    index >= mgr->cfg.pool_max)
		return NULL;

	txn = &mgr->pool[index];
	if (!txn->in_use || txn->gen != (handle & 0xffffu))
		return NULL;

	return txn;
}

uint32_t mgcp_txn_find(struct mgcp_txn_mgr *mgr, uint32_t txid)
{
	unsigned int i;

	if (!mgr || !mgr->pool)
		return MGCP_TXN_HANDLE_NONE;

	for (i = 0; i < mgr->cfg.pool_max; i++) {
		if (mgr->pool[i].in_use && mgr->pool[i].txid == txid)
			return mgcp_txn_handle(&mgr->pool[i]);
	}

	return MGCP_TXN_HANDLE_NONE;
}

unsigned int mgcp_txn_count(const struct mgcp_txn_mgr *mgr)
{
	return mgr->active;
}

static void txn_slot_reset(struct mgcp_txn_mgr *mgr, struct mgcp_txn *txn)
{
	char *wire = txn->wire;
	unsigned int index = txn->index;
	uint16_t gen = txn->gen;

	memset(txn, 0, sizeof(*txn));
	txn->mgr = mgr;
	txn->wire = wire;
	txn->index = index;
	txn->gen = gen ? gen : 1;
	proto_timer_init(&txn->retx, txn, TMR_RETX, txn_retx_fire);
	proto_timer_init(&txn->life, txn, TMR_LIFE, txn_life_fire);
}

static struct mgcp_txn *txn_alloc(struct mgcp_txn_mgr *mgr)
{
	unsigned int i;

	for (i = 0; i < mgr->cfg.pool_max; i++) {
		struct mgcp_txn *txn = &mgr->pool[i];

		if (txn->in_use)
			continue;

		txn_slot_reset(mgr, txn);
		txn->in_use = true;
		mgr->active++;
		mgr->stat.created++;
		return txn;
	}

	mgr->stat.denied++;

	return NULL;
}

static void txn_free(struct mgcp_txn *txn)
{
	struct mgcp_txn_mgr *mgr = txn->mgr;

	proto_timer_cancel(base_of(mgr), &txn->retx);
	proto_timer_cancel(base_of(mgr), &txn->life);
	txn->in_use = false;
	txn->gen++;
	if (!txn->gen)
		txn->gen = 1;
	mgr->active--;
}

/* ---------------------------------------------------- the gateway K: list */

static void acks_prune(struct mgcp_txn_mgr *mgr, uint64_t now)
{
	unsigned int i, kept = 0;

	for (i = 0; i < mgr->ack_count; i++) {
		if (now - mgr->ack[i].stamp_ms > mgr->cfg.t_hist_ms)
			continue;
		mgr->ack[kept++] = mgr->ack[i];
	}
	mgr->ack_count = kept;
}

static void acks_add(struct mgcp_txn_mgr *mgr, const struct mgcp_txn *txn,
		     uint64_t now)
{
	struct mgcp_txn_ack *a;

	acks_prune(mgr, now);
	if (mgr->ack_count == MGCP_ACK_LIST_MAX) {
		memmove(&mgr->ack[0], &mgr->ack[1],
			sizeof(mgr->ack[0]) * (MGCP_ACK_LIST_MAX - 1));
		mgr->ack_count--;
	}

	a = &mgr->ack[mgr->ack_count++];
	a->dst = txn->dst;
	a->dst_len = txn->dst_len;
	a->stamp_ms = now;
	a->txid = txn->txid;
	a->owner = txn->owner;
}

static bool ack_list_has(const struct mgcp_ack_list *acks, uint32_t txid)
{
	unsigned int i;

	for (i = 0; i < acks->count && i < MGCP_ACK_LIST_MAX; i++) {
		if (acks->txid[i] == txid)
			return true;
	}

	return false;
}

static void acks_take(struct mgcp_txn_mgr *mgr,
		      const struct mgcp_ack_list *acks)
{
	unsigned int i, kept = 0;

	for (i = 0; i < mgr->ack_count; i++) {
		if (ack_list_has(acks, mgr->ack[i].txid))
			continue;
		mgr->ack[kept++] = mgr->ack[i];
	}
	mgr->ack_count = kept;
}

unsigned int mgcp_txn_acks_get(struct mgcp_txn_mgr *mgr, uint32_t owner,
			       const struct sockaddr *dst, socklen_t dst_len,
			       struct mgcp_ack_list *out)
{
	struct sockaddr_storage sa;
	socklen_t sa_len;
	unsigned int i;

	out->count = 0;
	if (!mgr || !sa_store(&sa, &sa_len, dst, dst_len))
		return 0;

	acks_prune(mgr, now_ms(mgr));
	for (i = 0; i < mgr->ack_count; i++) {
		const struct mgcp_txn_ack *a = &mgr->ack[i];

		if (owner != MGCP_TXN_OWNER_ANY && a->owner != owner)
			continue;
		if (!sa_same(&a->dst, a->dst_len, &sa, sa_len))
			continue;
		out->txid[out->count++] = a->txid;
	}

	return out->count;
}

/* ------------------------------------------- the past gateway transactions */

static void past_add(struct mgcp_txn_mgr *mgr, uint32_t txid,
		     const struct sockaddr_storage *peer, socklen_t peer_len,
		     bool ack, uint64_t now)
{
	struct mgcp_txn_past *p = &mgr->past[mgr->past_next];

	mgr->past_next = (mgr->past_next + 1) % mgr->cfg.hist_max;
	p->peer = *peer;
	p->peer_len = peer_len;
	p->stamp_ms = now;
	p->txid = txid;
	p->ack = ack;
}

static struct mgcp_txn_past *past_find(struct mgcp_txn_mgr *mgr,
				       uint32_t txid, uint64_t now)
{
	unsigned int i;

	for (i = 0; i < mgr->cfg.hist_max; i++) {
		struct mgcp_txn_past *p = &mgr->past[i];

		if (p->txid == txid && now - p->stamp_ms <= mgr->cfg.t_hist_ms)
			return p;
	}

	return NULL;
}

/* "000 <txid>", 3.5.6 */
static void resp_ack_send(struct mgcp_txn_mgr *mgr, uint32_t txid,
			  const struct sockaddr_storage *to, socklen_t to_len)
{
	struct mgcp_resp r = { .code = 0, .txid = txid };
	char text[48];
	struct sip_buf b;

	sip_buf_init(&b, text, sizeof(text));
	if (!mgcp_resp_build(&b, &r))
		return;

	wire_send(mgr, text, sip_buf_len(&b), to, to_len);
	txn_trace(mgr, "mgcp txn %u: response acknowledgment sent", txid);
}

/* ------------------------------------------------------ gateway commands */

static uint32_t td_next(struct mgcp_txn_mgr *mgr, uint32_t td);

/*
 * 4.4.7 step 4: an attempt that left the endpoint disconnected doubles the
 * delay, armed from its end (arm). An aborted attempt arms nothing,
 * because the caller replaces it.
 */
static void disc_attempt_end(struct mgcp_txn_mgr *mgr, bool arm)
{
	mgr->disc.end_ms = now_ms(mgr);
	if (!arm)
		return;

	mgr->disc.td_ms = td_next(mgr, mgr->disc.td_ms);
	proto_timer_arm(base_of(mgr), &mgr->disc.timer, mgr->disc.td_ms);
	txn_trace(mgr, "mgcp disconnected: attempt failed, next in %u ms",
		  mgr->disc.td_ms);
}

/*
 * The end of a gateway transaction. It leaves the pool before an upcall
 * runs, so a handle the caller holds answers NULL inside the upcall.
 */
static void txn_end(struct mgcp_txn *txn, enum mgcp_txn_reason reason,
		    bool disconnect)
{
	struct mgcp_txn_mgr *mgr = txn->mgr;
	struct mgcp_txn_env *env = mgr->env;
	uint32_t owner = txn->owner, txid = txn->txid;
	bool up = !txn->detached && env->done;
	bool disc = txn->disc;

	txn_trace(mgr, "mgcp txn %u: end (%s) after %u sends", txid,
		  mgcp_txn_reason_name(reason), txn->sends);
	if (reason == MGCP_TXN_DONE_TIMEOUT)
		mgr->stat.timeout++;

	txn_free(txn);
	if (disc && mgr->disc.active)
		disc_attempt_end(mgr, reason == MGCP_TXN_DONE_TIMEOUT ||
			     reason == MGCP_TXN_DONE_TRANSPORT);
	if (disconnect && !mgr->disc.active && env->disconnected)
		env->disconnected(env);
	if (up)
		env->done(env, owner, txid, reason);
}

static void txn_quiet(struct mgcp_txn *txn, const char *why)
{
	txn->quiet = true;
	proto_timer_cancel(base_of(txn->mgr), &txn->retx);
	txn_trace(txn->mgr, "mgcp txn %u: retransmission ceased (%s)",
		  txn->txid, why);
}

static void txn_resend(struct mgcp_txn *txn, bool fresh)
{
	struct mgcp_txn_mgr *mgr = txn->mgr;

	if (wire_send(mgr, txn->wire, txn->len, &txn->dst, txn->dst_len) < 0) {
		txn_end(txn, MGCP_TXN_DONE_TRANSPORT, false);
		return;
	}

	txn->sends++;
	if (!fresh)
		txn->reps++;
	mgr->stat.retransmitted++;

	if (txn->provisional)
		txn->rto_ms = mgr->cfg.longtran_ms;
	else
		txn->rto_ms = rto_backoff(mgr, &txn->tdelay_ms);
	proto_timer_arm(base_of(mgr), &txn->retx, txn->rto_ms);
}

typedef void (*max_upcall)(struct mgcp_txn_env *env, uint32_t owner,
			   uint32_t txid, uint32_t handle);

/*
 * Run a Max1 or Max2 upcall. NULL means the transaction left the pool or
 * the caller moved it inside the upcall; the caller of this function then
 * stops, because a redirect armed its own send.
 */
static struct mgcp_txn *txn_max_up(struct mgcp_txn *txn, max_upcall up,
				   const char *what)
{
	struct mgcp_txn_mgr *mgr = txn->mgr;
	uint32_t handle = mgcp_txn_handle(txn);

	txn_trace(mgr, "mgcp txn %u: %s reached", txn->txid, what);
	if (!up)
		return txn;

	up(mgr->env, txn->owner, txn->txid, handle);
	txn = mgcp_txn_get(mgr, handle);
	if (!txn || txn->redirected)
		return NULL;

	return txn;
}

/*
 * The retransmission timer, in the order of the 4.3 figure: the T-MAX
 * bound, then Max2, then Max1. A send to a new address (fresh) counts no
 * repetition, because N restarts at 0 there.
 */
static void txn_retx_fire(struct proto_timer *t)
{
	struct mgcp_txn *txn = t->owner;
	struct mgcp_txn_mgr *mgr = txn->mgr;
	const struct mgcp_txn_cfg *cfg = &mgr->cfg;
	bool fresh = txn->redirected;

	txn->redirected = false;

	if (now_ms(mgr) - txn->first_ms > cfg->t_max_ms) {
		txn_quiet(txn, "T-MAX");
		return;
	}

	if (!fresh && !txn->provisional) {
		if (txn->reps >= cfg->max2) {
			if (!txn->max2_seen) {
				txn->max2_seen = true;
				txn = txn_max_up(txn, mgr->env->max2, "Max2");
				if (!txn)
					return;
			}
			if (cfg->flags & MGCP_TXN_CFG_MAX2_QUIET) {
				txn_quiet(txn, "Max2");
				return;
			}
			txn_end(txn, MGCP_TXN_DONE_TIMEOUT, true);
			return;
		}
		if (txn->reps == cfg->max1 && !txn->max1_seen) {
			txn->max1_seen = true;
			txn = txn_max_up(txn, mgr->env->max1, "Max1");
			if (!txn)
				return;
		}
	}

	txn_resend(txn, fresh);
}

static void txn_life_fire(struct proto_timer *t)
{
	struct mgcp_txn *txn = t->owner;

	txn_trace(txn->mgr, "mgcp txn %u: no final response in 2 * T-HIST",
		  txn->txid);
	txn_end(txn, MGCP_TXN_DONE_TIMEOUT, true);
}

uint32_t mgcp_txn_txid_next(struct mgcp_txn_mgr *mgr)
{
	uint32_t id = 1;
	unsigned int n;

	for (n = 0; n <= mgr->cfg.pool_max; n++) {
		id = mgr->txid_next;
		mgr->txid_next = id >= MGCP_TXID_MAX ? 1 : id + 1;
		if (mgcp_txn_find(mgr, id) == MGCP_TXN_HANDLE_NONE)
			break;
	}

	return id;
}

/*
 * An RSIP of the disconnected procedure that never got out is an attempt
 * that failed on the transport; without this the procedure would stall.
 */
static void disc_refused(struct mgcp_txn_mgr *mgr,
			 const struct mgcp_txn_req *req)
{
	if ((req->flags & MGCP_TXN_F_DISC) && mgr->disc.active)
		disc_attempt_end(mgr, true);
}

uint32_t mgcp_txn_send(struct mgcp_txn_mgr *mgr,
		       const struct mgcp_txn_req *req)
{
	struct mgcp_txn *txn;
	uint32_t handle;

	if (!mgr || !mgr->pool || !req || !req->wire || !req->len)
		return MGCP_TXN_HANDLE_NONE;
	if (!req->txid || req->txid > MGCP_TXID_MAX ||
	    mgcp_txn_find(mgr, req->txid) != MGCP_TXN_HANDLE_NONE)
		return MGCP_TXN_HANDLE_NONE;
	if (!req->dst || !req->dst_len ||
	    req->dst_len > sizeof(struct sockaddr_storage))
		return MGCP_TXN_HANDLE_NONE;
	if (req->len > mgr->cfg.wire_max) {
		mgr->stat.oversize++;
		return MGCP_TXN_HANDLE_NONE;
	}

	txn = txn_alloc(mgr);
	if (!txn) {
		disc_refused(mgr, req);
		return MGCP_TXN_HANDLE_NONE;
	}

	memcpy(txn->wire, req->wire, req->len);
	txn->len = req->len;
	sa_store(&txn->dst, &txn->dst_len, req->dst, req->dst_len);
	txn->txid = req->txid;
	txn->owner = req->owner;
	txn->disc = (req->flags & MGCP_TXN_F_DISC) != 0;
	txn->first_ms = now_ms(mgr);
	txn->rto_ms = mgcp_txn_rto_ms(mgr);
	txn->tdelay_ms = mgr->rto_carry_ms ? mgr->tdelay_carry_ms :
			 txn->rto_ms;
	handle = mgcp_txn_handle(txn);

	if (wire_send(mgr, txn->wire, txn->len, &txn->dst, txn->dst_len) < 0) {
		txn_trace(mgr, "mgcp txn %u: first send refused", txn->txid);
		txn_free(txn);
		disc_refused(mgr, req);
		return MGCP_TXN_HANDLE_NONE;
	}

	txn->sends = 1;
	if (req->acks)
		acks_take(mgr, req->acks);
	proto_timer_arm(base_of(mgr), &txn->retx, txn->rto_ms);
	proto_timer_arm(base_of(mgr), &txn->life,
			2 * mgr->cfg.t_hist_ms);
	txn_trace(mgr, "mgcp txn %u: sent, RTO %u ms", txn->txid, txn->rto_ms);

	return handle;
}

void mgcp_txn_abort(struct mgcp_txn_mgr *mgr, uint32_t handle)
{
	struct mgcp_txn *txn = mgcp_txn_get(mgr, handle);

	if (txn)
		txn_end(txn, MGCP_TXN_DONE_ABORT, false);
}

unsigned int mgcp_txn_owner_release(struct mgcp_txn_mgr *mgr, uint32_t owner)
{
	unsigned int i, n = 0;

	for (i = 0; mgr->pool && i < mgr->cfg.pool_max; i++) {
		struct mgcp_txn *txn = &mgr->pool[i];

		if (!txn->in_use || txn->owner != owner)
			continue;
		txn->detached = true;
		n++;
	}

	return n;
}

static void txn_move(struct mgcp_txn_mgr *mgr, struct mgcp_txn *txn,
		     const struct sockaddr_storage *sa, socklen_t sa_len)
{
	txn->dst = *sa;
	txn->dst_len = sa_len;
	txn->reps = 0;
	txn->max1_seen = false;
	txn->max2_seen = false;
	txn->quiet = false;
	txn->redirected = true;
	proto_timer_arm(base_of(mgr), &txn->retx, 0);
}

unsigned int mgcp_txn_redirect(struct mgcp_txn_mgr *mgr, uint32_t owner,
			       const struct sockaddr *dst, socklen_t dst_len)
{
	struct sockaddr_storage sa;
	socklen_t sa_len;
	unsigned int i, n = 0;

	if (!mgr || !mgr->pool || !sa_store(&sa, &sa_len, dst, dst_len))
		return 0;

	for (i = 0; i < mgr->cfg.pool_max; i++) {
		struct mgcp_txn *txn = &mgr->pool[i];

		if (!txn->in_use)
			continue;
		if (owner != MGCP_TXN_OWNER_ANY && txn->owner != owner)
			continue;

		txn_move(mgr, txn, &sa, sa_len);
		n++;
	}

	return n;
}

bool mgcp_txn_redirect_one(struct mgcp_txn_mgr *mgr, uint32_t handle,
			   const struct sockaddr *dst, socklen_t dst_len)
{
	struct mgcp_txn *txn = mgcp_txn_get(mgr, handle);
	struct sockaddr_storage sa;
	socklen_t sa_len;

	if (!txn || !sa_store(&sa, &sa_len, dst, dst_len))
		return false;

	txn_move(mgr, txn, &sa, sa_len);

	return true;
}

/* ---------------------------------------------------- received commands */

static struct mgcp_txn_hist *hist_find(struct mgcp_txn_mgr *mgr,
				       uint32_t txid)
{
	unsigned int i;

	for (i = 0; i < mgr->cfg.hist_max; i++) {
		struct mgcp_txn_hist *h = &mgr->hist[i];

		if (h->state != MGCP_HIST_FREE && h->txid == txid)
			return h;
	}

	return NULL;
}

static void hist_free(struct mgcp_txn_hist *h)
{
	proto_timer_cancel(base_of(h->mgr), &h->timer);
	h->state = MGCP_HIST_FREE;
	h->len = 0;
}

/*
 * The eviction class of an entry: an answered or confirmed entry goes
 * before one whose command still runs or whose final awaits its "000",
 * because losing those would run a repeated command twice.
 */
static unsigned int hist_keep(const struct mgcp_txn_hist *h)
{
	switch (h->state) {
	case MGCP_HIST_FREE:
		return 0;
	case MGCP_HIST_FINAL:
	case MGCP_HIST_CONFIRMED:
		return 1;
	default:
		return 2;
	}
}

/* a free entry, or the oldest of the lowest class, which loses its record */
static struct mgcp_txn_hist *hist_alloc(struct mgcp_txn_mgr *mgr)
{
	struct mgcp_txn_hist *oldest = NULL;
	unsigned int i;

	for (i = 0; i < mgr->cfg.hist_max; i++) {
		struct mgcp_txn_hist *h = &mgr->hist[i];

		if (h->state == MGCP_HIST_FREE) {
			oldest = h;
			break;
		}
		if (!oldest || hist_keep(h) < hist_keep(oldest) ||
		    (hist_keep(h) == hist_keep(oldest) &&
		     h->stamp_ms < oldest->stamp_ms))
			oldest = h;
	}

	if (oldest->state != MGCP_HIST_FREE) {
		txn_trace(mgr, "mgcp cmd %u: evicted before T-HIST",
			  oldest->txid);
		mgr->stat.evicted++;
		hist_free(oldest);
	}

	return oldest;
}

static struct mgcp_txn_hist *hist_open(struct mgcp_txn_mgr *mgr,
				       uint32_t txid, uint8_t verb,
				       const struct sockaddr_storage *src,
				       socklen_t src_len)
{
	struct mgcp_txn_hist *h = hist_alloc(mgr);

	h->state = MGCP_HIST_RUNNING;
	h->txid = txid;
	h->verb = verb;
	h->peer = *src;
	h->peer_len = src_len;
	h->stamp_ms = now_ms(mgr);
	h->final_ms = 0;
	h->len = 0;
	h->prov_sent = false;

	return h;
}

/* 3.5.2: the response is confirmed, its id stays until T-HIST */
static void hist_confirm(struct mgcp_txn_hist *h)
{
	struct mgcp_txn_mgr *mgr = h->mgr;
	uint64_t now = now_ms(mgr), until = h->final_ms + mgr->cfg.t_hist_ms;

	proto_timer_cancel(base_of(mgr), &h->timer);
	h->state = MGCP_HIST_CONFIRMED;
	mgr->stat.acked++;
	proto_timer_arm(base_of(mgr), &h->timer,
			until > now ? (unsigned int)(until - now) : 0);
	txn_trace(mgr, "mgcp cmd %u: final response confirmed", h->txid);
}

static void hist_fire(struct proto_timer *t)
{
	struct mgcp_txn_hist *h = t->owner;
	struct mgcp_txn_mgr *mgr = h->mgr;
	uint64_t now = now_ms(mgr), until;

	if (h->state != MGCP_HIST_ACKWAIT) {
		hist_free(h);
		return;
	}

	/* 3.5.6: the final waits for its "000" on the command timers, and
	 * T-MAX ends its retransmissions too. */
	if (now - h->final_ms > mgr->cfg.t_max_ms) {
		h->state = MGCP_HIST_FINAL;
		until = h->final_ms + mgr->cfg.t_hist_ms;
		proto_timer_arm(base_of(mgr), &h->timer,
				until > now ? (unsigned int)(until - now) : 0);
		txn_trace(mgr, "mgcp cmd %u: no response acknowledgment",
			  h->txid);
		return;
	}

	if (wire_send(mgr, h->wire, h->len, &h->peer, h->peer_len) >= 0)
		mgr->stat.retransmitted++;
	h->rto_ms = rto_backoff(mgr, &h->tdelay_ms);
	proto_timer_arm(base_of(mgr), &h->timer, h->rto_ms);
}

static void hist_prov_send(struct mgcp_txn_mgr *mgr, struct mgcp_txn_hist *h,
			   const struct sockaddr_storage *to, socklen_t to_len)
{
	struct mgcp_resp r = { .code = 100, .txid = h->txid };
	struct sip_buf b;

	sip_buf_init(&b, h->wire, mgr->cfg.wire_max);
	if (!mgcp_resp_build(&b, &r))
		return;

	h->len = sip_buf_len(&b);
	h->state = MGCP_HIST_PROVISIONAL;
	h->prov_sent = true;
	wire_send(mgr, h->wire, h->len, to, to_len);
	txn_trace(mgr, "mgcp cmd %u: repetition while running, 100 sent",
		  h->txid);
}

static void hist_repeat(struct mgcp_txn_mgr *mgr, struct mgcp_txn_hist *h,
			const struct sockaddr_storage *src, socklen_t src_len)
{
	mgr->stat.absorbed++;

	switch (h->state) {
	case MGCP_HIST_RUNNING:
		if (mgr->cfg.prov_verbs & (1u << h->verb))
			hist_prov_send(mgr, h, src, src_len);
		return;
	case MGCP_HIST_PROVISIONAL:
	case MGCP_HIST_FINAL:
	case MGCP_HIST_ACKWAIT:
		if (h->len)
			wire_send(mgr, h->wire, h->len, src, src_len);
		return;
	default:
		return;
	}
}

static void acks_confirm(struct mgcp_txn_mgr *mgr, struct sip_str k,
			 const struct sockaddr_storage *src, socklen_t src_len)
{
	unsigned int i;

	for (i = 0; i < mgr->cfg.hist_max; i++) {
		struct mgcp_txn_hist *h = &mgr->hist[i];

		if (h->state != MGCP_HIST_FINAL &&
		    h->state != MGCP_HIST_ACKWAIT)
			continue;
		if (!sa_same(&h->peer, h->peer_len, src, src_len))
			continue;
		if (mgcp_ack_covers(k, h->txid))
			hist_confirm(h);
	}
}

static enum mgcp_txn_rx cmd_recv(struct mgcp_txn_mgr *mgr,
				 const struct mgcp_msg *msg,
				 const struct sockaddr_storage *src,
				 socklen_t src_len)
{
	struct mgcp_txn_env *env = mgr->env;
	struct mgcp_txn_hist *h;
	struct sip_str k;

	if (mgcp_msg_value(msg, MGCP_PARAM_K, &k) && k.len)
		acks_confirm(mgr, k, src, src_len);

	h = hist_find(mgr, msg->txid);
	if (h) {
		hist_repeat(mgr, h, src, src_len);
		return MGCP_RX_ABSORBED;
	}

	hist_open(mgr, msg->txid, (uint8_t)msg->verb, src, src_len);
	mgr->stat.delivered++;
	if (env->command)
		env->command(env, msg, (const struct sockaddr *)src, src_len);

	return MGCP_RX_DELIVERED;
}

/* "000" from the call agent for a final of ours (3.5.6) */
static enum mgcp_txn_rx resp_ack_recv(struct mgcp_txn_mgr *mgr,
				      const struct mgcp_msg *msg,
				      const struct sockaddr_storage *src,
				      socklen_t src_len)
{
	struct mgcp_txn_hist *h = hist_find(mgr, msg->txid);

	/* 3.5.2: accept it only from the entity the command came from */
	if (!h || !sa_same(&h->peer, h->peer_len, src, src_len)) {
		mgr->stat.stray++;
		return MGCP_RX_STRAY;
	}

	if (h->state == MGCP_HIST_FINAL || h->state == MGCP_HIST_ACKWAIT)
		hist_confirm(h);

	return MGCP_RX_ABSORBED;
}

static enum mgcp_txn_rx past_recv(struct mgcp_txn_mgr *mgr,
				  const struct mgcp_msg *msg,
				  const struct sockaddr_storage *src,
				  socklen_t src_len)
{
	struct mgcp_txn_past *p = past_find(mgr, msg->txid, now_ms(mgr));

	if (!p) {
		mgr->stat.stray++;
		txn_trace(mgr, "mgcp txn %u: stray response %03u", msg->txid,
			  msg->code);
		return MGCP_RX_STRAY;
	}

	mgr->stat.absorbed++;
	if (p->ack && mgcp_msg_param(msg, MGCP_PARAM_K))
		resp_ack_send(mgr, msg->txid, src, src_len);

	return MGCP_RX_ABSORBED;
}

static enum mgcp_txn_rx resp_recv(struct mgcp_txn_mgr *mgr,
				  const struct mgcp_msg *msg,
				  const struct sockaddr_storage *src,
				  socklen_t src_len)
{
	struct mgcp_txn_env *env = mgr->env;
	enum mgcp_rc_class cls = mgcp_rc_class_of(msg->code);
	struct mgcp_txn *txn;
	uint32_t owner, txid = msg->txid;
	uint64_t now = now_ms(mgr);
	bool ack, up, disc;

	if (cls == MGCP_RC_ACK)
		return resp_ack_recv(mgr, msg, src, src_len);

	txn = mgcp_txn_get(mgr, mgcp_txn_find(mgr, txid));
	if (!txn)
		return past_recv(mgr, msg, src, src_len);

	if (!txn->answered) {
		txn->answered = true;
		if (txn->sends == 1) {
			rtt_sample(mgr, now - txn->first_ms);
			mgr->rto_carry_ms = 0;
			mgr->tdelay_carry_ms = 0;
		} else {
			mgr->rto_carry_ms = txn->rto_ms;
			mgr->tdelay_carry_ms = txn->tdelay_ms;
		}
	}

	if (cls == MGCP_RC_PROVISIONAL) {
		txn->provisional = true;
		if (!txn->quiet) {
			txn->rto_ms = mgr->cfg.longtran_ms;
			proto_timer_arm(base_of(mgr), &txn->retx, txn->rto_ms);
		}
		txn_trace(mgr, "mgcp txn %u: provisional %03u", txid,
			  msg->code);
		return MGCP_RX_ABSORBED;
	}

	ack = mgcp_msg_param(msg, MGCP_PARAM_K) != NULL;
	if (ack)
		resp_ack_send(mgr, txid, src, src_len);
	else
		acks_add(mgr, txn, now);
	past_add(mgr, txid, src, src_len, ack, now);

	owner = txn->owner;
	up = !txn->detached;
	disc = txn->disc;
	txn_trace(mgr, "mgcp txn %u: final %03u after %u sends", txid,
		  msg->code, txn->sends);
	txn_free(txn);
	mgr->stat.delivered++;

	/*
	 * 4.4.7 page 144: a success response to the RSIP completes the
	 * procedure; an error response is a failed attempt, and the
	 * rate-limiting delay still applies.
	 */
	if (disc && mgr->disc.active && cls == MGCP_RC_SUCCESS) {
		mgr->disc.end_ms = now;
		mgr->disc.active = false;
		proto_timer_cancel(base_of(mgr), &mgr->disc.timer);
		txn_trace(mgr, "mgcp disconnected: ended by %03u", msg->code);
		if (env->disc_ended)
			env->disc_ended(env);
	} else if (disc && mgr->disc.active) {
		disc_attempt_end(mgr, true);
	}

	if (up && env->response)
		env->response(env, owner, txid, msg);
	if (up && env->done)
		env->done(env, owner, txid, MGCP_TXN_DONE_OK);

	return MGCP_RX_DELIVERED;
}

enum mgcp_txn_rx mgcp_txn_msg_recv(struct mgcp_txn_mgr *mgr,
				   const struct mgcp_msg *msg,
				   const struct sockaddr *src,
				   socklen_t src_len)
{
	struct sockaddr_storage sa;
	socklen_t sa_len;

	if (!mgr || !mgr->pool || !msg ||
	    !sa_store(&sa, &sa_len, src, src_len))
		return MGCP_RX_IGNORED;

	if (msg->is_command)
		return cmd_recv(mgr, msg, &sa, sa_len);

	return resp_recv(mgr, msg, &sa, sa_len);
}

unsigned int mgcp_txn_recv(struct mgcp_txn_mgr *mgr, const char *buf,
			   size_t len, const struct sockaddr *src,
			   socklen_t src_len)
{
	struct mgcp_split it;
	struct mgcp_msg msg;
	struct sip_str one;
	unsigned int n = 0;

	if (!mgr || !buf)
		return 0;

	mgcp_split_init(&it, buf, len);
	while (mgcp_split_next(&it, &one)) {
		if (mgcp_msg_parse(&msg, one.p, one.len) != MGCP_PARSE_OK) {
			mgr->stat.malformed++;
			continue;
		}
		n++;
		mgcp_txn_msg_recv(mgr, &msg, src, src_len);
	}

	return n;
}

/*
 * The message of a datagram whose response line names txid. 4.4.7 puts a
 * disconnected RSIP in front of the response, so it need not be the first.
 */
static bool resp_find(const char *wire, size_t len, uint32_t txid,
		      struct sip_str *out, struct mgcp_msg *m)
{
	struct mgcp_split it;
	struct sip_str one;

	mgcp_split_init(&it, wire, len);
	while (mgcp_split_next(&it, &one)) {
		if (mgcp_msg_parse(m, one.p, one.len) != MGCP_PARSE_OK)
			continue;
		if (m->is_command || m->code < 100 || m->txid != txid)
			continue;
		*out = one;
		return true;
	}

	return false;
}

/*
 * 3.5.6: a final that follows a provisional carries an empty ResponseAck.
 * The line goes right after the response line when the text lacks it. The
 * record buffer holds wire_max + HIST_K_LEN bytes, so the line always fits.
 */
static bool final_store(struct mgcp_txn_hist *h, const char *wire,
			size_t len)
{
	struct mgcp_msg m;
	struct sip_str one;
	const char *lf;
	size_t head;

	if (!h->prov_sent) {
		memcpy(h->wire, wire, len);
		h->len = len;
		return true;
	}
	if (!resp_find(wire, len, h->txid, &one, &m))
		return false;
	if (mgcp_msg_param(&m, MGCP_PARAM_K)) {
		memcpy(h->wire, wire, len);
		h->len = len;
		return true;
	}

	lf = memchr(one.p, '\n', one.len);
	if (!lf)
		return false;

	head = (size_t)(lf - wire) + 1;
	memcpy(h->wire, wire, head);
	memcpy(h->wire + head, HIST_K_LINE, HIST_K_LEN);
	memcpy(h->wire + head + HIST_K_LEN, wire + head, len - head);
	h->len = len + HIST_K_LEN;

	return true;
}

bool mgcp_txn_respond(struct mgcp_txn_mgr *mgr, uint32_t txid,
		      const struct sockaddr *src, socklen_t src_len,
		      uint32_t code, const void *wire, size_t len)
{
	struct sockaddr_storage sa;
	struct mgcp_txn_hist *h;
	socklen_t sa_len;
	bool final = code >= 200;

	if (!mgr || !mgr->hist || !wire || !len || code < 100 || code > 999)
		return false;
	if (len > mgr->cfg.wire_max) {
		mgr->stat.oversize++;
		return false;
	}

	h = hist_find(mgr, txid);
	if (!h) {
		if (!sa_store(&sa, &sa_len, src, src_len))
			return false;
		h = hist_open(mgr, txid, MGCP_VERB_UNKNOWN, &sa, sa_len);
	}
	if (h->state != MGCP_HIST_RUNNING && h->state != MGCP_HIST_PROVISIONAL)
		return false;

	if (!final) {
		memcpy(h->wire, wire, len);
		h->len = len;
		h->state = MGCP_HIST_PROVISIONAL;
		h->prov_sent = true;
		return wire_send(mgr, h->wire, h->len, &h->peer,
				 h->peer_len) >= 0;
	}

	if (!final_store(h, wire, len)) {
		txn_trace(mgr, "mgcp cmd %u: final names no response line",
			  txid);
		return false;
	}

	h->final_ms = now_ms(mgr);
	proto_timer_cancel(base_of(mgr), &h->timer);
	if (h->prov_sent) {
		h->state = MGCP_HIST_ACKWAIT;
		h->rto_ms = rto_base(mgr);
		h->tdelay_ms = h->rto_ms;
		proto_timer_arm(base_of(mgr), &h->timer, h->rto_ms);
	} else {
		h->state = MGCP_HIST_FINAL;
		proto_timer_arm(base_of(mgr), &h->timer, mgr->cfg.t_hist_ms);
	}

	return wire_send(mgr, h->wire, h->len, &h->peer, h->peer_len) >= 0;
}

/* ------------------------------------------------- disconnected procedure */

/*
 * 4.4.7 step 4 doubles the delay. NCS 1.5 7.4.3.6 allows any value from 1.5
 * to 2 times it, and MGCP_TXN_CFG_TD_RANDOM draws one there.
 */
static uint32_t td_next(struct mgcp_txn_mgr *mgr, uint32_t td)
{
	uint64_t next = (uint64_t)td * 2;

	if (mgr->cfg.flags & MGCP_TXN_CFG_TD_RANDOM)
		next = uniform(mgr, td + td / 2,
			       next > UINT32_MAX ? UINT32_MAX : (uint32_t)next);
	if (next > mgr->cfg.td_max_ms)
		next = mgr->cfg.td_max_ms;

	return (uint32_t)next;
}

/* the next delay waits for the end of the attempt (4.4.7 step 4) */
static void disc_fire(struct proto_timer *t)
{
	struct mgcp_txn_mgr *mgr = t->owner;
	struct mgcp_txn_env *env = mgr->env;

	txn_trace(mgr, "mgcp disconnected: attempt");
	if (env->disc_attempt)
		env->disc_attempt(env);
}

void mgcp_txn_disc_enter(struct mgcp_txn_mgr *mgr)
{
	uint64_t now;

	if (!mgr || mgr->disc.active)
		return;

	now = now_ms(mgr);
	mgr->disc.active = true;
	mgr->disc.entered_ms = now;
	mgr->disc.end_ms = now;
	mgr->disc.td_ms = uniform(mgr, TD_FIRST_MIN_MS, mgr->cfg.td_init_ms);
	proto_timer_arm(base_of(mgr), &mgr->disc.timer, mgr->disc.td_ms);
	txn_trace(mgr, "mgcp disconnected: entered, first attempt in %u ms",
		  mgr->disc.td_ms);
}

bool mgcp_txn_disc_kick(struct mgcp_txn_mgr *mgr, bool local)
{
	uint64_t now, since;

	if (!mgr || !mgr->disc.active)
		return false;

	now = now_ms(mgr);
	since = mgr->disc.end_ms > mgr->disc.entered_ms ?
		mgr->disc.end_ms : mgr->disc.entered_ms;
	if (local && now - since < mgr->cfg.td_min_ms)
		return false;

	proto_timer_cancel(base_of(mgr), &mgr->disc.timer);
	txn_trace(mgr, "mgcp disconnected: attempt on %s",
		  local ? "local activity" : "a command");

	return true;
}

void mgcp_txn_disc_leave(struct mgcp_txn_mgr *mgr)
{
	if (!mgr || !mgr->disc.active)
		return;

	mgr->disc.active = false;
	proto_timer_cancel(base_of(mgr), &mgr->disc.timer);
	txn_trace(mgr, "mgcp disconnected: left");
}

bool mgcp_txn_disc_active(const struct mgcp_txn_mgr *mgr)
{
	return mgr && mgr->disc.active;
}

void mgcp_txn_disc_fail(struct mgcp_txn_mgr *mgr)
{
	if (!mgr || !mgr->disc.active)
		return;

	disc_attempt_end(mgr, true);
}

/* ------------------------------------------------------------- manager */

static void mgr_free(struct mgcp_txn_mgr *mgr)
{
	free(mgr->pool);
	free(mgr->hist);
	free(mgr->past);
	free(mgr->wires);
	mgr->pool = NULL;
	mgr->hist = NULL;
	mgr->past = NULL;
	mgr->wires = NULL;
}

int mgcp_txn_mgr_init(struct mgcp_txn_mgr *mgr, struct mgcp_txn_env *env,
		      const struct mgcp_txn_cfg *cfg)
{
	unsigned int i;
	size_t slots;

	if (!mgr || !env || !env->send || !env->base.now_ms ||
	    !env->base.timer_set || !env->base.timer_cancel)
		return -EINVAL;

	memset(mgr, 0, sizeof(*mgr));
	mgr->env = env;
	if (cfg)
		mgr->cfg = *cfg;
	cfg_fixup(&mgr->cfg);

	slots = (size_t)mgr->cfg.pool_max * mgr->cfg.wire_max +
		(size_t)mgr->cfg.hist_max * (mgr->cfg.wire_max + HIST_K_LEN);
	mgr->pool = calloc(mgr->cfg.pool_max, sizeof(*mgr->pool));
	mgr->hist = calloc(mgr->cfg.hist_max, sizeof(*mgr->hist));
	mgr->past = calloc(mgr->cfg.hist_max, sizeof(*mgr->past));
	mgr->wires = malloc(slots);
	if (!mgr->pool || !mgr->hist || !mgr->past || !mgr->wires) {
		mgr_free(mgr);
		return -ENOMEM;
	}

	for (i = 0; i < mgr->cfg.pool_max; i++) {
		mgr->pool[i].index = i;
		mgr->pool[i].wire = mgr->wires + (size_t)i * mgr->cfg.wire_max;
		txn_slot_reset(mgr, &mgr->pool[i]);
	}
	for (i = 0; i < mgr->cfg.hist_max; i++) {
		struct mgcp_txn_hist *h = &mgr->hist[i];

		h->mgr = mgr;
		h->wire = mgr->wires +
			  (size_t)mgr->cfg.pool_max * mgr->cfg.wire_max +
			  (size_t)i * (mgr->cfg.wire_max + HIST_K_LEN);
		proto_timer_init(&h->timer, h, TMR_HIST, hist_fire);
	}
	proto_timer_init(&mgr->disc.timer, mgr, TMR_DISC, disc_fire);
	mgr->txid_next = mgr->cfg.txid_first;

	return 0;
}

void mgcp_txn_mgr_done(struct mgcp_txn_mgr *mgr)
{
	unsigned int i;

	if (!mgr || !mgr->pool)
		return;

	for (i = 0; i < mgr->cfg.pool_max; i++) {
		if (mgr->pool[i].in_use)
			txn_free(&mgr->pool[i]);
	}
	for (i = 0; i < mgr->cfg.hist_max; i++)
		hist_free(&mgr->hist[i]);
	proto_timer_cancel(base_of(mgr), &mgr->disc.timer);
	mgr->disc.active = false;

	mgr_free(mgr);
}
