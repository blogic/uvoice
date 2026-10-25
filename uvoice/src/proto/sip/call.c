// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Call legs. See proto/sip/call.h for the contracts.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubox/ulog.h>

#include "posix.h"
#include "proto/proto.h"
#include "proto/shared/hdr.h"
#include "proto/shared/media_fill.h"
#include "proto/shared/sdp.h"
#include "proto/sip/account.h"
#include "proto/sip/auth.h"
#include "proto/sip/call.h"
#include "proto/sip/dialog.h"
#include "proto/sip/sub.h"
#include "proto/sip/ua.h"
#include "str.h"

#define CALL_WIRE_MAX		4096
#define CALL_BODY_MAX		1024
#define CALL_URI_MAX		320
#define CALL_HOST_MAX		128
#define CALL_USER_MAX		96
/* the widest escaped userinfo: every byte of a dialled or configured number
 * may become %XX. CFG_STR_MAX equals CALL_NUMBER_MAX, so the one bound covers
 * the dialled number of call_dial() and the bare target of
 * target_uri_build() alike. */
#define CALL_USER_ESC_MAX	(3 * (CALL_NUMBER_MAX - 1) + 1)
#define CALL_PASS_MAX		128
#define CALL_ID_MAX		192
#define CALL_IFACE_MAX		16
/* the Via spelling of the transport, "TCP" or "TLS" (account.h) */
#define CALL_TRANSPORT_MAX	4

/* RFC 3261 16.8 names timer C for a proxy; here it bounds one INVITE setup. */
#define CALL_TIMER_C_DEFAULT	180000

/* RFC 4028 4: the smallest session interval any side may demand. */
#define CALL_SESS_MIN_DEFAULT	90
/* RFC 4028 10: the peer refresh watchdog fires this many seconds early. */
#define CALL_SESS_LEAD_MAX	32
/*
 * The largest session interval that reaches a timer, one day, the same ceiling
 * reg.c puts on a Retry-After. A peer is free to name a number that overflows
 * seconds times 1000, and the wrapped result would arm a refresh seconds away
 * instead of days.
 */
#define CALL_SESS_MAX		86400

/*
 * What a caller is told to wait before it offers the call again (RFC 3261
 * 21.5.4). It is not the length of the bring-up window, which is milliseconds
 * on both backends. It is the cadence a switch gets for a DSP that failed and
 * comes back only when an operator repairs it. Five seconds re-offers a call
 * the window swallowed almost at once, and leaves a dead line nearly alone.
 */
#define CALL_HAL_RETRY_AFTER_S	5

/*
 * The RFC 3323 anonymous CPE identity. The Privacy value is the
 * RFC 3323 token: uvoice asserts no P-Asserted-Identity, because it has no
 * trust domain to assert one into.
 */
#define CALL_ANON_URI		"sip:anonymous@anonymous.invalid"
#define CALL_ANON_DISPLAY	"Anonymous"
#define CALL_PRIVACY_ID		"id"

/*
 * The widest Refer-To URI, term by term as call.h derives CALL_WIRE_EXTRA_MAX:
 * the target URI, the "?Replaces=" of a consult transfer, and the escaped
 * Replaces value behind it, whose every byte may become %XX.
 *
 * CALL_REPLACES_MAX is the raw value: a Call-ID, ";to-tag=" and a tag,
 * ";from-tag=" and a tag, which is 255 bytes plus the NUL.
 */
#define CALL_REPLACES_MAX	(CALL_ID_MAX + 8 + UA_TAG_MAX + 10 + UA_TAG_MAX - 2)
#define CALL_REFER_URI_MAX	(CALL_URI_MAX + 10 + 3 * (CALL_REPLACES_MAX - 1) + 1)

/* the two headers built out of it: "Refer-To: <" ">" CRLF around the URI above
 * and "Referred-By: <" ">" CRLF around our own identity */
#define CALL_REFER_HDRS_MAX	(11 + CALL_REFER_URI_MAX + 3 + 14 + CALL_URI_MAX + 3)

/*
 * The header block that the child INVITE of a transferee carries: the
 * Replaces promoted out of the Refer-To URI headers, the Require that RFC 3891
 * 4 puts beside it, and the Referred-By of RFC 3892 copied verbatim.
 *
 * The leg stores the block instead of building it at each send, because the
 * credential retry builds the request again. A retry that lost the
 * Replaces would answer the referral with a plain call to the same target,
 * which is a blind transfer where an attended one was asked for.
 *
 * The Referred-By value is bounded on its own. It is a name-addr the far end
 * wrote, so it carries a display name and header parameters this side has no
 * budget for; the width below is a URI plus that much again.
 */
#define CALL_REFERRED_BY_MAX	384
#define CALL_XFER_HDRS_MAX	(10 + CALL_REPLACES_MAX + 2 + 19 + \
				 13 + CALL_REFERRED_BY_MAX + 2)

/* the header block of a PRACK: "RAck: 4294967295 4294967295 INVITE\r\n" is 36
 * bytes, and the widest value of each number is already in that count */
#define CALL_RACK_HDRS_MAX	48

_Static_assert(CALL_REPLACES_MAX == 256, "the Replaces budget of call.h moved");
_Static_assert(CALL_REFER_HDRS_MAX < CALL_WIRE_EXTRA_MAX,
	       "the REFER headers must fit the in-dialog extra buffer of call.h");
/*
 * The three variable blocks of an INVITE, together, inside the wire buffer:
 * the service route of RFC 3608, the Replaces block of a transfer's child and
 * the body. The fixed headers and the two credential headers are not in the
 * count. This is therefore no proof that every INVITE fits. An overflow costs
 * no memory and no truncated request: sip_buf keeps the overflow sticky, and
 * invite_build() returns false before anything is sent.
 */
_Static_assert(SIP_ROUTE_HDRS_MAX + CALL_XFER_HDRS_MAX + CALL_BODY_MAX <
	       CALL_WIRE_MAX,
	       "the service route, the Replaces block and the body together "
	       "must fit CALL_WIRE_MAX");

#define CALL_GLARE_MAX		2	/* 491 retries per exchange */
#define CALL_MIN_SE_MAX		1	/* 422 retries per exchange */

/* the account interface and the pure helpers are declared in call.h; nothing
 * here is file-local by design except the pools */

/* ------------------------------------------------------------------ state */

enum call_state {
	CALL_ST_FREE = 0,
	CALL_ST_INVITE_SENT,		/* our INVITE is out, nothing back */
	CALL_ST_PROCEEDING,		/* an 18x arrived */
	CALL_ST_CANCEL_SENT,
	CALL_ST_RINGING_IN,		/* inbound INVITE, before our 2xx */
	CALL_ST_ACCEPT_WAIT_ACK,	/* our 2xx is out, waiting for the ACK */
	CALL_ST_ACTIVE,
	CALL_ST_REINVITE_SENT,		/* our re-INVITE is out */
	CALL_ST_REINVITE_WAIT_ACK,	/* a peer re-INVITE we answered 2xx */
	CALL_ST_BYE_SENT,		/* UI gone, the BYE still running */
};

/* what our own re-INVITE is for */
enum call_op {
	CALL_OP_NONE = 0,
	CALL_OP_HOLD,
	CALL_OP_RESUME,
	CALL_OP_REFRESH,		/* RFC 4028 session refresh, not an FSM op */
	CALL_OP_MIN_SE,			/* the 422 retry of the initial INVITE */
	CALL_OP_FAX,			/* the G.711 upspeed, not an FSM op either */
	CALL_OP_T38,			/* the T.38 upgrade, not an FSM op either */
	/*
	 * The hold a transferee's child re-offers because the leg it replaced
	 * was held. It is ours and not the FSM's for the same reason the
	 * refresh is: the machine already believes this UI slot is held, and
	 * the exchange only makes the wire agree with it.
	 */
	CALL_OP_XFER_HOLD,
};

/*
 * What a leg is to the UI (call.h, the two-name contract). Every leg carries
 * a UI slot; this says how it may use it.
 *
 * OWNER is every leg on an untransferred line, and it is the whole of the
 * suppression invariant: a leg emits to the FSM only while it owns the name it
 * would emit under.
 *
 * QUIET is the parent of a transfer this line took, from the moment the child
 * INVITE reaches the wire. It still holds the name, so an FSM command still
 * finds it and the subscriber can still hang up; it says nothing, because the
 * child is about to answer under that name and the machine must not learn that
 * the legs changed under it. A transferor may BYE a blindly transferred leg
 * right behind our 202, while the child is still ringing, and that
 * BYE would otherwise tear the line to IDLE and orphan the child.
 *
 * NONE is a leg that holds no name at all: the child before it takes the name
 * over, and the parent once it has.
 */
enum call_ui_role {
	CALL_UI_OWNER = 0,
	CALL_UI_QUIET,
	CALL_UI_NONE,
};

struct call {
	unsigned int line;
	unsigned int slot;		/* physical: the stream, the record, the handle */
	uint16_t gen;

	bool in_use;			/* protocol occupancy */
	bool ui_busy;			/* UI occupancy, call_slot_free reads this */
	bool uac;
	uint8_t state;
	uint8_t op;			/* enum call_op */
	/*
	 * The name the FSM knows this leg by (call.h, the two-name contract).
	 * It is the physical slot until a transfer moves it, and it is what
	 * every callback but the two record ones carries.
	 */
	uint8_t ui_slot;
	uint8_t ui_role;		/* enum call_ui_role */

	/* injected per account objects, valid until calls_line_teardown */
	struct sip_txn_mgr *mgr;
	struct sip_txn_env *env;
	struct ua *ua;
	struct sip_dialog_set *dialogs;

	uint32_t dialog;
	uint32_t invite_txn;		/* client INVITE (UAC) */
	uint32_t server_txn;		/* server INVITE (UAS) */
	uint32_t reinvite_txn;
	uint32_t bye_txn;
	uint32_t cancel_txn;
	uint32_t flash_txn;		/* the hook flash INFO, answer absorbed */
	uint32_t refer_txn;		/* the transfer REFER, the flash_txn pattern */
	uint32_t notify_txn;		/* the NOTIFY we owe a served referral */

	struct sdp_state sdp;
	struct sdp_local loc;
	struct sdp_msg offer;		/* the inbound offer, self contained */
	bool offer_valid;

	struct sip_auth_state auth[SIP_AUTH_KIND_MAX];
	unsigned int auth_retry[SIP_AUTH_KIND_MAX];	/* the INVITE exchange */
	unsigned int dlg_auth_retry[SIP_AUTH_KIND_MAX];	/* the in-dialog one */

	struct sip_timer t_session;
	struct sip_timer t_watchdog;
	struct sip_timer t_c;
	/*
	 * The transfer guard (call.h, CALL_XFER_WAIT_MS). The REFER transaction
	 * ends at the 202, so nothing below this layer bounds the NOTIFY that
	 * carries the outcome. It takes the timer id of the setup bound it
	 * inherits its value from, because call.h states that this wait is
	 * timer C and the two are never armed on one leg: t_c is cancelled
	 * before a leg is confirmed enough to carry a REFER.
	 */
	struct sip_timer t_xfer;
	/*
	 * The BYE guard of a leg another leg is meant to replace, in two roles
	 * that never overlap on one leg (call.h, CALL_XFER_BYE_MS and
	 * CALL_XFER_DRAIN_MS). On a transferee it is the parent, armed at the
	 * swap, and it BYEs the parent a transferor never ended. On a consult
	 * transferor it is the replaced leg, armed at the 202, and it BYEs a
	 * replaced leg the network never sent its Replaces-driven BYE for. Both
	 * keep two legs from living on one line for good. It takes the timer id
	 * of the setup bound for the reason t_xfer does: a leg being replaced
	 * runs no setup and no transfer of its own.
	 */
	struct sip_timer t_bye;

	uint32_t sess_interval;		/* seconds, 0 = no session timer */
	uint32_t sess_want;		/* what our INVITE asks for, seconds */
	uint32_t sess_min_se;		/* our floor, seconds */
	bool we_refresh;

	unsigned int glare_run;
	unsigned int min_se_run;

	uint32_t cseq;			/* our CSeq before a dialog exists */
	uint32_t exch_cseq;		/* the request our 2xx answered */
	uint32_t dlg_cseq;		/* the last in-dialog request we built */
	struct sip_txn_dst dst;
	struct sip_txn_dst src;		/* where an inbound request came from */
	/*
	 * The service route of RFC 3608, rendered as Route headers and copied
	 * out of the call view at leg start, beside dst. It is a copy and not
	 * the view's pointer, for the leg lifetime rule below. A registration
	 * refresh replaces the block, and a lost binding empties it. The 401,
	 * the 422 and the 491 retry of one INVITE have to reach the proxy the
	 * first one named. The CANCEL and the ACK for a non-2xx need no reader
	 * here: req_mirror_build() copies the Route headers of the retained
	 * request.
	 */
	struct sip_route_hdrs service_route;

	struct cfg_channel ch;		/* leg start snapshot, cfg lifetime rule */
	bool rtcp_enabled;
	unsigned int rtcp_interval;
	char alert_prefix[CFG_STR_MAX];

	char local_host[CALL_HOST_MAX];
	char domain[CALL_HOST_MAX];
	char user[CALL_USER_MAX];
	char display[CALL_USER_MAX];
	char auth_user[CALL_USER_MAX];
	char auth_pass[CALL_PASS_MAX];
	char bind_dev[CALL_IFACE_MAX];
	char user_agent[CFG_STR_MAX];
	/* the account's transport token, empty for UDP (account.h) */
	char transport[CALL_TRANSPORT_MAX];
	uint16_t local_port;
	uint16_t punch_interval_ms;	/* the view's, snapshot with bind_dev */

	char from_uri[CALL_URI_MAX];
	char to_uri[CALL_URI_MAX];
	char ruri[CALL_URI_MAX];
	char contact[CALL_URI_MAX];
	char call_id[CALL_ID_MAX];
	char local_tag[UA_TAG_MAX];
	char branch[SIP_BRANCH_MAX];

	char number[CALL_NUMBER_MAX];
	char cid_number[CALL_CID_MAX];
	char cid_name[CALL_CID_MAX];
	bool cid_number_private;	/* inbound: the caller withheld the number */
	bool cid_name_private;
	bool cid_present;		/* outbound: present our own number */
	unsigned int ring_id;

	struct sip_dialog_local dlocal;

	char wire[CALL_WIRE_MAX];	/* scratch: one build, one send */
	size_t wire_len;
	char body[CALL_BODY_MAX];
	size_t body_len;

	uint16_t rtp_port;
	struct hal_media media;
	bool media_started;
	bool media_ran;			/* a stream was started at least once */
	bool media_set;
	bool early_answered;		/* an 18x answer already programmed media */
	bool hold_prev;			/* local hold before the pending re-INVITE */
	/*
	 * The T.38 enable before the pending re-INVITE, restored wherever
	 * hold_prev is restored. It is snapshotted for every exchange and not
	 * for the upgrade alone: a hold, a resume or a refresh made
	 * while the session already carries T.38 has to put the flag back the
	 * way it found it, and a 491 that re-offered audio there would drop the
	 * image session the peer is still running.
	 */
	bool t38_prev;
	/*
	 * An operation of ours that a 491 held back, waiting for its glare
	 * timer (RFC 3261 14.1). The leg stays in CALL_ST_ACTIVE for the whole
	 * wait, because no INVITE of ours is in progress then and RFC 3261 14.2
	 * asks for the 491 answer only while one is: a peer that retries inside
	 * our own backoff window is answered instead of refused.
	 *
	 * glare_op carries the operation because an ACTIVE leg carries none in
	 * c->op. reinvite_done() and reinvite_fail() read c->op as the
	 * operation of the exchange they are ending, and the FSM parks in
	 * PENDING until one of them reports; a leg that sat in ACTIVE with an
	 * operation in c->op would let the next completion report an exchange
	 * that never ran.
	 */
	bool glare_wait;
	uint8_t glare_op;		/* enum call_op */
	bool cancel_wanted;		/* hangup before any provisional (9.1) */
	bool clearing;			/* the leg is going away; suppress outcomes */
	/*
	 * The leg drains for a transfer this side asked for (call.h call_xfer,
	 * call_xfer_consult): silenced, its dialog kept for the network BYE or
	 * for the verdict. A peer re-INVITE is answered 2xx without a stream,
	 * and no path of this module BYEs the leg early; the verdict, the
	 * guards and an eviction do. leg_release() alone clears it, because
	 * bye_send() runs xfer_state_clear() in the middle of a drain and the
	 * flag has to outlive that.
	 */
	bool xfer_drain;
	/*
	 * The leg is carrying a fax. It is set when the backend has accepted
	 * the passthrough profile, never merely when one was asked for. It
	 * dies with the leg, so the next call on this slot negotiates as a
	 * voice call again, and call_fax_exit() below is the one other exit:
	 * an isolated short plain answer tone takes the leg back to voice. Its
	 * one reader is media_program(), which re-asserts the three fields a
	 * fax carrier cannot survive on every later programming of the stream.
	 */
	bool fax_on;
	/*
	 * What that passthrough carries (enum hal_passthrough). The profile
	 * arms the modem kind, because a CED answer says a data device answered
	 * and nothing yet says which kind it is, and the first local V.21
	 * preamble moves it to the fax kind. It travels to the backend on the
	 * description, beside the three fields above, so a backend reads one
	 * record and no separate profile state can fall out of step with it.
	 */
	uint8_t fax_kind;
	/*
	 * When the shell last reported an answer tone from the wire on this leg
	 * (call.h call_fax_hint), 0 for never. It is the tone half of the
	 * far-end upspeed recognizer: the SDP half alone cannot tell a fax
	 * gateway's narrowing offer from an ordinary codec change, and this
	 * stamp is what says a data device answered on the other side.
	 *
	 * The stamp is clamped away from 0 (uvoice_stamp_nonzero), because the
	 * monotonic clock reads 0 for a millisecond after boot and again every
	 * 2^32 ms, and a real hint must never read as none.
	 */
	uint32_t fax_hint_ms;
	/*
	 * The return-to-voice rule of call.h call_fax_exit (ITU-T T.30 4.1.1,
	 * ITU-T V.25 2.2 and 4.4, ITU-T V.8 8.2.2).
	 *
	 * fax_tone_code and fax_tone_dir name the tone that armed the leg and
	 * the feed it arrived on, so an end row of another tone or another feed
	 * adds nothing. fax_tone_ms is that tone's accumulated duration, summed
	 * over the end rows, because a packet loss splits one real tone into
	 * two reports. fax_evidence says a later row, a followed upspeed or an
	 * accepted image session proved the tone real.
	 *
	 * fax_exit_pending says an exit ran while an exchange was open: the fax
	 * fields are already clear and the codec list still waits, because the
	 * answer of the leg's own exchange selects from it. The three fields
	 * beside it are what that exit cleared, kept on the leg because the
	 * completion can still fail: a stream that refuses the voice
	 * description leaves a leg carrying the carrier it always carried, and
	 * a status that read voice over it would describe a call that is not
	 * running.
	 */
	uint16_t fax_tone_code;
	uint8_t fax_tone_dir;
	uint32_t fax_tone_ms;
	bool fax_evidence;
	bool fax_exit_pending;
	uint8_t fax_exit_kind;
	bool fax_exit_on;
	uint32_t fax_exit_hint_ms;
	/*
	 * One programming of the stream that runs whatever the cache holds.
	 * media_program() reads it in its changed term alone and clears it
	 * behind the push, so a description the leg has to re-assert reaches
	 * the backend even where the bytes did not move. A failed exchange
	 * pushes no media of its own, and the exit behind it has a stream
	 * still running the kind the cache already describes.
	 */
	bool media_force;
	/*
	 * The leg's codec view before the first G.711 clamp of this call
	 * (loc_g711_clamp), kept so a return to voice can put it back. Only
	 * the three members the clamp moves are kept. fax_loc_set says the
	 * snapshot holds a view, and it is taken once per leg: a second clamp
	 * describes a list the first one already reduced.
	 */
	struct sdp_codec fax_loc_codec[SDP_CODEC_MAX];
	unsigned int fax_loc_codecs;
	bool fax_loc_cn;
	bool fax_loc_set;
	/*
	 * The transferor side of one REFER (call.h call_xfer, RFC 3515 2.4.4).
	 *
	 * xfer_pending is the implicit subscription: it is set when the REFER
	 * reaches the wire and stands until the verdict, so rx_notify() knows
	 * which NOTIFYs belong to a transfer. xfer_owed is the report debt: set
	 * beside it for the kinds that report, taken by the one cb.xfer_done,
	 * so the exactly-once rule is a flag and not a count. The two part on a
	 * blind 202, which reports at once and keeps the subscription for the
	 * NOTIFYs the drained leg still answers. refer_cseq is the CSeq of that
	 * REFER, which is the id of the implicit subscription the NOTIFY names.
	 *
	 * xfer_bye_grace names the second purpose of t_xfer: behind a 2xx
	 * verdict the guard is consumed, and the same timer bounds the wait for
	 * the network's own BYE before this side sends its own (call.h
	 * CALL_XFER_BYE_GRACE_MS). It dies with the state, as the guard does.
	 *
	 * xfer_target keeps the resolved Refer-To target because the auth
	 * retry has to build the request again; the consult form rebuilds
	 * the embedded Replaces from xfer_other.
	 *
	 * xfer_other is a handle and not a slot index. The replaced leg can die
	 * during the wait, and the subscriber of a consult transfer is already
	 * on hook, so the slot it frees is one a new inbound call may take and
	 * ring on. A raw index resolved at outcome time would then BYE that new
	 * call; the generation in the handle makes leg_get() answer NULL for a
	 * leg that is gone, which is exactly the leg this transfer meant.
	 */
	uint32_t refer_cseq;
	uint32_t xfer_other;		/* consult: the replaced leg handle */
	uint8_t xfer_kind;		/* enum call_xfer_kind */
	bool xfer_pending;		/* the implicit subscription stands */
	bool xfer_owed;			/* an outcome is owed through cb.xfer_done */
	bool xfer_bye_grace;		/* t_xfer waits for the network BYE */
	bool xfer_consult;		/* the on-hook form: the referee is BYEd,
					   the replaced leg drains or is BYEd */
	/*
	 * The verdict of a NOTIFY that overtook the answer to its own REFER.
	 * The subscription exists from the moment the REFER goes out, so on an
	 * unordered transport the final NOTIFY can arrive first: applying it
	 * there would clear the leg while the REFER itself can still fail, so
	 * it waits here and the answer to the REFER decides.
	 */
	bool xfer_verdict;
	bool xfer_verdict_ok;
	uint8_t xfer_verdict_src;	/* enum xfer_source */
	uint32_t xfer_verdict_code;
	char xfer_target[CALL_URI_MAX];
	/*
	 * The leg ended because a transfer completed (call.h, call_ended
	 * transferred). It is set on the accept and never on a failure, and it
	 * is the correlation key of the record this leg closes.
	 */
	bool transferred;
	/*
	 * The transferee side of RFC 3515. One leg holds either half and never
	 * both halves of one transfer.
	 *
	 * The parent half is refer_active, which is the implicit subscription
	 * this side serves: it is set when the referral is accepted and cleared
	 * by the final NOTIFY, so a second REFER while one is served is a flag
	 * test and the exactly-once rule of that final report is the same flag.
	 * refer_in_cseq is the CSeq of the REFER, which is the id the NOTIFY
	 * names (RFC 3515 2.4.4). xfer_kid is the child leg.
	 *
	 * The child half is xfer_child with the parent's handle beside it.
	 * Both are handles and not slot indexes, for the reason xfer_other is
	 * one: either leg can die while the other still runs, and a raw index
	 * resolved later would name whatever took that slot next.
	 *
	 * xfer_hdrs is the child's Replaces block; see CALL_XFER_HDRS_MAX.
	 */
	uint32_t refer_in_cseq;
	uint32_t xfer_kid;		/* the parent's child leg handle */
	uint32_t xfer_parent;		/* the child's parent leg handle */
	bool refer_active;		/* a referral is served on this leg */
	bool xfer_child;		/* this leg is the child of one */
	/*
	 * Whether the leg this one replaces was held, latched onto the child at
	 * the moment the referral was accepted. It is a latch and not a read of
	 * the parent, because the parent is usually gone by the time the child
	 * answers: the transferor BYEs it at the 202. The fact cannot move
	 * under the latch either, because a demoted leg refuses the two hold
	 * commands and a peer re-INVITE moves the remote hold, not this one.
	 */
	bool xfer_held;
	char xfer_hdrs[CALL_XFER_HDRS_MAX];
	/*
	 * The last NOTIFY of a served referral, kept so a proxy that challenges
	 * it can be answered: the credentials ride the same insert that builds
	 * the request, so the retry builds it again (the pattern the REFER of
	 * a transferor already follows).
	 */
	uint16_t notify_code;
	bool notify_final;
	bool prov_seen;
	bool ring_sent;
	/*
	 * The reliable 1xx this leg acknowledged (RFC 3262 4): the last
	 * in-order RSeq, 0 for none, and the PRACK in flight. early_final says
	 * a reliable 1xx carried the answer.
	 */
	uint32_t prack_rseq;
	uint32_t prack_txn;
	bool early_final;
	/*
	 * The reliable 180 this leg sent (RFC 3262 3). rel_open stands from the
	 * send until the matching PRACK, across the final response; t_rel
	 * retransmits it while the INVITE server transaction is proceeding and
	 * rejects the INVITE at rel_deadline_ms.
	 *
	 * rel_last marks the arm that runs to rel_deadline_ms, so the give-up
	 * needs no clock reading of its own: a timer fires as soon as the time
	 * left truncates to zero milliseconds, which leaves the clock short of
	 * the deadline it was armed for.
	 */
	struct sip_timer t_rel;
	uint64_t rel_deadline_ms;
	uint32_t rel_rseq;
	uint32_t rel_cseq;
	uint32_t rel_retx_ms;
	bool rel_open;
	bool rel_last;
	bool reported;			/* the FSM is owed no further end report */
	/* the call record facts, sticky for the life of the leg */
	bool answered;			/* the leg reached active */
	bool failed;			/* a clearing class was reported */
	uint8_t fail_class;		/* enum call_fail_class, valid when failed */
	uint64_t cleared_env_ms;	/* env clock at the BYE, 0 = never cleared */
	/*
	 * env clock when this leg reached active, 0 while it never did. The
	 * record needs the answer as an age for the reason it needs the clear
	 * as one (call.h, answered_ago_ms), and a transfer's child announces
	 * that moment to nobody.
	 */
	uint64_t answered_env_ms;
};

static struct {
	struct call_media_ops ops;
	struct call_cb cb;
	const struct cfg *cfg;
	const struct proto_host *host;	/* call.h calls_host_bind */
	bool (*hal_ready)(void);	/* call.h calls_hal_ready_bind */
	/* call.h calls_line_admit_bind */
	bool (*line_admit)(unsigned int line);
	uint32_t hal_features;		/* call.h calls_hal_features_set */
	uint32_t hal_codecs;		/* call.h calls_hal_codecs_set */
	bool inited;
} cv;

static struct call calls[CFG_CHANNEL_MAX][CALL_SLOTS];

/*
 * What one line turned away, in the three classes a reader asks for first: the
 * subscriber is busy, the line takes no call for now, and everything else. A
 * refusal in front of the leg, which is every rule of rx_new_invite(), opens
 * no record at all, so these counters are its only trace. A rejection of a
 * ringing leg, which is call_reject() and ring_refuse(), closes a failed
 * record as well and is counted here too.
 *
 * env is the transaction environment last_ms was read on. The refusal site
 * holds one and calls_status_dump() holds none, so the note keeps it for the
 * age the dump writes. The pointer outlives the view contract of account.h on
 * purpose, and two facts make that safe. The account table is static and the
 * environment is the first member of both transport arms, so one address
 * stands for the life of the daemon and a rebuild writes the same one back.
 * account_build() memsets the account before account_env_init() fills the
 * environment again; a dump inside that window would read a NULL now_ms, and
 * refused_dump() writes the age 0 for it. The view cannot serve instead:
 * account_call_view() answers nothing for a line that is down, and a line that
 * is down must still dump the calls it turned away.
 */
struct call_refused {
	uint32_t busy;
	uint32_t unavail;
	uint32_t other;
	uint32_t last_code;
	uint64_t last_ms;
	struct sip_txn_env *env;
};

static struct call_refused refused[CFG_CHANNEL_MAX];

/* ---------------------------------------------------------- small helpers */

/*
 * The three answers that say the called party is busy rather than absent or
 * unreachable (RFC 3261 21.4.24, 21.6.1 and 21.6.2). The clearing class below,
 * the refusal counters and the cause a rejection writes read this one list, so
 * the three surfaces cannot disagree about one code.
 */
static bool refused_busy_is(uint32_t code)
{
	return code == 486 || code == 600 || code == 603;
}

/*
 * The four clearing classes of call.h. Everything the two named lists do not
 * cover is congestion, which is the class that plays fast busy: a code uvoice
 * does not model must not sound like a free line.
 */
enum call_fail_class call_fail_map(uint32_t code)
{
	if (refused_busy_is(code))
		return CALL_FAIL_BUSY;

	switch (code) {
	case 404:
	case 484:
	case 604:
		return CALL_FAIL_UNOBTAINABLE;
	default:
		return CALL_FAIL_CONGESTION;
	}
}

/*
 * One refused inbound call. The code and the instant of the last refusal stand
 * beside the three counts, because the class alone does not say which rule
 * answered. env carries the clock the stamp is taken on. A refusal in front of
 * the leg has no leg to read it from, so the refusal site hands it over.
 */
static void refused_note(struct sip_txn_env *env, unsigned int line,
			 uint32_t code)
{
	struct call_refused *r;

	if (line >= CFG_CHANNEL_MAX)
		return;

	r = &refused[line];
	if (refused_busy_is(code))
		r->busy++;
	else if (code == 480 || code == 503)
		r->unavail++;
	else
		r->other++;

	r->last_code = code;
	r->last_ms = env && env->base.now_ms ? env->base.now_ms(&env->base) : 0;
	r->env = env;
}

void calls_refused_note(unsigned int line, struct sip_txn_env *env,
			uint32_t code)
{
	refused_note(env, line, code);
}

static const char *state_name(enum call_state st)
{
	switch (st) {
	case CALL_ST_FREE:
		return "free";
	case CALL_ST_INVITE_SENT:
		return "invite-sent";
	case CALL_ST_PROCEEDING:
		return "proceeding";
	case CALL_ST_CANCEL_SENT:
		return "cancel-sent";
	case CALL_ST_RINGING_IN:
		return "ringing-in";
	case CALL_ST_ACCEPT_WAIT_ACK:
		return "accept-wait-ack";
	case CALL_ST_ACTIVE:
		return "active";
	case CALL_ST_REINVITE_SENT:
		return "reinvite-sent";
	case CALL_ST_REINVITE_WAIT_ACK:
		return "reinvite-wait-ack";
	case CALL_ST_BYE_SENT:
		return "bye-sent";
	}

	return "?";
}

static void leg_trace(struct call *c, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void leg_trace(struct call *c, const char *fmt, ...)
{
	struct sip_txn_env *env = c->env;
	va_list ap;

	if (!env || !env->base.trace)
		return;

	va_start(ap, fmt);
	sip_txn_env_trace(env, fmt, ap, "call %u.%u %s: ", c->line, c->slot,
			  state_name((enum call_state)c->state));
	va_end(ap);
}

static void state_set(struct call *c, enum call_state st)
{
	if (c->state == (uint8_t)st)
		return;

	/* the call record wants the fact that this call connected, and the
	 * state moves on: a hold, a refresh and the BYE all leave active */
	if (st == CALL_ST_ACTIVE && !c->answered) {
		c->answered = true;
		if (c->env && c->env->base.now_ms)
			c->answered_env_ms = c->env->base.now_ms(&c->env->base);
	}

	c->state = (uint8_t)st;
	leg_trace(c, "entered");
}

static uint16_t gen_bump(uint16_t g)
{
	g++;

	return g ? g : 1;
}

static uint32_t leg_handle(const struct call *c)
{
	return sip_handle_make(c->line * CALL_SLOTS + c->slot, c->gen);
}

static uint32_t leg_owner(const struct call *c)
{
	return acct_owner_make(ACCT_OWNER_CALL, leg_handle(c));
}

static struct call *leg_get(uint32_t handle)
{
	unsigned int idx = sip_handle_index(handle);
	struct call *c;

	if (idx >= CFG_CHANNEL_MAX * CALL_SLOTS)
		return NULL;

	c = &calls[idx / CALL_SLOTS][idx % CALL_SLOTS];
	if (!c->in_use || c->gen != sip_handle_gen(handle))
		return NULL;

	return c;
}

static struct sip_dialog *leg_dialog(struct call *c)
{
	if (!c->dialogs || c->dialog == SIP_HANDLE_NONE)
		return NULL;

	return sip_dialog_get(c->dialogs, c->dialog);
}

static struct sip_txn *leg_txn(struct call *c, uint32_t handle)
{
	if (!c->mgr || handle == SIP_HANDLE_NONE)
		return NULL;

	return sip_txn_get(c->mgr, handle);
}

/* --------------------------------------------------------- the 491 backoff */

/*
 * The leg is free to start an exchange of its own: it is active and no
 * operation of ours waits for its glare timer. Every gate that asks "is this
 * leg settled" reads this, so a leg in the backoff answers as a leg with an
 * exchange in flight. rx_reinvite() is the one reader of the state alone,
 * because the backoff changes exactly one answer, the one to a re-INVITE of
 * the peer.
 */
static bool leg_free_is(const struct call *c)
{
	return c->state == CALL_ST_ACTIVE && !c->glare_wait;
}

/* the 491 arrived and uac_glare_take() armed the timer: the operation steps
 * aside and the leg goes back to active for the length of the wait */
static void glare_wait_enter(struct call *c)
{
	c->glare_op = c->op;
	c->op = CALL_OP_NONE;
	c->glare_wait = true;
	state_set(c, CALL_ST_ACTIVE);
	leg_trace(c, "the 491 retry of operation %u waits", c->glare_op);
}

/*
 * The wait ends, either because the retry is starting or because the leg is.
 * It runs at sites that cancel the glare timer already, so the cancel is
 * guarded by the flag: only a wait that really stood spends a second call.
 */
static void glare_wait_drop(struct call *c)
{
	if (!c->glare_wait)
		return;

	c->glare_wait = false;
	c->glare_op = CALL_OP_NONE;
	if (c->ua)
		ua_glare_cancel(c->ua, leg_owner(c));
}

/* the leg copies the account's netdev into a field of its own and the HAL
 * copies that into the stream, so a width that shrank on either side would
 * truncate a device name into a bind that scopes to the wrong netdev */
_Static_assert(CALL_IFACE_MAX == HAL_IFNAME_MAX,
	       "the leg and HAL bind device widths must agree");

/*
 * The wideband digit fallback. A telephone-event stream runs on the RTP
 * clock of the audio it rides (RFC 4733 section 2.1), so an Opus stream
 * needs telephone-event/48000 and a peer that named none leaves this leg with
 * no relay payload type at all. The policy is the one media_fill() already
 * applies: SIP INFO stays when the channel configured it and RFC 2833 falls
 * back to inband.
 *
 * The fallback is counted and named because inband DTMF over Opus is
 * unreliable and an operator has to see it happen. The narrowband refusal keeps
 * its silence: it is the shipped behavior of every board.
 *
 * The predicate lives here and the report does not. media_fill() runs
 * twice for one exchange, once when the 2xx goes out and again when the ACK
 * commits the same answer, so a report at the fill would count every Opus
 * call twice. media_program() owns the report: it is the one place that
 * knows whether the description is new, and it fires this once per exchange.
 * The predicate is read before the T.38 arm of the fill, so it reads the
 * negotiated audio codec, exactly as the fill does.
 */
static bool opus_dtmf_fell_back(const struct cfg_channel *ch, const struct sdp_result *res)
{
	if (res->dtmf_pt >= 0 || ch->dtmf_mode == CFG_DTMF_INBAND)
		return false;

	return media_codec_of((enum sdp_codec_id)res->codec.id) ==
	       HAL_CODEC_OPUS;
}

static unsigned int opus_dtmf_fallbacks;

unsigned int calls_opus_dtmf_fallbacks(void)
{
	return opus_dtmf_fallbacks;
}

static void opus_dtmf_fallback_report(const struct hal_media *m)
{
	opus_dtmf_fallbacks++;
	ULOG_WARN("opus stream: the peer named no telephone-event/48000, digits fall back to %s (%u so far)\n",
		  m->dtmf_mode == HAL_DTMF_INFO ? "SIP INFO" : "inband", opus_dtmf_fallbacks);
}

/* --------------------------------------------------------------- media */

static void media_stop(struct call *c)
{
	if (!c->media_started)
		return;

	c->media_started = false;

	/*
	 * The snapshot goes first, before the stop. This is the one choke
	 * point every media teardown
	 * passes through, so no other path may take the snapshot.
	 */
	if (cv.ops.stats_snapshot)
		cv.ops.stats_snapshot(cv.ops.priv, c->line, c->slot);

	if (cv.ops.stop)
		cv.ops.stop(cv.ops.priv, c->line, c->slot);
}

/*
 * The stop of a mode transition. An audio stream cannot be turned into a
 * UDPTL one in place: the DSP holds the codec of a running session, so the
 * stream stops, is configured again with the new mode and starts.
 *
 * It takes no media snapshot. The call record contract is one snapshot per
 * record, and the leg's single snapshot stays at teardown, where it describes
 * the phase the call ended in. A second one here would either overwrite that
 * record or spend the one job slot the stream has.
 *
 * The status is read, unlike the teardown stop: a stream that could not leave
 * audio cannot carry the fax the exchange just promised. hal.h section 3 asks
 * for one retry on HAL_TRANSIENT, which runs in this frame like every other
 * retry of that class (media_fax_apply, detector_arm).
 */
static bool media_mode_stop(struct call *c)
{
	enum hal_status st;

	/* a stream this leg never started needs no stop, which is the guard
	 * media_stop() takes for the same reason */
	if (!c->media_started || !cv.ops.stop)
		return true;

	st = cv.ops.stop(cv.ops.priv, c->line, c->slot);
	if (st == HAL_TRANSIENT)
		st = cv.ops.stop(cv.ops.priv, c->line, c->slot);

	if (st != HAL_OK) {
		leg_trace(c, "stream stop refused (%s), the mode transition is abandoned",
			  hal_status_name(st));
		return false;
	}

	c->media_started = false;

	return true;
}

/*
 * The one place a stream is programmed. It runs on every committed exchange,
 * not only on a change: the backend connects the media sockets to
 * hal_media.remote and never latches onto a new source, so a peer that moved
 * its address or its payload type must be re-applied or the stream keeps
 * sending to the old tuple.
 */
static bool media_program(struct call *c, const struct sdp_result *res, bool start)
{
	struct hal_media m;
	bool changed, mode_move, rekey;
	bool self_stopped = false;

	if (!res->valid)
		return false;

	if (c->rtcp_interval > MEDIA_RTCP_INTERVAL_MAX)
		leg_trace(c, "rtcp_interval %u s capped at %u", c->rtcp_interval,
			  MEDIA_RTCP_INTERVAL_MAX);

	/* the leg snapshot, never cv.cfg: a reload may have replaced it */
	media_fill(&c->ch, c->rtcp_enabled, c->rtcp_interval,
		   (cv.hal_features & HAL_FEAT_RTCP_ADDR) != 0, res,
		   c->loc.addr, c->loc.port, c->bind_dev, c->fax_kind,
		   c->punch_interval_ms, &m);

	/*
	 * An address the fill refused arrives as an empty description. A fill
	 * that ran to the end always names a remote port, because sdp.c refuses
	 * a zero port on the offer and on the answer side alike. The cache is
	 * left alone for the same reason the config refusal below leaves it.
	 */
	if (!m.remote.port) {
		leg_trace(c, "media address refused");
		return false;
	}

	/*
	 * The hold silence (hal.h, hal_media.tx_silence). RFC 3264 asks a
	 * holder to send silence or music and not the room, and a=sendonly says
	 * nothing about what the holder sends: without this the stream keeps
	 * encoding the live microphone, and the held party hears the room and
	 * the dial tone the FXS hybrid leaks into it.
	 *
	 * It is read from the leg and not from the answer, because local hold is
	 * this side's own state: a peer that answers a hold offer sendrecv, and
	 * a peer that holds us back, must not turn this uplink on again. It sits
	 * here and not in media_fill(), which takes no leg, so every
	 * exchange that programs a stream passes it: the hold, the resume, the
	 * session refresh, an incoming re-INVITE and the transferee promotion
	 * all reach this one choke point, and a resume therefore clears the flag
	 * by the same read that a hold sets it. It lands before the comparison
	 * below, so a leg whose description moved only here is still a change
	 * the stream is reprogrammed with.
	 *
	 * The latch is read beside it, because a transferee is held before it
	 * knows it. xfer_held says the leg this one replaced was held, and the
	 * child's own sdp_local_hold() is still false while it programs the
	 * stream it just answered: without the latch the promoted leg would
	 * carry the room until the settle's hold re-offer completes, and for
	 * good if that re-offer cannot be sent. The settle clears the latch and
	 * calls reinvite_start(), which sets the local direction to sendonly, in
	 * one frame with no media programmed in between, so the two together
	 * never open a window.
	 */
	m.tx_silence = sdp_local_hold(&c->sdp) || c->xfer_held;

	/*
	 * The mode moved: the cached description and the new one disagree
	 * about whether this stream carries T.38, so the stream has to be
	 * stopped before it is configured again. A stop failure leaves the
	 * cache untouched and the old stream running, and returns false. A
	 * config or start refusal behind a successful stop leaves the stream
	 * down and returns false. The 2xx callers abandon the leg with
	 * congestion on either false. uas_ack_take() in a later exchange and
	 * media_commit_push() read media_started instead. A refusal that left
	 * the stream running is only recorded. A refusal that left it down
	 * abandons the leg with congestion, because a stream that is down
	 * cannot carry what the exchange agreed.
	 *
	 * The record of such a leg carries no media table, and that is
	 * accepted: the teardown stop finds media_started already false and
	 * takes no snapshot, so the record is presence-gated to "absent" rather
	 * than carrying numbers from the phase before the transition. The
	 * alternative snapshots the same stream twice.
	 *
	 * A changed SDES key or suite takes the same path (hal.h, the srtp
	 * block). A bare re-configuration can move the transmit key at once,
	 * while the receiving side keeps authenticating with the key it had.
	 * The stop gives the receiving side a clean point to resynchronize
	 * against. libudsp can rekey in place, but this layer
	 * stops the stream on every backend. An identical description, the
	 * same suite under the same two keys, takes neither a stop nor a
	 * configure.
	 */
	mode_move = c->media_set &&
		    (c->media.codec == HAL_CODEC_T38) != (m.codec == HAL_CODEC_T38);
	rekey = c->media_set && memcmp(&m.srtp, &c->media.srtp, sizeof(m.srtp)) != 0;
	if (mode_move || rekey) {
		/* whether this call is the one that took a running stream
		 * down, which is what the start decision owes a restart */
		self_stopped = c->media_started;
		if (!media_mode_stop(c))
			return false;
	}

	/*
	 * Both descriptions were built by the same memset-then-fill, so the
	 * padding compares equal too.
	 *
	 * media_force is the one caller that programs an identical
	 * description. A leg that returned to voice behind a failed exchange
	 * has a stream running the kind, and a cache that already describes
	 * the committed answer; only the force reaches the backend there. It
	 * is read here and nowhere else, so the mode move and the rekey above
	 * keep deciding on the cache alone.
	 */
	changed = !c->media_set || c->media_force ||
		  memcmp(&m, &c->media, sizeof(m)) != 0;

	if (changed && cv.ops.config_set &&
	    cv.ops.config_set(cv.ops.priv, c->line, c->slot, &m) != HAL_OK) {
		/* The cache still describes what the stream actually carries,
		 * so the next attempt sees the change again and re-applies it. */
		leg_trace(c, "stream config refused");
		return false;
	}

	c->media = m;
	c->media_set = true;
	c->media_force = false;

	/*
	 * Once per exchange. The 2xx and the ACK program the same answer, so
	 * the second run finds an identical description and reports nothing; a
	 * re-INVITE that really moves the media is a new exchange and reports
	 * again. It sits behind the cache update so the count names the
	 * descriptions the stream took, not the ones the HAL refused.
	 */
	if (changed && opus_dtmf_fell_back(&c->ch, res))
		opus_dtmf_fallback_report(&m);

	/*
	 * A stream this call stopped is started again whatever the caller
	 * asked for. The mode move and the rekey above take a running stream
	 * down inside this one call, so a push made with start false would
	 * otherwise leave an active leg with no media at all. A stream that was
	 * already down before the call is the caller's to start: a re-program
	 * of a leg whose stream another path stopped, a transfer promotion
	 * above all, must not put that stream back on the wire.
	 */
	if (c->media_started || (!start && !self_stopped))
		return true;

	if (cv.ops.start && cv.ops.start(cv.ops.priv, c->line, c->slot) != HAL_OK) {
		leg_trace(c, "stream start refused");
		return false;
	}

	c->media_started = true;
	c->media_ran = true;

	return true;
}

/* ------------------------------------------------------- leg lifecycle */

static void sess_stop(struct call *c);
static void cb_ended(struct call *c);
/* the leg a failed child hands its name back to */
static void ui_undemote(struct call *c);

/*
 * The crypto attributes of one parsed message. Every sdp_msg this module parses
 * into a stack copy carries the peer's decoded master key, and a dead frame is
 * not storage anybody clears: each function that parses one wipes it where the
 * copy dies. Both sections are wiped, because the parser stores a crypto
 * attribute on whichever section carried it.
 */
static void sdp_msg_keys_wipe(struct sdp_msg *m)
{
	explicit_bzero(m->audio.crypto, sizeof(m->audio.crypto));
	explicit_bzero(m->image.crypto, sizeof(m->image.crypto));
}

/*
 * No key outlives the leg that used it (hal.h, the srtp block). The slots are
 * static and the memset that clears one runs at the next allocation, so a
 * released leg would otherwise keep its master keys readable for as long as
 * the line stays idle.
 *
 * Every copy this module retains is wiped, and the list is the whole of it: the
 * two keys this side offers, the exchange keys of both negotiation views, the
 * description the stream was programmed with, and the decoded keys of the
 * inbound offer. The short lived copies are out of reach from here, because a
 * callee's frame is gone by the time a leg is released: each of those is wiped
 * at its own site instead (sdp_msg_keys_wipe above, the gated local of
 * rx_reinvite()).
 *
 * The two text buffers go too. An a=crypto line is the one place a key crosses
 * as text (shared/sdp.h), so the offer or answer this leg last built still
 * carries it in base64, and a released slot must not keep that until the next
 * allocation runs its memset. Both are wiped whole, by sizeof and not by the
 * length either one last held: an earlier exchange may have written a longer
 * body, and the tail of it would otherwise survive.
 *
 * It is explicit_bzero and not memset: the compiler may drop a memset over
 * storage nothing reads again, which is exactly this storage.
 */
static void leg_keys_wipe(struct call *c)
{
	explicit_bzero(c->loc.srtp_key, sizeof(c->loc.srtp_key));
	explicit_bzero(c->sdp.committed.srtp_key_tx, sizeof(c->sdp.committed.srtp_key_tx));
	explicit_bzero(c->sdp.committed.srtp_key_rx, sizeof(c->sdp.committed.srtp_key_rx));
	explicit_bzero(c->sdp.proposed.srtp_key_tx, sizeof(c->sdp.proposed.srtp_key_tx));
	explicit_bzero(c->sdp.proposed.srtp_key_rx, sizeof(c->sdp.proposed.srtp_key_rx));
	explicit_bzero(&c->media.srtp, sizeof(c->media.srtp));

	sdp_msg_keys_wipe(&c->offer);

	explicit_bzero(c->body, sizeof(c->body));
	explicit_bzero(c->wire, sizeof(c->wire));
}

/*
 * The independent check of the wipe above (call.h). A released slot is
 * reachable from nothing else this module exports, so without this sweep the
 * zeroize rule has no test at all.
 */
bool call_slot_scan(unsigned int line, unsigned int slot, const void *pat, size_t len)
{
	const uint8_t *base = (const uint8_t *)&calls[line][slot];
	size_t span = sizeof(calls[line][slot]);
	size_t i;

	if (line >= CFG_CHANNEL_MAX || slot >= CALL_SLOTS)
		return false;
	if (!pat || !len || len > span)
		return false;

	for (i = 0; i + len <= span; i++) {
		if (!memcmp(base + i, pat, len))
			return true;
	}

	return false;
}

/*
 * The pending transfer is dropped, not reported. Every caller either fired
 * cb.xfer_done already or is a leg that died first, and call.h states that a
 * leg which dies first voids the pending outcome: the far-end death travels the
 * ordinary path instead. Cancelling the guard is what makes it safe, because an
 * armed timer on a released slot would fire on a leg that is gone.
 */
static void xfer_state_clear(struct call *c)
{
	c->xfer_pending = false;
	c->xfer_owed = false;
	c->xfer_bye_grace = false;
	c->xfer_consult = false;
	c->xfer_other = SIP_HANDLE_NONE;
	c->xfer_verdict = false;
	c->xfer_verdict_ok = false;
	c->xfer_verdict_src = 0;
	c->xfer_verdict_code = 0;
	c->refer_cseq = 0;
	c->refer_txn = SIP_HANDLE_NONE;
	c->xfer_target[0] = 0;
	if (c->env)
		sip_timer_cancel(c->env, &c->t_xfer);
}

static void xfer_release_take(struct call *c);

static void leg_release(struct call *c)
{
	struct sip_dialog *d;

	if (!c->in_use)
		return;

	/*
	 * The other half of a transfer hears first, while this leg still has a
	 * dialog to send a NOTIFY on and while the child it shadows can still
	 * be handed the UI slot it was waiting for.
	 */
	xfer_release_take(c);

	/*
	 * The two run in this order and before the teardown below: the stop
	 * inside media_stop() submits the media snapshot, and the record the
	 * report opens has to be the one that snapshot completes against.
	 */
	media_stop(c);
	cb_ended(c);

	sess_stop(c);
	xfer_state_clear(c);
	if (c->env) {
		sip_timer_cancel(c->env, &c->t_c);
		/* the guard is armed on a leg that is about to be released, and
		 * leg_alloc() memsets the storage it lives in: a timer left
		 * armed across that memset fires on the next call's slot */
		sip_timer_cancel(c->env, &c->t_bye);
		sip_timer_cancel(c->env, &c->t_rel);
	}
	glare_wait_drop(c);
	if (c->ua)
		ua_glare_cancel(c->ua, leg_owner(c));

	d = leg_dialog(c);
	if (d) {
		sip_dialog_terminate(d);
		sip_dialog_put(d);
	}
	c->dialog = SIP_HANDLE_NONE;

	cv.host->net_port_free(cv.host->ctx, c->rtp_port);
	c->rtp_port = 0;

	/* The fax dies with the leg, and the next call on this slot must fill
	 * an ordinary voice stream. The reset that guarantees that is the
	 * memset in leg_alloc(); this clear is documented redundancy, so a
	 * released leg already reads as not-in-fax for anything that inspects
	 * it before the slot is handed out again. The kind goes with it, for
	 * the same reason and by the same rule. A return to voice is the other
	 * exit (call.h call_fax_exit), and it leaves the leg standing. */
	c->fax_on = false;
	c->fax_kind = HAL_PASSTHROUGH_OFF;
	/* and so does the tone evidence: it describes one conversation, and the
	 * next call on this slot must not be followed on a tone the last one
	 * heard */
	c->fax_hint_ms = 0;
	/* the return-to-voice state of that conversation goes with it: the
	 * snapshot describes a codec list this slot no longer negotiates, and a
	 * pending exit has no exchange left to finish in */
	c->fax_tone_code = 0;
	c->fax_tone_dir = 0;
	c->fax_tone_ms = 0;
	c->fax_evidence = false;
	c->fax_exit_pending = false;
	c->fax_exit_kind = HAL_PASSTHROUGH_OFF;
	c->fax_exit_on = false;
	c->fax_exit_hint_ms = 0;
	c->fax_loc_set = false;
	c->media_force = false;

	/* the last frame that may read this leg's key material: the slot is
	 * marked free right below */
	leg_keys_wipe(c);

	c->in_use = false;
	c->ui_busy = false;
	c->xfer_drain = false;
	c->state = CALL_ST_FREE;
	c->gen = gen_bump(c->gen);
	leg_trace(c, "released");
}

/*
 * The protocol objects of an owner that is going away for good. The 2xx
 * retransmission record is not released here when the peer already ACKed: RFC
 * 6026 lets it expire on its own, and a record that never got its ACK is the
 * one case where dropping it early would lose the answer.
 */
static void leg_detach(struct call *c)
{
	uint32_t owner = leg_owner(c);

	if (c->ua)
		ua_owner_release(c->ua, owner);
	if (c->mgr)
		sip_txn_owner_release(c->mgr, owner);
}

static void leg_timers_init(struct call *c);
static bool bye_send(struct call *c);

/*
 * A draining predecessor gives the slot up: call_slot_free() reads ui_busy
 * alone, so the UI already handed this slot out. A transfer drain still owns
 * a dialog the network expects to end itself, so it is BYEd on the way out:
 * released in silence, the network's own BYE would meet 481. The BYE client
 * transaction keeps its wire copy and runs to its protocol end after the
 * detach (sip/txn.h), but nothing answers a challenge to it, and a network
 * BYE that lands behind the release is answered 481 as any crossing teardown
 * is. leg_slot_pick() keeps this the last resort: a free slot is preferred.
 *
 * This runs inside whatever frame asked for the slot, which can be the
 * cb.xfer_done report of this very leg (xfer_outcome): the memset behind it
 * is why that report is the last thing its caller does with the leg.
 */
static void leg_evict(struct call *c)
{
	if (c->xfer_drain && c->state != CALL_ST_BYE_SENT)
		bye_send(c);

	leg_detach(c);
	leg_release(c);
}

static struct call *leg_alloc(unsigned int line, unsigned int slot)
{
	struct call *c = &calls[line][slot];
	uint16_t gen = c->gen;

	if (c->in_use) {
		leg_evict(c);
		gen = c->gen;
	}

	memset(c, 0, sizeof(*c));
	c->line = line;
	c->slot = slot;
	c->gen = gen ? gen : 1;
	c->in_use = true;
	c->ui_busy = true;
	/* the two names are one until a transfer moves them (call.h) */
	c->ui_slot = (uint8_t)slot;
	c->ui_role = CALL_UI_OWNER;
	c->dialog = SIP_HANDLE_NONE;
	c->invite_txn = SIP_HANDLE_NONE;
	c->server_txn = SIP_HANDLE_NONE;
	c->reinvite_txn = SIP_HANDLE_NONE;
	c->bye_txn = SIP_HANDLE_NONE;
	c->cancel_txn = SIP_HANDLE_NONE;
	c->flash_txn = SIP_HANDLE_NONE;
	c->refer_txn = SIP_HANDLE_NONE;
	c->notify_txn = SIP_HANDLE_NONE;
	c->prack_txn = SIP_HANDLE_NONE;
	c->xfer_kid = SIP_HANDLE_NONE;
	c->xfer_parent = SIP_HANDLE_NONE;
	leg_timers_init(c);

	return c;
}

/*
 * The suppression invariant, and it is the whole of it: a leg emits to the
 * FSM only while it owns the UI slot it would emit under. Every callback site
 * of this module that reaches the machine reads this one predicate, so a leg
 * the transfer moved is silenced in one place instead of thirteen.
 */
static bool ui_emits(const struct call *c)
{
	return c->ui_role == CALL_UI_OWNER;
}

/* whether this leg is the one the FSM means when it names its UI slot: the
 * owner, and the demoted parent that has not been replaced yet */
static bool ui_holds(const struct call *c)
{
	return c->in_use && c->ui_busy && c->ui_role != CALL_UI_NONE;
}

/*
 * The leg an INVITE with Replaces created (RFC 3891): the completion of an
 * attended transfer at the party that is transferred to. It shares the
 * shadow-leg machinery of a transferee's child, because it is the same shape
 * read from the other end: a leg this module admitted by itself, holding no UI
 * name, placed to take the call of the leg it replaces. The direction is the
 * whole of what tells the two apart, and it is fixed at the allocation: a
 * transferee's child always dials out and a replacement always arrives.
 */
static bool leg_replaces(const struct call *c)
{
	return c->xfer_child && !c->uac;
}

/*
 * The UI-occupancy resolver (call.h, the two-name contract). Every command the
 * FSM sends comes through it, and a direct calls[line][slot] index is forbidden
 * for those: the slot the machine names is a UI name, and the leg wearing it
 * may sit on the other physical slot.
 *
 * At most one leg ever matches, so the walk needs no tie-break. A name is held
 * by an OWNER or by the QUIET leg a transfer demoted, and the swap moves the
 * parent out of the name in the same step that moves the child into it; every
 * other leg on the line holds none. A draining leg still answers, because the
 * hangup of a leg whose BYE is already on the wire has to find it.
 */
static struct call *leg_of_ui(unsigned int line, unsigned int ui_slot)
{
	unsigned int i;

	if (line >= CFG_CHANNEL_MAX || ui_slot >= CALL_SLOTS)
		return NULL;

	for (i = 0; i < CALL_SLOTS; i++) {
		struct call *c = &calls[line][i];

		if (c->in_use && c->ui_role != CALL_UI_NONE && c->ui_slot == ui_slot)
			return c;
	}

	return NULL;
}

/*
 * The UI name a new leg takes. leg_alloc() gives it the name of the physical
 * slot it landed on, which is the identity of every leg on an untransferred
 * line. That name can be in use by another leg: a child that took its parent's
 * name over sits on the other physical slot, so the slot freed by the parent
 * carries a name the child is wearing. The line has as many names as legs, so
 * the walk always finds one.
 */
static bool ui_slot_taken(const struct call *c, unsigned int want)
{
	unsigned int i;

	for (i = 0; i < CALL_SLOTS; i++) {
		const struct call *o = &calls[c->line][i];

		if (o != c && ui_holds(o) && o->ui_slot == want)
			return true;
	}

	return false;
}

static void ui_slot_take(struct call *c)
{
	unsigned int want;

	if (!ui_slot_taken(c, c->ui_slot))
		return;

	for (want = 0; want < CALL_SLOTS; want++) {
		if (ui_slot_taken(c, want))
			continue;

		c->ui_slot = (uint8_t)want;
		leg_trace(c, "takes the free UI slot %u, its own name is in use", want);
		return;
	}
}

static struct call *leg_of_dialog(unsigned int line, uint32_t dialog)
{
	unsigned int i;

	if (line >= CFG_CHANNEL_MAX || dialog == SIP_HANDLE_NONE)
		return NULL;

	for (i = 0; i < CALL_SLOTS; i++) {
		struct call *c = &calls[line][i];

		if (c->in_use && c->dialog == dialog)
			return c;
	}

	return NULL;
}

static struct call *leg_of_server_txn(unsigned int line, uint32_t txn)
{
	unsigned int i;

	if (line >= CFG_CHANNEL_MAX || txn == SIP_HANDLE_NONE)
		return NULL;

	for (i = 0; i < CALL_SLOTS; i++) {
		struct call *c = &calls[line][i];

		if (c->in_use && c->server_txn == txn)
			return c;
	}

	return NULL;
}

/* ------------------------------------------------------------------ T.38 */

/*
 * uvoiced admits one T.38 session at a time across all lines.
 *
 * It is a predicate over the legs and not a counter, because a counter has to
 * be decremented on every path a leg can leave a T.38 session by, and one
 * missed path leaks the whole capability until the daemon restarts. The flag it
 * reads is the leg's own T.38 enable, which covers the three states that must
 * all hold the admission: an upgrade whose re-INVITE is in flight, an accepted
 * inbound one, and a committed image session. A released leg is not in use, so
 * a slot handed back releases the admission whatever its state was.
 */
static bool t38_admission_free(const struct call *self)
{
	unsigned int line, slot;

	for (line = 0; line < CFG_CHANNEL_MAX; line++) {
		for (slot = 0; slot < CALL_SLOTS; slot++) {
			const struct call *c = &calls[line][slot];

			if (c == self || !c->in_use)
				continue;
			if (c->sdp.local_t38)
				return false;
		}
	}

	return true;
}

/*
 * The exchange completed, and with it the one place a T.38 session can end
 * without this side asking for it: the peer answered, or offered, an
 * audio session while this leg was still enabled for T.38. Clearing the flag
 * releases the daemon-wide admission and stops the next session refresh from
 * re-offering an image stream the peer has just left.
 */
static void leg_commit_take(struct call *c)
{
	sdp_commit(&c->sdp);

	if (!c->sdp.local_t38 || c->sdp.committed.t38)
		return;

	sdp_t38_set(&c->sdp, false);
	leg_trace(c, "the committed exchange carries audio, the T.38 session is released");
}

/* ------------------------------------------------------------- callbacks */

static void cb_failed(struct call *c, enum call_fail_class cls)
{
	if (c->reported)
		return;

	c->reported = true;
	/* the record keeps the class the FSM was told, so a call that ended in
	 * a treatment is told apart from one the subscriber hung up */
	c->failed = true;
	c->fail_class = (uint8_t)cls;
	leg_trace(c, "failed %s", call_fail_name(cls));

	/*
	 * A child that failed will never take the name over, so the leg it was
	 * placed to replace speaks for that name again from this instant and
	 * not from the child's release, which is a whole BYE drain later. The
	 * invariant reads "a leg whose name another leg owns or will own says
	 * nothing", and from here no leg will.
	 */
	if (c->xfer_child && !ui_emits(c))
		ui_undemote(leg_get(c->xfer_parent));

	/* The record facts above are kept whatever the leg's role is, and only
	 * the report is suppressed: a child that failed before it took the UI
	 * over still closes a record that says so. */
	if (ui_emits(c) && cv.cb.failed)
		cv.cb.failed(c->line, c->ui_slot, cls);
}

/*
 * The answer does not set reported: a call that is up can still fail, and the
 * matrix wants that failure as CALL_FAILED in CONNECTED. reported
 * covers only an outcome the FSM already knows, that is, a hangup, a reject,
 * a teardown or a clear this layer already announced.
 *
 * The child of a transfer answers nothing to the FSM. The machine was told
 * this UI slot was answered when the parent answered, and the
 * exchange it must never see is exactly the one where a second leg answers
 * under that same name. The swap is what this answer does instead.
 */
static void cb_answered(struct call *c)
{
	if (c->xfer_child)
		return;

	if (ui_emits(c) && cv.cb.answered)
		cv.cb.answered(c->line, c->ui_slot);
}

/*
 * The call ended at the BYE while the leg lives on for the transaction drain,
 * so the moment of the clear is kept and reported as an age: the leg has no
 * clock but the one its env carries, and an age is the only
 * form the record layer can use without mixing the two clock domains. The first
 * clear wins, because a BYE that could not be sent and the clearing that
 * follows it name one call end, not two.
 */
static void clear_stamp(struct call *c)
{
	if (c->cleared_env_ms || !c->env || !c->env->base.now_ms)
		return;

	c->cleared_env_ms = c->env->base.now_ms(&c->env->base);
}

/* the age of the answer on the leg's own clock, the shape clear_take() uses */
static void answered_take(const struct call *c, struct call_ended *info)
{
	uint64_t now, ago;

	if (!c->answered_env_ms || !c->env || !c->env->base.now_ms)
		return;

	now = c->env->base.now_ms(&c->env->base);
	ago = now > c->answered_env_ms ? now - c->answered_env_ms : 0;

	info->answered_ago_ms = ago > UINT32_MAX ? UINT32_MAX : (uint32_t)ago;
}

static void clear_take(const struct call *c, struct call_ended *info)
{
	uint64_t now, ago;

	if (!c->cleared_env_ms || !c->env || !c->env->base.now_ms)
		return;

	now = c->env->base.now_ms(&c->env->base);
	ago = now > c->cleared_env_ms ? now - c->cleared_env_ms : 0;

	info->cleared = true;
	info->cleared_ago_ms = ago > UINT32_MAX ? UINT32_MAX : (uint32_t)ago;
}

/*
 * What this leg was, for the call record. It is emitted from
 * leg_release() alone, whose in_use guard gives it the single fire the
 * contract promises, and it reports facts the leg collected rather than an
 * outcome: a hangup, a rejection and a peer BYE all reach it, and only the
 * failed flag says a treatment was played.
 */
static void cb_ended(struct call *c)
{
	struct call_ended info;

	if (!cv.cb.ended)
		return;

	memset(&info, 0, sizeof(info));
	info.inbound = !c->uac;
	info.answered = c->answered;
	info.failed = c->failed;
	info.fail_class = (enum call_fail_class)c->fail_class;
	info.media_ran = c->media_ran;
	clear_take(c, &info);
	answered_take(c, &info);
	info.number = c->number;
	info.remote_number = c->cid_number;
	info.remote_name = c->cid_name;
	if (c->sdp.committed.valid) {
		info.codec = (uint8_t)media_codec_of(
			(enum sdp_codec_id)c->sdp.committed.codec.id);
		info.ptime_ms = c->sdp.committed.ptime_ms;
		/* an image exchange negotiates no audio codec, so the codec
		 * above stays NONE and this is what names the session */
		info.t38 = c->sdp.committed.t38;
	}
	info.rtp_port = c->rtp_port;
	info.transferred = c->transferred;

	/* the leg's own slot and never the UI one (call.h cb.ended): the two
	 * legs of a transfer wear one name and own two records */
	cv.cb.ended(c->line, c->slot, &info);
}
/* ------------------------------------------------------- session timers */

static void sess_stop(struct call *c)
{
	if (!c->env)
		return;

	sip_timer_cancel(c->env, &c->t_session);
	sip_timer_cancel(c->env, &c->t_watchdog);
}

/*
 * RFC 4028 10 and 10.1. The refresher halves the interval; the other side
 * watches for a refresh and gives the peer until the interval minus the
 * smaller of a third of it and 32 seconds. The floor keeps a peer that
 * negotiated an absurdly small interval from arming a timer of 0 ms.
 */
static void sess_arm(struct call *c)
{
	uint32_t lead, ms;

	sess_stop(c);
	if (!c->sess_interval || !c->env)
		return;

	if (c->we_refresh) {
		ms = c->sess_interval * 1000 / 2;
		sip_timer_arm(c->env, &c->t_session, ms < 1000 ? 1000 : ms);
		leg_trace(c, "session refresh in %u ms", ms < 1000 ? 1000 : ms);
		return;
	}

	lead = c->sess_interval / 3;
	if (lead > CALL_SESS_LEAD_MAX)
		lead = CALL_SESS_LEAD_MAX;
	if (lead >= c->sess_interval)
		lead = c->sess_interval / 2;

	ms = (c->sess_interval - lead) * 1000;
	sip_timer_arm(c->env, &c->t_watchdog, ms < 1000 ? 1000 : ms);
	leg_trace(c, "session watchdog in %u ms", ms < 1000 ? 1000 : ms);
}

/*
 * RFC 4028 4 and 5: the floor is 90 seconds, and a configured minimum only
 * raises it. It applies to every inbound INVITE, initial or in dialog.
 */
static uint32_t sess_min_cfg(const struct call *c)
{
	uint32_t min = c->ch.session_min_interval;

	return min > CALL_SESS_MIN_DEFAULT ? min : CALL_SESS_MIN_DEFAULT;
}

static uint32_t sess_min_of_cfg(const struct cfg_channel *ch)
{
	return ch->session_min_interval > CALL_SESS_MIN_DEFAULT ? ch->session_min_interval :
								  CALL_SESS_MIN_DEFAULT;
}

/* false when the request demands a session shorter than we accept */
static bool sess_min_ok(const struct call *c, const struct sip_msg *req)
{
	enum sip_refresher who = SIP_REFRESHER_NONE;
	uint32_t se = 0;

	if (!hdr_session_expires_get(req, &se, &who) || !se)
		return true;

	return se >= sess_min_cfg(c);
}

static uint32_t sess_clamp(uint32_t secs)
{
	return secs > CALL_SESS_MAX ? CALL_SESS_MAX : secs;
}

/*
 * Who refreshes. An explicit refresher parameter always wins, and it names a
 * side of the transaction that carries it, not of the dialog (RFC 4028 7.2,
 * 7.4): txn_uac says whether this leg sent that request. A re-INVITE from the
 * called side is the case where the two differ.
 *
 * A request without one lets this leg pick (Table 2 row 4). A 2xx without one
 * breaks 7.2, and the same local rule answers it. A configured preference
 * names the caller or the called party. With none, this leg takes the
 * refresh itself.
 */
static bool refresher_is_us(const struct call *c, enum sip_refresher who, bool txn_uac)
{
	if (who == SIP_REFRESHER_UAC)
		return txn_uac;
	if (who == SIP_REFRESHER_UAS)
		return !txn_uac;

	if (c->ch.session_refresher == CFG_REFRESH_UAC)
		return c->uac;
	if (c->ch.session_refresher == CFG_REFRESH_UAS)
		return !c->uac;

	return true;
}

static const char *refresher_param(bool we_refresh, bool txn_uac)
{
	if (we_refresh)
		return txn_uac ? ";refresher=uac" : ";refresher=uas";

	return txn_uac ? ";refresher=uas" : ";refresher=uac";
}

/* RFC 4028 7.1: with no preference, the initial INVITE leaves the choice */
static const char *refresher_offer_param(const struct call *c)
{
	switch (c->ch.session_refresher) {
	case CFG_REFRESH_UAC:
		return ";refresher=uac";
	case CFG_REFRESH_UAS:
		return ";refresher=uas";
	case CFG_REFRESH_UNSPEC:
		break;
	}

	return "";
}

/*
 * A 2xx to an INVITE we sent, either the initial one or a refresh. No
 * Session-Expires means the peer does not run session timers, and RFC 4028 5
 * then leaves the session without one.
 */
static void sess_uac_take(struct call *c, const struct sip_msg *resp)
{
	enum sip_refresher who = SIP_REFRESHER_NONE;
	uint32_t delta = 0;

	if (!hdr_session_expires_get(resp, &delta, &who) || !delta) {
		c->sess_interval = 0;
		sess_stop(c);
		return;
	}

	if (delta < sess_min_cfg(c))
		delta = sess_min_cfg(c);

	c->sess_interval = sess_clamp(delta);
	c->we_refresh = refresher_is_us(c, who, true);
	sess_arm(c);
}

/*
 * RFC 4028 7.4: the leg keeps the largest Min-SE that a 422 or a refresh
 * request of the peer named on the dialog, whether this leg accepted that
 * request or not.
 */
static void sess_min_se_take(struct call *c, const struct sip_msg *msg)
{
	uint32_t min = 0;

	if (hdr_min_se_get(msg, &min) && min > c->sess_min_se)
		c->sess_min_se = min;
}

/*
 * RFC 4028 9: our answer can reduce the interval the request names (asked, 0
 * for none), but never below the Min-SE of the request or our own floor, and
 * never above what the request named. sess_min_se already holds the Min-SE
 * of the request, and the larger ones the dialog saw before it. Two limits
 * break the floor: a request whose Min-SE exceeds its own interval (7.1
 * forbids it) gets that interval back, and the one-day cap of sess_clamp()
 * still bounds every answer.
 */
static uint32_t sess_answer_interval(const struct call *c, uint32_t asked)
{
	uint32_t floor = sess_min_cfg(c), want = c->ch.session_max_expires;

	if (c->sess_min_se > floor)
		floor = c->sess_min_se;

	if (asked && (!want || want > asked))
		want = asked;
	if (want < floor)
		want = floor;
	if (asked && want > asked)
		want = asked;

	return sess_clamp(want);
}

/*
 * The UAS side of the negotiation, read from the INVITE that arrived. It runs
 * before anything is answered, so a Session-Expires below our Min-SE can still
 * become the 422 RFC 4028 6 asks for.
 */
static void sess_uas_take(struct call *c, const struct sip_msg *req)
{
	enum sip_refresher who = SIP_REFRESHER_NONE;
	uint32_t delta = 0;

	sess_min_se_take(c, req);
	c->sess_interval = 0;
	if (hdr_session_expires_get(req, &delta, &who) && delta) {
		c->sess_interval = sess_answer_interval(c, delta);
		/* RFC 4028 9 Table 2: a UAC without the option tag cannot
		 * refresh, whatever it names, so the refresh stays with this
		 * leg, and a uac answer would also need a Require it cannot
		 * honor */
		if (!hdr_token_has(req, SIP_HDR_SUPPORTED, "timer"))
			c->we_refresh = true;
		else
			c->we_refresh = refresher_is_us(c, who, false);
		return;
	}

	/*
	 * The peer asked for nothing. We may still request an interval, but
	 * only from a peer that advertised the option tag (RFC 4028 7.1).
	 */
	if (!c->ch.session_max_expires || !hdr_token_has(req, SIP_HDR_SUPPORTED, "timer"))
		return;

	c->sess_interval = sess_answer_interval(c, 0);
	c->we_refresh = refresher_is_us(c, SIP_REFRESHER_NONE, false);
}

/* ------------------------------------------------------------ wire build */

static void cred_write(struct call *c, struct sip_buf *b, enum sip_auth_kind kind,
		       const char *method, const char *uri)
{
	enum sip_auth_status status;

	status = sip_auth_cred_write(b, c->auth, kind, c->auth_user,
				     c->auth_pass, method, uri);
	if (status != SIP_AUTH_OK)
		leg_trace(c, "%s credentials refused (%s)", sip_auth_kind_name(kind),
			  sip_auth_status_name(status));
}

static bool body_offer_build(struct call *c)
{
	struct sip_buf b;

	sip_buf_init(&b, c->body, sizeof(c->body));
	if (!sdp_offer_build(&c->sdp, &c->loc, &b))
		return false;

	c->body_len = sip_buf_len(&b);

	return true;
}

/*
 * loc is the leg's own view unless the T.38 admission took it away for this one
 * answer (t38_answer_loc), so the answer builder stays the single place a
 * verdict about an offer is made.
 */
static enum sdp_verdict body_answer_build(struct call *c, const struct sdp_local *loc,
					  const struct sdp_msg *offer)
{
	struct sip_buf b;
	enum sdp_verdict v;

	sip_buf_init(&b, c->body, sizeof(c->body));
	v = sdp_answer_build(&c->sdp, loc, offer, &b);
	if (v != SDP_OK)
		return v;

	c->body_len = sip_buf_len(&b);

	return SDP_OK;
}

static bool invite_build(struct call *c, uint32_t cseq)
{
	struct sip_buf b;
	unsigned int k;

	sip_buf_init(&b, c->wire, sizeof(c->wire));

	msg_req_line_write(&b, "INVITE", c->ruri);
	hdr_via_write(&b, c->transport[0] ? c->transport : "UDP", c->local_host, c->local_port,
		      c->branch, true);
	hdr_write(&b, SIP_HDR_MAX_FORWARDS, "70");
	hdr_addr_tag_write(&b, SIP_HDR_FROM, c->display, c->from_uri,
			   c->local_tag);
	hdr_addr_write(&b, SIP_HDR_TO, NULL, c->to_uri, NULL);
	hdr_write(&b, SIP_HDR_CALL_ID, "%s", c->call_id);
	hdr_cseq_write(&b, cseq, "INVITE");
	/* RFC 3608 section 5: the route the registrar granted rides every
	 * request that leaves the dialog, so it rides each build of this one
	 * and not the first alone. The next hop does not move with it
	 * (sip/reg.h): the hop names are never resolved. */
	route_hdrs_write(&b, &c->service_route);
	hdr_addr_write(&b, SIP_HDR_CONTACT, NULL, c->contact, NULL);
	/* The anonymous From alone tells a proxy nothing, so the withheld call
	 * also asks for id privacy. call_dial already replaced the From
	 * identity; the tag above is untouched by that. */
	if (!c->cid_present)
		hdr_write(&b, SIP_HDR_PRIVACY, "%s", CALL_PRIVACY_ID);
	hdr_write(&b, SIP_HDR_ALLOW, "%s", UA_ALLOW_DEFAULT);
	/* sip/ua.h: allow_events rides wherever allow rides, so one answer
	 * never names a method without the package that makes it useful */
	hdr_write(&b, SIP_HDR_ALLOW_EVENTS, "%s", UA_ALLOW_EVENTS_DEFAULT);
	hdr_write(&b, SIP_HDR_SUPPORTED, "%s", UA_SUPPORTED_DEFAULT);
	/* the Replaces block of a transfer's child (CALL_XFER_HDRS_MAX), which
	 * rides every build of this request and not only the first: the
	 * credential retry builds it again */
	sip_buf_add(&b, c->xfer_hdrs);

	if (c->sess_want)
		hdr_write(&b, SIP_HDR_SESSION_EXPIRES, "%u%s", c->sess_want,
			  refresher_offer_param(c));
	if (c->sess_min_se)
		hdr_write(&b, SIP_HDR_MIN_SE, "%u", c->sess_min_se);
	if (c->user_agent[0])
		hdr_write(&b, SIP_HDR_USER_AGENT, "%s", c->user_agent);

	for (k = 0; k < SIP_AUTH_KIND_MAX; k++)
		cred_write(c, &b, (enum sip_auth_kind)k, "INVITE", c->ruri);

	msg_body_write(&b, "application/sdp", c->body, c->body_len);
	if (!sip_buf_ok(&b))
		return false;

	c->wire_len = sip_buf_len(&b);

	return true;
}

/*
 * The ACK to a non-2xx final (RFC 3261 17.1.1.3) and the CANCEL of an INVITE
 * (9.1) are the same shape: the request as sent, with one method swapped in
 * and one To header taken from somewhere else. Both must carry the original
 * Via branch and the original Route set, so both are built from the request
 * the transaction retained rather than from anything this layer remembers.
 *
 * to_src supplies the To header: the response for an ACK, because the tag the
 * peer chose belongs in it, and the request itself for a CANCEL, which copies
 * the To exactly as it was sent.
 *
 * sip_txn_request_get() reparses the retained request in place. It only undoes
 * header folding, and nothing this module builds is folded, so a transaction
 * that is still retransmitting keeps sending the same bytes.
 */
static bool req_mirror_build(struct call *c, struct sip_txn *txn, const char *method,
			     const struct sip_msg *to_src, uint32_t *cseq_out)
{
	char ruri[CALL_URI_MAX];
	struct sip_val_iter it;
	struct sip_msg req;
	struct sip_buf b;
	struct sip_via via;
	struct sip_str s;
	enum sip_method m;
	uint32_t cseq = 0;

	if (!sip_txn_request_get(txn, &req))
		return false;
	if (!sip_str_copy(ruri, sizeof(ruri), req.ruri))
		return false;
	if (!hdr_cseq_get(&req, &cseq, &m) || !hdr_via_top(&req, &via))
		return false;

	sip_buf_init(&b, c->wire, sizeof(c->wire));
	msg_req_line_write(&b, method, ruri);
	hdr_write_str(&b, SIP_HDR_VIA, via.raw);
	hdr_write(&b, SIP_HDR_MAX_FORWARDS, "70");

	if (msg_hdr_value(&req, SIP_HDR_FROM, &s))
		hdr_write_str(&b, SIP_HDR_FROM, s);
	if (msg_hdr_value(to_src, SIP_HDR_TO, &s))
		hdr_write_str(&b, SIP_HDR_TO, s);
	if (msg_hdr_value(&req, SIP_HDR_CALL_ID, &s))
		hdr_write_str(&b, SIP_HDR_CALL_ID, s);
	hdr_cseq_write(&b, cseq, method);

	if (hdr_val_first(&req, SIP_HDR_ROUTE, &it, &s)) {
		do {
			hdr_write_str(&b, SIP_HDR_ROUTE, s);
		} while (hdr_val_next(&it, &s));
	}

	/* RFC 3261 22.1: the ACK repeats the credentials of the request it
	 * acknowledges, and a CANCEL carries those of its INVITE (9.1). */
	if (msg_hdr_value(&req, SIP_HDR_AUTHORIZATION, &s))
		hdr_write_str(&b, SIP_HDR_AUTHORIZATION, s);
	if (msg_hdr_value(&req, SIP_HDR_PROXY_AUTHORIZATION, &s))
		hdr_write_str(&b, SIP_HDR_PROXY_AUTHORIZATION, s);

	if (c->user_agent[0])
		hdr_write(&b, SIP_HDR_USER_AGENT, "%s", c->user_agent);

	msg_body_write(&b, NULL, NULL, 0);
	if (!sip_buf_ok(&b))
		return false;

	c->wire_len = sip_buf_len(&b);
	if (cseq_out)
		*cseq_out = cseq;

	return true;
}

/* The ACK a client INVITE transaction owes for a final it just delivered. */
static void ack_nonok_set(struct call *c, struct sip_txn *txn, const struct sip_msg *resp)
{
	if (!req_mirror_build(c, txn, "ACK", resp, NULL))
		return;

	sip_txn_ack_set(txn, c->wire, c->wire_len);
}

/*
 * RFC 3261 9.1: the CANCEL shares the branch of the INVITE, so the two are one
 * transaction to a proxy and two here (the match key carries the method).
 *
 * The branch comes from the INVITE client transaction, not from c->branch.
 * c->branch holds the last request this leg built, and a PRACK before the
 * hangup drew its own branch into it. The mirrored Via below carries the
 * INVITE branch in either case. Keyed on the other branch, the peer's 200 to
 * the CANCEL matches no client transaction, and the CANCEL retransmits until
 * it times out.
 */
static bool cancel_send(struct call *c)
{
	struct sip_txn *txn = leg_txn(c, c->invite_txn);
	struct sip_msg req;
	struct sip_txn *ct;

	if (!txn)
		return false;
	if (!sip_txn_request_get(txn, &req))
		return false;
	if (!req_mirror_build(c, txn, "CANCEL", &req, NULL))
		return false;

	ct = sip_txn_client_start(c->mgr, SIP_METHOD_CANCEL,
				  sip_str_cstr(sip_txn_branch(txn)), c->wire,
				  c->wire_len, &c->dst, leg_owner(c));
	if (!ct)
		return false;

	c->cancel_txn = sip_txn_handle(ct);
	c->cancel_wanted = false;
	c->clearing = true;

	/* The setup is over: a second timer C expiry must not build another
	 * CANCEL, whose failure would abort the INVITE the 487 still needs. */
	sip_timer_cancel(c->env, &c->t_c);
	leg_trace(c, "CANCEL sent");

	return true;
}

/* --------------------------------------------------------- in dialog out */

/*
 * The headers an in-dialog request needs that the dialog layer does not write:
 * the RFC 4028 session headers of a refresh, the caller's own block (the
 * Refer-To and Referred-By of a REFER) and the cached credentials.
 * sip_dialog_req_build() owns the header block and closes it with
 * Content-Length, so they are spliced in front of that one header rather than
 * duplicating the route set and strict-router rules of the dialog layer here.
 * Content-Length is written exactly once, by msg_body_write(), and no stored
 * header value can carry a CRLF (the parser unfolds), so the insertion point is
 * unambiguous.
 *
 * The buffer refuses rather than truncates (call.h CALL_WIRE_EXTRA_MAX). A
 * block that does not fit makes this return false, so
 * the request never reaches the wire, and both refusals say so: a half written
 * Refer-To would transfer the call to whatever the truncation left.
 */
static bool wire_extra_insert(struct call *c, enum sip_method method, const char *uri,
			      const char *extra_hdrs)
{
	char extra[CALL_WIRE_EXTRA_MAX];
	const char *name = sip_method_name(method);
	struct sip_buf b;
	unsigned int k;
	char *at;
	size_t head, tail, len;

	sip_buf_init(&b, extra, sizeof(extra));

	/* RFC 4028 7.1: each INVITE lists the timer option tag, a re-INVITE
	 * too (sip/ua.h UA_SUPPORTED_REINVITE); the other in-dialog requests
	 * carry no Supported */
	if (method == SIP_METHOD_INVITE)
		hdr_write(&b, SIP_HDR_SUPPORTED, "%s", UA_SUPPORTED_REINVITE);

	/* RFC 4028 7.4: every refresh request carries the interval in force,
	 * raised to the Min-SE of the dialog, and this leg is the UAC of every
	 * request built here */
	if (method == SIP_METHOD_INVITE && c->sess_interval) {
		hdr_write(&b, SIP_HDR_SESSION_EXPIRES, "%u%s",
			  c->sess_interval > c->sess_min_se ? c->sess_interval : c->sess_min_se,
			  refresher_param(c->we_refresh, true));
		if (c->sess_min_se)
			hdr_write(&b, SIP_HDR_MIN_SE, "%u", c->sess_min_se);
	}

	sip_buf_add(&b, extra_hdrs ? extra_hdrs : "");

	for (k = 0; k < SIP_AUTH_KIND_MAX; k++)
		cred_write(c, &b, (enum sip_auth_kind)k, name, uri);

	if (!sip_buf_ok(&b)) {
		leg_trace(c, "the %s header block does not fit, the request is refused", name);
		return false;
	}

	len = sip_buf_len(&b);
	if (!len)
		return true;

	at = strstr(c->wire, "\r\nContent-Length:");
	if (!at || c->wire_len + len >= sizeof(c->wire)) {
		leg_trace(c, "the %s request does not fit the wire buffer, it is refused", name);
		return false;
	}

	head = (size_t)(at - c->wire) + 2;
	tail = c->wire_len - head;
	memmove(c->wire + head + len, c->wire + head, tail);
	memcpy(c->wire + head, extra, len);
	c->wire_len += len;

	return true;
}

/*
 * extra_hdrs is a whole CRLF terminated header block the caller owns, or NULL.
 * The REFER of a transfer carries one, and so does the PRACK with its RAck
 * (RFC 3262 7.2); every other in-dialog request this module sends needs
 * nothing the dialog layer and the block above do not write.
 */
static bool dialog_req_send(struct call *c, enum sip_method method, const char *ctype,
			    const char *body, size_t body_len, bool contact, uint32_t *txn_out,
			    const char *extra_hdrs)
{
	struct sip_dialog *d = leg_dialog(c);
	struct sip_dialog_req r;
	struct sip_txn *txn;
	struct sip_buf b;
	char ruri[CALL_URI_MAX];
	struct sip_msg built;

	if (!d)
		return false;
	if (!sip_branch_new(c->mgr, c->branch, sizeof(c->branch)))
		return false;

	memset(&r, 0, sizeof(r));
	r.method = method;
	r.branch = c->branch;
	r.ctype = ctype;
	r.body = body;
	r.body_len = body_len;
	r.with_contact = contact;

	sip_buf_init(&b, c->wire, sizeof(c->wire));
	if (!sip_dialog_req_build(d, &r, &b))
		return false;

	c->wire_len = sip_buf_len(&b);
	/* the number the answer carries, and the id of the implicit
	 * subscription a REFER creates (RFC 3515 2.4.4) */
	c->dlg_cseq = r.cseq;

	/*
	 * An authenticated proxy challenges the requests inside the dialog too,
	 * so a cached challenge is answered on every one of them. The request
	 * URI the digest covers is the one the dialog layer just built.
	 */
	if (msg_parse(&built, c->wire, c->wire_len) == SIP_PARSE_OK &&
	    sip_str_copy(ruri, sizeof(ruri), built.ruri)) {
		if (!wire_extra_insert(c, method, ruri, extra_hdrs))
			return false;
	} else if (extra_hdrs) {
		/*
		 * A cached credential the request could not be read back for is
		 * left off and the peer challenges again, which every caller
		 * without a block relies on. A caller's block is not
		 * optional that way: a REFER with no Refer-To names no target
		 * at all (RFC 3515 2.4.1), so it is refused instead.
		 */
		leg_trace(c, "the built request could not be read back, so its extra "
			  "headers cannot ride and it is refused");
		return false;
	}

	txn = sip_txn_client_start(c->mgr, method, sip_str_cstr(c->branch), c->wire, c->wire_len,
				   sip_dialog_dst(d), leg_owner(c));
	if (!txn)
		return false;

	if (txn_out)
		*txn_out = sip_txn_handle(txn);

	return true;
}

/*
 * Kill a transaction we no longer want an answer from. The handle is cleared
 * first: sip_txn_abort() terminates the transaction, which calls done() before
 * it returns, and a done() that still matched the handle would release the leg
 * under the caller's feet.
 */
static void txn_abort(struct call *c, uint32_t *handle)
{
	struct sip_txn *txn = leg_txn(c, *handle);

	*handle = SIP_HANDLE_NONE;
	if (txn)
		sip_txn_abort(txn);
}

/* the leg is on its way out: no outcome of a request still in flight counts */
static bool leg_clearing(const struct call *c)
{
	return c->clearing || c->cancel_wanted || c->state == CALL_ST_CANCEL_SENT ||
	       c->state == CALL_ST_BYE_SENT;
}

static bool bye_send(struct call *c)
{
	clear_stamp(c);
	media_stop(c);
	sess_stop(c);
	/*
	 * A leg that is being cleared voids its pending transfer, which is the
	 * call.h voiding rule applied to the local half: a hangup or a session
	 * death during the wait ends the leg through the ordinary path, and an
	 * outcome reported after it would name a slot the FSM has left. The
	 * paths that did decide a transfer take the outcome first, so this
	 * clear finds nothing left to drop.
	 */
	xfer_state_clear(c);
	/* a BYE of our own is the end the replaced-leg guard was waiting for */
	if (c->env)
		sip_timer_cancel(c->env, &c->t_bye);
	/* the leg ends here, so the operation that waited for its 491 retry
	 * owes the machine no report: the clearing chain carries the outcome */
	glare_wait_drop(c);
	c->clearing = true;
	memset(c->dlg_auth_retry, 0, sizeof(c->dlg_auth_retry));

	if (!dialog_req_send(c, SIP_METHOD_BYE, NULL, NULL, 0, false, &c->bye_txn, NULL)) {
		leg_trace(c, "BYE could not be sent");
		return false;
	}

	state_set(c, CALL_ST_BYE_SENT);
	leg_trace(c, "BYE sent");

	return true;
}

/* ------------------------------------------------------------ leg endings */

static void leg_end(struct call *c)
{
	c->ui_busy = false;
	leg_detach(c);
	leg_release(c);
}

/* The far end is gone and nothing is outstanding: report and release. */
static void leg_fail(struct call *c, enum call_fail_class cls)
{
	cb_failed(c, cls);
	leg_end(c);
}

/* The session died mid call: BYE, report quiet, drain the BYE transaction. */
static void leg_abandon(struct call *c, enum call_fail_class cls)
{
	cb_failed(c, cls);
	/*
	 * The report can clear the leg itself: the machine hangs a failed call
	 * up at once (fsm.c connected_failed()), and that hangup sends the BYE
	 * or releases the leg. A dialog takes one BYE, so a leg that is
	 * released or already clearing gets no second one.
	 */
	if (!c->in_use || c->state == CALL_ST_BYE_SENT || c->state == CALL_ST_CANCEL_SENT)
		return;

	c->ui_busy = false;
	if (!bye_send(c))
		leg_end(c);
}

/*
 * What one re-program of the committed description did. Three answers, because
 * the callers spend them differently: OK carries on, REFUSED is the caller's to
 * roll back, and ABANDONED says the leg is gone and nothing may touch it again.
 */
enum media_push {
	MEDIA_PUSH_OK = 0,
	MEDIA_PUSH_REFUSED,
	MEDIA_PUSH_ABANDONED,
};

/*
 * The push every caller of the committed description makes, with the rule the
 * commit callers already apply to a refusal (uas_ack_take, rx_reinvite,
 * uac_reinvite_ok): a stream this push stopped for a mode move or a rekey and
 * then could not configure or start cannot carry what the session agreed, so
 * the leg ends as a congestion abandon rather than stay active with its media
 * down. A refusal that left the stream running changes nothing on the wire and
 * is the caller's own to undo.
 *
 * These pushes carry no offer, so start stays false: media_program() restarts a
 * stream it stopped itself and leaves a stream another path stopped alone.
 */
static enum media_push media_commit_push(struct call *c)
{
	bool was_started = c->media_started;

	if (media_program(c, &c->sdp.committed, false))
		return MEDIA_PUSH_OK;

	if (!was_started || c->media_started)
		return MEDIA_PUSH_REFUSED;

	leg_abandon(c, CALL_FAIL_CONGESTION);

	return MEDIA_PUSH_ABANDONED;
}

/* ----------------------------------------------------------- timer fires */

static void timer_c_fire(struct sip_timer *t)
{
	struct call *c = t->owner;

	leg_trace(c, "timer C expired");

	/*
	 * RFC 3261 9.1 forbids a CANCEL before the first provisional, so an
	 * INVITE that never drew one is abandoned by killing its transaction
	 * instead. Either way the setup is over and the class is congestion.
	 */
	if (c->prov_seen && cancel_send(c)) {
		cb_failed(c, CALL_FAIL_CONGESTION);
		c->ui_busy = false;
		state_set(c, CALL_ST_CANCEL_SENT);
		return;
	}

	txn_abort(c, &c->invite_txn);
	leg_fail(c, CALL_FAIL_CONGESTION);
}

/* Build the offer and send the re-INVITE on c->reinvite_txn. On failure it
 * rolls the media description back and returns false; the caller restores any
 * state it layered on the offer and decides how the leg proceeds. */
static bool reinvite_offer_send(struct call *c)
{
	if (body_offer_build(c) &&
	    dialog_req_send(c, SIP_METHOD_INVITE, "application/sdp", c->body,
			    c->body_len, true, &c->reinvite_txn, NULL))
		return true;

	sdp_rollback(&c->sdp);

	return false;
}

/*
 * What every re-INVITE of ours starts from: a credential budget and a 422
 * budget of its own, and the hold and T.38 state that a 491, a failure or a
 * clearing leg puts back. Both snapshots are taken for every exchange and not
 * for the operation that moves them alone, because a hold, a resume or a
 * refresh made while the session already carries T.38 has to leave the flag
 * the way it found it, and a 491 that re-offered audio there would drop the
 * image session the peer is still running.
 */
static void exch_start_take(struct call *c)
{
	memset(c->dlg_auth_retry, 0, sizeof(c->dlg_auth_retry));
	c->min_se_run = 0;
	c->hold_prev = sdp_local_hold(&c->sdp);
	c->t38_prev = c->sdp.local_t38;
}

/*
 * The RFC 4028 10 refresh as a request of its own. It has two entries: the
 * timer below opens a fresh 491 budget in front of it, and glare_wait_fire()
 * enters here with the budget its own exchange already spent, so a retry of
 * one refresh stays bounded.
 */
static void session_refresh_send(struct call *c)
{
	leg_trace(c, "session refresh");
	exch_start_take(c);
	if (!reinvite_offer_send(c)) {
		leg_abandon(c, CALL_FAIL_QUIET);
		return;
	}

	c->op = CALL_OP_REFRESH;
	state_set(c, CALL_ST_REINVITE_SENT);
}

static void timer_session_fire(struct sip_timer *t)
{
	struct call *c = t->owner;

	if (!leg_free_is(c)) {
		/* A refresh cannot ride an exchange that is already running or
		 * a retry that waits for one; the accepted exchange re-arms
		 * both timers when it completes. */
		sess_arm(c);
		return;
	}

	/* The refresh is an exchange of its own, so it opens a budget of its
	 * own: a hold that spent both retries earlier in the call must not
	 * leave the first 491 of a refresh with no retry and clear the call. */
	c->glare_run = 0;
	session_refresh_send(c);
}

static void timer_watchdog_fire(struct sip_timer *t)
{
	struct call *c = t->owner;

	leg_trace(c, "session watchdog expired, peer stopped refreshing");
	leg_abandon(c, CALL_FAIL_QUIET);
}

static void timer_xfer_fire(struct sip_timer *t);
static void timer_bye_fire(struct sip_timer *t);
static void timer_rel_fire(struct sip_timer *t);

static void leg_timers_init(struct call *c)
{
	sip_timer_init(&c->t_c, c, SIP_TIMER_CALL_C, timer_c_fire);
	sip_timer_init(&c->t_session, c, SIP_TIMER_CALL_SESSION, timer_session_fire);
	sip_timer_init(&c->t_watchdog, c, SIP_TIMER_CALL_WATCHDOG, timer_watchdog_fire);
	sip_timer_init(&c->t_xfer, c, SIP_TIMER_CALL_C, timer_xfer_fire);
	sip_timer_init(&c->t_bye, c, SIP_TIMER_CALL_C, timer_bye_fire);
	sip_timer_init(&c->t_rel, c, SIP_TIMER_CALL_REL, timer_rel_fire);
}

static void timer_c_arm(struct call *c)
{
	unsigned int ms = c->ch.timer_c_ms ? c->ch.timer_c_ms : CALL_TIMER_C_DEFAULT;

	sip_timer_arm(c->env, &c->t_c, ms);
}

/* ------------------------------------------------------------- UAC paths */

/*
 * sip_dialog_uac_update() returns a different dialog when a 2xx carries a To
 * tag the early dialog did not have, and it has already terminated the old
 * one. Only our reference is left to drop (sip/dialog.h).
 */
static void dialog_adopt(struct call *c, struct sip_dialog *d)
{
	uint32_t h = sip_dialog_handle(d);
	struct sip_dialog *old;

	if (c->dialog == h)
		return;

	old = leg_dialog(c);
	if (old)
		sip_dialog_put(old);

	c->dialog = h;
	leg_trace(c, "dialog %08x adopted", h);
}

static bool ack_2xx_send(struct call *c, const struct sip_msg *resp, const char *ctype,
			 const char *body, size_t len)
{
	struct sip_dialog *d = leg_dialog(c);
	char branch[SIP_BRANCH_MAX];

	if (!d || !sip_branch_new(c->mgr, branch, sizeof(branch)))
		return false;

	return sip_dialog_ack_send(d, resp, branch, ctype, body, len);
}

static bool invite_send(struct call *c)
{
	struct sip_txn *txn;

	if (!sip_branch_new(c->mgr, c->branch, sizeof(c->branch)))
		return false;
	if (!body_offer_build(c) || !invite_build(c, c->cseq))
		return false;

	txn = sip_txn_client_start(c->mgr, SIP_METHOD_INVITE, sip_str_cstr(c->branch), c->wire,
				   c->wire_len, &c->dst, leg_owner(c));
	if (!txn)
		return false;

	c->invite_txn = sip_txn_handle(txn);
	c->prov_seen = false;
	timer_c_arm(c);
	state_set(c, CALL_ST_INVITE_SENT);
	leg_trace(c, "INVITE cseq %u sent", c->cseq);

	return true;
}

/* A retry inside one exchange: same Call-ID and From tag, a new CSeq. */
static bool invite_resend(struct call *c)
{
	c->cseq++;
	sdp_rollback(&c->sdp);
	/*
	 * RFC 3262 3 gives every INVITE its own RSeq space, and this is a new
	 * INVITE. The handle goes with the record. The RAck of a PRACK of the
	 * exchange that ends here names the CSeq of that exchange, which is no
	 * longer the CSeq of the INVITE in flight, so a late answer to it
	 * draws no retry (call_txn_resp).
	 */
	c->prack_rseq = 0;
	c->prack_txn = SIP_HANDLE_NONE;
	c->early_final = false;

	return invite_send(c);
}

/*
 * RFC 3261 22. One retry per challenge kind, exactly the reg.c rule: a 401 and
 * a 407 in sequence are two exchanges, not two attempts at one password.
 */
static bool auth_challenge_take(struct call *c, const struct sip_msg *msg, unsigned int *retry)
{
	enum sip_auth_status status[SIP_AUTH_KIND_MAX];
	unsigned int k;
	bool resend;

	resend = sip_auth_challenges_take(c->auth, msg, retry, sip_txn_rand,
					  c->mgr, status);

	for (k = 0; k < SIP_AUTH_KIND_MAX; k++) {
		if (status[k] != SIP_AUTH_OK && status[k] != SIP_AUTH_ERR_NONE)
			leg_trace(c, "%s challenge refused (%s)",
				  sip_auth_kind_name((enum sip_auth_kind)k),
				  sip_auth_status_name(status[k]));
	}

	return resend;
}

static bool uac_auth_take(struct call *c, const struct sip_msg *msg)
{
	return auth_challenge_take(c, msg, c->auth_retry) && invite_resend(c);
}

/*
 * The same rule for a request inside the dialog. The counter is its own: a
 * proxy that challenges the BYE, or the PRACK that rides the same dialog while
 * the INVITE is still in flight, is not a second attempt at the password the
 * INVITE offers.
 */
static bool dlg_auth_take(struct call *c, const struct sip_msg *msg)
{
	return auth_challenge_take(c, msg, c->dlg_auth_retry);
}

/*
 * RFC 4028 6: a 422 is answered with one retry whose Session-Expires is at
 * least the Min-SE the peer named, and whose Min-SE echoes that value.
 *
 * The retry does not depend on the Min-SE being larger than what we offered.
 * A peer that refuses 1800 with a Min-SE of 900 is odd, but it occurs (the
 * uas_session_timer_422 scenario does exactly that), and the RFC answer is
 * still a retry: the interval simply stays where it was. The retry bound, not
 * the comparison, is what stops a loop.
 */
static uint32_t min_se_want(uint32_t offer, const struct sip_msg *msg, uint32_t *min_out)
{
	uint32_t min = 0;

	if (!hdr_min_se_get(msg, &min))
		min = 0;

	*min_out = min;

	return min > offer ? min : offer;
}

static bool uac_min_se_take(struct call *c, const struct sip_msg *msg)
{
	uint32_t min = 0, want;

	if (c->min_se_run >= CALL_MIN_SE_MAX)
		return false;

	want = min_se_want(c->sess_want, msg, &min);

	c->min_se_run++;
	sess_min_se_take(c, msg);
	c->sess_want = want;
	c->op = CALL_OP_MIN_SE;
	leg_trace(c, "422 with Min-SE %u, asking for a %u s session", min, want);

	return invite_resend(c);
}

static bool uac_glare_take(struct call *c)
{
	uint32_t delay = 0;

	if (c->glare_run >= CALL_GLARE_MAX)
		return false;

	c->glare_run++;
	sdp_rollback(&c->sdp);
	sdp_hold_set(&c->sdp, c->hold_prev);
	sdp_t38_set(&c->sdp, c->t38_prev);

	/* RFC 3261 14.1: the side that owns the Call-ID takes the long range,
	 * and for uvoice that is the side that placed the call. */
	if (!ua_glare_arm(c->ua, leg_owner(c), c->uac, &delay))
		return false;

	leg_trace(c, "491 glare, retry in %u ms", delay);

	return true;
}

static bool early_media_try(struct call *c, const struct sip_msg *msg, bool rel)
{
	struct sdp_msg ans;
	struct sip_str body;
	bool early;
	bool ok;

	if (!c->sdp.offer_pending) {
		/*
		 * RFC 3261 13.2.1: the first answer stands. A reliable copy of
		 * an answer an unreliable 18x already carried closes the
		 * exchange for good (RFC 3262 5). The 2xx may then arrive
		 * without a body. Without this mark, the reopen rule of
		 * uac_ok() fails the whole call on that bodiless 2xx.
		 */
		if (rel && c->early_answered && msg_body_sdp_get(msg, &body)) {
			c->early_final = true;
			leg_trace(c, "reliable 1xx answer taken");
		}
		return false;
	}
	/*
	 * The child of a transfer stays out of early media. Its proceeding
	 * report is suppressed already, so the subscriber hears silence until
	 * the swap (uac_prov()), and nothing holds the parent, so m.tx_silence
	 * is clear in media_program(). An early stream of the child would
	 * therefore carry the subscriber's live audio to the transfer target
	 * before that target answers.
	 */
	early = c->ch.early_media_enabled && !c->xfer_child;

	/* the answer of a reliable 1xx is taken at either setting of the
	 * option, because it closes offer and answer; the option governs the
	 * early stream below */
	if (!rel && !early)
		return false;
	if (!msg_body_sdp_get(msg, &body))
		return false;

	/* the parsed copy dies here, so its crypto attributes go with it */
	ok = sdp_parse(body, &ans) &&
	     sdp_answer_take(&c->sdp, &c->loc, &ans) == SDP_OK;
	sdp_msg_keys_wipe(&ans);
	if (!ok)
		return false;

	if (rel) {
		c->early_final = true;		/* RFC 3261 13.2.1 */
		leg_trace(c, "reliable 1xx answer taken");
	}
	if (!early)
		return false;

	/*
	 * The stream runs in the direction the answer negotiated (RFC 3264 6.1,
	 * dir_answer() in sdp.c), so under a sendrecv answer this side sends
	 * from the 18x on. A far end that locates our media by the packets it
	 * receives cannot deliver early media before that first packet, and the
	 * RFC 6263 punch is not one: an empty datagram is discarded on the RTP
	 * minimum length.
	 */
	c->early_answered = true;

	return media_program(c, &c->sdp.proposed, true);
}

static bool prack_send(struct call *c, uint32_t rseq, uint32_t cseq)
{
	char hdrs[CALL_RACK_HDRS_MAX];
	struct sip_buf b;

	/* RFC 3262 7.2: the RSeq acknowledged, then the CSeq number and method
	 * of the response that carried it */
	sip_buf_init(&b, hdrs, sizeof(hdrs));
	if (!hdr_rack_write(&b, rseq, cseq, "INVITE"))
		return false;

	/* an ordinary in-dialog request (RFC 3262 4), so the cached credentials
	 * ride it as they ride the BYE, and it carries no Contact */
	return dialog_req_send(c, SIP_METHOD_PRACK, NULL, NULL, 0, false,
			       &c->prack_txn, hdrs);
}

static void uac_prov(struct call *c, const struct sip_msg *msg)
{
	struct sip_dialog *d;
	uint32_t rseq = 0, cseq = 0;
	bool early, rel;

	c->prov_seen = true;

	/* RFC 3261 9.1 held the CANCEL back until a provisional arrived. */
	if (c->cancel_wanted) {
		if (cancel_send(c))
			state_set(c, CALL_ST_CANCEL_SENT);
		else
			leg_end(c);
		return;
	}

	/* A leg that is clearing rings nothing and starts no media. It sends
	 * no PRACK either: the two arms above own the exchange from here, and
	 * the peer's own 487 ends its retransmission of the 1xx. */
	if (leg_clearing(c) || msg->status <= 100)
		return;

	rel = hdr_token_has(msg, SIP_HDR_REQUIRE, "100rel");
	if (rel && !hdr_rseq_get(msg, &rseq)) {
		leg_trace(c,
			"reliable 1xx without a usable RSeq, treated as plain");
		rel = false;
	}
	/* RFC 3262 4: a retransmitted or out-of-order reliable response is not
	 * processed further, so neither may refresh the dialog target below */
	if (rel && c->prack_rseq && rseq <= c->prack_rseq) {
		leg_trace(c, "retransmitted reliable 1xx RSeq %u discarded",
			  rseq);
		return;
	}
	if (rel && c->prack_rseq && rseq != c->prack_rseq + 1) {
		leg_trace(c, "reliable 1xx RSeq %u out of order, ignored",
			  rseq);
		return;
	}

	d = sip_dialog_uac_update(c->dialogs, msg, &c->dst, &c->dlocal);
	if (d)
		dialog_adopt(c, d);

	/* RFC 3262 4 makes the peer establish the dialog before it sends one,
	 * and there is no PRACK to send without it */
	if (rel && !d) {
		leg_trace(c, "reliable 1xx without a dialog, treated as plain");
		rel = false;
	}
	/*
	 * The PRACK goes out in front of the report below. cb.proceeding()
	 * reaches the line shell, which runs the FSM inside this frame.
	 * Nothing on this leg may follow
	 * that call here.
	 */
	if (rel && hdr_cseq_get(msg, &cseq, NULL)) {
		/* the record moves only for a PRACK that left, so a
		 * retransmission of this 1xx draws another attempt */
		if (prack_send(c, rseq, cseq)) {
			c->prack_rseq = rseq;
			leg_trace(c, "reliable 1xx RSeq %u, PRACK sent", rseq);
		} else {
			leg_trace(c, "reliable 1xx RSeq %u, PRACK not sent",
				  rseq);
		}
	}

	early = early_media_try(c, msg, rel);
	state_set(c, CALL_ST_PROCEEDING);
	/* No interim relay from the child of a transfer: the 180 of the target
	 * is progress on an exchange the FSM must not learn about, and the
	 * subscriber hears silence until the swap */
	if (!c->xfer_child && ui_emits(c) && cv.cb.proceeding)
		cv.cb.proceeding(c->line, c->ui_slot, early);
}

/*
 * A 2xx that carries a body behind a reliable 1xx answer. RFC 3262 5 with
 * RFC 3261 13.2.1: the answer of the 1xx is the answer of the exchange, so
 * the body of the 2xx says nothing new and is reported rather than taken.
 */
static void rel_body_note(struct call *c, const struct sip_msg *msg)
{
	struct sip_str body;

	if (!c->early_final || !msg_body_sdp_get(msg, &body))
		return;

	leg_trace(c, "2xx body ignored, the reliable 1xx carried the answer");
}

/* the transferee halves the uac success and failure paths reach (call.h) */
static void xfer_child_promote(struct call *c);
static void xfer_child_settle(struct call *c);
static void xfer_child_failed(struct call *c, uint32_t code);
/* the same swap on the target side, which the ACK of our own 2xx reaches */
static void replaces_takeover(struct call *c);

static void uac_ok(struct call *c, const struct sip_msg *msg)
{
	struct sip_dialog *d;
	struct sdp_msg ans;
	struct sip_str body;
	enum sdp_verdict v;

	c->invite_txn = SIP_HANDLE_NONE;
	sip_timer_cancel(c->env, &c->t_c);

	d = sip_dialog_uac_update(c->dialogs, msg, &c->dst, &c->dlocal);
	if (!d) {
		leg_trace(c, "2xx without a usable dialog");
		xfer_child_failed(c, 503);
		leg_fail(c, CALL_FAIL_CONGESTION);
		return;
	}
	dialog_adopt(c, d);

	/*
	 * The 200 raced our CANCEL (RFC 3261 9.1 leaves this open), or beat the
	 * CANCEL that a hangup before the first provisional had not released
	 * yet. Either way the user is gone: the call the 2xx created exists on
	 * the far end, so it is ACKed and cleared at once and no event is owed.
	 */
	if (leg_clearing(c)) {
		leg_trace(c, "2xx for a leg that is clearing");
		ack_2xx_send(c, msg, NULL, NULL, 0);
		xfer_child_failed(c, 487);
		if (!bye_send(c))
			leg_end(c);
		return;
	}

	/*
	 * RFC 3264: one answer per offer, and sdp.c enforces it. An answer in
	 * an unreliable 18x is provisional, so the offer is reopened for the
	 * final one; ignoring the 2xx body instead would leave media on an
	 * early description the peer may have changed. An answer in a reliable
	 * 1xx completed the exchange (RFC 3262 5, RFC 3261 13.2.1): the 2xx
	 * body is ignored and a 2xx with no body is whole.
	 */
	if (c->early_answered && !c->early_final && !c->sdp.offer_pending)
		c->sdp.offer_pending = true;

	rel_body_note(c, msg);

	if (!c->early_final) {
		if (!msg_body_sdp_get(msg, &body) || !sdp_parse(body, &ans)) {
			sdp_msg_keys_wipe(&ans);
			leg_trace(c, "2xx without a usable answer");
			ack_2xx_send(c, msg, NULL, NULL, 0);
			xfer_child_failed(c, 488);
			leg_abandon(c, CALL_FAIL_QUIET);
			return;
		}

		v = sdp_answer_take(&c->sdp, &c->loc, &ans);
		sdp_msg_keys_wipe(&ans);
		if (v != SDP_OK) {
			/* SDP_NO_CODEC covers the address, the port and the
			 * transport as well as the codec list, so the trace
			 * names the class */
			leg_trace(c, "answer refused (%s)",
				  v == SDP_NO_CODEC ?
				  "no usable codec or address" : "bad");
			ack_2xx_send(c, msg, NULL, NULL, 0);
			xfer_child_failed(c, 488);
			leg_abandon(c, v == SDP_NO_CODEC ?
				    CALL_FAIL_CONGESTION : CALL_FAIL_QUIET);
			return;
		}
	}

	/*
	 * sip_dialog_ack_send() retains the bytes before it sends them, so a
	 * transport error is recoverable: the peer retransmits its 2xx and the
	 * dialog layer re-drives the same ACK (RFC 6026). Clearing the call
	 * here would throw that recovery away.
	 */
	if (!ack_2xx_send(c, msg, NULL, NULL, 0) && !sip_dialog_ack_stored(d)) {
		/* No retained bytes at all: the dialog layer cannot answer a
		 * retransmitted 2xx, so the peer would never see an ACK. */
		leg_trace(c, "the ACK could not be built, clearing");
		xfer_child_failed(c, 503);
		leg_abandon(c, CALL_FAIL_QUIET);
		return;
	}

	leg_commit_take(c);

	/*
	 * The swap runs in front of the stream. The stock path below starts
	 * media and then reports, so a child that programmed its stream first
	 * would put two streams on one line; the parent's goes down inside the
	 * promotion instead. The UI slot moves in the same step, which is what
	 * makes the rollback below defined.
	 */
	if (c->xfer_child)
		xfer_child_promote(c);

	if (!media_program(c, &c->sdp.committed, true)) {
		xfer_child_failed(c, 503);
		leg_abandon(c, CALL_FAIL_CONGESTION);
		return;
	}

	sess_uac_take(c, msg);
	state_set(c, CALL_ST_ACTIVE);
	cb_answered(c);

	if (c->xfer_child)
		xfer_child_settle(c);
}

static void uac_invite_final(struct call *c, struct sip_txn *txn, const struct sip_msg *msg)
{
	uint32_t code = msg->status;

	/* The ACK for a non-2xx belongs to this upcall (sip/txn.h resp()). */
	ack_nonok_set(c, txn, msg);
	c->invite_txn = SIP_HANDLE_NONE;
	sip_timer_cancel(c->env, &c->t_c);

	/*
	 * The user already left, whether the CANCEL is out or still waiting for
	 * the first provisional. Any final ends it, and nothing here may start
	 * another INVITE: an auth, 422 or 491 retry would ring the far end again
	 * for a call nobody is on.
	 */
	if (leg_clearing(c)) {
		leg_end(c);
		return;
	}

	if ((code == 401 || code == 407) && uac_auth_take(c, msg))
		return;
	if (code == 422 && uac_min_se_take(c, msg))
		return;
	if (code == 491 && uac_glare_take(c))
		return;
	if (code == 503)
		ua_503_handle(c->ua, leg_owner(c), msg);

	/* The referral is reported with the status the target gave, which is
	 * the one status this side can prove, and the leg it was replacing
	 * takes the call back inside cb_failed() below */
	xfer_child_failed(c, code);

	leg_fail(c, call_fail_map(code));
}

/* ------------------------------------------------- our own re-INVITE */

/*
 * The second half of a return to voice the open exchange deferred (call.h
 * call_fax_exit). False says its push ended the leg, so the caller stops there
 * and touches it no further.
 */
static bool fax_exit_finish(struct call *c, bool ok);

static void reinvite_done(struct call *c, bool ok)
{
	enum call_op op = (enum call_op)c->op;

	c->op = CALL_OP_NONE;
	if (c->state == CALL_ST_REINVITE_SENT)
		state_set(c, CALL_ST_ACTIVE);

	/* A leg that returned to voice under this exchange holds no kind, so
	 * the deferred kind below is never its business. */
	if (!fax_exit_finish(c, ok))
		return;

	/* A failed exchange supplies no description for the deferred kind.
	 * A profile already changed the DSP codec, so its committed pre-upspeed
	 * description must not replace that carrier. UDPTL carries no carrier
	 * the kind describes. A refused push restores the kind from the applied
	 * description, so that description must exist. */
	if (!ok && c->state == CALL_ST_ACTIVE && !c->fax_on &&
	    !c->sdp.committed.t38 && c->media_set &&
	    c->fax_kind != HAL_PASSTHROUGH_OFF &&
	    c->media.passthrough != c->fax_kind) {
		leg_trace(c, "fax kind %u applies after the failed exchange",
			  c->fax_kind);
		switch (media_commit_push(c)) {
		case MEDIA_PUSH_OK:
			break;
		case MEDIA_PUSH_ABANDONED:
			return;
		case MEDIA_PUSH_REFUSED:
			leg_trace(c, "stream refused deferred fax kind %u",
				  c->fax_kind);
			c->fax_kind = c->media.passthrough;
			break;
		}
	}

	/*
	 * The far end kept the codec it had. The audio session stands and the
	 * local profile stays applied: the DSP is already in
	 * passthrough and putting it back would break the page this side is
	 * already carrying. The only report the refusal makes is this line.
	 */
	if (op == CALL_OP_FAX && !ok)
		leg_trace(c, "the far end refused the G.711 upspeed and keeps the "
			  "committed codec; the local fax profile stays");

	/*
	 * The refused T.38 upgrade has nothing left to do. The peer kept the
	 * audio session, and that session is already the G.711 passthrough of
	 * the answer tone: the upgrade never starts from a leg without it
	 * (sip/line.c, the two rules of the second stage), so a failover here
	 * would convert a stream that is already converted. sip/line.c latches
	 * the attempt, so no second upgrade follows either.
	 */
	if (op == CALL_OP_T38 && !ok)
		leg_trace(c, "the far end refused the T.38 upgrade; the leg keeps "
			  "the G.711 passthrough it already carries");

	/*
	 * A session refresh and a 422 retry are ours, not the FSM's: the
	 * matrix ignores CALL_REINVITE_OK outside a feature operation, and a
	 * refresh that failed fatally arrives as CALL_FAILED instead. The fax
	 * upspeed and the T.38 upgrade are ours for a stronger reason: the
	 * machine carries no fax state at all, so no outcome of either names a
	 * cell.
	 */
	if (op == CALL_OP_REFRESH || op == CALL_OP_MIN_SE || op == CALL_OP_FAX ||
	    op == CALL_OP_T38 || op == CALL_OP_XFER_HOLD || op == CALL_OP_NONE)
		return;

	if (ui_emits(c) && cv.cb.reinvite_done)
		cv.cb.reinvite_done(c->line, c->ui_slot, ok);
}

/*
 * The end of a re-INVITE of our own on a leg that is going away: the offer
 * is dropped and the hold and the T.38 enable go back to what they were. A
 * draining leg (xfer_drain) returns to ACTIVE with it, because it keeps
 * answering the peer's re-INVITEs (rx_reinvite_drain) and would otherwise
 * refuse every one of them 491 for the rest of the drain; a leg whose BYE is
 * out keeps that state.
 */
static void reinvite_clearing_settle(struct call *c)
{
	c->op = CALL_OP_NONE;
	sdp_rollback(&c->sdp);
	sdp_hold_set(&c->sdp, c->hold_prev);
	sdp_t38_set(&c->sdp, c->t38_prev);
	if (c->xfer_drain && c->state == CALL_ST_REINVITE_SENT)
		state_set(c, CALL_ST_ACTIVE);
}

static void reinvite_fail(struct call *c, bool fatal)
{
	enum call_op op = (enum call_op)c->op;

	/* a draining leg is bounded by its guards, and a BYE here would end
	 * the dialog the drain keeps for the network's own */
	if (c->xfer_drain) {
		c->reinvite_txn = SIP_HANDLE_NONE;
		reinvite_clearing_settle(c);
		leg_trace(c, "a re-INVITE of a draining leg failed, the drain stands");
		return;
	}

	sdp_rollback(&c->sdp);
	sdp_hold_set(&c->sdp, c->hold_prev);
	sdp_t38_set(&c->sdp, c->t38_prev);
	c->reinvite_txn = SIP_HANDLE_NONE;

	/* A refresh that fails means the session is gone (RFC 4028 10). */
	if (op == CALL_OP_REFRESH || fatal) {
		c->op = CALL_OP_NONE;
		leg_abandon(c, CALL_FAIL_QUIET);
		return;
	}

	reinvite_done(c, false);
}

static void uac_reinvite_ok(struct call *c, const struct sip_msg *msg)
{
	struct sip_dialog *d = leg_dialog(c);
	struct sdp_msg ans;
	struct sip_str body;
	bool held_before;

	c->reinvite_txn = SIP_HANDLE_NONE;

	/*
	 * RFC 3261 12.2.1.2: the Contact of the 2xx is the new remote target,
	 * and the ACK this exchange still owes has to go there, not to the
	 * address the peer left.
	 */
	if (d)
		sip_dialog_target_refresh(d, msg);

	/*
	 * A hangup during the exchange already sent the BYE. The 2xx still owes
	 * an ACK (RFC 3261 13.2.2.4), but nothing else of it counts: media,
	 * session timers and the operation result all belong to a call that is
	 * already going away.
	 */
	if (leg_clearing(c)) {
		leg_trace(c, "2xx for a re-INVITE of a clearing leg, ACK only");
		ack_2xx_send(c, msg, NULL, NULL, 0);
		reinvite_clearing_settle(c);
		return;
	}

	if (!msg_body_sdp_get(msg, &body) || !sdp_parse(body, &ans) ||
	    sdp_answer_take(&c->sdp, &c->loc, &ans) != SDP_OK) {
		sdp_msg_keys_wipe(&ans);
		leg_trace(c, "re-INVITE answered without a usable answer");
		ack_2xx_send(c, msg, NULL, NULL, 0);
		reinvite_fail(c, false);
		return;
	}
	sdp_msg_keys_wipe(&ans);

	if (!ack_2xx_send(c, msg, NULL, NULL, 0) && !sip_dialog_ack_stored(d))
		leg_trace(c, "the ACK could not be sent, waiting for the 2xx again");

	held_before = c->sdp.committed.remote_hold;
	leg_commit_take(c);

	/* A stream the HAL refused leaves the operation failed, not silently
	 * successful with the stream on the old description. */
	if (!media_program(c, &c->sdp.committed, true)) {
		leg_abandon(c, CALL_FAIL_CONGESTION);
		return;
	}

	sess_uac_take(c, msg);
	reinvite_done(c, true);

	if (held_before != c->sdp.committed.remote_hold && ui_emits(c) &&
	    cv.cb.hold_changed)
		cv.cb.hold_changed(c->line, c->ui_slot, c->sdp.committed.remote_hold);
}

/*
 * The same rule inside the dialog: one retry, at the larger of the interval in
 * force and the Min-SE the peer named. It stays our own exchange, so it
 * reports no reinvite_done.
 */
static bool reinvite_min_se_take(struct call *c, const struct sip_msg *msg)
{
	uint32_t min = 0, want;

	if (c->min_se_run >= CALL_MIN_SE_MAX || c->state != CALL_ST_REINVITE_SENT)
		return false;

	want = min_se_want(c->sess_interval, msg, &min);

	c->min_se_run++;
	sess_min_se_take(c, msg);
	c->sess_want = want;
	c->sess_interval = sess_clamp(want);
	leg_trace(c, "re-INVITE 422 with Min-SE %u, asking for a %u s session", min, want);

	if (!reinvite_offer_send(c))
		return false;

	return true;
}

/* one retry per challenge kind for a request inside the dialog */
static bool reinvite_auth_take(struct call *c, const struct sip_msg *msg)
{
	if (c->state != CALL_ST_REINVITE_SENT)
		return false;
	if (!dlg_auth_take(c, msg))
		return false;

	if (!reinvite_offer_send(c))
		return false;

	leg_trace(c, "re-INVITE resent with credentials");

	return true;
}

static void uac_reinvite_final(struct call *c, struct sip_txn *txn, const struct sip_msg *msg)
{
	uint32_t code = msg->status;

	ack_nonok_set(c, txn, msg);
	c->reinvite_txn = SIP_HANDLE_NONE;

	/* the BYE is already out, or the leg drains: this exchange has no
	 * outcome left to report */
	if (leg_clearing(c)) {
		reinvite_clearing_settle(c);
		return;
	}

	if ((code == 401 || code == 407) && reinvite_auth_take(c, msg))
		return;
	if (code == 422 && reinvite_min_se_take(c, msg))
		return;
	/* uac_glare_take() has put the SDP state of before the operation back,
	 * so the answer this leg gives the peer comes from that state */
	if (code == 491 && uac_glare_take(c)) {
		glare_wait_enter(c);
		return;
	}

	/* RFC 3261 12.2.1.2: a 481 or a 408 means the dialog is gone. */
	reinvite_fail(c, code == 481 || code == 408);
}

/* ------------------------------------------------------------- UAS paths */

/* leg-less responses: an admission refusal happens before any leg exists */
static char scratch[CALL_WIRE_MAX];

static bool reinvite_start(struct call *c, enum call_op op, bool hold);

/*
 * An operation the subscriber or a transfer asked for, with the 491 budget of
 * the exchange it just opened. The budget is cleared behind the start and not
 * in front of it, because a refused command must leave the budget of an
 * exchange that is already waiting for its own retry alone. The answer cannot
 * arrive inside this frame: the send puts bytes on a socket and the response
 * reaches the leg through call_txn_resp() in a later one.
 *
 * The fax and T.38 entries need no such helper. Both test leg_free_is() before
 * they clear, so neither reaches the clear while an operation waits.
 */
static bool op_start(struct call *c, enum call_op op, bool hold)
{
	if (!reinvite_start(c, op, hold))
		return false;

	c->glare_run = 0;

	return true;
}

/*
 * A server transaction that never answered holds no timer, so one whose
 * response could not be built or sent would sit in the pool until the pool is
 * full. It is discarded instead, which is the rule ua.c applies to the
 * responses it owns (sip/txn.h sip_txn_discard). The handle is resolved again
 * because a transport error inside the send already discarded it.
 */
static void txn_finish_or_discard(struct sip_txn_mgr *mgr, uint32_t handle)
{
	struct sip_txn *txn = sip_txn_get(mgr, handle);

	if (txn)
		sip_txn_discard(txn);
}

static void resp_send(struct ua *ua, struct sip_txn *txn, const struct sip_msg *req,
		      const struct ua_resp *r)
{
	uint32_t handle = sip_txn_handle(txn);

	if (!ua_response_send(ua, txn, req, r))
		txn_finish_or_discard(ua->mgr, handle);
}

static void resp_simple(struct ua *ua, struct sip_txn *txn, const struct sip_msg *req,
			uint32_t code, const char *to_tag)
{
	struct ua_resp r;

	memset(&r, 0, sizeof(r));
	r.code = code;
	r.to_tag = to_tag && *to_tag ? to_tag : NULL;
	resp_send(ua, txn, req, &r);
}

/* RFC 3261 21.5.4: a 503 names how long the caller waits before it tries the
 * same resource again, and a caller that reads it does not fail the call over. */
static void resp_503_send(struct ua *ua, struct sip_txn *txn, const struct sip_msg *req)
{
	struct ua_resp r;

	memset(&r, 0, sizeof(r));
	r.code = 503;
	r.retry_after = CALL_HAL_RETRY_AFTER_S;
	r.has_retry_after = true;
	resp_send(ua, txn, req, &r);
}

/* RFC 4028 6: the answer to a session interval below our floor names it. */
static void resp_422_send(struct ua *ua, struct sip_txn *txn, const struct sip_msg *req,
			  uint32_t min_se)
{
	uint32_t handle = sip_txn_handle(txn);
	char tag[UA_TAG_MAX];
	struct sip_buf b;

	sip_buf_init(&b, scratch, sizeof(scratch));

	if (ua_tag_new(ua, tag, sizeof(tag)) &&
	    msg_response_build(&b, req, 422, NULL, tag, 0) &&
	    hdr_write(&b, SIP_HDR_MIN_SE, "%u", min_se) &&
	    msg_body_write(&b, NULL, NULL, 0) &&
	    sip_txn_response_send(txn, 422, sip_buf_str(&b), sip_buf_len(&b)))
		return;

	txn_finish_or_discard(ua->mgr, handle);
}

/*
 * A target that already names a URI is used verbatim; a bare number becomes
 * sip:<number>@<the account domain>. The two are told
 * apart by a ':' or an '@', either of which makes the string a URI a
 * subscriber could never dial: the uci schema accepts both forms, and
 * wrapping a configured "sip:bob@example.net" would build a Contact no proxy
 * can route.
 */
static bool target_is_uri(const char *target)
{
	return strchr(target, ':') || strchr(target, '@');
}

/*
 * digit_esc_enabled read as what it is: which dial symbols leave this device
 * percent-escaped in a userinfo. c->number keeps the raw digits at every
 * value, because the record, the status and the caller id carry the
 * subscriber's own string; only the URI text is composed from the escaped
 * form.
 */
static unsigned int leg_user_esc(const struct call *c)
{
	switch (c->ch.digit_esc) {
	case 1:
		return SIP_URI_ESC_STAR | SIP_URI_ESC_POUND;
	case 2:
		return SIP_URI_ESC_POUND;
	default:
		return 0;
	}
}

/*
 * One resolution for every target this module is handed, whether it is the
 * forward target of a redirect or the Refer-To of a transfer (call.h
 * call_xfer: "the same target shapes call_dial() accepts, resolved the same way
 * against the account domain"). Two copies of the rule would let one of them
 * drift into building a URI the proxy cannot route.
 *
 * A target that does not fit is refused, not truncated, which is the rule this
 * layer applies to every other buffer a peer or a caller can fill: half a URI
 * names a party nobody meant.
 *
 * A bare number is escaped per digit_esc_enabled before it is wrapped; a
 * configured full URI is used verbatim, because its author already chose
 * every byte of it.
 */
static bool target_uri_build(struct call *c, const char *target, char *out, size_t olen)
{
	char user[CALL_USER_ESC_MAX];
	int n = -1;

	if (target_is_uri(target))
		n = snprintf(out, olen, "%s", target);
	else if (uri_user_escape(user, sizeof(user), target, leg_user_esc(c)))
		n = snprintf(out, olen, "sip:%s@%s", user, c->domain);

	if (n >= 0 && (size_t)n < olen)
		return true;

	out[0] = 0;
	leg_trace(c, "the target does not fit a request URI, it is refused");

	return false;
}

/*
 * The local call forward redirect. The body is empty. A leg that never sent a
 * provisional carries no tag yet, and a final response needs one, so the tag
 * is minted here exactly as the UAS dialog mints it.
 */
static void resp_302_send(struct call *c, struct sip_txn *txn, const struct sip_msg *req,
			  const char *contact)
{
	uint32_t handle = sip_txn_handle(txn);
	struct sip_buf b;

	if (!c->local_tag[0] && !ua_tag_new(c->ua, c->local_tag, sizeof(c->local_tag))) {
		leg_trace(c, "no tag for the 302");
		return;
	}

	sip_buf_init(&b, c->wire, sizeof(c->wire));

	if (msg_response_build(&b, req, 302, NULL, c->local_tag, 0) &&
	    hdr_addr_write(&b, SIP_HDR_CONTACT, NULL, contact, NULL) &&
	    msg_body_write(&b, NULL, NULL, 0) &&
	    sip_txn_response_send(txn, 302, sip_buf_str(&b), sip_buf_len(&b)))
		return;

	txn_finish_or_discard(c->mgr, handle);
}

static bool dialog_uas_ensure(struct call *c, const struct sip_msg *req, struct sip_txn *txn)
{
	struct sip_dialog *d;

	if (c->dialog != SIP_HANDLE_NONE)
		return leg_dialog(c) != NULL;

	if (!c->local_tag[0] && !ua_tag_new(c->ua, c->local_tag, sizeof(c->local_tag)))
		return false;

	d = sip_dialog_uas_create(c->dialogs, req, c->local_tag, sip_txn_dst_get(txn), &c->dlocal);
	if (!d)
		return false;

	c->dialog = sip_dialog_handle(d);
	leg_trace(c, "UAS dialog %08x", c->dialog);

	return true;
}

/*
 * Our 2xx to an INVITE. Record-Route is copied only for the request that
 * creates the dialog (RFC 3261 12.1.1); a re-INVITE inherits the route set
 * that was fixed then. ua.c takes the bytes over for the RFC 3261 13.3.1.4
 * retransmission the moment the server transaction ends.
 */
static bool resp_200_send(struct call *c, struct sip_txn *txn, const struct sip_msg *req,
			  bool initial, bool *owned)
{
	struct sip_txn_dst dst = *sip_txn_dst_get(txn);
	struct sip_msg resp;
	struct sip_buf b;

	*owned = true;
	sip_buf_init(&b, c->wire, sizeof(c->wire));
	if (!msg_response_build(&b, req, 200, NULL, c->local_tag,
				initial ? SIP_RESP_F_RECORD_ROUTE : 0))
		return false;

	hdr_addr_write(&b, SIP_HDR_CONTACT, NULL, c->contact, NULL);
	hdr_write(&b, SIP_HDR_ALLOW, "%s", UA_ALLOW_DEFAULT);
	/* sip/ua.h: allow_events rides wherever allow rides, so one answer
	 * never names a method without the package that makes it useful */
	hdr_write(&b, SIP_HDR_ALLOW_EVENTS, "%s", UA_ALLOW_EVENTS_DEFAULT);
	hdr_write(&b, SIP_HDR_SUPPORTED, "%s", UA_SUPPORTED_DEFAULT);

	if (c->sess_interval) {
		hdr_write(&b, SIP_HDR_SESSION_EXPIRES, "%u%s", c->sess_interval,
			  refresher_param(c->we_refresh, false));
		/* RFC 4028 7.1: only a peer that offered the tag is required
		 * to run the timer it just agreed to. */
		if (hdr_token_has(req, SIP_HDR_SUPPORTED, "timer"))
			hdr_write(&b, SIP_HDR_REQUIRE, "timer");
	}

	if (c->user_agent[0])
		hdr_write(&b, SIP_HDR_USER_AGENT, "%s", c->user_agent);

	msg_body_write(&b, c->body_len ? "application/sdp" : NULL, c->body, c->body_len);
	if (!sip_buf_ok(&b))
		return false;

	c->wire_len = sip_buf_len(&b);
	hdr_cseq_get(req, &c->exch_cseq, NULL);

	if (!sip_txn_response_send(txn, 200, c->wire, c->wire_len))
		return false;

	/*
	 * Without the retransmission record nothing repeats the 2xx and nothing
	 * ever reports a missing ACK, so the leg would hold its slot for good.
	 * The caller ends the session instead.
	 */
	if (msg_parse(&resp, c->wire, c->wire_len) != SIP_PARSE_OK ||
	    !ua_accept_add(c->ua, &resp, c->wire, c->wire_len, &dst, leg_owner(c))) {
		leg_trace(c, "no owner for the 2xx retransmission");
		*owned = false;
	}

	return true;
}

/* the exchange an ACK completes, initial or re-INVITE */
static void uas_ack_take(struct call *c, const struct sip_msg *ack)
{
	bool held_before = c->sdp.committed.remote_hold;
	/*
	 * The takeover of a replacement is one-shot, and this is what makes it
	 * one. This entry serves the ACK of every 2xx this side sends inside a
	 * dialog, the initial one and every later re-INVITE alike, and only
	 * the first of them completes a replacement. Without the gate a peer
	 * re-INVITE after the takeover would run the swap again: the replaced
	 * leg is still in use while its BYE drains, so its handle still
	 * resolves, and it would be BYEd a second time and its hold re-offered
	 * a second time.
	 *
	 * It is read here, before anything below moves the state, because the
	 * state is the only thing that tells the two exchanges apart and the
	 * common path sets it to ACTIVE for both.
	 */
	bool initial = c->state == CALL_ST_ACCEPT_WAIT_ACK;
	struct sdp_msg ans;
	struct sip_str body;
	bool media_ok;

	if (c->sdp.offer_in_200) {
		if (!msg_body_sdp_get(ack, &body) || !sdp_parse(body, &ans) ||
		    sdp_answer_take(&c->sdp, &c->loc, &ans) != SDP_OK) {
			sdp_msg_keys_wipe(&ans);
			leg_trace(c, "ACK without the answer our 2xx asked for");
			/* an exchange accepted before the leg began to drain:
			 * the dialog stays for the network BYE or the verdict,
			 * and the offer it carried is dropped */
			if (c->xfer_drain) {
				sdp_rollback(&c->sdp);
				state_set(c, CALL_ST_ACTIVE);
				return;
			}
			leg_abandon(c, CALL_FAIL_QUIET);
			return;
		}
		sdp_msg_keys_wipe(&ans);
	}

	leg_commit_take(c);

	/*
	 * The takeover of a Replaces answer runs in front of everything below
	 * (RFC 3891 3), for the two reasons the transferee's promotion runs in
	 * front of media_program(): this leg holds no UI name until it takes
	 * one, so the gate a few lines down would send it away with no stream
	 * at all, and the leg it replaces still owns the one stream this line
	 * carries, so that stream has to go down before this one comes up.
	 */
	if (initial && leg_replaces(c))
		replaces_takeover(c);

	/*
	 * The first exchange is the call: no stream, no call.
	 *
	 * A later one only records a refusal that leaves the old stream
	 * running, because that call is better off kept than cleared.
	 *
	 * A rekey stops the stream before it configures it (media_program): a
	 * changed SDES key or suite takes the path of a T.38 mode move. A
	 * configure or a start that refuses behind a successful stop leaves
	 * this leg active with its media down. Such a leg ends, because a
	 * stream that is down cannot carry what the exchange agreed.
	 *
	 * media_started is what tells the two apart. A stop that itself refused
	 * leaves it true and the old stream alive, and the record-only rule
	 * keeps that leg.
	 */
	/*
	 * The name can move between our 2xx and this ACK. The child answers on
	 * its own schedule, so an exchange this leg accepted while it still
	 * carried the call completes on a leg that no longer does: the
	 * negotiation still commits above, because the dialog agreed to it, and
	 * the stream and the timers do not, because there is no stream to
	 * program and no session left to watch.
	 */
	if (!ui_holds(c)) {
		leg_trace(c, "the ACK completes an exchange of a leg the transfer replaced");
		state_set(c, CALL_ST_ACTIVE);
		return;
	}

	media_ok = media_program(c, &c->sdp.committed, true);
	if (!media_ok &&
	    (c->state == CALL_ST_ACCEPT_WAIT_ACK || !c->media_started)) {
		leg_abandon(c, CALL_FAIL_CONGESTION);
		return;
	}

	sess_arm(c);
	state_set(c, CALL_ST_ACTIVE);

	/*
	 * A return to voice that ran under this exchange. A push that landed
	 * carried the committed description with no kind in it, and an
	 * offerless exchange took its answer from the leg's codec list a few
	 * lines up, so the restore is the last thing left and this is the
	 * earliest frame it may run in.
	 *
	 * A push the stream refused leaves the old description running, so the
	 * completion is told: it retries it once and, if that is refused too,
	 * puts the fax fields back rather than report voice over a carrier the
	 * backend is still holding. A retry that stopped the stream and could
	 * not bring it back ends the leg, and nothing below may touch it then.
	 */
	if (!fax_exit_finish(c, media_ok))
		return;

	/* the hold the machine still believes this name is in, re-offered on the
	 * leg that now wears it: the same settle a transferee's child makes, and
	 * it needs the ACTIVE state above */
	if (initial && leg_replaces(c))
		xfer_child_settle(c);

	if (held_before != c->sdp.committed.remote_hold && ui_emits(c) &&
	    cv.cb.hold_changed)
		cv.cb.hold_changed(c->line, c->ui_slot, c->sdp.committed.remote_hold);
}

/*
 * Which view answers an inbound image offer. The daemon carries one T.38
 * session, and the second line asking for one has to be refused with the
 * m=image port 0 the disabled path already writes, byte for byte. Handing the
 * answer builder a copy with the enable cleared is what produces that: the
 * refusal shape then has one implementation instead of two, and the leg's own
 * view keeps the truth about what its channel and backend allow.
 *
 * The image section only has to be present for the admission to be consulted;
 * every other condition of the acceptance gate belongs to sdp.c, and an offer
 * that fails one of those is refused with or without this.
 */
static const struct sdp_local *t38_answer_loc(struct call *c, const struct sdp_msg *offer,
					      struct sdp_local *copy)
{
	if (!c->loc.t38 || !offer->image.present || t38_admission_free(c))
		return &c->loc;

	leg_trace(c, "a T.38 session already runs, so this image offer is refused "
		  "and the audio session stands");
	*copy = c->loc;
	copy->t38 = false;

	return copy;
}

/* the far-end fax upspeed recognizer, whose two signals meet in rx_reinvite() */
static void fax_follow_check(struct call *c, const struct sdp_msg *offer);
/* and its earlier half, which the answer of that same exchange depends on */
static void fax_follow_loc_prepare(struct call *c, const struct sdp_msg *offer);

/*
 * A peer re-INVITE on a leg that drains for a consult transfer. Asterisk
 * re-INVITEs the replaced leg the moment it accepts the REFER, and
 * sends its Replaces-driven BYE on that leg only behind a 2xx to it. A 488
 * would leave the leg without that BYE for as long as the drain guard runs.
 * The answer is built the ordinary way behind the local hold the drain set,
 * so it reads sendonly or inactive, and it programs
 * nothing: the stream is stopped and the timers are off, and uas_ack_take()
 * sends a leg with no UI name away before it would start one. No session
 * headers ride it either, because the drain zeroed the interval.
 *
 * Two offers keep the 488. An offerless one would put our offer in the 2xx,
 * and an ACK without the answer that asks for is a leg abandoned above the
 * name gate. An image offer would be accepted in front of the direction and
 * hand the peer a UDPTL stream nobody programs, so the answer is built from a
 * view with T.38 off: an audio offer beside an image gets its audio answered
 * and the image refused with port 0, and an image-only offer builds nothing.
 */
static void rx_reinvite_drain(struct call *c, struct sip_txn *txn, const struct sip_msg *req)
{
	struct sip_dialog *d = leg_dialog(c);
	struct sdp_local loc = c->loc;
	struct sdp_msg offer;
	struct sip_str body;
	enum sdp_verdict v;
	bool owned;

	/* RFC 4028 6: the floor every re-INVITE meets, drain or not */
	if (!sess_min_ok(c, req)) {
		resp_422_send(c->ua, txn, req, sess_min_cfg(c));
		return;
	}

	if (!msg_body_sdp_get(req, &body)) {
		leg_trace(c, "offerless re-INVITE on a draining leg, 488");
		resp_simple(c->ua, txn, req, 488, c->local_tag);
		return;
	}
	if (!sdp_parse(body, &offer)) {
		sdp_msg_keys_wipe(&offer);
		resp_simple(c->ua, txn, req, 488, c->local_tag);
		return;
	}

	loc.t38 = false;
	v = body_answer_build(c, &loc, &offer);
	/* the copy carries this leg's master keys and the offer the peer's */
	explicit_bzero(&loc, sizeof(loc));
	sdp_msg_keys_wipe(&offer);
	if (v != SDP_OK) {
		sdp_rollback(&c->sdp);
		leg_trace(c, "re-INVITE on a draining leg builds no answer, 488");
		resp_simple(c->ua, txn, req, 488, c->local_tag);
		return;
	}

	if (!resp_200_send(c, txn, req, false, &owned)) {
		sdp_rollback(&c->sdp);
		return;
	}

	if (d)
		sip_dialog_target_refresh(d, req);

	state_set(c, CALL_ST_REINVITE_WAIT_ACK);
	leg_trace(c, "re-INVITE on a draining leg answered with no stream");
	/* a 2xx nobody retransmits is still bounded by the guards, where an
	 * ordinary leg would abandon itself here */
	if (!owned)
		leg_trace(c, "the 2xx is not retransmitted, the drain guard bounds the leg");
}

/* RFC 3261 14.2 plus the T.38 and codec refusals of sdp.c */
static void rx_reinvite(struct call *c, struct sip_txn *txn, const struct sip_msg *req)
{
	struct sip_dialog *d = leg_dialog(c);
	/* the leg's codec list before the fax follow may widen it, so an
	 * exchange that does not complete leaves the view it found */
	unsigned int loc_codecs = c->loc.codecs;
	struct sdp_local gated;
	struct sdp_msg offer;
	struct sip_str body;
	enum sdp_verdict v;
	bool offerless, owned;

	/*
	 * RFC 3261 15.1.1: the session ended for this side when its BYE went
	 * out, and the dialog that BYE names is no longer one this side serves
	 * (12.2.2). A 491 here would promise a retry on a dialog that will never
	 * take one, and a switch can leave such a 491 without an ACK.
	 */
	if (c->state == CALL_ST_BYE_SENT) {
		leg_trace(c, "re-INVITE behind our own BYE, the dialog is gone, 481");
		resp_simple(c->ua, txn, req, 481, c->local_tag);
		return;
	}

	/* before any refusal below: a refused request still names a Min-SE
	 * that the path holds (RFC 4028 7.4) */
	sess_min_se_take(c, req);

	if (c->state != CALL_ST_ACTIVE) {
		leg_trace(c, "re-INVITE while an exchange is running, 491");
		resp_simple(c->ua, txn, req, 491, c->local_tag);
		return;
	}

	/* a consult transfer's draining leg answers, where the name gate
	 * below would refuse it 488 (call.h call_xfer_consult) */
	if (c->xfer_drain) {
		rx_reinvite_drain(c, txn, req);
		return;
	}

	/*
	 * A leg that holds no UI name has no stream left. A transferor that
	 * moves its port on the replaced dialog after the swap would otherwise
	 * have this leg's stream programmed and started again, which is a
	 * second stream on a line that carries one, and it would re-arm the
	 * session timers demotion cancelled.
	 *
	 * The demoted parent before the swap is not this case. It still holds
	 * its name and its stream, the subscriber is still on it, and its
	 * refreshes and holds must keep working: ui_holds() is what tells the
	 * two apart, where a test on the QUIET role alone would silence a leg
	 * that is still carrying the call.
	 */
	if (!ui_holds(c)) {
		leg_trace(c, "re-INVITE on a leg the transfer replaced, 488");
		resp_simple(c->ua, txn, req, 488, c->local_tag);
		return;
	}

	/* RFC 4028 6: the floor is checked before any of our state moves. */
	if (!sess_min_ok(c, req)) {
		resp_422_send(c->ua, txn, req, sess_min_cfg(c));
		return;
	}

	offerless = !msg_body_sdp_get(req, &body);
	if (!offerless && !sdp_parse(body, &offer)) {
		sdp_msg_keys_wipe(&offer);
		resp_simple(c->ua, txn, req, 488, c->local_tag);
		return;
	}

	if (offerless) {
		/* BroadWorks style: our offer rides the 2xx, the answer the ACK. */
		c->sdp.offer_in_200 = true;
		if (!body_offer_build(c)) {
			sdp_rollback(&c->sdp);
			resp_simple(c->ua, txn, req, 500, c->local_tag);
			return;
		}
	} else {
		/*
		 * The recognizer's verdict is needed twice, and this is the
		 * earlier of the two. A channel provisioned without G.711
		 * answers the far end's narrowing offer 488, because the
		 * selection below walks this leg's own list and finds nothing
		 * the offer also names, so the leg would refuse the exchange it
		 * means to follow. Every other offer leaves the list alone.
		 */
		fax_follow_loc_prepare(c, &offer);

		v = body_answer_build(c, t38_answer_loc(c, &offer, &gated), &offer);

		/*
		 * Both copies die here. gated is a whole sdp_local, so it
		 * carries both of this leg's master keys, and offer carries the
		 * peer's; the answer is built, so neither is read again.
		 */
		explicit_bzero(&gated, sizeof(gated));
		sdp_msg_keys_wipe(&offer);
		if (v != SDP_OK) {
			c->loc.codecs = loc_codecs;
			sdp_rollback(&c->sdp);
			resp_simple(c->ua, txn, req, 488, c->local_tag);
			return;
		}
	}

	sess_uas_take(c, req);
	if (!resp_200_send(c, txn, req, false, &owned)) {
		/* the answer never left, so the widened list has no exchange to
		 * belong to; the offerless path never widened one */
		c->loc.codecs = loc_codecs;
		sdp_rollback(&c->sdp);
		return;
	}

	/*
	 * The inbound upgrade is accepted only now. The answer named an image
	 * session, so this leg negotiates T.38 from here: the flag holds
	 * the daemon-wide admission and makes the next offer of ours, a session
	 * refresh above all, re-offer the image stream instead of dropping the
	 * far end back to audio. Every path in front of this one leaves it
	 * unset, so an answer that could not be built or could not be sent
	 * leaks no admission.
	 *
	 * An SRTP leg needs no gate of its own here. sdp.c's t38_answer_gate()
	 * stays closed while the committed exchange names a suite, so
	 * proposed.t38 never becomes true on such a leg and the offer took the
	 * port 0 image refusal instead.
	 */
	if (c->sdp.proposed.t38 && !c->sdp.committed.t38) {
		sdp_t38_set(&c->sdp, true);
		/* an image session is a fax on the wire, so no later rule takes
		 * this leg back to voice (call.h call_fax_exit) */
		c->fax_evidence = true;
		leg_trace(c, "the peer's T.38 upgrade is accepted");
	}

	/*
	 * The request is accepted only now, so this is where its Contact
	 * becomes the remote target (RFC 6141: a target refresh that was not
	 * answered 2xx moves nothing).
	 */
	if (d)
		sip_dialog_target_refresh(d, req);

	/*
	 * The answer is out and the exchange can no longer be rolled back, so a
	 * profile applied here cannot outlive a 200 that never left. It runs in
	 * front of the programming below for the reason the outgoing upspeed
	 * applies its profile first: the description that reaches the stream
	 * then already carries the fax kind and the clamped fields, and one
	 * config_set does the whole move.
	 */
	if (!offerless)
		fax_follow_check(c, &offer);

	/*
	 * The peer may send from the new description the moment its 2xx
	 * arrives, so the stream and the session timers follow the answer at
	 * the 2xx; only the negotiation itself waits for the ACK to commit.
	 */
	if (!offerless && !media_program(c, &c->sdp.proposed, true)) {
		leg_abandon(c, CALL_FAIL_CONGESTION);
		return;
	}
	sess_arm(c);

	state_set(c, CALL_ST_REINVITE_WAIT_ACK);
	if (!owned)
		leg_abandon(c, CALL_FAIL_QUIET);
}

/* the leg reached a state where the FSM believes media exists */
static bool leg_answered(const struct call *c)
{
	switch ((enum call_state)c->state) {
	case CALL_ST_ACCEPT_WAIT_ACK:
	case CALL_ST_ACTIVE:
	case CALL_ST_REINVITE_SENT:
	case CALL_ST_REINVITE_WAIT_ACK:
	case CALL_ST_BYE_SENT:
		return true;
	default:
		return false;
	}
}

/*
 * RFC 3261 15.1.2: a BYE on an early dialog ends the dialog, and the INVITE
 * that made it still needs its final response. 487 is the one the RFC
 * recommends.
 */
static void early_invite_end(struct call *c)
{
	struct sip_txn *txn = leg_txn(c, c->server_txn);
	struct sip_msg req;

	if (!txn || !sip_txn_request_get(txn, &req))
		return;

	ua_trying_cancel(c->ua, c->server_txn);
	resp_simple(c->ua, txn, &req, 487, c->local_tag);
}

static void rx_bye(struct call *c, struct sip_txn *txn, const struct sip_msg *req)
{
	bool own_end = c->reported;

	clear_stamp(c);
	resp_simple(c->ua, txn, req, 200, c->local_tag);
	if (!c->uac && c->state == CALL_ST_RINGING_IN)
		early_invite_end(c);
	media_stop(c);
	sess_stop(c);
	leg_trace(c, "peer BYE");

	/*
	 * A BYE cannot precede the answer, and the CALLING and PROCEEDING cells
	 * make the FSM ignore CALL_REMOTE_BYE for an outbound leg that is not
	 * answered yet. A peer that ends an early dialog that way is a clear,
	 * so it is reported as one and the FSM leaves the state.
	 */
	if (c->uac && !leg_answered(c)) {
		leg_fail(c, CALL_FAIL_QUIET);
		return;
	}

	c->reported = true;

	/*
	 * A leg that reported its own end never reports a far-end clear
	 * (call.h states the transfer instance of it in the last-word clause
	 * of cb.xfer_done). Three endings reach this gate, and they are the
	 * three that leave the leg alive to drain a BYE of its
	 * own: a subscriber hangup, a line teardown and a decided transfer.
	 * Each of them resolves the slot itself, and the FSM may have given
	 * that slot another role by the time the peer's own BYE crosses ours: a
	 * second report would name a leg the machine has finished with. The BYE
	 * is still answered, still stamps the clear and still ends the leg;
	 * only the report is dropped.
	 *
	 * A rejected or a forwarded leg never arrives here. Both release the
	 * leg in the frame that answers the offer, the dialog dies with the
	 * release, and a BYE behind it resolves to no leg at all.
	 *
	 * The flag is taken at entry, because the line above sets it for the
	 * clear this path is itself making.
	 */
	if (!own_end && ui_emits(c) && cv.cb.remote_bye)
		cv.cb.remote_bye(c->line, c->ui_slot);

	leg_end(c);
}

/* -------------------------------------------- the BroadWorks call waiting */

/*
 * The wire of the network call waiting INFO. No RFC defines it, so these are
 * the tokens the switch sends, verbatim. Every scan below is bounded by the
 * body length, so a body without a terminator is safe.
 */
#define CALL_CW_CTYPE		"application/broadsoft"
#define CALL_CW_STOP		"stop CallWaitingTone"
#define CALL_CW_PLAY		"play tone CallWaitingTone"
#define CALL_CW_NAME		"Calling-Name:"
#define CALL_CW_NUMBER		"Calling-Number:"

static struct sip_str cw_ws_skip(struct sip_str s)
{
	while (s.len && (s.p[0] == ' ' || s.p[0] == '\t')) {
		s.p++;
		s.len--;
	}

	return s;
}

/* the command may be preceded by whitespace */
static struct sip_str cw_lead_skip(struct sip_str s)
{
	while (s.len && (s.p[0] == ' ' || s.p[0] == '\t' || s.p[0] == '\r' ||
			 s.p[0] == '\n')) {
		s.p++;
		s.len--;
	}

	return s;
}

/* the keywords are generated by the switch, so they stay case sensitive */
static bool cw_lit_take(struct sip_str *s, const char *lit)
{
	size_t n = strlen(lit);

	if (s->len < n || memcmp(s->p, lit, n))
		return false;

	s->p += n;
	s->len -= n;

	return true;
}

/* what follows the first occurrence of a keyword, anywhere in the body */
static bool cw_kw_find(struct sip_str body, const char *kw, struct sip_str *rest)
{
	size_t n = strlen(kw), i;

	for (i = 0; body.len >= n && i <= body.len - n; i++) {
		if (memcmp(body.p + i, kw, n))
			continue;

		*rest = sip_str_init(body.p + i + n, body.len - i - n);

		return true;
	}

	return false;
}

/*
 * The bytes between the first pair of double quotes after the keyword. An
 * unclosed quote yields nothing, so the rest of the message never reaches the
 * CID.
 */
static void cw_name_take(struct sip_str body, char *dst, size_t dlen)
{
	struct sip_str rest;
	size_t i;

	dst[0] = 0;
	if (!cw_kw_find(body, CALL_CW_NAME, &rest))
		return;

	for (i = 0; i < rest.len && rest.p[i] != '"'; i++)
		;
	if (i == rest.len)
		return;

	rest = sip_str_init(rest.p + i + 1, rest.len - i - 1);
	for (i = 0; i < rest.len && rest.p[i] != '"'; i++)
		;
	if (i == rest.len)
		return;

	sip_str_copy(dst, dlen, sip_str_init(rest.p, i));
}

/* digits only, after the colon of the keyword and any blanks */
static void cw_number_take(struct sip_str body, char *dst, size_t dlen)
{
	struct sip_str rest;
	size_t i;

	dst[0] = 0;
	if (!cw_kw_find(body, CALL_CW_NUMBER, &rest))
		return;

	rest = cw_ws_skip(rest);
	for (i = 0; i < rest.len && rest.p[i] >= '0' && rest.p[i] <= '9'; i++)
		;

	sip_str_copy(dst, dlen, sip_str_init(rest.p, i));
}

bool call_cw_ctype_match(struct sip_str ctype)
{
	return sip_str_eq_ci(sip_media_type(ctype), CALL_CW_CTYPE);
}

bool call_cw_parse(struct sip_str body, struct call_cw *out)
{
	struct sip_str s = cw_lead_skip(body);

	memset(out, 0, sizeof(*out));

	if (cw_lit_take(&s, CALL_CW_STOP))
		return true;

	if (!cw_lit_take(&s, CALL_CW_PLAY))
		return false;

	/* the pattern digit is glued to the keyword and is not optional */
	if (!s.len || s.p[0] < '1' || s.p[0] > '4')
		return false;

	out->play = true;
	out->tone = (uint8_t)(s.p[0] - '0');
	cw_name_take(body, out->name, sizeof(out->name));
	cw_number_take(body, out->number, sizeof(out->number));

	return true;
}

/* ------------------------------------------- the out of band digit INFO */

/*
 * The digit the far end sends in a SIP INFO. Two content types carry it and
 * both are in the field: application/dtmf-relay carries a Signal line and an
 * optional Duration line, and application/dtmf carries the digit alone. The
 * grammar has no RFC; these are the tokens the switches send.
 *
 * Signal takes either separator, because both spellings are on the wire, and
 * it takes the digit alphabet or an RFC 4733 event code. Event codes 10 to 15
 * are star, pound and A to D; a code of 16 and above is a flash or a wire
 * event this line cannot play, and it is refused with the rest.
 */
#define CALL_DTMF_RELAY_CTYPE	"application/dtmf-relay"
#define CALL_DTMF_CTYPE		"application/dtmf"
#define CALL_DTMF_SIGNAL	"Signal"
#define CALL_DTMF_DURATION	"Duration"
#define CALL_DTMF_ALPHABET	"0123456789*#ABCD"

static bool dtmf_char_take(char c, char *digit)
{
	if (c >= 'a' && c <= 'd')
		c = (char)(c - 'a' + 'A');

	if (!c || !strchr(CALL_DTMF_ALPHABET, c))
		return false;

	*digit = c;

	return true;
}

static bool dtmf_code_take(unsigned int code, char *digit)
{
	if (code > 15)
		return false;

	*digit = CALL_DTMF_ALPHABET[code];

	return true;
}

/* the separator of a keyword line, in both spellings, and the whitespace
 * around it */
static bool dtmf_sep_take(struct sip_str *s)
{
	*s = cw_ws_skip(*s);

	if (!s->len || (s->p[0] != '=' && s->p[0] != ':'))
		return false;

	s->p++;
	s->len--;
	*s = cw_ws_skip(*s);

	return true;
}

/* the value runs to the end of the line, so trailing text refuses the body
 * instead of being read as a digit */
static struct sip_str dtmf_line_take(struct sip_str s)
{
	size_t i;

	for (i = 0; i < s.len && s.p[i] != '\r' && s.p[i] != '\n'; i++)
		;

	while (i && (s.p[i - 1] == ' ' || s.p[i - 1] == '\t'))
		i--;

	return sip_str_init(s.p, i);
}

static bool dtmf_signal_take(struct sip_str rest, char *digit)
{
	unsigned int code = 0;
	struct sip_str tok;
	size_t i;

	if (!dtmf_sep_take(&rest))
		return false;

	tok = dtmf_line_take(rest);
	if (!tok.len)
		return false;

	if (tok.len == 1 && (tok.p[0] < '0' || tok.p[0] > '9'))
		return dtmf_char_take(tok.p[0], digit);

	for (i = 0; i < tok.len; i++) {
		if (tok.p[i] < '0' || tok.p[i] > '9')
			return false;

		code = code * 10 + (unsigned int)(tok.p[i] - '0');
		if (code > 15)
			return false;
	}

	return dtmf_code_take(code, digit);
}

static void dtmf_duration_take(struct sip_str rest, uint16_t *duration_ms)
{
	uint32_t ms = 0;
	size_t i;

	if (!dtmf_sep_take(&rest))
		return;

	for (i = 0; i < rest.len && rest.p[i] >= '0' && rest.p[i] <= '9'; i++) {
		ms = ms * 10 + (uint32_t)(rest.p[i] - '0');
		if (ms > UINT16_MAX)
			ms = UINT16_MAX;
	}

	*duration_ms = (uint16_t)ms;
}

bool call_dtmf_relay_ctype_match(struct sip_str ctype)
{
	struct sip_str type = sip_media_type(ctype);

	return sip_str_eq_ci(type, CALL_DTMF_RELAY_CTYPE) ||
	       sip_str_eq_ci(type, CALL_DTMF_CTYPE);
}

bool call_dtmf_relay_parse(struct sip_str ctype, struct sip_str body,
			   char *digit, uint16_t *duration_ms)
{
	struct sip_str type = sip_media_type(ctype);
	struct sip_str rest;

	*digit = 0;
	*duration_ms = 0;

	if (sip_str_eq_ci(type, CALL_DTMF_CTYPE)) {
		rest = cw_lead_skip(body);

		return rest.len && dtmf_char_take(rest.p[0], digit);
	}

	if (!sip_str_eq_ci(type, CALL_DTMF_RELAY_CTYPE))
		return false;

	if (!cw_kw_find(body, CALL_DTMF_SIGNAL, &rest) ||
	    !dtmf_signal_take(rest, digit))
		return false;

	if (cw_kw_find(body, CALL_DTMF_DURATION, &rest))
		dtmf_duration_take(rest, duration_ms);

	return true;
}

/*
 * The out of band digit half of an in-dialog INFO. The answer is decided by
 * the content type and not by the body: a far end that sends a digit this
 * parser refuses keeps its call, because a 400 here would end it.
 * The 200 goes out before the dispatch, as it does for the alert below.
 */
static void rx_info_dtmf(struct call *c, struct sip_txn *txn,
			 const struct sip_msg *req, struct sip_str ctype,
			 struct sip_str body)
{
	uint16_t duration_ms;
	char digit;

	resp_simple(c->ua, txn, req, 200, c->local_tag);

	if (!call_dtmf_relay_parse(ctype, body, &digit, &duration_ms)) {
		leg_trace(c, "out of band digit INFO body refused");
		return;
	}

	leg_trace(c, "out of band digit '%c' duration %u", digit, duration_ms);

	if (ui_emits(c) && cv.cb.digit_net)
		cv.cb.digit_net(c->line, c->ui_slot, digit, duration_ms);
}

/*
 * An in-dialog INFO. A body-less one keeps the 200 of RFC 2976 2.2, and every
 * content type this module does not consume is acknowledged and dropped,
 * because a 400 to a body we do not read would break a call over nothing.
 *
 * Two bodies are consumed, and each answers by its own rule. A broadsoft
 * alert answers by the body, 200 when it parses and 400 when it does not. An
 * out of band digit answers 200 by the content type alone, whatever the body
 * holds. The 200 of a parsed body goes out before the dispatch, so a
 * downstream state that drops the report never changes the SIP answer.
 */
static void rx_info(struct call *c, struct sip_txn *txn, const struct sip_msg *req)
{
	struct sip_str body, ctype;
	struct call_cw cw;

	if (!msg_body_get(req, &body) ||
	    !msg_hdr_value(req, SIP_HDR_CONTENT_TYPE, &ctype)) {
		resp_simple(c->ua, txn, req, 200, c->local_tag);
		return;
	}

	if (call_dtmf_relay_ctype_match(ctype)) {
		rx_info_dtmf(c, txn, req, ctype, body);
		return;
	}

	if (!call_cw_ctype_match(ctype)) {
		resp_simple(c->ua, txn, req, 200, c->local_tag);
		return;
	}

	if (!call_cw_parse(body, &cw)) {
		leg_trace(c, "broadsoft INFO body refused");
		resp_simple(c->ua, txn, req, 400, c->local_tag);
		return;
	}

	resp_simple(c->ua, txn, req, 200, c->local_tag);
	leg_trace(c, "network call waiting %s tone %u number '%s'",
		  cw.play ? "play" : "stop", cw.tone, cw.number);

	if (ui_emits(c) && cv.cb.cw_net)
		cv.cb.cw_net(c->line, c->ui_slot, cw.play, cw.tone, cw.name, cw.number);
}

/* ------------------------------------------------- the transferor's REFER */

/*
 * The transferor side of RFC 3515: this module sends a REFER, keeps the
 * implicit subscription it creates and reports the one outcome. The transferee
 * side, which serves an inbound REFER, starts at rx_refer() below.
 */

static bool leg_in_dialog(const struct call *c);

/*
 * The Replaces of a consult transfer, oriented at the receiver (RFC 3891 3,
 * call.h call_xfer_consult): the target compares to-tag against its own local
 * tag, so the replaced dialog's remote tag is the one that rides to-tag.
 *
 * The three fields come from the dialog and not from the leg, because an
 * inbound leg never built a Call-ID or a tag of its own: the dialog is the one
 * place that holds both orientations whichever side created it.
 */
static bool replaces_value_build(struct call *other, char *out, size_t olen)
{
	struct sip_dialog *d = leg_dialog(other);
	struct sip_buf b;

	if (!d)
		return false;

	sip_buf_init(&b, out, olen);

	return hdr_replaces_write(&b, d->call_id, d->remote_tag, d->local_tag, false);
}

/*
 * The Refer-To URI. other is the leg a consult transfer hands over, and its
 * Replaces rides the URI headers part after the '?' (RFC 3515 2.4.2), escaped
 * because every separator inside it would otherwise end the URI.
 */
static bool refer_uri_build(struct call *c, struct call *other, char *out, size_t olen)
{
	char replaces[CALL_REPLACES_MAX];
	struct sip_buf b;

	sip_buf_init(&b, out, olen);
	sip_buf_add(&b, c->xfer_target);

	if (other) {
		if (!replaces_value_build(other, replaces, sizeof(replaces)))
			return false;

		/* RFC 3261 25.1: the first URI header opens with '?' and every
		 * one behind it with '&', and a peer Contact may already carry
		 * one of its own */
		sip_buf_add(&b, strchr(c->xfer_target, '?') ? "&Replaces=" : "?Replaces=");
		uri_header_escape(&b, replaces);
	}

	return sip_buf_ok(&b);
}

/*
 * The two headers a REFER carries beside what the dialog layer writes.
 * Referred-By is our own identity (RFC 3892 5), taken from the dialog's local
 * URI, which is what the From of every in-dialog request of this leg already
 * carries, so a call the subscriber withheld refers under the same anonymous
 * identity it dialled with and no second decision can disagree with it.
 */
static bool refer_hdrs_build(struct call *c, struct call *other, char *out, size_t olen)
{
	struct sip_dialog *d = leg_dialog(c);
	char uri[CALL_REFER_URI_MAX];
	struct sip_buf b;

	if (!d || !d->local_uri)
		return false;
	if (!refer_uri_build(c, other, uri, sizeof(uri)))
		return false;

	sip_buf_init(&b, out, olen);
	hdr_addr_write(&b, SIP_HDR_REFER_TO, NULL, uri, NULL);
	hdr_addr_write(&b, SIP_HDR_REFERRED_BY, NULL, d->local_uri, NULL);

	return sip_buf_ok(&b);
}

/*
 * The REFER itself, on the leg's own transaction handle: the flash_txn pattern,
 * with the owner left as this leg so the answer arrives at call_txn_resp().
 * The CSeq the dialog layer handed out is the id of the subscription this
 * request creates (RFC 3515 2.4.4), so it is kept before anything else can
 * build a request on this leg.
 */
static bool refer_send(struct call *c, struct call *other)
{
	char hdrs[CALL_REFER_HDRS_MAX];

	if (!refer_hdrs_build(c, other, hdrs, sizeof(hdrs))) {
		leg_trace(c, "the REFER headers could not be built, the transfer is refused");
		return false;
	}

	if (!dialog_req_send(c, SIP_METHOD_REFER, NULL, NULL, 0, true, &c->refer_txn, hdrs))
		return false;

	c->refer_cseq = c->dlg_cseq;
	leg_trace(c, "REFER cseq %u sent", c->refer_cseq);

	return true;
}

/*
 * Where an outcome came from. Four of them arrive as (false, 408), and the
 * consult arms of xfer_outcome() treat them differently: the guard expiry
 * keeps the replaced leg draining where a verdict of failure BYEs it.
 */
enum xfer_source {
	XFER_SRC_REFER = 0,	/* the answer to the REFER, or its transaction's death */
	XFER_SRC_VERDICT,	/* a sipfrag of 200 or above */
	XFER_SRC_SUB_END,	/* the subscription ended with no final sipfrag */
	XFER_SRC_GUARD,		/* CALL_XFER_WAIT_MS or CALL_XFER_CONSULT_WAIT_MS */
};

/*
 * The report debt is consumed here and nowhere else: whoever takes it owes the
 * one report and nothing may report twice. The subscription is not touched,
 * because a blind 202 reports at once and keeps answering the NOTIFYs of a
 * leg that still drains; the arms of xfer_decide() end the subscription.
 */
static bool xfer_owed_take(struct call *c)
{
	bool owed = c->xfer_owed;

	c->xfer_owed = false;

	return owed;
}

/*
 * The local clearing a decided transfer costs a leg. It is the ordinary hangup
 * shape, because the FSM asked for the transfer and is owed no clearing class.
 * The UI occupancy is not touched here: the arms that free it do so before
 * they report, and the consult legs keep it until the protocol ends (call.h).
 */
static void xfer_leg_clear(struct call *c)
{
	c->reported = true;
	if (!bye_send(c))
		leg_end(c);
}

/*
 * The active leg a consult transfer handed over. It is cleared beside the
 * referee and reports no outcome of its own: call.h fires xfer_done for
 * referee_slot alone, and for the record.
 *
 * A handle that no longer resolves is the leg that already ended, through its
 * own far-end path, and its slot may hold a new call by now. Nothing is
 * cleared for it, because the only leg this transfer ever meant is gone.
 */
static void xfer_other_clear(uint32_t handle, bool transferred)
{
	struct call *o = leg_get(handle);

	if (!o)
		return;

	o->transferred = transferred;
	xfer_leg_clear(o);
}

static void xfer_replaced_end(struct call *c);

/*
 * The silence a transfer gives a leg the subscriber has left: both legs of a
 * consult at the 202, a blind referee at the 202, an attended referee at its
 * verdict. The subscriber is gone from it, so nothing may reach the FSM again
 * (reported) and no command resolves the name (ui_role NONE, which keeps
 * uas_ack_take() from starting a stream; xfer_drain is what routes a peer
 * re-INVITE to rx_reinvite_drain()). The call ended for the subscriber here,
 * so the record dates its clear here and not at whatever ends the dialog
 * later: the stamp is taken once, and a drain of up to the guard would
 * otherwise read as talk time. The media and the session timers stop, and
 * the interval goes with them: resp_200_send() writes Session-Expires for as
 * long as it stands, and a drain answer refreshes nothing. The local hold
 * makes every answer this leg still builds read sendonly or inactive, and
 * hold_prev keeps it there when an exchange of the leg's own completes and
 * puts the previous hold back.
 *
 * The leg is marked clearing on top, for an exchange of its own still in
 * flight: the re-INVITE paths then treat its answer as they treat one that
 * lands after a hangup, the 2xx is ACKed (RFC 3261 13.2.2.4) and nothing else
 * of it counts, and a challenge, a 422 or a 491 starts no fresh INVITE.
 * Aborting that transaction instead would leave the 2xx with no ACK, because
 * the stray-2xx path answers only a fork of the setup. A glare retry already
 * armed is cancelled for the same reason.
 *
 * The dialog stays. What ends it is the network BYE, which rx_bye() answers
 * 200 and reports nothing, or the guard the caller arms.
 */
static void xfer_leg_silence(struct call *c)
{
	clear_stamp(c);
	c->reported = true;
	c->ui_role = CALL_UI_NONE;
	c->clearing = true;
	c->xfer_drain = true;
	glare_wait_drop(c);
	if (c->ua)
		ua_glare_cancel(c->ua, leg_owner(c));

	media_stop(c);
	sess_stop(c);
	c->sess_interval = 0;
	sdp_hold_set(&c->sdp, true);
	c->hold_prev = true;
}

/*
 * The replaced leg of a consult transfer, kept alive instead of BYEd (RFC
 * 5589). On success the target the referred leg reached replaces this leg, so
 * the network ends its dialog with a Replaces-driven BYE: BYEing it here first
 * destroys the dialog that BYE needs, answers it 481, and confuses the
 * softswitch's own transfer bookkeeping. The CALL_XFER_DRAIN_MS guard BYEs
 * the leg if the network never does.
 */
static void xfer_leg_drain(struct call *c)
{
	xfer_leg_silence(c);
	if (c->env)
		sip_timer_arm(c->env, &c->t_bye, CALL_XFER_DRAIN_MS);
	leg_trace(c, "the replaced leg drains for the network Replaces BYE");
}

/* the drain form of xfer_other_clear: it sets transferred the same way and
 * keeps the leg alive rather than BYEing it */
static void xfer_other_drain(uint32_t handle, bool transferred)
{
	struct call *o = leg_get(handle);

	if (!o)
		return;

	o->transferred = transferred;
	xfer_leg_drain(o);
}

/*
 * The 202 of a consult REFER. Both legs fall silent here and neither is BYEd:
 * the replaced leg drains for the network's Replaces-driven BYE, and the
 * referee waits for the verdict the NOTIFY carries (RFC 5589 7.1), bounded
 * by CALL_XFER_CONSULT_WAIT_MS. It runs in front of a stashed verdict, so a
 * NOTIFY that overtook this answer finds the same two legs the ordinary order
 * does.
 */
static void xfer_consult_settle(struct call *c)
{
	xfer_other_drain(c->xfer_other, true);
	xfer_leg_silence(c);
	leg_trace(c, "the referee waits for the transfer verdict");
}

/*
 * The 202 of a blind REFER, both the star-code form and the on-hook form. The
 * subscriber is done with the call the moment the far end takes the referral
 * on, and the report says so, but the leg is not BYEd: it falls silent and
 * keeps its dialog and its subscription, so the transferee's NOTIFYs are
 * answered 200, a switch that re-INVITEs the leg with c=0.0.0.0 behind the
 * 202 gets a 2xx (a switch can do both within 2 ms), and the
 * BYE comes from whoever owes it: the network's own, or this side's behind
 * the verdict. The UI occupancy goes with the report, because the drain is
 * bounded by CALL_XFER_WAIT_MS and a line held for that long would refuse
 * every inbound call for a subscriber who has left; leg_alloc() evicts the
 * drain when a new call needs the slot before then.
 */
static void xfer_blind_settle(struct call *c)
{
	c->transferred = true;
	xfer_leg_silence(c);
	c->ui_busy = false;
	if (c->env)
		sip_timer_arm(c->env, &c->t_xfer, CALL_XFER_WAIT_MS);
	leg_trace(c, "the referred leg drains for the verdict and the network BYE, "
		  "bounded by %u ms", CALL_XFER_WAIT_MS);
}

/*
 * The wait behind a 2xx verdict for the network's own BYE (call.h
 * CALL_XFER_BYE_GRACE_MS). The guard is consumed by then, so t_xfer is free
 * to carry it, and the flag tells timer_xfer_fire() which purpose fired. It
 * runs behind xfer_state_clear(), which cancels the timer and drops the flag.
 */
static void xfer_grace_arm(struct call *c)
{
	c->xfer_bye_grace = true;
	if (c->env)
		sip_timer_arm(c->env, &c->t_xfer, CALL_XFER_BYE_GRACE_MS);
	leg_trace(c, "the referee waits %u ms for the network BYE before its own",
		  CALL_XFER_BYE_GRACE_MS);
}

/*
 * What a decided transfer does to its legs, the half of xfer_outcome() that
 * touches them. The replaced-leg handle and the kind are read first, because
 * the clearing of the referee runs xfer_state_clear() and would take the
 * handle with it.
 *
 * A blind 202 settles the leg (xfer_blind_settle). Every other success ends
 * the subscription and arms the BYE grace on the referee: the attended
 * referee is silenced here, the blind and consult ones already were, and
 * every kind but the consult frees the UI occupancy, whose two legs keep
 * theirs until the protocol ends (call.h call_xfer_consult). A new NOTIFY
 * behind the verdict meets no subscription (RFC 3515, it ended) and is
 * answered 481; a retransmission keeps its cached 200.
 *
 * A failure leaves an attended or a star-code blind leg alive while the
 * subscriber is still on it, which is before any 202 for the blind kind: a
 * failure behind the 202 meets a drained leg nobody is left to resume, and
 * the on-hook blind kind has nobody on the line at all, so both BYE. The
 * consult kind BYEs the referee on every failure, and its replaced leg is
 * where the sources part: a verdict of failure and a refused REFER BYE it
 * too, because nothing replaces it, where the guard leaves it draining,
 * because a missing NOTIFY is no verdict at all and a self-BYE re-opens the
 * race the drain closed.
 */
static void xfer_decide(struct call *c, enum xfer_source src, bool ok, uint32_t code)
{
	uint32_t other = c->xfer_other;
	bool consult = c->xfer_kind == CALL_XFER_CONSULT;
	bool onhook = c->xfer_kind == CALL_XFER_ONHOOK;
	bool blind = onhook || c->xfer_kind == CALL_XFER_BLIND;

	if (blind && ok && src == XFER_SRC_REFER) {
		xfer_blind_settle(c);
		return;
	}

	if (ok) {
		if (!c->xfer_drain)
			xfer_leg_silence(c);
		if (!consult)
			c->ui_busy = false;
		c->transferred = true;
		xfer_state_clear(c);
		xfer_grace_arm(c);
		return;
	}

	if (consult) {
		xfer_leg_clear(c);
		if (src != XFER_SRC_GUARD)
			xfer_other_clear(other, false);
		return;
	}

	if (onhook || c->xfer_drain) {
		xfer_leg_clear(c);
		return;
	}

	xfer_state_clear(c);
}

/*
 * The one outcome of one transfer (call.h cb.xfer_done). The slot name and
 * the report debt are read before the legs are touched, because a BYE that
 * could not be sent releases the slot inside this frame: the report still
 * fires exactly once and still names the slot the transfer belonged to.
 *
 * The report is the last thing this frame does with the leg, and the leg may
 * not exist when it returns. The FSM's PENDING cell enters OFFHOOK_DIAL inside
 * the callback, a hotline dials there, and the slot pick can hand that dial
 * the physical slot this leg drains on, where leg_alloc() evicts it and wipes
 * the storage. Every caller returns right behind this function for the same
 * reason.
 *
 * The consult kind fires no xfer_done: the machine is idle and fire-and-forget
 * for this kind, and a report that waited on the verdict could land after the
 * replaced leg's slot was handed to a new call under the referee's old name,
 * where the IDLE CALL_XFER_FAIL cell would hang that new call up (call.h
 * call_xfer_consult). call_xfer_consult() never sets the debt.
 */
static void xfer_outcome(struct call *c, enum xfer_source src, bool ok, uint32_t code)
{
	unsigned int line = c->line, slot = c->ui_slot;
	bool owed = xfer_owed_take(c);

	leg_trace(c, "transfer %s with %u", ok ? "accepted" : "failed", code);
	xfer_decide(c, src, ok, code);

	if (owed && cv.cb.xfer_done)
		cv.cb.xfer_done(line, slot, ok, code);
}

/*
 * The referral was accepted. A blind transfer reports here:
 * the subscriber leaves the call whatever the transferee makes of the
 * referral, and the leg drains for the NOTIFYs that follow and for the BYE
 * (xfer_blind_settle). An attended one waits for the NOTIFY that carries the
 * outcome, bounded by the guard, because the REFER transaction ended with
 * this very answer and nothing below this layer bounds what comes next. A
 * consult settles both of its legs first and then waits the same way, under
 * its own shorter guard.
 *
 * A stashed verdict is released before the blind settle and not after it: a
 * verdict of failure that overtook the 202 belongs to a leg the subscriber is
 * still on, and the settle would silence it first.
 */
static void xfer_accept(struct call *c, uint32_t code)
{
	if (c->xfer_kind == CALL_XFER_CONSULT)
		xfer_consult_settle(c);

	/*
	 * A NOTIFY that overtook this answer already decided it. The referral
	 * is accepted, so the verdict it carried is genuine and it is the more
	 * specific of the two: it says what the target did, where this answer
	 * only says the transferee took the referral on.
	 */
	if (c->xfer_verdict) {
		leg_trace(c, "the REFER answer %u releases the verdict that overtook it",
			  code);
		xfer_outcome(c, (enum xfer_source)c->xfer_verdict_src, c->xfer_verdict_ok,
			     c->xfer_verdict_code);
		return;
	}

	if (c->xfer_kind == CALL_XFER_ATTENDED) {
		sip_timer_arm(c->env, &c->t_xfer, CALL_XFER_WAIT_MS);
		leg_trace(c, "REFER accepted with %u, the outcome is bounded by %u ms",
			  code, CALL_XFER_WAIT_MS);
		return;
	}

	if (c->xfer_kind == CALL_XFER_CONSULT) {
		sip_timer_arm(c->env, &c->t_xfer, CALL_XFER_CONSULT_WAIT_MS);
		leg_trace(c, "REFER accepted with %u, the verdict is bounded by %u ms",
			  code, CALL_XFER_CONSULT_WAIT_MS);
		return;
	}

	xfer_outcome(c, XFER_SRC_REFER, true, code);
}

/*
 * The credential retry builds the request again rather than replaying it,
 * because the credentials ride the same insert that writes its headers.
 *
 * A consult whose replaced leg died meanwhile fails with it. Rebuilding
 * without that leg would put a Refer-To with no Replaces on the wire, which is
 * a blind transfer to the same target: the subscriber asked to join two calls
 * and would get one handed away instead.
 */
static bool refer_resend(struct call *c)
{
	struct call *o = NULL;

	if (c->xfer_consult) {
		o = leg_get(c->xfer_other);
		if (!o)
			return false;
	}

	return refer_send(c, o);
}

/*
 * The answer to our REFER. A 401 or a 407 is answered once through the
 * dlg_auth_take pattern the BYE already follows.
 */
static void refer_final(struct call *c, const struct sip_msg *msg)
{
	uint32_t code = msg->status;

	c->refer_txn = SIP_HANDLE_NONE;

	if ((code == 401 || code == 407) && dlg_auth_take(c, msg) && refer_resend(c)) {
		leg_trace(c, "REFER resent with credentials");
		return;
	}

	if (code < 300) {
		xfer_accept(c, code);
		return;
	}

	xfer_outcome(c, XFER_SRC_REFER, false, code);
}

static void timer_xfer_fire(struct sip_timer *t)
{
	struct call *c = t->owner;

	/* the grace: the network sent no BYE of its own, so this side owes
	 * the one RFC 5589 section 6.1 puts on the transferor */
	if (c->xfer_bye_grace) {
		c->xfer_bye_grace = false;
		leg_trace(c, "no network BYE inside the grace, ending the referee");
		if (!bye_send(c))
			leg_end(c);
		return;
	}

	leg_trace(c, "the transfer guard expired with no outcome");
	xfer_outcome(c, XFER_SRC_GUARD, false, 408);
}

/*
 * The id of the implicit subscription is the CSeq of the REFER that created it
 * (RFC 3515 2.4.4). A NOTIFY that carries no id at all is accepted: a leg holds
 * at most one outstanding REFER, so there is exactly one subscription such a
 * NOTIFY could name, and a switch that omits the parameter would otherwise lose
 * every transfer outcome it reports.
 */
static bool xfer_event_id_ok(const struct call *c, const struct sip_event *ev)
{
	uint32_t id = 0;

	if (!ev->id.len)
		return true;

	return sip_str_uint(ev->id, &id) && id == c->refer_cseq;
}

/*
 * RFC 3515 2.4.5: the verdict rides a message/sipfrag body and nothing else. A
 * NOTIFY of this package carrying another type says nothing about the transfer,
 * and a body that merely looks like a status line must not decide one. The
 * media type is compared without case and its parameters are tolerated, which
 * is the rule call_cw_ctype_match() applies to the other body this leg reads.
 */
static bool sipfrag_ctype_is(const struct sip_msg *msg)
{
	struct sip_str ctype;

	if (!msg_hdr_value(msg, SIP_HDR_CONTENT_TYPE, &ctype))
		return false;

	return sip_str_eq_ci(sip_media_type(ctype), "message/sipfrag");
}

/*
 * One verdict, held back while the REFER itself is still outstanding. The
 * implicit subscription exists from the moment the REFER goes out (RFC 3515
 * 2.4.4), so on an unordered transport the final NOTIFY can overtake the answer
 * to that REFER. Applying it here would clear the leg for a referral that can
 * still be refused, and a 401 or a 407 answer would then retry on a leg that is
 * already going away.
 *
 * The answer to the REFER decides: its accept releases the stash, and its
 * failure discards it with the rest of the transfer state and wins.
 */
static void xfer_verdict_take(struct call *c, enum xfer_source src, bool ok, uint32_t code)
{
	if (c->refer_txn == SIP_HANDLE_NONE) {
		xfer_outcome(c, src, ok, code);
		return;
	}

	c->xfer_verdict = true;
	c->xfer_verdict_ok = ok;
	c->xfer_verdict_src = (uint8_t)src;
	c->xfer_verdict_code = code;
	leg_trace(c, "the transfer verdict %u arrived before the REFER was answered",
		  code);
}

/*
 * The NOTIFY of the implicit subscription (RFC 3515 2.4.4 and 2.4.6). Its body
 * is a message/sipfrag whose status line carries the verdict: a 2xx BYEs the
 * leg and reports the transfer done, and 300 or above reports it failed with
 * that status and leaves the call standing.
 *
 * A refer NOTIFY that names no outstanding transfer is answered 481: the
 * subscription it speaks of does not exist on this leg. Every other package is
 * acknowledged and dropped, because the account layer hands only a refer
 * NOTIFY to a dialog and a caller that drives this layer directly must not
 * turn a body we never read into a refusal.
 */
static void rx_notify(struct call *c, struct sip_txn *txn, const struct sip_msg *req)
{
	struct sip_sub_state st;
	struct sip_event ev;
	struct sip_str body;
	uint32_t code = 0;

	if (!hdr_event_get(req, &ev) || !sip_str_eq_ci(ev.package, SIP_SUB_EVENT_REFER)) {
		resp_simple(c->ua, txn, req, 200, c->local_tag);
		return;
	}

	if (!c->xfer_pending || !xfer_event_id_ok(c, &ev)) {
		leg_trace(c, "a refer NOTIFY names no outstanding transfer, 481");
		resp_simple(c->ua, txn, req, 481, c->local_tag);
		return;
	}

	/* every consumed NOTIFY is answered before the verdict is acted on, so
	 * a leg the verdict releases still answered the switch */
	resp_simple(c->ua, txn, req, 200, c->local_tag);

	if (sipfrag_ctype_is(req) && msg_body_get(req, &body) &&
	    sipfrag_status(body, &code) && code >= 200) {
		xfer_verdict_take(c, XFER_SRC_VERDICT, code < 300, code);
		return;
	}

	/*
	 * A subscription that ends with no final sipfrag leaves the transfer
	 * with no verdict at all. call.h names no status for that arm, so it
	 * takes the one the guard uses for the same fact, an outcome that never
	 * arrived.
	 */
	if (hdr_sub_state_get(req, &st) && sip_str_eq_ci(st.state, "terminated")) {
		leg_trace(c, "the transfer subscription ended with no final verdict");
		xfer_verdict_take(c, XFER_SRC_SUB_END, false, 408);
		return;
	}

	/* RFC 3515 2.4.5: the first NOTIFY carries "SIP/2.0 100 Trying", which
	 * is progress and not an outcome. The guard still bounds the wait. */
	leg_trace(c, "transfer progress %u", code);
}

/* the transferee intake, which needs the leg start below it (call.h) */
static void rx_refer(struct call *c, struct sip_txn *txn, const struct sip_msg *req);

/*
 * The PRACK of the reliable 180 this leg sent. A session description in it is
 * ignored: our 180 carries neither an offer nor an answer, so RFC 3262 5
 * leaves the body no part in the exchange, and the 200 carries none either.
 *
 * A PRACK that crosses our own final rejection never reaches this arm.
 * leg_end() released the leg and terminated its dialog, so call_rx_request()
 * answers it 481 where RFC 3262 3 asks for a 2xx. The peer holds the rejection
 * and has acknowledged it by then, and nothing it does next depends on the
 * answer.
 */
static void rx_prack(struct call *c, struct sip_txn *txn,
		     const struct sip_msg *req)
{
	enum sip_method method;
	uint32_t rseq, cseq;

	/* RFC 3262 3: the RAck triple names the one outstanding reliable
	 * provisional of this dialog, which the caller already matched */
	if (!c->rel_open || !hdr_rack_get(req, &rseq, &cseq, &method) ||
	    method != SIP_METHOD_INVITE || rseq != c->rel_rseq ||
	    cseq != c->rel_cseq) {
		leg_trace(c, "PRACK matches no reliable provisional, 481");
		resp_simple(c->ua, txn, req, 481, c->local_tag);
		return;
	}

	c->rel_open = false;
	sip_timer_cancel(c->env, &c->t_rel);
	leg_trace(c, "PRACK matched RSeq %u, retransmission stopped", rseq);
	resp_simple(c->ua, txn, req, 200, c->local_tag);
}

static void rx_in_dialog(struct call *c, struct sip_dialog *d, struct sip_txn *txn,
			 const struct sip_msg *req)
{
	switch (sip_dialog_remote_cseq_take(d, req)) {
	case SIP_DLG_CSEQ_OLD:
		/* RFC 3261 12.2.2 */
		resp_simple(c->ua, txn, req, 500, c->local_tag);
		return;
	case SIP_DLG_CSEQ_BAD:
		resp_simple(c->ua, txn, req, 400, c->local_tag);
		return;
	case SIP_DLG_CSEQ_OK:
		break;
	}

	switch (req->method) {
	case SIP_METHOD_BYE:
		rx_bye(c, txn, req);
		return;
	case SIP_METHOD_INVITE:
		rx_reinvite(c, txn, req);
		return;
	case SIP_METHOD_INFO:
		rx_info(c, txn, req);
		return;
	case SIP_METHOD_NOTIFY:
		rx_notify(c, txn, req);
		return;
	case SIP_METHOD_REFER:
		rx_refer(c, txn, req);
		return;
	case SIP_METHOD_PRACK:
		rx_prack(c, txn, req);
		return;
	default:
		/* OPTIONS in dialog: acknowledged, no effect.
		 * ua_request_policy() already refused every method we do not
		 * serve, so nothing unexpected reaches this point. */
		resp_simple(c->ua, txn, req, 200, c->local_tag);
		return;
	}
}

/*
 * The transport parameter of our Contact URI. It belongs inside the angle
 * brackets hdr_addr_write() writes, so it is part of the URI and not a header
 * parameter (RFC 3261 19.1.1). account.h hands the token in the upper case
 * Via form and leaves the lower case spelling to this writer.
 */
static void contact_transport_param(const struct call *c, char *out, size_t olen)
{
	size_t i;

	if (!c->transport[0]) {
		out[0] = 0;
		return;
	}

	snprintf(out, olen, ";transport=%s", c->transport);
	for (i = 0; out[i]; i++)
		out[i] = (char)tolower((unsigned char)out[i]);
}

static void leg_ident_take(struct call *c, const struct acct_call_view *view)
{
	const struct cfg_channel *ch = &cv.cfg->channel[c->line];
	char param[sizeof(";transport=") + CALL_TRANSPORT_MAX];

	c->mgr = view->mgr;
	c->env = view->env;
	c->ua = view->ua;
	c->dialogs = view->dialogs;

	str_copy(c->local_host, sizeof(c->local_host), view->local_host);
	str_copy(c->domain, sizeof(c->domain), view->domain);
	str_copy(c->user, sizeof(c->user), view->user);
	str_copy(c->display, sizeof(c->display), view->display);
	str_copy(c->auth_user, sizeof(c->auth_user), view->auth_user);
	str_copy(c->auth_pass, sizeof(c->auth_pass), view->auth_pass);
	str_copy(c->user_agent, sizeof(c->user_agent), view->user_agent);
	str_copy(c->transport, sizeof(c->transport), view->transport);
	str_copy(c->bind_dev, sizeof(c->bind_dev), view->bind_dev);
	c->punch_interval_ms = view->punch_interval_ms;
	c->local_port = view->local_port;
	if (view->dst)
		c->dst = *view->dst;
	if (view->service_route)
		c->service_route = *view->service_route;

	/* cfg lifetime rule: the leg owns everything it reads after this. */
	c->ch = *ch;
	c->rtcp_enabled = cv.cfg->rtcp_enabled;
	c->rtcp_interval = cv.cfg->rtcp_interval;
	str_copy(c->alert_prefix, sizeof(c->alert_prefix),
		cv.cfg->alert_info_prefix[0] ? cv.cfg->alert_info_prefix :
					       SIP_ALERT_PREFIX_DEFAULT);

	contact_transport_param(c, param, sizeof(param));
	/*
	 * RFC 3261 19.1.1 wraps an IPv6 host in square brackets inside a URI,
	 * and hdr_addr_write() writes this text as it stands. dlocal.host below
	 * keeps the bare address instead: hdr_via_write() brackets a sent-by
	 * itself, and dialog.c writes the URI this line just built.
	 */
	if (sip_host_bracket_needed(c->local_host))
		snprintf(c->contact, sizeof(c->contact), "sip:%s@[%s]:%u%s",
			 c->user, c->local_host, c->local_port, param);
	else
		snprintf(c->contact, sizeof(c->contact), "sip:%s@%s:%u%s",
			 c->user, c->local_host, c->local_port, param);

	c->dlocal.host = c->local_host;
	/* the in-dialog requests dialog.c builds carry the same token */
	c->dlocal.transport = c->transport[0] ? c->transport : NULL;
	c->dlocal.contact = c->contact;
	c->dlocal.user_agent = c->user_agent[0] ? c->user_agent : NULL;
	c->dlocal.port = c->local_port;

	c->sess_min_se = c->ch.session_min_interval;
	c->sess_want = c->ch.session_max_expires;
}

static uint64_t sess_id_new(struct call *c)
{
	return ((uint64_t)sip_txn_rand(c->mgr) << 32) | sip_txn_rand(c->mgr);
}

/*
 * The two keys this side offers, drawn once and kept for the whole life of the
 * leg: every re-offer re-emits them, because a changed key stops and restarts
 * the running stream (media_program). One key per suite, indexed suite - 1
 * (shared/sdp.h, sdp_local).
 */
static bool srtp_keys_take(struct call *c)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(c->loc.srtp_key); i++) {
		if (!cv.ops.rand_bytes(cv.ops.priv, c->loc.srtp_key[i], SDP_SRTP_KEY_LEN))
			return false;
	}

	return true;
}

/*
 * Is this leg's own media address an IPv6 literal? The transport parses a
 * literal of either family into a sockaddr and answers false for a name, so
 * the family of that result is the whole answer and this layer keeps no
 * second parser of its own.
 */
static bool leg_host_ipv6(const struct call *c)
{
	struct sip_txn_dst dst;

	if (!sip_dst_set(&dst, c->local_host, 0))
		return false;

	return dst.sa.ss_family == AF_INET6;
}

static bool leg_media_take(struct call *c)
{
	uint16_t port = 0, min, max;

	/*
	 * The media of a leg runs in the family of its signalling (RFC 6157
	 * section 5). A line whose own address is IPv6 therefore needs a
	 * backend that carries IPv6 media: without HAL_FEAT_IPV6_MEDIA,
	 * stream_config_set rule 4 refuses every description this leg can
	 * build. The leg is refused here instead, the way a missing RTP port
	 * refuses it: 486 inbound and congestion outbound. The test runs in
	 * front of the port draw, because a leg no backend can serve must hold
	 * no port, and the release path already tolerates a leg that drew none.
	 */
	if (leg_host_ipv6(c) && !(cv.hal_features & HAL_FEAT_IPV6_MEDIA)) {
		leg_trace(c, "the backend carries no IPv6 media");
		return false;
	}

	/* the same effective range the firewall reconcile opens */
	cv.host->net_port_range(cv.host->ctx, &c->ch, &min, &max);
	if (!cv.host->net_port_alloc(cv.host->ctx, min, max, &port))
		return false;

	c->rtp_port = port;
	/*
	 * The codec capability of this leg, read once at the leg start under
	 * the same rule as the two gates below: the shell pushes the mask
	 * the backend probed and a codec the board cannot run never reaches an
	 * offer, however the config names it.
	 */
	media_sdp_local_build(cv.cfg, &c->ch, cv.hal_codecs, c->local_host, port,
			      &c->loc);
	/*
	 * The T.38 enable of this leg. Both gates are read once, at the leg
	 * start, and live in the leg for its whole life: the channel option
	 * follows the cfg lifetime rule like every other option the leg reads,
	 * and the backend capability cannot change under a running call. A leg
	 * that answers false here offers no image stream and refuses every one
	 * the peer offers, which is the shipped behavior of every board without
	 * the feature.
	 */
	c->loc.t38 = c->ch.fax_t38_enable && (cv.hal_features & HAL_FEAT_T38);

	/*
	 * The SRTP enable of this leg, read once at the leg start under the
	 * same rule as the T.38 pair above: the base config
	 * switch and the backend feature bit, so a backend without SRTP runs
	 * plain RTP under a config that asks for it (cfg.h, srtp_enabled).
	 */
	c->loc.srtp = cv.cfg && cv.cfg->srtp_enabled && (cv.hal_features & HAL_FEAT_SRTP);

	/*
	 * The Annex B capability of this leg, read once at the leg start like
	 * the two gates above. No config switch joins it: silence_suppression
	 * keeps its TR-104 meaning and gates our transmitter alone, while this
	 * bit says whether the media plane carries the coder at all
	 * (shared/sdp.h, sdp_local.g729b). A backend without it writes
	 * annexb=no on every G.729 rtpmap and never arms the DTX, which is the
	 * truthful answer for a coder whose Annex B is unverified.
	 */
	c->loc.g729b = (cv.hal_features & HAL_FEAT_G729B) != 0;

	/* a caller that injected no entropy op gets a plain call, not a refusal
	 * (call.h, rand_bytes) */
	if (c->loc.srtp && !cv.ops.rand_bytes) {
		c->loc.srtp = false;
		leg_trace(c, "no entropy op is bound, so this leg offers plain RTP");
	}

	/* the same refusal a missing RTP port takes: 486 inbound, congestion
	 * outbound. A leg cannot offer a key it could not draw. */
	if (c->loc.srtp && !srtp_keys_take(c)) {
		leg_trace(c, "the entropy op gave no SRTP master key");
		return false;
	}

	return c->loc.codecs > 0;
}

/*
 * One ring template vocabulary, 0 to 7. Every Alert-Info dialect the parse
 * layer knows folds into it right here, so sip/line.c joins one id against the
 * region rows and the built-in GR-506 table and never learns which switch
 * spoke. 0 is the default ring.
 *
 * The BroadWorks call waiting INFO carries a cadence selector of its own
 * (call_cw_parse). This id and that selector feed two outputs, the alerting
 * ring and the call waiting tone; sip/line.c resolves a call waiting variant
 * from whichever the waiting call carried (cw_variant_of_ring_id,
 * cw_net_variant).
 */
/*
 * The Sylantro alert token map. alert-autoanswer, the bare answer token and
 * alert-visual are deliberately absent: the first two are auto-answer behavior
 * classes and the third is a visual alert, so none of them names a ring
 * pattern. A value that carries one does not end the walk below.
 */
#define SYLANTRO_RING_ROWS \
	X("alert-internal", false, 1) \
	X("alert-external", false, 2) \
	X("alert-priority", false, 3) \
	X("alert-emergency", false, 3) \
	X("alert-acd", false, 4) \
	X("alert-community-", true, 4) \
	X("alert-group", false, 4)

static const struct {
	const char *token;
	bool prefix;
	uint32_t ring;
} sylantro_rings[] = {
#define X(token, prefix, ring)	{ token, prefix, ring },
	SYLANTRO_RING_ROWS
#undef X
};

/* a table row that named a template sip/line.c cannot join would ring the
 * default */
#define X(token, prefix, ring) \
	_Static_assert((ring) < HAL_REGION_RING_MAX, "ring template out of range");
SYLANTRO_RING_ROWS
#undef X

static uint32_t sylantro_ring_of(struct sip_str token)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(sylantro_rings); i++) {
		const char *want = sylantro_rings[i].token;
		bool hit = sylantro_rings[i].prefix ? sip_str_prefix_ci(token, want) :
						      sip_str_eq_ci(token, want);

		if (hit)
			return sylantro_rings[i].ring;
	}

	return 0;
}

/*
 * The Nortel CS2K map. The index is the dialect scoped digit shared/hdr.h
 * describes: standard_ring is the plain ring 1, dst_ring_1 to dst_ring_3 take
 * the GR-506 numbers 2 to 4, splash_ring is the splash template 5, and
 * dst_ring_4 and dst_ring_5 take the two region rows above it. dst_ring_6 to
 * dst_ring_9 name cadences this product has no row for, so they stay 0 and
 * ring the default. The folding is reversible: nothing under this table knows a
 * Nortel name.
 */
#define NORTEL_RING_ROWS \
	X(1, 2) X(2, 3) X(3, 4) X(4, 6) X(5, 7) \
	X(SIP_ALERT_NORTEL_SPLASH, 5) \
	X(SIP_ALERT_NORTEL_STANDARD, 1)

static const uint32_t nortel_rings[SIP_ALERT_NORTEL_STANDARD + 1] = {
#define X(digit, ring)	[digit] = ring,
	NORTEL_RING_ROWS
#undef X
};

#define X(digit, ring) \
	_Static_assert((ring) < HAL_REGION_RING_MAX, "ring template out of range");
NORTEL_RING_ROWS
#undef X

static uint32_t nortel_ring_of(uint32_t ring)
{
	if (ring >= ARRAY_SIZE(nortel_rings))
		return 0;

	return nortel_rings[ring];
}

/*
 * The Bellcore digits 8 and 9 have no template either, and the NZ digits are
 * the templates themselves: the shipped NZ region rows carry the cadences.
 */
static uint32_t alert_ring_of(const struct sip_alert_info *ai)
{
	switch (ai->dialect) {
	case SIP_ALERT_BELLCORE:
		return ai->ring < HAL_REGION_RING_MAX ? ai->ring : 0;
	case SIP_ALERT_NORTEL:
		return nortel_ring_of(ai->ring);
	case SIP_ALERT_SYLANTRO:
		return sylantro_ring_of(ai->pattern);
	case SIP_ALERT_NZ:
		return ai->ring;
	case SIP_ALERT_NONE:
	case SIP_ALERT_OTHER:
		return 0;
	}

	/* the switch names every dialect, so a fifth one fails the build here
	 * instead of ringing the default silently */
	return 0;
}

static bool alert_dialect_known(const struct sip_alert_info *ai)
{
	return ai->dialect != SIP_ALERT_NONE && ai->dialect != SIP_ALERT_OTHER;
}

enum alert_pick {
	ALERT_PICK_NONE = 0,
	ALERT_PICK_PARSED,
	ALERT_PICK_DIALECT,
};

/*
 * The precedence over the values of every Alert-Info header the request
 * carries: the first value that names a ring template wins, else the first
 * value with a dialect, else the first value that parsed. A token this product
 * maps to no ring does not end the walk, because a walk that ended there
 * would lose a mapped token behind it, for example behind an
 * alert-autoanswer.
 *
 * It is read at the ring, behind every refusal, because its only consumers ride
 * the accepted path: the incoming callback carries the id, and sip/line.c joins
 * it against the ring templates.
 */
static void ring_id_take(struct call *c, const struct sip_msg *req)
{
	enum alert_pick rank = ALERT_PICK_NONE, want;
	struct sip_alert_info ai, pick;
	struct sip_val_iter it;
	uint32_t id;

	if (!hdr_alert_info_first(req, c->alert_prefix, &it, &ai))
		return;

	/* the first pass always ranks above ALERT_PICK_NONE, so pick is set
	 * before any exit that reads it */
	do {
		id = alert_ring_of(&ai);
		if (id) {
			c->ring_id = id;
			return;
		}

		want = alert_dialect_known(&ai) ? ALERT_PICK_DIALECT : ALERT_PICK_PARSED;
		if (want > rank) {
			rank = want;
			pick = ai;
		}
	} while (hdr_alert_info_next(&it, c->alert_prefix, &ai));

	if (rank == ALERT_PICK_DIALECT)
		leg_trace(c, "alert pattern " SIP_STR_PRI " names no ring, default ring",
			  SIP_STR_ARG(pick.pattern));
	else
		leg_trace(c, "alert info " SIP_STR_PRI " names no dialect, default ring",
			  SIP_STR_ARG(pick.url));
}

/*
 * The leg keeps what the SIP layer parsed and records the privacy verdict
 * beside it, instead of blanking the strings: the daemon is allowed to know a
 * number the subscriber may not see, and the backend needs the flags to render
 * the GR-30 reason letter rather than an empty field.
 *
 * The derivation itself is the hdr layer's: "Privacy: id" hides the identity
 * whatever header supplied it, because the RFC 3323 header is about the
 * message and not about one header of it.
 *
 * It is read in front of the four refusals that allocate a leg and release it
 * at once, because the ended report of a leg that never rang still has to name
 * who called.
 */
static void cid_take(struct call *c, const struct sip_msg *req)
{
	struct sip_cid cid;

	if (hdr_cid_get(req, &cid)) {
		sip_str_copy(c->cid_number, sizeof(c->cid_number), cid.number);
		sip_str_copy(c->cid_name, sizeof(c->cid_name), cid.name);
		c->cid_number_private = (cid.flags & SIP_CID_F_NUMBER_PRIVATE) != 0;
		c->cid_name_private = (cid.flags & (SIP_CID_F_NAME_PRIVATE |
						    SIP_CID_F_ANONYMOUS)) != 0;
	}
}

/* ------------------------------------------------ the transferee's REFER */

/*
 * The transferee side of RFC 3515: this line is referred somewhere else. It
 * answers 202, serves the implicit subscription with NOTIFY, and places the
 * child INVITE on the other physical slot, which then takes the UI name of the
 * leg it replaces.
 *
 * The FSM sees none of it, and that is the whole design: the machine is told a
 * call connected once, when the parent answered, and every report a leg makes
 * about the exchange is silenced by the one invariant of ui_emits().
 */

/*
 * One NOTIFY of the subscription this leg serves (RFC 3515 2.4.4 and 2.4.5).
 * The id is the CSeq of the REFER that created it, and the body is a sipfrag
 * carrying one status line and nothing else.
 *
 * No Refer-Sub is emitted and none is read (RFC 4488): this side always
 * notifies, which is the compliant shape.
 */
static bool xfer_notify_send(struct call *c, uint32_t code, bool final)
{
	char extra[96];
	char body[64];
	struct sip_buf b;

	snprintf(extra, sizeof(extra), "Event: refer;id=%u\r\nSubscription-State: %s\r\n",
		 c->refer_in_cseq, final ? "terminated;reason=noresource" : "active");

	sip_buf_init(&b, body, sizeof(body));
	if (!sipfrag_write(&b, code, NULL) || !sip_buf_ok(&b))
		return false;

	/* what a challenge makes this side build again (RFC 3261 22) */
	c->notify_code = (uint16_t)code;
	c->notify_final = final;

	return dialog_req_send(c, SIP_METHOD_NOTIFY, "message/sipfrag", body,
			       sip_buf_len(&b), true, &c->notify_txn, extra);
}

/* the referral this leg is the child of, decided here rather than a BYE drain
 * later; a parent that died first took its subscription with it */
static void xfer_child_failed(struct call *c, uint32_t code);

/*
 * The one final report of one referral, and refer_active is what makes it
 * exactly-once: every path that can decide the transfer comes through here, and
 * the second one finds the subscription already ended. A NULL leg is a parent
 * that died first, whose dialog took the subscription with it.
 */
static void xfer_notify_final(struct call *c, uint32_t code)
{
	if (!c || !c->refer_active)
		return;

	c->refer_active = false;
	c->xfer_kid = SIP_HANDLE_NONE;
	if (!xfer_notify_send(c, code, true))
		leg_trace(c, "the final transfer NOTIFY %u could not be sent", code);
}

/*
 * Parent suppression starts when the child INVITE is sent and not at the
 * child's 200: the transferor BYEs a blindly transferred leg the moment it
 * has our 202, while the child is still ringing, and a remote_bye reported
 * from there would tear the line to IDLE and orphan the child.
 *
 * Demotion cancels the three timers. A session watchdog firing on
 * this leg inside the guard window would otherwise report a failure for a UI
 * slot the child now owns and clear a call that is up.
 */
static void xfer_child_failed(struct call *c, uint32_t code)
{
	if (!c->xfer_child)
		return;

	xfer_notify_final(leg_get(c->xfer_parent), code);
}

static void ui_demote(struct call *c)
{
	if (c->ui_role != CALL_UI_OWNER)
		return;

	c->ui_role = CALL_UI_QUIET;
	sess_stop(c);
	if (c->env)
		sip_timer_cancel(c->env, &c->t_c);
	leg_trace(c, "demoted: a child leg answers under UI slot %u", c->ui_slot);
}

/*
 * The child failed, so the leg it was going to replace speaks again.
 *
 * There is no media to resume. This leg's stream is stopped at the swap alone,
 * which is the path where the child succeeded, so a failure finds it running
 * and the subscriber never lost the audio. What has to come back are the
 * session timers demotion cancelled, and sess_arm() re-derives both from the
 * interval this leg negotiated.
 */
static void ui_undemote(struct call *c)
{
	if (!c || c->ui_role != CALL_UI_QUIET)
		return;

	c->ui_role = CALL_UI_OWNER;
	sess_arm(c);
	leg_trace(c, "the child leg failed, so this leg speaks for UI slot %u again",
		  c->ui_slot);
}

/*
 * The swap itself, shared by the two legs that make one: the transferee's child
 * below and the leg an INVITE with Replaces created (RFC 3891). The replaced
 * stream goes down first, then the name moves, then the leg that took it over
 * speaks for it. Every caller needs those three in that order, and a second
 * copy of them would drift the day one of the two roles changes.
 *
 * A replaced leg that is already gone leaves the first two undone and nothing
 * to undo: its stream stopped with it, and the name passed to this leg at its
 * release.
 */
static void ui_name_take(struct call *c, struct call *p)
{
	if (p) {
		media_stop(p);
		p->transferred = true;
		p->ui_role = CALL_UI_NONE;
		c->ui_slot = p->ui_slot;
	}

	c->ui_role = CALL_UI_OWNER;
	leg_trace(c, "the transferred call took UI slot %u over", c->ui_slot);
}

/*
 * The swap of a transferee's child. The BYE guard is armed in the same step so
 * the replaced leg cannot outlive the transfer whatever the transferor does
 * next; a parent that is already gone needs no guard either.
 */
static void xfer_child_promote(struct call *c)
{
	struct call *p = leg_get(c->xfer_parent);

	ui_name_take(c, p);
	if (p)
		sip_timer_arm(p->env, &p->t_bye, CALL_XFER_BYE_MS);
}

/*
 * What follows a swap the stream survived: the referral is reported done, and
 * a leg the subscriber had put on hold is re-offered as held, so the hold state
 * the machine still believes is the state the wire carries. That re-INVITE is
 * ours and reports nothing, exactly as a session refresh does.
 */
static void xfer_child_settle(struct call *c)
{
	xfer_notify_final(leg_get(c->xfer_parent), 200);

	if (!c->xfer_held)
		return;

	/* The latch is consumed here. It answers "the leg this one replaced
	 * was held", which is true once and settled once: a second settle on
	 * the same leg would re-offer a hold the wire already carries. Both
	 * roles clear it here, so neither can repeat it. */
	c->xfer_held = false;
	if (!op_start(c, CALL_OP_XFER_HOLD, true))
		leg_trace(c, "the replaced leg was held and this one could not re-offer the hold");
}

static bool leg_hangup(struct call *c);

/* the child of a referral nobody is waiting for any more */
static void xfer_child_drop(struct call *c, uint32_t code)
{
	struct call *ch = leg_get(c->xfer_kid);

	xfer_notify_final(c, code);
	if (!ch)
		return;

	leg_trace(ch, "the referral was abandoned, so this child goes with it");
	ch->xfer_parent = SIP_HANDLE_NONE;
	leg_hangup(ch);
}

/*
 * The other half of a transfer, told at the moment this leg is released. It
 * runs in front of everything else leg_release() does, so this leg still has a
 * dialog to notify on and the child it shadows can still be handed a name.
 */
static void xfer_release_take(struct call *c)
{
	struct call *o;

	if (c->xfer_child) {
		o = leg_get(c->xfer_parent);
		c->xfer_parent = SIP_HANDLE_NONE;

		/* a child that never took the name over failed, whatever ended
		 * it; 503 is what a caller is told when no status from the
		 * target says why, because none arrived */
		if (o && c->ui_role != CALL_UI_OWNER) {
			xfer_notify_final(o, 503);
			ui_undemote(o);
		}

		return;
	}

	o = leg_get(c->xfer_kid);
	c->xfer_kid = SIP_HANDLE_NONE;
	/* the subscription lived in the dialog this leg is dropping */
	c->refer_active = false;
	if (!o)
		return;

	if (c->ui_role != CALL_UI_QUIET || o->ui_role != CALL_UI_NONE)
		return;

	/*
	 * A child that cannot speak must not be handed the name. One that has
	 * already failed or is clearing will never report anything again, so
	 * the machine would sit in a call with no leg behind it forever: this
	 * leg keeps the name instead and reports the ending it owes, which is
	 * the rule calls_line_teardown() applies for the same reason.
	 */
	if (o->reported || leg_clearing(o)) {
		c->ui_role = CALL_UI_OWNER;
		leg_trace(c, "the child cannot take UI slot %u, so this leg keeps it",
			  c->ui_slot);
		return;
	}

	/*
	 * The name passes to the child now (the blind arm). The transferor
	 * BYEs this leg the moment it accepts the referral, long before the
	 * target answers, and a child left holding no name would have nothing
	 * to report its own outcome on. The subscriber hears silence from here
	 * until the child answers.
	 *
	 * The record of this leg carries the transfer key from here. It is the
	 * blind arm, which is the common arm, and the leg ends without ever
	 * reaching the swap that would otherwise have set it: a referral took
	 * its call away, whatever the child makes of it.
	 */
	o->ui_slot = c->ui_slot;
	o->ui_role = CALL_UI_OWNER;
	c->ui_role = CALL_UI_NONE;
	c->transferred = true;
	leg_trace(o, "the replaced leg is gone, so this child holds UI slot %u early",
		  o->ui_slot);
}

/*
 * The replaced leg is ended without a word to the machine. The name it held
 * belongs to another leg from the swap, so the FSM is owed no outcome for it,
 * and the UI occupancy goes with the name: the line admits a new call again the
 * moment this runs, while the dialog drains its BYE on its own reference.
 *
 * It has three callers, one per role: the transferee's guard, which ends a leg
 * the transferor promised to end and did not; the target's takeover, where
 * RFC 3891 4 puts the BYE on this side from the start; and the consult
 * transferor's drain guard, which ends its own replaced leg when the network
 * never sends the Replaces-driven BYE (CALL_XFER_DRAIN_MS).
 */
static void xfer_replaced_end(struct call *c)
{
	c->reported = true;
	c->ui_busy = false;
	if (!bye_send(c))
		leg_end(c);
}

static void timer_bye_fire(struct sip_timer *t)
{
	struct call *c = t->owner;

	leg_trace(c, "the replaced leg was never ended by the peer, ending it here");
	xfer_replaced_end(c);
}

/* the Refer-To target, stripped of the URI headers that ride behind its '?' */
static bool refer_target_take(const struct sip_addr *a, char *out, size_t olen)
{
	struct sip_str uri = a->uri.raw;

	if (a->uri.headers.len && a->uri.headers.p > uri.p)
		uri.len = (size_t)(a->uri.headers.p - 1 - uri.p);

	return sip_str_copy(out, olen, uri);
}

/*
 * Whether the URI headers carry a Replaces at all. uri_headers_get() answers
 * false both for a value it refused and for one that is not there, and the two
 * must never be treated alike: a Replaces this side could not read still means
 * an attended transfer, and completing it without the header would hand the
 * subscriber's call away blind instead of joining the two calls.
 */
static bool refer_replaces_present(struct sip_str headers)
{
	static const char want[] = "Replaces=";
	size_t n = sizeof(want) - 1, i;

	for (i = 0; i + n <= headers.len; i++) {
		if (i && headers.p[i - 1] != '&')
			continue;
		if (sip_str_eq_ci(sip_str_init(headers.p + i, n), want))
			return true;
	}

	return false;
}

/*
 * A header value this side copies verbatim must carry no control byte. The
 * parser unfolds, so a CR or an LF cannot reach a stored value, but every other
 * byte below 0x20 and the DEL can, and this one value is written into a request
 * of ours rather than read: uri_headers_get() screens the same class on the
 * other half of the referral, and this is that rule on the half that arrives as
 * a plain header.
 */
static bool ctl_bytes_has(const char *s)
{
	for (; *s; s++) {
		if ((unsigned char)*s < 0x20 || (unsigned char)*s == 0x7f)
			return true;
	}

	return false;
}

/*
 * The child's header block. An embedded Replaces is promoted to exactly one
 * header field, and Require: replaces
 * rides with it (RFC 3891 4) so a target that cannot replace a dialog refuses
 * instead of ringing a second call. Referred-By is copied as it arrived
 * (RFC 3892 5).
 *
 * The promotion resurrects nothing. uri_headers_get() refused the broken
 * escape and every value that decodes to a control byte before this ran, so the
 * %0D%0A that would inject a header line and the %00 that would truncate one
 * never reach this buffer.
 *
 * A Referred-By that does not fit is dropped and the referral stands. RFC 3892
 * makes the header informational, so a display name wider than this side's
 * budget must not cost the subscriber the transfer.
 */
static bool xfer_child_hdrs_build(struct call *c, const struct sip_msg *req,
				  const char *replaces, char *out, size_t olen)
{
	char by[CALL_REFERRED_BY_MAX];
	struct sip_buf b;
	struct sip_str s;

	sip_buf_init(&b, out, olen);

	if (*replaces) {
		hdr_write(&b, SIP_HDR_REPLACES, "%s", replaces);
		hdr_write(&b, SIP_HDR_REQUIRE, "replaces");
	}

	if (msg_hdr_value(req, SIP_HDR_REFERRED_BY, &s)) {
		if (!sip_str_copy(by, sizeof(by), s))
			leg_trace(c, "the Referred-By does not fit, the child carries none");
		else if (ctl_bytes_has(by))
			leg_trace(c, "the Referred-By carries a control byte, the child "
				  "carries none");
		else
			hdr_write(&b, SIP_HDR_REFERRED_BY, "%s", by);
	}

	return sip_buf_ok(&b);
}

/* the account half of every new INVITE this line sends; the referral child
 * reads it alone, so a failed refresh does not refuse a transfer inside a
 * call */
static bool dial_view_get(unsigned int line, struct acct_call_view *view)
{
	return account_call_view(line, view) && view->active && view->dst_valid;
}

/* the dial funnel and the seize gate (call.h calls_line_dial_ready): a live
 * registration too */
static bool dial_ready_view_get(unsigned int line, struct acct_call_view *view)
{
	return dial_view_get(line, view) && view->registered;
}

/*
 * The INVITE the referral asked for, on the other physical slot and holding no
 * UI name of its own: it is a leg the FSM never asked for and must never learn
 * about until it wears the name of the one it replaces.
 *
 * The identity of the line is unchanged. A referral moves the far end, not who
 * this subscriber is, so the From, the Contact and the caller id are the ones
 * every other call of this line carries.
 */
static bool xfer_child_start(struct call *p, unsigned int slot, const char *ruri,
			     const char *hdrs)
{
	struct acct_call_view view;
	struct call *c;

	if (!dial_view_get(p->line, &view))
		return false;

	c = leg_alloc(p->line, slot);
	c->ui_role = CALL_UI_NONE;
	c->xfer_child = true;
	c->xfer_parent = leg_handle(p);
	c->uac = true;
	/*
	 * The presentation of the child, in two arms.
	 *
	 * An outbound parent carries the subscriber's own verdict, because
	 * call_dial() is where sip/line.c resolves one, and the child inherits
	 * it: somebody who withheld their number on the call being referred
	 * does not present it on the call the referral places.
	 *
	 * An inbound parent carries no verdict at all. The field is never set
	 * on a leg this line answered, so inheriting it would read the zero of
	 * the allocation as a decision, and the daemon's default is to present
	 * the number: the RFC 3323 anonymous form is opt in. Such a child keeps
	 * the line identity.
	 *
	 * This layer does not read the persistent withhold state of sip/line.c.
	 */
	c->cid_present = p->uac ? p->cid_present : true;
	leg_ident_take(c, &view);
	str_copy(c->number, sizeof(c->number), ruri);
	str_copy(c->xfer_hdrs, sizeof(c->xfer_hdrs), hdrs);

	if (!leg_media_take(c)) {
		leg_trace(c, "no RTP port or no codec for the transferred call");
		leg_release(c);
		return false;
	}

	snprintf(c->from_uri, sizeof(c->from_uri), "sip:%s@%s", c->user, c->domain);
	if (!c->cid_present) {
		str_copy(c->from_uri, sizeof(c->from_uri), CALL_ANON_URI);
		str_copy(c->display, sizeof(c->display), CALL_ANON_DISPLAY);
		leg_trace(c, "the transferred call withholds the caller id too");
	}
	str_copy(c->to_uri, sizeof(c->to_uri), ruri);
	str_copy(c->ruri, sizeof(c->ruri), ruri);
	snprintf(c->call_id, sizeof(c->call_id), "%08x%08x@%s", sip_txn_rand(c->mgr),
		 sip_txn_rand(c->mgr), c->local_host);

	if (!ua_tag_new(c->ua, c->local_tag, sizeof(c->local_tag))) {
		leg_release(c);
		return false;
	}

	sdp_state_init(&c->sdp, sess_id_new(c));
	c->cseq = 1;

	if (!invite_send(c)) {
		leg_trace(c, "the transferred call could not be placed");
		leg_release(c);
		return false;
	}

	p->xfer_kid = leg_handle(c);
	c->xfer_held = sdp_local_hold(&p->sdp);
	ui_demote(p);

	/* the record every leg is owed, for the one leg no FSM command and no
	 * offer announced, correlated to the record of the leg whose call it
	 * was placed to take over (call.h cb.started) */
	if (cv.cb.started)
		cv.cb.started(c->line, c->slot, false, p->slot);

	return true;
}

/* a refusal whose reason phrase this module names itself: hdr.c carries no row
 * for the two codes the referral grammar answers with, and a transferor reading
 * "416 Client Error" learns nothing from it */
static void resp_reason_send(struct call *c, struct sip_txn *txn, const struct sip_msg *req,
			     uint32_t code, const char *reason)
{
	struct ua_resp r;

	memset(&r, 0, sizeof(r));
	r.code = code;
	r.reason = reason;
	r.to_tag = c->local_tag[0] ? c->local_tag : NULL;
	resp_send(c->ua, txn, req, &r);
}

/* RFC 3515 2.4: the 2xx that accepts a REFER carries our Contact, the header
 * the request itself owes as well */
static void resp_202_send(struct call *c, struct sip_txn *txn, const struct sip_msg *req)
{
	struct ua_resp r;

	memset(&r, 0, sizeof(r));
	r.code = 202;
	r.to_tag = c->local_tag;
	r.contact = c->contact;
	resp_send(c->ua, txn, req, &r);
}

/*
 * An in-dialog REFER (RFC 3515 2.4.2). The order below is the contract: every
 * refusal is decided before the 202, so an accepted referral is one this side
 * has already found a slot and a usable target for.
 *
 * An out-of-dialog REFER never reaches here: call_rx_request() answers 481 from
 * its no-dialog tail, which is the RFC 3261 12.2.2 answer and costs nothing.
 */
static void rx_refer(struct call *c, struct sip_txn *txn, const struct sip_msg *req)
{
	char replaces[CALL_REPLACES_MAX] = { 0 };
	char hdrs[CALL_XFER_HDRS_MAX];
	char ruri[CALL_URI_MAX];
	struct sip_addr to;
	unsigned int slot = 0;
	uint32_t cseq = 0;
	bool secure;

	/*
	 * One exchange at a time on one leg, and this leg fails that test three
	 * ways. The state is the rule rx_reinvite() takes for the same reason.
	 *
	 * A leg that is already referring somebody else is the second: the FSM
	 * put it in PENDING{XFER} and is waiting for the one outcome call.h
	 * promises, and serving an inbound referral would demote the leg that
	 * outcome has to be reported on.
	 *
	 * A leg that holds no UI name is the third: the child of a referral
	 * before it takes the name over, and the parent once it has. Serving a
	 * referral there would place a child no name will ever reach, and its
	 * own child would then shadow a leg the machine does not know about.
	 */
	if (!leg_free_is(c) || c->xfer_pending ||
	    c->refer_txn != SIP_HANDLE_NONE || !ui_holds(c)) {
		leg_trace(c, "REFER on a leg that is not free to take one, 491");
		resp_simple(c->ua, txn, req, 491, c->local_tag);
		return;
	}

	if (c->refer_active) {
		leg_trace(c, "a referral is already served on this leg, 486");
		resp_simple(c->ua, txn, req, 486, c->local_tag);
		return;
	}

	if (!hdr_addr_get(req, SIP_HDR_REFER_TO, &to) || !to.uri.scheme.len) {
		leg_trace(c, "the REFER carries no Refer-To, 400");
		resp_simple(c->ua, txn, req, 400, c->local_tag);
		return;
	}

	secure = (to.uri.flags & SIP_URI_F_SECURE) != 0;
	if (!secure && !sip_str_eq_ci(to.uri.scheme, "sip")) {
		leg_trace(c, "the Refer-To names no sip scheme, 416");
		resp_reason_send(c, txn, req, 416, "Unsupported URI Scheme");
		return;
	}

	/*
	 * A sips: target reached over a cleartext account would put a cleartext
	 * INVITE on the wire toward a URI whose scheme demands TLS end to end.
	 * The account transport is the leg's own snapshot.
	 */
	if (secure && strcmp(c->transport, "TLS")) {
		leg_trace(c, "a sips: Refer-To needs a TLS account, 416");
		resp_reason_send(c, txn, req, 416, "Unsupported URI Scheme");
		return;
	}

	if (!to.uri.host.len) {
		leg_trace(c, "the Refer-To URI cannot be read, 400");
		resp_simple(c->ua, txn, req, 400, c->local_tag);
		return;
	}

	/* the capacity limit: this board carries two legs per line and a
	 * transferred call needs one of its own */
	if (!call_slot_free(c->line, &slot)) {
		leg_trace(c, "no free slot for the transferred call, 486");
		resp_simple(c->ua, txn, req, 486, c->local_tag);
		return;
	}

	if (refer_replaces_present(to.uri.headers) &&
	    !uri_headers_get(to.uri.headers, "Replaces", replaces, sizeof(replaces))) {
		leg_trace(c, "the embedded Replaces cannot be used, 400");
		resp_simple(c->ua, txn, req, 400, c->local_tag);
		return;
	}

	/* the refuse-not-truncate rule of this layer, at a target that arrives
	 * from the wire: half a URI names a party nobody meant */
	if (!refer_target_take(&to, ruri, sizeof(ruri)) ||
	    !xfer_child_hdrs_build(c, req, replaces, hdrs, sizeof(hdrs))) {
		leg_trace(c, "the Refer-To does not fit a request this side can send, 414");
		resp_reason_send(c, txn, req, 414, "Request-URI Too Long");
		return;
	}

	/* RFC 3515 2.4.4: the CSeq of the REFER is the id of the subscription
	 * it creates, so every NOTIFY of this transfer names it */
	hdr_cseq_get(req, &cseq, NULL);
	c->refer_in_cseq = cseq;
	c->refer_active = true;
	/* the NOTIFY exchange gets its own credential budget, the rule every
	 * other in-dialog exchange of this leg follows */
	memset(c->dlg_auth_retry, 0, sizeof(c->dlg_auth_retry));

	resp_202_send(c, txn, req);
	leg_trace(c, "referral to %s accepted with id %u", ruri, cseq);

	if (!xfer_notify_send(c, 100, false))
		leg_trace(c, "the first transfer NOTIFY could not be sent");

	if (!xfer_child_start(c, slot, ruri, hdrs))
		xfer_notify_final(c, 503);
}

/* ------------------------------------------- the target's INVITE Replaces */

/*
 * The fifth REFER role (RFC 3891): an INVITE that carries a Replaces is the
 * completion of an attended transfer at the party the call is transferred to.
 * It does not offer a new call; it asks this line to move the call it already
 * has onto a new dialog.
 *
 * The FSM sees nothing of it, exactly as it sees nothing of the transferee's
 * exchange, and for the same reason: no name changes hands as far as the
 * machine is concerned. The two roles therefore share one set of machinery,
 * ui_name_take() and the shadow-leg fields, and differ in three facts only:
 * the new leg arrives instead of dialling, it is answered without ringing, and
 * the BYE for the replaced dialog is owed by this side (RFC 3891 4).
 */

static bool leg_answer(struct call *c, bool media);

/*
 * The dialog a Replaces names (RFC 3891 3). The tags are oriented at the
 * receiver: to-tag is compared with our local tag and from-tag with the peer's,
 * which is the orientation shared/hdr.h documents and the transferor writes.
 *
 * The three values are read from the dialog and not from the leg, for the
 * reason replaces_value_build() reads them there: a leg this line answered
 * never built a Call-ID or a tag of its own, and the dialog is the one place
 * that holds both orientations whichever side created it.
 *
 * Two classes of leg never match, and both are dialogs on their way out. A leg
 * that does not speak for its name has already been claimed by another
 * transfer, and a second claim would leave two legs waiting for one name. A leg
 * whose clearing is already on the wire is the first window of the BYE race:
 * the call the transferor meant is over, so the answer is the 481 of a dialog
 * that does not exist rather than a takeover of nothing.
 */
static struct call *replaces_match(unsigned int line, const struct sip_replaces *r)
{
	unsigned int i;

	for (i = 0; i < CALL_SLOTS; i++) {
		struct call *c = &calls[line][i];
		struct sip_dialog *d;

		if (!c->in_use || !c->ui_busy || !ui_emits(c) || leg_clearing(c))
			continue;

		d = leg_dialog(c);
		if (!d || !d->call_id || !d->local_tag || !d->remote_tag)
			continue;

		if (sip_str_eq(r->call_id, d->call_id) &&
		    sip_str_eq(r->to_tag, d->local_tag) &&
		    sip_str_eq(r->from_tag, d->remote_tag))
			return c;
	}

	return NULL;
}

/*
 * The admission reads Replaces before the line is refused for capacity, and
 * that is the governing rule of this file. A Replaces INVITE is not a new
 * call: it frees the leg it replaces, so a capacity refusal in front of the
 * match would answer 486 for every completion a busy line receives, and it
 * would answer 486 where the truthful answer is 481 for a dialog nobody has,
 * or 400 for a header nobody can read.
 *
 * 0 admits the INVITE and fills *out with the leg to replace, or with NULL when
 * the request carries no Replaces at all; anything else is the status this side
 * answers with and the INVITE goes no further.
 *
 * The four refusals:
 *
 *   400  the header cannot be read, or there is more than one of it
 *        (RFC 3891 3 names both). A malformed Replaces names no dialog, and
 *        ringing the subscriber instead would offer a call the transferor never
 *        placed.
 *   481  no dialog of this line matches, which is also what a dialog that is
 *        already ending answers.
 *   486  the match is an early dialog. This side replaces none: it would
 *        have to answer for a call it has not accepted yet.
 *   486  early-only is present and the match is confirmed (RFC 3891 7.1). The
 *        parameter asks for an early dialog alone, and this side replaces none,
 *        so the tag can never be honored.
 */
static uint32_t replaces_admit(unsigned int line, const struct sip_msg *req,
			       struct call **out)
{
	struct sip_replaces rep;
	struct sip_str v;
	struct call *p;
	unsigned int n;

	*out = NULL;
	n = hdr_val_count(req, SIP_HDR_REPLACES);
	if (!n)
		return 0;

	if (n > 1 || !msg_hdr_value(req, SIP_HDR_REPLACES, &v) ||
	    !hdr_replaces_parse(v, &rep))
		return 400;

	p = replaces_match(line, &rep);
	if (!p)
		return 481;

	if (sip_dialog_state(leg_dialog(p)) != SIP_DLG_CONFIRMED)
		return 486;
	if (rep.early_only)
		return 486;

	*out = p;

	return 0;
}

/*
 * Where the leg of a Replaces answer goes (the capacity rule after the match).
 * The replaced leg keeps its own physical slot until the takeover BYEs it, so
 * the new leg needs one of its own and the allocator must not hand back the
 * slot the replaced leg is sitting on. A line with nothing else left is
 * refused 486, which is the same capacity limit the transferee's intake
 * applies: this board carries two legs per line.
 */
static bool replaces_slot_free(unsigned int line, const struct call *replaced,
			       unsigned int *slot)
{
	if (!call_slot_free(line, slot))
		return false;

	/* Unreachable today and kept: the match refuses a leg that is
	 * clearing, so the replaced leg is always UI-busy and the allocator
	 * above can never offer its slot. The test guards a future
	 * call_slot_free() that learns to. */
	return &calls[line][*slot] != replaced;
}

/*
 * The answer, and it rings nobody. The subscriber is already in the call this
 * INVITE replaces, so there is nothing to alert them about: no 180 goes out and
 * no cb.incoming is emitted.
 *
 * That bypasses the call waiting policy by design. Every other inbound INVITE
 * is offered to sip/line.c, which decides whether the subscriber may be alerted
 * at all; this one is not offered, so no policy of that layer can refuse it. It
 * is the correct reading of RFC 3891 3: the request names a call this line
 * already accepted, and a second decision about accepting it would be a
 * decision about a call the subscriber is already on.
 *
 * The replaced leg goes quiet at the 2xx and not at the takeover, which is the
 * suppression invariant read one moment early, and never late. From the 2xx
 * this side is committed: the leg that will wear the name exists and is
 * answered, so the invariant's own words apply, "a leg whose name another leg
 * owns or will own says nothing". It is also what
 * makes the second window of the BYE race work at all, because the orphan
 * promotion of leg_release() only hands the name on from a leg that is QUIET.
 *
 * The record edge is cb.started, for the reason a transferee's child needs one:
 * no FSM command dialled this leg and no offer rang it, so nothing else
 * announces the record it owes, and from_slot carries the leg whose call it
 * took over.
 */
static void replaces_answer(struct call *c, struct call *p)
{
	/*
	 * The name the admission found for this leg is discarded here. Every
	 * inbound leg is handed a free name at its allocation, because every
	 * other one is offered under it; this one is not offered to anybody, and
	 * the only name it will ever wear is the one the takeover moves onto it.
	 * The role is what makes that true for the resolver, the emission
	 * invariant and the status dump alike, so nothing between here and the
	 * ACK can find this leg by a name.
	 */
	c->ui_role = CALL_UI_NONE;
	c->xfer_child = true;
	c->xfer_parent = leg_handle(p);
	state_set(c, CALL_ST_RINGING_IN);
	leg_trace(c, "an INVITE replaces the call on slot %u, answering it", p->slot);

	/*
	 * No stream starts with this 2xx, which is the one place this answer
	 * differs from every other (call_answer: media normally starts with the
	 * 2xx so the first word is not clipped). The line carries one audio
	 * path and the leg being replaced is still on it until the ACK, so a
	 * stream started here would be the second one on that line. The
	 * takeover stops the replaced stream and starts this one, in that order.
	 */
	/*
	 * The replacement must survive its own answer. leg_answer() answers
	 * true for a 2xx that reached the wire and then abandoned the leg
	 * inside the same frame: the UA core had no record left for the
	 * retransmission, so the leg was cleared with a BYE and no ACK will
	 * ever arrive for it. Linking that leg would demote the call the
	 * subscriber is on and hand its name to a corpse, and the machine
	 * would sit in a call with no leg behind it.
	 *
	 * The replaced call is left exactly as it was, which is the whole point:
	 * it still owns its name, still speaks, and the transferor learns the
	 * replacement failed from the BYE the abandon already sent.
	 */
	if (!leg_answer(c, false) || !c->in_use || leg_clearing(c)) {
		leg_trace(c, "the replacement did not survive its own answer");
		return;
	}

	p->xfer_kid = leg_handle(c);
	c->xfer_held = sdp_local_hold(&p->sdp);
	ui_demote(p);

	if (cv.cb.started)
		cv.cb.started(c->line, c->slot, true, p->slot);
}

/*
 * The takeover, at the ACK of our own 2xx. The three steps are the
 * transferee's, through the shared ui_name_take(): the replaced stream goes
 * down, the name moves to this leg, and the replaced leg's record closes as a
 * transfer's.
 *
 * The one difference is who owes the BYE. A transferee waits CALL_XFER_BYE_MS
 * for the transferor to end the leg it referred, because that side owes it;
 * here RFC 3891 4 puts the BYE on the UA that accepted the replacement, so it
 * goes out in this frame and no guard is armed at all.
 *
 * A replaced leg that is already gone is the second window of the BYE race:
 * see the rule beside cb.started above. It handed this leg the name at its own
 * release, stopped its stream with it and owes no BYE, so the takeover is
 * already done and this is a no-op with a trace.
 */
static void replaces_takeover(struct call *c)
{
	struct call *p = leg_get(c->xfer_parent);

	ui_name_take(c, p);
	if (p)
		xfer_replaced_end(p);
}

/* RFC 3261 8.1.1.3: the From of a request carries a tag, and that tag is the
 * remote half of every dialog identifier this side builds from it. */
static bool from_tag_ok(const struct sip_msg *req)
{
	struct sip_addr from;

	return hdr_addr_get(req, SIP_HDR_FROM, &from) && from.tag.len;
}

static void rx_new_invite(unsigned int line, struct sip_txn *txn, const struct sip_msg *req,
			  const struct sip_txn_dst *src, const struct acct_call_view *view)
{
	enum sip_refresher who = SIP_REFRESHER_NONE;
	struct call *replaced = NULL;
	struct sip_str body;
	unsigned int slot = 0;
	uint32_t se = 0, min_se, code;
	struct call *c;

	/*
	 * The account is unregistering for a reload or a shutdown. It is not
	 * busy, it is going away, and the objects a leg would run on are about
	 * to be destroyed, so the caller is told to come back later. account.c
	 * refuses the same request before it ever reaches here; this is the
	 * check that holds when the call layer is driven directly.
	 */
	if (!view->active) {
		refused_note(view->env, line, 480);
		resp_simple(view->ua, txn, req, 480, NULL);
		return;
	}

	/*
	 * The line holds no live registration, so it takes no new call: the
	 * 480 that account.c answers in front of its slot check, held here too
	 * for a call layer driven directly (the IDLE SEIZE cell). A
	 * Replaces INVITE goes on to replaces_admit(), which admits it for a
	 * confirmed dialog of this line only. The answer sits in front of the
	 * 503, because a fact of the account holds whatever the hardware does.
	 */
	if (!view->registered && !hdr_val_count(req, SIP_HDR_REPLACES)) {
		refused_note(view->env, line, 480);
		resp_simple(view->ua, txn, req, 480, NULL);
		return;
	}

	/*
	 * The hardware cannot carry this call. The line shell wires this layer
	 * at start-up and the DSP finishes its bring-up milliseconds later, so
	 * a call can be offered to a line that cannot ring yet; a bring-up that
	 * failed never gets there at all, and the answer would ring a dead
	 * SLIC. Neither subscriber is busy, so neither answers 486: the caller
	 * is told the service is unavailable and when to come back. It sits in
	 * front of the slot check, because a line the hardware cannot serve is
	 * not busy even when both of its slots are, and behind the two 480s
	 * above: an account whose objects are about to be destroyed is going
	 * away for good rather than for now, and a missing binding is a fact of
	 * the account.
	 */
	if (cv.hal_ready && !cv.hal_ready()) {
		refused_note(view->env, line, 503);
		resp_503_send(view->ua, txn, req);
		return;
	}

	/*
	 * The line is out of service. Its machine ignores the incoming call
	 * event, so a leg allocated here would ring nothing and the caller
	 * would hear ringback on a silent port until it gave up. The
	 * subscriber is not busy, so the answer is the 480 an account being
	 * torn down gets: the service is unavailable for now, and the
	 * registration stays up. It sits behind the 503 because a DSP that has
	 * not started is a fact about the board and not about this line.
	 */
	if (cv.line_admit && !cv.line_admit(line)) {
		refused_note(view->env, line, 480);
		resp_simple(view->ua, txn, req, 480, NULL);
		return;
	}

	/*
	 * A request this side can never build a dialog for (RFC 3261 8.1.1.3).
	 * sip_dialog_uas_create() refuses a From with no tag, so without this
	 * the leg would ring the subscriber and answer 500 at the lift. The
	 * refusal stands in front of every admission that allocates a leg, so
	 * it leaves no leg, no ring and no record. It sits behind the 480 and
	 * the 503, which answer for the account and for the hardware, and in
	 * front of the Replaces admission, whose 481 and 486 both name a dialog
	 * this request could not identify.
	 */
	if (!from_tag_ok(req)) {
		refused_note(view->env, line, 400);
		resp_simple(view->ua, txn, req, 400, NULL);
		return;
	}

	/*
	 * The Replaces is read here, in front of the capacity refusal below,
	 * which is the governing rule of this file. See replaces_admit(): an
	 * INVITE that replaces a dialog is not a new call, so the slot count
	 * cannot answer it. The admission earns four refusals of its own: a
	 * 400, a 481 and two 486s. A capacity 486 in front of it would hide the
	 * 400 and the 481.
	 *
	 * It sits behind the 480 and the 503, which answer for the account and
	 * for the hardware: neither is a fact about the caller's request, and a
	 * line whose DSP cannot carry a call cannot carry a replaced one either.
	 *
	 * An INVITE without Replaces leaves this whole block untouched and
	 * takes the ordinary admission behind it.
	 */
	code = replaces_admit(line, req, &replaced);
	if (code) {
		refused_note(view->env, line, code);
		resp_simple(view->ua, txn, req, code, NULL);
		return;
	}

	if (replaced ? !replaces_slot_free(line, replaced, &slot) :
		       !call_slot_free(line, &slot)) {
		refused_note(view->env, line, 486);
		resp_simple(view->ua, txn, req, 486, NULL);
		return;
	}

	/* RFC 4028 4: 90 seconds is the floor even with nothing configured. */
	min_se = sess_min_of_cfg(&cv.cfg->channel[line]);
	if (hdr_session_expires_get(req, &se, &who) && se && se < min_se) {
		refused_note(view->env, line, 422);
		resp_422_send(view->ua, txn, req, min_se);
		return;
	}

	c = leg_alloc(line, slot);
	/* the physical slot is free, so a UI name is (call.h): a line carries
	 * as many names as legs, and the child of a transfer may be wearing the
	 * one this slot is called by */
	ui_slot_take(c);
	leg_ident_take(c, view);
	c->uac = false;
	c->server_txn = sip_txn_handle(txn);
	if (src)
		c->src = *src;
	sdp_state_init(&c->sdp, sess_id_new(c));
	cid_take(c, req);

	if (!leg_media_take(c)) {
		leg_trace(c, "no RTP port or no codec for an inbound call");
		refused_note(view->env, line, 486);
		resp_simple(c->ua, txn, req, 486, NULL);
		leg_release(c);
		return;
	}

	if (msg_body_sdp_get(req, &body)) {
		if (!sdp_parse(body, &c->offer)) {
			refused_note(view->env, line, 488);
			resp_simple(c->ua, txn, req, 488, NULL);
			leg_release(c);
			return;
		}
		/*
		 * The ghost ring. An offer whose audio address the media layer
		 * can never program has one outcome only, and the subscriber
		 * does not decide it: sdp.c refuses the same address at the
		 * answer, so the leg would ring and then answer 488 the moment
		 * the handset comes up. The refusal therefore belongs in front
		 * of the ring. It sits behind the 480 and the 503 because an
		 * account that is going away and a line the hardware cannot
		 * serve are answers about this line rather than about the
		 * caller's offer, and behind the slot check because a busy
		 * line reads no offer at all. The answer-time refusal in
		 * call_answer() stays as the backstop for the offers this
		 * check lets through.
		 */
		if (!sdp_offer_usable(&c->offer)) {
			leg_trace(c, "the offer names no usable address, 488 before the ring");
			refused_note(view->env, line, 488);
			resp_simple(c->ua, txn, req, 488, NULL);
			leg_release(c);
			return;
		}

		/*
		 * The family half of the same pre-ring check. One stream runs
		 * in one address family (RFC 6157 section 5), so an offer
		 * outside this line's family names a stream no socket of ours
		 * carries. It is refused in front of the ring, exactly as an
		 * FQDN is, and sdp_answer_build() stays the backstop for a
		 * re-offer.
		 */
		if (!sdp_offer_local_ok(&c->offer, c->local_host)) {
			leg_trace(c, "the offer is not in this line's "
				  "family, 488 before the ring");
			refused_note(view->env, line, 488);
			resp_simple(c->ua, txn, req, 488, NULL);
			leg_release(c);
			return;
		}

		/* the flag marks an offer this leg will answer, so it is set
		 * once the offer survived every refusal in front of the ring */
		c->offer_valid = true;
	}

	/*
	 * A Replaces answer leaves the offer path here. Everything above it is
	 * shared, because a replacement still needs a port, a codec and an offer
	 * this side can program, and the refusals those make are the same ones;
	 * nothing below it applies, because the call it names is already up.
	 */
	if (replaced) {
		replaces_answer(c, replaced);
		return;
	}

	ring_id_take(c, req);
	state_set(c, CALL_ST_RINGING_IN);
	leg_trace(c, "inbound call from %s%s", c->cid_number,
		  c->cid_number_private ? " (withheld)" : "");

	/* an offered leg is always its own UI name, because a transfer's child
	 * is the only leg that ever wears another and it is never offered */
	if (cv.cb.incoming)
		cv.cb.incoming(line, c->ui_slot, c->cid_number, c->cid_name,
			       c->cid_number_private, c->cid_name_private, c->ring_id);
}

/* -------------------------------------------------- the account.c interface */

void call_rx_request(unsigned int line, struct sip_txn *txn, const struct sip_msg *req,
		     const struct sip_txn_dst *src)
{
	struct acct_call_view view;
	struct sip_dialog *d;
	struct call *c;

	if (!cv.inited || !cv.cfg || line >= CFG_CHANNEL_MAX || !txn || !req)
		return;
	if (!account_call_view(line, &view) || !view.dialogs || !view.ua)
		return;

	d = sip_dialog_find(view.dialogs, req);
	if (d) {
		c = leg_of_dialog(line, sip_dialog_handle(d));
		if (!c) {
			/* the dialog outlived its leg: nothing serves it */
			resp_simple(view.ua, txn, req, 481, NULL);
			return;
		}

		rx_in_dialog(c, d, txn, req);
		return;
	}

	if (req->method == SIP_METHOD_INVITE) {
		rx_new_invite(line, txn, req, src, &view);
		return;
	}

	/* an in-dialog method with no dialog (RFC 3261 12.2.2) */
	resp_simple(view.ua, txn, req, 481, NULL);
}

void call_rx_ack(unsigned int line, const struct sip_msg *ack)
{
	struct acct_call_view view;
	struct sip_dialog *d;
	struct call *c;
	uint32_t cseq = 0;

	if (!cv.inited || line >= CFG_CHANNEL_MAX || !ack)
		return;
	if (!account_call_view(line, &view) || !view.dialogs)
		return;

	d = sip_dialog_find(view.dialogs, ack);
	if (!d)
		return;

	sip_dialog_confirm(d);
	c = leg_of_dialog(line, sip_dialog_handle(d));
	if (!c)
		return;

	if (c->state != CALL_ST_ACCEPT_WAIT_ACK && c->state != CALL_ST_REINVITE_WAIT_ACK)
		return;

	/*
	 * The ACK belongs to one exchange. A late first ACK arriving after a
	 * re-INVITE was answered would otherwise commit the wrong description
	 * (RFC 3261 13.2.2.4: the ACK carries the CSeq of its INVITE).
	 */
	if (!hdr_cseq_get(ack, &cseq, NULL) || cseq != c->exch_cseq) {
		leg_trace(c, "ACK for another exchange, cseq %u", cseq);
		return;
	}

	uas_ack_take(c, ack);
}

/*
 * A 2xx the transaction layer and the dialog layer both passed. The only one
 * that matters is a second fork answering an INVITE we already have a dialog
 * for: RFC 3261 13.2.2.4 makes the UAC ACK it and then end it, or the far end
 * keeps a call up that nobody is on.
 */
bool call_rx_stray(unsigned int line, const struct sip_msg *resp)
{
	struct acct_call_view view;
	struct sip_dialog *d;
	struct sip_addr from;
	struct sip_str call_id;
	enum sip_method method;
	struct call *c = NULL;
	char branch[SIP_BRANCH_MAX];
	unsigned int i;
	uint32_t cseq;

	if (!cv.inited || line >= CFG_CHANNEL_MAX || !resp || resp->is_request)
		return false;
	if (resp->status < 200 || resp->status > 299)
		return false;
	if (!hdr_cseq_get(resp, &cseq, &method) || method != SIP_METHOD_INVITE)
		return false;
	if (!hdr_call_id_get(resp, &call_id) || !hdr_addr_get(resp, SIP_HDR_FROM, &from))
		return false;

	for (i = 0; i < CALL_SLOTS; i++) {
		struct call *l = &calls[line][i];

		if (l->in_use && l->uac && sip_str_eq(call_id, l->call_id))
			c = l;
	}

	if (!c || !account_call_view(line, &view))
		return false;

	d = sip_dialog_uac_update(view.dialogs, resp, &c->dst, &c->dlocal);
	if (!d || sip_dialog_handle(d) == c->dialog)
		return d != NULL;

	leg_trace(c, "another fork answered, clearing it");

	if (sip_branch_new(c->mgr, branch, sizeof(branch)))
		sip_dialog_ack_send(d, resp, branch, NULL, NULL, 0);

	if (sip_branch_new(c->mgr, branch, sizeof(branch))) {
		struct sip_dialog_req r;
		struct sip_buf b;

		memset(&r, 0, sizeof(r));
		r.method = SIP_METHOD_BYE;
		r.branch = branch;

		sip_buf_init(&b, scratch, sizeof(scratch));
		if (sip_dialog_req_build(d, &r, &b))
			sip_txn_client_start(c->mgr, SIP_METHOD_BYE, sip_str_cstr(branch),
					     sip_buf_str(&b), sip_buf_len(&b),
					     sip_dialog_dst(d), leg_owner(c));
	}

	/*
	 * Every distinct fork is cleared the same way, and the work is bounded
	 * per stray rather than by a single record: keeping one would absorb
	 * the 2xx of a third fork without ever ACKing or ending it, and the far
	 * end would hold that call up. The dialog goes back to the pool at
	 * once; a retransmission of the same fork's 2xx is answered by clearing
	 * it again, which is idempotent for the peer.
	 */
	sip_dialog_terminate(d);
	sip_dialog_put(d);

	return true;
}

void call_txn_resp(uint32_t owner_val, uint32_t txn, const struct sip_msg *msg)
{
	struct call *c = leg_get(owner_val);
	uint32_t num = 0, rseq = 0, cseq = 0;
	enum sip_method method;
	struct sip_msg req;
	struct sip_txn *t;

	if (!c || !msg || msg->is_request)
		return;
	if (msg->status < 100)
		return;

	t = leg_txn(c, txn);
	if (!t)
		return;

	if (txn == c->invite_txn) {
		if (msg->status < 200)
			uac_prov(c, msg);
		else if (msg->status < 300)
			uac_ok(c, msg);
		else
			uac_invite_final(c, t, msg);
		return;
	}

	if (txn == c->reinvite_txn) {
		if (msg->status < 200)
			return;
		if (msg->status < 300)
			uac_reinvite_ok(c, msg);
		else
			uac_reinvite_final(c, t, msg);
		return;
	}

	/*
	 * A proxy that guards the dialog challenges the BYE too, and an
	 * unanswered challenge leaves the call up on the far end. One retry per
	 * kind, on the next CSeq the dialog layer hands out.
	 */
	if (txn == c->bye_txn && (msg->status == 401 || msg->status == 407)) {
		c->bye_txn = SIP_HANDLE_NONE;
		if (dlg_auth_take(c, msg) &&
		    dialog_req_send(c, SIP_METHOD_BYE, NULL, NULL, 0, false, &c->bye_txn, NULL)) {
			leg_trace(c, "BYE resent with credentials");
			return;
		}

		leg_end(c);
		return;
	}

	/*
	 * The PRACK of a reliable 1xx (RFC 3262 4). A 481, another failure or
	 * a timeout is traced, and the INVITE runs on. The INVITE already has
	 * two bounds before the peer's final response: our timer C, and the
	 * peer's own 64*T1 give-up. A CANCEL here would race a 200 the peer may
	 * already be sending. After that final response the call stands, so a
	 * BYE for a late PRACK failure would drop a working call. RFC 3262 4
	 * makes a PRACK after the final response optional anyway.
	 *
	 * The answer is recognised by the CSeq method and not by the handle.
	 * RFC 3262 3 forbids a second reliable provisional only before the
	 * first one is acknowledged, and after that it recommends the wait, so
	 * a conforming peer can leave two PRACKs of this leg in flight. The leg
	 * keeps one handle for the trace, and the retry names its RAck out of
	 * the request that the answered transaction retains.
	 *
	 * The credential retry runs only while the INVITE is in flight, the leg
	 * is not clearing and the RAck still names that INVITE. dlg_auth_take()
	 * and bye_send() share one counter, so a PRACK retry behind the final
	 * response would spend the retry the BYE needs for its own challenge,
	 * and the peer would keep the dialog.
	 */
	if (msg->status >= 200 && hdr_cseq_get(msg, &num, &method) &&
	    method == SIP_METHOD_PRACK) {
		if (txn == c->prack_txn)
			c->prack_txn = SIP_HANDLE_NONE;
		if ((msg->status == 401 || msg->status == 407) &&
		    c->invite_txn != SIP_HANDLE_NONE && !leg_clearing(c) &&
		    sip_txn_request_get(t, &req) &&
		    hdr_rack_get(&req, &rseq, &cseq, NULL) &&
		    cseq == c->cseq && dlg_auth_take(c, msg) &&
		    prack_send(c, rseq, cseq))
			return;
		if (msg->status >= 300)
			leg_trace(c, "PRACK answered %u, the INVITE continues",
				  msg->status);
		return;
	}

	/* the REFER of a transfer: the accept, the one auth retry and the
	 * failure all live behind this handle (call.h call_xfer) */
	if (txn == c->refer_txn && msg->status >= 200) {
		refer_final(c, msg);
		return;
	}

	/*
	 * The NOTIFY of a referral we serve. Nothing here acts on the answer: a
	 * transferor that BYEd the leg the moment it accepted our 202 answers
	 * 481 to every NOTIFY behind it, which is the "REFER then BYE" shape
	 * and not a defect (call.h call_xfer, the blind arm).
	 */
	if (txn == c->notify_txn && msg->status >= 200) {
		c->notify_txn = SIP_HANDLE_NONE;

		/*
		 * A proxy that guards the dialog challenges this request too,
		 * and an unanswered challenge loses the transfer outcome the
		 * transferor is waiting for. One retry per kind, the rule the
		 * BYE and the REFER of this module already follow, and the
		 * credentials ride the rebuild rather than a replay.
		 */
		if ((msg->status == 401 || msg->status == 407) &&
		    dlg_auth_take(c, msg) &&
		    xfer_notify_send(c, c->notify_code, c->notify_final)) {
			leg_trace(c, "transfer NOTIFY resent with credentials");
			return;
		}

		if (msg->status >= 300)
			leg_trace(c, "transfer NOTIFY refused %u", msg->status);
		return;
	}

	/*
	 * The relayed hook flash is fire and forget: the far end owns the
	 * feature, so a refusal changes nothing here and is only worth saying
	 * out loud once.
	 */
	if (txn == c->flash_txn && msg->status >= 200) {
		c->flash_txn = SIP_HANDLE_NONE;
		if (msg->status >= 300)
			leg_trace(c, "flash INFO refused %u", msg->status);
		return;
	}

	/* Every other answer inside the dialog carries nothing to act on. */
}

void call_txn_done(uint32_t owner_val, uint32_t txn, enum sip_txn_reason reason)
{
	struct call *c = leg_get(owner_val);
	bool dead;

	if (!c)
		return;

	dead = reason == SIP_TXN_DONE_TIMEOUT || reason == SIP_TXN_DONE_TRANSPORT;

	if (txn == c->invite_txn) {
		/* every final response clears the handle first, so a match here
		 * is a transaction that never got one */
		c->invite_txn = SIP_HANDLE_NONE;
		sip_timer_cancel(c->env, &c->t_c);
		if (c->state == CALL_ST_CANCEL_SENT || c->cancel_wanted)
			leg_end(c);
		else
			leg_fail(c, CALL_FAIL_CONGESTION);
		return;
	}

	/*
	 * The INVITE server transaction of an inbound leg. Under a leg that
	 * has not answered it dies of a transport error alone (RFC 3261
	 * 17.2.4): the peer retransmits its INVITE, or the reliable schedule
	 * asks for another copy, and the transport refuses the 180 that
	 * sip/txn.c retains. That leg has no transaction left to answer on, so
	 * it takes the class a refused first 180 gives it in call_ring()
	 * instead of ringing on against a dead transaction.
	 *
	 * Every other end of this transaction is a normal one and leaves the
	 * leg alone. Our 2xx ends it inside leg_answer() and hands the
	 * retransmission to the UA core, which is why nothing here may read
	 * the state: the leg is still being answered. The ACK or timer H of a
	 * non-2xx we sent ends it behind a leg that already ended, and a leg
	 * that ends detaches its transactions, so that end reaches no upcall
	 * at all. The dead handle goes in every case, and no reader misses it:
	 * leg_txn() answers NULL for a dead handle as for none, and a CANCEL
	 * reaches call_rx_cancel() through a live transaction alone.
	 */
	if (txn == c->server_txn) {
		c->server_txn = SIP_HANDLE_NONE;
		if (reason != SIP_TXN_DONE_TRANSPORT)
			return;

		leg_trace(c, "the INVITE server transaction failed, "
			  "transport error");
		leg_fail(c, CALL_FAIL_CONGESTION);
		return;
	}

	if (txn == c->reinvite_txn) {
		c->reinvite_txn = SIP_HANDLE_NONE;
		if (dead)
			reinvite_fail(c, true);
		return;
	}

	if (txn == c->bye_txn) {
		c->bye_txn = SIP_HANDLE_NONE;
		leg_end(c);
		return;
	}

	if (txn == c->cancel_txn) {
		c->cancel_txn = SIP_HANDLE_NONE;
		return;
	}

	/*
	 * A PRACK that never got its answer leaves the INVITE alone, for the
	 * reasons call_txn_resp() states beside its own arm. The leg holds one
	 * PRACK handle. RFC 3262 3 forbids a second reliable provisional only
	 * before the first one is acknowledged, and recommends the wait after
	 * that, so two PRACKs overlap against a conforming peer as well. The
	 * older handle then falls through the tail below, and its answer is
	 * recognised by the CSeq method in call_txn_resp().
	 */
	if (txn == c->prack_txn) {
		c->prack_txn = SIP_HANDLE_NONE;
		if (dead)
			leg_trace(c, "PRACK unanswered, the INVITE continues");
		return;
	}

	if (txn == c->flash_txn) {
		c->flash_txn = SIP_HANDLE_NONE;
		return;
	}

	if (txn == c->notify_txn) {
		c->notify_txn = SIP_HANDLE_NONE;
		return;
	}

	/*
	 * A REFER that died without a final answer is a transfer with no
	 * verdict, and the transaction timers are what bound it up to the 202:
	 * the outcome is reported here rather than left to the guard, which an
	 * unaccepted REFER never armed.
	 */
	if (txn == c->refer_txn) {
		c->refer_txn = SIP_HANDLE_NONE;
		if (dead)
			xfer_outcome(c, XFER_SRC_REFER, false, 408);
	}
}

void call_ua_ack_missing(uint32_t owner_val)
{
	struct call *c = leg_get(owner_val);
	struct sip_dialog *d;

	if (!c)
		return;

	/* RFC 3261 13.3.1.4: confirm the dialog and end the session. */
	d = leg_dialog(c);
	if (d)
		sip_dialog_confirm(d);

	leg_trace(c, "no ACK for our 2xx");
	/* a draining leg is bounded by its guards, and a BYE here would end
	 * the dialog the drain keeps for the network's own */
	if (c->xfer_drain) {
		if (c->state == CALL_ST_REINVITE_WAIT_ACK)
			state_set(c, CALL_ST_ACTIVE);
		return;
	}
	leg_abandon(c, CALL_FAIL_QUIET);
}

/*
 * The held operation ends without a request. A refresh that cannot run means
 * the session is gone, which is the verdict reinvite_fail() gives it (RFC 4028
 * 10); every other operation owes the machine the outcome it parked in PENDING
 * for, and reinvite_done() reports the operation it is given.
 */
static void glare_wait_fail(struct call *c, enum call_op op)
{
	if (op == CALL_OP_REFRESH) {
		leg_abandon(c, CALL_FAIL_QUIET);
		return;
	}

	c->op = (uint8_t)op;
	reinvite_done(c, false);
}

/*
 * The retry of a re-INVITE a 491 held back, and the one entry such a retry
 * has: no re-INVITE 491 reaches the setup path below.
 *
 * The exchange of the peer that crossed ours can still be open here, because
 * the leg answered it during the wait and its ACK decides when the leg is free
 * again. The wait is then armed again, and nothing counts those re-arms: the
 * bound on a peer that never finishes is call_ua_ack_missing() at 64*T1, which
 * every unacknowledged 2xx of ours meets.
 *
 * A hold re-offered by a transferee's child rides CALL_OP_XFER_HOLD, so the
 * retry has to ask for the hold under that operation too: uac_glare_take() put
 * the leg back to the state of before the offer, and an offer without the hold
 * would resume a call the machine still believes is held.
 */
static void glare_wait_fire(struct call *c)
{
	enum call_op op = (enum call_op)c->glare_op;

	/* the leg is not free yet, so the wait is armed again. The refusal
	 * below cannot run today: glare_expire() frees the slot before this
	 * upcall and ua_glare_arm() cancels this owner first, so the table
	 * always has the entry back. The bool still has to be handled. */
	if (c->state != CALL_ST_ACTIVE || leg_clearing(c)) {
		if (ua_glare_arm(c->ua, leg_owner(c), c->uac, NULL)) {
			leg_trace(c, "the peer exchange is open, the retry "
				  "waits again");
			return;
		}
		leg_trace(c, "no glare timer left, the held retry gives up");
		glare_wait_drop(c);
		glare_wait_fail(c, op);
		return;
	}

	glare_wait_drop(c);

	if (op == CALL_OP_REFRESH) {
		session_refresh_send(c);
		return;
	}

	if (!reinvite_start(c, op,
			    op == CALL_OP_HOLD || op == CALL_OP_XFER_HOLD))
		glare_wait_fail(c, op);
}

void call_ua_glare_retry(uint32_t owner_val)
{
	struct call *c = leg_get(owner_val);

	if (!c)
		return;

	if (c->glare_wait) {
		glare_wait_fire(c);
		return;
	}

	/*
	 * The leg moved on while the timer ran: a hangup during a re-INVITE
	 * leaves the leg alive with a BYE outstanding, and a retry from there
	 * would put a second INVITE into a dialog that is ending. Only the
	 * setup states still want one.
	 */
	if (c->state != CALL_ST_INVITE_SENT && c->state != CALL_ST_PROCEEDING) {
		leg_trace(c, "glare retry dropped, the leg moved on");
		return;
	}

	if (!invite_resend(c))
		leg_fail(c, CALL_FAIL_CONGESTION);
}

uint32_t call_slot_handle(unsigned int line, unsigned int slot)
{
	struct call *c;
	uint16_t gen;

	if (line >= CFG_CHANNEL_MAX || slot >= CALL_SLOTS)
		return SIP_HANDLE_NONE;

	c = &calls[line][slot];
	gen = c->gen ? c->gen : 1;

	/* leg_alloc() releases a draining predecessor, and that bumps once */
	if (c->in_use)
		gen = gen_bump(gen);

	return sip_handle_make(line * CALL_SLOTS + slot, gen);
}

const char *call_txn_local_tag(unsigned int line, uint32_t txn)
{
	struct call *c = leg_of_server_txn(line, txn);

	if (!c || !c->local_tag[0])
		return NULL;

	return c->local_tag;
}

void call_rx_cancel(unsigned int line, uint32_t invite_txn)
{
	struct call *c = leg_of_server_txn(line, invite_txn);

	if (!c)
		return;

	leg_trace(c, "peer CANCEL");
	c->reported = true;
	if (ui_emits(c) && cv.cb.remote_cancel)
		cv.cb.remote_cancel(c->line, c->ui_slot);

	leg_end(c);
}

/* ---------------------------------------------------------- FSM commands */

bool call_slot_free(unsigned int line, unsigned int *slot)
{
	unsigned int i;

	if (!cv.inited || !cv.cfg || line >= CFG_CHANNEL_MAX)
		return false;

	/* a fully free slot first, so a draining leg is only reused when the
	 * line has nothing else left */
	for (i = 0; i < CALL_SLOTS; i++) {
		if (calls[line][i].state == CALL_ST_FREE) {
			if (slot)
				*slot = i;
			return true;
		}
	}

	for (i = 0; i < CALL_SLOTS; i++) {
		if (!calls[line][i].ui_busy) {
			if (slot)
				*slot = i;
			return true;
		}
	}

	return false;
}

/*
 * The physical slot a dial on a UI name lands on (call.h, the two-name
 * contract). The slot of that name is the answer on every untransferred line,
 * which is what keeps a dial putting its stream where it always did; only when
 * that slot is occupied, by a child that took the other name over, does the
 * dial go looking for whatever is free.
 *
 * A transfer drain on that slot is the one occupant the dial walks past: it
 * still owns a dialog the network expects to end, where a BYE_SENT
 * predecessor owns nothing a reclaim could lose. A drain whose own BYE is
 * already out is that predecessor, whatever flag it still carries, and is
 * reclaimed in place as before: walking past it would hold its RTP port for
 * the BYE transaction and could refuse the dial on an exhausted range.
 * call_slot_free() prefers a free slot and returns a live drain only when
 * nothing else is left, which is where leg_alloc() evicts it.
 */
static bool leg_slot_pick(unsigned int line, unsigned int want, unsigned int *out)
{
	const struct call *c = &calls[line][want];

	if (!c->ui_busy && (!c->xfer_drain || c->state == CALL_ST_BYE_SENT)) {
		*out = want;
		return true;
	}

	return call_slot_free(line, out);
}

/* the UI question the funnel asks, which is occupancy and not existence: a
 * draining leg the machine already gave up does not hold its name */
static bool ui_slot_busy(unsigned int line, unsigned int ui_slot)
{
	const struct call *c = leg_of_ui(line, ui_slot);

	return c && c->ui_busy;
}

bool calls_line_dial_ready(unsigned int line)
{
	struct acct_call_view view;

	if (!cv.inited || !cv.cfg || line >= CFG_CHANNEL_MAX)
		return false;

	return dial_ready_view_get(line, &view);
}

/*
 * The one refusal a dial reports (call.h call_dial). The machine has entered
 * CALLING by the time the op runs, so a silent false would leave it waiting
 * for an outcome nothing was left to send.
 */
static bool dial_refuse(unsigned int line, unsigned int slot)
{
	if (cv.cb.failed)
		cv.cb.failed(line, slot, CALL_FAIL_CONGESTION);

	return false;
}

unsigned int call_slot_stream(unsigned int line, unsigned int ui_slot)
{
	const struct call *c = leg_of_ui(line, ui_slot);

	return c ? c->slot : CALL_SLOTS;
}

/*
 * The passthrough kind the legs of one line describe, which is what a backend
 * resolves its canceller policy from. It is this side's mirror of
 * udsp/src/core/stream.c stream_line_passthrough(), and the two gates below are
 * that function's stream_governs() rules read off a leg instead of a stream:
 *
 *   media_started    the stream is running, its st->active
 *   tx_silence       a leg on local hold puts nothing of its own on the wire
 *                    (hal.h, the tx_silence block), so the library stops
 *                    counting it and the other call of the line gets the
 *                    canceller back for as long as the hold lasts
 *
 * The kind read is the description and not c->fax_kind: media.passthrough is
 * the field a backend actually reads, and it is written on every programming of
 * the stream. media_started implies media_set, because the start runs behind
 * the config in media_program().
 *
 * The modem kind wins for stream_line_passthrough()'s own reason: it takes the
 * canceller off the frame whole, the safe answer for a carrier of either kind,
 * and the fax kind's policy only relaxes it.
 *
 * One divergence remains, and this side cannot close it. The library also drops
 * a dead-marked stream, the per-stream failure isolation of its bounded stop,
 * and that mark reaches no HAL surface: a stream it gave up on still reads as
 * governing here.
 *
 * media_running answers whether any leg of the line has running media, held
 * legs included: the library's latch cannot outlive the line's streams
 * (stream_lec_release), so a policy view consulted with no media running would
 * describe a canceller that no longer exists.
 */
uint8_t calls_line_fax_kind(unsigned int line, bool *media_running)
{
	uint8_t kind = HAL_PASSTHROUGH_OFF;
	bool running = false;
	unsigned int i;

	if (line >= CFG_CHANNEL_MAX)
		goto out;

	for (i = 0; i < CALL_SLOTS; i++) {
		const struct call *c = &calls[line][i];

		if (!c->in_use || !c->media_started)
			continue;
		running = true;
		if (c->media.tx_silence)
			continue;
		/* The library reads a T.38 leg as the modem kind whatever its
		 * description says (udsp media_passthrough_kind): the image
		 * session is full duplex data and the canceller is off the
		 * frame whole for it. */
		if (c->media.codec == HAL_CODEC_T38 ||
		    c->media.passthrough == HAL_PASSTHROUGH_MODEM) {
			kind = HAL_PASSTHROUGH_MODEM;
			break;
		}
		if (c->media.passthrough == HAL_PASSTHROUGH_FAX)
			kind = HAL_PASSTHROUGH_FAX;
	}

out:
	if (media_running)
		*media_running = running;

	return kind;
}

/* the R-URI and the To of a dialled call, from the raw collected number */
static bool leg_ruri_build(struct call *c)
{
	char user[CALL_USER_ESC_MAX];
	int n;

	if (!uri_user_escape(user, sizeof(user), c->number, leg_user_esc(c)))
		return false;

	n = snprintf(c->to_uri, sizeof(c->to_uri), "sip:%s@%s", user, c->domain);
	if (n < 0 || (size_t)n >= sizeof(c->to_uri))
		return false;

	str_copy(c->ruri, sizeof(c->ruri), c->to_uri);

	return true;
}

bool call_dial(unsigned int line, unsigned int slot, const char *number,
	       bool cid_present)
{
	struct acct_call_view view;
	unsigned int phys = 0;
	struct call *c;

	if (!cv.inited || !cv.cfg || line >= CFG_CHANNEL_MAX || slot >= CALL_SLOTS)
		return false;
	if (!number || !*number)
		return false;

	/*
	 * Every refusal that names a slot comes through one funnel (call.h
	 * call_dial): an account that cannot carry a dial, a UI name another
	 * leg holds and a line with no physical slot left. Each one reports
	 * congestion, because the machine is in CALLING by the time this op
	 * runs and would otherwise wait for an outcome that nothing sends.
	 */
	if (ui_slot_busy(line, slot) || !leg_slot_pick(line, slot, &phys) ||
	    !dial_ready_view_get(line, &view))
		return dial_refuse(line, slot);

	c = leg_alloc(line, phys);

	/*
	 * The name is asked for again behind the allocation. leg_alloc()
	 * releases a draining predecessor, and that release hands a shadowing
	 * child the name it was waiting for: the leg just allocated would then
	 * be the second owner of one name, and the resolver would answer with
	 * whichever of the two it walked into first.
	 */
	if (ui_slot_taken(c, slot)) {
		leg_trace(c, "the UI slot was taken while this leg was allocated");
		c->ui_role = CALL_UI_NONE;
		leg_end(c);
		return dial_refuse(line, slot);
	}

	c->ui_slot = (uint8_t)slot;
	c->uac = true;
	leg_ident_take(c, &view);
	str_copy(c->number, sizeof(c->number), number);
	c->cid_present = cid_present;

	if (!leg_media_take(c)) {
		leg_trace(c, "no RTP port or no codec");
		leg_fail(c, CALL_FAIL_CONGESTION);
		return false;
	}

	/*
	 * A withheld call sends the RFC 3323 anonymous identity in the From
	 * and asks the network for id privacy; the Contact stays the real
	 * address, because media has to reach this line either way, and the
	 * Call-ID is already random. The dialog copies its local URI from what
	 * this INVITE carried, so every in-dialog request keeps the same
	 * identity without a second decision.
	 */
	snprintf(c->from_uri, sizeof(c->from_uri), "sip:%s@%s", c->user, c->domain);
	if (!cid_present) {
		str_copy(c->from_uri, sizeof(c->from_uri), CALL_ANON_URI);
		str_copy(c->display, sizeof(c->display), CALL_ANON_DISPLAY);
		leg_trace(c, "outgoing caller id withheld");
	}

	if (!leg_ruri_build(c)) {
		leg_trace(c, "the number does not fit a request URI");
		leg_fail(c, CALL_FAIL_CONGESTION);
		return false;
	}

	snprintf(c->call_id, sizeof(c->call_id), "%08x%08x@%s", sip_txn_rand(c->mgr),
		 sip_txn_rand(c->mgr), c->local_host);

	if (!ua_tag_new(c->ua, c->local_tag, sizeof(c->local_tag))) {
		leg_fail(c, CALL_FAIL_CONGESTION);
		return false;
	}

	sdp_state_init(&c->sdp, sess_id_new(c));
	c->cseq = 1;

	if (!invite_send(c)) {
		leg_trace(c, "INVITE could not be sent");
		leg_fail(c, CALL_FAIL_CONGESTION);
		return false;
	}

	return true;
}

/*
 * The option gates the reliable 180 whole. RFC 3262 3 makes it a MUST when
 * the INVITE carries Require: 100rel; with the option clear uvoice sends the
 * plain 180 anyway, because the option means "this side sends
 * reliable provisionals" and a switch that requires them tolerates the plain
 * one. The transaction layer still admits the tag, so the INVITE proceeds
 * instead of drawing a 420.
 *
 * An INVITE without an offer takes the same bodiless 180 and keeps its offer
 * in the 2xx. RFC 3262 5 asks for the offer in the first reliable provisional
 * of such an exchange; meeting it needs the answer out of the PRACK and a 2xx
 * held until that PRACK arrives, neither of which this leg builds.
 */
static bool rel_wanted(const struct call *c, const struct sip_msg *req)
{
	if (!c->ch.prack)
		return false;

	return hdr_token_has(req, SIP_HDR_REQUIRE, "100rel") ||
	       hdr_token_has(req, SIP_HDR_SUPPORTED, "100rel");
}

/*
 * The schedule of RFC 3262 3: the interval starts at T1 and doubles for each
 * copy, with no T2 cap, and the whole schedule ends at 64*T1. The bound comes
 * from T1 and not from timer H or timer B, because those carry their own
 * configured values and sip_timer_cfg_fixup() does not derive them from a
 * lowered T1.
 */
static void rel_start(struct call *c, uint32_t rseq, uint32_t cseq)
{
	uint32_t t1 = sip_txn_mgr_timers_get(c->mgr)->t1;

	c->rel_rseq = rseq;
	c->rel_cseq = cseq;
	c->rel_open = true;
	c->rel_last = false;
	c->rel_retx_ms = t1;
	c->rel_deadline_ms = c->env->base.now_ms(&c->env->base) + 64 * t1;
	sip_timer_arm(c->env, &c->t_rel, c->rel_retx_ms);
}

static void timer_rel_fire(struct sip_timer *t)
{
	struct call *c = t->owner;
	struct sip_txn *txn = leg_txn(c, c->server_txn);
	struct sip_msg req;
	uint64_t now, left;
	uint32_t arm;

	/* the leg is ending and its server transaction went with it */
	if (!txn)
		return;

	if (c->rel_last) {
		/*
		 * RFC 3262 3 asks for a 5xx when no PRACK arrives. 504 names a
		 * server that did not answer in time (RFC 3261 21.5.5); 500
		 * already marks a stale in-dialog CSeq on this leg.
		 */
		leg_trace(c, "no PRACK within 64*T1, INVITE rejected 504");
		c->rel_open = false;
		if (sip_txn_request_get(txn, &req))
			resp_simple(c->ua, txn, &req, 504, c->local_tag);
		leg_fail(c, CALL_FAIL_CONGESTION);
		return;
	}

	if (!sip_txn_response_resend(txn)) {
		/*
		 * Neither cause leaves work here. A final response ended the
		 * retransmission, and the record stays for a PRACK that is
		 * still to come. A transport that refused the send terminated
		 * the transaction inside this call (sip/txn.h), and the
		 * server_txn arm of call_txn_done() failed the leg from there,
		 * which released the slot: nothing may read the leg after that.
		 */
		return;
	}

	leg_trace(c, "180 retransmitted, RSeq %u", c->rel_rseq);
	c->rel_retx_ms *= 2;
	now = c->env->base.now_ms(&c->env->base);
	left = now < c->rel_deadline_ms ? c->rel_deadline_ms - now : 0;
	c->rel_last = c->rel_retx_ms >= left;
	arm = c->rel_last ? (uint32_t)left : c->rel_retx_ms;
	sip_timer_arm(c->env, &c->t_rel, arm);
}

/*
 * The 180 this side could not build or send. Every cause is a fault inside
 * the daemon, so the caller is answered 500 (RFC 3261 21.5.1), which is what
 * leg_answer() answers for the same dialog failure. A 480 or a 486 would make
 * the SBC apply subscriber treatment, and a 503 can take the contact out of
 * service.
 *
 * The leg fails here, where leg_answer() keeps it at the same "no server
 * transaction" point. The two points differ in who is waiting: the answer
 * runs on a command of the machine, which still owns the name and can hang up
 * or reject, and the ring runs before the subscriber knows of the call at
 * all. The deferred FSM_EV_CALL_FAILED of leg_fail() is the one way to stop a
 * bell that is about to ring for a call no caller is on.
 */
static bool ring_refuse(struct call *c, struct sip_txn *txn,
			const struct sip_msg *req, const char *why)
{
	leg_trace(c, "180 not sent: %s", why);
	/* the counters record what a caller was told, so a refusal whose server
	 * transaction is already gone answers nobody and counts nothing */
	if (txn && req) {
		refused_note(c->env, c->line, 500);
		resp_simple(c->ua, txn, req, 500, c->local_tag);
	}
	leg_fail(c, CALL_FAIL_CONGESTION);

	return false;
}

bool call_ring(unsigned int line, unsigned int slot)
{
	struct call *c = leg_of_ui(line, slot);
	uint32_t cseq = 0, rseq = 0;
	struct sip_txn *txn;
	struct sip_msg req;
	struct sip_buf b;
	bool rel;

	if (!c || c->state != CALL_ST_RINGING_IN)
		return false;
	if (c->ring_sent)
		return true;

	txn = leg_txn(c, c->server_txn);
	if (!txn || !sip_txn_request_get(txn, &req))
		return ring_refuse(c, NULL, NULL, "no server transaction");

	/* the 180 creates the early dialog, so the route set is captured here */
	if (!dialog_uas_ensure(c, &req, txn))
		return ring_refuse(c, txn, &req, "no UAS dialog");

	/* the RAck of the PRACK names the CSeq of this INVITE (RFC 3262 7.2) */
	rel = rel_wanted(c, &req) && hdr_cseq_get(&req, &cseq, NULL);
	if (rel)
		rseq = 1 + sip_txn_rand(c->mgr) % 0x7fffffffu;

	sip_buf_init(&b, c->wire, sizeof(c->wire));
	if (!msg_response_build(&b, &req, 180, NULL, c->local_tag, SIP_RESP_F_RECORD_ROUTE))
		return ring_refuse(c, txn, &req, "the response does not build");

	hdr_addr_write(&b, SIP_HDR_CONTACT, NULL, c->contact, NULL);
	hdr_write(&b, SIP_HDR_ALLOW, "%s", UA_ALLOW_DEFAULT);
	/* sip/ua.h: allow_events rides wherever allow rides, so one answer
	 * never names a method without the package that makes it useful */
	hdr_write(&b, SIP_HDR_ALLOW_EVENTS, "%s", UA_ALLOW_EVENTS_DEFAULT);
	hdr_write(&b, SIP_HDR_SUPPORTED, "%s", UA_SUPPORTED_DEFAULT);
	if (rel) {
		/* RFC 3262 3: the option tag and the sequence number are what
		 * make the 180 reliable, and the first RSeq of a transaction is
		 * drawn in 1..2**31-1 */
		hdr_write(&b, SIP_HDR_REQUIRE, "100rel");
		hdr_write(&b, SIP_HDR_RSEQ, "%u", rseq);
	}
	if (c->user_agent[0])
		hdr_write(&b, SIP_HDR_USER_AGENT, "%s", c->user_agent);
	msg_body_write(&b, NULL, NULL, 0);
	if (!sip_buf_ok(&b))
		return ring_refuse(c, txn, &req, "the response does not fit the wire buffer");

	/* our own provisional makes the automatic 100 unnecessary */
	ua_trying_cancel(c->ua, c->server_txn);
	if (!sip_txn_response_send(txn, 180, sip_buf_str(&b), sip_buf_len(&b))) {
		c->server_txn = SIP_HANDLE_NONE;
		leg_fail(c, CALL_FAIL_CONGESTION);
		return false;
	}

	c->ring_sent = true;
	if (!rel) {
		leg_trace(c, "180 sent");
		return true;
	}

	rel_start(c, rseq, cseq);
	leg_trace(c, "180 sent reliably, RSeq %u", rseq);

	return true;
}

/*
 * The 2xx of an INVITE this line accepts, and everything that rides it. It has
 * two callers, one per kind of acceptance: the FSM command below, which is a
 * subscriber lifting the handset, and the auto-answer of an INVITE that carries
 * a Replaces, which is a transfer completing on a call the machine already
 * believes is up.
 *
 * media says whether the stream starts with the 2xx. It does for an ordinary
 * answer: the stream is programmed with the remote address, so it can send the
 * moment the answer exists, and waiting for the ACK clips the first word of the
 * call. It does not for a Replaces answer, whose line is still carrying the
 * leg this one replaces; that stream goes down at the takeover, and the
 * takeover is at the ACK. The negotiation is only committed at the ACK either
 * way.
 */
static bool leg_answer(struct call *c, bool media)
{
	struct sip_txn *txn;
	struct sip_msg req;
	bool owned;

	if (c->state != CALL_ST_RINGING_IN)
		return false;

	txn = leg_txn(c, c->server_txn);
	if (!txn || !sip_txn_request_get(txn, &req)) {
		/*
		 * A leg that holds no name has no second chance. An offered
		 * call keeps its slot here on purpose: the FSM still owns the
		 * name and can hang up or reject. A replacement owns no name
		 * at all, so nothing could ever reach it again and its slot
		 * would be held until the daemon restarts.
		 *
		 * It is defence and not a live path: the caller has just started
		 * the server transaction this reads, so it is still there. The
		 * arm costs one predicate and closes the class.
		 */
		if (!ui_holds(c))
			leg_release(c);
		return false;
	}
	if (!dialog_uas_ensure(c, &req, txn)) {
		resp_simple(c->ua, txn, &req, 500, NULL);
		leg_fail(c, CALL_FAIL_CONGESTION);
		return false;
	}

	if (c->offer_valid) {
		/* the leg's own view: an initial INVITE is never a T.38 upgrade
		 * (sdp.c gates the image branch on a committed exchange), so
		 * the admission has nothing to decide here */
		if (body_answer_build(c, &c->loc, &c->offer) != SDP_OK) {
			leg_trace(c, "the offer carries no usable codec or address");
			resp_simple(c->ua, txn, &req, 488, c->local_tag);
			leg_fail(c, CALL_FAIL_CONGESTION);
			return false;
		}
	} else {
		/* offerless INVITE: we offer in the 2xx, the answer is in the ACK */
		c->sdp.offer_in_200 = true;
		if (!body_offer_build(c)) {
			resp_simple(c->ua, txn, &req, 500, c->local_tag);
			leg_fail(c, CALL_FAIL_CONGESTION);
			return false;
		}
	}

	sess_uas_take(c, &req);
	ua_trying_cancel(c->ua, c->server_txn);

	if (!resp_200_send(c, txn, &req, true, &owned)) {
		c->server_txn = SIP_HANDLE_NONE;
		leg_fail(c, CALL_FAIL_CONGESTION);
		return false;
	}

	/*
	 * RFC 3262 3: a 2xx may go out before the PRACK, because our reliable
	 * 180 carries no session description. The retransmission stops here and
	 * rel_open stands, so a PRACK that arrives behind the 2xx still earns
	 * its 200.
	 */
	sip_timer_cancel(c->env, &c->t_rel);

	if (media && !c->sdp.offer_in_200 && !media_program(c, &c->sdp.proposed, true)) {
		leg_abandon(c, CALL_FAIL_CONGESTION);
		return false;
	}

	sess_arm(c);
	state_set(c, CALL_ST_ACCEPT_WAIT_ACK);

	if (!owned)
		leg_abandon(c, CALL_FAIL_QUIET);

	return true;
}

bool call_answer(unsigned int line, unsigned int slot)
{
	struct call *c = leg_of_ui(line, slot);

	return c && leg_answer(c, true);
}

bool call_reject(unsigned int line, unsigned int slot, uint32_t code)
{
	struct call *c = leg_of_ui(line, slot);
	struct sip_txn *txn;
	struct sip_msg req;

	if (!c || c->state != CALL_ST_RINGING_IN)
		return false;

	if (!code)
		code = 486;

	txn = leg_txn(c, c->server_txn);
	/* the counters record what a caller was told (ring_refuse) */
	if (txn && sip_txn_request_get(txn, &req)) {
		refused_note(c->env, line, code);
		resp_simple(c->ua, txn, &req, code, c->local_tag);
	}

	/*
	 * The FSM asked for this, so no clearing class is owed and cb_failed()
	 * returns early on reported. The record still needs the cause, and this
	 * is the one path that writes it without that report. The first cause
	 * wins, the rule clear_stamp() follows: a line teardown reports the
	 * quiet class and then rejects the offered leg with 480, and the record
	 * must name the class the machine was told.
	 */
	c->reported = true;
	if (!c->failed) {
		c->failed = true;
		c->fail_class = (uint8_t)(refused_busy_is(code) ?
					  CALL_FAIL_BUSY :
					  CALL_FAIL_CONGESTION);
	}
	leg_trace(c, "rejected with %u", code);
	leg_end(c);

	return true;
}

bool call_forward(unsigned int line, unsigned int slot, const char *target)
{
	struct call *c = leg_of_ui(line, slot);
	char contact[CALL_URI_MAX];
	struct sip_txn *txn;
	struct sip_msg req;

	if (!c || c->state != CALL_ST_RINGING_IN)
		return false;
	if (!target || !*target)
		return false;

	/*
	 * The target is resolved before the leg is committed to the redirect.
	 * A refusal has to leave the offer unforwarded and the leg alive, so
	 * the caller keeps being served: ending it here would cost the call for
	 * a Contact this layer could not build. No configured target reaches
	 * this today, because the schema bounds them well under a request URI,
	 * and the order is what keeps that true if one ever grows.
	 */
	if (!target_uri_build(c, target, contact, sizeof(contact)))
		return false;

	txn = leg_txn(c, c->server_txn);
	if (txn && sip_txn_request_get(txn, &req))
		resp_302_send(c, txn, &req, contact);

	/* the FSM asked for this, so no clearing class is owed */
	c->reported = true;
	leg_trace(c, "forwarded to %s", target);
	leg_end(c);

	/* True means the leg took the order, not that the 302 reached the wire,
	 * which is exactly what call_reject answers for its own response: a
	 * transaction that died under it leaves nothing for the caller to do */
	return true;
}

/*
 * The hangup of a leg this module already resolved. It exists because a
 * transfer's child holds no UI name at all, so the entry below could never
 * reach it, and a subscriber who goes on hook while a referral is running has
 * to take both legs with them.
 */
static bool leg_hangup(struct call *c)
{
	/* a user hangup owes the FSM nothing: it asked for this */
	c->reported = true;

	switch ((enum call_state)c->state) {
	case CALL_ST_INVITE_SENT:
	case CALL_ST_PROCEEDING:
		c->ui_busy = false;
		if (!c->prov_seen) {
			/* RFC 3261 9.1: the CANCEL waits for a provisional. The
			 * leg is clearing from this moment, so a 2xx or a
			 * challenge that arrives first starts nothing. */
			c->cancel_wanted = true;
			c->clearing = true;
			leg_trace(c, "hangup before any provisional, CANCEL deferred");
			return true;
		}
		if (cancel_send(c))
			state_set(c, CALL_ST_CANCEL_SENT);
		else
			leg_end(c);
		return true;
	case CALL_ST_RINGING_IN:
		return call_reject(c->line, c->ui_slot, 486);
	case CALL_ST_ACCEPT_WAIT_ACK:
	case CALL_ST_ACTIVE:
	case CALL_ST_REINVITE_SENT:
	case CALL_ST_REINVITE_WAIT_ACK:
		c->ui_busy = false;
		if (!bye_send(c))
			leg_end(c);
		return true;
	case CALL_ST_CANCEL_SENT:
	case CALL_ST_BYE_SENT:
		/*
		 * The path that put the BYE or the CANCEL on the wire decided the
		 * UI occupancy, and a decided transfer keeps it while its BYE
		 * drains (xfer_leg_clear). The machine's own hangup behind that
		 * failure must not hand the slot to leg_alloc() with the BYE
		 * still in flight.
		 */
		return true;
	case CALL_ST_FREE:
		break;
	}

	return false;
}

bool call_hangup(unsigned int line, unsigned int slot)
{
	struct call *c = leg_of_ui(line, slot);

	if (!c)
		return false;

	/*
	 * The subscriber left while a referral was running. The child has not
	 * taken this name over, so nothing it could still do belongs to
	 * anybody: the referral is reported terminated with the status of a
	 * request this side gave up on, and the child goes first, so the leg
	 * the FSM asked about is the last thing this frame releases.
	 */
	if (c->xfer_kid != SIP_HANDLE_NONE)
		xfer_child_drop(c, 487);

	return leg_hangup(c);
}

/*
 * The T.38 enable is set here and nowhere else on the outbound side.
 * sdp_offer_build() reads st->local_t38 alone and asks no further question, so
 * this layer's flag lifecycle is the only thing standing between a leg whose
 * channel or backend does not carry T.38 and an image offer on the wire. There
 * are exactly two setters, this one behind call_t38_start's enable and
 * admission gates and the inbound accept behind the same two, and every path
 * that abandons an exchange restores the flag beside the hold state.
 */
static bool reinvite_start(struct call *c, enum call_op op, bool hold)
{
	if (!leg_free_is(c))
		return false;

	/*
	 * The admission is re-checked at the offer, not only where the upgrade
	 * was decided. A 491 releases this leg's enable (uac_glare_take), so the
	 * daemon-wide session is free while the retry timer runs and another
	 * line may take it; the retry then has to compete for it again instead
	 * of admitting a second image session behind the first. Losing that race
	 * is not a failure of the call: the false travels the path a refused
	 * send already takes, and the leg keeps the G.711 passthrough it came
	 * with exactly as a peer refusal leaves it.
	 */
	if (op == CALL_OP_T38 && !t38_admission_free(c)) {
		leg_trace(c, "a T.38 session was taken while this upgrade waited, so "
			  "the offer is abandoned");
		return false;
	}

	/*
	 * No T.38 upspeed on an SRTP leg (hal.h, the srtp block). The image
	 * stream this board carries is plain UDPTL, so the upgrade would drop
	 * the encryption the call negotiated and say nothing about it. The leg
	 * keeps the audio session it has, and the G.711 passthrough the answer
	 * tone already applied keeps running.
	 *
	 * It is read here, beside the admission re-check and for the same
	 * reason: this is the one path that turns a decision into an image
	 * offer, so the glare retry of call_ua_glare_retry() passes it too.
	 *
	 * Both negotiation views are read. Between the answer take and the
	 * commit the suite lives in proposed alone, so a leg that has just
	 * negotiated SRTP and not yet committed it would otherwise pass a gate
	 * that read the committed view by itself.
	 */
	if (op == CALL_OP_T38 && (c->sdp.committed.srtp_suite != SDP_SRTP_NONE ||
				  c->sdp.proposed.srtp_suite != SDP_SRTP_NONE)) {
		leg_trace(c, "T.38 upspeed refused on an SRTP leg");
		return false;
	}

	exch_start_take(c);
	sdp_hold_set(&c->sdp, hold);
	/* the op is preserved across a 491 backoff (glare_wait_fire), so the
	 * upgrade is re-applied here rather than by its caller */
	if (op == CALL_OP_T38)
		sdp_t38_set(&c->sdp, true);

	if (!reinvite_offer_send(c)) {
		sdp_hold_set(&c->sdp, c->hold_prev);
		sdp_t38_set(&c->sdp, c->t38_prev);
		return false;
	}

	c->op = (uint8_t)op;
	state_set(c, CALL_ST_REINVITE_SENT);
	leg_trace(c, "re-INVITE sent");

	return true;
}

/*
 * A leg that says nothing takes no operation that owes an answer. The two
 * re-INVITE commands below report through cb.reinvite_done, which the
 * suppression invariant would swallow on a leg a transfer has demoted, and the
 * machine would then wait in PENDING for an outcome that can never arrive. The
 * refusal travels sip/line.c's deferred-failure path instead, so the cell takes
 * its fail arm inside the same frame. The fax and T.38 exchanges need no gate
 * of their own: neither reports to the machine at all.
 */
static bool xfer_op_ready(const struct call *c)
{
	return c && ui_emits(c);
}

bool call_hold(unsigned int line, unsigned int slot)
{
	struct call *c = leg_of_ui(line, slot);

	if (!xfer_op_ready(c))
		return false;

	return op_start(c, CALL_OP_HOLD, true);
}

bool call_resume(unsigned int line, unsigned int slot)
{
	struct call *c = leg_of_ui(line, slot);

	if (!xfer_op_ready(c))
		return false;

	return op_start(c, CALL_OP_RESUME, false);
}

/*
 * Whether an in-dialog request of ours can go out on this leg: the dialog is
 * confirmed and the session is not being torn down. A re-INVITE in either
 * direction does not stop it, because the dialog stands throughout.
 */
static bool leg_in_dialog(const struct call *c)
{
	return c->state == CALL_ST_ACTIVE || c->state == CALL_ST_REINVITE_SENT ||
	       c->state == CALL_ST_REINVITE_WAIT_ACK;
}

static bool dtmf_digit_ok(char digit)
{
	if (digit >= '0' && digit <= '9')
		return true;
	if (digit >= 'A' && digit <= 'D')
		return true;

	return digit == '*' || digit == '#';
}

bool call_dtmf(unsigned int line, unsigned int slot, char digit,
	       uint16_t duration_ms, bool measured)
{
	struct call *c = leg_of_ui(line, slot);
	char body[64];
	uint32_t txn;
	int n;

	if (!c || !dtmf_digit_ok(digit) || !leg_in_dialog(c))
		return false;

	/*
	 * The negotiated mode decides, not the configured one: an answer that
	 * carried no telephone-event downgraded the stream to inband
	 * (media_fill), and asking the HAL to relay a payload type the
	 * peer never agreed to sends nothing at all.
	 *
	 * A backend that advertises HAL_FEAT_DTMF_RELAY puts the RFC 4733
	 * event on the wire itself while the tone sounds, so an event from
	 * here sends the same digit a second time. The backend hears a
	 * measured digit alone: a pulse digit and an injected digit still go
	 * to the stream.
	 */
	switch (c->media_set ? (enum hal_dtmf_mode)c->media.dtmf_mode :
			       media_dtmf_of(c->ch.dtmf_mode)) {
	case HAL_DTMF_RFC2833:
		if (!cv.ops.dtmf_send || !c->media_started)
			return false;
		if (measured && (cv.hal_features & HAL_FEAT_DTMF_RELAY))
			return true;
		/* the stream is the leg's own slot and never the UI name */
		return cv.ops.dtmf_send(cv.ops.priv, line, c->slot, digit,
					duration_ms) == HAL_OK;
	case HAL_DTMF_INFO:
		n = snprintf(body, sizeof(body), "Signal=%c\r\nDuration=%u\r\n", digit,
			     duration_ms);
		if (n < 0 || (size_t)n >= sizeof(body))
			return false;
		return dialog_req_send(c, SIP_METHOD_INFO, "application/dtmf-relay", body,
				       (size_t)n, false, &txn, NULL);
	case HAL_DTMF_INBAND:
		/* the audio path carries the tone; nothing to send */
		return true;
	}

	return false;
}

/*
 * The wire form of a relayed hook flash: an in-dialog INFO on the same
 * broadsoft media type the network call waiting command arrives on,
 * with the flash event as the whole body. The audio/telephone-event
 * "Signal=!" form is not used, and RFC 2833 has no path on this hardware at
 * all.
 */
#define CALL_FLASH_CTYPE	CALL_CW_CTYPE
#define CALL_FLASH_BODY		"event flashhook\r\n"

/*
 * Fire and forget. The INFO rides the leg's normal in-dialog identity, so a
 * withheld call keeps the anonymous From it dialled with, and the CSeq is the
 * next one the dialog hands out. call_txn_resp() absorbs the answer.
 *
 * The handle holds one relay and a newer flash takes it. The superseded
 * transaction still runs to its own end, but its final answer then matches no
 * handle and lands in the branch that acts on nothing, so a refusal of it would
 * go unsaid. The trace below says so, and nothing more is needed: the two
 * handle tests downstream are equality tests, so an overwritten handle changes
 * no state and clears nothing that belongs to another request, and a
 * subscriber tapping the hook twice does not deserve a handle per
 * transaction.
 */
bool call_flash_relay(unsigned int line, unsigned int slot)
{
	struct call *c = leg_of_ui(line, slot);

	if (!c || !leg_in_dialog(c))
		return false;

	if (c->flash_txn != SIP_HANDLE_NONE)
		leg_trace(c, "flash INFO superseded by a newer one");

	return dialog_req_send(c, SIP_METHOD_INFO, CALL_FLASH_CTYPE, CALL_FLASH_BODY,
			       sizeof(CALL_FLASH_BODY) - 1, false, &c->flash_txn, NULL);
}

/* ------------------------------------------------------ G.711 fax upspeed */

static bool codec_is_g711(uint8_t id)
{
	return id == SDP_CODEC_PCMU || id == SDP_CODEC_PCMA;
}

/*
 * The G.711 this channel prefers: the first one of the leg's own offer list,
 * which was built in the configured priority order, with silence suppression
 * off. A channel that offers no G.711 at all still switches to PCMU, because a
 * fax needs an uncompressed carrier whatever the codec sections asked for.
 */
static void loc_g711_first(const struct call *c, struct sdp_codec *out)
{
	unsigned int i;

	for (i = 0; i < c->loc.codecs; i++) {
		if (!codec_is_g711(c->loc.codec[i].id))
			continue;

		*out = c->loc.codec[i];
		out->vad = false;
		return;
	}

	memset(out, 0, sizeof(*out));
	out->id = SDP_CODEC_PCMU;
	out->pt = media_codec_pt_of(SDP_CODEC_PCMU, &c->ch);
}

/*
 * The offer list of every later exchange on this leg, cut down to G.711. Both
 * members survive when both are enabled, in the
 * channel's own priority order, and the far end picks.
 *
 * Silence suppression and comfort noise go with them. A fax carrier through a
 * VAD gate loses the quiet parts of the page, so the offer must not ask for
 * either, and the answer can then not carry a CN payload type back.
 *
 * This is the offer-side half of that rule and media_fill() is the
 * stream-side half; neither is redundant. The comfort noise clear is the one
 * with a token on the wire, and G.711 has no way to express silence
 * suppression in SDP at all, so on this codec family the vad clear only keeps
 * the leg's own view honest for whatever a later exchange negotiates against
 * it. The stream is what the fill guarantees.
 *
 * It runs only once the DSP has taken the profile: a leg whose switch the
 * backend refused keeps the codec list it was negotiating with, so its next
 * hold or refresh offers what it always offered.
 *
 * The first clamp of a leg keeps the view it found, because a return to voice
 * has to put it back (call.h call_fax_exit). A second clamp takes no snapshot:
 * the list it would keep is the one the first clamp already reduced.
 */
static void loc_g711_clamp(struct call *c, const struct sdp_codec *pick)
{
	struct sdp_local *loc = &c->loc;
	unsigned int i, n = 0;

	if (!c->fax_loc_set) {
		memcpy(c->fax_loc_codec, loc->codec, sizeof(c->fax_loc_codec));
		c->fax_loc_codecs = loc->codecs;
		c->fax_loc_cn = loc->cn;
		c->fax_loc_set = true;
	}

	for (i = 0; i < loc->codecs; i++) {
		if (!codec_is_g711(loc->codec[i].id))
			continue;

		loc->codec[n] = loc->codec[i];
		loc->codec[n].vad = false;
		n++;
	}

	if (!n) {
		loc->codec[0] = *pick;
		n = 1;
	}

	loc->codecs = n;
	loc->cn = false;
}

/*
 * Everything a switch to the fax carrier does on this side alone, shared by the
 * two ways a leg reaches it: the answer tone this side heard (fax_upspeed_run
 * below) and the far end's own G.711 narrowing (fax_follow_check). Neither
 * carries an offer of ours, so nothing here touches the wire.
 *
 * Both signals prove only that a data device answered, so an unset kind
 * becomes the modem kind. An earlier V.21 preamble supplies stronger
 * evidence, so the leg keeps its fax kind.
 *
 * pick is the G.711 the DSP is asked for, and it is also what the clamp seats
 * when the leg's own list carries none at all.
 */
static bool fax_profile_local(struct call *c, const struct sdp_codec *pick)
{
	struct hal_fax_profile fax;

	media_fax_profile_build(pick, &fax);

	/*
	 * The DSP first. The re-INVITE only tells the far end what this side is
	 * already sending, and a switch made the other way round would put the
	 * negotiated codec on a stream still running through the voice path,
	 * with the canceller and the VAD gate on top of the carrier.
	 */
	if (!cv.ops.fax_apply ||
	    cv.ops.fax_apply(cv.ops.priv, c->line, c->slot, &fax) != HAL_OK) {
		leg_trace(c, "fax profile refused, no upspeed");
		return false;
	}

	/* the backend took it, so from here every later programming of this
	 * stream carries the profile-critical fields and the kind
	 * (media_fill) */
	c->fax_on = true;
	if (c->fax_kind == HAL_PASSTHROUGH_OFF)
		c->fax_kind = HAL_PASSTHROUGH_MODEM;
	leg_trace(c, "fax passthrough applied, codec %u pt %u", fax.codec, fax.codec_pt);
	loc_g711_clamp(c, pick);

	return true;
}

/*
 * The two steps of the upspeed, in the one order they may run in, without the
 * state check the entry below owns.
 */
static bool fax_upspeed_run(struct call *c)
{
	const struct sdp_result *com = &c->sdp.committed;
	struct sdp_codec pick;

	/*
	 * The committed codec while the call already runs on G.711, so the
	 * switch changes no payload on the wire, and the channel's preferred
	 * G.711 otherwise. Silence suppression is cleared on both branches: the
	 * same entry is the fallback the clamp seats, and a fax carrier asks
	 * for no VAD gate whatever the call negotiated.
	 */
	loc_g711_first(c, &pick);
	if (com->valid && codec_is_g711(com->codec.id)) {
		pick = com->codec;
		pick.vad = false;
	}

	if (!fax_profile_local(c, &pick))
		return false;

	c->glare_run = 0;

	if (!reinvite_start(c, CALL_OP_FAX, false)) {
		leg_trace(c, "fax upspeed re-INVITE refused, the local profile stays");
		return false;
	}

	return true;
}

/* whether the answer tone still stands as evidence, wrap safe by construction:
 * both operands are the same 32-bit monotonic reading and the difference is
 * taken unsigned, so a clock that wrapped between them still measures the true
 * distance */
static bool fax_hint_fresh(const struct call *c)
{
	if (!c->fax_hint_ms)
		return false;

	return uvoice_mono_ms() - c->fax_hint_ms <= CALL_FAX_HINT_WINDOW_MS;
}

/*
 * What the recognizer makes of one incoming offer. The verdict is read at two
 * points of the same exchange, once before the answer is built and once after
 * it is sent, so the gates live here and nowhere else: two copies of this list
 * would let the leg widen its codec view for an offer it then declines to
 * follow, or the reverse.
 *
 * TAKE is the only one that converts. NONE says nothing about a fax at all and
 * is the answer every ordinary re-INVITE gets, so the caller that traces stays
 * silent for it.
 */
enum fax_follow_verdict {
	FAX_FOLLOW_TAKE = 0,	/* both signals are in, and this leg follows */
	FAX_FOLLOW_NONE,	/* not a fax upspeed this leg can read */
	FAX_FOLLOW_CARRIED,	/* the leg is already in the passthrough */
	FAX_FOLLOW_OFF,		/* the channel does not follow one */
	FAX_FOLLOW_STALE,	/* no answer tone inside the window */
};

/*
 * Is this incoming re-INVITE the far end's own G.711 fax upspeed, and does this
 * leg follow it? A gateway that converts a call to a fax narrows its audio list
 * to G.711 and offers nothing else that says so, so neither signal decides
 * alone: an ordinary codec renegotiation looks the same on the wire, and an
 * answer tone alone says only that a data device answered somewhere.
 *
 * The two signals meet in this one exchange: the offered set lives on
 * rx_reinvite()'s stack and nowhere else (the committed view stores one codec),
 * and the tone arrived seconds earlier through call_fax_hint().
 *
 * The T.38 tests ride here rather than in the SDP predicate because they are
 * the leg's state and not the offer's: a session already carrying UDPTL, or one
 * this exchange's own answer just accepted, takes no G.711 profile. The
 * proposed view is the previous exchange's at the earlier of the two reading
 * points, where it equals the committed one, so the term costs that reading
 * nothing and covers the later one.
 *
 * The narrowing is not required to change the codec. A call that already ran on
 * G.711 is converted too, because the far side sheds comfort noise and silence
 * suppression on the same offer and the qualified answer tone is the evidence,
 * not the codec move.
 */
static enum fax_follow_verdict fax_follow_verdict_of(const struct call *c,
						     const struct sdp_msg *offer)
{
	if (!sdp_audio_g711_only(offer) || c->sdp.committed.t38 || c->sdp.proposed.t38)
		return FAX_FOLLOW_NONE;

	if (c->fax_on)
		return FAX_FOLLOW_CARRIED;

	if (!c->ch.fax_upspeed_follow)
		return FAX_FOLLOW_OFF;

	if (!fax_hint_fresh(c))
		return FAX_FOLLOW_STALE;

	return FAX_FOLLOW_TAKE;
}

/*
 * The G.711 a followed upspeed needs in the leg's own view, for the channel
 * that names none. Without it such a leg answers the narrowing offer 488:
 * codec_select() walks this list against the offered one and a G.729-only
 * channel shares nothing with a G.711-only offer, so the exchange the
 * recognizer wants to follow never completes.
 *
 * The entry is the same fallback the outgoing upspeed programs
 * (loc_g711_first: a fax needs an uncompressed carrier whatever the codec
 * sections asked for), so both directions convert the same provisioning the
 * same way.
 *
 * The fallback extends the list instead of replacing it, because a refused
 * profile must leave the committed codec available. If the profile applies,
 * loc_g711_clamp() reduces it to G.711, as the outgoing upspeed does.
 *
 * The view is the leg's private copy and never the config (shared/sdp.h, struct
 * sdp_local: the struct is copied into the leg at leg start, so a reload never
 * moves a live negotiation). loc_g711_clamp() moves the same field for the same
 * reason, and rx_reinvite() puts it back when the exchange does not complete.
 */
static void loc_g711_seat(struct call *c)
{
	struct sdp_local *loc = &c->loc;
	struct sdp_codec pick;
	unsigned int i;

	for (i = 0; i < loc->codecs; i++) {
		if (codec_is_g711(loc->codec[i].id))
			return;
	}

	/* a full list is left alone: the answer then refuses the offer 488 */
	if (loc->codecs >= SDP_CODEC_MAX)
		return;

	loc_g711_first(c, &pick);
	loc->codec[loc->codecs++] = pick;
	leg_trace(c, "the far end's fax upspeed adds %s pt %u to this leg's list",
		  sdp_codec_name((enum sdp_codec_id)pick.id), pick.pt);
}

/* rx_reinvite(): the leg's view, made able to answer a fax upspeed at all */
static void fax_follow_loc_prepare(struct call *c, const struct sdp_msg *offer)
{
	enum fax_follow_verdict v = fax_follow_verdict_of(c, offer);

	/*
	 * A leg already carrying the passthrough counts as following, and the
	 * seat is then a no-op: loc_g711_clamp() left its list holding G.711
	 * alone when the profile landed. It is admitted here so the two readings
	 * of the verdict answer one question and not two.
	 */
	if (v != FAX_FOLLOW_TAKE && v != FAX_FOLLOW_CARRIED)
		return;

	loc_g711_seat(c);
}

/*
 * The second reading of the same verdict, and the one that acts on it.
 *
 * Following costs no exchange of its own. The 200 for this very re-INVITE is
 * already out with the peer's own codec in it, so this side only has to put its
 * DSP on the carrier the far end is about to send. Rule 2 of t38_tone_event()
 * stands unbroken: a followed leg still offers no T.38.
 */
static void fax_follow_check(struct call *c, const struct sdp_msg *offer)
{
	switch (fax_follow_verdict_of(c, offer)) {
	case FAX_FOLLOW_NONE:
		/* an ordinary exchange: a hold, a refresh, a moved address, an
		 * image session. It earns no word of its own */
		return;
	case FAX_FOLLOW_CARRIED:
		/* The peer retries its upspeed after a 491 of ours, and the leg
		 * has one carrier either way. The offer is the far end's own
		 * conversion whichever branch answers it, so it proves the
		 * answer tone here as it does under TAKE below. */
		c->fax_evidence = true;
		leg_trace(c, "far-end upspeed repeat, the profile stands");
		return;
	case FAX_FOLLOW_OFF:
		leg_trace(c, "far-end G.711 upspeed ignored, fax_upspeed_follow is 0");
		return;
	case FAX_FOLLOW_STALE:
		leg_trace(c, "G.711-only re-INVITE without a recent answer tone, "
			  "the leg stays voice");
		return;
	case FAX_FOLLOW_TAKE:
		break;
	}

	/* The far end converted its own side, which is the strongest evidence
	 * this leg gets that the answer tone was real (call.h call_fax_exit).
	 * It counts whatever the backend then makes of the profile. */
	c->fax_evidence = true;

	/*
	 * The codec of the answer this frame just built, never the committed
	 * one: the peer may have narrowed to the G.711 this side did not
	 * commit to, and the profile has to name what the stream is about to
	 * carry.
	 */
	if (!fax_profile_local(c, &c->sdp.proposed.codec))
		return;

	leg_trace(c, "far-end G.711 upspeed followed, no offer of ours goes out");
}

bool call_fax_start(unsigned int line, unsigned int slot)
{
	struct call *c = leg_of_ui(line, slot);

	/*
	 * The line already decided that this leg is the one answered call with
	 * media; this is the leg's own half of it, and it is what keeps the
	 * switch off a leg with an exchange already running.
	 */
	if (!c || !leg_free_is(c))
		return false;

	if (c->fax_on) {
		leg_trace(c, "fax profile already applied, no second upspeed");
		return false;
	}

	/*
	 * A session that already carries T.38 takes no passthrough profile: a
	 * tone is ignored outright once the call carries T.38. The two stages
	 * are ordered, and the answer tone normally comes first, but its latch
	 * is the line's and is independent of the T.38 one: a CED burst that
	 * arrives after the upgrade, on a leg whose stage 1 never ran, would
	 * otherwise push a G.711 profile onto a UDPTL stream and re-offer the
	 * image session as a CALL_OP_FAX exchange, because the T.38 enable is
	 * still set.
	 */
	if (c->sdp.committed.t38) {
		leg_trace(c, "the session carries T.38, so the answer tone starts no "
			  "G.711 passthrough");
		return false;
	}

	return fax_upspeed_run(c);
}

/* call.h: the tone half of the far-end upspeed recognizer */
void call_fax_hint(unsigned int line, unsigned int slot)
{
	struct call *c = leg_of_ui(line, slot);

	if (!c)
		return;

	c->fax_hint_ms = uvoice_stamp_nonzero(uvoice_mono_ms());
}

/* call.h: the staleness test hook, and nothing else reaches this field */
bool call_fax_hint_stamp_set(unsigned int line, unsigned int slot, uint32_t stamp_ms)
{
	struct call *c = leg_of_ui(line, slot);

	if (!c)
		return false;

	c->fax_hint_ms = uvoice_stamp_nonzero(stamp_ms);

	return true;
}

/* call.h: whether this leg already carries a G.711 fax passthrough */
bool call_fax_on(unsigned int line, unsigned int slot)
{
	const struct call *c = leg_of_ui(line, slot);

	return c && c->fax_on;
}

uint8_t call_fax_kind_get(unsigned int line, unsigned int slot)
{
	const struct call *c = leg_of_ui(line, slot);

	return c ? c->fax_kind : HAL_PASSTHROUGH_OFF;
}

/*
 * The committed codec belongs to SIP, but bearer stimuli can change its
 * media policy without an exchange (V.152 10.1.1 and 10.3).
 * A refused description leaves the kind at the stream's last applied value,
 * so the next tone can retry it.
 */
bool call_fax_kind_set(unsigned int line, unsigned int slot, uint8_t kind)
{
	struct call *c = leg_of_ui(line, slot);
	uint8_t carried;

	if (!c)
		return false;

	if (kind == HAL_PASSTHROUGH_FAX && !c->fax_on &&
	    c->fax_kind == HAL_PASSTHROUGH_OFF)
		return false;

	/* A preamble supplies stronger evidence than a repeated answer tone. */
	if (kind == HAL_PASSTHROUGH_MODEM &&
	    c->fax_kind == HAL_PASSTHROUGH_FAX)
		return true;

	/*
	 * The preamble repeats through the T.30 negotiation, and a stream that
	 * already carries this kind needs nothing.
	 *
	 * The leg's own record has to agree with the stream for that to hold. A
	 * return to voice under an open exchange clears the record and leaves
	 * the stream carrying the kind until the answer lands (call_fax_exit),
	 * and a genuine answer tone inside that window has to reach the record
	 * so the answer carries it.
	 */
	if (c->media_set && c->media.passthrough == kind && c->fax_kind == kind)
		return true;

	carried = c->media_set ? c->media.passthrough : c->fax_kind;
	c->fax_kind = kind;

	/*
	 * An exchange of the leg's own is still open or waiting for its 491
	 * retry, which is the usual case: the preamble follows the answer tone
	 * by a second or two and the upspeed 200 OK has not landed yet. The
	 * committed answer is then the one from before the switch, so
	 * programming it here would put the pre-fax codec back on a stream the
	 * profile already clamped to G.711. The answer carries the kind
	 * instead: leg_commit_take() takes the new negotiation and
	 * media_program() fills the description from the field this frame just
	 * set. If no profile or T.38 session stands, reinvite_done() applies
	 * the recorded kind after a failed exchange.
	 */
	if (!leg_free_is(c)) {
		leg_trace(c, "fax kind %u waits for the leg's own exchange",
			  kind);
		return true;
	}

	/*
	 * A leg whose committed session is the image one keeps its description.
	 * That stream carries UDPTL and no carrier the kind describes, and the
	 * record is still worth keeping: a peer that drops the image session
	 * back to audio programs the stream from this kind.
	 */
	if (c->sdp.committed.t38) {
		leg_trace(c, "fax kind %u recorded, the session carries T.38",
			  kind);
		return true;
	}

	switch (media_commit_push(c)) {
	case MEDIA_PUSH_OK:
		break;
	case MEDIA_PUSH_ABANDONED:
		return false;
	case MEDIA_PUSH_REFUSED:
		c->fax_kind = carried;
		leg_trace(c, "fax kind %u refused by the stream", kind);
		return false;
	}

	leg_trace(c, "fax kind %u programmed", kind);

	return true;
}

/* ------------------------------------------------------- return to voice */

/*
 * The answer tone family. Each of these says a data device answered, and none
 * of them says whether it carries a fax (ITU-T V.152 clause 9).
 */
static bool fax_tone_is_answer(uint16_t code)
{
	switch (code) {
	case HAL_TONE_DET_CED:
	case HAL_TONE_DET_ANS:
	case HAL_TONE_DET_ANSAM:
	case HAL_TONE_DET_ANSBAR:
		return true;
	default:
		return false;
	}
}

/*
 * A row that proves the tone the leg is timing was real: another answer tone,
 * the V.21 preamble of a T.30 negotiation, or a modem carrier the detector
 * classified. A CNG belongs to a calling terminal and proves nothing here.
 */
static bool fax_tone_is_evidence(uint16_t code)
{
	return fax_tone_is_answer(code) ||
	       code == HAL_TONE_DET_V21_PREAMBLE ||
	       code == HAL_TONE_DET_MODEM;
}

/*
 * The image session of this leg, read from the same pair
 * fax_follow_verdict_of() reads. The proposed view is what makes the reading
 * whole: an accepted upgrade programs UDPTL on the stream at the 2xx and
 * commits only at the ACK, so a leg reading the committed view alone would
 * answer "no image session" for the length of that window.
 */
static bool fax_t38_session(const struct call *c)
{
	return c->sdp.committed.t38 || c->sdp.proposed.t38;
}

/*
 * Whether this leg carries a data call at all: the kind alone, the G.711
 * profile behind it, or an image session.
 *
 * The image session counts even where no tone of this leg ever armed a kind.
 * Its acceptance is the strongest evidence a leg gets, and a later answer tone
 * on it must read as one more row of a fax that is already running, not as the
 * first tone of a new one.
 */
static bool fax_carried(const struct call *c)
{
	return c->fax_on || c->fax_kind != HAL_PASSTHROUGH_OFF ||
	       fax_t38_session(c);
}

/* call.h: the tone half of the return-to-voice rule */
enum call_fax_tone call_fax_tone_note(unsigned int line, unsigned int slot,
				      uint16_t code, uint8_t direction,
				      bool end, uint16_t duration_ms)
{
	struct call *c = leg_of_ui(line, slot);

	if (!c)
		return end ? CALL_FAX_TONE_END : CALL_FAX_TONE_NOTED;

	if (!end && fax_carried(c)) {
		if (fax_tone_is_evidence(code))
			c->fax_evidence = true;

		return CALL_FAX_TONE_NOTED;
	}

	/*
	 * The arming candidate, taken on a leg with nothing to lose. The total
	 * and the evidence start again with it, because a leg that already
	 * returned to voice once is timing a new tone and not the old one.
	 */
	if (!end) {
		if (fax_tone_is_answer(code)) {
			c->fax_tone_code = code;
			c->fax_tone_dir = direction;
			c->fax_tone_ms = 0;
			c->fax_evidence = false;
		}

		return CALL_FAX_TONE_NOTED;
	}

	if (!fax_tone_is_answer(code) || code != c->fax_tone_code ||
	    direction != c->fax_tone_dir)
		return CALL_FAX_TONE_END;

	/*
	 * The sum and not the reading: once the concealment is off, a lost
	 * packet splits one real tone into two reports 500 ms apart
	 * (udsp/src/dsp/fax_det.c, ANSWER_MIN_FRAMES).
	 */
	c->fax_tone_ms += duration_ms;

	return CALL_FAX_TONE_ENDED;
}

/* call.h: the identity a caller holds across a timer window */
uint32_t call_leg_handle_get(unsigned int line, unsigned int ui_slot)
{
	const struct call *c = leg_of_ui(line, ui_slot);

	return c ? leg_handle(c) : SIP_HANDLE_NONE;
}

/*
 * The codec view the first clamp of this leg found. It runs behind a stream
 * that took the voice description again, so a later hold or refresh offers
 * what this leg offered before any answer tone reached it.
 */
static void loc_g711_restore(struct call *c)
{
	if (!c->fax_loc_set)
		return;

	memcpy(c->loc.codec, c->fax_loc_codec, sizeof(c->loc.codec));
	c->loc.codecs = c->fax_loc_codecs;
	c->loc.cn = c->fax_loc_cn;
	c->fax_loc_set = false;
	leg_trace(c, "the leg negotiates its %u codecs again", c->loc.codecs);
}

/*
 * The line's tone disabler, dropped. It is the line's and not the stream's, so
 * a description that carries no kind leaves it standing: under a plain tone it
 * holds for seconds of quiet (ITU-T G.168 7.8) and holds the DTMF detector
 * with it.
 *
 * Every refusal is traced and blocks nothing. The disabler releases on its own
 * quiet rule, and it re-arms on the next answer tone either way.
 */
static void fax_latch_clear(struct call *c)
{
	enum hal_status st;

	if (!cv.ops.lec_latch_clear) {
		leg_trace(c, "the backend runs no tone disabler to release");
		return;
	}

	st = cv.ops.lec_latch_clear(cv.ops.priv, c->line);
	if (st != HAL_OK)
		leg_trace(c, "the tone disabler release answered %s",
			  hal_status_name(st));
}

/* call.h: the return to voice after an isolated short plain answer tone */
bool call_fax_exit(uint32_t handle)
{
	struct call *c = leg_get(handle);
	uint32_t hint;
	uint8_t kind;
	bool on;

	if (!c || !fax_carried(c))
		return false;

	/*
	 * An image session is a fax on the wire, whatever the tone rows say.
	 * Nothing takes such a leg back to voice: the SIP session decides when
	 * a T.38 call ends, and releasing the line's disabler under a running
	 * page would put the canceller back on the carrier. The stream carries
	 * UDPTL from the 2xx, so the proposed view is read with the committed
	 * one.
	 */
	if (fax_t38_session(c)) {
		leg_trace(c, "the session carries T.38, so no answer tone "
			  "returns this leg to voice");
		return false;
	}

	if (c->fax_tone_code != HAL_TONE_DET_CED) {
		leg_trace(c, "%s armed this leg, and only the plain answer "
			  "tone returns to voice",
			  hal_tone_detect_name(c->fax_tone_code));
		return false;
	}

	if (c->fax_tone_ms >= CALL_FAX_TONE_MIN_MS) {
		leg_trace(c, "the answer tone ran %u ms, so the leg keeps "
			  "its kind", c->fax_tone_ms);
		return false;
	}

	if (c->fax_evidence) {
		leg_trace(c, "a later signal proved the answer tone, so "
			  "the leg keeps its kind");
		return false;
	}

	fax_latch_clear(c);

	kind = c->fax_kind;
	on = c->fax_on;
	hint = c->fax_hint_ms;
	c->fax_kind = HAL_PASSTHROUGH_OFF;
	c->fax_on = false;
	/* a G.711-only offer behind this point is an ordinary renegotiation:
	 * the tone that would have qualified it is the one this rule
	 * rejected */
	c->fax_hint_ms = 0;
	c->media_force = true;

	/*
	 * An exchange is open, normally the upspeed the answer tone sent. Two
	 * things wait for it. The leg's own offer is unanswered, and that
	 * answer selects from the leg's codec list (proto/shared/sdp.c
	 * sdp_answer_take), so the restore cannot run yet. A peer exchange this
	 * side already answered 2xx has its own description on the stream and
	 * the committed one is still the exchange before it, so programming
	 * here would move the stream backwards for the length of the ACK
	 * window.
	 *
	 * Both complete where the leg comes back to ACTIVE with its
	 * negotiation settled: reinvite_done() for an exchange of ours, on
	 * either outcome, and uas_ack_take() for a peer exchange, whose ACK
	 * pushes the committed description with no kind in it. An exchange
	 * that waits for its 491 retry reaches reinvite_done() through
	 * glare_wait_fire(), on the retry's own outcome or on its refusal.
	 *
	 * The fields go now and are kept, because that completion can still be
	 * refused.
	 */
	if (!leg_free_is(c)) {
		c->fax_exit_pending = true;
		c->fax_exit_kind = kind;
		c->fax_exit_on = on;
		c->fax_exit_hint_ms = hint;
		leg_trace(c, "the short answer tone takes the leg back to "
			  "voice, the codec list waits for the exchange");
		return true;
	}

	switch (media_commit_push(c)) {
	case MEDIA_PUSH_OK:
		break;
	case MEDIA_PUSH_ABANDONED:
		return false;
	case MEDIA_PUSH_REFUSED:
		c->fax_kind = kind;
		c->fax_on = on;
		c->fax_hint_ms = hint;
		c->media_force = false;
		leg_trace(c, "the stream refused the return to voice");
		return false;
	}

	loc_g711_restore(c);
	leg_trace(c, "the answer tone ran %u ms and nothing followed it, "
		  "the leg carries voice again", c->fax_tone_ms);

	return true;
}

/*
 * The second half of an exit that ran under an open exchange. A usable answer
 * has already programmed the stream from the leg's own fields, which carry no
 * kind since the exit, so only the codec list is left. A failure programs
 * nothing at all (reinvite_fail rolls the offer back), so the completion owes
 * the committed description and the force that carries an identical one.
 *
 * A refused push puts the three fields back, exactly as the immediate exit
 * does: the stream still runs the profile or the kind, and a leg that reported
 * voice over it would describe a call nothing is carrying. The codec list
 * stays clamped with them, because such a leg is in the passthrough again and
 * a later exchange must not offer a codec the DSP no longer decodes as voice.
 * fax_loc_set therefore stands, so the next exit of this leg still has the
 * view the first clamp found.
 */
static bool fax_exit_finish(struct call *c, bool ok)
{
	if (!c->fax_exit_pending)
		return true;

	c->fax_exit_pending = false;

	if (!ok) {
		c->media_force = true;
		switch (media_commit_push(c)) {
		case MEDIA_PUSH_OK:
			break;
		case MEDIA_PUSH_ABANDONED:
			return false;
		case MEDIA_PUSH_REFUSED:
			c->media_force = false;
			c->fax_kind = c->fax_exit_kind;
			c->fax_on = c->fax_exit_on;
			c->fax_hint_ms = c->fax_exit_hint_ms;
			/* the line already re-armed its stage 1 latch on the
			 * true answer, so this call offers no T.38 of its own
			 * any more and a later local tone meets the profile
			 * gate */
			leg_trace(c, "the stream refused the return to voice "
				  "after an exchange whose media push failed, "
				  "the fax stands and the line's tone latch "
				  "is spent");
			return true;
		}
	}

	loc_g711_restore(c);

	return true;
}

/* ---------------------------------------------------------- T.38 upgrade */

enum call_t38_answer call_t38_start(unsigned int line, unsigned int slot)
{
	struct call *c = leg_of_ui(line, slot);

	if (!c)
		return CALL_T38_REFUSED;

	/*
	 * The same leg-side half of the line's own gate as the upspeed above:
	 * one answered leg, with no exchange of its own already running. It is
	 * the one refusal the caller does not spend its attempt on, because the
	 * exchange in the way is normally the upspeed the answer tone sent and
	 * the preamble repeats until it lands (call.h, the answer).
	 */
	if (!leg_free_is(c)) {
		leg_trace(c, "this leg is not free, the T.38 upgrade waits");
		return CALL_T38_BUSY;
	}

	/*
	 * The carrier the upgrade replaces has to be on the leg. sip/line.c
	 * fires this stage from a leg its own first stage switched and takes
	 * that latch on the attempt, so a leg whose profile the backend refused
	 * carries the latch and a plain voice path: the image offer would then
	 * be made for a fax nothing on this side is carrying.
	 */
	if (!c->fax_on) {
		leg_trace(c, "no G.711 passthrough on this leg, so no T.38 upgrade");
		return CALL_T38_REFUSED;
	}

	if (!c->loc.t38) {
		leg_trace(c, "T.38 is not enabled on this leg");
		return CALL_T38_REFUSED;
	}

	if (!t38_admission_free(c)) {
		leg_trace(c, "a T.38 session already runs, this call keeps its audio");
		return CALL_T38_REFUSED;
	}

	if (c->sdp.committed.t38) {
		leg_trace(c, "the session already carries T.38");
		return CALL_T38_REFUSED;
	}

	c->t38_prev = c->sdp.local_t38;
	c->glare_run = 0;

	if (!reinvite_start(c, CALL_OP_T38, false))
		return CALL_T38_REFUSED;

	return CALL_T38_STARTED;
}

/* ------------------------------------------------------- the transfer entries */

/*
 * The leg-side half of both transfer entries (call.h, the refusal list): a leg
 * the UI still owns, with a confirmed dialog of its own, and with no REFER
 * already outstanding. leg_in_dialog() is the same predicate every other
 * in-dialog request of this module is admitted against, so an early dialog and
 * a leg that is clearing are refused here exactly as they are there.
 */
static bool xfer_leg_ready(const struct call *c)
{
	return c && c->ui_busy && ui_emits(c) && leg_in_dialog(c) &&
	       !c->xfer_pending && c->refer_txn == SIP_HANDLE_NONE;
}

bool call_xfer(unsigned int line, unsigned int slot, enum call_xfer_kind kind,
	       const char *target)
{
	struct call *c = leg_of_ui(line, slot);

	/* the consult kind needs the second leg only call_xfer_consult() has */
	if (kind == CALL_XFER_CONSULT || !xfer_leg_ready(c) || !target || !*target)
		return false;
	if (!target_uri_build(c, target, c->xfer_target, sizeof(c->xfer_target)))
		return false;

	c->xfer_kind = (uint8_t)kind;
	c->xfer_consult = false;
	c->xfer_other = SIP_HANDLE_NONE;
	/* the REFER exchange gets its own credential budget, the rule every
	 * other in-dialog exchange of this leg follows */
	memset(c->dlg_auth_retry, 0, sizeof(c->dlg_auth_retry));

	if (!refer_send(c, NULL)) {
		c->xfer_target[0] = 0;
		return false;
	}

	/* From here an outcome is owed: false means no wire exchange happened
	 * and no callback follows, so the two can never both be true */
	c->xfer_pending = true;
	c->xfer_owed = true;

	return true;
}

bool call_xfer_consult(unsigned int line, unsigned int referee_slot,
		       unsigned int replaced_slot)
{
	struct call *c = leg_of_ui(line, referee_slot);
	struct call *o = leg_of_ui(line, replaced_slot);
	struct sip_dialog *od;

	/*
	 * The replaced leg takes the same confirmed-dialog rule as the leg the
	 * REFER rides on (call.h: the same synchronous refusal contract). A
	 * Replaces that named an early dialog would need the early-only
	 * parameter this module never emits, and the target would answer for
	 * a call it has not accepted yet.
	 *
	 * It also owes no transfer of its own. A leg with an outcome still
	 * pending is being handed somewhere else already, and handing it here
	 * as well would leave two transfers racing for one dialog.
	 */
	if (!xfer_leg_ready(c) || !o || o == c || !o->ui_busy || !leg_in_dialog(o) ||
	    o->xfer_pending)
		return false;

	/* the target is the remote target of the replaced dialog, which is
	 * where that leg's own in-dialog requests already go */
	od = leg_dialog(o);
	if (!od || !od->remote_target)
		return false;

	/* the refuse-not-truncate rule of target_uri_build(), at the one target
	 * that arrives from the wire: half a remote target names nobody */
	if (strlen(od->remote_target) >= sizeof(c->xfer_target))
		return false;

	str_copy(c->xfer_target, sizeof(c->xfer_target), od->remote_target);
	c->xfer_kind = CALL_XFER_CONSULT;
	c->xfer_consult = true;
	c->xfer_other = leg_handle(o);
	memset(c->dlg_auth_retry, 0, sizeof(c->dlg_auth_retry));

	/*
	 * A refusal cleans nothing (call.h): both legs are still up with nobody
	 * left to clear them, and sip/line.c's op wrapper call_hangup()s both.
	 * Only this entry's own bookkeeping is put back.
	 */
	if (!refer_send(c, o)) {
		c->xfer_target[0] = 0;
		c->xfer_consult = false;
		c->xfer_other = SIP_HANDLE_NONE;
		return false;
	}

	/* the subscription alone: this kind reports nothing (xfer_outcome) */
	c->xfer_pending = true;

	return true;
}

/* --------------------------------------------------------------- lifecycle */

void calls_init(const struct call_media_ops *ops, const struct call_cb *cb)
{
	unsigned int line, slot;

	memset(&cv, 0, sizeof(cv));
	if (ops)
		cv.ops = *ops;
	if (cb)
		cv.cb = *cb;
	cv.inited = true;

	memset(calls, 0, sizeof(calls));
	/* the refusal counters start at the daemon start and nowhere else:
	 * calls_reload() leaves them, so a reload cannot hide the calls a line
	 * turned away */
	memset(refused, 0, sizeof(refused));
	for (line = 0; line < CFG_CHANNEL_MAX; line++) {
		for (slot = 0; slot < CALL_SLOTS; slot++) {
			calls[line][slot].line = line;
			calls[line][slot].slot = slot;
			calls[line][slot].gen = 1;
		}
	}
}

void calls_host_bind(const struct proto_host *host)
{
	cv.host = host;
}

/* Separate from calls_init(), which clears it: the shell wires the layer once
 * and the readiness it reads moves on its own (call.h). */
void calls_hal_ready_bind(bool (*hal_ready)(void))
{
	cv.hal_ready = hal_ready;
}

/* Separate from calls_init() for the reason calls_hal_ready_bind() is: the
 * service state of a line belongs to the shell (call.h). */
void calls_line_admit_bind(bool (*line_admit)(unsigned int line))
{
	cv.line_admit = line_admit;
}

/* The new config governs the next leg; a live one keeps the snapshot it
 * started with (the cfg lifetime rule). */
void calls_reload(const struct cfg *cfg)
{
	cv.cfg = cfg;
}

/* Separate from calls_init(), which clears it, for the reason
 * calls_hal_ready_bind() is separate: the capability belongs to the shell. */
void calls_hal_features_set(uint32_t features)
{
	cv.hal_features = features;
}

/* Separate from calls_init() for the reason calls_hal_features_set() is, and
 * pushed beside it on every ops->dsp_ready (sip/proto.c). */
void calls_hal_codecs_set(uint32_t codecs)
{
	cv.hal_codecs = codecs;
}

void calls_line_teardown(unsigned int line)
{
	unsigned int i;

	if (line >= CFG_CHANNEL_MAX)
		return;

	for (i = 0; i < CALL_SLOTS; i++) {
		struct call *c = &calls[line][i];

		if (!c->in_use)
			continue;

		leg_trace(c, "line teardown");

		/*
		 * A leg the FSM still owns has to hear that it is gone: the
		 * account is being rebuilt or the interface address moved under
		 * it, and the FSM would otherwise stay in a call state with no
		 * leg behind it. The leg is terminated, not renegotiated.
		 *
		 * A leg a transfer demoted speaks again for this one report. It
		 * still holds its UI slot, and the child that was going to take
		 * that slot over holds none, so suppressing it here would take
		 * the line down with nobody left to tell the machine.
		 */
		if (c->ui_role == CALL_UI_QUIET)
			c->ui_role = CALL_UI_OWNER;
		if (c->ui_busy)
			cb_failed(c, CALL_FAIL_QUIET);
		c->reported = true;
		c->ui_busy = false;
		c->clearing = true;

		/*
		 * Bounded: nothing waits here. A leg whose clearing request is
		 * on the wire is left alive to finish it, because releasing it
		 * would detach its INVITE client transaction and the ACK that
		 * transaction owes for the 487 would never be built
		 * (RFC 3261 17.1.1.3). The account grace window drains it, and
		 * account_teardown() calls this again, which ends whatever is
		 * left through the default branch below.
		 */
		switch ((enum call_state)c->state) {
		case CALL_ST_ACCEPT_WAIT_ACK:
		case CALL_ST_ACTIVE:
		case CALL_ST_REINVITE_SENT:
		case CALL_ST_REINVITE_WAIT_ACK:
			if (bye_send(c))
				continue;
			break;
		case CALL_ST_INVITE_SENT:
		case CALL_ST_PROCEEDING:
			if (c->prov_seen && cancel_send(c)) {
				state_set(c, CALL_ST_CANCEL_SENT);
				continue;
			}
			break;
		case CALL_ST_RINGING_IN:
			call_reject(line, c->ui_slot, 480);
			continue;
		case CALL_ST_CANCEL_SENT:
		case CALL_ST_BYE_SENT:
		case CALL_ST_FREE:
			break;
		}

		leg_end(c);
	}
}

/*
 * Whether any leg of a line still holds protocol state. The reload drain waits
 * on it so a BYE or a CANCEL still on the wire keeps its transaction, and with
 * it the ACK that transaction owes, until the grace deadline.
 */
bool calls_line_busy(unsigned int line)
{
	unsigned int i;

	if (line >= CFG_CHANNEL_MAX)
		return false;

	for (i = 0; i < CALL_SLOTS; i++) {
		if (calls[line][i].in_use)
			return true;
	}

	return false;
}

static bool dst_listed(const struct sip_txn_dst *const *list, unsigned int n,
		       const struct sip_txn_dst *dst)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		if (sip_dst_eq(list[i], dst))
			return true;
	}

	return false;
}

const struct sip_txn_dst *calls_line_dialog_dst(unsigned int line,
						unsigned int idx)
{
	const struct sip_txn_dst *found[CALL_SLOTS];
	unsigned int n = 0;
	unsigned int i;

	if (line >= CFG_CHANNEL_MAX)
		return NULL;

	for (i = 0; i < CALL_SLOTS; i++) {
		struct call *c = &calls[line][i];
		struct sip_dialog *d = c->in_use ? leg_dialog(c) : NULL;
		const struct sip_txn_dst *dst = d ? sip_dialog_dst(d) : NULL;

		if (!dst || dst_listed(found, n, dst))
			continue;

		found[n++] = dst;
	}

	return idx < n ? found[idx] : NULL;
}

/*
 * The stop half of the teardown. calls_line_teardown() leaves a leg whose BYE
 * or CANCEL is on the wire alive to finish it, because a caller that tears a
 * line down for a reload calls it again once the drain ends (account_teardown).
 * Nothing runs after this one, so the leg ends here, through the ordinary
 * release, and its record closes with the clear the teardown BYE already
 * stamped (cdr.h "Time").
 */
static void line_legs_end(unsigned int line)
{
	unsigned int i;

	for (i = 0; i < CALL_SLOTS; i++) {
		struct call *c = &calls[line][i];

		if (!c->in_use)
			continue;

		leg_trace(c, "daemon stop");
		leg_end(c);
	}
}

/*
 * The two run per line and before the wipe: the wipe takes the callback table
 * with it, so a report made after it would reach nobody and the record of a call
 * that was up at the stop would stay open for good.
 */
void calls_line_end(unsigned int line)
{
	if (line >= CFG_CHANNEL_MAX)
		return;

	calls_line_teardown(line);
	line_legs_end(line);
}

void calls_done(void)
{
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++)
		calls_line_end(i);

	memset(&cv, 0, sizeof(cv));
}

/* ------------------------------------------------------------------ status */

/*
 * What this line turned away, written only once it turned something away: a
 * line that refused nothing carries no table, so the key itself says there is
 * something to read. The last refusal travels as an age and not as a stamp,
 * because the stamp is on the transaction clock and a reader of this page has
 * no way to age one.
 */
static void refused_dump(struct blob_buf *b, unsigned int line)
{
	const struct call_refused *r = &refused[line];
	uint64_t ago = 0;
	void *entry;

	if (!r->busy && !r->unavail && !r->other)
		return;

	if (r->env && r->env->base.now_ms) {
		uint64_t now = r->env->base.now_ms(&r->env->base);

		ago = now > r->last_ms ? now - r->last_ms : 0;
	}

	entry = blobmsg_open_table(b, "refused");
	blobmsg_add_u32(b, "busy", r->busy);
	blobmsg_add_u32(b, "unavailable", r->unavail);
	blobmsg_add_u32(b, "other", r->other);
	blobmsg_add_u32(b, "last_code", r->last_code);
	blobmsg_add_u32(b, "last_age_ms",
			ago > UINT32_MAX ? UINT32_MAX : (uint32_t)ago);
	blobmsg_close_table(b, entry);
}

/*
 * A T.38 session negotiates no audio codec, so the name is written here rather
 * than by sdp_codec_name(), whose vocabulary stays the audio one. The ptime key
 * keeps its place beside it and carries the 0 an image exchange pins.
 */
static void codec_dump(struct blob_buf *b, const struct call *c)
{
	blobmsg_add_string(b, "codec",
			   c->sdp.committed.t38 ? "t38" :
			   sdp_codec_name((enum sdp_codec_id)
					  c->sdp.committed.codec.id));
	blobmsg_add_u32(b, "ptime", c->sdp.committed.ptime_ms);
}

void call_codec_dump(struct blob_buf *b, unsigned int line,
		     unsigned int ui_slot)
{
	const struct call *c = leg_of_ui(line, ui_slot);

	if (c && c->sdp.committed.valid)
		codec_dump(b, c);
}

void calls_status_dump(struct blob_buf *b, unsigned int line)
{
	void *arr, *entry;
	unsigned int i;

	if (line >= CFG_CHANNEL_MAX)
		return;

	arr = blobmsg_open_array(b, "calls");
	for (i = 0; i < CALL_SLOTS; i++) {
		struct call *c = &calls[line][i];

		if (!c->in_use)
			continue;

		entry = blobmsg_open_table(b, NULL);
		blobmsg_add_u32(b, "slot", i);
		/*
		 * Both names of the leg (call.h, the two-name contract). "slot"
		 * stays the physical one every key beside it describes, and
		 * "ui_slot" is the name the machine knows the leg by; the two
		 * differ only on a line a transfer moved. "shadow" is present
		 * for a leg that does not speak for its name: the child of a
		 * referral before it takes one over, the leg it replaced once
		 * it has, and the leg still carrying the call while a referral
		 * it accepted is decided.
		 */
		blobmsg_add_u32(b, "ui_slot", c->ui_slot);
		if (c->ui_role != CALL_UI_OWNER)
			blobmsg_add_u8(b, "shadow", true);
		blobmsg_add_string(b, "state", state_name((enum call_state)c->state));
		blobmsg_add_u8(b, "ui_busy", c->ui_busy);
		blobmsg_add_u8(b, "inbound", !c->uac);
		if (c->number[0])
			blobmsg_add_string(b, "number", c->number);
		/* the real parsed identity, withheld or not: the daemon knows
		 * it, and only the subscriber-facing render hides it. The flag
		 * beside it is what an operator or the e2e reads to tell a
		 * withheld caller from one who never sent a number. */
		if (c->cid_number[0])
			blobmsg_add_string(b, "remote", c->cid_number);
		if (c->cid_name[0])
			blobmsg_add_string(b, "name", c->cid_name);
		if (c->cid_number_private)
			blobmsg_add_u8(b, "cid_private", true);
		if (c->sdp.committed.valid) {
			codec_dump(b, c);
			blobmsg_add_string(b, "direction",
					   sdp_dir_name((enum sdp_dir)c->sdp.committed.dir));
			/*
			 * The SDES suite this call negotiated, present only
			 * where it negotiated one, the way the fax key below is
			 * present only for a call that carries fax. It is
			 * never a key and never a digest of one: the key
			 * material reaches no trace, log or dump (hal.h, the
			 * srtp block).
			 */
			if (c->sdp.committed.srtp_suite != SDP_SRTP_NONE)
				blobmsg_add_string(b, "srtp",
						   sdp_srtp_suite_name(
							   (enum sdp_srtp_suite)
							   c->sdp.committed.srtp_suite));
		}
		/*
		 * What this call is doing about fax, one word for the two
		 * mutually exclusive answers and absent for a
		 * plain voice call. The committed session wins: a leg that
		 * upgraded from passthrough carries both states, and the image
		 * session is the one the media is running.
		 */
		if (c->sdp.committed.t38) {
			blobmsg_add_string(b, "fax", "t38");
		} else if (c->fax_on || c->fax_kind != HAL_PASSTHROUGH_OFF) {
			blobmsg_add_string(b, "fax", "passthrough");
			/* What the carrier is, which is what the canceller
			 * policy of the line follows (hal.h). The answer tone
			 * names the modem kind and the V.21 preamble the fax
			 * one, so the word also says which stage the call
			 * reached. */
			blobmsg_add_string(b, "fax_kind",
					   c->fax_kind == HAL_PASSTHROUGH_FAX ?
					   "fax" : "modem");
		}
		if (c->rtp_port)
			blobmsg_add_u32(b, "rtp_port", c->rtp_port);
		if (c->sess_interval) {
			blobmsg_add_u32(b, "session_expires", c->sess_interval);
			blobmsg_add_u8(b, "we_refresh", c->we_refresh);
		}
		blobmsg_close_table(b, entry);
	}
	blobmsg_close_array(b, arr);

	refused_dump(b, line);
}
