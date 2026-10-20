// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Per account registration client (RFC 3261 10, src/proto/sip/reg.c).
 *
 * Dependencies. The transaction layer environment and nothing else: no uloop,
 * no socket, no resolver, no uci. One instance serves one account; a two line
 * device runs two of them.
 *
 * Server list, not per transaction RFC 3263. The caller injects the resolved
 * targets with sip_reg_target_add() and this layer keeps a sticky selection:
 * the current target is used until it fails, never rotated while it answers,
 * and a failed target goes into a penalty box for cfg.penalty_s seconds. The
 * resolver of shared/resolve.h fills the list; nothing here parses DNS.
 *
 * A TTL re-resolution replaces the list. The same servers come back in a fresh
 * RFC 2782 draw, so the same server sits at another index, and a plain clear
 * plus re-add would re-randomize the sticky selection and empty every penalty
 * box on every DNS refresh cycle. sip_reg_targets_reload() and
 * sip_reg_targets_reload_end() bracket the replacement instead: both the
 * selection and the penalty state follow the identity of a target, which is
 * its resolved address and port, never its index or the name it came from.
 *
 * Refresh. From the accepted expiry, never from the requested one:
 *
 *   refresh = max(min(expires / 2, expires - 30), 15)   [seconds]
 *
 * with one correction: when that result is not smaller than the expiry, the
 * floor would refresh a binding that has already lapsed, so a granted expiry
 * at or below the floor refreshes at max(expires / 2, 2) instead. A registrar
 * that grants 10 s is refreshed at 5 s.
 *
 * The accepted expiry itself comes from, in this order: the expires parameter
 * of our binding among the Contact headers of the 200, then the Expires header
 * of the 200, then the value the request asked for (RFC 3261 10.2.4). A
 * registrar that answers with several bindings therefore never makes uvoice
 * refresh on somebody else's expiry.
 *
 * Identifiers. One Call-ID and one From tag for the life of the account, and a
 * CSeq that only ever grows, so a registrar sees one registration and not a new
 * one per refresh (RFC 3261 10.2). The unregister on stop keeps both.
 *
 * A REGISTER with Expires 0 asks the registrar to take a binding away, and two
 * rules of an ordinary REGISTER are wrong for it. A 423 ends the exchange,
 * because a resend with Min-Expires would register at that server again. A
 * received or rport in the response is not applied, because a rebuilt Contact
 * would make the authenticated resend name a binding the registrar never held,
 * and the real one would stay. Both rules cover the unregister of stop and the
 * removal of a server move.
 *
 * The server move. Every REGISTER toward a server other than the one that
 * granted the live binding is a move, whoever started it: a retarget, the
 * refresh timer, a refresh after a family or local host move, a retry. The
 * binding stays live at the old server while the exchange runs, and the
 * exchange itself runs as any other, a challenge and a 423 included.
 *
 *   - Its 2xx moves the binding. With cfg.move_unregister set, and with the
 *     old server still in a family the line serves, the client then sends a
 *     REGISTER with Expires 0 to the old server on the same Call-ID and a
 *     grown CSeq. A registrar holds one binding per Contact, so that request
 *     names the URI the old server was granted, as a second Contact with
 *     expires=0 when the line carries another one now. With the option clear,
 *     the old binding lapses on its own.
 *   - Its failure rolls back. The new server is penalized, the selection
 *     returns to the binding's server, both cached challenges are reset, the
 *     state reads REGISTERED again and the refresh is re-armed where it
 *     stood. A 2xx that grants expiry 0 is a failed move too, because the new
 *     server took no binding. Nothing is reported: no failed(), no down event,
 *     no change to the failure ladder, and a Retry-After the new server named
 *     is dropped, because it names that server's delay. The one report is
 *     rolled_back(), which tells an owner that followed the new server to
 *     follow the selection back. When the old server left the answer there is
 *     nothing to roll back to and the failure is an ordinary one.
 *   - It takes a received or an rport from its 2xx alone. Before that the new
 *     server has granted nothing, so a challenge or a failure would move the
 *     Contact of a binding the old server still holds and the rollback would
 *     leave the new server's mapping on it.
 *   - A 2xx that does move the Contact corrects it at the server that granted
 *     the binding, never at a selection a reload moved under it, and the
 *     removal the move owes waits for the clean answer that ends that chase.
 *   - The removal reports nothing either way. Its failure penalizes the old
 *     server and leaves the selection, the state and the refresh alone.
 *   - No unregistered() report runs inside a move or a removal, so the state
 *     reads registered throughout.
 *
 * A refresh or a stop that arrives during a move drops it, and the attempt
 * each of them starts follows the same rules; a retarget waits for the move to
 * end and reads the selection then. One that arrives during a removal drops
 * the removal, and the old binding lapses on its own. A stop with an
 * unregister sends its Expires 0 to the server that holds the binding, never
 * to the current selection.
 *
 * Contact and NAT. The Contact is built from the Via received and rport
 * parameters of the last response when the registrar sent them (RFC 3581), and
 * from cfg.local_host and cfg.local_port otherwise. A change rebuilds the
 * Contact and re-registers, because the binding the registrar holds points at
 * the old address.
 *
 * sip_reg_local_host_set() also supplies the Contact host.
 * RFC 3261 20.42 requires a bare literal in received.
 * nat_check() tolerates brackets and stores the address bare.
 * With a literal local host, nat_check() rejects the other family, a
 * v4-mapped literal or a value that does not parse.
 * It drops the whole observation because received and rport describe one
 * mapping (RFC 3581 section 4).
 * A port without its address would name a binding nobody listens on.
 *
 * No Route header. An outbound proxy is a next hop, that is, a target address,
 * not a preloaded Route header on a REGISTER. A granted service route does not
 * change that rule: it rides the requests the registration serves, never the
 * REGISTER itself (RFC 3608 section 5).
 */
#ifndef UVOICE_SIP_REG_H
#define UVOICE_SIP_REG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/sip/auth.h"
#include "proto/shared/hdr.h"
#include "proto/sip/txn.h"

#define SIP_REG_TARGET_MAX	16
#define SIP_REG_URI_MAX		256
#define SIP_REG_HOST_MAX	64
#define SIP_REG_ID_MAX		64
#define SIP_REG_TAG_MAX		24
#define SIP_REG_WIRE_MAX	4096

#define SIP_REG_EXPIRES_DEFAULT	3600
#define SIP_REG_REFRESH_MIN	15	/* seconds, the floor of the clamp */
#define SIP_REG_REFRESH_LEAD	30	/* seconds before the expiry */

/*
 * The floor that applies instead when SIP_REG_REFRESH_MIN would land at or
 * past the granted expiry. It exists so an expiry of 0 to 4 seconds still
 * produces a timer, and not one that fires immediately.
 */
#define SIP_REG_REFRESH_FLOOR	2
#define SIP_REG_RETRY_DEFAULT	5	/* seconds, uci sip_register_retry */
#define SIP_REG_PENALTY_DEFAULT	60	/* seconds a failed target sits out */

/*
 * The retry delay doubles per consecutive failed attempt up to this cap, so a
 * carrier outage does not turn into a REGISTER every five seconds for hours.
 * A credential failure takes SIP_REG_BACKOFF_AUTH instead, fixed and short:
 * the line refuses every call while it waits, so a fix at the provider must
 * show quickly. The cost is a provider guard that
 * bans an address after repeated failures; a password changed through uci
 * registers at once, whatever this delay says.
 */
#define SIP_REG_BACKOFF_MAX	1800	/* seconds */
#define SIP_REG_BACKOFF_AUTH	30	/* seconds */
#define SIP_REG_BACKOFF_SHIFT	5	/* doublings before the cap applies */

/*
 * The ceiling of any scheduled delay, in seconds. A Retry-After or an Expires
 * is a number a server chose, and seconds times 1000 has to stay inside the
 * millisecond timer.
 */
#define SIP_REG_DELAY_MAX	86400

/* 423 arrives once in a healthy exchange; more than this is a loop */
#define SIP_REG_MIN_EXPIRES_MAX	2

/*
 * Consecutive Contact rebuilds before the account stops chasing the address a
 * registrar reports. A NAT that hands out a new source port per datagram, or
 * two servers that disagree about it, would otherwise keep the account in a
 * REGISTER loop with no delay between the attempts.
 */
#define SIP_REG_REBIND_MAX	3

enum sip_reg_state {
	SIP_REG_UNREGISTERED = 0,
	SIP_REG_RESOLVING,	/* running, but the target list is empty */
	SIP_REG_TRYING,		/* a REGISTER is in flight, no binding yet */
	SIP_REG_REGISTERED,
	SIP_REG_REFRESHING,	/* a REGISTER is in flight, binding still live */
	SIP_REG_BACKOFF,	/* waiting to try again after a failure */
};

const char *sip_reg_state_name(enum sip_reg_state state);

/*
 * Every string must outlive the registration. reg.c copies nothing from here:
 * the account copies its configuration strings at start, and this struct is
 * that copy's address book.
 *
 * aor        the address of record, "sip:user@domain". From and To.
 * registrar  the REGISTER request URI, normally "sip:domain".
 * contact_user  the user part of the Contact URI.
 * local_host, local_port  the Via sent-by, and the Contact host and port until
 *            a registrar answers with received or rport.
 */
struct sip_reg_cfg {
	const char *aor;
	const char *registrar;
	const char *contact_user;
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
	uint16_t local_port;
	uint32_t expires;		/* requested, 0 takes the default */
	uint32_t retry_s;		/* uci sip_register_retry, 0 takes 5 */
	uint32_t penalty_s;		/* 0 takes SIP_REG_PENALTY_DEFAULT */
	/*
	 * uci dns_move_unregister. True sends a REGISTER with Expires 0 to the
	 * server the binding left, once the new one granted a binding. False
	 * lets the old binding lapse on its own. It decides the removal only;
	 * the rollback of a failed move runs either way.
	 */
	bool move_unregister;
};

/*
 * registered   a binding is live; expires_s is the accepted expiry.
 * unregistered the binding is gone, either because stop() removed it or
 *              because the registrar answered a refresh with expiry 0.
 * failed       an attempt failed. code is the SIP status, 408 for a timeout
 *              and 503 for a transport failure; retry_s is when the next
 *              attempt runs, 0 when there will not be one.
 * rolled_back  a move failed and the selection went back to the server that
 *              holds the binding. The registration itself never went down, so
 *              this is no failure report: it tells an owner that followed the
 *              new server, with a socket or an address family of its own, to
 *              follow the selection back. The state reads REGISTERED and the
 *              refresh is armed when it runs, and the owner may send from it.
 */
struct sip_reg;

struct sip_reg_cb {
	void (*registered)(struct sip_reg *reg, uint32_t expires_s, void *priv);
	void (*unregistered)(struct sip_reg *reg, void *priv);
	void (*failed)(struct sip_reg *reg, uint32_t code, uint32_t retry_s, void *priv);
	void (*rolled_back)(struct sip_reg *reg, void *priv);
};

struct sip_reg_target {
	struct sip_txn_dst dst;
	uint64_t penalty_until;		/* ms on the env clock, 0 when clear */
	unsigned int failures;
	/*
	 * The RFC 2782 priority of the SRV record this endpoint came from, and
	 * the zone operator's routing policy. A plain address answer carries 0
	 * on every target, so the whole list is one group.
	 */
	uint16_t prio;
};

struct sip_reg_stat {
	uint64_t sent;
	uint64_t rebind_capped;
	uint64_t ok;
	uint64_t challenged;
	uint64_t failed;
	uint64_t timeout;
	uint64_t switched;		/* target changes after a failure */
	uint64_t contact_rebuilt;
};

enum sip_reg_timer {
	SIP_REG_T_NONE = 0,
	SIP_REG_T_REFRESH,
	SIP_REG_T_RETRY,
};

struct sip_reg {
	struct sip_txn_mgr *mgr;
	struct sip_txn_env *env;
	struct sip_reg_cfg cfg;
	struct sip_reg_cb cb;
	void *priv;
	uint32_t owner;

	struct sip_timer timer;
	struct sip_reg_target target[SIP_REG_TARGET_MAX];

	/*
	 * The reload window. stash holds the identity and the penalty state of
	 * the targets the previous answer produced, stashed how many of them
	 * there are, and stash_cur which one the sticky selection was on. It is
	 * live only between sip_reg_targets_reload() and its _end(), and it is
	 * bounded exactly like target[], so a reload allocates nothing.
	 */
	struct sip_reg_target stash[SIP_REG_TARGET_MAX];

	struct sip_auth_state auth[SIP_AUTH_KIND_MAX];

	char call_id[SIP_REG_ID_MAX];
	char from_tag[SIP_REG_TAG_MAX];
	char contact_host[SIP_REG_HOST_MAX];
	char contact_uri[SIP_REG_URI_MAX];
	char old_contact_uri[SIP_REG_URI_MAX];

	/*
	 * The Contact URI the live binding was granted with, and the one a
	 * removal deferred by a Contact correction has to name. A registrar
	 * holds a binding per Contact, so an Expires 0 that names the current
	 * URI asks a server to remove a binding it never held and leaves the
	 * real one standing.
	 */
	char binding_contact[SIP_REG_URI_MAX];
	char remove_contact[SIP_REG_URI_MAX];

	char branch[SIP_BRANCH_MAX];
	char wire[SIP_REG_WIRE_MAX];
	size_t wire_len;

	uint32_t txn;			/* the REGISTER in flight, or none */
	uint32_t cseq;
	uint32_t req_expires;		/* what the request in flight asked for */
	uint32_t min_expires;		/* raised by a 423, kept for every refresh */
	uint32_t granted;		/* accepted expiry of the live binding */
	uint32_t fail_run;		/* consecutive failed attempts */
	uint32_t next_retry_s;

	/* the absolute deadline of the armed refresh, so a rollback re-arms
	 * the refresh of the binding it goes back to where it stood */
	uint64_t refresh_due_ms;

	unsigned int targets;
	unsigned int cur;		/* the sticky selection */
	unsigned int stashed;		/* targets held for the reload in progress */
	unsigned int stash_cur;		/* the selection that reload has to carry */
	unsigned int auth_retry[SIP_AUTH_KIND_MAX];	/* per kind, this exchange */
	unsigned int rebind_run;	/* consecutive Contact rebuilds */
	unsigned int min_expires_retry;

	/*
	 * The destination of the exchange in flight, captured when the attempt
	 * starts. Every send of one exchange goes here, the credential resend
	 * included, so a reload under a challenge cannot re-aim the answer to
	 * one server's nonce at another server, which reads as a credential
	 * failure and earns the SIP_REG_BACKOFF_AUTH delay.
	 */
	struct sip_txn_dst attempt_dst;

	/*
	 * The server whose 2xx granted the binding the line holds now. It is
	 * captured from attempt_dst when that answer arrives and cleared with
	 * granted, so it names a server that owes the line a binding and never
	 * one that does not.
	 */
	struct sip_txn_dst binding_dst;

	/*
	 * The service route the registrar of that binding granted (RFC 3608),
	 * rendered as Route headers. It belongs to binding_dst: the 2xx that
	 * captures the one captures the other, and a request toward another
	 * server must not carry it.
	 */
	struct sip_route_hdrs service_route;

	/*
	 * The server of a removal a Contact correction deferred. A move whose
	 * 2xx moved the Contact registers again at once, and the removal the
	 * move owes runs from the clean answer that ends that chase.
	 */
	struct sip_txn_dst remove_dst;

	uint16_t contact_port;
	uint8_t state;
	bool running;
	bool unregistering;
	bool contact_dirty;		/* the Contact changed under the request */
	bool notifying;			/* inside the unregistered() callback */
	bool reloading;			/* between reload() and reload_end() */

	/* the REGISTER in flight targets a server other than binding_dst while
	 * a binding is live there */
	bool moving;

	/* the exchange in flight is the Expires 0 to the server the binding
	 * left */
	bool removing;

	/* the removal in flight put the old binding's URI in the removal slot,
	 * so its completion is what empties the slot again */
	bool removal_slot;

	/* the last REGISTER sent carried the removal slot as a second Contact */
	bool old_contact_sent;

	/* a final answer refused that REGISTER, and the failure it ends in
	 * decides whether the slot goes with it */
	bool old_contact_refused;

	/* the next attempt corrects the Contact at the server that granted the
	 * binding, so it reads binding_dst and not the selection */
	bool rebinding;

	/* a retarget waits for the exchange in flight to end */
	bool retarget_pending;

	/*
	 * The armed retry delay is one a server named (Retry-After) or the
	 * credential backoff, so a retarget must not cut it short.
	 */
	bool retry_fixed;

	struct sip_reg_stat stat;
};

void sip_reg_init(struct sip_reg *reg, struct sip_txn_mgr *mgr, struct sip_txn_env *env,
		  uint32_t owner, const struct sip_reg_cfg *cfg, const struct sip_reg_cb *cb,
		  void *priv);
void sip_reg_done(struct sip_reg *reg);

/*
 * The resolved target set. Adding a target while the account waits in
 * SIP_REG_RESOLVING starts the first attempt. Clearing the list while a
 * request is in flight leaves that request alone; the failure path then finds
 * an empty list and goes back to SIP_REG_RESOLVING.
 *
 * One endpoint, one entry. An add whose identity, the resolved address and the
 * port, is already in the list is absorbed: the list does not change and the
 * call reports true, because the endpoint the caller asked for is present. A
 * zone may carry two SRV records naming the same host and port, and each of
 * them collects the same address, so a resolver answer legitimately offers one
 * identity twice; a second entry would take a share of the penalty state of the
 * first across a reload, and a failover would "advance" from an endpoint to
 * itself. A caller that walks a resolver answer therefore needs no duplicate
 * test of its own. A false return means the host is not an address, or the list
 * is full and the identity is new.
 *
 * sip_reg_target_add_prio() carries the SRV priority of the record the endpoint
 * came from; sip_reg_target_add() is the same call with priority 0, which is
 * what a plain address answer has. An absorbed duplicate keeps the lower of the
 * two priorities, because one host under two SRV records is reachable at its
 * best one.
 */
bool sip_reg_target_add(struct sip_reg *reg, const char *host, uint16_t port);
bool sip_reg_target_add_prio(struct sip_reg *reg, const char *host,
			     uint16_t port, uint16_t prio);
void sip_reg_targets_clear(struct sip_reg *reg);

/*
 * Replace the whole list with a fresh answer for the same server name, which
 * is what a DNS TTL re-resolution delivers. reload() remembers the identity of
 * the current target, and the identity and the penalty state of every target,
 * and then clears the list exactly as sip_reg_targets_clear() does. The caller
 * re-adds the answer through sip_reg_target_add() and calls reload_end(),
 * which:
 *
 *   - gives every target whose identity came back the penalty state it had;
 *   - points the sticky selection at the new index of the target it was on;
 *   - falls back, when that target is not in the answer, to the same rule a
 *     failure uses (an unpenalized target first, else the one whose penalty
 *     ends soonest), and never to index 0 of the fresh draw.
 *
 * An answer that removes the target of a REGISTER already in flight leaves
 * that request alone. Its outcome then belongs to no target of the new list
 * and penalizes and clears none of them. The owner of an outcome is the entry
 * whose identity equals attempt_dst, so a target that left one answer and
 * returned in the next owns its attempt again: a server that failed is
 * penalized whether or not it blinked out of the answer. A failure of a target
 * the reload moved away from never advances the selection.
 *
 * The priority rule of reload_end(). The line follows the lowest priority group
 * that holds a server with an empty penalty box, and the sticky selection keeps
 * the line only while it is in that group. A selection that is penalized or
 * outranked hands the line to the first unpenalized target of the best group,
 * in list order, which is the fresh RFC 2782 draw. A selection with every
 * target penalized keeps the line, because refusing to send is worse than an
 * early retry. A redraw of one priority therefore keeps the server, and a
 * recovered primary is re-taken at the first re-resolution after its penalty
 * ends.
 *
 * reload_end() reports whether the endpoint identity set moved: true when the
 * new list holds an endpoint the old one did not, or the counts differ. Both
 * lists are duplicate free, so that is set inequality. A pure redraw of the
 * same endpoints reports false, and the first answer after an empty list
 * reports true. It also reports true when the selection's identity moved, so a
 * caller that acts on a changed answer acts on a priority move too.
 *
 * sip_reg_targets_clear() is the full teardown and keeps resetting everything;
 * it is what an account rebuild wants and a re-resolution does not.
 */
void sip_reg_targets_reload(struct sip_reg *reg);
bool sip_reg_targets_reload_end(struct sip_reg *reg);

unsigned int sip_reg_target_count(const struct sip_reg *reg);
unsigned int sip_reg_target_current(const struct sip_reg *reg);
const struct sip_reg_target *sip_reg_target_get(const struct sip_reg *reg, unsigned int idx);

void sip_reg_start(struct sip_reg *reg);

/*
 * unregister true sends a REGISTER with Expires: 0 and reports through
 * unregistered() when the registrar answers; false drops the binding locally.
 * A stop while nothing is registered completes at once.
 */
void sip_reg_stop(struct sip_reg *reg, bool unregister);

enum sip_reg_state sip_reg_state(const struct sip_reg *reg);
uint32_t sip_reg_expires(const struct sip_reg *reg);
const char *sip_reg_contact(const struct sip_reg *reg);

/*
 * The server whose 2xx granted the binding the line holds, or NULL when no
 * binding is live. A caller that watches transport events reads it to tell the
 * connection the binding travels on from a connection toward another server.
 */
const struct sip_txn_dst *sip_reg_binding_dst(const struct sip_reg *reg);

/*
 * The service route the registrar granted (RFC 3608 section 5), rendered as
 * Route headers for a request toward that registrar.
 *
 * The pointer is stable for the life of the registration and never NULL: a
 * registration with no granted route reports a block with len 0, as
 * sip_reg_contact() reports an empty Contact. A caller that copies the pointer
 * into a configuration before the first 2xx therefore reads the granted route
 * once it arrives. sip_reg_done() clears the block, so a stopped registration
 * reports none.
 *
 * The block follows the binding. A 200 replaces it, a 200 without the header
 * clears it (RFC 3608 section 6.1) and the loss of the binding clears it. A
 * caller that selects another server must not send it: the route names the
 * proxies of the server in binding_dst.
 *
 * Not supported. The hop names are never resolved and never change the next
 * hop, so a registrar that is not a proxy and names another host in
 * Service-Route, and a hop that needs another transport, are both out of
 * scope. In uvoice the one resolved target is the outbound proxy by
 * construction, which is what makes the Route and the next hop agree.
 */
const struct sip_route_hdrs *sip_reg_service_route(const struct sip_reg *reg);

/*
 * Replace the Contact host in place. A line that changes address family keeps
 * its targets, penalties and sticky selection. Re-init would discard them.
 * The caller first updates the buffer that cfg.local_host points at, because
 * each request reads its Via host from that buffer.
 *
 * The Contact port returns to cfg.local_port. A NAT-learned port belongs to
 * the mapping of the old address. The Call-ID stays unchanged (RFC 3261 10.2).
 * Use a bare IPv6 address. The Contact writer adds brackets (RFC 3261 19.1.1).
 * This call sends nothing. The caller sends the next REGISTER with
 * sip_reg_refresh_now().
 *
 * Each later REGISTER carries the first old URI as a second Contact with
 * expires=0, if it fits.
 * One request removes that binding and refreshes the new one (RFC 3261 10.2.2).
 * A 2xx response forgets the old URI only if the Contact is not dirty.
 * A dirty Contact means the answered request predates the move.
 * A move back to the old URI or sip_reg_stop(reg, true) also forgets it.
 * If the removal Contact does not fit, the old binding expires on its own.
 *
 * The removal is best effort, and two more rules forget the old URI.
 * A response that moves the Contact onto the old URI forgets it (RFC 3581).
 * Behind NAT the registrar often reports the mapping it reported before the
 * move, and one request must not refresh and remove the same Contact.
 * A final failure response to a REGISTER that carried the old URI forgets it.
 * The retry then carries the new Contact alone.
 * A 401, a 407 and a 423 keep it, because each asks for the same request again.
 * A timeout keeps it, because no registrar judged the request.
 * A failed server move that rolls back keeps it, because the server that
 * refused does not hold the binding.
 *
 * A NULL reg, an empty cfg.local_host or a Contact that does not fit returns
 * false. On false, the old Contact stands. The caller must restore the buffer
 * cfg.local_host points at, or the Via and the Contact would name two families.
 */
bool sip_reg_local_host_set(struct sip_reg *reg);

/*
 * Follow a reload that moved the sticky selection away from the destination of
 * the last attempt, without a stop and start: the Call-ID, the From tag, the
 * cached credentials and the granted binding all stand, the CSeq grows, and no
 * unregistered() report runs. It differs from sip_reg_refresh_now() on
 * purpose, in three ways:
 *
 *   - a REGISTER in flight is not dropped: the exchange completes against its
 *     own destination first, and the retarget follows when it ends. Only a
 *     success sends at once then; a failure armed its own retry, and that
 *     attempt reads the new selection anyway.
 *   - the failure ladder is not reset: a changed answer earns one immediate
 *     attempt, and a pool that is entirely dead must not restart the ladder
 *     on every TTL.
 *   - a retry delay a server named (Retry-After) and the credential backoff
 *     stand: the armed retry reads the new selection when it fires.
 *
 * A stopped registration, an unregister in progress, an empty list, and a
 * selection that still matches the last attempt's destination are all no-ops.
 */
void sip_reg_retarget(struct sip_reg *reg);

/*
 * Refresh a running registration at once after a local host move.
 * Drop the REGISTER in flight and clear the retry backoff before the request.
 * Keep the targets, penalties, selection, Call-ID and cached credentials.
 * There is no down event and no re-resolve.
 * The state becomes REFRESHING with a granted registration, TRYING otherwise,
 * or RESOLVING with no targets.
 * The account layer reads these states as a REGISTER in flight.
 * A stopped registration or an unregister in progress stays unchanged.
 */
void sip_reg_refresh_now(struct sip_reg *reg);

/*
 * Start the attempt a failure backoff holds, because the local transport came
 * back: a socket the caller rebound, or a listener it opened again. The line
 * would otherwise wait out a delay that was earned by a send which cannot fail
 * that way now.
 *
 * It starts an attempt only from SIP_REG_T_RETRY with a delay of our own
 * making, and it returns true then. The ladder keeps its value, so a registrar
 * that is still dead earns the next delay of the ladder and not the first one.
 * sip_reg_refresh_now() is the wrong call here: it resets the ladder.
 *
 * Everything else reports false and changes nothing: a stopped line, an
 * unregister in progress, a line that holds a binding or waits on any timer
 * other than the retry, a delay a server named (Retry-After), the credential
 * backoff, and a line with no target, which waits for the resolver.
 *
 * True says the attempt started, not that it left the machine. A send that
 * fails again takes the whole failure path inside this call: the failure run
 * grows by one, the target is penalized, the next delay of the ladder is armed
 * and the failed() callback runs, all before the true return. Call it for a
 * line whose transport is back, and be ready for the callback to run inside
 * the call.
 */
bool sip_reg_retry_now(struct sip_reg *reg);

/*
 * The kind of the armed timer. reg->timer tells it: none when the timer is not
 * armed, refresh when its id is SIP_TIMER_REG_REFRESH, retry otherwise.
 */
enum sip_reg_timer sip_reg_timer_kind(const struct sip_reg *reg);

const char *sip_reg_call_id(const struct sip_reg *reg);
uint32_t sip_reg_cseq(const struct sip_reg *reg);

/*
 * The refresh delay of an accepted expiry, exported because it is a rule with
 * three boundary cases the tests pin (expires 30, expires below 15, and the
 * ordinary 3600).
 */
uint32_t sip_reg_refresh_secs(uint32_t expires);

/*
 * The daemon routes the transaction upcalls by owner. Both are safe to call
 * with a transaction this registration does not own; they ignore it.
 */
void sip_reg_txn_resp(struct sip_reg *reg, uint32_t txn, const struct sip_msg *msg);
void sip_reg_txn_done(struct sip_reg *reg, uint32_t txn, enum sip_txn_reason reason);

#endif
