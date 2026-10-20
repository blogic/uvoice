// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Per account keepalive: the CRLF NAT keepalive and the OPTIONS ping. See
 * sip/keepalive.h for the contracts.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "proto/shared/hdr.h"
#include "proto/sip/keepalive.h"

static void ka_trace(struct sip_ka *ka, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void ka_trace(struct sip_ka *ka, const char *fmt, ...)
{
	struct sip_txn_env *env = ka->env;
	va_list ap;

	if (!env->base.trace)
		return;

	va_start(ap, fmt);
	sip_txn_env_trace(env, fmt, ap, "ka %s: ",
			  ka->cfg.aor ? ka->cfg.aor : "?");
	va_end(ap);
}

/* ---------------------------------------------------------------- config */

static void cfg_take(struct sip_ka *ka, const struct sip_ka_cfg *cfg)
{
	ka->cfg = *cfg;
	if (!ka->cfg.local_port)
		ka->cfg.local_port = SIP_PORT_DEFAULT;
}

/*
 * A configured interval goes to a millisecond timer, so an option a carrier or
 * an operator wrote as a very large number must not overflow the conversion and
 * fire almost at once, which is the opposite of what it asked for. 0 keeps its
 * meaning of OFF and never becomes the minimum.
 */
static uint32_t interval_clamp(uint32_t secs)
{
	if (!secs)
		return 0;
	if (secs < SIP_KA_INTERVAL_MIN)
		return SIP_KA_INTERVAL_MIN;
	if (secs > SIP_KA_INTERVAL_MAX)
		return SIP_KA_INTERVAL_MAX;

	return secs;
}

static uint32_t crlf_secs(const struct sip_ka *ka)
{
	if (!ka->cfg.crlf_enabled)
		return 0;

	return interval_clamp(ka->cfg.crlf_interval_s);
}

/*
 * An OPTIONS needs an address of record, a request URI and a Via sent-by. A
 * configuration that carries none of them cannot build the request at all, so
 * the side stays off instead of arming a timer that can only fail.
 */
static bool options_cfg_ok(const struct sip_ka *ka)
{
	return ka->cfg.aor && ka->cfg.registrar && ka->cfg.local_host;
}

static uint32_t options_secs(const struct sip_ka *ka)
{
	if (!options_cfg_ok(ka))
		return 0;

	return interval_clamp(ka->cfg.options_interval_s);
}

/* ---------------------------------------------------------------- timers */

static void timer_rearm(struct sip_ka *ka, struct sip_timer *t, uint32_t secs)
{
	if (!ka->running || !secs) {
		sip_timer_cancel(ka->env, t);
		return;
	}

	sip_timer_arm(ka->env, t, secs * 1000);
}

static uint64_t ka_now(struct sip_ka *ka)
{
	return ka->env->base.now_ms(&ka->env->base);
}

static const struct sip_txn_dst *dst_at(struct sip_ka *ka, unsigned int idx)
{
	return ka->dst_at ? ka->dst_at(ka, ka->priv, idx) : NULL;
}

/* ------------------------------------------------------- CRLF NAT keepalive */

static void ping_send(struct sip_ka *ka, const struct sip_txn_dst *dst,
		      unsigned int idx)
{
	if (ka->env->send(ka->env, SIP_KA_PING, SIP_KA_PING_LEN, dst) < 0) {
		ka->stat.crlf_failed++;
		ka_trace(ka, "crlf %u refused by the transport", idx);
		return;
	}

	ka->stat.crlf_sent++;
}

/*
 * One round: a ping to every destination the callback lists, index 0 first.
 * The busy flag spans the callback too, because the account may update the
 * config from inside it (sip/keepalive.h, sip_ka_cfg_update).
 */
static void crlf_round(struct sip_ka *ka)
{
	const struct sip_txn_dst *dst;
	unsigned int idx = 0;

	ka->crlf_last_ms = ka_now(ka);
	ka->crlf_busy = true;
	for (dst = dst_at(ka, 0); dst; dst = dst_at(ka, ++idx))
		ping_send(ka, dst, idx);
	ka->crlf_busy = false;

	if (idx)
		return;

	ka->stat.crlf_skipped++;
	ka_trace(ka, "crlf skipped, no destination");
}

static void crlf_fire(struct sip_timer *t)
{
	struct sip_ka *ka = t->owner;

	if (!ka->running)
		return;

	crlf_round(ka);
	timer_rearm(ka, &ka->t_crlf, crlf_secs(ka));
}

/* ---------------------------------------------------------- OPTIONS ping */

/*
 * Every ping is independent (sip/keepalive.h): a fresh Call-ID and a fresh From
 * tag per request, so a registrar never sees a dialog forming out of them.
 */
static void ident_make(struct sip_ka *ka)
{
	const char *host = ka->cfg.local_host ? ka->cfg.local_host : "uvoice";

	snprintf(ka->call_id, sizeof(ka->call_id), "%08x%08x@%s", sip_txn_rand(ka->mgr),
		 sip_txn_rand(ka->mgr), host);
	snprintf(ka->from_tag, sizeof(ka->from_tag), "%08x%04x", sip_txn_rand(ka->mgr),
		 sip_txn_rand(ka->mgr) & 0xffffu);
}

static bool wire_build(struct sip_ka *ka)
{
	struct sip_buf b;

	if (!sip_branch_new(ka->mgr, ka->branch, sizeof(ka->branch)))
		return false;

	ident_make(ka);
	sip_buf_init(&b, ka->wire, sizeof(ka->wire));

	msg_req_line_write(&b, "OPTIONS", ka->cfg.registrar);
	hdr_via_write(&b, ka->cfg.transport, ka->cfg.local_host,
		      ka->cfg.local_port, ka->branch, true);
	hdr_write(&b, SIP_HDR_MAX_FORWARDS, "70");
	hdr_addr_tag_write(&b, SIP_HDR_FROM, ka->cfg.display, ka->cfg.aor,
			   ka->from_tag);
	hdr_addr_write(&b, SIP_HDR_TO, NULL, ka->cfg.aor, NULL);
	hdr_write(&b, SIP_HDR_CALL_ID, "%s", ka->call_id);
	hdr_cseq_write(&b, ka->cseq, "OPTIONS");

	/* RFC 3261 11.1: a UAC sending OPTIONS states what it would accept. */
	hdr_write(&b, SIP_HDR_ACCEPT, "application/sdp");
	if (ka->cfg.user_agent)
		hdr_write(&b, SIP_HDR_USER_AGENT, "%s", ka->cfg.user_agent);

	msg_body_write(&b, NULL, NULL, 0);
	if (!sip_buf_ok(&b))
		return false;

	ka->wire_len = sip_buf_len(&b);

	return true;
}

static void options_send(struct sip_ka *ka)
{
	const struct sip_txn_dst *dst;
	struct sip_txn *txn;

	if (ka->txn != SIP_HANDLE_NONE && sip_txn_get(ka->mgr, ka->txn)) {
		ka->stat.options_skipped++;
		ka_trace(ka, "options skipped, one still in flight");
		return;
	}

	ka->txn = SIP_HANDLE_NONE;

	dst = dst_at(ka, 0);
	if (!dst) {
		ka->stat.options_skipped++;
		ka_trace(ka, "options skipped, no destination");
		return;
	}

	ka->cseq++;
	if (!wire_build(ka)) {
		ka->stat.options_failed++;
		ka_trace(ka, "options cseq %u could not be built", ka->cseq);
		return;
	}

	txn = sip_txn_client_start(ka->mgr, SIP_METHOD_OPTIONS, sip_str_cstr(ka->branch),
				   ka->wire, ka->wire_len, dst, ka->owner);
	if (!txn) {
		ka->stat.options_failed++;
		ka_trace(ka, "options cseq %u refused by the transaction layer", ka->cseq);
		return;
	}

	ka->txn = sip_txn_handle(txn);
	ka->stat.options_sent++;
	ka_trace(ka, "OPTIONS cseq %u", ka->cseq);
}

/*
 * Drop the ping in flight. The done upcall with SIP_TXN_DONE_ABORT clears the
 * handle and counts nothing, which is what makes an abort different from a
 * timeout.
 */
static void options_abort(struct sip_ka *ka)
{
	struct sip_txn *txn;

	if (ka->txn == SIP_HANDLE_NONE)
		return;

	txn = sip_txn_get(ka->mgr, ka->txn);
	if (txn)
		sip_txn_abort(txn);
}

/* the busy flag spans the send for the reason crlf_round() carries one */
static void options_round(struct sip_ka *ka)
{
	ka->options_last_ms = ka_now(ka);
	ka->options_busy = true;
	options_send(ka);
	ka->options_busy = false;
}

static void options_fire(struct sip_timer *t)
{
	struct sip_ka *ka = t->owner;

	if (!ka->running)
		return;

	options_round(ka);
	timer_rearm(ka, &ka->t_options, options_secs(ka));
}

/* --------------------------------------------------------------- upcalls */

void sip_ka_txn_resp(struct sip_ka *ka, uint32_t txn, const struct sip_msg *msg)
{
	if (!ka || !msg || msg->is_request)
		return;
	if (ka->txn == SIP_HANDLE_NONE || txn != ka->txn)
		return;

	/* A provisional says the path carries the request, not that the far end
	 * answered it; only a final proves the round trip. */
	if (msg->status < 200)
		return;

	ka->stat.options_alive++;
	ka_trace(ka, "options answered %u", msg->status);
}

void sip_ka_txn_done(struct sip_ka *ka, uint32_t txn, enum sip_txn_reason reason)
{
	if (!ka || ka->txn == SIP_HANDLE_NONE || txn != ka->txn)
		return;

	ka->txn = SIP_HANDLE_NONE;
	if (reason != SIP_TXN_DONE_TIMEOUT && reason != SIP_TXN_DONE_TRANSPORT)
		return;

	ka->stat.options_failed++;
	ka_trace(ka, "options %s", sip_txn_reason_name(reason));
}

/* ------------------------------------------------------------------- API */

void sip_ka_init(struct sip_ka *ka, struct sip_txn_mgr *mgr, struct sip_txn_env *env,
		 uint32_t owner, const struct sip_ka_cfg *cfg, sip_ka_dst_at dst_at,
		 void *priv)
{
	memset(ka, 0, sizeof(*ka));
	ka->mgr = mgr;
	ka->env = env;
	ka->owner = owner;
	ka->dst_at = dst_at;
	ka->priv = priv;
	ka->txn = SIP_HANDLE_NONE;

	if (cfg)
		cfg_take(ka, cfg);

	sip_timer_init(&ka->t_crlf, ka, SIP_TIMER_KA_CRLF, crlf_fire);
	sip_timer_init(&ka->t_options, ka, SIP_TIMER_KA_OPTIONS, options_fire);
}

void sip_ka_start(struct sip_ka *ka)
{
	if (!ka || ka->running)
		return;

	ka->running = true;
	ka->crlf_last_ms = ka_now(ka);
	ka->options_last_ms = ka->crlf_last_ms;
	timer_rearm(ka, &ka->t_crlf, crlf_secs(ka));
	timer_rearm(ka, &ka->t_options, options_secs(ka));
}

void sip_ka_ping_now(struct sip_ka *ka)
{
	if (!ka || !ka->running || !crlf_secs(ka))
		return;

	crlf_round(ka);
	timer_rearm(ka, &ka->t_crlf, crlf_secs(ka));
}

void sip_ka_stop(struct sip_ka *ka)
{
	if (!ka)
		return;

	ka->running = false;
	sip_timer_cancel(ka->env, &ka->t_crlf);
	sip_timer_cancel(ka->env, &ka->t_options);
	options_abort(ka);
}

void sip_ka_done(struct sip_ka *ka)
{
	if (!ka)
		return;

	sip_ka_stop(ka);
	sip_txn_owner_release(ka->mgr, ka->owner);
	ka->txn = SIP_HANDLE_NONE;
}

typedef uint32_t (*ka_secs_of)(const struct sip_ka *ka);

/*
 * One side of an update. A side that keeps running keeps the time it already
 * waited, so a reload burst cannot push the next ping out indefinitely, and a
 * bound that shrinks under a running line is met at once. A side inside its
 * own send is left alone: its fire re-arms from the new interval after the
 * send returns, and a second send from here would double the round. A round
 * this update fires can re-enter the update from the destination callback,
 * so the arm after it reads the interval of the moment through secs_of and
 * not the one this call started with.
 */
static void side_update(struct sip_ka *ka, struct sip_timer *t,
			uint32_t was_secs, ka_secs_of secs_of,
			uint64_t *last_ms, bool busy,
			void (*round)(struct sip_ka *))
{
	uint32_t secs = secs_of(ka);
	uint64_t now = ka_now(ka);
	uint64_t due = *last_ms + (uint64_t)secs * 1000;

	if (!secs) {
		sip_timer_cancel(ka->env, t);
		return;
	}

	if (busy)
		return;

	if (!was_secs) {
		*last_ms = now;
		sip_timer_arm(ka->env, t, secs * 1000);
		return;
	}

	if (due > now) {
		sip_timer_arm(ka->env, t, (unsigned int)(due - now));
		return;
	}

	round(ka);
	timer_rearm(ka, t, secs_of(ka));
}

void sip_ka_cfg_update(struct sip_ka *ka, const struct sip_ka_cfg *cfg)
{
	uint32_t was_crlf, was_options, secs;

	if (!ka || !cfg)
		return;

	was_crlf = crlf_secs(ka);
	was_options = options_secs(ka);
	cfg_take(ka, cfg);
	secs = options_secs(ka);

	/*
	 * A side the new configuration turned off owes nothing more. Without
	 * this the ping in flight keeps its retransmission timers, puts
	 * datagrams on the wire after the option was disabled, and counts a
	 * failure when it finally times out.
	 */
	if (!secs)
		options_abort(ka);

	if (!ka->running) {
		sip_timer_cancel(ka->env, &ka->t_crlf);
		sip_timer_cancel(ka->env, &ka->t_options);
		return;
	}

	side_update(ka, &ka->t_crlf, was_crlf, crlf_secs, &ka->crlf_last_ms,
		    ka->crlf_busy, crlf_round);
	side_update(ka, &ka->t_options, was_options, options_secs,
		    &ka->options_last_ms, ka->options_busy, options_round);
}
