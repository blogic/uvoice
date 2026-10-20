// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Per account keepalive: the CRLF NAT keepalive and the OPTIONS ping
 * (src/proto/sip/keepalive.c, uci options keepalive_enabled, keepalive_interval
 * and options_interval).
 *
 * Dependencies. The transaction layer environment and nothing else, exactly
 * like sip/reg.h: no uloop, no socket, no resolver, no uci. One instance
 * serves one account.
 *
 * CRLF keepalive (RFC 5626 4.4.1). Every crlf_interval_s seconds the instance
 * runs one round: it asks dst_at() for the destinations in order, index 0
 * first, and sends the four byte double CRLF ping through env->send to each
 * one until dst_at() answers NULL. No transaction carries a ping and no
 * destination is penalized for one. A round whose index 0 is NULL is skipped
 * and counted; the timer always re-arms. The round runs whenever the CRLF
 * side is enabled, whatever firewall mode the account runs under: under the
 * conntrack pinhole form every destination of the round is a tuple the box
 * has to keep alive, and under a firewall rule the extra pings cost nothing.
 *
 * The pong side does not live here: the transport reports an inbound
 * CRLF-only datagram through its ping callback, and the account answers a
 * ping (length >= 4) with the two byte pong. A pong from the peer (length
 * < 4) is never answered, so two conforming stacks cannot chatter.
 *
 * OPTIONS ping. Every options_interval_s seconds the instance sends an out of
 * dialog OPTIONS with request URI cfg.registrar through a non-INVITE client
 * transaction. Each ping is independent: a fresh Call-ID, a fresh From tag and
 * a fresh branch, with one CSeq counter that only grows across pings. At most
 * one ping is in flight; a tick that finds the previous transaction still
 * running is skipped and counted.
 *
 * The ping carries no Route header, and the service route a registrar granted
 * (RFC 3608, sip/reg.h) is no exception. The ping measures the next hop and
 * the NAT binding toward it, so it must reach that hop and nothing else. A
 * proxy of the service route that is down would otherwise mark a registrar
 * that answers as unreachable. The ping has no failover of its own to correct
 * that verdict.
 *
 * Any final response, 401 and 407 included, counts the path alive: the ping
 * exists to prove reachability and to keep intermediaries warm, not to use a
 * service, so a challenge is never answered with credentials. A timeout or a
 * transport failure counts failed. Neither outcome starts a failover (no
 * target penalty, no re-resolve, no re-register).
 *
 * Lifetime. cfg strings belong to the caller and must outlive the instance
 * (the sip_reg_cfg rule). dst_at() is called on the tick and its result is
 * used inside that call only.
 */
#ifndef UVOICE_SIP_KEEPALIVE_H
#define UVOICE_SIP_KEEPALIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/sip/txn.h"

#define SIP_KA_WIRE_MAX		2048
#define SIP_KA_ID_MAX		64
#define SIP_KA_TAG_MAX		24

/* seconds; both intervals clamp into this range when nonzero */
#define SIP_KA_INTERVAL_MIN	1
#define SIP_KA_INTERVAL_MAX	86400

/* the RFC 5626 4.4.1 ping and pong payloads */
#define SIP_KA_PING		"\r\n\r\n"
#define SIP_KA_PING_LEN		4
#define SIP_KA_PONG		"\r\n"
#define SIP_KA_PONG_LEN		2

/*
 * crlf_enabled    uci keepalive_enabled (base scope).
 * crlf_interval_s uci keepalive_interval (base scope).
 * options_interval_s  the effective value after the channel alias rule:
 *                 channel options_interval when nonzero, else base. 0 turns
 *                 the OPTIONS ping off; the CRLF side needs crlf_enabled.
 * aor             "sip:user@domain", From and To of the OPTIONS.
 * registrar       the OPTIONS request URI, the sip_reg_cfg.registrar string.
 * local_host, local_port  the Via sent-by.
 * user_agent      may be NULL.
 * display         From display name, may be NULL.
 */
struct sip_ka_cfg {
	const char *aor;
	const char *registrar;
	const char *local_host;
	const char *user_agent;
	const char *display;
	/*
	 * The transport token for the Via and the Contact. NULL takes UDP. It
	 * is the upper case form ("TCP", "TLS") that the Via carries after
	 * "SIP/2.0/"; the Contact writer lowercases it for ";transport=".
	 */
	const char *transport;
	uint16_t local_port;
	uint32_t crlf_interval_s;
	uint32_t options_interval_s;
	bool crlf_enabled;
};

struct sip_ka_stat {
	uint64_t crlf_sent;		/* datagrams, one per destination */
	uint64_t crlf_skipped;		/* rounds whose index 0 was NULL */
	uint64_t crlf_failed;		/* datagrams the transport refused */
	uint64_t options_sent;
	uint64_t options_alive;		/* any final response */
	uint64_t options_failed;	/* timeout, transport, or a request
					 * that does not fit the wire buffer */
	uint64_t options_skipped;	/* no destination, or one still in flight */
};

struct sip_ka;

/*
 * The destinations of one round, in order. Index 0 is the sticky registration
 * target (sip_reg_target_get of sip_reg_target_current), NULL before the
 * first resolution; the indexes after it are the other destinations the
 * account wants pinged, and NULL ends the list. The OPTIONS ping reads index
 * 0 alone. Called on every tick; a result is used inside that call only.
 */
typedef const struct sip_txn_dst *(*sip_ka_dst_at)(struct sip_ka *ka, void *priv,
						    unsigned int idx);

struct sip_ka {
	struct sip_txn_mgr *mgr;
	struct sip_txn_env *env;
	struct sip_ka_cfg cfg;
	sip_ka_dst_at dst_at;
	void *priv;
	uint32_t owner;

	struct sip_timer t_crlf;
	struct sip_timer t_options;

	/*
	 * The env clock at the start and at every fire of each side, so a
	 * config update re-arms with the time already elapsed kept. The busy
	 * flags say a side is inside its send: an update that arrives from
	 * the dst_at callback then leaves that side to the re-arm of its own
	 * fire.
	 */
	uint64_t crlf_last_ms;
	uint64_t options_last_ms;
	bool crlf_busy;
	bool options_busy;

	char wire[SIP_KA_WIRE_MAX];
	size_t wire_len;
	char call_id[SIP_KA_ID_MAX];
	char from_tag[SIP_KA_TAG_MAX];
	char branch[SIP_BRANCH_MAX];

	uint32_t txn;			/* the OPTIONS in flight, or none */
	uint32_t cseq;
	bool running;

	struct sip_ka_stat stat;
};

void sip_ka_init(struct sip_ka *ka, struct sip_txn_mgr *mgr, struct sip_txn_env *env,
		 uint32_t owner, const struct sip_ka_cfg *cfg, sip_ka_dst_at dst_at,
		 void *priv);
void sip_ka_done(struct sip_ka *ka);

/*
 * start arms both timers per the config; each side first fires one full
 * interval after the start, never immediately, so a reload burst costs no
 * datagram. stop cancels both timers and aborts an OPTIONS still in flight;
 * the done() upcall with SIP_TXN_DONE_ABORT clears the handle and counts
 * nothing.
 */
void sip_ka_start(struct sip_ka *ka);
void sip_ka_stop(struct sip_ka *ka);

/*
 * Send the CRLF round now and re-arm the CRLF timer one full interval from
 * now. The account calls it when the destinations moved under a running
 * instance: after a resolver answer or an interface address change the
 * conntrack tuples of the new destinations would otherwise wait for the next
 * tick. A stopped instance, or one whose CRLF side is off, sends nothing.
 */
void sip_ka_ping_now(struct sip_ka *ka);

/*
 * The live reload path: replace the config in place and re-arm both timers.
 * Each side that stays enabled keeps the time that elapsed since its last
 * fire: the next due is the new interval minus that time, a due at or below
 * zero fires the side now and re-arms a full interval, and a side that was
 * off starts a fresh full interval. A disabled side is cancelled and an
 * OPTIONS still in flight is aborted. It is safe from inside the dst_at
 * callback of a round: the side that is sending is left to the re-arm of its
 * own fire, which reads the new interval.
 */
void sip_ka_cfg_update(struct sip_ka *ka, const struct sip_ka_cfg *cfg);

/*
 * The daemon routes the transaction upcalls by owner, exactly as it does for
 * sip_reg. Both ignore a transaction this instance does not own.
 */
void sip_ka_txn_resp(struct sip_ka *ka, uint32_t txn, const struct sip_msg *msg);
void sip_ka_txn_done(struct sip_ka *ka, uint32_t txn, enum sip_txn_reason reason);

#endif
