// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * RFC 3261 section 17 transaction layer: the four state machines, their
 * timers, the two match indexes and the retained wire buffers.
 *
 * Dependencies. libc plus the libubox avl for the two indexes. No uloop, no
 * socket, no ubus, no HAL. Everything the layer needs from the outside arrives
 * through struct sip_txn_env: one send function, one clock, a timer pair and
 * the upcalls. A unit test drives the whole layer with a virtual clock and a
 * capture buffer; uvoiced hands it the uloop timer implementation from
 * txn_uloop.c and the UDP transport.
 *
 * Buffer contract. shared/msg.h buffers belong to the caller and die with the
 * datagram, so every transaction copies what it may have to put on the wire
 * again. Each transaction owns two buffers:
 *
 *   req   client: the request, retransmitted by timer A or E.
 *         server: the request as received, so a late final response (487 to a
 *                 cancelled INVITE, 408, 422) can be built from it without the
 *                 transport buffer. sip_txn_request_get() reparses it.
 *   rsp   the answer to a retransmission from the peer. For a client
 *         transaction that is the ACK for a non-2xx final (RFC 3261 17.1.1.3);
 *         for a server transaction it is the last response sent.
 *
 * Both are one allocation each, taken when the message is first sent or
 * received. No timer path allocates.
 *
 * Lifetime. The transactions live in a fixed pool. A caller never stores a
 * struct sip_txn pointer across an upcall or a uloop iteration: it stores the
 * handle from sip_txn_handle() and resolves it with sip_txn_get(), which
 * validates the generation. The handle is (index << 16) | generation, and it
 * is what makes an owner that vanished mid transaction harmless.
 *
 * Owner. owner is an opaque handle the TU assigns, carried back in every
 * upcall. The layer never dereferences it. sip_txn_owner_release() detaches
 * every transaction of an owner that went away: the transactions keep running
 * to their protocol end (up to timer J or K), and their upcalls stop.
 */
#ifndef UVOICE_SIP_TXN_H
#define UVOICE_SIP_TXN_H

#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include <libubox/avl.h>

#include "proto/shared/timer.h"
#include "proto/shared/dst.h"
#include "proto/shared/handle.h"
#include "proto/shared/msg.h"

#define SIP_TXN_MAX		64

/*
 * The server match key holds "s|<branch>|<host>:<port>|<method>". RFC 1035
 * caps a host name at 255 bytes and RFC 3261 8.1.1.7 builds a branch of about
 * 40, so 640 covers the worst combination a conforming peer can send with room
 * to spare. A key that still does not fit is refused: it is never truncated
 * and never demoted to the pre-RFC 3261 rule, because either would let two
 * different branches share one transaction.
 */
#define SIP_TXN_KEY_MAX		640
#define SIP_BRANCH_MAX		64
#define SIP_BRANCH_MAGIC	"z9hG4bK"
#define SIP_PORT_DEFAULT	5060

/* ------------------------------------------------------------------ timers */

enum sip_timer_id {
	SIP_TIMER_NONE = 0,
	SIP_TIMER_A,		/* ICT request retransmission, doubles */
	SIP_TIMER_B,		/* ICT transaction timeout, 64*T1 */
	SIP_TIMER_D,		/* ICT wait for response retransmissions */
	SIP_TIMER_E,		/* NICT request retransmission, doubles to T2 */
	SIP_TIMER_F,		/* NICT transaction timeout, 64*T1 */
	SIP_TIMER_K,		/* NICT wait for response retransmissions, T4 */
	SIP_TIMER_G,		/* IST final response retransmission, doubles to T2 */
	SIP_TIMER_H,		/* IST wait for ACK, 64*T1 */
	SIP_TIMER_I,		/* IST wait for ACK retransmissions, T4 */
	SIP_TIMER_J,		/* NIST wait for request retransmissions, 64*T1 */
	SIP_TIMER_2XX,		/* ua.c, RFC 3261 13.3.1.4 2xx retransmission */
	SIP_TIMER_ACCEPT,	/* ua.c, RFC 6026 accepted state, 64*T1 */
	SIP_TIMER_GLARE,	/* ua.c, RFC 3261 14.1 491 retry */
	SIP_TIMER_TRYING,	/* ua.c, RFC 3261 17.2.1 automatic 100, 200 ms */
	SIP_TIMER_REG_REFRESH,	/* reg.c, refresh of an accepted binding */
	SIP_TIMER_REG_RETRY,	/* reg.c, next attempt after a failure */
	SIP_TIMER_CALL_SESSION,	/* call.c, RFC 4028 refresh we owe the peer */
	SIP_TIMER_CALL_WATCHDOG,/* call.c, RFC 4028 refresh the peer owes us */
	SIP_TIMER_CALL_C,	/* call.c, the INVITE setup bound */
	SIP_TIMER_CALL_REL,	/* call.c, RFC 3262 3 reliable 1xx retx */
	SIP_TIMER_KA_CRLF,	/* keepalive.c, the CRLF NAT keepalive tick */
	SIP_TIMER_KA_OPTIONS,	/* keepalive.c, the OPTIONS ping tick */
	SIP_TIMER_SUB_REFRESH,	/* sub.c, refresh of an accepted subscription */
	SIP_TIMER_SUB_RETRY,	/* sub.c, next attempt after a failure */
	SIP_TIMER_ID_MAX,
};

const char *sip_timer_name(enum sip_timer_id id);

/*
 * Timer values in milliseconds. The caller fills this from the uci options
 * sip_timer_t1, _t2, _t4 and sip_timer_a to _k; this layer never reads uci.
 * a, e and g are the first interval of a doubling series, and the series for E
 * and G is capped at t2 (RFC 3261 17.1.2.2, 17.2.1); the A series is not
 * capped (17.1.1.2).
 */
struct sip_timer_cfg {
	uint32_t t1;
	uint32_t t2;
	uint32_t t4;
	uint32_t a;
	uint32_t b;
	uint32_t d;
	uint32_t e;
	uint32_t f;
	uint32_t g;
	uint32_t h;
	uint32_t i;
	uint32_t j;
	uint32_t k;
};

void sip_timer_cfg_default(struct sip_timer_cfg *tc);

/*
 * Derive every zero field from t1, t2 and t4 and clamp the result, so a config
 * that sets sip_timer_t1 alone still gets an RFC consistent set.
 */
void sip_timer_cfg_fixup(struct sip_timer_cfg *tc);

/*
 * The timer of this layer is the shared protocol timer (proto/shared/timer.h).
 * The SIP names stay as aliases, so the stack keeps its own vocabulary: a
 * struct sip_timer is a struct proto_timer, and the four helpers below run the
 * shared ones on the embedded environment of struct sip_txn_env.
 */
#define sip_timer		proto_timer
#define SIP_TIMER_PRIV_SIZE	PROTO_TIMER_PRIV_SIZE

struct sip_txn_env;

typedef proto_timer_cb sip_timer_cb;

/* ----------------------------------------------------------- destinations */

/*
 * The destination came from an accepted stream connection, so its port is the
 * peer's ephemeral source port and no SIP element listens on it (sip/stream.h
 * "Dst provenance"). A send to it after its connection is gone fails at once
 * instead of opening a connection that could only reach a closed port. The UDP
 * transport neither sets nor reads it.
 */
#define SIP_DST_F_NO_CONNECT	(1u << 0)

bool sip_dst_set_str(struct sip_txn_dst *dst, struct sip_str host, uint16_t port);
uint16_t sip_dst_port(const struct sip_txn_dst *dst);

/*
 * Response destination for a request we received (RFC 3261 18.2.2,
 * RFC 3581 section 4):
 *
 *   ;rport present  -> the source address and the source port of the request.
 *   otherwise       -> the source address, port from the Via sent-by, 5060
 *                      when the sent-by carries none.
 *
 * The address is always the source address of the datagram. That is the
 * "received" rule of 18.2.2 for every case where the two differ, it is what
 * makes a UA behind NAT reachable, and this layer has no resolver to turn a
 * sent-by host name into an address.
 */
bool sip_dst_from_via(const struct sip_msg *req, const struct sip_txn_dst *src,
		      struct sip_txn_dst *out);

/* ------------------------------------------------------------ environment */

enum sip_txn_reason {
	SIP_TXN_DONE_OK = 0,		/* the transaction ran to its end */
	SIP_TXN_DONE_TIMEOUT,		/* timer B, F or H: treat as 408 */
	SIP_TXN_DONE_TRANSPORT,		/* the transport refused a send */
	SIP_TXN_DONE_ABORT,		/* the TU killed it */
};

const char *sip_txn_reason_name(enum sip_txn_reason reason);

struct sip_txn;

/*
 * Reliable transport timers. The environment knows what it sends on, so the
 * transport class travels with it as one bit. A zero initialised environment
 * is UDP: every timer decision below is taken only when the bit is set.
 *
 * With SIP_TXN_ENV_RELIABLE the transaction layer arms this set:
 *
 *   A  ICT request retransmission     not armed   RFC 3261 17.1.1.2
 *   E  NICT request retransmission    not armed   RFC 3261 17.1.2.2
 *   G  IST response retransmission    not armed   RFC 3261 17.2.1
 *   D  ICT wait for retransmissions   0 ms        RFC 3261 17.1.1.2
 *   K  NICT wait for retransmissions  0 ms        RFC 3261 17.1.2.2
 *   I  IST wait for ACK retransmits   0 ms        RFC 3261 17.2.1
 *   J  NIST wait for retransmissions  0 ms        RFC 3261 17.2.2
 *   B  ICT transaction timeout        unchanged
 *   F  NICT transaction timeout       unchanged
 *   H  IST wait for ACK               unchanged
 *
 * The four zero millisecond arms fire on the next tick rather than running
 * inline, which keeps the no reentrancy shape of the layer: a transaction
 * still ends from a timer callback and never from inside the call that
 * completed it.
 *
 * Only sip/txn.c reads the flag. The ua.c timers are out of scope by design,
 * not by oversight: SIP_TIMER_2XX must keep retransmitting a 2xx over a
 * reliable transport (RFC 3261 13.3.1.4, because the ACK can still be lost),
 * SIP_TIMER_ACCEPT is the only detector of that lost ACK (RFC 6026), and
 * SIP_TIMER_GLARE and SIP_TIMER_TRYING measure peer behaviour, not
 * retransmission. The reg, sub, keepalive and call timers are out of scope for
 * the same reason.
 *
 * Response destination. On a reliable transport a server transaction answers
 * on the connection the request arrived on (RFC 3261 18.2.2), so it takes the
 * src it was given verbatim and does not compute one from the Via sent-by with
 * sip_dst_from_via().
 */
#define SIP_TXN_ENV_RELIABLE	(1u << 0)

/*
 * base is the shared timer environment (proto/shared/timer.h): priv, the
 * clock, the timer pair, the entropy hook and the trace sink. Its now_ms(),
 * timer_set() and timer_cancel() are required; its rand32() and trace() may
 * be NULL. It is the first member, so a pointer to base converts to a pointer
 * to this environment (sip_txn_env_of) and back.
 *
 * send() is required. It returns 0 or more on success and a negative value on
 * a transport error, which terminates the transaction (RFC 3261 17.1.1.2,
 * 17.1.2.2, 17.2.1).
 *
 * resp() delivers a response a client transaction accepted. The TU must build
 * the ACK for a non-2xx final inside this call and hand it to
 * sip_txn_ack_set(); the transaction owns every later retransmission of it.
 *
 * done() reports the end of a transaction. The transaction is already gone
 * when it runs, so the handle serves to identify it and to clear it; a
 * sip_txn_get() on it returns NULL.
 *
 * base.trace() takes a preformatted line, so the environment needs no va_list
 * plumbing. With base.rand32() NULL the layer uses its own generator seeded
 * from base.now_ms().
 */
struct sip_txn_env {
	struct proto_timer_env base;

	/* SIP_TXN_ENV_RELIABLE alone; 0 is UDP */
	uint32_t flags;

	int (*send)(struct sip_txn_env *env, const void *buf, size_t len,
		    const struct sip_txn_dst *dst);

	void (*resp)(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
		     const struct sip_msg *msg);
	void (*done)(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
		     enum sip_txn_reason reason);
};

/* The environment an entry of base was called with. */
static inline struct sip_txn_env *sip_txn_env_of(struct proto_timer_env *base)
{
	return (struct sip_txn_env *)base;
}

/*
 * The base of env, by conversion to the first member rather than by member
 * access: a caller cancels an unarmed timer with no environment at all, and
 * proto_timer_cancel() reads none then.
 */
static inline struct proto_timer_env *sip_txn_env_base(struct sip_txn_env *env)
{
	return (struct proto_timer_env *)env;
}

static inline void sip_timer_init(struct sip_timer *t, void *owner,
				  enum sip_timer_id id, sip_timer_cb cb)
{
	proto_timer_init(t, owner, (unsigned int)id, cb);
}

static inline void sip_timer_arm(struct sip_txn_env *env, struct sip_timer *t,
				 unsigned int ms)
{
	proto_timer_arm(sip_txn_env_base(env), t, ms);
}

static inline void sip_timer_cancel(struct sip_txn_env *env,
				    struct sip_timer *t)
{
	proto_timer_cancel(sip_txn_env_base(env), t);
}

/* the environment calls this when an armed timer expires */
static inline void sip_timer_expire(struct sip_timer *t)
{
	proto_timer_expire(t);
}

/*
 * Format one trace line of 240 bytes and give it to env->base.trace. prefix_fmt
 * writes the prefix of the line, and fmt with ap writes the message after it.
 * A prefix that does not fit drops the line. A message that does not fit is
 * cut at the end of the line. The caller tests env->base.trace first.
 */
void sip_txn_env_trace(struct sip_txn_env *env, const char *fmt, va_list ap,
		       const char *prefix_fmt, ...)
	__attribute__((format(printf, 2, 0), format(printf, 4, 5)));

/* ----------------------------------------------------------- transactions */

enum sip_txn_kind {
	SIP_TXN_ICT = 0,	/* INVITE client */
	SIP_TXN_NICT,		/* non-INVITE client */
	SIP_TXN_IST,		/* INVITE server */
	SIP_TXN_NIST,		/* non-INVITE server */
};

enum sip_txn_state {
	SIP_TXN_S_CALLING = 0,	/* ICT only */
	SIP_TXN_S_TRYING,	/* NICT, NIST */
	SIP_TXN_S_PROCEEDING,
	SIP_TXN_S_COMPLETED,
	SIP_TXN_S_CONFIRMED,	/* IST only */
	SIP_TXN_S_TERMINATED,
};

const char *sip_txn_kind_name(enum sip_txn_kind kind);
const char *sip_txn_state_name(enum sip_txn_state state);

struct sip_txn_mgr;

struct sip_txn {
	struct avl_node node;
	struct sip_txn_mgr *mgr;

	struct sip_timer retx;		/* A, E, G */
	struct sip_timer life;		/* B, D, F, K, H, I, J */

	struct sip_txn_dst dst;

	char key[SIP_TXN_KEY_MAX];
	char branch[SIP_BRANCH_MAX];

	char *req;
	size_t req_len;
	char *rsp;
	size_t rsp_len;

	uint32_t owner;
	uint32_t code;			/* last final code sent or received */
	uint32_t retx_ms;		/* current A, E or G interval */
	unsigned int tx_count;		/* sends of the retained message */

	unsigned int index;
	uint16_t gen;
	uint8_t kind;			/* enum sip_txn_kind */
	uint8_t state;			/* enum sip_txn_state */
	uint8_t method;			/* enum sip_method */
	bool client;
	bool in_use;
	bool indexed;
	bool detached;			/* owner released, upcalls suppressed */
};

struct sip_txn_stat {
	uint64_t created;
	uint64_t absorbed;		/* retransmissions the layer answered */
	uint64_t delivered;		/* responses passed to the TU */
	uint64_t timeout;		/* timer B, F or H */
	uint64_t transport;		/* send failures */
	uint64_t stray;			/* responses with no transaction */
	uint64_t denied;		/* pool exhausted */
};

struct sip_txn_mgr {
	struct sip_txn_env *env;
	struct sip_timer_cfg tc;
	struct avl_tree client;
	struct avl_tree server;
	struct sip_txn pool[SIP_TXN_MAX];
	unsigned int active;
	uint32_t rand_state;
	uint32_t branch_seq;
	struct sip_txn_stat stat;
};

void sip_txn_mgr_init(struct sip_txn_mgr *mgr, struct sip_txn_env *env,
		      const struct sip_timer_cfg *tc);
void sip_txn_mgr_done(struct sip_txn_mgr *mgr);
void sip_txn_mgr_timers_set(struct sip_txn_mgr *mgr, const struct sip_timer_cfg *tc);

/* The values in use after the fixup, for a transaction user that derives its
 * own bound from T1. */
const struct sip_timer_cfg *sip_txn_mgr_timers_get(
	const struct sip_txn_mgr *mgr);

uint32_t sip_txn_handle(const struct sip_txn *txn);
struct sip_txn *sip_txn_get(struct sip_txn_mgr *mgr, uint32_t handle);
const char *sip_txn_branch(const struct sip_txn *txn);
const struct sip_txn_dst *sip_txn_dst_get(const struct sip_txn *txn);
unsigned int sip_txn_count(const struct sip_txn_mgr *mgr);

uint32_t sip_txn_rand(struct sip_txn_mgr *mgr);

/* out must hold SIP_BRANCH_MAX bytes; a smaller buffer is refused, never
 * truncated, because a truncated branch would alias two transactions. */
bool sip_branch_new(struct sip_txn_mgr *mgr, char *out, size_t olen);

/*
 * A field that goes into a match key must not carry an embedded NUL. The
 * message parser is deliberately tolerant and keeps one (shared/msg.h
 * "Tolerance policy"), but a key comparator stops at it, so "branchA\0x" and
 * "branchA\0y" would name one transaction. Nothing in RFC 3261 puts a NUL in
 * a token, a quoted string or a host, so a field that carries one is refused
 * here and the message gets no transaction. ua.c applies the same rule to the
 * identifiers it stores.
 */
bool sip_str_nul_free(struct sip_str s);

/*
 * txn_uloop.c. Fills base.now_ms, base.timer_set and base.timer_cancel with the
 * uloop implementation (proto_timer_env_uloop_init) and leaves the transport
 * and the upcalls to the caller. It lives in its own object so sip/txn.c stays
 * free of uloop and the unit tests link without it.
 */
void sip_txn_env_uloop_init(struct sip_txn_env *env);

/*
 * Start a client transaction. branch is the top Via branch of wire, which the
 * caller generated with sip_branch_new(); the pair (branch, method) is the
 * match key, so the CANCEL of an INVITE shares the branch and still gets its
 * own transaction (RFC 3261 9.1, 17.1.3).
 *
 * The request goes on the wire before the call returns. NULL means the send
 * failed or the pool is full, and no upcall follows.
 */
struct sip_txn *sip_txn_client_start(struct sip_txn_mgr *mgr, enum sip_method method,
				     struct sip_str branch, const void *wire, size_t len,
				     const struct sip_txn_dst *dst, uint32_t owner);

/*
 * Hand the ACK for a non-2xx final to the INVITE client transaction. Valid
 * only from inside the resp() upcall that carried that final. The ACK goes out
 * at once and again on every retransmission of the final (RFC 3261 17.1.1.3).
 */
bool sip_txn_ack_set(struct sip_txn *txn, const void *wire, size_t len);

/*
 * Start a server transaction for a request sip_txn_msg_recv() reported as
 * SIP_RX_REQUEST_NEW. The request is copied and the response destination is
 * computed from the top Via and src.
 */
struct sip_txn *sip_txn_server_start(struct sip_txn_mgr *mgr, const struct sip_msg *req,
				     const struct sip_txn_dst *src, uint32_t owner);

/*
 * Send a response through a server transaction. false means the response was
 * refused (wrong state, wrong transaction) or that the transport failed, in
 * which case the transaction is already terminated and the pointer is dead.
 * A transport failure reported this way produces no done() upcall, because the
 * caller learns it from the return value.
 */
bool sip_txn_response_send(struct sip_txn *txn, uint32_t code, const void *wire, size_t len);

/*
 * Send the retained response of an INVITE server transaction again while that
 * transaction is proceeding. RFC 3262 3 makes the UAS core pass a reliable
 * provisional to the transaction layer again and again until its PRACK
 * arrives. The call returns false and sends nothing when the transaction is
 * not a proceeding INVITE server transaction. It returns false too when the
 * transport refused the send: the transaction is then terminated with
 * SIP_TXN_DONE_TRANSPORT (RFC 3261 17.2.4), as on every other send in
 * sip/txn.c, and the owner learns it through the done() upcall. The caller
 * tells the two apart by looking the handle up again.
 */
bool sip_txn_response_resend(struct sip_txn *txn);

/* reparse the retained request of a server transaction */
bool sip_txn_request_get(struct sip_txn *txn, struct sip_msg *out);

void sip_txn_abort(struct sip_txn *txn);

/*
 * Remove a transaction without a done() upcall, for a caller that already
 * learned the failure from a return value. The automatic responses in ua.c
 * use it: a request whose answer cannot be built must not leave a server
 * transaction sitting in the pool, because a server transaction that never
 * answered holds no timer to free it.
 */
void sip_txn_discard(struct sip_txn *txn);

/*
 * Detach every transaction of an owner that no longer exists. The
 * transactions keep running to their protocol end and stop calling back.
 * Returns how many were detached.
 */
unsigned int sip_txn_owner_release(struct sip_txn_mgr *mgr, uint32_t owner);

enum sip_txn_rx {
	SIP_RX_IGNORED = 0,	/* unusable for matching: no Via, no CSeq */
	SIP_RX_ABSORBED,	/* a transaction handled it, the TU sees nothing */
	SIP_RX_DELIVERED,	/* passed to the TU through resp() */
	SIP_RX_REQUEST_NEW,	/* no server transaction: the UA must create one */
	SIP_RX_ACK_2XX,		/* ACK that belongs to a 2xx: the dialog layer */
	SIP_RX_RESPONSE_STRAY,	/* response with no client transaction */
};

const char *sip_txn_rx_name(enum sip_txn_rx rx);

/*
 * The single entry point from the transport. Matching needs the message only:
 * the response destination of a server transaction is fixed when it starts,
 * so the source address of a retransmission never moves it.
 *
 * out, when not NULL, receives the matched transaction for SIP_RX_ABSORBED and
 * SIP_RX_DELIVERED, and NULL otherwise, including the case where the match
 * terminated the transaction. It is valid only until the caller returns to the
 * loop.
 */
enum sip_txn_rx sip_txn_msg_recv(struct sip_txn_mgr *mgr, const struct sip_msg *msg,
				 struct sip_txn **out);

/*
 * The INVITE server transaction a CANCEL targets (RFC 3261 9.2): same branch
 * and same sent-by, method INVITE. 0 when there is none, which is the 481
 * case. The CANCEL itself is a separate server transaction for its own 200.
 */
uint32_t sip_txn_cancel_target(struct sip_txn_mgr *mgr, const struct sip_msg *cancel);

#endif
