// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * INVITE dialogs, UAC and UAS (RFC 3261 12, RFC 6026).
 *
 * Dependencies. The message and header layer plus the transaction layer
 * environment, for the one send this layer owns by itself: the ACK to a 2xx.
 * No uloop, no socket, no timer of its own.
 *
 * What this layer owns, and why it has to.
 *
 *   The INVITE client transaction ends the moment the 2xx arrives
 *   (RFC 3261 17.1.1.2), so nothing below this layer can answer a
 *   retransmitted 2xx. RFC 6026 gives that duty to the transaction user:
 *   sip_dialog_resp_recv() answers a repeated 2xx with the same ACK bytes,
 *   which is why sip_dialog_ack_send() retains them.
 *
 *   A 200 whose To tag differs from the tag of an earlier 18x is a different
 *   dialog. The early one is discarded and the new one adopted. A 200 with no
 *   To tag at all is tolerated: it confirms the early dialog when there is one
 *   and otherwise builds a dialog with no remote tag.
 *
 * Route set. Record-Route of the response, reversed, for a UAC
 * (RFC 3261 12.1.2); Record-Route of the request, in order, for a UAS
 * (12.1.1). Both forms are stored as the received name-addr text, so they go
 * back out unchanged. A first hop without ;lr is a strict router: the request
 * URI becomes that hop and the remote target moves to the end of the Route
 * list (12.2.1.1).
 *
 * Lifetime. Fixed pool, the sip/txn.h handle rule ((index << 16) | generation)
 * and a reference count. A transaction that carries a dialog holds a strong
 * reference, and a dialog holds only transaction handles, so the memory
 * outlives an owner that vanished, and nothing here ever points back at a
 * transaction.
 */
#ifndef UVOICE_SIP_DIALOG_H
#define UVOICE_SIP_DIALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/shared/hdr.h"
#include "proto/sip/txn.h"

/* two lines, call waiting and a conference leg, plus room to drain */
#define SIP_DIALOG_MAX		8

enum sip_dialog_state {
	SIP_DLG_EARLY = 0,
	SIP_DLG_CONFIRMED,
	SIP_DLG_TERMINATED,
};

const char *sip_dialog_state_name(enum sip_dialog_state state);

/*
 * Our own side of the wire. Every string must outlive the dialog; the dialog
 * copies none of them, exactly like struct sip_reg_cfg.
 *
 * host, port  the Via sent-by of the requests this dialog sends.
 * contact     our Contact URI, written into the target refresh requests
 *             (INVITE and UPDATE). NULL omits the header.
 */
struct sip_dialog_local {
	const char *host;
	const char *transport;		/* NULL takes UDP */
	const char *contact;
	const char *user_agent;
	uint16_t port;
};

struct sip_dialog_set;

struct sip_dialog {
	struct sip_dialog_set *set;
	struct sip_dialog_local local;

	char *call_id;
	char *local_tag;
	char *remote_tag;		/* NULL when the peer sent no tag */
	char *local_uri;
	char *remote_uri;
	char *remote_target;		/* the peer Contact URI */
	char *route[SIP_ROUTE_MAX];
	unsigned int routes;
	bool route_overflow;

	struct sip_txn_dst dst;
	bool dst_set;

	/* RFC 6026: the ACK of the 2xx, kept to answer a retransmitted 2xx */
	char *ack;
	size_t ack_len;
	uint32_t ack_cseq;

	uint32_t local_cseq;
	uint32_t remote_cseq;
	bool remote_cseq_set;

	unsigned int index;
	uint16_t gen;
	uint16_t refs;
	uint8_t state;
	bool uac;
	bool in_use;
};

struct sip_dialog_stat {
	uint64_t created;
	uint64_t confirmed;
	uint64_t replaced;		/* early dialog dropped for a new To tag */
	uint64_t reack;			/* retransmitted 2xx answered with the ACK */
	uint64_t absorbed;		/* retransmitted 2xx with no ACK to repeat */
	uint64_t denied;		/* pool full or unusable response */
	uint64_t terminated;
};

struct sip_dialog_set {
	struct sip_txn_env *env;
	struct sip_dialog pool[SIP_DIALOG_MAX];
	unsigned int active;
	struct sip_dialog_stat stat;
};

void sip_dialog_set_init(struct sip_dialog_set *set, struct sip_txn_env *env);
void sip_dialog_set_done(struct sip_dialog_set *set);
unsigned int sip_dialog_count(const struct sip_dialog_set *set);

uint32_t sip_dialog_handle(const struct sip_dialog *d);
struct sip_dialog *sip_dialog_get(struct sip_dialog_set *set, uint32_t handle);

/*
 * The creator holds the first reference. Every transaction that carries the
 * dialog takes one more and drops it when it ends, so a dialog whose owner
 * disappeared still lives until the last transaction is done.
 */
void sip_dialog_hold(struct sip_dialog *d);
void sip_dialog_put(struct sip_dialog *d);

/*
 * Leave the dialog: it stops matching new requests and responses at once, and
 * the memory goes back to the pool when the last reference is dropped.
 */
void sip_dialog_terminate(struct sip_dialog *d);

enum sip_dialog_state sip_dialog_state(const struct sip_dialog *d);
const struct sip_txn_dst *sip_dialog_dst(const struct sip_dialog *d);
void sip_dialog_dst_set(struct sip_dialog *d, const struct sip_txn_dst *dst);

/*
 * A response to an INVITE we sent. 101 to 199 with a To tag creates or
 * updates an early dialog, 2xx creates or confirms one, and everything else
 * returns NULL. dst is the next hop the INVITE went to and may be NULL when
 * the dialog already has one.
 *
 * A 2xx whose To tag differs from an early dialog returns a different dialog
 * and leaves the early one terminated. The caller compares the handle it gets
 * with the one it held and drops its reference to the old dialog; this layer
 * never drops a reference it did not take.
 */
struct sip_dialog *sip_dialog_uac_update(struct sip_dialog_set *set, const struct sip_msg *resp,
					 const struct sip_txn_dst *dst,
					 const struct sip_dialog_local *local);

/*
 * An INVITE we accept. local_tag is the tag this side puts in its responses,
 * so the dialog and the 18x or 2xx agree on it. The dialog starts early and
 * sip_dialog_confirm() moves it on when the ACK arrives.
 */
struct sip_dialog *sip_dialog_uas_create(struct sip_dialog_set *set, const struct sip_msg *req,
					 const char *local_tag, const struct sip_txn_dst *dst,
					 const struct sip_dialog_local *local);

/*
 * Both creators refuse a Record-Route chain longer than SIP_ROUTE_MAX. A
 * truncated route set routes every later request past proxies the peer put in
 * the chain, and a BYE that never arrives leaves the call up on the far side.
 */

void sip_dialog_confirm(struct sip_dialog *d);

/* an in-dialog request: Call-ID, our tag in To, the peer tag in From */
struct sip_dialog *sip_dialog_find(struct sip_dialog_set *set, const struct sip_msg *req);

enum sip_dialog_cseq {
	SIP_DLG_CSEQ_OK = 0,
	SIP_DLG_CSEQ_OLD,	/* below the highest we saw: RFC 3261 12.2.2, 500 */
	SIP_DLG_CSEQ_BAD,	/* no CSeq at all */
};

/*
 * Check and record the remote sequence number of an in-dialog request. ACK and
 * CANCEL carry the number of the request they belong to, so they are checked
 * and never recorded. A request the check refuses changes nothing.
 *
 * The check does not move the remote target. Passing it is not acceptance: a
 * re-INVITE can still be answered 488 or 491, and RFC 6141 keeps the target of
 * a target refresh request that was not answered 2xx. The caller applies the
 * Contact with sip_dialog_target_refresh() once it decides to accept.
 */
enum sip_dialog_cseq sip_dialog_remote_cseq_take(struct sip_dialog *d,
						 const struct sip_msg *req);

/*
 * Take the Contact of msg as the new remote target (RFC 3261 12.2.2). msg is
 * either an in-dialog request this side accepts or a response that carries a
 * moved Contact, so both directions of a target refresh use one entry point.
 * False means there was no usable Contact and the target did not move.
 */
bool sip_dialog_target_refresh(struct sip_dialog *d, const struct sip_msg *msg);

enum sip_dialog_rx {
	SIP_DLG_RX_PASS = 0,	/* not a response this layer owns */
	SIP_DLG_RX_ABSORBED,	/* a retransmitted 2xx, answered here */
};

/*
 * The RFC 6026 duty outlives the dialog: a terminated dialog still answers a
 * retransmitted 2xx with its stored ACK, for as long as a reference keeps the
 * record alive. A peer that missed the ACK retransmits for 64*T1, and the
 * owner that already left the call must not turn that into a lost ACK.
 */

/*
 * A response the transaction layer could not place (SIP_RX_RESPONSE_STRAY). A
 * retransmitted 2xx of a dialog we ACKed is answered with the same ACK bytes
 * and absorbed; everything else passes, including a 2xx from a second fork,
 * which the call layer turns into its own dialog.
 */
enum sip_dialog_rx sip_dialog_resp_recv(struct sip_dialog_set *set, const struct sip_msg *resp);

/*
 * Build an in-dialog request. cseq is an out parameter: BYE, INVITE, INFO and
 * every other request take the next local number, and the caller needs it to
 * match the answer.
 */
struct sip_dialog_req {
	enum sip_method method;
	const char *method_name;	/* NULL takes the name of method */
	const char *branch;		/* from sip_branch_new() */
	const char *ctype;
	const char *body;
	size_t body_len;
	bool with_contact;		/* target refresh, and REFER and NOTIFY,
					 * whose Contact is mandatory (RFC 3515
					 * 2.4, RFC 6665 8.6) */
	uint32_t cseq;			/* out */
};

bool sip_dialog_req_build(struct sip_dialog *d, struct sip_dialog_req *r, struct sip_buf *b);

/*
 * Build, send and retain the ACK for a 2xx (RFC 3261 13.2.2.4). branch is a
 * fresh branch: the ACK to a 2xx is its own transaction. The retained bytes
 * answer every retransmission of that 2xx.
 *
 * Either orientation. The creator of the dialog decides nothing here: a
 * re-INVITE this side places inside a dialog the peer created owes the same
 * ACK, with our identity in the From and the peer in the To, down the same
 * route set as every other in-dialog request.
 */
bool sip_dialog_ack_send(struct sip_dialog *d, const struct sip_msg *resp, const char *branch,
			 const char *ctype, const char *body, size_t body_len);

bool sip_dialog_ack_stored(const struct sip_dialog *d);

#endif
