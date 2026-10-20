// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Message waiting indication: the NOTIFY intake and the SUBSCRIBE client
 * (RFC 3842 message-summary, RFC 6665 the event framework; src/proto/sip/sub.c,
 * uci options mwi_subscribe, amwi and vmwi).
 *
 * Dependencies. The transaction layer environment and nothing else, exactly
 * like sip/keepalive.h and sip/reg.h: no uloop, no socket, no resolver, no
 * uci, no HAL. One instance serves one account.
 *
 * Two duties, one file.
 *
 * 1. The intake is the primary path. The deployed switches send the message
 *    summary as an unsolicited NOTIFY, outside any subscription and outside
 *    any dialog, which is what the default mwi_subscribe 0 relies on. The
 *    account layer routes a NOTIFY here by its Event package, answers 200
 *    for everything this file accepts and 489 for a package nobody claims.
 *    sip_sub_notify_recv() works with sub NULL, which
 *    is the state of a line that never subscribes.
 * 2. The SUBSCRIBE client is optional and runs only when uci mwi_subscribe is
 *    1. It carries its own Call-ID and From tag, one CSeq that only grows and
 *    at most one request in flight, and it holds no sip_dialog: a NOTIFY that
 *    matches this subscription by Call-ID and tags is recognised here, and
 *    everything else falls to the unsolicited path. The account starts it
 *    after the first successful REGISTER and stops it with the account.
 *
 * The body grammar is bounded parsing and nothing else. Every string stays a
 * span of the parse buffer, no line is copied and no scan runs past the body.
 * The boolean and the counts are independent: a switch that says
 * "yes" with 0 new and 0 old messages means the indication is on and the
 * counts carry no detail, which is what a subscriber on that switch sees. The
 * boolean is therefore never derived from the counts.
 *
 * A body that does not parse is still answered 200. The switch is not helped
 * by a 4xx it does not act on, and a refusal earns a retransmission of the
 * same body. The stored state is left alone and the failure is counted.
 *
 * Both content types are accepted. RFC 3842 defines
 * application/simple-message-summary; Sylantro switches send
 * application/message-summary. A body with no Content-Type is parsed as well,
 * because the grammar itself decides whether the body is a message summary.
 *
 * Lifetime. cfg strings belong to the caller and must outlive the instance
 * (the sip_reg_cfg rule). dst_get() is called when a request is built and its
 * result is used inside that call only.
 */
#ifndef UVOICE_SIP_SUB_H
#define UVOICE_SIP_SUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/sip/auth.h"
#include "proto/shared/hdr.h"
#include "proto/sip/txn.h"

/*
 * A base budget of 2048 bytes for the SUBSCRIBE, plus the widest rendered
 * route block, so the route takes no space from the base budget. This is no
 * proof that every SUBSCRIBE fits: two digest credential headers alone can
 * approach 2000 bytes at the declared bounds. An overflow fails clean:
 * wire_build() returns false and the attempt is counted, never truncated.
 */
#define SIP_SUB_WIRE_MAX	(2048 + SIP_ROUTE_HDRS_MAX)
#define SIP_SUB_ID_MAX		64
#define SIP_SUB_TAG_MAX		24

/* RFC 3842 3: the package, its body type, and the Sylantro type */
#define SIP_SUB_EVENT_MWI	"message-summary"
#define SIP_SUB_CTYPE_MWI	"application/simple-message-summary"
#define SIP_SUB_CTYPE_MWI_ALT	"application/message-summary"

/*
 * RFC 3515 2.4.4: the implicit subscription a REFER creates. This file serves
 * no such subscription; the name is here because the account layer routes a
 * NOTIFY by its package and the call leg owns this one.
 */
#define SIP_SUB_EVENT_REFER	"refer"

/* seconds a failed attempt waits before the next one */
#define SIP_SUB_RETRY_S		60

/* 423 arrives once in a healthy exchange; more than this is a loop */
#define SIP_SUB_MIN_EXPIRES_MAX	1

/*
 * Consecutive fresh identities minted for a 481 or a terminated subscription
 * before the client falls back to the retry timer. Without the bound a
 * notifier that answers every SUBSCRIBE with 481 keeps the daemon sending
 * them with no delay at all.
 */
#define SIP_SUB_RESTART_MAX	1

/*
 * The message summary one NOTIFY carried.
 *
 * waiting      the Messages-Waiting line, the only element the grammar needs.
 * counts_valid a Voice-Message line parsed, so new_msgs and old_msgs mean
 *              something. False leaves both zero and says the switch sent no
 *              usable count, which is not the same as a count of zero.
 */
struct sip_mwi {
	bool waiting;
	bool counts_valid;
	uint32_t new_msgs;
	uint32_t old_msgs;
};

/*
 * The routing verdict for an inbound NOTIFY, read by the account layer.
 * SIP_SUB_EV_OTHER covers a NOTIFY carrying another package and one carrying
 * no Event header at all: neither names a package uvoice serves, and
 * RFC 6665 8.2.1 answers both with 489.
 *
 * REFER names a package this file does not serve either, and it is the one
 * exception to that rule: the call layer holds the implicit subscription of a
 * transfer it started, so the account layer hands such a NOTIFY to the dialog
 * it belongs to instead of answering it 489. The id parameter of Event: refer
 * is part of that subscription and is not read here.
 */
enum sip_sub_event {
	SIP_SUB_EV_NONE = 0,		/* not a NOTIFY: the caller's own path */
	SIP_SUB_EV_MWI,			/* Event: message-summary */
	SIP_SUB_EV_REFER,		/* Event: refer, the call layer's */
	SIP_SUB_EV_OTHER,		/* 489 Bad Event */
};

enum sip_sub_event sip_sub_event_class(const struct sip_msg *msg);

/* both are answered 200; only SIP_SUB_RX_STORE carries state */
enum sip_sub_rx {
	SIP_SUB_RX_STORE = 0,
	SIP_SUB_RX_BAD_BODY,
};

const char *sip_sub_rx_name(enum sip_sub_rx rx);

/*
 * The RFC 3842 body grammar, exported because it is the whole intake:
 *
 *   Messages-Waiting: yes | no      case insensitive, an exact token. A value
 *                                   the grammar does not define fails the
 *                                   body, and the body is never searched for
 *                                   the word as a substring of something else.
 *   Voice-Message: <new>/<old>      bounded decimal integers, both required
 *                                   when the line is present. Anything behind
 *                                   the second number, which is where
 *                                   RFC 3842 puts the urgent pair, is ignored.
 *
 * Every other line, header and message class is ignored. False means the body
 * carries no usable Messages-Waiting line, and *out is zeroed.
 */
bool sip_mwi_body_parse(struct sip_str body, struct sip_mwi *out);

/* ------------------------------------------------------- the subscription */

struct sip_sub;

/*
 * aor        "sip:user@domain": From and To of the SUBSCRIBE.
 * registrar  the SUBSCRIBE request URI, the sip_reg_cfg.registrar string.
 * contact    the registered Contact URI, which is the address a NOTIFY has to
 *            reach. It carries the rewrite the registration learned from a NAT
 *            (sip/reg.h "Contact and NAT"), so the subscription must not build
 *            one of its own.
 * expires    the account's registration expiry: the subscription rides it.
 *            0 takes SIP_REG_EXPIRES_DEFAULT.
 */
struct sip_sub_cfg {
	const char *aor;
	const char *registrar;
	const char *contact;
	const char *auth_user;
	const char *auth_pass;
	const char *display;		/* From display name, may be NULL */
	const char *user_agent;		/* may be NULL */
	const char *local_host;
	/*
	 * The transport token for the Via and the Contact. NULL takes UDP. It
	 * is the upper case form ("TCP", "TLS") that the Via carries after
	 * "SIP/2.0/"; the Contact writer lowercases it for ";transport=".
	 */
	const char *transport;
	/*
	 * The service route the registrar granted (RFC 3608 section 5), or
	 * NULL for none. A request that carries no To tag takes it, and a
	 * request that carries one does not. A To tag says the notifier has
	 * put this subscription in a dialog, and this file keeps no dialog
	 * route set. So a refresh takes none, and an unsubscribe of an
	 * established subscription takes none. An unsubscribe of a
	 * subscription no 2xx ever answered has no To tag and does take it.
	 * That request is still out of dialog, and it goes to the registrar
	 * the first SUBSCRIBE went to.
	 *
	 * The pointer belongs to the caller and must outlive the instance, the
	 * rule every other field of this structure follows. The caller decides
	 * its value again inside dst_get(), which runs when a request is
	 * built. Three facts make that the only sound point: the route belongs
	 * to one server, the destination can move between two requests, and
	 * the first configuration is built before any route exists (sip/reg.h
	 * sip_reg_service_route).
	 */
	const struct sip_route_hdrs *service_route;
	uint16_t local_port;
	uint32_t expires;
};

/*
 * The tag is sip_sub_status and not sip_sub_state because shared/hdr.h already
 * owns "struct sip_sub_state", the parsed Subscription-State header, and C
 * keeps struct and enum tags in one namespace.
 */
enum sip_sub_status {
	SIP_SUB_OFF = 0,
	SIP_SUB_TRYING,			/* a SUBSCRIBE is in flight */
	SIP_SUB_ACTIVE,			/* the notifier accepted it */
	SIP_SUB_RETRY,			/* waiting to try again */

	/*
	 * The notifier refused the package itself (403, 404, 405, 489, 501).
	 * Asking a minute later earns the same answer, so the client stays off
	 * until a reload or a restart offers it a new configuration.
	 */
	SIP_SUB_REFUSED,
};

const char *sip_sub_status_name(enum sip_sub_status status);

struct sip_sub_stat {
	uint64_t sent;
	uint64_t ok;
	uint64_t challenged;
	uint64_t failed;
	uint64_t refused;
	uint64_t restarted;		/* 481, or a subscription that ended */
	uint64_t notify_ours;		/* matched this subscription */
	uint64_t notify_unsolicited;
	uint64_t notify_bad_body;
};

/*
 * The destination of the moment: the account returns the sticky registration
 * target, or NULL before the first resolution. Never stored.
 */
typedef const struct sip_txn_dst *(*sip_sub_dst_get)(struct sip_sub *sub, void *priv);

struct sip_sub {
	struct sip_txn_mgr *mgr;
	struct sip_txn_env *env;
	struct sip_sub_cfg cfg;
	sip_sub_dst_get dst_get;
	void *priv;
	uint32_t owner;

	struct sip_timer timer;
	struct sip_auth_state auth[SIP_AUTH_KIND_MAX];
	unsigned int auth_retry[SIP_AUTH_KIND_MAX];

	char call_id[SIP_SUB_ID_MAX];
	char from_tag[SIP_SUB_TAG_MAX];
	char to_tag[SIP_SUB_TAG_MAX];	/* learned from the 2xx or the NOTIFY */
	char branch[SIP_BRANCH_MAX];
	char wire[SIP_SUB_WIRE_MAX];
	size_t wire_len;

	uint32_t txn;			/* the SUBSCRIBE in flight, or none */
	uint32_t cseq;
	uint32_t req_expires;		/* what the request in flight asked for */
	uint32_t min_expires;		/* raised by a 423, kept for every refresh */
	uint32_t granted;		/* accepted expiry of the subscription */
	unsigned int min_expires_retry;
	unsigned int restart_run;	/* consecutive fresh identities */

	uint8_t status;			/* enum sip_sub_status */
	bool running;
	bool unsubscribing;

	struct sip_sub_stat stat;
};

void sip_sub_init(struct sip_sub *sub, struct sip_txn_mgr *mgr, struct sip_txn_env *env,
		  uint32_t owner, const struct sip_sub_cfg *cfg, sip_sub_dst_get dst_get,
		  void *priv);
void sip_sub_done(struct sip_sub *sub);

/*
 * start sends the first SUBSCRIBE at once, because a subscription that was
 * never established delivers nothing. The caller runs it after the first
 * successful REGISTER, so the Contact the notifier is handed is the one the
 * registrar already holds.
 *
 * stop with unsubscribe true sends Expires: 0 and does not wait for the
 * answer; the account tears down on its own schedule. Both forms abort a
 * request still in flight.
 */
void sip_sub_start(struct sip_sub *sub);
void sip_sub_stop(struct sip_sub *sub, bool unsubscribe);

/*
 * The LIVE reload path: replace the config in place. A running subscription
 * keeps its Call-ID and its timer, because its identity did not move; a caller
 * whose identity did move stops the instance and starts it again.
 */
void sip_sub_cfg_update(struct sip_sub *sub, const struct sip_sub_cfg *cfg);

enum sip_sub_status sip_sub_status_get(const struct sip_sub *sub);
uint32_t sip_sub_expires(const struct sip_sub *sub);
const char *sip_sub_call_id(const struct sip_sub *sub);
uint32_t sip_sub_cseq(const struct sip_sub *sub);

/*
 * One inbound NOTIFY the account layer classified as SIP_SUB_EV_MWI. sub may
 * be NULL, and a NOTIFY that matches no subscription is taken just the same:
 * the unsolicited path is the primary one. A NOTIFY that does match also
 * refreshes the subscription from its Subscription-State header.
 *
 * The verdict says what the caller does with *out, never what it answers:
 * every NOTIFY this call sees is answered 200.
 */
enum sip_sub_rx sip_sub_notify_recv(struct sip_sub *sub, const struct sip_msg *msg,
				    struct sip_mwi *out);

/*
 * The daemon routes the transaction upcalls by owner, exactly as it does for
 * sip_reg and sip_ka. Both are safe to call with a transaction this instance
 * does not own; they ignore it.
 */
void sip_sub_txn_resp(struct sip_sub *sub, uint32_t txn, const struct sip_msg *msg);
void sip_sub_txn_done(struct sip_sub *sub, uint32_t txn, enum sip_txn_reason reason);

#endif
