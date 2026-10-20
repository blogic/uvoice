// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * UA core duties that sit outside the transaction layer (src/proto/sip/ua.c):
 *
 *   1. Retransmission of a 2xx to an INVITE until the ACK arrives
 *      (RFC 3261 13.3.1.4), and absorption of retransmitted ACKs and INVITEs
 *      while the accepted state lasts (RFC 6026). The INVITE server
 *      transaction terminates the moment the 2xx goes out, so nothing below
 *      it can do this.
 *   2. The 491 glare retry timer (RFC 3261 14.1).
 *   3. Retry-After from a 503, reported to the owner (RFC 3261 21.5.4).
 *   4. The inbound request policy for methods uvoice does not serve:
 *      REGISTER 403, unknown or unsupported method 405 with Allow, OPTIONS 200
 *      with Allow, and Require with an option tag we do not implement, 420
 *      with Unsupported. An option tag list too long to enumerate is answered
 *      400 rather than a 420 that lists only part of it.
 *   5. The CANCEL pair: 200 to the CANCEL and 487 to the INVITE it targets
 *      (RFC 3261 9.2).
 *   6. The automatic 100 Trying an INVITE server transaction owes 200 ms after
 *      it starts (RFC 3261 17.2.1). It needs a response builder and the
 *      request, which is why it lives here and not in sip/txn.c. It is sent
 *      only when the call layer has produced no response of its own by then, so
 *      a fast 180 or 486 keeps the 100 off the wire.
 *
 * Receive order in the daemon:
 *
 *   sip_txn_msg_recv()
 *     SIP_RX_ABSORBED, SIP_RX_DELIVERED  -> done
 *     SIP_RX_ACK_2XX, SIP_RX_REQUEST_NEW -> ua_msg_recv()
 *       UA_RX_ABSORBED                   -> done
 *       UA_RX_PASS                       -> sip_txn_server_start(),
 *                                           ua_request_policy(), then the call
 *                                           layer
 *
 * This layer uses the same struct sip_txn_env as the transaction layer, so a
 * unit test drives both with one virtual clock.
 */
#ifndef UVOICE_SIP_UA_H
#define UVOICE_SIP_UA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/shared/hdr.h"
#include "proto/sip/txn.h"

#define UA_ACCEPT_MAX		8
#define UA_GLARE_MAX		8
#define UA_TRYING_MAX		8
#define UA_TAG_MAX		24

/* RFC 3261 17.2.1: the 100 is owed 200 ms after the INVITE arrives */
#define UA_TRYING_MS		200

/*
 * The response buffer holds a status line, the copied Via chain and, for a
 * 420, the whole Unsupported list. One Require header value can reach
 * SIP_HDR_LEN_MAX before the parser even flags it, so 8 KB keeps a legitimate
 * 420 buildable instead of turning it into a refusal.
 */
#define UA_BUF_MAX		8192

/*
 * The served method set. No UPDATE, no SUBSCRIBE, and that last one is an
 * inbound statement: uvoice serves no subscription, so a SUBSCRIBE it
 * receives is answered 405 from this list. It sends one as a client for the
 * message waiting package (sip/sub.h), which needs no entry here, because
 * Allow names the methods a peer may send us.
 *
 * REFER is in the list because call.c serves an in-dialog REFER from the
 * transferee; a list that names a method nobody serves would swallow a
 * request this daemon can do nothing with. The three lines below move
 * together, because this header's rule is that allow_events rides wherever
 * allow rides: the NOTIFY of the implicit subscription a REFER creates carries
 * the refer package, and its body is the message/sipfrag of RFC 3515 2.4.5.
 *
 * "replaces" is in Supported under the same rule: call.c completes an
 * attended transfer at the target, and the option tag is what tells a
 * transferee it may send the Replaces at all. It is also what admits the
 * INVITE that carries the header: RFC 3891 4 puts Require: replaces beside
 * it, and require_check() below answers 420 for every tag this list does not
 * name, so a list without the tag would refuse that INVITE.
 *
 * "100rel" is in Supported and PRACK is in Allow for the reliable provisional
 * responses of RFC 3262. Every initial INVITE this daemon sends offers the tag
 * (RFC 3262 4 SHOULD), so a switch that insists on reliable provisionals
 * answers the call instead of refusing it 421. Inbound, the tag admits an
 * INVITE that carries Require: 100rel, which require_check() below would
 * otherwise answer 420. As a UAS, call.c sends its 180 reliably only when the
 * prack option of the channel is set, under either tag. With the option clear
 * a Require: 100rel takes the plain 180, the documented deviation from
 * RFC 3262 3 (call.c rel_wanted). The PRACK that acknowledges a reliable 180
 * is served by the same leg. As a UAC, call.c acknowledges every reliable 1xx
 * it receives, whatever the option holds.
 */
#define UA_ALLOW_DEFAULT	"INVITE, ACK, CANCEL, BYE, OPTIONS, INFO, " \
				"NOTIFY, REFER, PRACK"
#define UA_SUPPORTED_DEFAULT	"timer, replaces, 100rel"

/*
 * The Supported list of a re-INVITE of ours. RFC 4028 7.1 asks for the timer
 * tag in each request. It leaves out 100rel: RFC 3262 3 lets a UAS answer any
 * INVITE that lists the tag with a reliable provisional, and call.c sends no
 * PRACK inside a dialog.
 */
#define UA_SUPPORTED_REINVITE	"timer, replaces"

/*
 * The event packages a NOTIFY may carry here. It is the companion of the
 * NOTIFY entry in the Allow list: Allow says the method is served, and
 * Allow-Events says which package (RFC 6665 4.4.4, RFC 3842). Every other
 * package is answered 489 by the account layer.
 */
#define UA_ALLOW_EVENTS_DEFAULT	"message-summary, refer"

/* the message summary body rides beside the offer answer body, and the
 * sipfrag beside both (RFC 3515 2.4.5) */
#define UA_ACCEPT_DEFAULT	"application/sdp, application/simple-message-summary, message/sipfrag"

struct ua;

/*
 * Every string here must outlive the ua. A NULL field takes the default
 * above.
 *
 * allow is the served method set: ua_request_policy() answers exactly what it
 * does not name. REGISTER must not appear in it, because uvoice is not a
 * registrar and the policy answers 403; a list that names REGISTER is refused
 * at ua_init() and the default takes its place (ua.allow_conflict records it).
 *
 * allow_events rides wherever allow rides, so one answer never advertises a
 * method without the package that makes it useful.
 */
struct ua_cfg {
	const char *allow;
	const char *allow_events;
	const char *supported;
	const char *accept;
};

/*
 * ack_missing: 64*T1 of 2xx retransmission produced no ACK. RFC 3261 13.3.1.4
 * confirms the dialog and asks the TU to end the session with a BYE.
 * retry_after: a 503 arrived. present is false when the response carried no
 * Retry-After, which is the "try the next server now" case of RFC 3263.
 */
struct ua_cb {
	void (*ack_missing)(struct ua *ua, uint32_t owner, void *priv);
	void (*retry_after)(struct ua *ua, uint32_t owner, uint32_t secs, bool present,
			    void *priv);
	void (*glare_retry)(struct ua *ua, uint32_t owner, void *priv);
};

/*
 * One accepted INVITE. The identifiers are stored one allocation each and
 * compared field by field, because RFC 3261 bounds none of them: a Call-ID and
 * two tags that a fixed key buffer could not hold would otherwise cost the 2xx
 * its retransmission, which is the one thing this record exists for.
 *
 * The Via fields match a retransmitted INVITE (RFC 3261 17.2.3), the dialog
 * fields plus cseq match its ACK.
 */
struct ua_accept {
	struct sip_timer retx;
	struct sip_timer life;
	struct sip_txn_dst dst;
	struct ua *ua;
	char *wire;
	size_t len;

	char *call_id;
	char *from_tag;
	char *to_tag;
	char *branch;
	char *host;			/* Via sent-by host, case insensitive */
	uint16_t port;			/* Via sent-by port, 5060 when absent */

	uint32_t owner;
	uint32_t cseq;
	uint32_t retx_ms;
	unsigned int tx_count;
	uint64_t acked_seq;		/* order of the ACK among the records */
	bool acked;
	bool in_use;
	bool detached;
};

struct ua_glare {
	struct sip_timer timer;
	struct ua *ua;
	uint32_t owner;
	bool in_use;
	bool detached;
};

/*
 * One armed 100 Trying. The transaction is named by its handle, never by a
 * pointer, so a transaction that ended inside the 200 ms leaves nothing to
 * dereference (sip/txn.h "Lifetime").
 */
struct ua_trying {
	struct sip_timer timer;
	struct ua *ua;
	uint32_t txn;
	uint32_t owner;
	bool in_use;
};

struct ua_stat {
	uint64_t rtx_2xx;
	uint64_t ack_absorbed;
	uint64_t invite_absorbed;
	uint64_t ack_missing;
	uint64_t trying_sent;
	uint64_t trying_skipped;	/* the call layer answered inside 200 ms */
	uint64_t denied;
	uint64_t accept_reused;		/* an ACKed record gave way to a new 2xx */
};

struct ua {
	struct sip_txn_mgr *mgr;
	struct sip_txn_env *env;
	struct ua_cfg cfg;
	struct ua_cb cb;
	void *priv;

	struct ua_accept accept[UA_ACCEPT_MAX];
	uint64_t accept_acks;		/* ACKs taken, the order of acked_seq */
	struct ua_glare glare[UA_GLARE_MAX];
	struct ua_trying trying[UA_TRYING_MAX];

	char out[UA_BUF_MAX];
	uint32_t tag_seq;
	bool allow_conflict;		/* the configured Allow named REGISTER */
	struct ua_stat stat;
};

void ua_init(struct ua *ua, struct sip_txn_mgr *mgr, struct sip_txn_env *env,
	     const struct ua_cfg *cfg, const struct ua_cb *cb, void *priv);
void ua_done(struct ua *ua);

/* a To tag or a From tag; the caller owns tag generation everywhere else too */
bool ua_tag_new(struct ua *ua, char *out, size_t olen);

/* ------------------------------------------------------ response building */

/*
 * A response the UA core sends through a server transaction. Only code is
 * mandatory: reason NULL takes sip_status_reason(), to_tag NULL generates one
 * when the request carries none, and every header field below is written only
 * when it is set.
 */
struct ua_resp {
	uint32_t code;
	const char *reason;
	const char *to_tag;
	const char *contact;
	const char *allow;
	const char *allow_events;
	const char *unsupported;
	const char *accept;
	const char *supported;
	uint32_t retry_after;
	bool has_retry_after;
};

/*
 * False means the response was not built or not sent. The transaction is left
 * as it is, except that a transport error inside sip_txn_response_send() has
 * already discarded it, so a caller that keeps going must resolve its handle
 * again. ua_request_policy() takes the stricter line for the responses it
 * sends by itself: see UA_POLICY_FAILED.
 */
bool ua_response_send(struct ua *ua, struct sip_txn *txn, const struct sip_msg *req,
		      const struct ua_resp *r);

/* ------------------------------------------------------- inbound policy */

enum ua_policy {
	UA_POLICY_PASS = 0,	/* uvoice serves this request */
	UA_POLICY_ANSWERED,	/* the UA core answered it, nothing else to do */
	UA_POLICY_DROP,		/* an ACK: it is never answered */

	/*
	 * The answer could not be built or could not be sent. The server
	 * transaction is discarded, so the caller must drop its handle. A
	 * transaction that never answered holds no timer, so leaving it in the
	 * pool would let a flood of unanswerable requests fill the pool.
	 */
	UA_POLICY_FAILED,
};

const char *ua_policy_name(enum ua_policy policy);

enum ua_policy ua_request_policy(struct ua *ua, const struct sip_msg *req,
				 struct sip_txn *txn);

/*
 * RFC 3261 9.2. Answers the CANCEL with 200 when it targets a live INVITE
 * server transaction and with 481 when it does not, and sends 487 for the
 * INVITE in the first case. to_tag is the tag the UAS already used in its
 * provisional responses; NULL generates one, which is correct only when no
 * provisional went out. True means an INVITE was cancelled.
 */
bool ua_cancel_handle(struct ua *ua, const struct sip_msg *cancel,
		      struct sip_txn *cancel_txn, const char *to_tag);

/* ------------------------------------------------------ automatic 100 */

/*
 * Arm the 100 Trying of a new INVITE server transaction. Call it right after
 * sip_txn_server_start(); it does nothing for any other kind of transaction.
 *
 * The timer reads the state of the transaction when it fires: a transaction
 * that already sent any response, provisional or final, gets no 100, and one
 * that is gone gets nothing at all. A 100 carries no To tag
 * (RFC 3261 8.2.6.1), which is why it needs no tag from the caller.
 */
bool ua_trying_arm(struct ua *ua, struct sip_txn *txn);

/* Drop the armed 100 of a transaction, for a caller that answered by itself. */
void ua_trying_cancel(struct ua *ua, uint32_t txn);

/* ------------------------------------------- 2xx retransmission (13.3.1.4) */

/*
 * Take over a 2xx that a UAS just sent for an INVITE. resp is the parsed
 * response, which carries everything needed to match both the ACK (Call-ID,
 * both tags, CSeq) and a retransmitted INVITE (the copied top Via). wire is
 * the response as sent and is copied.
 */
bool ua_accept_add(struct ua *ua, const struct sip_msg *resp, const void *wire, size_t len,
		   const struct sip_txn_dst *dst, uint32_t owner);

bool ua_accept_active(struct ua *ua, uint32_t owner);
unsigned int ua_accept_count(const struct ua *ua);

enum ua_rx {
	UA_RX_PASS = 0,		/* not ours: the call layer decides */
	UA_RX_ABSORBED,		/* a retransmitted ACK or INVITE, handled here */
	UA_RX_ACK,		/* the first ACK for an accepted INVITE */
};

const char *ua_rx_name(enum ua_rx rx);

enum ua_rx ua_msg_recv(struct ua *ua, const struct sip_msg *msg);

/* ------------------------------------------------------ 491 glare, 503 */

/*
 * RFC 3261 14.1 picks the range from Call-ID ownership, not from the role in
 * the failed transaction: the side that generated the Call-ID waits 2.1 to 4.0
 * s, the other side 0 to 2.0 s, both in units of 10 ms. For uvoice the Call-ID
 * owner is the side that placed the call.
 */
uint32_t ua_glare_delay_ms(struct ua *ua, bool call_id_owner);
bool ua_glare_arm(struct ua *ua, uint32_t owner, bool call_id_owner, uint32_t *delay_ms);
void ua_glare_cancel(struct ua *ua, uint32_t owner);

bool ua_retry_after_get(const struct sip_msg *resp, uint32_t *secs);
bool ua_503_handle(struct ua *ua, uint32_t owner, const struct sip_msg *resp);

/*
 * Detach every record of an owner that went away. The 2xx retransmission runs
 * to its RFC 6026 end so the peer still sees a complete answer, and the
 * callbacks stop.
 */
unsigned int ua_owner_release(struct ua *ua, uint32_t owner);

#endif
