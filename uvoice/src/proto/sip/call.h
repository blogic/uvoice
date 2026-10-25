// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Call legs: the SIP side of one FXS call (src/proto/sip/call.c).
 *
 * What it binds. One UI call slot to one INVITE dialog: the UAC path
 * (INVITE, auth resend, CANCEL, ACK, BYE), the UAS path (180/200, the 2xx
 * retransmission through ua_accept_add, CANCEL and 487), re-INVITE both
 * directions (hold, resume, a moved remote address), session timers
 * (RFC 4028) and the SDP exchange through shared/sdp.h. This is the only
 * module that programs HAL streams: media follows the negotiation
 * exactly, including exchanges the FSM never sees (a session refresh, a
 * far-end hold), so stream ops routed anywhere else would let media state
 * and UI state diverge. Tones, ring and CID belong to line/line.c.
 *
 * Slots. calls[CFG_CHANNEL_MAX][CALL_SLOTS] static: two slots per line,
 * because call waiting needs two live legs on one line. UI occupancy
 * (ui_busy, freed the moment the FSM leaves the call) is separate from
 * protocol lifetime: dialogs and transactions drain on their own references,
 * for up to 32 s behind a BYE and for up to CALL_XFER_WAIT_MS on a referred
 * leg that waits for the network's BYE, and call_slot_free() reads ui_busy
 * alone, so a draining leg never blocks a new call and an occupied slot
 * always does; a drain that still owns a dialog is BYEd when a new call takes
 * its slot (leg_alloc). Both slots
 * occupied answers a new INVITE 486 before any dialog is created, with one
 * exception, and it is an ordering exception rather than a capacity one: an
 * INVITE that carries a Replaces is read first (RFC 3891), because such a
 * request replaces a call this line already holds instead of adding one. The
 * refusals it earns are truer than a capacity answer in front of them, and
 * the capacity answer still follows a match that found a dialog but no slot
 * to put the replacement on.
 *
 * A slot has two names (the two-name contract). The physical slot is the leg:
 * the HAL stream index, the call record key and the leg handle. The UI
 * slot is the name the FSM knows that leg by. On a line no transfer has
 * touched they are the same number for every leg, and they come apart in two
 * exchanges, one per transfer role this line plays. A transfer this line
 * receives puts the child INVITE on the other physical slot and hands it the
 * parent's UI name; a transfer completed here, an INVITE that carries a
 * Replaces (RFC 3891), puts the replacement on the other physical slot and
 * hands it the name of the leg it replaces. Either way the machine never sees
 * the legs change under it. The name moves at one of two moments: when the
 * leg taking the call over is answered, which is the child's own 2xx or the
 * ACK of the 2xx this side gave a replacement, or, when the leg being
 * replaced ends first, at that leg's release. A line that has carried one
 * transfer keeps the two names apart until both legs are gone, so a leg
 * allocated later takes whichever name is free rather than the number of the
 * physical slot it sits on.
 *
 * Which name every entry of this header speaks, once, so nobody derives it
 * again. The FSM commands below, call_slot_stream() and every callback of
 * struct call_cb but two take the UI slot. cb.started and cb.ended take the
 * leg's own slot, because a record belongs to a leg and not to a name, and
 * both of them are invisible to the FSM. call_slot_free() and
 * call_slot_handle() name the physical slot, because a new leg is allocated on
 * one; call_slot_scan() reads one slot's storage and is physical for the same
 * reason.
 *
 * Ownership and routing. Every transaction this module starts carries owner
 * acct_owner_make(OWNER_CALL, call handle); account.c routes the env resp
 * and done upcalls back through call_txn_resp() and call_txn_done() by that
 * kind. The receive path is the documented order in sip/ua.h: account.c
 * feeds new requests through ua_request_policy() and hands the survivors to
 * call_rx_request(), first ACKs to call_rx_ack(), and stray responses that
 * the dialog layer did not absorb to call_rx_stray().
 *
 * Failure classes. SIP outcomes reach the FSM as one of four clearing
 * classes, not as codes: 486/600/603 busy; 5xx, 408, timer C, transport
 * errors and port exhaustion congestion; 404/484/604 unobtainable; a far
 * end that vanished (session refresh failure, missing ACK) quiet.
 *
 * Testability. Media ops and the SIP env are injected. test_call drives a
 * leg with the virtual clock env from test_sip_txn.c, a scripted peer and a
 * recording media_ops; no socket, no uloop, no HAL backend.
 */
#ifndef UVOICE_CALL_H
#define UVOICE_CALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cfg/cfg.h"
#include "hal/hal.h"
#include "proto/proto_media.h"
#include "proto/shared/msg.h"
#include "proto/sip/txn.h"

#define CALL_SLOTS		2
#define CALL_NUMBER_MAX		64
#define CALL_CID_MAX		64

/*
 * The transfer guards.
 *
 * CALL_XFER_WAIT_MS bounds the transferor's implicit REFER subscription
 * (RFC 3515 2.4.4 and 2.4.6). The REFER transaction ends at the 202, so no
 * transaction timer bounds the outcome: the NOTIFY that carries it can never
 * arrive. Expiry is the failure verdict, reported as code 408.
 *
 * It is Timer C. The wait is for a call the target has to answer. A shorter
 * guard fails a transfer whose only fault is a subscriber who lets the phone
 * ring: at 30 s, a slow-ringing target fails every time. Timer C already
 * bounds an INVITE that nobody answers (CALL_TIMER_C_DEFAULT, 180 s). The
 * transfer inherits that bound and fires only on a far end that is dead or
 * silent.
 *
 * CALL_XFER_BYE_MS bounds the transferee's wait for the transferor to end the
 * replaced parent leg after the child answers. A transferor that never sends
 * that BYE would leave two legs on one line forever, so the transferee BYEs
 * the parent itself when the guard expires. The value is a policy choice,
 * not a measured bound: nothing waits on a subscriber here, only on a peer
 * that owes an immediate BYE.
 *
 * CALL_XFER_DRAIN_MS bounds the consult transferor's own replaced leg after
 * the 202 (RFC 5589). That leg is not BYEd here: the target the referred leg
 * reached replaces it and the network sends the Replaces-driven BYE on it, so
 * this side keeps the dialog alive to answer that BYE 200. The guard is a
 * leak-prevention fallback, so it must err long: a short guard would BYE
 * first and re-open the race against a network BYE that arrives late, which
 * can be about 16 s after the 202. It takes Timer C for
 * the reason CALL_XFER_WAIT_MS does.
 *
 * CALL_XFER_CONSULT_WAIT_MS bounds the consult transferor's referee leg
 * between the 202 and the verdict NOTIFY (RFC 5589 7.1: the transferor BYEs
 * the transferee after the NOTIFY that reports the 2xx). The Replaces names
 * a confirmed dialog, so no party has to answer before the verdict: a switch
 * notifies within 3 ms of the 202, and a REFER a proxy forwards costs the
 * transferee one INVITE round trip. It is short on purpose: both
 * legs hold their UI occupancy through the wait, so an off-hook inside it
 * meets the congestion refusal, and a peer that never notifies would hold
 * the line for the whole of a Timer C guard. The expiry BYEs the referee and
 * leaves the replaced leg to its own drain guard. The value is a policy
 * choice, not a measured bound.
 *
 * CALL_XFER_BYE_GRACE_MS bounds the wait between a 2xx verdict and this
 * side's own BYE of the referee, on every kind. RFC 5589 section 6.1 puts
 * that BYE on the transferor, so this side owes one; a switch that ends both
 * transferor legs itself sends its own 1 ms behind its final NOTIFY. A BYE
 * from this side 0.2 ms behind the same NOTIFY meets a 481, because the
 * switch's BYE is already out. One second lets that BYE arrive first,
 * answered 200 with no report, and still ends the leg on a peer that sends
 * none. It rides t_xfer, whose guard is consumed by the verdict, and a flag
 * on the leg tells the fire which of the two purposes is armed. The value is
 * a policy choice, not a measured bound.
 */
#define CALL_XFER_WAIT_MS	180000
#define CALL_XFER_BYE_MS	5000
#define CALL_XFER_DRAIN_MS	180000
#define CALL_XFER_CONSULT_WAIT_MS	5000
#define CALL_XFER_BYE_GRACE_MS	1000

/*
 * The size of the in-dialog extra-header buffer of wire_extra_insert(). The
 * session headers and the credentials of a refresh alone fit in 896 bytes; a
 * REFER puts three more headers through the same buffer, and the buffer
 * refuses rather than truncates (an overflow makes the insert return false,
 * so the request never reaches the wire).
 *
 * The worst case is the REFER, term by term. The field limits are the ones
 * call.c and sip/auth.h name; the widths below are the value width, that is
 * the array size minus the NUL:
 *
 *   Refer-To                                                   1108
 *     "Refer-To: <"                                              11
 *     the target URI, CALL_URI_MAX - 1                           319
 *     "?Replaces="                                                10
 *     the embedded Replaces, escaped by uri_header_escape():      765
 *       raw value 255 = CALL_ID_MAX - 1 (191) + ";to-tag=" (8)
 *         + UA_TAG_MAX - 1 (23) + ";from-tag=" (10) + 23,
 *       and every byte of it can become %XX, so 3 x 255
 *     ">" and CRLF                                                 3
 *   Referred-By (RFC 3892 5)                                     336
 *     "Referred-By: <" (14) + CALL_URI_MAX - 1 (319) + ">" + CRLF
 *   Two credential headers, because the REFER               2 x 1154
 *   auth retry answers a 401 and a 407
 *     "Proxy-Authorization" (19) + ": Digest " (9)                28
 *     username, CALL_USER_MAX - 1                                106
 *     realm, SIP_AUTH_REALM_MAX - 1                              137
 *     nonce, SIP_AUTH_NONCE_MAX - 1                              265
 *     uri, CALL_URI_MAX - 1                                      327
 *     response, SIP_AUTH_HEX_MAX - 1                              77
 *     algorithm, cnonce, qop, nc                                  74
 *     opaque, SIP_AUTH_OPAQUE_MAX - 1                            138
 *     CRLF                                                         2
 *     (each field carries its own name, its quotes and the ", "
 *      that separates it from the one before)
 *   ------------------------------------------------------------------
 *   1108 + 336 + 2308 = 3752, rounded up to a 256 byte multiple
 *
 * The RFC 4028 session headers of a refresh (63 bytes) and the Supported
 * header of every re-INVITE (28 bytes) ride the same buffer. A REFER carries
 * none of them and the credentials dominate both cases, so the REFER total is
 * the bound.
 *
 * One expansion is deliberately not budgeted: the quoted-string escape of
 * hdr_cred_write(), which doubles a realm, a nonce or an opaque made of '"'
 * or '\' bytes. Budgeting it would put the buffer past CALL_WIRE_MAX, which
 * the insert can never fill anyway, for a peer nobody has met. Such a peer
 * overflows the buffer and the request is refused with a trace, which is the
 * refuse-not-truncate rule this layer applies everywhere else.
 */
#define CALL_WIRE_EXTRA_MAX	3840

/*
 * SIP final -> FSM clearing class. The class is the shared one of
 * proto_media.h, and the SIP names stay as aliases.
 */
#define call_fail_class		proto_fail_class
#define CALL_FAIL_BUSY		PROTO_FAIL_BUSY
#define CALL_FAIL_CONGESTION	PROTO_FAIL_CONGESTION
#define CALL_FAIL_UNOBTAINABLE	PROTO_FAIL_UNOBTAINABLE
#define CALL_FAIL_QUIET		PROTO_FAIL_QUIET
#define call_fail_name		proto_fail_name

/*
 * The injected stream ops are struct call_media_ops of proto_media.h. The
 * daemon wraps hal_backend_ops; test_call records. line and stream are the
 * HAL indexes; one leg drives stream = its slot.
 */

/*
 * The BroadWorks network call waiting command of an in-dialog INFO (the
 * CW_NET cell of CONNECTED). The switch holds the waiting call itself and asks
 * this line for the subscriber alerting signal, so the record carries the
 * caller identity the alert presents. Both strings hold what the line CID
 * record holds and are empty when the INFO carried none.
 */
struct call_cw {
	bool play;			/* false is "stop CallWaitingTone" */
	uint8_t tone;			/* GR-506 pattern 1 to 4, play only */
	char name[HAL_CID_STR_MAX];
	char number[HAL_CID_STR_MAX];
};

/*
 * What one leg was, reported once when it is released. It is the call record
 * source: the FSM learns nothing from it, so it carries the facts a record
 * needs and no verdict. The three strings point into the leg and are valid
 * for the callback only, like every other string here.
 *
 * The clear is reported as an age, not as a time. A leg
 * a BYE cleared keeps its slot while the BYE transaction drains, up to timer K
 * later, and the call itself ended at the BYE. A leg a transfer silenced
 * keeps its dialog for the network's BYE, up to the transfer guard later, and
 * the call itself ended at the silence, the 202 or the verdict. cleared_ago_ms
 * is the distance from that end to this report, measured on the leg's own env
 * clock, so the reader subtracts it from its own elapsed time and the two
 * clock domains never meet. cleared is false for every other ending, and then
 * cleared_ago_ms is 0.
 *
 * The record is struct proto_call_ended of proto_media.h. codec carries the
 * enum hal_codec of the committed sdp codec, which call.c maps at the release.
 *
 * transferred does not claim the transfer completed, because the leg cannot
 * know: a transferee's parent is ended by the transferor's BYE while the child
 * is still ringing. Three legs carry it, one per role: the transferor's leg,
 * which its own BYE ended after the 202 or after the sipfrag 2xx; the
 * transferee's parent leg, whose call a child was placed to take over; and the
 * target's replaced leg, which this side BYEs the moment the ACK of its own
 * answer completes the replacement. The child's record carries the other half
 * of the pair (cdr.h).
 *
 * answered_ago_ms exists for the leg nobody announced. A record normally
 * learns the answer moment from cb.answered or from the accepted answer, and a
 * transfer's child makes neither report.
 */
#define call_ended		proto_call_ended

/*
 * Events toward sip/line.c and fsm.c, all on the uloop thread. The cid and name
 * strings are valid for the duration of the callback only; sip/line.c copies
 * what it keeps. ring_id is the Alert-Info distinctive ring (0 = default).
 *
 * The strings and the flags say different things. The strings keep the
 * identity the SIP layer parsed, whether or not the caller withheld it: the
 * daemon may know a number the subscriber may not see, and the status dump
 * and the trace both need it. number_private and name_private say what the
 * subscriber may be shown, and the backend renders the withheld field as
 * the GR-30 reason letter instead of the value. A caller with no identity at
 * all arrives as two empty strings with both flags clear.
 */
struct call_cb {
	void (*incoming)(unsigned int line, unsigned int slot,
			 const char *cid_number, const char *cid_name,
			 bool number_private, bool name_private,
			 unsigned int ring_id);
	/*
	 * early_media says the answer of this 18x programmed and started the
	 * early stream. It does not say audio is arriving: the far end may
	 * send nothing for seconds, or nothing at all. The line shell keeps
	 * the local ringback playing until the first packet of that stream
	 * reaches it, and stops it then (RFC 3960 3.2, sip/line.c
	 * early_rx_arm()).
	 */
	void (*proceeding)(unsigned int line, unsigned int slot,
			   bool early_media);
	void (*answered)(unsigned int line, unsigned int slot);
	void (*failed)(unsigned int line, unsigned int slot,
		       enum call_fail_class cls);
	/*
	 * A peer BYE for an answered leg. It is also the far-end clear the
	 * forward disconnect of CONNECTED runs on, and it is the
	 * only report that carries the distinction: a peer that ends an early
	 * dialog arrives through failed() instead, and the quiet clearing
	 * class those failures share also holds a session timer death and a
	 * transport death, neither of which the far end made. sip/line.c
	 * narrows it from there.
	 *
	 * It never follows an end this leg already reported. A leg the
	 * subscriber hung up, one a line teardown cleared and one a decided
	 * transfer cleared each resolved their slot already, so the peer BYE
	 * that crossed our own is answered and ends the leg without reaching
	 * the machine. A rejected or forwarded leg cannot even meet that BYE:
	 * its same-frame release ends the dialog, so no later BYE resolves to
	 * it. The last-word clause of cb.xfer_done below is the transfer
	 * instance of this same rule.
	 */
	void (*remote_bye)(unsigned int line, unsigned int slot);
	void (*remote_cancel)(unsigned int line, unsigned int slot);
	void (*hold_changed)(unsigned int line, unsigned int slot, bool held);
	void (*reinvite_done)(unsigned int line, unsigned int slot, bool ok);
	/*
	 * The outcome of one call_xfer() or call_xfer_consult(), fired
	 * exactly once per accepted request and never for a refused one. code
	 * carries the status the verdict came from: 202 for an accepted blind
	 * transfer, the sipfrag 2xx of an attended one, the failing SIP
	 * status, or 408 when the CALL_XFER_WAIT_MS guard expired. sip/line.c
	 * maps ok to FSM_EV_CALL_XFER_OK and !ok to FSM_EV_CALL_XFER_FAIL.
	 *
	 * A failure leaves the leg alive (see call_xfer): the transfer failed,
	 * the call did not, so the FSM resumes the leg it held. The consult
	 * form never reports here at all (call_xfer_consult below): it ends
	 * its own legs and the machine is idle by then.
	 *
	 * An OK outcome is the last word on that slot. call.c has already
	 * silenced the leg by the time this fires: it holds no UI name and no
	 * UI occupancy, so no further FSM-visible event follows for that slot,
	 * no remote_bye, no failed. The cell that consumes the OK therefore
	 * owns the slot roles from that instant and must not wait for an
	 * ending event that will never arrive. The leg itself may still be in
	 * use while its dialog drains for the network's BYE (enum
	 * call_xfer_kind), and cb.ended fires whenever that ends, because a
	 * record is owed for every leg that was in use; the FSM learns nothing
	 * from it.
	 */
	void (*xfer_done)(unsigned int line, unsigned int slot, bool ok,
			  uint32_t code);
	/*
	 * The network call waiting alert of a BroadWorks INFO. on is
	 * the play, tone the GR-506 pattern 1 to 4 on a play and 0 on a
	 * stop, name and number the parsed INFO fields, empty when absent
	 * and valid for the callback only. There is no second leg: the
	 * switch owns the waiting call, and slot names the leg the INFO
	 * arrived on.
	 */
	void (*cw_net)(unsigned int line, unsigned int slot, bool on,
		       unsigned int tone, const char *name, const char *number);
	/*
	 * A leg this module admitted by itself, and two exchanges make one,
	 * one per transfer role: the child INVITE of a transfer this line was
	 * asked to take, and the INVITE carrying a Replaces that completes one
	 * here (RFC 3891). No FSM command dialled either and no offer rang
	 * either, so nothing else in this table announces them, and the one
	 * record per leg the CDR handoff owes would have no start. It fires
	 * once, at the moment the leg reaches the wire: the child's own INVITE
	 * going out, or the 2xx this side answers a replacement with. The FSM
	 * learns nothing from it either way: the whole point of both
	 * exchanges is that the machine never sees them.
	 *
	 * inbound says which direction the leg took, exactly as the ended
	 * report below says it, so the reader of a record never has to infer
	 * it from who opened one.
	 *
	 * from_slot is the other half of the correlation: the slot of the leg
	 * whose call this one was placed to take over, or CALL_SLOTS when there
	 * is none. It is a slot and not a record handle, because this layer
	 * knows legs and the record layer knows records; the shell joins them,
	 * and it can, because the leg named here still holds its record when
	 * this fires.
	 */
	void (*started)(unsigned int line, unsigned int slot, bool inbound,
			unsigned int from_slot);
	/*
	 * Exactly once per leg that was in use, from the release path. The
	 * in_use guard of that path is what makes the single fire structural
	 * rather than a rule this layer has to remember. It reports what the
	 * leg was, whatever ended it, so a record exists for a call the FSM
	 * was never told about.
	 *
	 * It names the leg's own slot and not the UI one (the two-name contract
	 * at the top of this header). A transferee's parent and its child both
	 * hold the same UI name for a while, so a name could not tell their two
	 * records apart; the slot each leg was allocated on can, and it is also
	 * the slot the media snapshot of that leg was taken on.
	 */
	void (*ended)(unsigned int line, unsigned int slot,
		      const struct call_ended *info);
	/*
	 * The digit the far end sent out of band, in a SIP INFO body this
	 * module parsed (call_dtmf_relay_parse). slot is the UI slot of the
	 * leg the INFO arrived on, digit is one of 0 to 9, star, pound and A
	 * to D, and duration_ms is the length the body announced, 0 when it
	 * announced none. The shell decides whether the digit is played
	 * toward the phone; this layer only reports it.
	 */
	void (*digit_net)(unsigned int line, unsigned int slot, char digit,
			  uint16_t duration_ms);
};

void calls_init(const struct call_media_ops *ops, const struct call_cb *cb);

/*
 * The core services of the daemon (proto/proto.h struct proto_host). A leg
 * draws its RTP port from the pool of the core through it, inside the effective
 * range of its channel, which is the range the firewall reconcile opens. It
 * must be bound after calls_init(), which clears it, and before the first
 * leg.
 */
struct proto_host;
void calls_host_bind(const struct proto_host *host);
void calls_reload(const struct cfg *cfg);
/*
 * The backend feature mask (enum hal_feature). The shell reads the
 * capability the backend probed and hands the mask down, exactly as it hands
 * down the readiness predicate above: this layer asks the HAL nothing. The one
 * reader is the leg start, which enables T.38 on a leg only when the channel
 * asks for it and the backend carries HAL_FEAT_T38, so a board without the
 * feature never offers an image stream and never accepts one.
 *
 * It must be bound after calls_init(), which clears it. A layer with no mask
 * carries no optional feature, which is what a suite that drives the legs
 * directly gets.
 */
void calls_hal_features_set(uint32_t features);

/*
 * The backend codec mask (1 << enum hal_codec). It crosses exactly as
 * the feature mask above does, and for the same reason: the offer this layer
 * builds must name only codecs the board can run. A channel that also names an
 * advertised codec offers that one. A channel whose only codec section names an
 * unadvertised codec offers nothing at all, and the leg is refused, which is
 * the shipped behaviour of a line with no usable codec.
 *
 * It must be bound after calls_init(), which clears it. A layer with no mask
 * offers no codec, which is the honest answer for a daemon that has not been
 * told what the hardware carries; a suite that drives the legs directly pushes
 * ~0u for the same reason it binds the two entries above.
 */
void calls_hal_codecs_set(uint32_t codecs);

/*
 * The running total of the wideband digit fallback, counted once per exchange
 * and published beside the daemon counters (ubus.c, the media table). An Opus
 * exchange whose peer named no telephone-event/48000 relays no digits, and
 * the warning alone leaves an operator counting log lines.
 *
 * It is a daemon total, not a per-leg one: it survives calls_init() the way the
 * event counters survive a reload, so a reload cannot hide the calls that ran
 * before it.
 */
unsigned int calls_opus_dtmf_fallbacks(void);
void calls_done(void);

/*
 * The DSP readiness a new INVITE is admitted against. The line shell wires
 * this layer at start-up, before the DSP has finished its bring-up, so a call
 * can be offered to a line whose hardware cannot carry it yet, and a bring-up
 * that failed never carries one at all. The predicate is injected rather than
 * read here: the daemon HAL state belongs to the shell, which already reads
 * it for the region load and the media statistics, and this layer stays free
 * of it.
 *
 * It must be bound after calls_init(), which clears it. A layer with no
 * predicate bound admits every call, which is what a suite that drives the
 * legs directly gets.
 */
void calls_hal_ready_bind(bool (*hal_ready)(void));

/*
 * The service state of the line a new INVITE names. The shell answers it
 * (line/line.h line_call_admit): a line it holds out of service runs a machine
 * that ignores the incoming call event, so a leg allocated for such a line
 * gives the caller ringback on a port that stays silent. The admission rule
 * answers 480 instead, in front of the leg.
 *
 * It is injected for the reason the readiness predicate above is: the service
 * state belongs to the shell and this layer stays free of it. It must be bound
 * after calls_init(), which clears it, and a layer with no predicate bound
 * admits every line, which is what a suite that drives the legs directly gets.
 */
void calls_line_admit_bind(bool (*line_admit)(unsigned int line));

/*
 * Best effort teardown of every leg on one line: BYE what is confirmed,
 * CANCEL what is early, release the protocol objects. account.c calls it
 * before it replaces or removes an account and on shutdown; the existing
 * 3 s grace window doubles as the drain.
 */
void calls_line_teardown(unsigned int line);

/*
 * The daemon stop of one line: the teardown above, then the end of every leg
 * the teardown left alive to finish its BYE or CANCEL. Nothing waits; the
 * media of every leg stops here, while the backend is still open.
 */
void calls_line_end(unsigned int line);

/*
 * The pre-dialog admission check (486 before dialog creation).
 * True when a UI-free slot exists; fills *slot. The slot is physical: it is
 * the one a new leg is allocated on, and call_slot_handle() below turns it
 * into that leg's handle.
 */
bool call_slot_free(unsigned int line, unsigned int *slot);

/*
 * Which physical stream a UI slot is running on (the two-name contract at the
 * top of this header). The shell keeps its per-stream state by the physical
 * index, because that is what it hands the backend, while a northbound query
 * and the FSM both name a UI slot: this is the one lookup that joins them, and
 * a second copy of it would drift the day a transfer moves a leg.
 *
 * It answers CALL_SLOTS when no leg holds that UI slot, which is the same
 * answer an idle line gives, so a caller needs no second emptiness test.
 */
unsigned int call_slot_stream(unsigned int line, unsigned int ui_slot);

/*
 * The strongest passthrough kind (enum hal_passthrough) the running legs of one
 * line describe, for the lec row of "voice status": it is where the canceller
 * policy of the line comes from whenever call control named one.
 *
 * A leg counts only while its stream runs and is not on local hold, the two
 * rules the library applies to the same question
 * (udsp/src/core/stream.c stream_governs), so a held fax leg beside a live
 * voice leg answers HAL_PASSTHROUGH_OFF exactly as the DSP resolves it. The
 * modem kind wins over the fax kind. A line with no such leg answers
 * HAL_PASSTHROUGH_OFF, which the caller reads as "call control named nothing"
 * and not as "the canceller is off".
 */
uint8_t calls_line_fax_kind(unsigned int line, bool *media_running);

/*
 * Commands from the FSM (via sip/line.c); all return false on refusal.
 *
 * call_dial carries the resolved presentation: cid_present true
 * presents the number, false withholds it, and the leg then sends the RFC 3323
 * anonymous From with a Privacy header instead of the account identity.
 * sip/line.c owns the precedence (one-call override, persistent runtime state,
 * config); the leg only obeys the verdict.
 *
 * A dial that finds no place to put the call reports congestion and then
 * returns false. A silent false would leave the machine, which has already
 * entered CALLING by the time the op runs, waiting for an outcome nothing was
 * left to send: the refusal the missing account reports is also made for a
 * UI slot another leg holds and for a line with no free physical slot. A
 * caller error, an unknown line, an unknown slot or an empty number answer
 * false in silence, because none of them names a slot an event could be
 * reported on.
 */
bool call_dial(unsigned int line, unsigned int slot, const char *number,
	       bool cid_present);

/*
 * Whether the account of the line can carry a dial now: the account exists,
 * is not being replaced or stopped, has a registrar target, and holds a live
 * registration (account.h acct_reg_live). It is the account half of the
 * call_dial refusal, from one helper, so a line that this answers false for
 * gets the congestion refusal from call_dial as well. The INVITE that a REFER
 * asks for tests the target alone, so a failed refresh does not refuse a
 * transfer inside a call. sip/line.c asks it at every seize, and the IDLE SEIZE
 * cell plays the fast busy chain instead of a dial tone on false.
 */
bool calls_line_dial_ready(unsigned int line);
bool call_ring(unsigned int line, unsigned int slot);		/* send 180 */
bool call_answer(unsigned int line, unsigned int slot);		/* send 2xx */
bool call_reject(unsigned int line, unsigned int slot, uint32_t code);

/*
 * Local call forwarding: answer the offered call 302 with a Contact of
 * sip:<target>@<the account domain>, and end the leg as a refusal the caller
 * asked for. There is no Diversion header, because this line does not
 * re-originate the call. False when the
 * slot holds no offered call or the target is empty; the caller then keeps
 * whatever treatment it would have given.
 */
bool call_forward(unsigned int line, unsigned int slot, const char *target);
bool call_hangup(unsigned int line, unsigned int slot);	/* CANCEL or BYE */
bool call_hold(unsigned int line, unsigned int slot);
bool call_resume(unsigned int line, unsigned int slot);
/*
 * measured says a detector on this line timed the digit. A backend that
 * advertises HAL_FEAT_DTMF_RELAY relays such a digit itself, so the RFC2833
 * dispatch sends nothing more for it. A pulse digit and an injected digit
 * carry false: no detector heard them, so they reach the stream on every
 * backend.
 */
bool call_dtmf(unsigned int line, unsigned int slot, char digit,
	       uint16_t duration_ms, bool measured);

/*
 * The hook flash of a connected leg, relayed to the far end as an in-dialog
 * INFO. Fire and forget: the
 * final response is absorbed, a
 * non-2xx is traced, and nothing else changes. False when the leg holds no
 * confirmed dialog to send it on; the caller only traces.
 */
bool call_flash_relay(unsigned int line, unsigned int slot);

/*
 * Apply the G.711 profile before the upspeed re-INVITE, so the offer describes
 * media the backend already runs. The offer withdraws suppression and comfort
 * noise even when the committed codec is already G.711.
 * The codec clamp and profile persist if the peer refuses the offer.
 * The clamp lasts for the rest of the leg, so later peer re-INVITEs
 * negotiate against G.711 alone.
 *
 * sip/line.c calls this for a local answer-family tone on a connected line.
 * The caller must select the active, unheld leg with running media.
 * A leg with only a kind still converts: T.30 6.1.7 permits the calling
 * terminal to answer a manual fax procedure. A leg with the profile refuses
 * a second upspeed, which would restart playout and send another offer.
 *
 * A missing or inactive leg, an exchange in progress, an existing profile,
 * or a T.38 session answers false. A refused profile or a failed send also
 * answers false.
 * A failed send leaves the applied profile in place.
 */
bool call_fax_start(unsigned int line, unsigned int slot);

/*
 * How long an answer tone from the wire stays evidence that the far end is
 * converting this call to a fax. Every such tone re-stamps the leg, so the
 * window runs from the last one: ten seconds covers the far gateway's own
 * media switch, one full 491 glare retry of the re-INVITE it then sends
 * (RFC 3261 14.1 draws that retry from 2.1 to 4 seconds) and the transit
 * behind both.
 */
#define CALL_FAX_HINT_WINDOW_MS	10000

/*
 * Record a remote answer-family tone for the G.711 upspeed recognizer.
 * sip/line.c separately arms the modem kind without a profile or an offer.
 * The hint and a G.711-only offer within CALL_FAX_HINT_WINDOW_MS permit the
 * profile and codec clamp; neither signal alone identifies that offer.
 * A slot with no leg records nothing.
 */
void call_fax_hint(unsigned int line, unsigned int slot);

/*
 * True only after the G.711 profile applies, not for a kind alone.
 * sip/line.c uses this distinction to keep a followed leg from taking the local
 * latch and offering T.38. A slot with no leg answers false.
 */
bool call_fax_on(unsigned int line, unsigned int slot);

/*
 * The answer-tone stamp of one leg, written directly. It is exported for one
 * reason, under the rule call_slot_scan() below is exported under: the
 * staleness of the hint has no other independent check. The window is ten
 * seconds of monotonic time, so a suite that could not write the field would
 * have to sleep through it or shim the clock, and neither belongs in a unit
 * test.
 *
 * The nonzero convention is applied here too, so a caller cannot forge the
 * "no hint at all" state through it. False for a slot with no leg.
 */
bool call_fax_hint_stamp_set(unsigned int line, unsigned int slot, uint32_t stamp_ms);

/*
 * Set the media policy on the leg's current codec without a profile or offer.
 * The modem kind needs only a leg. The fax kind needs an existing kind or
 * profile, because a preamble alone must not start passthrough.
 * The modem kind never replaces the fax kind: a preamble supplies stronger
 * evidence than a repeated answer tone.
 *
 * A kind change re-programs the committed answer without an offer.
 * media_program() compares the whole description against its cache, so one
 * changed kind field causes one config_set even when the codec stays unchanged.
 *
 * An exchange in progress records the kind for its next description.
 * A committed T.38 session records it without a push.
 * A kind the stream already carries answers true without another config_set.
 * A fax request on a leg that carries neither a kind nor the profile
 * answers false. A missing leg or a refused description also answers false.
 * A refusal restores the kind the stream carries so the next tone can retry.
 */
bool call_fax_kind_set(unsigned int line, unsigned int slot, uint8_t kind);

/*
 * sip/line.c reads the recorded kind in t38_tone_event()'s preamble gate.
 * It can precede the description during an exchange, so the gate does not
 * depend on whether the stream already carries it.
 * A slot with no leg answers HAL_PASSTHROUGH_OFF.
 */
uint8_t call_fax_kind_get(unsigned int line, unsigned int slot);

/*
 * How long a plain answer tone must last before the leg keeps its kind for
 * good. A fax CED runs 2.6 to 4.0 s (ITU-T T.30 4.1.1) and a plain V.25
 * answering tone runs 3.3 +- 0.7 s (ITU-T V.25 2.2), so 2000 ms sits 600 ms
 * under the floor of both.
 *
 * A shorter tone is not proof of a false one. A calling station response
 * truncates a plain V.25 answering tone, which can then end about 0.6 s after
 * it starts (ITU-T V.25 3.9 and 4.3). Such a leg returns to voice unless a
 * carrier follows inside the deferral below.
 */
#define CALL_FAX_TONE_MIN_MS	2000

/*
 * How long the leg waits after the end row before it returns to voice. A
 * genuine data device fills this window: 75 +- 20 ms of silence follows the
 * answer tone (ITU-T V.25 4.4), a V.34 fax that heard no CM waits 450 +- 50 ms
 * before its first preamble (ITU-T T.30 Amd.1 6.1.3), the detector needs
 * ANSWER_MIN_FRAMES (500 ms) to report a tone that resumed after a loss gap,
 * and a MODEM row needs 1010 ms of carrier.
 */
#define CALL_FAX_EXIT_DEFER_MS	1500

/*
 * What one tone row means to the return-to-voice rule.
 *
 * NOTED is every start row. END is an end row of a tone the leg is not timing.
 * ENDED is the end row of the tone that armed the rule, and its duration is in
 * the leg's running total by the time the answer arrives.
 *
 * sip/line.c drops the end rows of the answer family and of the V.21 preamble
 * behind this answer, so an edge the detector reports twice starts no second
 * upspeed and no second image offer.
 */
enum call_fax_tone {
	CALL_FAX_TONE_NOTED = 0,
	CALL_FAX_TONE_END,
	CALL_FAX_TONE_ENDED,
};

/*
 * Record one tone row against the return-to-voice rule, before any other gate
 * reads it.
 *
 * A start row on a leg that carries no kind records the arming candidate, its
 * code and its feed, and clears the running total. A start row on a leg that
 * carries a kind counts evidence for the answer family, the V.21 preamble and
 * a modem carrier: each of them says the tone was real. An end row of the
 * arming tone, on the same feed, adds duration_ms to the total.
 *
 * A slot with no leg records nothing and answers by the edge alone.
 */
enum call_fax_tone call_fax_tone_note(unsigned int line, unsigned int slot,
				      uint16_t code, uint8_t direction,
				      bool end, uint16_t duration_ms);

/*
 * The handle of the leg a UI slot names, or SIP_HANDLE_NONE for a slot no leg
 * holds. A UI slot is not an identity of its own: a transfer moves the name
 * between two physical legs (the two-name contract at the top of this header),
 * and a released leg bumps the generation the handle carries. A caller that
 * has to name one leg across a timer window takes this and hands it back.
 */
uint32_t call_leg_handle_get(unsigned int line, unsigned int ui_slot);

/*
 * Return the leg to voice after an isolated short plain answer tone.
 *
 * The rule: the arming tone is HAL_TONE_DET_CED, the only code a fax CED, a
 * V.25 answering tone without phase reversals and a music note all reach; its
 * accumulated duration is under CALL_FAX_TONE_MIN_MS; and no evidence landed
 * since. ANSam and ANSbar never arm it, because both end early by design
 * (ITU-T V.8 8.2.2, ITU-T T.30 6.1.1).
 *
 * The exit clears the line's tone-disabler latch, drops the kind, the profile
 * and the answer-tone stamp, programs the committed description and puts the
 * leg's codec list back the way loc_g711_clamp() found it.
 *
 * False for a handle that names no live leg, for a leg with no kind, for an
 * arming tone that is not the plain one, for a tone that ran long enough, for
 * evidence that stands, and for a description the stream refused. A refused
 * description leaves every fax field as it was.
 *
 * An exchange of the leg's own defers the second half: the fax fields go at
 * once, the codec list waits, because the answer of an outstanding upspeed
 * selects from that list. reinvite_done() finishes it on either outcome.
 */
bool call_fax_exit(uint32_t handle);

/*
 * What one upgrade attempt did. The two refusals are told apart because the
 * caller spends its one attempt on them differently: BUSY says the leg is
 * running an exchange of its own, normally the upspeed of the answer tone,
 * so nothing reached the peer and the next preamble of the same burst has a
 * leg to work on. Every other refusal is final for this call.
 */
enum call_t38_answer {
	CALL_T38_STARTED = 0,
	CALL_T38_BUSY,
	CALL_T38_REFUSED,
};

/*
 * The T.38 upgrade. sip/line.c calls it when its detector reports a V.21
 * preamble toward the phone on a line with one answered leg that already
 * carries the G.711 passthrough, which is the second stage of a two-stage
 * trigger: the CED family started the passthrough, and the preamble
 * says the terminals are talking fax.
 *
 * The leg re-INVITEs with an image offer and programs the stream when the peer
 * answers one. A peer that refuses the upgrade keeps the audio session, and the
 * leg keeps the G.711 passthrough it already carries: sip/line.c starts no
 * upgrade from a leg without it, so a refusal has nothing left to convert.
 *
 * Six refusals, all silent but for a trace: no leg, a leg that is not active or
 * is running an exchange of its own, a leg that carries no G.711 passthrough
 * for the image session to replace, a leg whose channel or backend does not
 * carry T.38, a daemon that already runs its one T.38 session, and a session
 * that is already an image one. The second of them answers BUSY and the rest
 * answer REFUSED. sip/line.c latches its own attempt on everything but BUSY, so
 * a refusal costs that call its T.38 and never repeats.
 *
 * The passthrough refusal is the leg's own half of sip/line.c's first rule, and
 * it is not redundant with it: sip/line.c latches its first stage on the
 * attempt, so a leg whose profile the backend refused passes that gate carrying
 * a plain voice path.
 */
enum call_t38_answer call_t38_start(unsigned int line, unsigned int slot);

/*
 * Which transfer the transferor asked for.
 *
 * CALL_XFER_BLIND sends the REFER and reports the outcome at the 202, so the
 * subscriber is done the moment the far end accepts the referral. The leg is
 * not BYEd there: it falls silent, gives
 * its UI occupancy up and keeps its dialog and its subscription, so the
 * transferee's NOTIFYs are answered 200, a switch that re-INVITEs the leg
 * with c=0.0.0.0 behind the 202 gets a 2xx, and the BYE comes from whoever
 * owes it. A verdict of 300 or above, a subscription that ends with no
 * sipfrag, and the CALL_XFER_WAIT_MS guard BYE the leg, because nobody is
 * left to resume it. A peer that accepts the REFER and then neither notifies
 * nor BYEs holds the slot, the RTP port and the dialog for that guard, on a
 * leg whose subscriber left at the 202: the value is the subscription bound
 * (a slow-ringing target), and leg_alloc() evicts the drain for a new call
 * before then.
 *
 * CALL_XFER_ATTENDED sends the same REFER and waits for the NOTIFY that
 * carries the outcome, bounded by
 * CALL_XFER_WAIT_MS.
 *
 * CALL_XFER_ONHOOK is the blind REFER of a subscriber who is already on hook:
 * the on-hook transfer of a second call that still rings (fsm.c
 * outbound_xfer). It is the wire shape and the
 * 202 outcome of CALL_XFER_BLIND, and it differs in the failure arm alone: a
 * refused REFER BYEs the leg too, because nobody is on this line to resume
 * it and the machine may be anywhere by the time the failure arrives. The
 * leg gives its UI occupancy up at the 202, unlike the consult form: its
 * drain is bounded by CALL_XFER_WAIT_MS, and a line held for that long would
 * refuse every inbound call for a subscriber who is already on hook. sip/line.c
 * selects it in its op wrapper (op_call_xfer); no star-code row maps to it.
 *
 * CALL_XFER_CONSULT is the on-hook consult form: the
 * REFER carries an embedded Replaces naming the second leg, and the 202
 * silences both legs and waits for the verdict NOTIFY under
 * CALL_XFER_CONSULT_WAIT_MS before the referee is BYEd. It needs two legs,
 * so only call_xfer_consult() below selects it; call_xfer() refuses it.
 *
 * The option names are shifted one slot against these two. State it once
 * here so nobody derives it again:
 *
 *   blind_transfer_exten     -> CALL_XFER_BLIND
 *   attended_transfer_exten  -> CALL_XFER_ATTENDED
 *   consult_transfer_exten   -> call_xfer_consult()
 *
 * That is, the "attended" option is a REFER that waits for its verdict, and
 * the transfer that RFC 5589 calls attended is the on-hook consult form.
 */
enum call_xfer_kind {
	CALL_XFER_BLIND = 0,
	CALL_XFER_ATTENDED,
	CALL_XFER_ONHOOK,
	CALL_XFER_CONSULT,
};

/*
 * Send a REFER on the leg in slot (RFC 3515). The FSM has already put that
 * leg on hold through the PENDING pattern before it asks for the transfer, so
 * this entry holds nothing itself.
 *
 * target is a Refer-To: the same target shapes call_dial() accepts, a dialled
 * number or a URI, resolved the same way against the account domain.
 * Referred-By carries our own identity (RFC 3892 5).
 *
 * The outcome arrives exactly once, through cb.xfer_done, and never for a
 * false return:
 *
 *   blind     the accepting 2xx reports ok with the code it received, 202
 *             from a conforming peer (RFC 3515 2.4.2), and the leg drains
 *             (enum call_xfer_kind). A later verdict ends the drain and
 *             reports nothing.
 *   attended  the 202 arms CALL_XFER_WAIT_MS. A NOTIFY whose sipfrag carries
 *             a 2xx silences the leg, frees its UI occupancy and reports ok
 *             with that 2xx; the leg then ends as the blind drain does. A
 *             sipfrag of 300 or above, a Subscription-State of terminated
 *             with no final sipfrag before it, or the guard expiry report
 *             failure with that status, or 408 on the expiry.
 *   onhook    as blind, and a failure BYEs the leg as well (the kind's
 *             whole difference, stated at enum call_xfer_kind).
 *   all       a non-2xx answer to the REFER itself, after the bye_txn-style
 *             dlg_auth_take retry on a 401 or a 407, reports failure with
 *             that code.
 *
 * A failure does not tear the leg down, the on-hook kind excepted. The
 * transfer failed, the call did not, and the FSM resumes the leg it held.
 *
 * A leg that dies first voids the pending outcome. An attended transfer
 * waits on a NOTIFY, and the transferee may BYE the leg the moment it
 * accepted the referral, which is before that NOTIFY: the leg then ends
 * through the ordinary far-end path, cb.remote_bye and cb.ended, and no
 * xfer_done follows at all. That is deliberate, and it is the same rule the
 * whole module keeps: one mechanism reports a far-end death, whatever
 * exchange was outstanding when it happened. The FSM consumes it in the XFER
 * row of the far-end clear during PENDING, where a dead slot_op enters
 * CLEARING(quiet).
 *
 * The synchronous refusal contract. false means no wire exchange happened and
 * no xfer_done will follow, so the caller owes the FSM the failure itself:
 * sip/line.c injects FSM_EV_CALL_XFER_FAIL inside the same frame, which is the
 * op_call_hold deferred-failure pattern, and PENDING{XFER} can never strand
 * on a refusal.
 *
 * The bool is sip/line.c's, not the machine's. The matching fsm_ops entries
 * (fsm.h) return void, and the only machine ops that answer are
 * feature_invoke and feature_invoke_arg. This bool is the answer this entry
 * gives its one caller,
 * sip/line.c's op wrapper, which turns it into the injected event above. The
 * two layers therefore carry one refusal signal between them and not two: read
 * this return as the wrapper's input, never as a value a cell acts on.
 *
 * The refusals are an unknown or UI-free slot, a leg whose
 * dialog is still early rather than confirmed, a leg that already carries an
 * outstanding REFER, an empty target, the consult kind, and a leg in any
 * other state that owns no confirmed dialog to send the REFER on. (The 491
 * and 486 rules govern an inbound REFER; a local second request is refused.)
 */
bool call_xfer(unsigned int line, unsigned int slot, enum call_xfer_kind kind,
	       const char *target);

/*
 * The on-hook consult transfer, reached by the *074 star code the subscriber
 * armed or by cfg xfer_on_hook.
 * The subscriber holds one party, calls a second, and goes on hook to join
 * them.
 *
 * The REFER goes out on the held leg, referee_slot, and hands it the active
 * leg: Refer-To is the remote target of replaced_slot with an embedded
 *
 *   ?Replaces=<replaced call_id>;to-tag=<replaced remote_tag>
 *             ;from-tag=<replaced local_tag>
 *
 * escaped by uri_header_escape(). The tags are oriented at the receiver
 * (RFC 3891 3): the target compares to-tag against its own local tag, so our
 * remote tag is the one that rides to-tag. Referred-By carries our identity.
 *
 * Fire and forget at the machine, because the subscriber is already on hook;
 * the call layer owns both legs from the 202 and ends them in the RFC 5589
 * 7.1 order:
 *
 *   202       both legs fall silent (media and session timers stopped, no UI
 *             name, the dialogs kept) and nothing is BYEd. A peer re-INVITE
 *             on either leg is answered 2xx with a sendonly or inactive
 *             answer and no stream, so a switch that re-INVITEs the replaced
 *             leg before it BYEs it (Asterisk) gets the 2xx it waits for.
 *   verdict   a NOTIFY whose sipfrag carries a 2xx ends the referee
 *             (referee_slot): it was referred away, and this side BYEs it
 *             one CALL_XFER_BYE_GRACE_MS later, or not at all when the
 *             network's own BYE arrives first. The replaced leg
 *             (replaced_slot) is NOT BYEd: the target the referred leg
 *             reached replaces it, and the network sends the Replaces-driven
 *             BYE on that dialog (RFC 5589), which this side answers 200. It
 *             drains until that BYE or the CALL_XFER_DRAIN_MS fallback. A
 *             sipfrag of 300 or above, or a subscription that ends with no
 *             sipfrag, BYEs both legs, because nothing replaces either.
 *   guard     CALL_XFER_CONSULT_WAIT_MS with no verdict BYEs the referee and
 *             leaves the replaced leg draining: a missing NOTIFY is no
 *             verdict, and a self-BYE there re-opens the race the drain
 *             closed.
 *   REFER     a non-2xx answer, or a REFER transaction that dies, BYEs both
 *             legs; nothing was silenced.
 *   peer BYE  a network BYE on either leg during the wait is answered 200
 *             without a report (a switch that ends both transferor legs
 *             itself arrives this way); a referee that ends
 *             so voids the outcome, and its record still closes.
 *
 * Both kept legs hold their ui_busy until the protocol ends, so admission
 * answers a new INVITE 486 and leg_alloc cannot cannibalize a leg that is
 * still draining. The referee slot frees when its BYE transaction ends. The
 * blind and on-hook kinds do the opposite (enum call_xfer_kind): their
 * drained leg holds no occupancy, a dial on its name lands on the free slot
 * first, and a new call that still needs its slot BYEs it on the way in.
 *
 * xfer_done never fires for this kind. The machine is idle by contract, and a
 * report that waited on the verdict could land after the replaced leg's slot
 * was handed to a new call under the referee's old UI name, where the IDLE
 * CALL_XFER_FAIL cell would hang that new call up. The outcome goes to the
 * trace and to the record (call_ended transferred).
 *
 * The machine fires and forgets, which costs both parties their call when the
 * transfer fails and costs no matrix cells; a machine that waited for the
 * verdict would need a state of its own, one cell per event. The wait of the
 * referee and the drain of the replaced leg are met here at the call layer
 * instead.
 *
 * Same synchronous refusal contract as call_xfer(), with one addition the
 * on-hook timing forces: false means nothing went on the wire and no callback
 * follows, and the two legs are still up with nobody left to clear them,
 * because the cell that asked for the transfer has already entered IDLE. The
 * cleanup is sip/line.c's, in the op wrapper: it call_hangup()s both slots on a
 * false return. This entry does not do it itself, so a caller that drives the
 * legs directly keeps whatever it built.
 */
bool call_xfer_consult(unsigned int line, unsigned int referee_slot,
		       unsigned int replaced_slot);

/*
 * The account interface of call.c. call_rx_request takes a request that
 * survived ua_request_policy() with its already-started server transaction;
 * call_rx_ack takes the first ACK for an accepted 2xx (UA_RX_ACK);
 * call_rx_stray takes a stray response the dialog layer passed and returns
 * true when a leg consumed it. The owner-routed env upcalls and the ua
 * callbacks arrive through the call_* forms below with the OWNER_CALL
 * value, that is, the call handle.
 */
void call_rx_request(unsigned int line, struct sip_txn *txn,
		     const struct sip_msg *req,
		     const struct sip_txn_dst *src);
void call_rx_ack(unsigned int line, const struct sip_msg *ack);
bool call_rx_stray(unsigned int line, const struct sip_msg *resp);
void call_txn_resp(uint32_t owner_val, uint32_t txn,
		   const struct sip_msg *msg);
void call_txn_done(uint32_t owner_val, uint32_t txn,
		   enum sip_txn_reason reason);
void call_ua_ack_missing(uint32_t owner_val);
void call_ua_glare_retry(uint32_t owner_val);

/*
 * A new INVITE that account.c refused before call_rx_request, counted in the
 * refused table of the line beside the refusals of this layer. The table
 * counts what the line turned away, whichever layer answered. env is the
 * environment of the account, for the age of the last refusal.
 */
void calls_refused_note(unsigned int line, struct sip_txn_env *env,
			uint32_t code);

struct blob_buf;

/**
 * call_codec_dump() - add the codec of the leg a UI slot names
 * @b: the blob buffer to add to
 * @line: the line of the leg
 * @ui_slot: the UI slot of the leg
 *
 * Adds the codec and ptime keys with the words of calls_status_dump(). A slot
 * no leg holds, or a leg without a committed session, adds nothing.
 */
void call_codec_dump(struct blob_buf *b, unsigned int line,
		     unsigned int ui_slot);

/* per line and per slot state for voice status and the e2e asserts */
void calls_status_dump(struct blob_buf *b, unsigned int line);

/*
 * -------------------------------------------- the account interface, continued
 *
 * Three more interface entries account.c needs: the owner of a server
 * transaction is fixed when the transaction starts, so account.c names the
 * slot it is about to fill; the CANCEL pair needs the tag the leg already
 * used in its provisionals plus a way to tell the leg its INVITE was
 * cancelled.
 */
uint32_t call_slot_handle(unsigned int line, unsigned int slot);
const char *call_txn_local_tag(unsigned int line, uint32_t txn);
void call_rx_cancel(unsigned int line, uint32_t invite_txn);

/* True while any leg on the line still holds protocol state; the reload
 * grace drain waits on it before it discards the account objects. */
bool calls_line_busy(unsigned int line);

/*
 * The idx-th distinct remote destination over the live legs of one line that
 * hold a dialog with a destination (sip_dialog_dst), NULL past the end. The
 * account's CRLF round pings them beside the resolved targets, because a
 * dialog keeps the proxy its INVITE went through (sip_dialog_dst_set) and a
 * DNS move can leave that proxy outside the target set while the call runs.
 * The pointer is the dialog's own and holds until the caller returns to the
 * loop; the index is a position of the moment, not a name.
 */
const struct sip_txn_dst *calls_line_dialog_dst(unsigned int line,
						unsigned int idx);

/*
 * The SIP final to clearing class map, exported so test_call drives it
 * without a daemon. The cfg to sdp_local derivation and the hal_media fill
 * are media_fill.h's.
 */
enum call_fail_class call_fail_map(uint32_t code);

/*
 * The BroadWorks call waiting INFO, exported under the same rule as the
 * helpers above so test_call drives the grammar without a daemon.
 * call_cw_ctype_match() takes a whole Content-Type value and compares the
 * media type case insensitively with any parameters tolerated;
 * call_cw_parse() reads the body through its length alone and never looks for
 * a terminator, so it is safe on a body that carries none.
 */
bool call_cw_ctype_match(struct sip_str ctype);
bool call_cw_parse(struct sip_str body, struct call_cw *out);

/*
 * The other in-dialog INFO body this module consumes, the out of band digit,
 * exported under the same rule as the call waiting pair above.
 * call_dtmf_relay_ctype_match() takes a whole Content-Type value and answers
 * true for application/dtmf-relay and for application/dtmf, with the media
 * type compared case insensitively and any parameters tolerated.
 *
 * call_dtmf_relay_parse() reads the body through its length alone. It takes
 * the content type too, because the two bodies differ: a dtmf-relay body
 * carries a Signal line and an optional Duration line, and a dtmf body
 * carries the bare digit. On true, digit holds one of 0 to 9, star, pound and
 * A to D, and duration_ms holds the announced length in milliseconds,
 * saturated at UINT16_MAX and 0 when the body announced none. On false both
 * outputs are 0.
 */
bool call_dtmf_relay_ctype_match(struct sip_str ctype);
bool call_dtmf_relay_parse(struct sip_str ctype, struct sip_str body,
			   char *digit, uint16_t *duration_ms);

/*
 * A raw byte sweep of one call slot, true when the pattern is found anywhere
 * in its storage. It is exported for one reason, under the same rule as the
 * helpers above: the key wipe of a released leg has no other independent
 * check at all,
 * because a released slot is reachable from nothing else this header offers.
 * test_call ends a leg and
 * then asserts that neither master key, the peer's key, nor the base64 text of
 * any of them is still there.
 *
 * It reads the slot as bytes and answers about bytes alone: it says nothing
 * about which field a hit came from, and it is not a debugging aid for anything
 * else. An unknown line or slot, an empty pattern and a pattern wider than the
 * slot all answer false.
 */
bool call_slot_scan(unsigned int line, unsigned int slot, const void *pat, size_t len);

#endif
