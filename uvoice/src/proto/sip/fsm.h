// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The per line FXS call control machine (src/proto/sip/fsm.c).
 *
 * Purity contract. fsm.c calls nothing: every side effect leaves through
 * struct fsm_ops, every timer rides a struct sip_timer on the injected
 * environment, and the only inputs are fsm_event_push() calls. That makes
 * the machine a function of (state, event, cfg) and lets test_fsm mirror
 * the state and event matrix of the machine cell for cell with the virtual
 * clock env from test_sip_txn.c: no HAL, no uloop, no socket. sip/line.c is
 * the impure shell that feeds it and executes its ops.
 *
 * The specification is that state and event matrix: test_fsm has a case
 * for each cell and a coverage table of all cells. Every transition and
 * every deliberate ignore in fsm.c must match a cell of the matrix. test_fsm
 * asserts the cell count, so the matrix and the code change together.
 *
 * Pending. One generic in-flight feature operation (hold, resume, swap,
 * answer-the-waiting-call, join the two calls into a conference, split that
 * conference again) with an on-success and an on-fail state. Depth is
 * exactly one: a flash during PENDING is swallowed, an on-hook tears down
 * both legs, and a far end BYE cancels the operation, so no outcome is
 * awaited on a dead leg.
 *
 * First event. Each backend reports the current hook state when the line
 * starts; udsp reports it after its first debounced hook sample. So SEIZE
 * can be the first event the machine sees, also right after daemon start
 * with the handset already lifted. IDLE handles it as a normal seize.
 */
#ifndef UVOICE_CALL_FSM_H
#define UVOICE_CALL_FSM_H

#include <stdbool.h>
#include <stdint.h>

#include "dialplan.h"
#include "proto/sip/features.h"
#include "hal/hal.h"
#include "proto/sip/txn.h"

#define FSM_DIGITS_MAX		DIALPLAN_DIGITS_MAX

/* no uci option exists */
#define FSM_CW_TIMEOUT_MS	30000
/* cw_offer_ms is engine time: the playout pauses for the whole type 2 CID
 * exchange (roughly a second) and the offer timer starts a tick apart from
 * the tone, so the offer needs wall-clock headroom past the alert total or
 * the last presentation is cut mid-gap. */
#define FSM_CW_SAS_MARGIN_MS	2000

/* the CLEARING(quiet) silence before the reorder chain */
#define FSM_QUIET_MS		2000

/* the reorder (fast busy) dwell before the howler; fixed, not a uci knob */
#define FSM_REORDER_MS		40000

/*
 * ------------------------------------------------------------------------
 * The REFER transfer.
 *
 * The transferor part of the machine. It is one state, two events, one
 * PENDING op, two vtable ops, one cfg field and three struct members. They
 * belong together, because the enums cannot grow on their own. Three checks
 * see a new state or a new event:
 *   - fsm.c asserts at build time that its handler table has one entry per
 *     state, counted up to FSM_SILENT, which must stay the last state;
 *   - test_fsm.c asserts at build time the state, event and cell counts;
 *   - test_fsm fails at run time on each cell that no case covers.
 * Each handler ignores an event that it has no arm for, so a new event
 * builds, and the coverage table of test_fsm finds its cells.
 *
 * 1. State FSM_XFER_COLLECT, the second-stage collection of a transfer
 *    target with a held leg. FSM_SPRE_COLLECT is the
 *    wrong state to reuse: its invariant is that the line owns no call, and
 *    every entry here owns one. It is the value after FSM_OOS, and its matrix
 *    row follows the OOS row.
 *
 * 2. Events FSM_EV_CALL_XFER_OK and FSM_EV_CALL_XFER_FAIL, immediately after
 *    FSM_EV_CALL_REINVITE_FAIL so the CALL_* group stays contiguous. sip/line.c
 *    derives both from cb.xfer_done (call.h): ok true is the OK event, ok
 *    false is the fail event, and the arg is the slot.
 *
 * 3. Op FSM_OP_XFER in enum fsm_pending_op. It amends the PENDING contract
 *    in three places, and in three places only:
 *      - the one-exchange rule names one outstanding exchange, not one
 *        re-INVITE, because the XFER op sends a REFER. The depth-one
 *        property it protects holds.
 *      - the rule that PENDING runs no FSM timer gains the exception that an
 *        attended transfer is bounded by
 *        call.c's CALL_XFER_WAIT_MS guard after the 202 and not by
 *        transaction timers, because the REFER transaction ends at the 202
 *        and the outcome arrives later, in a NOTIFY that may never come. The
 *        window before the 202 is the REFER transaction's own.
 *      - the on-hook rule stands unchanged: an on-hook in PENDING{XFER}
 *        tears down both legs and enters IDLE. The far end completes or
 *        fails the transfer on its own.
 *    The far-end clear during PENDING has one XFER row: a dead slot_op enters
 *    the CLEARING(quiet) state. That row is also where a transferee's early
 *    BYE lands, because call.h voids the pending xfer_done when the leg dies
 *    first.
 *
 * 4. The two ops below return void. feature_invoke and feature_invoke_arg
 *    are the only ops that return a value (the vtable statement further
 *    down), because their cells need the outcome inside the same transition
 *    and these two do not.
 *
 *    The single refusal signal is the injected event. The call.c entries
 *    behind these ops (call_xfer() and call_xfer_consult() in call.h) do
 *    answer bool, and that bool is consumed by sip/line.c's op wrapper, never
 *    by the machine: a false return makes the wrapper inject
 *    FSM_EV_CALL_XFER_FAIL inside the same frame, exactly as op_call_hold
 *    injects FSM_EV_CALL_REINVITE_FAIL for a refused hold. So PENDING{XFER}
 *    cannot strand on a refusal that made no wire exchange, and one refusal is
 *    never signalled twice.
 *
 *    The consult refusal also owes a cleanup, and it is the wrapper's, not
 *    the machine's. Its cell has already gone to IDLE, so no event can reach
 *    a machine that still owns the legs: sip/line.c call_hangup()s both slots
 *    on a false return from call_xfer_consult(). The event it injects is
 *    therefore inert by the slot guards, which is the same shape the accepted
 *    path has (call.h: xfer_done fires for the referee slot for the record
 *    alone).
 *
 * 5. The matrix: 19 states x 22 events = 418 cells. FSM_SILENT, the
 *    silent off hook state after the howler, is the value after
 *    FSM_XFER_COLLECT and the last row.
 * ------------------------------------------------------------------------
 */
enum fsm_state {
	FSM_IDLE = 0,
	FSM_OFFHOOK_DIAL,	/* dial tone, no digit yet */
	FSM_COLLECTING,		/* digits against the dial plan */
	FSM_SPRE_COLLECT,	/* star code second stage: the forward target */
	FSM_CALLING,		/* INVITE sent, nothing back yet */
	FSM_PROCEEDING,		/* 18x received */
	FSM_CONNECTED,
	FSM_RINGING,		/* inbound, phone rings */
	FSM_CW_OFFERED,		/* connected plus a waiting call */
	FSM_HOLD_DIAL,		/* held call plus recall dial tone */
	FSM_TWO_CALLS,		/* one active, one held */
	FSM_CONF,		/* local three-way conference */
	FSM_PENDING,		/* one feature op in flight */
	FSM_HOLDING,		/* on hook with a held call, recall ring */
	FSM_CLEARING,		/* busy/fastbusy/unobtainable/quiet chain */
	FSM_ROH,		/* howler for the ROH window, then FSM_SILENT */
	FSM_OOS,		/* line has no enabled channel */
	FSM_XFER_COLLECT,	/* transfer target, with the subject leg held */
	FSM_SILENT,		/* off hook, silent, after the howler window */
};

enum fsm_clearing {
	FSM_CLR_BUSY = 0,
	FSM_CLR_FASTBUSY,
	FSM_CLR_UNOBTAINABLE,
	FSM_CLR_QUIET,
};

/*
 * Which path presents a waiting call. FSM_CW_BOTH is 0, so a zeroed view
 * serves both paths. The mode does not move cfg.call_waiting: that field
 * stays the effective service enable the star codes toggle, and the query
 * code answers from it in every mode, because a network served call waiting
 * is enabled and merely served elsewhere.
 */
enum fsm_cw_mode {
	FSM_CW_BOTH = 0,
	FSM_CW_LOCAL,			/* the local offer of CONNECTED alone */
	FSM_CW_NETWORK,			/* the CW_NET alert alone */
};

/*
 * What a hook flash does to a running local three-way conference (the FLASH
 * cell of CONF). FSM_CONF_OFF is 0, so a zeroed view offers
 * no conference at all and the two-call flash keeps the swap. The two other
 * modes both end the conference. The split gives the subscriber the two calls
 * back, one active and one held. The drop hangs the second call up and keeps
 * the first.
 *
 * The mode also gates the join, so a line that offers no conference reaches
 * neither the state nor the mixer.
 */
enum fsm_conf_mode {
	FSM_CONF_OFF = 0,
	FSM_CONF_SPLIT,
	FSM_CONF_DROP,
};

enum fsm_pending_op {
	FSM_OP_HOLD = 0,
	FSM_OP_RESUME,
	FSM_OP_SWAP,
	FSM_OP_CW_ANSWER,
	FSM_OP_XFER,			/* a REFER, not a re-INVITE: item 3 above */
	FSM_OP_CONF,			/* the resume that joins the calls */
	FSM_OP_CONF_SPLIT,		/* the hold that ends the conference */
};

#define FSM_SLOT_NONE		255u

/*
 * One in-flight feature operation. on_success and
 * on_fail name plain states; when the matrix chains a second operation onto
 * a completion (the SWAP resume step, the record rewrites of a far-end clear
 * during PENDING), the
 * chain record below carries it and the matching chain_on_* flag replaces
 * that state. The matrix never chains deeper than one level, so this struct
 * is the whole expressive power PENDING needs, and depth stays one: a chain
 * is a sequence, not nesting.
 */
struct fsm_pending {
	enum fsm_pending_op op;
	enum fsm_state on_success;
	enum fsm_state on_fail;
	unsigned int slot_op;		/* the leg the re-INVITE belongs to */
	unsigned int slot_other;	/* the other leg, FSM_SLOT_NONE if none */
	bool chain_on_success;
	bool chain_on_fail;
	struct {
		enum fsm_pending_op op;
		unsigned int slot;
		enum fsm_state on_success;
		enum fsm_state on_fail;
	} chain;
};

/*
 * arg carries the call slot for the CALL_ events, the digit character for
 * DIGIT, the enum call_fail_class for CALL_FAILED, and the network call
 * waiting command for CW_NET: 0 stops the alert and 1 to 4 plays it with
 * that GR-506 pattern. The caller identity of a CW_NET play does not reach
 * this layer; sip/line.c stores it, and the core renders it.
 */
enum fsm_ev {
	FSM_EV_SEIZE = 0,
	FSM_EV_RELEASE,
	FSM_EV_FLASH,
	FSM_EV_DIGIT,
	FSM_EV_CALL_INCOMING,
	FSM_EV_CALL_PROCEEDING,		/* arg: (slot << 1) | early_media */
	FSM_EV_CALL_ANSWERED,
	FSM_EV_CALL_FAILED,		/* arg: (slot << 8) | fail class */
	FSM_EV_CALL_REMOTE_BYE,
	FSM_EV_CALL_REMOTE_CANCEL,
	FSM_EV_CALL_REINVITE_OK,
	FSM_EV_CALL_REINVITE_FAIL,
	/*
	 * The outcome of one REFER, arg the slot. PENDING with op XFER owns
	 * one, and IDLE consumes the failure of the on-hook blind transfer of
	 * CALLING and PROCEEDING by hanging the referred leg up; the matrix
	 * ignores it
	 * everywhere else.
	 */
	FSM_EV_CALL_XFER_OK,
	FSM_EV_CALL_XFER_FAIL,
	FSM_EV_CW_NET,			/* arg: 0 stop, 1-4 play that pattern */
	FSM_EV_T_START_DIGIT,
	FSM_EV_T_INTERDIGIT,
	FSM_EV_T_WARMLINE,
	FSM_EV_T_CW,
	FSM_EV_T_CLEARING,
	FSM_EV_T_ROH,
	/*
	 * The ring-no-answer window of local call forwarding. RINGING arms it
	 * on its entry and only when the no-answer condition is armed with a
	 * target, so a line without the service keeps ringing forever. It
	 * rides t_cw, which RINGING
	 * never uses for the call waiting budget.
	 */
	FSM_EV_T_RING_NOANSWER,
	FSM_EV_COUNT,
};

const char *fsm_state_name(enum fsm_state state);
const char *fsm_ev_name(enum fsm_ev ev);

/*
 * The action vtable sip/line.c implements. These are UI intents: the FSM never
 * sees SIP except the reject codes, which are UI policy (486 busy line,
 * 480 unanswered waiting call). tone(HAL_TONE_NONE) stops the tone.
 *
 * feature_invoke and feature_invoke_arg are the two ops that answer. The star
 * code cells of OFFHOOK_DIAL and COLLECTING need the outcome inside the same
 * transition, because the cell plays CONFIRM or enters CLEARING(fastbusy) by
 * that outcome, and the shell applies the toggle synchronously with no wire
 * exchange to wait for. No other op returns a value.
 *
 * The nullable state_changed op is a notification. It emits no action and
 * changes no matrix cell. fsm_event_push() calls it after all three settle
 * calls when the state moves. fsm_oos_force() and fsm_oos_clear() call it
 * when they move the state.
 */
struct fsm_ops {
	void (*call_dial)(void *priv, unsigned int slot, const char *number);
	void (*call_ring)(void *priv, unsigned int slot);
	void (*call_answer)(void *priv, unsigned int slot);
	void (*call_reject)(void *priv, unsigned int slot, uint32_t code);
	void (*call_hangup)(void *priv, unsigned int slot);
	void (*call_hold)(void *priv, unsigned int slot);
	void (*call_resume)(void *priv, unsigned int slot);
	void (*call_dtmf)(void *priv, char digit, uint16_t duration_ms);
	void (*tone)(void *priv, enum hal_tone tone);
	void (*ring_start)(void *priv, unsigned int slot);
	void (*ring_stop)(void *priv);
	void (*cw_alert)(void *priv, unsigned int slot, bool on);
	/*
	 * The network form of the same alert. It carries no slot: the
	 * switch owns the waiting call, so there is no local leg to name, and
	 * the caller identity the alert presents is the one sip/line.c stored
	 * from the INFO.
	 */
	void (*cw_net_alert)(void *priv, bool on);
	/*
	 * The hook flash of a connected leg, handed to the far end as an
	 * in-dialog INFO instead of to the local hold path. It carries no slot
	 * either: the relay always names the one connected leg, and sip/line.c
	 * resolves it.
	 */
	void (*flash_relay)(void *priv);
	bool (*feature_invoke)(void *priv, const char *code);
	/*
	 * The second stage of a star code that carries a target: the code and
	 * the digits the subscriber dialled behind it, with the terminating '#'
	 * already consumed. The argument can be empty, which the shell reads as
	 * "use the configured target". It answers like feature_invoke, and for
	 * the same reason: the cell plays CONFIRM or enters CLEARING(fastbusy)
	 * on the answer.
	 */
	bool (*feature_invoke_arg)(void *priv, const char *code, const char *arg);
	/*
	 * Local call forwarding: hand the offered call to target instead of
	 * ringing or refusing it. The leg answers 302 with the target in its
	 * Contact and ends as a refusal the FSM asked for, so no further event
	 * is owed for that slot. Like every other op it names the slot alone:
	 * the shell resolves the line from priv.
	 */
	void (*call_forward)(void *priv, unsigned int slot, const char *target);
	/*
	 * Refer the leg in slot to target. The kind rides enum feature_action,
	 * which is the type this header already has: features.h is included
	 * above for the star code table, and the two values the op accepts are
	 * FEATURE_ACT_XFER_BLIND and FEATURE_ACT_XFER_ATTENDED, that is, the
	 * very row that matched the dialled code. sip/line.c maps them to enum
	 * call_xfer_kind (call.h), which this header must not include.
	 *
	 * The FSM has already held the leg through the PENDING pattern before
	 * it asks, and the outcome arrives as FSM_EV_CALL_XFER_OK or
	 * FSM_EV_CALL_XFER_FAIL for that slot.
	 */
	void (*call_xfer)(void *priv, unsigned int slot, enum feature_action act,
			  const char *target);
	/*
	 * The on-hook consult transfer: refer the held leg (referee) to the
	 * active one (replaced) with an embedded Replaces. Fire and forget, and
	 * the cell that runs it has already entered IDLE, so the refusal
	 * cleanup belongs to the shell (item 4 of the block above).
	 */
	void (*call_xfer_consult)(void *priv, unsigned int referee,
				  unsigned int replaced);
	/*
	 * The mixer of the local three-way conference. It carries no slot: the
	 * conference is the whole line, so the shell mixes the two legs it
	 * owns and the phone. The machine turns it on when the join completes
	 * and off on every path that ends the conference.
	 *
	 * A shell that leaves it NULL offers no conference: the eligibility
	 * guard of the TWO_CALLS flash tests the op, so the two-call flash
	 * keeps the swap.
	 */
	void (*conf_set)(void *priv, bool on);
	void (*state_changed)(void *priv, enum fsm_state before,
			      enum fsm_state after);
};

/*
 * The cfg view the machine reads, value-copied by sip/line.c so a reload never
 * moves a machine that is mid call; sip/line.c applies a new view at IDLE/OOS
 * only. dp points at the line's compiled dialplan, which sip/line.c owns and
 * replaces under the same rule.
 */
struct fsm_cfg {
	const struct dialplan *dp;
	/*
	 * The star code hooks, injected exactly like dp: features points at
	 * the line's runtime feature state, feature_codes at its loaded channel
	 * config, and both belong to sip/line.c. Both NULL means the line has
	 * no feature table at all: no star code, no do-not-disturb filter and
	 * no forward, which is the plain digit path. A view that carries them
	 * requires ops->feature_invoke, ops->feature_invoke_arg and
	 * ops->call_forward, which the vtable rule of the matrix conventions
	 * does not check: a half wired shell must crash at bring-up.
	 */
	const struct feature_state *features;
	const struct cfg_channel *feature_codes;
	uint32_t start_digit_ms;
	uint32_t interdigit_short_ms;
	uint32_t interdigit_long_ms;
	uint32_t busy_tone_ms;
	uint32_t roh_ms;
	uint32_t warmline_ms;
	bool call_waiting;
	/*
	 * The effective message waiting indication: the line holds a message
	 * and uci amwi asks for the audible form. sip/line.c derives it from
	 * both and the machine only chooses a tone with it (OFFHOOK_DIAL). It
	 * is not a config value, so fsm_mwi_set() below moves it outside a rest
	 * state.
	 */
	bool mwi;
	/* the alert source: read by the CALL_INCOMING admission of CONNECTED
	 * and by the CW_NET play gate, and by nothing else */
	enum fsm_cw_mode cw_mode;
	bool flash_features;		/* the region's flash service is not off:
					   gates the CONNECTED flash-hold path */
	bool flash_relay;		/* CONNECTED flash goes to the wire,
					   not to the hold path:
					   local_conf_enabled 0 with
					   sip_flash_relay on */
	bool warmline;
	/*
	 * The on-hook consult transfer of TWO_CALLS without a star code,
	 * derived by sip/line.c from local_conf_enabled == 2 and the region's
	 * drop flash service (the read-back's flash_service). The third
	 * condition, the held leg being the incoming one, is a cell guard and
	 * not a config value: the machine reads it from slot_inbound[] below.
	 */
	bool xfer_on_hook;
	/*
	 * Whether the flash of TWO_CALLS offers a conference, and what the
	 * flash of CONF then does to it. The shell fills it from the line's
	 * conference config; a view that leaves it FSM_CONF_OFF offers no
	 * conference and keeps the swap.
	 */
	enum fsm_conf_mode conf_mode;
	char warmline_number[FSM_DIGITS_MAX + 1];
	/*
	 * The region's whole call-waiting subscriber alerting signal in
	 * milliseconds, the backend read-back's cw_sas_total_ms. The offer
	 * window is max(FSM_CW_TIMEOUT_MS, this plus FSM_CW_SAS_MARGIN_MS),
	 * so a region that presents the alert for 50 s is not cut off at 30.
	 * 0 keeps the constant, which is what a view with no read-back
	 * carries.
	 */
	uint32_t cw_offer_ms;
};

struct fsm {
	unsigned int line;
	enum fsm_state state;
	enum fsm_clearing clearing;
	struct fsm_pending pending;
	unsigned int active_slot;
	unsigned int other_slot;
	bool other_held;
	bool cw_net;			/* a network call waiting alert runs */
	char digits[FSM_DIGITS_MAX + 1];
	unsigned int ndigits;
	/*
	 * How much of the digit buffer is the star code itself while the second
	 * stage collects. The rest of the buffer is the target, so one buffer
	 * carries both and the split is a length, not a copy.
	 */
	unsigned int spre_code_len;
	/*
	 * The consult star code armed a transfer for the next on-hook (set by
	 * the XFER_COLLECT DIGIT cell, consumed by the TWO_CALLS RELEASE cell).
	 * It has no disarm gesture: the flash that ends the consult call
	 * cancels it by leaving the states that keep it.
	 *
	 * Two points clear it, and there is no third: the settle step at the
	 * end of fsm_event_push, which covers every transition an event makes,
	 * and fsm_oos_force(), which is the one transition that is not an event
	 * and therefore runs no settle of its own.
	 */
	bool xfer_armed;
	/* which of the three transfer rows is being collected or armed */
	enum feature_action xfer_act;
	/*
	 * Whether the call in that slot came in. Two cells read it, and no
	 * other layer keeps it per slot: the on-hook consult transfer of
	 * TWO_CALLS needs the direction rule (the held leg incoming, the active
	 * leg outgoing), and the conference eligibility of the same state needs
	 * the direction of the second call. It is written where a leg is
	 * acquired, that is, at the CALL_INCOMING acceptances of IDLE,
	 * CONNECTED and HOLDING and at every call_dial, so a role swap moves it
	 * with the slot and never with the role. fsm_slot_inbound_set() below
	 * states the direction of a call the shell put into a slot the machine
	 * already owns.
	 */
	bool slot_inbound[2];
	/*
	 * The call the line acquired while it already owned another one, which
	 * is what makes the conference eligibility of TWO_CALLS order based.
	 * It survives a swap, because a swap moves the
	 * roles and not the slots. FSM_SLOT_NONE means the line owns no second
	 * call.
	 */
	unsigned int slot_second;
	bool conf_on;			/* the mixer runs */
	/*
	 * The mixer refused a conference this line already joined, so the next
	 * flash of TWO_CALLS swaps instead of joining again. It clears where
	 * slot_second clears, because both describe one pair of calls.
	 */
	bool conf_refused;
	/*
	 * The bell this machine asked for is not sounding, because a metallic
	 * line test refused it (fsm_ring_held below). No ring-no-answer window
	 * is armed while it is set. The shell answers from inside the RINGING
	 * entry, before that entry arms anything, so a flag and not a timer
	 * cancel is what both orders have in common.
	 */
	bool ring_held;
	/*
	 * The account of the line can carry a dial (fsm_dial_ready_set below).
	 * The IDLE SEIZE cell and the CLEARING FLASH cell read it: a line with
	 * nowhere to send a call gets the fast busy chain at the seize instead
	 * of a dial tone whose every dial would be refused, and a flash over
	 * the clearing chain does not open one either.
	 */
	bool dial_ready;
	/*
	 * The far end refused the resume of the held leg (the PENDING
	 * REINVITE_FAIL cell). An on-hook in the clearing chain then hangs that
	 * leg up instead of ringing it back, because the pickup would only draw
	 * the same refusal (the CLEARING RELEASE cell). held_set() clears it,
	 * so it never outlives the leg it describes.
	 */
	bool held_refused;
	struct sip_timer t_digit;	/* start digit, interdigit, warmline */
	struct sip_timer t_cw;		/* call waiting budget, ring no answer */
	struct sip_timer t_clearing;	/* clearing chain and ROH */
	struct sip_txn_env *env;
	struct fsm_cfg cfg;
	const struct fsm_ops *ops;
	void *priv;
};

void fsm_init(struct fsm *f, unsigned int line, struct sip_txn_env *env,
	      const struct fsm_cfg *cfg, const struct fsm_ops *ops,
	      void *priv);
void fsm_event_push(struct fsm *f, enum fsm_ev ev, uint32_t arg);

/*
 * OOS is entered and left by sip/line.c, never by an event. The force runs
 * the OOS entry work through the vtable (hang up every owned leg, tone and
 * ring off, timers cancelled, roles cleared), so the shell never has to reach
 * into the struct. The clear returns to IDLE and runs its entry actions.
 */
void fsm_oos_force(struct fsm *f);
void fsm_oos_clear(struct fsm *f);

/*
 * Cancel every timer this machine owns, without moving it. sip/line.c calls it
 * on the lifecycle edges where the storage under a machine goes away: an armed
 * sip_timer stays linked into the environment's list, that list is threaded
 * through the timer inside this struct, and re-initialising or freeing a
 * machine that still holds one corrupts it. This is not a transition: it emits
 * no action and changes no state, and fsm_oos_force() remains the way to stop a
 * machine that keeps running.
 */
void fsm_timers_stop(struct fsm *f);

/* refused (false) unless the machine is in IDLE or OOS */
bool fsm_cfg_update(struct fsm *f, const struct fsm_cfg *cfg);

/*
 * The one cfg field a star code moves while the line is off hook.
 * The rest of the view is config and lands at a rest state, but call waiting
 * is a runtime toggle: a subscriber who dials the disable code and then dials
 * a number, without going on hook in between, must not get a waiting offer on
 * that call. This changes no state and emits no action.
 */
void fsm_call_waiting_set(struct fsm *f, bool on);

/*
 * The message waiting indication, moved by the switch and not by the
 * subscriber. A NOTIFY can arrive at any moment, including
 * while the line is off hook, and the next dial tone must already know about
 * it, so it bypasses the at-rest view exactly as call waiting does. This
 * changes no state and emits no action.
 */
void fsm_mwi_set(struct fsm *f, bool on);

/*
 * Whether the account of the line can carry a dial, moved by the account and
 * not by the subscriber. The shell sets it in front of every SEIZE and every
 * FLASH it delivers, from the same test the dial refusal makes (call.h
 * calls_line_dial_ready), so the IDLE SEIZE cell and the CLEARING FLASH cell
 * each read the account as it is at that event. fsm_init() sets it true. This
 * changes no state and emits no action.
 */
void fsm_dial_ready_set(struct fsm *f, bool ready);

/*
 * The per slot direction, stated from outside. A slot changes
 * hands without a CALL_INCOMING and without a call_dial when the shell puts
 * another call into it. The two cells that read the direction must then see
 * the direction of the new call. This changes no state and emits no action.
 */
void fsm_slot_inbound_set(struct fsm *f, unsigned int slot, bool inbound);

/*
 * The two halves of a bell a metallic line test held back (sip/line.c). The
 * ring-no-answer window measures how long a subscriber left a ringing phone
 * alone, so it must not run while that phone is silent: a window that did
 * would forward a call nobody could answer, and a hold longer than the window
 * would forward it before it ever rang.
 *
 * fsm_ring_held() says the shell asked for a bell and did not get one. It
 * stops the window and keeps the next entry from arming one. fsm_ring_started()
 * says the bell is sounding now, and starts the window from this moment. A
 * line that rang on the first attempt calls neither. Both change no state and
 * emit no action.
 */
void fsm_ring_held(struct fsm *f);
void fsm_ring_started(struct fsm *f);

/*
 * The backend will not ring this line for this call at all (sip/line.c). The
 * 180 is already on the wire, so the caller hears ringback for a telephone that
 * stays quiet. This refuses that caller 480 and ends the alerting leg, which
 * leaves the line idle, or holding when a held leg stays.
 *
 * The code is the one of a call the subscriber did not take (RFC 3261 section
 * 21.4.18), so a session border controller applies voicemail or forward on no
 * answer. The local busy forward does not run: reject_send() redirects 486
 * alone. A machine that is not alerting clears the held flag and emits
 * nothing.
 */
void fsm_ring_failed(struct fsm *f);

#endif
