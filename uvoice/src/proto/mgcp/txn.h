// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * MGCP transaction layer of a gateway (RFC 3435 sections 3.5 and 4.3): the
 * commands the gateway sends, the at-most-once record of the commands it
 * receives, the three-way handshake and the timers of the disconnected
 * procedure (4.4.7).
 *
 * Dependencies. libc, the MGCP text layer (mgcp/msg.h) and the shared timer
 * (proto/shared/timer.h). No uloop, no socket. Everything from the outside
 * arrives through struct mgcp_txn_env: the clock, the timer pair and the
 * entropy hook of its base, one send function and the upcalls. A suite
 * drives the layer on a virtual clock with a capture send.
 *
 * Memory. mgcp_txn_mgr_init() allocates the transaction pool with one wire
 * buffer of cfg.wire_max bytes per transaction, and the record of received
 * commands with one buffer of cfg.wire_max + 4 bytes per entry, so the K:
 * line of 3.5.6 always fits. With the defaults that is 16 * 4096 +
 * 32 * 4100 = 196736 bytes. Nothing allocates after that. A message longer
 * than wire_max is refused and counted, never truncated. The 4096 byte
 * default covers the 4000 byte datagram of 3.5.4.
 *
 * Gateway commands (3.5.3, 4.3). mgcp_txn_send() puts a command on the wire
 * at once and keeps a copy. The copy goes out again on the retransmission
 * timer:
 *   - the first timer is the estimator RTO, AAD + 4 * ADEV (3.5.3, with the
 *     gains of RFC 6298 2.3), or cfg.rto_init_ms before the first sample;
 *   - T-DELAY starts at that first timer, after its bounds; after each
 *     retransmission the transaction doubles T-DELAY and arms a random value
 *     between T-DELAY / 2 and T-DELAY, plus 4 * ADEV, bounded by
 *     cfg.rto_min_ms and cfg.rto_max_ms (3.5.3), so the backoff stays
 *     exponential above the floor;
 *   - a provisional response switches the timer to cfg.longtran_ms (3.5.6)
 *     and ends the Max1 and Max2 checks below, because the call agent
 *     answered;
 *   - a retransmission due more than cfg.t_max_ms after the first send is
 *     not sent: retransmissions cease (4.3);
 *   - when the retransmissions to the current address reach cfg.max1, the
 *     layer calls max1() once for that address and keeps retransmitting to
 *     it until the caller moves the transaction with mgcp_txn_redirect(),
 *     which starts the count again for the new address (4.3, page 125);
 *   - when cfg.max2 retransmissions to the current address stay unanswered,
 *     the layer calls max2() once for that address. A redirect inside it
 *     keeps the transaction alive (4.3, page 126). Otherwise the transaction
 *     ends with MGCP_TXN_DONE_TIMEOUT and disconnected() runs. With
 *     MGCP_TXN_CFG_MAX2_QUIET the retransmissions cease instead and the end
 *     waits for 2 * T-HIST, as NCS 1.5 7.4.2 asks;
 *   - 2 * cfg.t_hist_ms after the first send a transaction that holds no
 *     final response ends with MGCP_TXN_DONE_TIMEOUT and disconnected()
 *     runs (3.5.6, 4.3).
 * A sample feeds the estimator only from a transaction with no
 * retransmission. A transaction that needed retransmissions passes its last
 * RTO and its T-DELAY to the next commands, until one completes without
 * retransmission (3.5.3).
 *
 * A final response from the call agent that carries a K: line gets a "000"
 * response acknowledgment from this layer, whatever the values of the line
 * (3.3, page 103: "any ResponseAck values provided will be ignored"). Any
 * other final goes onto the gateway's K: list, which mgcp_txn_acks_get()
 * offers to the next command of the same owner to the same destination,
 * and which loses an id once a command that carries it is sent (3.5.2). A
 * repeated final of a completed transaction is absorbed, and a "000" goes
 * out again when it asked for one. Two acknowledged NTFY commands in one
 * datagram (4.4.1 step d) are not supported: the endpoint layer sends one
 * at a time (4.4.1 step f).
 *
 * Commands from the call agent (3.5.1, 3.5.6). Each new command gets an
 * entry in the record and one command() upcall. A repeated command, matched
 * on the transaction id alone (3.2.1.2, 3.5.2), never reaches the caller:
 *   - an answered one gets the retained response again, sent to the source
 *     of the repetition (3.5);
 *   - one still running gets a provisional "100" when its verb is in
 *     cfg.prov_verbs (3.5.1, 3.5.6; NCS 1.5 8.8 limits it to CRCX and
 *     MDCX), and nothing otherwise;
 *   - a confirmed one is discarded (3.5.2).
 * A command whose first line carries MGCP_MSG_F_VERB_BAD or
 * MGCP_MSG_F_BAD_VERSION reaches command() like any other, and the caller
 * answers it; the text after a "." line inside an SDP body parses as such
 * a command (mgcp/msg.h).
 * A final that follows a provisional carries an empty K: (the layer adds it
 * after the response line of the message that names the command, when the
 * caller's text lacks it, so a datagram that puts an RSIP first keeps the
 * RSIP unchanged) and goes out again on the RTO ladder above until a "000"
 * or a K: range of a later command confirms it, or until T-MAX. Its loss
 * never makes the gateway disconnected (3.5.6). The record keeps every
 * answered command for cfg.t_hist_ms after its final response. When it is
 * full, a confirmed or answered entry goes first, the oldest of them, and
 * an entry still running or awaiting its "000" goes last, so a repeated
 * command never runs twice because its running entry was evicted.
 *
 * Disconnected procedure (4.4.7). mgcp_txn_disc_enter() arms the first
 * delay, uniform between 1 s and cfg.td_init_ms. The expiry calls
 * disc_attempt() and arms nothing: the caller then sends RSIP
 * "disconnected" through mgcp_txn_send() with MGCP_TXN_F_DISC, and one such
 * transaction is outstanding at a time. The attempt fails when the RSIP
 * ends without a final response (MGCP_TXN_DONE_TIMEOUT or
 * MGCP_TXN_DONE_TRANSPORT) or with a final that is not a success. The
 * layer then arms the next delay from that end, double the last one and
 * bounded by cfg.td_max_ms (step 4), and keeps the procedure active. An
 * RSIP that the pool refuses or whose first send fails is a failed attempt
 * too, so the procedure never stalls. An RSIP that never reaches the layer
 * (no address for the notified entity) is a failed attempt the caller
 * reports with mgcp_txn_disc_fail(). With MGCP_TXN_CFG_TD_RANDOM the next
 * delay is random between 1.5 and 2 times the last one (NCS 1.5
 * 7.4.3.6). Only a success response completes the procedure (page 144):
 * the layer leaves it and calls disc_ended() before response(). An error
 * response reaches response() like any other, and the caller reads the
 * code; for an immediate retry (a 521 with a new notified entity) it calls
 * mgcp_txn_disc_kick(mgr, false), which keeps the accumulated delay. An
 * aborted RSIP arms nothing, because the caller replaces it.
 * mgcp_txn_disc_kick() starts an attempt at once for a received command,
 * or for local activity when cfg.td_min_ms passed since entry or since the
 * last attempt ended; the next delay then waits for the end of that
 * attempt. mgcp_txn_disc_leave() ends the procedure. The layer sends
 * nothing on its own here.
 *
 * Entropy. The caller must give base.rand32 from the entropy of the
 * operating system: 4.4.7 step 1 forbids draws that gateways share. The
 * generator a NULL base.rand32 selects is seeded from the clock and serves
 * a suite alone.
 *
 * Lifetime. Transactions live in a fixed pool. A caller stores the handle
 * that mgcp_txn_send() returns and resolves it with mgcp_txn_get(), which
 * checks the generation; the handle is (index << 16) | generation and never
 * 0. owner is the caller's value: the layer stores it and never reads it.
 *
 * No reentrancy. Every upcall runs from a timer expiry or from a receive.
 * A call the caller makes never runs an upcall, with one exception:
 * mgcp_txn_abort() reports MGCP_TXN_DONE_ABORT before it returns, as
 * sip_txn_abort() does. A first send that fails inside mgcp_txn_send()
 * returns 0 and runs no upcall. A response that ends a transaction reaches
 * the caller after the transaction left the pool, so mgcp_txn_get() on its
 * handle answers NULL inside response() and done().
 */
#ifndef UVOICE_MGCP_TXN_H
#define UVOICE_MGCP_TXN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "proto/mgcp/msg.h"
#include "proto/shared/timer.h"

#define MGCP_TXN_HANDLE_NONE		0u
#define MGCP_TXN_OWNER_ANY		UINT32_MAX

/* RFC 3435 4.3 */
#define MGCP_TXN_MAX1_DEFAULT		5
#define MGCP_TXN_MAX2_DEFAULT		7
#define MGCP_TXN_T_MAX_DEFAULT		20000
/* 3.5.1 */
#define MGCP_TXN_T_HIST_DEFAULT		30000
/* 3.5.3 RTO-MAX; NCS 1.5 8.5.2 gives the 200 ms initial value */
#define MGCP_TXN_RTO_MAX_DEFAULT	4000
#define MGCP_TXN_RTO_INIT_DEFAULT	200
/* 3.5.3 asks for a floor on the RTO and names no value */
#define MGCP_TXN_RTO_MIN_DEFAULT	100
/* 3.5.6 LONGTRAN-TIMER */
#define MGCP_TXN_LONGTRAN_DEFAULT	5000
/* 4.4.7 */
#define MGCP_TXN_TD_INIT_DEFAULT	15000
#define MGCP_TXN_TD_MIN_DEFAULT		15000
#define MGCP_TXN_TD_MAX_DEFAULT		600000

#define MGCP_TXN_POOL_DEFAULT		16
#define MGCP_TXN_HIST_DEFAULT		32
#define MGCP_TXN_WIRE_DEFAULT		4096

/*
 * The bound of cfg.t_hist_ms and cfg.td_max_ms, one hour, so that
 * 2 * T-HIST fits an unsigned int and a delay of 1.5 times Tdmax fits too.
 */
#define MGCP_TXN_LONG_MAX_MS		3600000

/* the N of 3.5.3, the K of RFC 6298 2.3 */
#define MGCP_TXN_RTO_K			4

/* cfg.flags */
#define MGCP_TXN_CFG_MAX2_QUIET		(1u << 0)	/* NCS 1.5 7.4.2 */
#define MGCP_TXN_CFG_TD_RANDOM		(1u << 1)	/* NCS 1.5 7.4.3.6 */

/*
 * The layer's configuration. mgcp_txn_cfg_default() fills the RFC values; a
 * zero field takes its default at init. The init also keeps three relations
 * of the text: Max2 above Max1 (4.3), T-HIST at least T-MAX (4.3) and
 * LONGTRAN below T-HIST (3.5.6). It bounds t_hist_ms and td_max_ms by
 * MGCP_TXN_LONG_MAX_MS.
 */
struct mgcp_txn_cfg {
	unsigned int max1;
	unsigned int max2;
	uint32_t rto_init_ms;
	uint32_t rto_min_ms;
	uint32_t rto_max_ms;
	uint32_t t_max_ms;
	uint32_t t_hist_ms;
	uint32_t longtran_ms;
	uint32_t td_init_ms;
	uint32_t td_min_ms;
	uint32_t td_max_ms;
	/* 1u << enum mgcp_verb for each verb whose repetition gets a 100 */
	uint32_t prov_verbs;
	uint32_t flags;
	uint32_t txid_first;		/* the first id the pool gives, 0: 1 */
	unsigned int pool_max;
	unsigned int hist_max;
	size_t wire_max;
};

void mgcp_txn_cfg_default(struct mgcp_txn_cfg *cfg);

enum mgcp_txn_reason {
	MGCP_TXN_DONE_OK = 0,		/* a final response arrived */
	MGCP_TXN_DONE_TIMEOUT,		/* no final response, see above */
	MGCP_TXN_DONE_TRANSPORT,	/* send() refused a retransmission */
	MGCP_TXN_DONE_ABORT,		/* the caller ended it */
};

const char *mgcp_txn_reason_name(enum mgcp_txn_reason reason);

/*
 * The environment. base is the first member, so a pointer to it converts to
 * the environment (timer.h). base.now_ms, base.timer_set,
 * base.timer_cancel and send are required; everything else can be NULL. A
 * NULL base.rand32 selects a generator seeded from base.now_ms.
 *
 * send() answers the bytes sent or a negative errno.
 * command() delivers a new command from the call agent; the caller answers
 * it with mgcp_txn_respond(), inside the upcall or later.
 * response() delivers the final response to a command the gateway sent.
 * done() reports the end of a gateway transaction; after a response() it
 * follows with MGCP_TXN_DONE_OK.
 * disconnected() reports that a transaction met a disconnection rule of
 * 4.3. It does not run while the disconnected procedure is active.
 * max1() reports that the retransmissions of one transaction to its current
 * address reached Max1 (4.3, page 125). The caller can move it to an
 * address not tried yet with mgcp_txn_redirect(); when to query the DNS
 * again is the caller's rule.
 * max2() reports that they reached Max2 (4.3, page 126). A redirect inside
 * it keeps the transaction alive; otherwise it ends.
 * Both run for transactions of a released owner too, and the layer fetches
 * the transaction again by its handle after each, so an abort inside them
 * is safe.
 * disc_attempt() asks for an RSIP of the disconnected procedure.
 * disc_ended() reports that a success response to that RSIP completed the
 * procedure; the response() that follows carries it.
 */
struct mgcp_txn_env {
	struct proto_timer_env base;

	int (*send)(struct mgcp_txn_env *env, const void *buf, size_t len,
		    const struct sockaddr *dst, socklen_t dst_len);
	void (*command)(struct mgcp_txn_env *env, const struct mgcp_msg *msg,
			const struct sockaddr *src, socklen_t src_len);
	void (*response)(struct mgcp_txn_env *env, uint32_t owner,
			 uint32_t txid, const struct mgcp_msg *msg);
	void (*done)(struct mgcp_txn_env *env, uint32_t owner, uint32_t txid,
		     enum mgcp_txn_reason reason);
	void (*disconnected)(struct mgcp_txn_env *env);
	void (*max1)(struct mgcp_txn_env *env, uint32_t owner, uint32_t txid,
		     uint32_t handle);
	void (*max2)(struct mgcp_txn_env *env, uint32_t owner, uint32_t txid,
		     uint32_t handle);
	void (*disc_attempt)(struct mgcp_txn_env *env);
	void (*disc_ended)(struct mgcp_txn_env *env);
};

struct mgcp_txn_mgr;

/* one command the gateway sent */
struct mgcp_txn {
	struct proto_timer retx;
	struct proto_timer life;
	struct mgcp_txn_mgr *mgr;
	struct sockaddr_storage dst;
	socklen_t dst_len;
	char *wire;
	size_t len;
	uint64_t first_ms;
	uint32_t txid;
	uint32_t owner;
	uint32_t rto_ms;		/* the timer armed last */
	uint32_t tdelay_ms;		/* T-DELAY of 3.5.3 */
	unsigned int sends;		/* the first send included */
	unsigned int reps;		/* N of 4.3: retransmissions to dst */
	unsigned int index;
	uint16_t gen;
	bool in_use;
	bool answered;			/* a response of any class arrived */
	bool provisional;
	bool quiet;			/* retransmissions ceased */
	bool max1_seen;			/* max1() ran for dst */
	bool max2_seen;			/* max2() ran for dst */
	bool redirected;
	bool detached;
	bool disc;			/* MGCP_TXN_F_DISC */
};

enum mgcp_hist_state {
	MGCP_HIST_FREE = 0,
	MGCP_HIST_RUNNING,		/* received, not answered */
	MGCP_HIST_PROVISIONAL,		/* a 1xx went out */
	MGCP_HIST_FINAL,		/* the final went out */
	MGCP_HIST_ACKWAIT,		/* final after a 1xx, awaiting "000" */
	MGCP_HIST_CONFIRMED,		/* final confirmed, id kept */
};

/* one command the gateway received (3.5.1) */
struct mgcp_txn_hist {
	struct proto_timer timer;
	struct mgcp_txn_mgr *mgr;
	struct sockaddr_storage peer;
	socklen_t peer_len;
	char *wire;
	size_t len;
	uint64_t stamp_ms;		/* receipt */
	uint64_t final_ms;		/* the final response */
	uint32_t txid;
	uint32_t rto_ms;
	uint32_t tdelay_ms;
	uint8_t state;			/* enum mgcp_hist_state */
	uint8_t verb;			/* enum mgcp_verb */
	bool prov_sent;
};

/* a gateway transaction that ended with a final response */
struct mgcp_txn_past {
	struct sockaddr_storage peer;
	socklen_t peer_len;
	uint64_t stamp_ms;
	uint32_t txid;
	bool ack;			/* its final asked for a "000" */
};

/* a final response the gateway still has to confirm with K: (3.5.2) */
struct mgcp_txn_ack {
	struct sockaddr_storage dst;
	socklen_t dst_len;
	uint64_t stamp_ms;
	uint32_t txid;
	uint32_t owner;
};

struct mgcp_txn_stat {
	uint64_t created;		/* gateway commands started */
	uint64_t retransmitted;		/* commands and finals sent again */
	uint64_t absorbed;		/* repetitions the layer answered */
	uint64_t delivered;		/* commands and finals passed up */
	uint64_t timeout;
	uint64_t transport;		/* send failures */
	uint64_t denied;		/* the pool was full */
	uint64_t stray;			/* responses with no transaction */
	uint64_t evicted;		/* record entries lost before T-HIST */
	uint64_t malformed;		/* messages the parser refused */
	uint64_t acked;			/* finals confirmed by "000" or K: */
	uint64_t oversize;		/* messages over wire_max */
};

struct mgcp_txn_disc {
	struct proto_timer timer;
	uint64_t entered_ms;
	uint64_t end_ms;		/* the last attempt that ended */
	uint32_t td_ms;			/* the delay armed last */
	bool active;
};

struct mgcp_txn_mgr {
	struct mgcp_txn_env *env;
	struct mgcp_txn_cfg cfg;
	struct mgcp_txn *pool;
	struct mgcp_txn_hist *hist;
	struct mgcp_txn_past *past;
	unsigned int past_next;
	struct mgcp_txn_ack ack[MGCP_ACK_LIST_MAX];
	unsigned int ack_count;
	char *wires;
	unsigned int active;
	uint32_t txid_next;
	uint32_t rand_state;
	uint32_t aad_us;		/* AAD of 3.5.3 */
	uint32_t adev_us;		/* ADEV of 3.5.3 */
	bool rtt_valid;
	uint32_t rto_carry_ms;		/* last RTO of a retransmitted one */
	uint32_t tdelay_carry_ms;	/* its T-DELAY */
	struct mgcp_txn_disc disc;
	struct mgcp_txn_stat stat;
};

/* 0, or -EINVAL for a NULL argument or required member, or -ENOMEM */
int mgcp_txn_mgr_init(struct mgcp_txn_mgr *mgr, struct mgcp_txn_env *env,
		      const struct mgcp_txn_cfg *cfg);

/* Detach and end every transaction without an upcall and free the pool. */
void mgcp_txn_mgr_done(struct mgcp_txn_mgr *mgr);

uint32_t mgcp_txn_rand(struct mgcp_txn_mgr *mgr);

/* The next transaction id: 1 to MGCP_TXID_MAX, wrapping, never one in use. */
uint32_t mgcp_txn_txid_next(struct mgcp_txn_mgr *mgr);

/* the RTO a new command starts with */
uint32_t mgcp_txn_rto_ms(const struct mgcp_txn_mgr *mgr);

/* ---------------------------------------------------------- gateway side */

#define MGCP_TXN_F_DISC			(1u << 0)	/* RSIP of 4.4.7 */

/*
 * A command to send. txid comes from mgcp_txn_txid_next() and is the one
 * in wire. acks is the K: list wire carries, as mgcp_txn_acks_get() gave
 * it, or NULL.
 */
struct mgcp_txn_req {
	uint32_t txid;
	const void *wire;
	size_t len;
	const struct sockaddr *dst;
	socklen_t dst_len;
	uint32_t owner;
	const struct mgcp_ack_list *acks;
	uint32_t flags;
};

/* The handle, or MGCP_TXN_HANDLE_NONE when the command did not go out. */
uint32_t mgcp_txn_send(struct mgcp_txn_mgr *mgr,
		       const struct mgcp_txn_req *req);

uint32_t mgcp_txn_handle(const struct mgcp_txn *txn);
struct mgcp_txn *mgcp_txn_get(struct mgcp_txn_mgr *mgr, uint32_t handle);
uint32_t mgcp_txn_find(struct mgcp_txn_mgr *mgr, uint32_t txid);
unsigned int mgcp_txn_count(const struct mgcp_txn_mgr *mgr);

/* End a transaction now; done() reports MGCP_TXN_DONE_ABORT before return. */
void mgcp_txn_abort(struct mgcp_txn_mgr *mgr, uint32_t handle);

/* Stop the upcalls of an owner's transactions; they run to their end. */
unsigned int mgcp_txn_owner_release(struct mgcp_txn_mgr *mgr,
				    uint32_t owner);

/*
 * Move the outstanding commands of owner (MGCP_TXN_OWNER_ANY: of every
 * owner) to dst, with the repetition count back at 0 and max1() and max2()
 * free to run again for dst (4.3). The first send to dst comes from a timer
 * expiry. Answers the count moved.
 */
unsigned int mgcp_txn_redirect(struct mgcp_txn_mgr *mgr, uint32_t owner,
			       const struct sockaddr *dst, socklen_t dst_len);

/*
 * The K: list for the next command of owner (MGCP_TXN_OWNER_ANY: any owner)
 * to dst: finals that arrived inside T-HIST from commands sent to dst.
 */
unsigned int mgcp_txn_acks_get(struct mgcp_txn_mgr *mgr, uint32_t owner,
			       const struct sockaddr *dst, socklen_t dst_len,
			       struct mgcp_ack_list *out);

/* ----------------------------------------------------------- receive side */

enum mgcp_txn_rx {
	MGCP_RX_IGNORED = 0,		/* not usable */
	MGCP_RX_ABSORBED,		/* the layer handled it */
	MGCP_RX_DELIVERED,		/* passed up */
	MGCP_RX_STRAY,			/* a response with no transaction */
};

const char *mgcp_txn_rx_name(enum mgcp_txn_rx rx);

enum mgcp_txn_rx mgcp_txn_msg_recv(struct mgcp_txn_mgr *mgr,
				   const struct mgcp_msg *msg,
				   const struct sockaddr *src,
				   socklen_t src_len);

/*
 * One datagram: every message in order, each on its own (3.5.5). A message
 * the parser refuses is counted and skipped. Answers the count parsed.
 */
unsigned int mgcp_txn_recv(struct mgcp_txn_mgr *mgr, const char *buf,
			   size_t len, const struct sockaddr *src,
			   socklen_t src_len);

/*
 * Answer the command txid. code 100 to 199 is a provisional, any higher code
 * a final; the record keeps wire for the repetitions. src is the source of
 * the command and recreates its entry when the record lost it. false when
 * the command is already answered, when wire does not fit, when a final
 * after a provisional holds no response line for txid, or when send()
 * refused it; in the last case the entry keeps the response.
 */
bool mgcp_txn_respond(struct mgcp_txn_mgr *mgr, uint32_t txid,
		      const struct sockaddr *src, socklen_t src_len,
		      uint32_t code, const void *wire, size_t len);

/* ------------------------------------------------- disconnected procedure */

void mgcp_txn_disc_enter(struct mgcp_txn_mgr *mgr);

/*
 * A command arrived (local false) or local activity happened (local true).
 * true asks the caller to start an attempt now. The pending delay is
 * canceled; the next one is armed when that attempt ends without a final
 * response.
 */
bool mgcp_txn_disc_kick(struct mgcp_txn_mgr *mgr, bool local);

void mgcp_txn_disc_leave(struct mgcp_txn_mgr *mgr);
bool mgcp_txn_disc_active(const struct mgcp_txn_mgr *mgr);

/*
 * The attempt the caller started never reached the layer. It ends as a lost
 * RSIP does: the next delay arms, doubled as step 4 asks. Nothing happens
 * while the procedure is not active.
 */
void mgcp_txn_disc_fail(struct mgcp_txn_mgr *mgr);

/*
 * Move one outstanding command, by its handle, to dst as mgcp_txn_redirect()
 * moves those of an owner. The switch of Max1 and Max2 (4.3, page 125)
 * applies to the one command that reached the limit. false when the handle
 * names no command in progress or dst does not fit.
 */
bool mgcp_txn_redirect_one(struct mgcp_txn_mgr *mgr, uint32_t handle,
			   const struct sockaddr *dst, socklen_t dst_len);

#endif
