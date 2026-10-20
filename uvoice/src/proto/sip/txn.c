// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * RFC 3261 section 17 transaction layer. See sip/txn.h for the contracts.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/avl-cmp.h>

#include "proto/shared/hdr.h"
#include "proto/sip/txn.h"

#define TXN_TRACE_MAX		240
#define TIMER_MS_MIN		10
#define TIMER_MS_MAX		600000

static const char *const timer_names[SIP_TIMER_ID_MAX] = {
	[SIP_TIMER_NONE] = "none",
	[SIP_TIMER_A] = "A",
	[SIP_TIMER_B] = "B",
	[SIP_TIMER_D] = "D",
	[SIP_TIMER_E] = "E",
	[SIP_TIMER_F] = "F",
	[SIP_TIMER_K] = "K",
	[SIP_TIMER_G] = "G",
	[SIP_TIMER_H] = "H",
	[SIP_TIMER_I] = "I",
	[SIP_TIMER_J] = "J",
	[SIP_TIMER_2XX] = "2xx",
	[SIP_TIMER_ACCEPT] = "accept",
	[SIP_TIMER_GLARE] = "glare",
	[SIP_TIMER_TRYING] = "trying",
	[SIP_TIMER_REG_REFRESH] = "reg-refresh",
	[SIP_TIMER_REG_RETRY] = "reg-retry",
	[SIP_TIMER_CALL_SESSION] = "call-session",
	[SIP_TIMER_CALL_WATCHDOG] = "call-watchdog",
	[SIP_TIMER_CALL_C] = "call-C",
	[SIP_TIMER_CALL_REL] = "call-rel",
	[SIP_TIMER_KA_CRLF] = "ka-crlf",
	[SIP_TIMER_KA_OPTIONS] = "ka-options",
	[SIP_TIMER_SUB_REFRESH] = "sub-refresh",
	[SIP_TIMER_SUB_RETRY] = "sub-retry",
};

const char *sip_timer_name(enum sip_timer_id id)
{
	if (id >= SIP_TIMER_ID_MAX || !timer_names[id])
		return "?";

	return timer_names[id];
}

const char *sip_txn_kind_name(enum sip_txn_kind kind)
{
	switch (kind) {
	case SIP_TXN_ICT:
		return "ict";
	case SIP_TXN_NICT:
		return "nict";
	case SIP_TXN_IST:
		return "ist";
	case SIP_TXN_NIST:
		return "nist";
	}

	return "?";
}

const char *sip_txn_state_name(enum sip_txn_state state)
{
	switch (state) {
	case SIP_TXN_S_CALLING:
		return "calling";
	case SIP_TXN_S_TRYING:
		return "trying";
	case SIP_TXN_S_PROCEEDING:
		return "proceeding";
	case SIP_TXN_S_COMPLETED:
		return "completed";
	case SIP_TXN_S_CONFIRMED:
		return "confirmed";
	case SIP_TXN_S_TERMINATED:
		return "terminated";
	}

	return "?";
}

const char *sip_txn_reason_name(enum sip_txn_reason reason)
{
	switch (reason) {
	case SIP_TXN_DONE_OK:
		return "ok";
	case SIP_TXN_DONE_TIMEOUT:
		return "timeout";
	case SIP_TXN_DONE_TRANSPORT:
		return "transport";
	case SIP_TXN_DONE_ABORT:
		return "abort";
	}

	return "?";
}

const char *sip_txn_rx_name(enum sip_txn_rx rx)
{
	switch (rx) {
	case SIP_RX_IGNORED:
		return "ignored";
	case SIP_RX_ABSORBED:
		return "absorbed";
	case SIP_RX_DELIVERED:
		return "delivered";
	case SIP_RX_REQUEST_NEW:
		return "request-new";
	case SIP_RX_ACK_2XX:
		return "ack-2xx";
	case SIP_RX_RESPONSE_STRAY:
		return "stray";
	}

	return "?";
}

/* ------------------------------------------------------------ timer config */

static uint32_t ms_clamp(uint32_t ms)
{
	if (ms < TIMER_MS_MIN)
		return TIMER_MS_MIN;
	if (ms > TIMER_MS_MAX)
		return TIMER_MS_MAX;

	return ms;
}

void sip_timer_cfg_fixup(struct sip_timer_cfg *tc)
{
	if (!tc->t1)
		tc->t1 = 500;
	if (!tc->t2)
		tc->t2 = 4000;
	if (!tc->t4)
		tc->t4 = 5000;

	tc->t1 = ms_clamp(tc->t1);
	tc->t2 = ms_clamp(tc->t2);
	tc->t4 = ms_clamp(tc->t4);
	if (tc->t2 < tc->t1)
		tc->t2 = tc->t1;

	if (!tc->a)
		tc->a = tc->t1;
	if (!tc->b)
		tc->b = 64 * tc->t1;
	if (!tc->d)
		tc->d = 32000;
	if (!tc->e)
		tc->e = tc->t1;
	if (!tc->f)
		tc->f = 64 * tc->t1;
	if (!tc->g)
		tc->g = tc->t1;
	if (!tc->h)
		tc->h = 64 * tc->t1;
	if (!tc->i)
		tc->i = tc->t4;
	if (!tc->j)
		tc->j = 64 * tc->t1;
	if (!tc->k)
		tc->k = tc->t4;

	tc->a = ms_clamp(tc->a);
	tc->b = ms_clamp(tc->b);
	tc->d = ms_clamp(tc->d);
	tc->e = ms_clamp(tc->e);
	tc->f = ms_clamp(tc->f);
	tc->g = ms_clamp(tc->g);
	tc->h = ms_clamp(tc->h);
	tc->i = ms_clamp(tc->i);
	tc->j = ms_clamp(tc->j);
	tc->k = ms_clamp(tc->k);
}

void sip_timer_cfg_default(struct sip_timer_cfg *tc)
{
	memset(tc, 0, sizeof(*tc));
	sip_timer_cfg_fixup(tc);
}

/* ------------------------------------------------------------ destinations */

static bool dst_port_set(struct sip_txn_dst *dst, uint16_t port)
{
	struct sockaddr_in *v4 = (struct sockaddr_in *)&dst->sa;
	struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&dst->sa;

	if (dst->sa.ss_family == AF_INET) {
		v4->sin_port = htons(port);
		return true;
	}
	if (dst->sa.ss_family == AF_INET6) {
		v6->sin6_port = htons(port);
		return true;
	}

	return false;
}

bool sip_dst_set_str(struct sip_txn_dst *dst, struct sip_str host, uint16_t port)
{
	char text[INET6_ADDRSTRLEN + 8];

	if (!sip_str_copy(text, sizeof(text), host))
		return false;

	return sip_dst_set(dst, text, port);
}

uint16_t sip_dst_port(const struct sip_txn_dst *dst)
{
	const struct sockaddr_in *v4 = (const struct sockaddr_in *)&dst->sa;
	const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&dst->sa;

	if (dst->sa.ss_family == AF_INET)
		return ntohs(v4->sin_port);
	if (dst->sa.ss_family == AF_INET6)
		return ntohs(v6->sin6_port);

	return 0;
}

bool sip_dst_from_via(const struct sip_msg *req, const struct sip_txn_dst *src,
		      struct sip_txn_dst *out)
{
	struct sip_via via;
	uint16_t port;

	if (!req || !src || !out)
		return false;
	if (!hdr_via_top(req, &via))
		return false;

	*out = *src;

	/*
	 * A via derived destination is never an accepted connection's tuple, so
	 * it must not inherit SIP_DST_F_NO_CONNECT from src: the sip/txn.h flag
	 * contract says a destination built elsewhere starts at "nothing is
	 * known".
	 */
	out->flags = 0;

	/* RFC 3581 section 4: rport means source address and source port. */
	if (via.flags & SIP_VIA_F_RPORT)
		return true;

	port = (via.flags & SIP_VIA_F_PORT) ? (uint16_t)via.port : SIP_PORT_DEFAULT;

	return dst_port_set(out, port);
}

/* -------------------------------------------------------------------- keys */

static void key_lower_add(struct sip_buf *b, struct sip_str s)
{
	size_t i;

	for (i = 0; i < s.len; i++) {
		char c = (char)tolower((unsigned char)s.p[i]);

		sip_buf_add_len(b, &c, 1);
	}
}

bool sip_str_nul_free(struct sip_str s)
{
	if (!s.len)
		return true;
	if (!s.p)
		return false;

	return memchr(s.p, 0, s.len) == NULL;
}

static bool key_method_add(struct sip_buf *b, enum sip_method method, struct sip_str name)
{
	if (method != SIP_METHOD_UNKNOWN) {
		sip_buf_add(b, sip_method_name(method));
		return true;
	}

	if (!sip_str_nul_free(name))
		return false;

	sip_buf_add_str(b, name);

	return true;
}

static bool key_client_build(char *out, size_t olen, struct sip_str branch,
			     enum sip_method method, struct sip_str name)
{
	struct sip_buf b;

	if (!branch.len || !sip_str_nul_free(branch))
		return false;

	sip_buf_init(&b, out, olen);
	sip_buf_add(&b, "c|");
	sip_buf_add_str(&b, branch);
	sip_buf_add(&b, "|");
	if (!key_method_add(&b, method, name))
		return false;

	return sip_buf_ok(&b);
}

/*
 * RFC 3261 17.2.3 keys a server transaction on the branch, the sent-by and the
 * method. A request whose branch carries no magic cookie predates RFC 3261.
 * The full pre-RFC 3261 rule of 17.2.3 needs the request URI and the To tag
 * as well; this layer settles for Call-ID, CSeq number and From tag, which is
 * enough to absorb a retransmission and to let a pre-RFC 3261 CANCEL find its
 * INVITE. Every deployed carrier sends the cookie.
 *
 * The pre-RFC 3261 rule is reserved strictly for a branch without the cookie.
 * A cookie branch that does not fit the key buffer fails here instead of
 * falling through, because the pre-RFC 3261 key ignores the branch and would
 * make two different branches of one dialog and CSeq share a transaction.
 */
static bool key_server_build(char *out, size_t olen, const struct sip_msg *req,
			     enum sip_method method, struct sip_str name)
{
	struct sip_buf b;
	struct sip_addr from;
	struct sip_str call_id;
	struct sip_via via;
	uint32_t cseq;
	bool cookie;

	/* A request without Via cannot be answered (RFC 3261 8.2.6.2), so it
	 * gets no transaction either. */
	if (!msg_hdr_first(req, SIP_HDR_VIA))
		return false;

	cookie = hdr_via_top(req, &via) && (via.flags & SIP_VIA_F_BRANCH) &&
		 (via.flags & SIP_VIA_F_MAGIC);

	sip_buf_init(&b, out, olen);

	if (cookie) {
		if (!sip_str_nul_free(via.branch) || !sip_str_nul_free(via.host))
			return false;

		sip_buf_add(&b, "s|");
		sip_buf_add_str(&b, via.branch);
		sip_buf_add(&b, "|");
		key_lower_add(&b, via.host);
		sip_buf_addf(&b, ":%u|",
			     (via.flags & SIP_VIA_F_PORT) ? via.port : SIP_PORT_DEFAULT);
		if (!key_method_add(&b, method, name))
			return false;

		return sip_buf_ok(&b);
	}

	if (!hdr_call_id_get(req, &call_id))
		return false;
	if (!hdr_cseq_get(req, &cseq, NULL))
		return false;
	if (!sip_str_nul_free(call_id))
		return false;

	sip_buf_add(&b, "l|");
	sip_buf_add_str(&b, call_id);
	sip_buf_addf(&b, "|%u|", cseq);
	if (hdr_addr_get(req, SIP_HDR_FROM, &from)) {
		if (!sip_str_nul_free(from.tag))
			return false;

		sip_buf_add_str(&b, from.tag);
	}
	sip_buf_add(&b, "|");
	if (!key_method_add(&b, method, name))
		return false;

	return sip_buf_ok(&b);
}

/*
 * The method a server transaction is keyed under. RFC 3261 17.2.3: an ACK
 * matches the transaction its INVITE created.
 */
static enum sip_method key_method(const struct sip_msg *req, struct sip_str *name)
{
	if (req->method == SIP_METHOD_ACK) {
		*name = sip_str_cstr("INVITE");
		return SIP_METHOD_INVITE;
	}

	*name = req->method_name;

	return req->method;
}

/* avl_find_element() needs the bare typeof, which -std=c11 does not have. */
static struct sip_txn *txn_find(struct avl_tree *tree, const char *key)
{
	struct avl_node *node = avl_find(tree, key);

	if (!node)
		return NULL;

	return container_of(node, struct sip_txn, node);
}

/* -------------------------------------------------------------- pool, trace */

static struct sip_txn *txn_alloc(struct sip_txn_mgr *mgr)
{
	unsigned int i;

	for (i = 0; i < SIP_TXN_MAX; i++) {
		struct sip_txn *txn = &mgr->pool[i];
		uint16_t gen = txn->gen;

		if (txn->in_use)
			continue;

		memset(txn, 0, sizeof(*txn));
		txn->mgr = mgr;
		txn->index = i;
		txn->gen = gen ? gen : 1;
		txn->in_use = true;
		mgr->active++;
		mgr->stat.created++;

		return txn;
	}

	mgr->stat.denied++;

	return NULL;
}

static void txn_unindex(struct sip_txn *txn)
{
	if (!txn->indexed)
		return;

	avl_delete(txn->client ? &txn->mgr->client : &txn->mgr->server, &txn->node);
	txn->indexed = false;
}

static void txn_free(struct sip_txn *txn)
{
	free(txn->req);
	free(txn->rsp);
	txn->req = NULL;
	txn->rsp = NULL;
	txn->req_len = 0;
	txn->rsp_len = 0;
	txn->in_use = false;
	txn->gen++;
	if (!txn->gen)
		txn->gen = 1;
	txn->mgr->active--;
}

uint32_t sip_txn_handle(const struct sip_txn *txn)
{
	if (!txn || !txn->in_use)
		return SIP_HANDLE_NONE;

	return sip_handle_make(txn->index, txn->gen);
}

struct sip_txn *sip_txn_get(struct sip_txn_mgr *mgr, uint32_t handle)
{
	unsigned int index = sip_handle_index(handle);
	struct sip_txn *txn;

	if (handle == SIP_HANDLE_NONE || index >= SIP_TXN_MAX)
		return NULL;

	txn = &mgr->pool[index];
	if (!txn->in_use || txn->gen != sip_handle_gen(handle))
		return NULL;

	return txn;
}

const char *sip_txn_branch(const struct sip_txn *txn)
{
	return txn->branch;
}

const struct sip_txn_dst *sip_txn_dst_get(const struct sip_txn *txn)
{
	return &txn->dst;
}

unsigned int sip_txn_count(const struct sip_txn_mgr *mgr)
{
	return mgr->active;
}

void sip_txn_env_trace(struct sip_txn_env *env, const char *fmt, va_list ap,
		       const char *prefix_fmt, ...)
{
	char line[TXN_TRACE_MAX];
	va_list pap;
	int n;

	va_start(pap, prefix_fmt);
	n = vsnprintf(line, sizeof(line), prefix_fmt, pap);
	va_end(pap);
	if (n < 0 || (size_t)n >= sizeof(line))
		return;

	vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);

	env->base.trace(&env->base, line);
}

static void txn_trace(const struct sip_txn *txn, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void txn_trace(const struct sip_txn *txn, const char *fmt, ...)
{
	struct sip_txn_env *env = txn->mgr->env;
	va_list ap;

	if (!env->base.trace)
		return;

	va_start(ap, fmt);
	sip_txn_env_trace(env, fmt, ap, "txn %u %s/%s %s: ", txn->index,
			  sip_txn_kind_name(txn->kind),
			  sip_method_name(txn->method),
			  sip_txn_state_name(txn->state));
	va_end(ap);
}

uint32_t sip_txn_rand(struct sip_txn_mgr *mgr)
{
	struct sip_txn_env *env = mgr->env;
	uint32_t x;

	if (env->base.rand32)
		return env->base.rand32(&env->base);

	/* xorshift32; the seed is the clock, which is all a branch token needs
	 * on a device with no entropy pool at boot. */
	if (!mgr->rand_state) {
		uint64_t now = env->base.now_ms ? env->base.now_ms(&env->base) : 0;

		mgr->rand_state = (uint32_t)(now ^ (now >> 32)) | 1u;
	}

	x = mgr->rand_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	mgr->rand_state = x;

	return x;
}

bool sip_branch_new(struct sip_txn_mgr *mgr, char *out, size_t olen)
{
	if (olen < SIP_BRANCH_MAX)
		return false;

	mgr->branch_seq++;
	snprintf(out, olen, "%s%08x%08x", SIP_BRANCH_MAGIC, sip_txn_rand(mgr),
		 mgr->branch_seq);

	return true;
}

/* ------------------------------------------------------------ wire buffers */

static bool wire_store(char **dst, size_t *dlen, const void *buf, size_t len)
{
	char *mem;

	if (!buf || !len || len > SIP_MSG_LEN_MAX)
		return false;

	mem = malloc(len);
	if (!mem)
		return false;

	memcpy(mem, buf, len);
	free(*dst);
	*dst = mem;
	*dlen = len;

	return true;
}

static bool txn_wire_send(struct sip_txn *txn, const char *buf, size_t len)
{
	struct sip_txn_env *env = txn->mgr->env;

	if (!buf || !len)
		return false;

	txn->tx_count++;
	if (env->send(env, buf, len, &txn->dst) >= 0)
		return true;

	txn->mgr->stat.transport++;
	txn_trace(txn, "transport error after %u sends", txn->tx_count);

	return false;
}

/*
 * Answer a retransmission from the peer. An empty slot is not an error: an
 * INVITE that arrives again before the TU produced its first provisional has
 * nothing to answer with.
 */
static bool txn_rsp_resend(struct sip_txn *txn)
{
	if (!txn->rsp || !txn->rsp_len) {
		txn_trace(txn, "retransmission with nothing to answer");
		return true;
	}

	return txn_wire_send(txn, txn->rsp, txn->rsp_len);
}

/* --------------------------------------------------------- state machinery */

static void txn_state_set(struct sip_txn *txn, enum sip_txn_state state)
{
	txn->state = (uint8_t)state;
	txn_trace(txn, "entered");
}

static void txn_timers_stop(struct sip_txn *txn)
{
	sip_timer_cancel(txn->mgr->env, &txn->retx);
	sip_timer_cancel(txn->mgr->env, &txn->life);
}

/*
 * Remove a transaction that never reached the peer. The caller learns the
 * failure from the return value of the call it made, so there is no done()
 * upcall and no dangling pointer left behind.
 */
static void txn_drop(struct sip_txn *txn)
{
	txn->state = SIP_TXN_S_TERMINATED;
	txn_timers_stop(txn);
	txn_unindex(txn);
	txn_free(txn);
}

static void txn_terminate(struct sip_txn *txn, enum sip_txn_reason reason)
{
	struct sip_txn_env *env = txn->mgr->env;
	uint32_t owner = txn->owner;
	uint32_t handle = sip_txn_handle(txn);
	bool up = !txn->detached && env->done;

	if (txn->state == SIP_TXN_S_TERMINATED)
		return;

	txn->state = SIP_TXN_S_TERMINATED;
	sip_timer_cancel(env, &txn->retx);
	sip_timer_cancel(env, &txn->life);
	txn_unindex(txn);
	txn_trace(txn, "terminated (%s)", sip_txn_reason_name(reason));

	if (reason == SIP_TXN_DONE_TIMEOUT)
		txn->mgr->stat.timeout++;

	txn_free(txn);
	if (up)
		env->done(env, owner, handle, reason);
}

static void txn_timer_arm(struct sip_txn *txn, struct sip_timer *t, enum sip_timer_id id,
			  unsigned int ms, sip_timer_cb cb)
{
	t->owner = txn;
	t->id = (uint8_t)id;
	t->fire = cb;
	sip_timer_arm(txn->mgr->env, t, ms);
	txn_trace(txn, "timer %s armed %u ms", sip_timer_name(id), ms);
}

static bool txn_reliable(const struct sip_txn_mgr *mgr)
{
	return (mgr->env->flags & SIP_TXN_ENV_RELIABLE) != 0;
}

/*
 * The wait timers D, K, I and J run to 0 ms on a reliable transport: no
 * retransmission of a request, a final response or an ACK reaches this layer,
 * so nothing has to be absorbed (RFC 3261 17.1.1.2, 17.1.2.2, 17.2.1, 17.2.2).
 * A 0 ms arm still fires on the next tick, so the transaction ends from a timer
 * callback and never inline.
 */
static unsigned int txn_wait_ms(const struct sip_txn_mgr *mgr, unsigned int ms)
{
	return txn_reliable(mgr) ? 0 : ms;
}

static void txn_resp_up(struct sip_txn *txn, const struct sip_msg *msg)
{
	struct sip_txn_env *env = txn->mgr->env;

	txn->mgr->stat.delivered++;
	if (txn->detached || !env->resp)
		return;

	env->resp(env, txn->owner, sip_txn_handle(txn), msg);
}

static void txn_timer_timeout(struct sip_timer *t)
{
	txn_terminate(t->owner, SIP_TXN_DONE_TIMEOUT);
}

static void txn_timer_end(struct sip_timer *t)
{
	txn_terminate(t->owner, SIP_TXN_DONE_OK);
}

/* RFC 3261 17.1.2.2, 17.2.1: timers E and G double up to T2. */
static uint32_t retx_double(uint32_t ms, uint32_t t2)
{
	return ms * 2 > t2 ? t2 : ms * 2;
}

/* ------------------------------------------------- INVITE client (17.1.1) */

static void ict_timer_a(struct sip_timer *t)
{
	struct sip_txn *txn = t->owner;

	if (!txn_wire_send(txn, txn->req, txn->req_len)) {
		txn_terminate(txn, SIP_TXN_DONE_TRANSPORT);
		return;
	}

	/* RFC 3261 17.1.1.2: the interval doubles on every retransmission and
	 * is not capped; timer B ends the series. */
	txn->retx_ms *= 2;
	txn_timer_arm(txn, &txn->retx, SIP_TIMER_A, txn->retx_ms, ict_timer_a);
}

static enum sip_txn_rx ict_resp(struct sip_txn *txn, const struct sip_msg *msg)
{
	struct sip_txn_mgr *mgr = txn->mgr;
	uint32_t code = msg->status;
	uint32_t handle;

	if (txn->state == SIP_TXN_S_COMPLETED) {
		/* RFC 3261 17.1.1.2: the ACK answers every retransmission of
		 * the final response; the TU never sees it again. */
		mgr->stat.absorbed++;
		if (!txn_rsp_resend(txn))
			txn_terminate(txn, SIP_TXN_DONE_TRANSPORT);
		return SIP_RX_ABSORBED;
	}

	if (txn->state != SIP_TXN_S_CALLING && txn->state != SIP_TXN_S_PROCEEDING) {
		mgr->stat.absorbed++;
		return SIP_RX_ABSORBED;
	}

	if (code < 200) {
		txn_timers_stop(txn);
		txn_state_set(txn, SIP_TXN_S_PROCEEDING);
		txn_resp_up(txn, msg);
		return SIP_RX_DELIVERED;
	}

	txn_timers_stop(txn);
	txn->code = code;

	if (code < 300) {
		/* RFC 3261 17.1.1.2: a 2xx ends the transaction at once. The
		 * ACK and any retransmitted 2xx belong to the TU (ua.c). */
		handle = sip_txn_handle(txn);
		txn_resp_up(txn, msg);
		txn = sip_txn_get(mgr, handle);
		if (txn)
			txn_terminate(txn, SIP_TXN_DONE_OK);
		return SIP_RX_DELIVERED;
	}

	txn_state_set(txn, SIP_TXN_S_COMPLETED);
	txn_timer_arm(txn, &txn->life, SIP_TIMER_D, txn_wait_ms(mgr, mgr->tc.d),
		      txn_timer_end);
	txn_resp_up(txn, msg);

	return SIP_RX_DELIVERED;
}

bool sip_txn_ack_set(struct sip_txn *txn, const void *wire, size_t len)
{
	if (!txn || txn->kind != SIP_TXN_ICT || txn->state != SIP_TXN_S_COMPLETED)
		return false;
	if (!wire_store(&txn->rsp, &txn->rsp_len, wire, len))
		return false;

	txn_trace(txn, "ack stored, %zu bytes", len);
	if (txn_wire_send(txn, txn->rsp, txn->rsp_len))
		return true;

	/*
	 * The first ACK never reached the peer, so timer D would sit out its
	 * 32 s and then report a completion the caller already knows failed.
	 * The caller learns it from this return value.
	 */
	txn_drop(txn);

	return false;
}

/* --------------------------------------------- non-INVITE client (17.1.2) */

static void nict_timer_e(struct sip_timer *t)
{
	struct sip_txn *txn = t->owner;
	uint32_t t2 = txn->mgr->tc.t2;

	if (!txn_wire_send(txn, txn->req, txn->req_len)) {
		txn_terminate(txn, SIP_TXN_DONE_TRANSPORT);
		return;
	}

	/* RFC 3261 17.1.2.2: the interval doubles up to T2 while trying and
	 * stays at T2 once a provisional response arrived. */
	if (txn->state == SIP_TXN_S_PROCEEDING)
		txn->retx_ms = t2;
	else
		txn->retx_ms = retx_double(txn->retx_ms, t2);

	txn_timer_arm(txn, &txn->retx, SIP_TIMER_E, txn->retx_ms, nict_timer_e);
}

static enum sip_txn_rx nict_resp(struct sip_txn *txn, const struct sip_msg *msg)
{
	struct sip_txn_mgr *mgr = txn->mgr;
	uint32_t code = msg->status;

	if (txn->state != SIP_TXN_S_TRYING && txn->state != SIP_TXN_S_PROCEEDING) {
		mgr->stat.absorbed++;
		return SIP_RX_ABSORBED;
	}

	if (code < 200) {
		txn_state_set(txn, SIP_TXN_S_PROCEEDING);
		txn_resp_up(txn, msg);
		return SIP_RX_DELIVERED;
	}

	txn_timers_stop(txn);
	txn->code = code;
	txn_state_set(txn, SIP_TXN_S_COMPLETED);
	txn_timer_arm(txn, &txn->life, SIP_TIMER_K, txn_wait_ms(mgr, mgr->tc.k),
		      txn_timer_end);
	txn_resp_up(txn, msg);

	return SIP_RX_DELIVERED;
}

/* ------------------------------------------------- INVITE server (17.2.1) */

static void ist_timer_g(struct sip_timer *t)
{
	struct sip_txn *txn = t->owner;
	uint32_t t2 = txn->mgr->tc.t2;

	if (!txn_wire_send(txn, txn->rsp, txn->rsp_len)) {
		txn_terminate(txn, SIP_TXN_DONE_TRANSPORT);
		return;
	}

	txn->retx_ms = retx_double(txn->retx_ms, t2);
	txn_timer_arm(txn, &txn->retx, SIP_TIMER_G, txn->retx_ms, ist_timer_g);
}

static enum sip_txn_rx ist_ack(struct sip_txn *txn)
{
	struct sip_txn_mgr *mgr = txn->mgr;

	mgr->stat.absorbed++;

	if (txn->state != SIP_TXN_S_COMPLETED) {
		/* An ACK in Proceeding answers a final we never sent, and one
		 * in Confirmed is a retransmission: both are absorbed. */
		txn_trace(txn, "ack absorbed");
		return SIP_RX_ABSORBED;
	}

	txn_timers_stop(txn);
	txn_state_set(txn, SIP_TXN_S_CONFIRMED);
	txn_timer_arm(txn, &txn->life, SIP_TIMER_I, txn_wait_ms(mgr, mgr->tc.i),
		      txn_timer_end);

	return SIP_RX_ABSORBED;
}

static enum sip_txn_rx ist_request(struct sip_txn *txn, const struct sip_msg *req)
{
	if (req->method == SIP_METHOD_ACK)
		return ist_ack(txn);

	txn->mgr->stat.absorbed++;
	if (txn->state != SIP_TXN_S_PROCEEDING && txn->state != SIP_TXN_S_COMPLETED)
		return SIP_RX_ABSORBED;

	if (!txn_rsp_resend(txn))
		txn_terminate(txn, SIP_TXN_DONE_TRANSPORT);

	return SIP_RX_ABSORBED;
}

/* --------------------------------------------- non-INVITE server (17.2.2) */

static enum sip_txn_rx nist_request(struct sip_txn *txn)
{
	txn->mgr->stat.absorbed++;

	/* RFC 3261 17.2.2: nothing goes out while trying, because no response
	 * exists yet. */
	if (txn->state != SIP_TXN_S_PROCEEDING && txn->state != SIP_TXN_S_COMPLETED)
		return SIP_RX_ABSORBED;

	if (!txn_rsp_resend(txn))
		txn_terminate(txn, SIP_TXN_DONE_TRANSPORT);

	return SIP_RX_ABSORBED;
}

/* --------------------------------------------------------------- receive */

static enum sip_txn_rx resp_recv(struct sip_txn_mgr *mgr, const struct sip_msg *msg,
				 struct sip_txn **out)
{
	char key[SIP_TXN_KEY_MAX];
	struct sip_str name = { NULL, 0 };
	struct sip_str cseq;
	struct sip_via via;
	enum sip_method method;
	enum sip_txn_rx rx;
	uint32_t num, handle;
	struct sip_txn *txn;

	if (!hdr_via_top(msg, &via) || !(via.flags & SIP_VIA_F_BRANCH))
		return SIP_RX_IGNORED;
	if (!msg_hdr_value(msg, SIP_HDR_CSEQ, &cseq))
		return SIP_RX_IGNORED;
	if (!hdr_cseq_parse(cseq, &num, &method, &name))
		return SIP_RX_IGNORED;
	if (method == SIP_METHOD_ACK)
		return SIP_RX_IGNORED;
	if (!key_client_build(key, sizeof(key), via.branch, method, name))
		return SIP_RX_IGNORED;

	txn = txn_find(&mgr->client, key);
	if (!txn) {
		mgr->stat.stray++;
		return SIP_RX_RESPONSE_STRAY;
	}

	handle = sip_txn_handle(txn);
	rx = txn->kind == SIP_TXN_ICT ? ict_resp(txn, msg) : nict_resp(txn, msg);
	if (out)
		*out = sip_txn_get(mgr, handle);

	return rx;
}

static enum sip_txn_rx req_recv(struct sip_txn_mgr *mgr, const struct sip_msg *msg,
				struct sip_txn **out)
{
	char key[SIP_TXN_KEY_MAX];
	struct sip_str name;
	enum sip_method method;
	enum sip_txn_rx rx;
	uint32_t handle;
	struct sip_txn *txn;

	method = key_method(msg, &name);
	if (!key_server_build(key, sizeof(key), msg, method, name))
		return SIP_RX_IGNORED;

	txn = txn_find(&mgr->server, key);
	if (!txn) {
		/* The ACK of a 2xx cannot match the INVITE server transaction,
		 * because RFC 3261 17.2.1 terminated it when the 2xx went out.
		 * It belongs to the dialog layer (ua.c). */
		if (msg->method == SIP_METHOD_ACK)
			return SIP_RX_ACK_2XX;
		return SIP_RX_REQUEST_NEW;
	}

	handle = sip_txn_handle(txn);
	rx = txn->kind == SIP_TXN_IST ? ist_request(txn, msg) : nist_request(txn);
	if (out)
		*out = sip_txn_get(mgr, handle);

	return rx;
}

enum sip_txn_rx sip_txn_msg_recv(struct sip_txn_mgr *mgr, const struct sip_msg *msg,
				 struct sip_txn **out)
{
	if (out)
		*out = NULL;
	if (!mgr || !msg)
		return SIP_RX_IGNORED;

	if (msg->is_request)
		return req_recv(mgr, msg, out);

	return resp_recv(mgr, msg, out);
}

uint32_t sip_txn_cancel_target(struct sip_txn_mgr *mgr, const struct sip_msg *cancel)
{
	char key[SIP_TXN_KEY_MAX];
	struct sip_txn *txn;

	if (!mgr || !cancel || !cancel->is_request)
		return SIP_HANDLE_NONE;
	if (cancel->method != SIP_METHOD_CANCEL)
		return SIP_HANDLE_NONE;
	if (!key_server_build(key, sizeof(key), cancel, SIP_METHOD_INVITE,
			      sip_str_cstr("INVITE")))
		return SIP_HANDLE_NONE;

	txn = txn_find(&mgr->server, key);
	if (!txn || txn->kind != SIP_TXN_IST)
		return SIP_HANDLE_NONE;

	return sip_txn_handle(txn);
}

/* ----------------------------------------------------------- construction */

static bool txn_key_set(struct sip_txn *txn, const char *key)
{
	size_t len = strlen(key);

	if (len >= sizeof(txn->key))
		return false;

	memcpy(txn->key, key, len + 1);
	txn->node.key = txn->key;

	return true;
}

struct sip_txn *sip_txn_client_start(struct sip_txn_mgr *mgr, enum sip_method method,
				     struct sip_str branch, const void *wire, size_t len,
				     const struct sip_txn_dst *dst, uint32_t owner)
{
	char key[SIP_TXN_KEY_MAX];
	struct sip_txn *txn;
	bool invite = method == SIP_METHOD_INVITE;

	if (!mgr || !dst || method == SIP_METHOD_ACK || method == SIP_METHOD_UNKNOWN)
		return NULL;
	if (!key_client_build(key, sizeof(key), branch, method, sip_str_init(NULL, 0)))
		return NULL;
	if (txn_find(&mgr->client, key))
		return NULL;

	txn = txn_alloc(mgr);
	if (!txn)
		return NULL;

	txn->client = true;
	txn->kind = invite ? SIP_TXN_ICT : SIP_TXN_NICT;
	txn->state = invite ? SIP_TXN_S_CALLING : SIP_TXN_S_TRYING;
	txn->method = (uint8_t)method;
	txn->owner = owner;
	txn->dst = *dst;

	if (!sip_str_copy(txn->branch, sizeof(txn->branch), branch) ||
	    !txn_key_set(txn, key) ||
	    !wire_store(&txn->req, &txn->req_len, wire, len)) {
		txn_drop(txn);
		return NULL;
	}
	if (avl_insert(&mgr->client, &txn->node) < 0) {
		txn_drop(txn);
		return NULL;
	}
	txn->indexed = true;

	if (!txn_wire_send(txn, txn->req, txn->req_len)) {
		txn_drop(txn);
		return NULL;
	}

	txn->retx_ms = invite ? mgr->tc.a : mgr->tc.e;
	/* RFC 3261 17.1.1.2, 17.1.2.2: a reliable transport never retransmits
	 * the request, so timer A/E is not armed; timer B/F is unchanged. */
	if (!txn_reliable(mgr))
		txn_timer_arm(txn, &txn->retx, invite ? SIP_TIMER_A : SIP_TIMER_E,
			      txn->retx_ms, invite ? ict_timer_a : nict_timer_e);
	txn_timer_arm(txn, &txn->life, invite ? SIP_TIMER_B : SIP_TIMER_F,
		      invite ? mgr->tc.b : mgr->tc.f, txn_timer_timeout);

	return txn;
}

struct sip_txn *sip_txn_server_start(struct sip_txn_mgr *mgr, const struct sip_msg *req,
				     const struct sip_txn_dst *src, uint32_t owner)
{
	char key[SIP_TXN_KEY_MAX];
	struct sip_str name;
	struct sip_via via;
	enum sip_method method;
	struct sip_txn *txn;
	bool invite;

	if (!mgr || !req || !req->is_request || !src)
		return NULL;
	if (req->method == SIP_METHOD_ACK)
		return NULL;

	method = key_method(req, &name);
	if (!key_server_build(key, sizeof(key), req, method, name))
		return NULL;
	if (txn_find(&mgr->server, key))
		return NULL;

	txn = txn_alloc(mgr);
	if (!txn)
		return NULL;

	invite = req->method == SIP_METHOD_INVITE;
	txn->kind = invite ? SIP_TXN_IST : SIP_TXN_NIST;
	txn->state = invite ? SIP_TXN_S_PROCEEDING : SIP_TXN_S_TRYING;
	txn->method = (uint8_t)req->method;
	txn->owner = owner;

	if (hdr_via_top(req, &via))
		sip_str_copy(txn->branch, sizeof(txn->branch), via.branch);

	/* RFC 3261 18.2.2: a reliable transport answers on the connection the
	 * request arrived on, so src is the response destination verbatim; UDP
	 * still derives it from the top Via sent-by. */
	if (txn_reliable(mgr)) {
		txn->dst = *src;
	} else if (!sip_dst_from_via(req, src, &txn->dst)) {
		txn_drop(txn);
		return NULL;
	}
	if (!txn_key_set(txn, key) ||
	    !wire_store(&txn->req, &txn->req_len, req->buf, req->len)) {
		txn_drop(txn);
		return NULL;
	}
	if (avl_insert(&mgr->server, &txn->node) < 0) {
		txn_drop(txn);
		return NULL;
	}
	txn->indexed = true;
	txn_trace(txn, "server transaction started");

	return txn;
}

bool sip_txn_request_get(struct sip_txn *txn, struct sip_msg *out)
{
	if (!txn || !out || !txn->req)
		return false;

	return msg_parse(out, txn->req, txn->req_len) == SIP_PARSE_OK;
}

static bool ist_response_send(struct sip_txn *txn, uint32_t code, const void *wire, size_t len)
{
	struct sip_txn_mgr *mgr = txn->mgr;
	uint32_t handle;

	if (txn->state != SIP_TXN_S_PROCEEDING)
		return false;
	if (!wire_store(&txn->rsp, &txn->rsp_len, wire, len))
		return false;

	txn->code = code;

	if (code >= 200 && code < 300) {
		/* RFC 3261 17.2.1: a 2xx ends the server transaction; ua.c
		 * owns its retransmission until the ACK arrives. */
		handle = sip_txn_handle(txn);
		if (!txn_wire_send(txn, txn->rsp, txn->rsp_len)) {
			txn_drop(txn);
			return false;
		}
		txn = sip_txn_get(mgr, handle);
		if (txn)
			txn_terminate(txn, SIP_TXN_DONE_OK);
		return true;
	}

	if (!txn_wire_send(txn, txn->rsp, txn->rsp_len)) {
		txn_drop(txn);
		return false;
	}

	if (code < 200)
		return true;

	txn_state_set(txn, SIP_TXN_S_COMPLETED);
	txn->retx_ms = mgr->tc.g;
	/* RFC 3261 17.2.1: a reliable transport never retransmits the final
	 * response, so timer G is not armed; timer H is unchanged. */
	if (!txn_reliable(mgr))
		txn_timer_arm(txn, &txn->retx, SIP_TIMER_G, txn->retx_ms, ist_timer_g);
	txn_timer_arm(txn, &txn->life, SIP_TIMER_H, mgr->tc.h,
		      txn_timer_timeout);

	return true;
}

static bool nist_response_send(struct sip_txn *txn, uint32_t code, const void *wire, size_t len)
{
	struct sip_txn_mgr *mgr = txn->mgr;

	if (txn->state != SIP_TXN_S_TRYING && txn->state != SIP_TXN_S_PROCEEDING)
		return false;
	if (!wire_store(&txn->rsp, &txn->rsp_len, wire, len))
		return false;

	txn->code = code;
	if (!txn_wire_send(txn, txn->rsp, txn->rsp_len)) {
		txn_drop(txn);
		return false;
	}

	if (code < 200) {
		txn_state_set(txn, SIP_TXN_S_PROCEEDING);
		return true;
	}

	txn_state_set(txn, SIP_TXN_S_COMPLETED);
	txn_timer_arm(txn, &txn->life, SIP_TIMER_J, txn_wait_ms(mgr, mgr->tc.j),
		      txn_timer_end);

	return true;
}

bool sip_txn_response_send(struct sip_txn *txn, uint32_t code, const void *wire, size_t len)
{
	if (!txn || txn->client || code < 100 || code > 699)
		return false;

	if (txn->kind == SIP_TXN_IST)
		return ist_response_send(txn, code, wire, len);

	return nist_response_send(txn, code, wire, len);
}

bool sip_txn_response_resend(struct sip_txn *txn)
{
	if (!txn || txn->kind != SIP_TXN_IST ||
	    txn->state != SIP_TXN_S_PROCEEDING)
		return false;

	if (txn_rsp_resend(txn))
		return true;

	txn_terminate(txn, SIP_TXN_DONE_TRANSPORT);

	return false;
}

void sip_txn_abort(struct sip_txn *txn)
{
	if (!txn || !txn->in_use)
		return;

	txn_terminate(txn, SIP_TXN_DONE_ABORT);
}

void sip_txn_discard(struct sip_txn *txn)
{
	if (!txn || !txn->in_use)
		return;

	txn_trace(txn, "discarded");
	txn_drop(txn);
}

unsigned int sip_txn_owner_release(struct sip_txn_mgr *mgr, uint32_t owner)
{
	unsigned int i, n = 0;

	if (owner == SIP_HANDLE_NONE)
		return 0;

	for (i = 0; i < SIP_TXN_MAX; i++) {
		struct sip_txn *txn = &mgr->pool[i];

		if (!txn->in_use || txn->owner != owner)
			continue;

		txn->owner = SIP_HANDLE_NONE;
		txn->detached = true;
		n++;
	}

	return n;
}

/* --------------------------------------------------------------- manager */

void sip_txn_mgr_init(struct sip_txn_mgr *mgr, struct sip_txn_env *env,
		      const struct sip_timer_cfg *tc)
{
	memset(mgr, 0, sizeof(*mgr));
	mgr->env = env;
	sip_txn_mgr_timers_set(mgr, tc);
	avl_init(&mgr->client, avl_strcmp, false, NULL);
	avl_init(&mgr->server, avl_strcmp, false, NULL);
}

void sip_txn_mgr_timers_set(struct sip_txn_mgr *mgr, const struct sip_timer_cfg *tc)
{
	if (tc)
		mgr->tc = *tc;
	else
		memset(&mgr->tc, 0, sizeof(mgr->tc));

	sip_timer_cfg_fixup(&mgr->tc);
}

const struct sip_timer_cfg *sip_txn_mgr_timers_get(
	const struct sip_txn_mgr *mgr)
{
	return &mgr->tc;
}

void sip_txn_mgr_done(struct sip_txn_mgr *mgr)
{
	unsigned int i;

	for (i = 0; i < SIP_TXN_MAX; i++) {
		struct sip_txn *txn = &mgr->pool[i];

		if (!txn->in_use)
			continue;

		txn->detached = true;
		txn_terminate(txn, SIP_TXN_DONE_ABORT);
	}
}
