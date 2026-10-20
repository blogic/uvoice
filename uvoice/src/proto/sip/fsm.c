// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The per line FXS call control machine.
 *
 * The specification is the state and event matrix that test_fsm checks: 19
 * states by 22 events. Each handler other than st_outbound() covers one
 * state; st_outbound() covers CALLING and PROCEEDING. A switch arm
 * implements each cell that acts in at least one branch, and the default arm
 * ignores every other event. test_fsm mirrors the matrix and checks each of
 * the 418 cells.
 *
 * Entry action rule. A cell emits its own action list
 * in the listed order, then the entry actions of the target state that the
 * cell did not already emit with the same argument. The ENT_ flags below
 * carry that "already emitted" information from a cell to the entry helper
 * of its target state. A transition whose target is the current state runs
 * no entry actions.
 *
 * Timer objects. struct fsm carries three struct sip_timer members and the
 * matrix arms at most one event per member per state, so each member derives
 * its event from the state it fires in (digit_timer_ev and friends). A
 * cancel reaches the environment only when the timer is armed, so a cell
 * that cancels a timer the state never armed is silent.
 *
 * Purity. No allocation, no uloop, no HAL, no logging. Side effects leave
 * through struct fsm_ops and through the injected timer environment only.
 */
#include <string.h>

#include "proto/sip/call.h"
#include "proto/sip/fsm.h"

#define ENT_TONE	(1u << 0)
#define ENT_BELL	(1u << 1)
#define ENT_TIMER	(1u << 2)

#define FSM_REJECT_BUSY		486u	/* line busy for a new offer */
#define FSM_REJECT_NOANSWER	480u	/* waiting call the user never took */

static const char *const state_names[] = {
	"IDLE", "OFFHOOK_DIAL", "COLLECTING", "SPRE_COLLECT", "CALLING",
	"PROCEEDING", "CONNECTED", "RINGING", "CW_OFFERED", "HOLD_DIAL",
	"TWO_CALLS", "CONF", "PENDING", "HOLDING", "CLEARING", "ROH", "OOS",
	"XFER_COLLECT", "SILENT",
};

static const char *const ev_names[] = {
	"SEIZE", "RELEASE", "FLASH", "DIGIT", "CALL_INCOMING",
	"CALL_PROCEEDING", "CALL_ANSWERED", "CALL_FAILED", "CALL_REMOTE_BYE",
	"CALL_REMOTE_CANCEL", "CALL_REINVITE_OK", "CALL_REINVITE_FAIL",
	"CALL_XFER_OK", "CALL_XFER_FAIL",
	"CW_NET", "T_START_DIGIT", "T_INTERDIGIT", "T_WARMLINE", "T_CW",
	"T_CLEARING", "T_ROH", "T_RING_NOANSWER",
};

const char *fsm_state_name(enum fsm_state state)
{
	if ((unsigned int)state >= sizeof(state_names) / sizeof(state_names[0]))
		return "?";

	return state_names[state];
}

const char *fsm_ev_name(enum fsm_ev ev)
{
	if ((unsigned int)ev >= sizeof(ev_names) / sizeof(ev_names[0]))
		return "?";

	return ev_names[ev];
}

/* ---------------------------------------------------------------- actions */

static void tone_set(struct fsm *f, enum hal_tone tone)
{
	f->ops->tone(f->priv, tone);
}

/* the shell answers fsm_ring_held() from inside this op when a line test
 * refuses the ring, so the verdict of the newest request is the one that
 * stands and neither wrapper may leave the older one behind */
static void bell_start(struct fsm *f, unsigned int slot)
{
	f->ring_held = false;
	f->ops->ring_start(f->priv, slot);
}

static void bell_stop(struct fsm *f)
{
	f->ring_held = false;
	f->ops->ring_stop(f->priv);
}

static void ring180_send(struct fsm *f, unsigned int slot)
{
	f->ops->call_ring(f->priv, slot);
}

static void answer_send(struct fsm *f, unsigned int slot)
{
	f->ops->call_answer(f->priv, slot);
}

static void forward_send(struct fsm *f, unsigned int slot, const char *target)
{
	f->ops->call_forward(f->priv, slot, target);
}

/*
 * The armed target of one forward condition, or NULL when the condition is
 * off, when the line carries no feature table at all, or when the condition is
 * armed with no target: a config that sets call_fwd_local_mode without the
 * matching uri arms a condition this machine cannot act on, so the offer falls
 * through to the treatment it would have had. sip/line.c warns about that
 * config once per reload, because the machine cannot trace.
 */
static const char *cf_target_of(const struct fsm *f, enum feature_cf kind)
{
	const char *target;

	if (!f->cfg.features || !f->cfg.feature_codes)
		return NULL;
	if (!feature_cf_armed(f->cfg.features, kind, &target) || !*target)
		return NULL;

	return target;
}

static bool dnd_is(const struct fsm *f)
{
	return f->cfg.features && feature_dnd_is(f->cfg.features);
}

/*
 * Call forward on busy lives here, in the one helper every 486 of the matrix
 * goes through. A line that refuses an offer as busy, for any reason this
 * machine has, redirects it instead when the busy condition is armed. That
 * includes the do-not-disturb refusal, so the DND-to-CFB interception costs
 * no cell of its own.
 *
 * The 480 of an unanswered waiting call is not redirected: that call was
 * offered and not taken, which is not the busy condition.
 */
static void reject_send(struct fsm *f, unsigned int slot, uint32_t code)
{
	const char *target = code == FSM_REJECT_BUSY ?
			     cf_target_of(f, FEATURE_CF_BUSY) : NULL;

	if (target) {
		forward_send(f, slot, target);
		return;
	}

	f->ops->call_reject(f->priv, slot, code);
}

static void hangup_send(struct fsm *f, unsigned int slot)
{
	f->ops->call_hangup(f->priv, slot);
}

static void hold_send(struct fsm *f, unsigned int slot)
{
	f->ops->call_hold(f->priv, slot);
}

static void resume_send(struct fsm *f, unsigned int slot)
{
	f->ops->call_resume(f->priv, slot);
}

/* Zero is the sentinel that the line shell replaces with the measured duration. */
static void dtmf_send(struct fsm *f, char digit)
{
	f->ops->call_dtmf(f->priv, digit, 0);
}

static void cw_alert_set(struct fsm *f, unsigned int slot, bool on)
{
	f->ops->cw_alert(f->priv, slot, on);
}

static void cw_net_alert_set(struct fsm *f, bool on)
{
	f->ops->cw_net_alert(f->priv, on);
}

static void flash_relay_send(struct fsm *f)
{
	f->ops->flash_relay(f->priv);
}

static void xfer_send(struct fsm *f, unsigned int slot, enum feature_action act,
		      const char *target)
{
	f->ops->call_xfer(f->priv, slot, act, target);
}

static void xfer_consult_send(struct fsm *f, unsigned int referee, unsigned int replaced)
{
	f->ops->call_xfer_consult(f->priv, referee, replaced);
}

static void timer_arm(struct fsm *f, struct sip_timer *t, uint32_t ms)
{
	sip_timer_arm(f->env, t, ms);
}

static void timer_stop(struct fsm *f, struct sip_timer *t)
{
	sip_timer_cancel(f->env, t);
}

static void timers_stop_all(struct fsm *f)
{
	timer_stop(f, &f->t_digit);
	timer_stop(f, &f->t_cw);
	timer_stop(f, &f->t_clearing);
}

/* ------------------------------------------------------------ slots, digits */

static bool slot_owned(const struct fsm *f, unsigned int slot)
{
	return slot != FSM_SLOT_NONE &&
	       (slot == f->active_slot || slot == f->other_slot);
}

static bool held_has(const struct fsm *f)
{
	return f->other_held && f->other_slot != FSM_SLOT_NONE;
}

static bool held_is(const struct fsm *f, unsigned int slot)
{
	return held_has(f) && f->other_slot == slot;
}

static void held_set(struct fsm *f, unsigned int slot)
{
	f->other_slot = slot;
	f->other_held = slot != FSM_SLOT_NONE;
	f->held_refused = false;
}

static void slot_release(struct fsm *f, unsigned int slot)
{
	if (slot == FSM_SLOT_NONE)
		return;
	if (f->active_slot == slot)
		f->active_slot = FSM_SLOT_NONE;
	if (f->other_slot != slot)
		return;

	f->other_slot = FSM_SLOT_NONE;
	f->other_held = false;
}

static bool slot_free_has(const struct fsm *f)
{
	unsigned int used = 0;

	if (f->active_slot < CALL_SLOTS)
		used++;
	if (f->other_slot < CALL_SLOTS)
		used++;

	return used < CALL_SLOTS;
}

static unsigned int slot_free_get(const struct fsm *f)
{
	if (f->active_slot == 0 || f->other_slot == 0)
		return 1;

	return 0;
}

/*
 * The per slot direction. Two cells read it, and nothing else on this line
 * remembers which call went out and which came in: the on-hook consult
 * transfer of TWO_CALLS, which fires when the held leg came in and the active
 * one went out, and the conference eligibility of the same
 * section, which joins calls this line originated. FSM_SLOT_NONE indexes
 * nothing, so both entries take the slot guard.
 */
static void slot_inbound_set(struct fsm *f, unsigned int slot, bool inbound)
{
	if (slot >= sizeof(f->slot_inbound) / sizeof(f->slot_inbound[0]))
		return;

	f->slot_inbound[slot] = inbound;
}

static bool slot_inbound_is(const struct fsm *f, unsigned int slot)
{
	if (slot >= sizeof(f->slot_inbound) / sizeof(f->slot_inbound[0]))
		return false;

	return f->slot_inbound[slot];
}

/*
 * The line forgets which call it took second, and any mixer refusal recorded
 * against that pair. Both describe one pair of calls,
 * so the entries that end a pair drop them together.
 */
static void slot_second_clear(struct fsm *f)
{
	f->slot_second = FSM_SLOT_NONE;
	f->conf_refused = false;
}

static void legs_hangup(struct fsm *f)
{
	if (f->active_slot != FSM_SLOT_NONE)
		hangup_send(f, f->active_slot);
	if (f->other_slot != FSM_SLOT_NONE)
		hangup_send(f, f->other_slot);
}

static void digits_clear(struct fsm *f)
{
	f->ndigits = 0;
	f->digits[0] = 0;
}

/* A full buffer keeps its content: the matcher verdict then repeats and the
 * collector simply waits, which beats dropping the number the user dialed. */
static void digits_append(struct fsm *f, char digit)
{
	if (f->ndigits >= FSM_DIGITS_MAX)
		return;

	f->digits[f->ndigits++] = digit;
	f->digits[f->ndigits] = 0;
}

static void pending_clear(struct fsm *f)
{
	memset(&f->pending, 0, sizeof(f->pending));
	f->pending.slot_op = FSM_SLOT_NONE;
	f->pending.slot_other = FSM_SLOT_NONE;
}

/* --------------------------------------------------------------- dial plan */

static enum dp_action digits_verdict(const struct fsm *f)
{
	struct dp_match m;

	dialplan_match(f->cfg.dp, f->digits, f->ndigits, &m);

	return dp_action_of(&m);
}

/*
 * An interdigit expiry under the collector contract of dialplan.h. The buffer
 * cannot change while the timer runs (a digit cancels it), so this rescan
 * returns the verdict that armed the timer, and no shadow state is needed. A
 * verdict that expects a timer takes it as a 'T' symbol and answers the
 * verdict of the longer buffer. Otherwise the expiry keeps its own meaning:
 * DIAL (a live map reload while the timer runs) and WAIT_SHORT dial, the rest
 * reorders. A full buffer has no room for the 'T' and keeps that meaning too.
 */
static enum dp_action digits_expire_verdict(struct fsm *f)
{
	struct dp_match m;
	enum dp_action act;

	dialplan_match(f->cfg.dp, f->digits, f->ndigits, &m);
	act = dp_action_of(&m);
	if (act == DP_ACT_DIAL)
		return DP_ACT_DIAL;

	if ((m.complete_on_timer || m.timer_next) &&
	    f->ndigits < FSM_DIGITS_MAX) {
		digits_append(f, 'T');
		return digits_verdict(f);
	}

	return act == DP_ACT_WAIT_SHORT ? DP_ACT_DIAL : DP_ACT_REORDER;
}

static bool digits_number_empty(const struct fsm *f)
{
	char number[FSM_DIGITS_MAX + 1];

	return dialplan_digits_strip(f->digits, number, sizeof(number)) == 0;
}

/*
 * An alternative that starts with a timer ("T", "TT") completes on a buffer
 * that holds no digit. The number to dial is then empty, so the expiry is the
 * reorder of the collector, as dialplan.h states.
 */
static enum dp_action digits_expire(struct fsm *f)
{
	enum dp_action act = digits_expire_verdict(f);

	if (act == DP_ACT_DIAL && digits_number_empty(f))
		return DP_ACT_REORDER;

	return act;
}

/* the window a waiting verdict asks for; false for DIAL and REORDER */
static bool digits_wait_arm(struct fsm *f, enum dp_action act)
{
	switch (act) {
	case DP_ACT_WAIT_SHORT:
		timer_arm(f, &f->t_digit, f->cfg.interdigit_short_ms);
		return true;
	case DP_ACT_WAIT_LONG:
		timer_arm(f, &f->t_digit, f->cfg.interdigit_long_ms);
		return true;
	default:
		return false;
	}
}

/*
 * The buffer keeps the dialed number through CALLING and PROCEEDING (4.5 and
 * 4.6 refer it), so the timer markers leave the buffer itself.
 */
static void digits_markers_strip(struct fsm *f)
{
	char number[FSM_DIGITS_MAX + 1];

	f->ndigits = (unsigned int)dialplan_digits_strip(f->digits, number,
							 sizeof(number));
	memcpy(f->digits, number, f->ndigits + 1);
}

static void dial_start(struct fsm *f, const char *number)
{
	unsigned int slot = slot_free_get(f);

	/* a line that already owns a call takes this one second */
	if (f->other_slot != FSM_SLOT_NONE)
		f->slot_second = slot;

	f->active_slot = slot;
	slot_inbound_set(f, slot, false);
	f->ops->call_dial(f->priv, slot, number);
}

/* ------------------------------------------------------------ star codes */

/*
 * The feature table of OFFHOOK_DIAL and COLLECTING. It is consulted only on
 * a buffer that starts with '*' or '#', and before the dial plan, so a code
 * that carries its own '#' consumes it.
 *
 * HOLD_DIAL consults it too, and for the three transfer rows alone (its
 * DIGIT cell). Those three go through the two pure lookups below rather than
 * through feature_match, because the cell has to tell a transfer row from any
 * other match and has to know a buffer on its way to one. Every other code is
 * not legal against a held call and falls through to the dial plan and the
 * network.
 */
static bool star_first(const struct fsm *f, char digit)
{
	char first = f->ndigits ? f->digits[0] : digit;

	return first == '*' || first == '#';
}

/* the buffer plus one more digit; false when the buffer cannot take one */
static bool digits_cand(const struct fsm *f, char digit, char *cand)
{
	if (f->ndigits >= FSM_DIGITS_MAX)
		return false;

	memcpy(cand, f->digits, f->ndigits);
	cand[f->ndigits] = digit;
	cand[f->ndigits + 1] = 0;

	return true;
}

/*
 * The dial plan's own answer to a dialed '#'. The '#' is part of the number
 * when the map still admits it. It is the delimiter when appending it kills
 * every alternative: nothing longer can follow, and the number is the buffer
 * in front of it. A full buffer reads the same way, because nothing can
 * extend it either. digit_esc_enabled is not consulted; it selects the URI
 * escaping in call.c and the delimiter is always active. A '#' behind no
 * digit, an empty buffer or 'T' markers alone, ends no number: the collector
 * takes it as a digit, and a map that refuses it gives the impossible match.
 */
static bool digit_pound_delim(const struct fsm *f, char digit)
{
	char cand[FSM_DIGITS_MAX + 2];
	struct dp_match m;

	if (digit != '#' || digits_number_empty(f))
		return false;
	if (!digits_cand(f, digit, cand))
		return true;

	dialplan_match(f->cfg.dp, cand, f->ndigits + 1, &m);

	return dp_action_of(&m) == DP_ACT_REORDER;
}

static enum feature_verdict feature_probe(const struct fsm *f, char digit)
{
	char cand[FSM_DIGITS_MAX + 2];

	if (!f->cfg.features || !f->cfg.feature_codes)
		return FEATURE_NONE;
	if (!star_first(f, digit) || !digits_cand(f, digit, cand))
		return FEATURE_NONE;

	return feature_match(f->cfg.features, f->cfg.feature_codes, cand);
}

/*
 * The three transfer rows are the only star codes a held call admits (the
 * HOLD_DIAL DIGIT cell). They are also the only rows that must never open
 * the second stage of SPRE_COLLECT: a transfer code with no held call
 * refuses at once. features.c refuses to execute them,
 * because their effect is the call op this machine runs.
 */
static bool xfer_action_is(enum feature_action act)
{
	return act == FEATURE_ACT_XFER_BLIND || act == FEATURE_ACT_XFER_ATTENDED ||
	       act == FEATURE_ACT_XFER_CONSULT;
}

static enum feature_action xfer_code_of(const struct fsm *f, const char *buf)
{
	enum feature_action act;

	if (!f->cfg.feature_codes)
		return FEATURE_ACT_NONE;

	act = feature_action_get(f->cfg.feature_codes, buf);

	return xfer_action_is(act) ? act : FEATURE_ACT_NONE;
}

/*
 * The transfer hook gate. The star code hooks of OFFHOOK_DIAL require their
 * own ops, and this gate requires both transfer ops plus the loaded table: a
 * shell that wires neither keeps the held-call digit path free of transfer
 * branches, so no cell can reach a NULL op.
 */
static bool xfer_hooks_present(const struct fsm *f)
{
	return f->cfg.feature_codes && f->ops->call_xfer && f->ops->call_xfer_consult;
}

/* ----------------------------------------------------------- state entries */

static void calling_enter(struct fsm *f, unsigned int done);

static void start_window_arm(struct fsm *f)
{
	if (f->cfg.warmline)
		timer_arm(f, &f->t_digit, f->cfg.warmline_ms);
	else
		timer_arm(f, &f->t_digit, f->cfg.start_digit_ms);
}

/*
 * The hotline. The uci schema defines the trigger: a warmline whose window is
 * zero, with a number to dial, is the hotline configuration.
 *
 * The shape: no dial tone, no collection, the number dialed at the entry
 * itself. A zero length timer would put the digit and the expiry in one event
 * pass, and a digit that wins would cancel the window so the call never
 * leaves. The entry therefore dials instead of arming, and the race has no
 * window to open in.
 *
 * A zero window with an empty number is not a hotline. That configuration
 * still arms, so the T_WARMLINE cell of OFFHOOK_DIAL gives it the fast busy an
 * unusable warmline earns, instead of silence.
 */
static bool hotline_is(const struct fsm *f)
{
	return f->cfg.warmline && !f->cfg.warmline_ms && f->cfg.warmline_number[0];
}

static void idle_enter(struct fsm *f, unsigned int done)
{
	if (!(done & ENT_TONE))
		tone_set(f, HAL_TONE_NONE);
	if (!(done & ENT_BELL))
		bell_stop(f);

	digits_clear(f);
	pending_clear(f);
	slot_second_clear(f);
	f->active_slot = FSM_SLOT_NONE;
	held_set(f, FSM_SLOT_NONE);
	f->state = FSM_IDLE;
}

/*
 * The dial tone of a line the subscriber just seized: the message waiting
 * stutter when cfg.mwi says a message is waiting and the audible form is
 * configured, the ordinary dial tone otherwise. The choice belongs to the
 * first dial tone only; the recall dial tone of HOLD_DIAL plays
 * HAL_TONE_SHORT_STUTTER_DIAL whatever is waiting.
 */
static enum hal_tone dial_tone_of(const struct fsm *f)
{
	return f->cfg.mwi ? HAL_TONE_STUTTER : HAL_TONE_DIAL;
}

static void offhook_dial_enter(struct fsm *f, unsigned int done)
{
	digits_clear(f);
	pending_clear(f);
	slot_second_clear(f);
	f->active_slot = FSM_SLOT_NONE;
	held_set(f, FSM_SLOT_NONE);

	/*
	 * ENT_TIMER means the caller armed a window of its own, so that entry
	 * is not a warmline entry and the hotline does not belong to it: the
	 * hotline replaces exactly the zero length window start_window_arm()
	 * would otherwise arm. The tone is stopped in every branch, which also
	 * cancels the dial tone the shell defers, so nothing plays over the
	 * call this entry starts.
	 */
	if (!(done & ENT_TIMER) && hotline_is(f)) {
		tone_set(f, HAL_TONE_NONE);
		dial_start(f, f->cfg.warmline_number);
		calling_enter(f, ENT_TONE);
		return;
	}

	if (!(done & ENT_TONE))
		tone_set(f, dial_tone_of(f));

	if (!(done & ENT_TIMER))
		start_window_arm(f);

	f->state = FSM_OFFHOOK_DIAL;
}

static void calling_enter(struct fsm *f, unsigned int done)
{
	if (!(done & ENT_TONE))
		tone_set(f, HAL_TONE_NONE);

	pending_clear(f);
	f->state = FSM_CALLING;
}

static void digits_dial(struct fsm *f, unsigned int done)
{
	digits_markers_strip(f);
	dial_start(f, f->digits);
	calling_enter(f, done);
}

static void proceeding_enter(struct fsm *f, bool early_media)
{
	tone_set(f, early_media ? HAL_TONE_NONE : HAL_TONE_RINGBACK);
	pending_clear(f);
	f->state = FSM_PROCEEDING;
}

static void connected_enter(struct fsm *f, unsigned int active, unsigned int done)
{
	if (!(done & ENT_TONE))
		tone_set(f, HAL_TONE_NONE);

	pending_clear(f);
	/* one call is left, so the pair the eligibility describes is over */
	slot_second_clear(f);
	f->active_slot = active;
	held_set(f, FSM_SLOT_NONE);
	f->state = FSM_CONNECTED;
}

static void two_calls_enter(struct fsm *f, unsigned int active, unsigned int held,
			    unsigned int done)
{
	if (!(done & ENT_TONE))
		tone_set(f, HAL_TONE_NONE);

	pending_clear(f);
	f->active_slot = active;
	held_set(f, held);
	f->state = FSM_TWO_CALLS;
}

/*
 * The conference state CONF. Both legs face the phone, so neither is held:
 * active is the leg that was active when the join started and other is the
 * leg the join resumed.
 */
static void conf_enter(struct fsm *f, unsigned int active, unsigned int other,
		       unsigned int done)
{
	if (!(done & ENT_TONE))
		tone_set(f, HAL_TONE_NONE);

	pending_clear(f);
	f->active_slot = active;
	f->other_slot = other;
	f->other_held = false;
	f->state = FSM_CONF;
}

/*
 * The ring-no-answer window. It is armed on every entry into RINGING: the
 * inbound call of IDLE, the one HOLDING passes through and the waiting call
 * CW_OFFERED hands back when the subscriber goes on hook. It is armed only when
 * the no-answer condition is armed with a target. A line without the service
 * arms nothing and rings until the caller gives up.
 *
 * A window of zero seconds arms nothing either: it would redirect before the
 * bell could ring once, and the schema seeds fifteen.
 *
 * A held bell arms nothing for the same reason a zero window does not: the
 * subscriber is ignoring nothing yet. fsm_ring_started() arms it when the
 * bell really starts.
 */
static void ring_noanswer_arm(struct fsm *f)
{
	uint32_t ms;

	if (f->ring_held)
		return;

	if (!cf_target_of(f, FEATURE_CF_NOANSWER))
		return;

	ms = (uint32_t)feature_cfnr_timeout_s(f->cfg.features, f->cfg.feature_codes) *
	     1000u;
	if (!ms)
		return;

	timer_arm(f, &f->t_cw, ms);
}

static void ringing_enter(struct fsm *f, unsigned int ringing)
{
	pending_clear(f);
	f->active_slot = ringing;
	f->state = FSM_RINGING;
	ring_noanswer_arm(f);
}

static void hold_dial_enter(struct fsm *f, unsigned int held, unsigned int done)
{
	if (!(done & ENT_TONE))
		tone_set(f, HAL_TONE_SHORT_STUTTER_DIAL);
	if (!(done & ENT_TIMER))
		timer_arm(f, &f->t_digit, f->cfg.start_digit_ms);

	digits_clear(f);
	pending_clear(f);
	f->active_slot = FSM_SLOT_NONE;
	held_set(f, held);
	f->state = FSM_HOLD_DIAL;
}

/* the shell reads the state and the held role from inside the bell op to
 * pick the recall profile, so both are set before it */
static void holding_enter(struct fsm *f, unsigned int held, unsigned int done)
{
	if (!(done & ENT_TONE))
		tone_set(f, HAL_TONE_NONE);
	if (!(done & ENT_BELL))
		bell_stop(f);

	pending_clear(f);
	f->active_slot = FSM_SLOT_NONE;
	held_set(f, held);
	f->state = FSM_HOLDING;
	bell_start(f, held);
}

static enum hal_tone clearing_tone(enum fsm_clearing cls)
{
	if (cls == FSM_CLR_BUSY)
		return HAL_TONE_BUSY;
	if (cls == FSM_CLR_FASTBUSY)
		return HAL_TONE_REORDER;
	if (cls == FSM_CLR_UNOBTAINABLE)
		return HAL_TONE_NUMBER_UNOBTAINABLE;

	return HAL_TONE_NONE;
}

static void clearing_timer_arm(struct fsm *f, enum fsm_clearing cls)
{
	if (cls == FSM_CLR_BUSY)
		timer_arm(f, &f->t_clearing, f->cfg.busy_tone_ms);
	else if (cls == FSM_CLR_QUIET)
		timer_arm(f, &f->t_clearing, FSM_QUIET_MS);
	else
		timer_arm(f, &f->t_clearing, FSM_REORDER_MS);
}

static void clearing_enter(struct fsm *f, enum fsm_clearing cls, unsigned int done)
{
	if (!(done & ENT_TONE))
		tone_set(f, clearing_tone(cls));
	if (!(done & ENT_TIMER))
		clearing_timer_arm(f, cls);

	pending_clear(f);
	f->active_slot = FSM_SLOT_NONE;
	f->clearing = cls;
	f->state = FSM_CLEARING;
}

/* the cell already stopped the previous tone when it enters a quiet clearing */
static unsigned int clearing_done_tone(enum fsm_clearing cls)
{
	return cls == FSM_CLR_QUIET ? ENT_TONE : 0;
}

static void roh_enter(struct fsm *f)
{
	tone_set(f, HAL_TONE_HOWLER);
	pending_clear(f);
	f->state = FSM_ROH;

	/* zero roh_ms keeps the howler until on hook */
	if (f->cfg.roh_ms)
		timer_arm(f, &f->t_clearing, f->cfg.roh_ms);
}

static void silent_enter(struct fsm *f)
{
	pending_clear(f);
	f->state = FSM_SILENT;
}

/*
 * Whether the other leg of a record is a held one. Three operations leave it
 * unheld and the roles must say so: the waiting call of an answer is
 * alerting, and the two conference operations run while both legs face the
 * phone.
 */
static bool pending_other_held_is(enum fsm_pending_op op)
{
	return op != FSM_OP_CW_ANSWER && op != FSM_OP_CONF &&
	       op != FSM_OP_CONF_SPLIT;
}

static void pending_enter(struct fsm *f, enum fsm_pending_op op, unsigned int slot_op,
			  unsigned int slot_other, enum fsm_state on_success,
			  enum fsm_state on_fail)
{
	pending_clear(f);
	f->pending.op = op;
	f->pending.slot_op = slot_op;
	f->pending.slot_other = slot_other;
	f->pending.on_success = on_success;
	f->pending.on_fail = on_fail;

	f->active_slot = slot_op;
	f->other_slot = slot_other;
	f->other_held = slot_other != FSM_SLOT_NONE &&
			pending_other_held_is(op);
	f->state = FSM_PENDING;
}

static enum fsm_clearing clearing_class_of(uint32_t cls)
{
	if (cls == CALL_FAIL_BUSY)
		return FSM_CLR_BUSY;
	if (cls == CALL_FAIL_UNOBTAINABLE)
		return FSM_CLR_UNOBTAINABLE;
	if (cls == CALL_FAIL_QUIET)
		return FSM_CLR_QUIET;

	return FSM_CLR_FASTBUSY;
}

static unsigned int ev_slot(enum fsm_ev ev, uint32_t arg)
{
	if (ev == FSM_EV_CALL_PROCEEDING)
		return arg >> 1;
	if (ev == FSM_EV_CALL_FAILED)
		return arg >> 8;

	return arg;
}

/* --------------------------------------------- the network call waiting */

/*
 * The CW_NET cells of CONNECTED, TWO_CALLS and CONF. The alert belongs to an
 * answered call and to no slot: the switch holds the waiting call, so the
 * machine tracks one bool and the caller identity stays in sip/line.c.
 */
static void cw_net_off(struct fsm *f)
{
	f->cw_net = false;
	cw_net_alert_set(f, false);
	timer_stop(f, &f->t_cw);
}

/* the same off for a caller that must not disturb a line with no alert */
static void cw_net_stop(struct fsm *f)
{
	if (f->cw_net)
		cw_net_off(f);
}

/*
 * The exit discipline. CONNECTED, TWO_CALLS and CONF
 * are the states that carry the alert, so one choke point after the
 * transition ends it everywhere else: on hook, on flash, on a call end, and
 * on the local offer that takes the alert over. That keeps the other fifteen
 * states total without a stop in each of their cells, and a new cell cannot
 * forget it. The one cell that must stop the alert before its own actions run
 * is the local call waiting offer, which arms the same timer.
 *
 * The alert shares t_cw with the ring window, so a stop here would cancel a
 * window a transition into RINGING had just armed. Nothing reaches RINGING
 * with an alert still running: the alert lives in those three answered states
 * alone, the two paths into RINGING from them, the local offer of CONNECTED
 * and the release of CW_OFFERED, both end it before they get there, and CONF
 * reaches RINGING through no cell at all. The rule is stated because a
 * change that gives another state the alert would break it.
 */
static void cw_net_settle(struct fsm *f)
{
	if (f->state == FSM_CONNECTED || f->state == FSM_TWO_CALLS ||
	    f->state == FSM_CONF)
		return;

	cw_net_stop(f);
}

/*
 * The same exit discipline for the armed consult transfer. The arm belongs to
 * the consult call and to the two-call state it ends in, so one choke point
 * after the transition drops it everywhere else. That is what gives the arm
 * its cancel gesture without a cell of its own: a subscriber who flashes away
 * from the consult call, or whose consult call fails, lands in a state that
 * does not keep it. PENDING keeps it because the swap inside TWO_CALLS runs
 * through PENDING and the two far parties are still the ones being joined.
 * CONF keeps it for the same reason: the conference holds the same two
 * parties, and its on-hook cell fires the transfer.
 *
 * It emits no action, so it cannot change any cell's action list, and it is
 * total over the event path whatever a later change adds. The forced out of
 * service entry is the one transition that is not an event, so fsm_oos_force()
 * drops the arm itself: those are the two clearing points and there is no
 * third.
 */
static void xfer_armed_settle(struct fsm *f)
{
	if (f->state == FSM_CALLING || f->state == FSM_PROCEEDING ||
	    f->state == FSM_TWO_CALLS || f->state == FSM_PENDING ||
	    f->state == FSM_CONF)
		return;

	f->xfer_armed = false;
}

/*
 * The play gate is the effective call waiting value, the same field the local
 * offer of CONNECTED reads, so the star code override moves both. The local
 * mode then takes this path out: that mode serves the waiting call from this
 * line, so a switch driven alert would announce a second one. A repeated
 * play re-presents the alert
 * and restarts the auto-stop budget.
 *
 * The stop above the gate is deliberate and unconditional: the switch may end
 * an alert this line never presented, and the answer is the same off.
 *
 * Do not disturb does not gate this path, deliberately. The filter applies to
 * an offered call, and a call the switch holds never arrives as one.
 * Silencing it here would lose the call twice over. The alert would vanish,
 * and the busy forward would never fire, because no offer ever reaches this
 * line to be refused; the locally served twin of the same subscriber diverts
 * instead.
 */
static uint32_t cw_offer_ms(const struct fsm *f)
{
	uint32_t bound;

	if (!f->cfg.cw_offer_ms)
		return FSM_CW_TIMEOUT_MS;

	bound = f->cfg.cw_offer_ms + FSM_CW_SAS_MARGIN_MS;

	return bound < FSM_CW_TIMEOUT_MS ? FSM_CW_TIMEOUT_MS : bound;
}

static void cw_net_run(struct fsm *f, uint32_t arg)
{
	if (!arg) {
		cw_net_off(f);
		return;
	}

	if (!f->cfg.call_waiting || f->cfg.cw_mode == FSM_CW_LOCAL)
		return;

	f->cw_net = true;
	cw_net_alert_set(f, true);
	timer_arm(f, &f->t_cw, cw_offer_ms(f));
}

/* ----------------------------------------------------- the conference mixer */

/*
 * The mixer of the local three-way conference. These
 * two are the only emitters: the completion of the join turns it on, and
 * every path that ends the conference goes through the off, which is silent
 * when no conference runs. So a cell that never joined emits nothing.
 *
 * The on needs no ops guard. A join starts behind the eligibility guard of
 * TWO_CALLS, and that guard tests the op, which is the reasoning the on-hook
 * consult transfer of the same state gives for its own wiring.
 */
static void conf_start(struct fsm *f)
{
	f->ops->conf_set(f->priv, true);
	f->conf_on = true;
}

static void conf_stop(struct fsm *f)
{
	if (!f->conf_on)
		return;

	f->ops->conf_set(f->priv, false);
	f->conf_on = false;
}

/*
 * The exit discipline of the mixer, in the shape the network alert uses
 * above. CONF carries the conference, and so does the PENDING record of an
 * outstanding split, because the mixer runs until that hold answers. Every
 * other state ends it here, which covers the transitions that own no off of
 * their own, the on-hook of PENDING among them. The forced out of service entry
 * is not an event, so fsm_oos_force() emits its off by hand.
 */
static void conf_settle(struct fsm *f)
{
	if (f->state == FSM_CONF)
		return;
	if (f->state == FSM_PENDING && f->pending.op == FSM_OP_CONF_SPLIT)
		return;

	conf_stop(f);
}

/* ------------------------------------------------------- shared transitions */

static void answered_state_enter(struct fsm *f, unsigned int done)
{
	if (!held_has(f)) {
		connected_enter(f, f->active_slot, done);
		return;
	}

	two_calls_enter(f, f->active_slot, f->other_slot, done);
}

static bool two_calls_xfer_is(const struct fsm *f);

/*
 * The RELEASE of CALLING and PROCEEDING with a held leg and the transfer
 * guard of TWO_CALLS: the subscriber flashed, dialed a second party and went
 * on hook while that party still rings, so the held leg is referred to the
 * number dialed. No confirmed dialog exists to name in a Replaces, so an
 * armed consult also takes this blind form. The ringing leg is canceled
 * first: call.c marks it clearing, so a 200 that races the CANCEL starts
 * nothing.
 *
 * The target is the digit buffer. CALLING and PROCEEDING neither clear it nor
 * take a digit, so it still holds the number the second call was dialed with.
 *
 * Fire and forget, as the consult form of TWO_CALLS is. The outcome reaches
 * IDLE, where CALL_XFER_FAIL hangs the referred leg up and CALL_XFER_OK is
 * inert.
 * call.c settles the leg at the 202: xfer_blind_settle() drains it, and the BYE
 * comes from the network or from call.c behind the verdict. The subscriber may
 * have seized again by the time a failure arrives, so the teardown of the
 * referred leg does not depend on this machine: sip/line.c hands the call layer
 * a kind whose failure BYEs the leg itself (call.h CALL_XFER_ONHOOK).
 */
static void outbound_xfer(struct fsm *f)
{
	unsigned int held = f->other_slot;
	unsigned int active = f->active_slot;

	hangup_send(f, active);
	slot_release(f, active);
	tone_set(f, HAL_TONE_NONE);
	xfer_send(f, held, FEATURE_ACT_XFER_BLIND, f->digits);
	idle_enter(f, ENT_TONE);
}

/* the RELEASE of CALLING and PROCEEDING */
static void outbound_release(struct fsm *f)
{
	/* held_has() first: the guard is true on an armed transfer alone, and
	 * an origination with no held leg has nothing to refer */
	if (held_has(f) && two_calls_xfer_is(f)) {
		outbound_xfer(f);
		return;
	}

	hangup_send(f, f->active_slot);
	slot_release(f, f->active_slot);
	tone_set(f, HAL_TONE_NONE);

	if (!held_has(f)) {
		idle_enter(f, ENT_TONE);
		return;
	}

	holding_enter(f, f->other_slot, ENT_TONE);
}

/* flash off hook with no answered call */
static void offhook_flash(struct fsm *f)
{
	if (f->active_slot != FSM_SLOT_NONE) {
		hangup_send(f, f->active_slot);
		slot_release(f, f->active_slot);
	}

	if (!held_has(f)) {
		offhook_dial_enter(f, 0);
		return;
	}

	resume_send(f, f->other_slot);
	pending_enter(f, FSM_OP_RESUME, f->other_slot, FSM_SLOT_NONE, FSM_CONNECTED,
		      FSM_CLEARING);
}

static void held_resume(struct fsm *f)
{
	tone_set(f, HAL_TONE_NONE);
	resume_send(f, f->other_slot);
	pending_enter(f, FSM_OP_RESUME, f->other_slot, FSM_SLOT_NONE, FSM_CONNECTED,
		      FSM_CLEARING);
}

/*
 * The CALL_FAILED of CALLING and PROCEEDING. A held leg that dies releases its
 * slot and nothing else: it must not clear an outbound call in progress,
 * which is the CALL_REMOTE_BYE rule of the same two sections.
 */
static void outbound_failed(struct fsm *f, unsigned int slot, uint32_t cls)
{
	enum fsm_clearing cl;

	if (held_is(f, slot)) {
		slot_release(f, slot);
		return;
	}
	if (slot != f->active_slot)
		return;

	cl = clearing_class_of(cls);
	slot_release(f, slot);
	tone_set(f, HAL_TONE_NONE);
	clearing_enter(f, cl, clearing_done_tone(cl));
}

/*
 * The admission of an offer a line could otherwise take. Two conditions come
 * before the ring, in this order:
 *
 * 1. Unconditional forward. Armed with a target, the call goes to that target
 *    and the line neither rings nor offers. Armed without one, it falls
 *    through, because a redirect with no destination is not a service.
 * 2. Do not disturb. The offer is refused as busy, which reject_send turns
 *    into the busy forward when that condition is armed.
 *
 * True means the offer is dealt with and the caller changes nothing else: the
 * state, the roles and the timers all stay as they were, which is what lets
 * HOLDING keep its held leg and its recall ring.
 */
static bool incoming_refused(struct fsm *f, unsigned int slot)
{
	const char *target = cf_target_of(f, FEATURE_CF_UNCOND);

	if (target) {
		forward_send(f, slot, target);
		return true;
	}

	if (!dnd_is(f))
		return false;

	reject_send(f, slot, FSM_REJECT_BUSY);

	return true;
}

/*
 * A code that applied plays the confirmation tone and hands the line a fresh
 * dial tone, fresh timers and an empty buffer. A code that refused is
 * announced like any other failure.
 */
static void feature_run(struct fsm *f)
{
	if (f->ops->feature_invoke(f->priv, f->digits)) {
		tone_set(f, HAL_TONE_CONFIRM);
		offhook_dial_enter(f, 0);
		return;
	}

	timer_stop(f, &f->t_digit);
	digits_clear(f);
	clearing_enter(f, FSM_CLR_FASTBUSY, 0);
}

/*
 * An unfinished star code must not let the dial plan clear the line, so a
 * NO_MATCH verdict arms the long window instead of entering CLEARING. Every
 * other verdict keeps the dial plan's own timer choice, which is what the
 * T_INTERDIGIT cell rescans when it expires, so an abandoned star code ends
 * exactly where the dial plan says it ends.
 */
static void feature_partial_arm(struct fsm *f)
{
	enum dp_action act = digits_verdict(f);
	uint32_t ms = f->cfg.interdigit_long_ms;

	if (act == DP_ACT_DIAL || act == DP_ACT_WAIT_SHORT)
		ms = f->cfg.interdigit_short_ms;

	timer_arm(f, &f->t_digit, ms);
}

/*
 * The digit branches of OFFHOOK_DIAL, COLLECTING and HOLD_DIAL. The caller
 * emits the per state prefix; collect_state is where a partial match parks,
 * and done says whether that prefix already emitted the tone(NONE) of the
 * CALLING entry.
 */
static void digit_collect(struct fsm *f, char digit, enum fsm_state collect_state,
			  unsigned int done)
{
	if (digit_pound_delim(f, digit)) {
		timer_stop(f, &f->t_digit);
		digits_dial(f, done);
		return;
	}

	digits_append(f, digit);

	switch (digits_verdict(f)) {
	case DP_ACT_DIAL:
		timer_stop(f, &f->t_digit);
		digits_dial(f, done);
		return;
	case DP_ACT_WAIT_SHORT:
		timer_arm(f, &f->t_digit, f->cfg.interdigit_short_ms);
		f->state = collect_state;
		return;
	case DP_ACT_WAIT_LONG:
		timer_arm(f, &f->t_digit, f->cfg.interdigit_long_ms);
		f->state = collect_state;
		return;
	case DP_ACT_REORDER:
		timer_stop(f, &f->t_digit);
		digits_clear(f);
		clearing_enter(f, FSM_CLR_FASTBUSY, 0);
		return;
	}
}

/*
 * The second stage of SPRE_COLLECT. The code stays in the buffer and the
 * target is collected behind it, so the split is spre_code_len and no second
 * buffer exists. The recall dial tone is HAL_TONE_SECOND_DIAL, which has two
 * entries: this one and the XFER_COLLECT entry.
 */
static void spre_collect_enter(struct fsm *f)
{
	tone_set(f, HAL_TONE_SECOND_DIAL);
	timer_arm(f, &f->t_digit, f->cfg.interdigit_long_ms);
	f->spre_code_len = f->ndigits;
	f->state = FSM_SPRE_COLLECT;
}

/*
 * The second stage of XFER_COLLECT, which is the same recall tone with a
 * call held. The buffer is EMPTY here, unlike the SPRE_COLLECT one, because
 * the dial plan verdicts of the target run on the bare number: a transfer
 * target is dialed, not carried behind its code.
 */
static void xfer_collect_enter(struct fsm *f)
{
	tone_set(f, HAL_TONE_SECOND_DIAL);
	timer_arm(f, &f->t_digit, f->cfg.interdigit_long_ms);
	f->state = FSM_XFER_COLLECT;
}

/*
 * The '#' that ends the target is consumed, exactly as the '#' inside a code
 * is. The argument can be empty, which the shell reads as "the
 * configured target"; the answer decides the tone either way.
 */
static void spre_run(struct fsm *f)
{
	char code[FSM_DIGITS_MAX + 1];

	memcpy(code, f->digits, f->spre_code_len);
	code[f->spre_code_len] = 0;

	if (f->ops->feature_invoke_arg(f->priv, code, f->digits + f->spre_code_len)) {
		tone_set(f, HAL_TONE_CONFIRM);
		offhook_dial_enter(f, 0);
		return;
	}

	digits_clear(f);
	clearing_enter(f, FSM_CLR_FASTBUSY, 0);
}

/*
 * The DIGIT cell of SPRE_COLLECT: '#' ends the target, every other digit
 * extends it.
 *
 * A digit that does not fit refuses the whole code. Holding the buffer and
 * collecting on, which is what the dial plan collector of COLLECTING does,
 * would arm a forward on the number the subscriber dialed minus its last
 * digits, and announce that with the confirmation tone. features.c refuses
 * an over-long target for the same reason, so both layers answer the same
 * way.
 */
static void spre_digit(struct fsm *f, char digit)
{
	if (digit == '#') {
		timer_stop(f, &f->t_digit);
		spre_run(f);
		return;
	}

	if (f->ndigits >= FSM_DIGITS_MAX) {
		timer_stop(f, &f->t_digit);
		digits_clear(f);
		clearing_enter(f, FSM_CLR_FASTBUSY, 0);
		return;
	}

	digits_append(f, digit);
	timer_arm(f, &f->t_digit, f->cfg.interdigit_long_ms);
}

/*
 * the DIGIT cells of OFFHOOK_DIAL and COLLECTING: the feature table first,
 * then the HOLD_DIAL digit path.
 *
 * A transfer code here has no held call, so it never opens the second stage:
 * the three rows take the argument branch at the table surface, and
 * XFER_COLLECT is the state that collects for them. features.c refuses to
 * execute them, so the match runs the refusal path and the subscriber gets
 * the fast busy at once.
 */
static void digit_feature_collect(struct fsm *f, char digit, unsigned int done)
{
	switch (feature_probe(f, digit)) {
	case FEATURE_MATCH:
		digits_append(f, digit);
		if (xfer_code_of(f, f->digits) == FEATURE_ACT_NONE &&
		    f->cfg.feature_codes &&
		    feature_takes_arg(f->cfg.feature_codes, f->digits)) {
			spre_collect_enter(f);
			return;
		}
		feature_run(f);
		return;
	case FEATURE_PARTIAL:
		digits_append(f, digit);
		feature_partial_arm(f);
		f->state = FSM_COLLECTING;
		return;
	case FEATURE_NONE:
		break;
	}

	digit_collect(f, digit, FSM_COLLECTING, done);
}

/* ------------------------------------------------------------------ IDLE */

static void st_idle(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	switch (ev) {
	case FSM_EV_SEIZE:
		if (!f->dial_ready) {
			clearing_enter(f, FSM_CLR_FASTBUSY, 0);
			return;
		}
		offhook_dial_enter(f, 0);
		return;
	case FSM_EV_CALL_INCOMING:
		if (incoming_refused(f, arg))
			return;
		slot_inbound_set(f, arg, true);
		ring180_send(f, arg);
		bell_start(f, arg);
		ringing_enter(f, arg);
		return;
	case FSM_EV_CALL_XFER_FAIL:
		/*
		 * The on-hook blind transfer of CALLING and PROCEEDING was
		 * refused after this line went idle, and the referred leg is
		 * still up with nobody on the phone: hang it up. The hangup is
		 * idempotent on a leg the call layer ended itself (call.h
		 * CALL_XFER_ONHOOK).
		 */
		hangup_send(f, arg);
		return;
	default:
		return;
	}
}

/* ---------------------------------------------------------- OFFHOOK_DIAL */

static void offhook_warmline(struct fsm *f)
{
	tone_set(f, HAL_TONE_NONE);

	if (f->cfg.warmline_number[0]) {
		dial_start(f, f->cfg.warmline_number);
		calling_enter(f, ENT_TONE);
		return;
	}

	clearing_enter(f, FSM_CLR_FASTBUSY, 0);
}

static void st_offhook_dial(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	switch (ev) {
	case FSM_EV_RELEASE:
		tone_set(f, HAL_TONE_NONE);
		timer_stop(f, &f->t_digit);
		idle_enter(f, ENT_TONE);
		return;
	case FSM_EV_FLASH:
		tone_set(f, dial_tone_of(f));
		offhook_dial_enter(f, ENT_TONE);
		return;
	case FSM_EV_DIGIT:
		timer_stop(f, &f->t_digit);
		tone_set(f, HAL_TONE_NONE);
		digit_feature_collect(f, (char)arg, ENT_TONE);
		return;
	case FSM_EV_CALL_INCOMING:
		reject_send(f, arg, FSM_REJECT_BUSY);
		return;
	case FSM_EV_T_START_DIGIT:
		tone_set(f, HAL_TONE_NONE);
		clearing_enter(f, FSM_CLR_FASTBUSY, 0);
		return;
	case FSM_EV_T_WARMLINE:
		offhook_warmline(f);
		return;
	default:
		return;
	}
}

/* ------------------------------------------------------------ COLLECTING */

static void interdigit_fire(struct fsm *f, bool tone_first)
{
	enum dp_action act = digits_expire(f);

	if (act == DP_ACT_DIAL) {
		digits_dial(f, 0);
		return;
	}
	if (digits_wait_arm(f, act))
		return;

	if (tone_first)
		tone_set(f, HAL_TONE_NONE);

	digits_clear(f);
	clearing_enter(f, FSM_CLR_FASTBUSY, 0);
}

static void st_collecting(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	switch (ev) {
	case FSM_EV_RELEASE:
		tone_set(f, HAL_TONE_NONE);
		timer_stop(f, &f->t_digit);
		idle_enter(f, ENT_TONE);
		return;
	case FSM_EV_FLASH:
		timer_stop(f, &f->t_digit);
		offhook_dial_enter(f, 0);
		return;
	case FSM_EV_DIGIT:
		digit_feature_collect(f, (char)arg, 0);
		return;
	case FSM_EV_CALL_INCOMING:
		reject_send(f, arg, FSM_REJECT_BUSY);
		return;
	case FSM_EV_T_INTERDIGIT:
		interdigit_fire(f, false);
		return;
	default:
		return;
	}
}

/* ---------------------------------------------------------- SPRE_COLLECT */

static void st_spre_collect(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	switch (ev) {
	case FSM_EV_RELEASE:
		tone_set(f, HAL_TONE_NONE);
		timers_stop_all(f);
		legs_hangup(f);
		idle_enter(f, ENT_TONE);
		return;
	case FSM_EV_DIGIT:
		tone_set(f, HAL_TONE_NONE);
		spre_digit(f, (char)arg);
		return;
	/* the window between two digits of the target; an abandoned code ends
	 * where every abandoned dial string ends */
	case FSM_EV_T_INTERDIGIT:
		digits_clear(f);
		clearing_enter(f, FSM_CLR_FASTBUSY, 0);
		return;
	case FSM_EV_CALL_INCOMING:
		reject_send(f, arg, FSM_REJECT_BUSY);
		return;
	case FSM_EV_CALL_FAILED:
		slot_release(f, ev_slot(ev, arg));
		tone_set(f, HAL_TONE_NONE);
		clearing_enter(f, FSM_CLR_FASTBUSY, 0);
		return;
	case FSM_EV_CALL_REMOTE_BYE:
		slot_release(f, arg);
		tone_set(f, HAL_TONE_NONE);
		clearing_enter(f, FSM_CLR_QUIET, ENT_TONE);
		return;
	default:
		return;
	}
}

/* ------------------------------------------------ CALLING and PROCEEDING */

static void st_outbound(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	unsigned int slot = ev_slot(ev, arg);

	switch (ev) {
	case FSM_EV_RELEASE:
		outbound_release(f);
		return;
	case FSM_EV_FLASH:
		offhook_flash(f);
		return;
	case FSM_EV_CALL_INCOMING:
		reject_send(f, arg, FSM_REJECT_BUSY);
		return;
	case FSM_EV_CALL_PROCEEDING:
		if (slot != f->active_slot)
			return;
		if (f->state == FSM_CALLING) {
			proceeding_enter(f, (arg & 1u) != 0);
			return;
		}
		if (arg & 1u)
			tone_set(f, HAL_TONE_NONE);
		return;
	case FSM_EV_CALL_ANSWERED:
		if (slot != f->active_slot)
			return;
		tone_set(f, HAL_TONE_NONE);
		answered_state_enter(f, ENT_TONE);
		return;
	case FSM_EV_CALL_FAILED:
		outbound_failed(f, slot, arg & 0xffu);
		return;
	case FSM_EV_CALL_REMOTE_BYE:
		if (!held_is(f, slot))
			return;
		slot_release(f, slot);
		return;
	default:
		return;
	}
}

/* ------------------------------------------------------------- CONNECTED */

/*
 * The relay is tried first and it is exclusive: local feature ownership
 * decides who gets the flash, and sip/line.c already resolved that this line
 * owns no local conference. The state does not change, so the network-alert
 * exit rule does not apply here.
 */
static void connected_flash(struct fsm *f)
{
	if (f->cfg.flash_relay) {
		flash_relay_send(f);
		return;
	}

	if (!f->cfg.flash_features || !slot_free_has(f))
		return;

	hold_send(f, f->active_slot);
	pending_enter(f, FSM_OP_HOLD, f->active_slot, FSM_SLOT_NONE, FSM_HOLD_DIAL,
		      FSM_CONNECTED);
}

static void cw_offer_enter(struct fsm *f, unsigned int waiting)
{
	ring180_send(f, waiting);
	cw_alert_set(f, waiting, true);
	timer_arm(f, &f->t_cw, cw_offer_ms(f));

	f->other_slot = waiting;
	f->other_held = false;
	f->state = FSM_CW_OFFERED;
}

/*
 * The network mode gives the waiting call to the switch, so the local offer
 * is refused exactly as a line with call waiting off refuses it.
 * call_waiting itself is not lowered:
 * the service is enabled and merely network served, so the query code keeps
 * answering it and the star codes keep toggling it.
 */
static void connected_incoming(struct fsm *f, unsigned int slot)
{
	/* the unconditional forward and the do-not-disturb filter come first
	 * here too, in the same order */
	if (incoming_refused(f, slot))
		return;

	if (!f->cfg.call_waiting || f->cfg.cw_mode == FSM_CW_NETWORK ||
	    !slot_free_has(f)) {
		reject_send(f, slot, FSM_REJECT_BUSY);
		return;
	}

	slot_inbound_set(f, slot, true);
	/* the line already owns the answered call, so this one is the second.
	 * The clear comes first: a refusal recorded against the pair this offer
	 * replaces must not describe the new one */
	slot_second_clear(f);
	f->slot_second = slot;

	/* the local offer takes the alert and T_CW over, so the network one
	 * ends before the offer arms them and not after */
	cw_net_stop(f);
	cw_offer_enter(f, slot);
}

static void connected_failed(struct fsm *f, unsigned int slot, uint32_t cls)
{
	enum fsm_clearing cl = clearing_class_of(cls);

	hangup_send(f, slot);
	slot_release(f, slot);
	tone_set(f, HAL_TONE_NONE);
	clearing_enter(f, cl, clearing_done_tone(cl));
}

static void st_connected(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	unsigned int slot = ev_slot(ev, arg);

	switch (ev) {
	case FSM_EV_RELEASE:
		hangup_send(f, f->active_slot);
		tone_set(f, HAL_TONE_NONE);
		idle_enter(f, ENT_TONE);
		return;
	case FSM_EV_FLASH:
		connected_flash(f);
		return;
	case FSM_EV_DIGIT:
		dtmf_send(f, (char)arg);
		return;
	case FSM_EV_CALL_INCOMING:
		connected_incoming(f, arg);
		return;
	case FSM_EV_CALL_FAILED:
		if (slot != f->active_slot)
			return;
		connected_failed(f, slot, arg & 0xffu);
		return;
	case FSM_EV_CALL_REMOTE_BYE:
		if (slot != f->active_slot)
			return;
		slot_release(f, slot);
		tone_set(f, HAL_TONE_NONE);
		clearing_enter(f, FSM_CLR_QUIET, ENT_TONE);
		return;
	case FSM_EV_CW_NET:
		cw_net_run(f, arg);
		return;
	case FSM_EV_T_CW:
		/* the auto-stop budget of the alert; with no alert running the
		 * stale timer ignore stands */
		cw_net_stop(f);
		return;
	default:
		return;
	}
}

/* --------------------------------------------------------------- RINGING */

/* the tail every end of the alerting leg shares, whatever ended it */
static void ringing_leg_end(struct fsm *f, unsigned int slot)
{
	slot_release(f, slot);

	if (!held_has(f)) {
		idle_enter(f, ENT_BELL);
		return;
	}

	holding_enter(f, f->other_slot, ENT_BELL);
}

static void ringing_leg_clear(struct fsm *f, unsigned int slot)
{
	if (held_is(f, slot)) {
		slot_release(f, slot);
		return;
	}
	if (slot != f->active_slot)
		return;

	bell_stop(f);
	timer_stop(f, &f->t_cw);
	ringing_leg_end(f, slot);
}

/*
 * The window expired with the handset still on hook. The bell
 * stops, the caller is redirected and the leg ends as a refusal this machine
 * asked for, so the line reaches the same place a canceled call reaches.
 */
static void ringing_noanswer(struct fsm *f)
{
	const char *target = cf_target_of(f, FEATURE_CF_NOANSWER);
	unsigned int slot = f->active_slot;

	if (!target || slot == FSM_SLOT_NONE)
		return;

	bell_stop(f);
	forward_send(f, slot, target);
	ringing_leg_end(f, slot);
}

static void st_ringing(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	unsigned int slot = ev_slot(ev, arg);

	switch (ev) {
	case FSM_EV_SEIZE:
		bell_stop(f);
		timer_stop(f, &f->t_cw);
		answer_send(f, f->active_slot);
		answered_state_enter(f, 0);
		return;
	case FSM_EV_CALL_INCOMING:
		reject_send(f, arg, FSM_REJECT_BUSY);
		return;
	case FSM_EV_CALL_FAILED:
	case FSM_EV_CALL_REMOTE_BYE:
		ringing_leg_clear(f, slot);
		return;
	case FSM_EV_CALL_REMOTE_CANCEL:
		if (slot != f->active_slot)
			return;
		ringing_leg_clear(f, slot);
		return;
	case FSM_EV_T_RING_NOANSWER:
		ringing_noanswer(f);
		return;
	default:
		return;
	}
}

/* ------------------------------------------------------------ CW_OFFERED */

/*
 * The on-hook of CW_OFFERED. With the active leg gone the line is playing the
 * reorder of cw_active_clear() below, and the tone stops before the bell:
 * this is the on-hook that presents the waiting call, and the loop is open
 * now, so the ring a closed loop refused is possible.
 */
static void cw_release(struct fsm *f)
{
	unsigned int waiting = f->other_slot;

	timer_stop(f, &f->t_cw);
	cw_alert_set(f, waiting, false);
	if (f->active_slot != FSM_SLOT_NONE)
		hangup_send(f, f->active_slot);
	else
		tone_set(f, HAL_TONE_NONE);

	bell_start(f, waiting);
	held_set(f, FSM_SLOT_NONE);
	ringing_enter(f, waiting);
}

static void cw_flash(struct fsm *f)
{
	unsigned int waiting = f->other_slot;

	timer_stop(f, &f->t_cw);
	cw_alert_set(f, waiting, false);

	if (f->active_slot != FSM_SLOT_NONE) {
		hold_send(f, f->active_slot);
		pending_enter(f, FSM_OP_CW_ANSWER, f->active_slot, waiting, FSM_TWO_CALLS,
			      FSM_CONNECTED);
		return;
	}

	held_set(f, FSM_SLOT_NONE);
	answer_send(f, waiting);
	connected_enter(f, waiting, 0);
}

/*
 * The waiting call is gone, or its offer window ended. With an active leg the
 * line goes back to that call. With none, the line is already playing the
 * reorder of cw_active_clear() below, and the fast busy clearing continues it
 * into the howler: its entry re-issues the tone, which restarts the tone's own
 * duration beside the clearing timer, so the two end together and no silence
 * opens between the offer window and the howler.
 */
static void cw_waiting_clear(struct fsm *f)
{
	timer_stop(f, &f->t_cw);
	cw_alert_set(f, f->other_slot, false);
	held_set(f, FSM_SLOT_NONE);

	if (f->active_slot != FSM_SLOT_NONE) {
		connected_enter(f, f->active_slot, 0);
		return;
	}

	clearing_enter(f, FSM_CLR_FASTBUSY, 0);
}

/*
 * The active party hung up while a call waits (CW_OFFERED CALL_REMOTE_BYE and
 * CALL_FAILED on the active slot): the subscriber hears the
 * reorder, so the end of the call is announced, and the waiting call is kept
 * for the on-hook, which rings it (cw_release), or the flash, which answers it
 * (cw_flash). It cannot ring now: the line is off hook and a SLIC cannot ring
 * a closed loop. The state stays CW_OFFERED with the active slot empty, which
 * is the sub-shape those two cells already carry.
 *
 * The alert stops, because the beep would ride over the reorder and the reorder
 * is the cue now. The offer window is re-armed to the smaller of the offer
 * budget and the reorder duration: the shell plays the reorder for
 * FSM_REORDER_MS and then stops it, so a window that outlived the tone would
 * leave silence until the fast busy clearing re-issued it, where a window
 * bounded by the tone hands the line to that clearing while the tone still
 * plays.
 *
 * A failure hangs the leg up first, as the CONNECTED cell does; a far-end BYE
 * has nothing left to hang up.
 */
static void cw_active_clear(struct fsm *f, unsigned int slot, bool hangup)
{
	uint32_t window = cw_offer_ms(f);

	if (hangup)
		hangup_send(f, slot);
	slot_release(f, slot);
	cw_alert_set(f, f->other_slot, false);
	tone_set(f, HAL_TONE_REORDER);

	if (FSM_REORDER_MS < window)
		window = FSM_REORDER_MS;
	timer_arm(f, &f->t_cw, window);
}

static void st_cw_offered(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	unsigned int slot = ev_slot(ev, arg);

	switch (ev) {
	case FSM_EV_RELEASE:
		cw_release(f);
		return;
	case FSM_EV_FLASH:
		cw_flash(f);
		return;
	case FSM_EV_DIGIT:
		if (f->active_slot == FSM_SLOT_NONE)
			return;
		dtmf_send(f, (char)arg);
		return;
	case FSM_EV_CALL_INCOMING:
		reject_send(f, arg, FSM_REJECT_BUSY);
		return;
	case FSM_EV_CALL_FAILED:
	case FSM_EV_CALL_REMOTE_BYE:
		if (f->active_slot != FSM_SLOT_NONE && slot == f->active_slot) {
			cw_active_clear(f, slot, ev == FSM_EV_CALL_FAILED);
			return;
		}
		if (slot != f->other_slot)
			return;
		cw_waiting_clear(f);
		return;
	case FSM_EV_CALL_REMOTE_CANCEL:
		/* the active leg is answered, so its CANCEL has no effect */
		if (slot != f->other_slot)
			return;
		cw_waiting_clear(f);
		return;
	case FSM_EV_T_CW:
		reject_send(f, f->other_slot, FSM_REJECT_NOANSWER);
		cw_waiting_clear(f);
		return;
	default:
		return;
	}
}

/* ------------------------------------------------------------- HOLD_DIAL */

static void hold_dial_recall(struct fsm *f)
{
	timer_stop(f, &f->t_digit);
	digits_clear(f);
	held_resume(f);
}

/*
 * The transfer classification of the HOLD_DIAL DIGIT cell. It runs after the
 * cell prefix and before the dial-plan verdict. True means the digit was
 * consumed here.
 *
 * The transfer patterns take precedence over the dial plan in the held-call
 * scope. Only the three transfer rows act: every other code is not legal
 * against a held call and falls through to the dial plan and the network.
 */
static bool hold_dial_xfer(struct fsm *f, char digit)
{
	char cand[FSM_DIGITS_MAX + 2];
	enum feature_action act;

	if (!xfer_hooks_present(f) || !digits_cand(f, digit, cand))
		return false;

	act = xfer_code_of(f, cand);
	if (act != FEATURE_ACT_NONE) {
		timer_stop(f, &f->t_digit);
		f->xfer_act = act;
		digits_clear(f);
		xfer_collect_enter(f);
		return true;
	}

	if (!feature_xfer_prefix(f->cfg.feature_codes, cand))
		return false;

	digits_append(f, digit);
	timer_arm(f, &f->t_digit, f->cfg.interdigit_long_ms);

	return true;
}

/*
 * The long window of such a prefix expired. The dial plan was not consulted
 * for any of those digits, so it rules the whole buffer once here and only a
 * DIAL verdict commits it.
 */
static bool hold_dial_prefix_held(const struct fsm *f)
{
	return xfer_hooks_present(f) && feature_xfer_prefix(f->cfg.feature_codes, f->digits);
}

static void hold_dial_prefix_expire(struct fsm *f)
{
	if (digits_verdict(f) == DP_ACT_DIAL) {
		digits_dial(f, 0);
		return;
	}

	tone_set(f, HAL_TONE_NONE);
	digits_clear(f);
	clearing_enter(f, FSM_CLR_FASTBUSY, 0);
}

static void hold_dial_leg_dead(struct fsm *f, unsigned int slot)
{
	slot_release(f, slot);

	if (f->ndigits > 0) {
		f->state = FSM_COLLECTING;
		return;
	}

	/* the loss of the held leg grants no second first-digit window */
	offhook_dial_enter(f, ENT_TIMER);
}

static void st_hold_dial(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	unsigned int slot = ev_slot(ev, arg);

	switch (ev) {
	case FSM_EV_RELEASE:
		tone_set(f, HAL_TONE_NONE);
		timer_stop(f, &f->t_digit);
		digits_clear(f);
		holding_enter(f, f->other_slot, ENT_TONE);
		return;
	case FSM_EV_FLASH:
		hold_dial_recall(f);
		return;
	case FSM_EV_DIGIT:
		timer_stop(f, &f->t_digit);
		tone_set(f, HAL_TONE_NONE);
		if (hold_dial_xfer(f, (char)arg))
			return;
		digit_collect(f, (char)arg, FSM_HOLD_DIAL, ENT_TONE);
		return;
	case FSM_EV_CALL_INCOMING:
		reject_send(f, arg, FSM_REJECT_BUSY);
		return;
	case FSM_EV_CALL_FAILED:
	case FSM_EV_CALL_REMOTE_BYE:
		if (!held_is(f, slot))
			return;
		hold_dial_leg_dead(f, slot);
		return;
	case FSM_EV_T_INTERDIGIT:
		if (hold_dial_prefix_held(f)) {
			hold_dial_prefix_expire(f);
			return;
		}
		interdigit_fire(f, true);
		return;
	case FSM_EV_T_START_DIGIT:
		tone_set(f, HAL_TONE_NONE);
		clearing_enter(f, FSM_CLR_FASTBUSY, 0);
		return;
	default:
		return;
	}
}

/* ------------------------------------------------------------- TWO_CALLS */

static void two_calls_leg_dead(struct fsm *f, unsigned int slot)
{
	if (slot == f->active_slot) {
		slot_release(f, slot);
		resume_send(f, f->other_slot);
		pending_enter(f, FSM_OP_RESUME, f->other_slot, FSM_SLOT_NONE,
			      FSM_CONNECTED, FSM_CLEARING);
		return;
	}
	if (!held_is(f, slot))
		return;

	slot_release(f, slot);
	connected_enter(f, f->active_slot, 0);
}

/*
 * The on-hook consult transfer (TWO_CALLS RELEASE). Two ways in, and the star
 * code is the one that bypasses the direction rule:
 *
 * 1. the subscriber armed it with the consult star code before the second
 *    call, which is the arm-before-call order;
 * 2. the line is configured for it and the two legs face the required
 *    directions, the held one incoming and the active one outgoing. With the
 *    legs the other way round the on-hook is the ordinary park of the cell.
 *
 * The op is fire and forget: the machine reaches IDLE and the far parties
 * finish the transfer between themselves. A refusal costs both parties their
 * call. The drain of the replaced leg on success is the call layer's (call.h
 * CALL_XFER_DRAIN_MS), so this machine keeps no cell for it.
 */
static bool two_calls_xfer_is(const struct fsm *f)
{
	if (f->xfer_armed)
		return true;
	if (!f->cfg.xfer_on_hook)
		return false;

	return slot_inbound_is(f, f->other_slot) && !slot_inbound_is(f, f->active_slot);
}

/*
 * No ops guard here, and none is needed: xfer_armed is only ever set behind
 * the transfer hook gate, and sip/line.c derives xfer_on_hook in the same view
 * it wires the ops in, so a shell with no transfer reaches neither branch.
 */
static void two_calls_xfer(struct fsm *f)
{
	unsigned int held = f->other_slot;
	unsigned int active = f->active_slot;

	tone_set(f, HAL_TONE_NONE);
	xfer_consult_send(f, held, active);
	idle_enter(f, ENT_TONE);
}

/*
 * The conference eligibility of the TWO_CALLS FLASH cell. slot_second names the
 * call this line acquired while it already owned another one, so the test is
 * order based and a swap does not move it.
 *
 * Both direction terms keep the join to calls this line originated. The
 * second call takes the test in every mode, and the first call takes it as
 * well when the mode splits the conference back into two calls.
 */
static bool two_calls_conf_is(const struct fsm *f)
{
	unsigned int first;

	if (f->cfg.conf_mode == FSM_CONF_OFF || !f->ops->conf_set ||
	    f->conf_refused)
		return false;
	if (!slot_owned(f, f->slot_second) ||
	    slot_inbound_is(f, f->slot_second))
		return false;
	if (f->cfg.conf_mode != FSM_CONF_SPLIT)
		return true;

	first = f->slot_second == f->active_slot ? f->other_slot :
		f->active_slot;

	return !slot_inbound_is(f, first);
}

/*
 * The join. The held leg comes back first and the mixer follows on the
 * completion, never here.
 */
static void two_calls_conf(struct fsm *f)
{
	unsigned int held = f->other_slot;
	unsigned int active = f->active_slot;

	resume_send(f, held);
	pending_enter(f, FSM_OP_CONF, held, active, FSM_CONF, FSM_TWO_CALLS);
}

static void st_two_calls(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	unsigned int slot = ev_slot(ev, arg);

	switch (ev) {
	case FSM_EV_RELEASE:
		if (two_calls_xfer_is(f)) {
			two_calls_xfer(f);
			return;
		}
		hangup_send(f, f->active_slot);
		slot_release(f, f->active_slot);
		tone_set(f, HAL_TONE_NONE);
		holding_enter(f, f->other_slot, ENT_TONE);
		return;
	case FSM_EV_FLASH:
		if (two_calls_conf_is(f)) {
			two_calls_conf(f);
			return;
		}
		hold_send(f, f->active_slot);
		pending_enter(f, FSM_OP_SWAP, f->active_slot, f->other_slot,
			      FSM_TWO_CALLS, FSM_TWO_CALLS);
		return;
	case FSM_EV_DIGIT:
		dtmf_send(f, (char)arg);
		return;
	case FSM_EV_CALL_INCOMING:
		reject_send(f, arg, FSM_REJECT_BUSY);
		return;
	case FSM_EV_CALL_FAILED:
	case FSM_EV_CALL_REMOTE_BYE:
		two_calls_leg_dead(f, slot);
		return;
	case FSM_EV_CW_NET:
		cw_net_run(f, arg);
		return;
	case FSM_EV_T_CW:
		/* the auto-stop budget of the alert; with no alert running the
		 * stale timer ignore stands */
		cw_net_stop(f);
		return;
	default:
		return;
	}
}

/* ------------------------------------------------------------------ CONF */

/*
 * The on-hook of CONF. The predicate is the one of TWO_CALLS, reused: it
 * reads the active and the other role, which this state fills, so an armed
 * consult transfer and the configured form both fire from the conference as
 * well.
 *
 * The mixer stops first in both branches. What each far end hears after the
 * off belongs to the backend and to the shell.
 */
static void conf_release(struct fsm *f)
{
	conf_stop(f);

	if (two_calls_xfer_is(f)) {
		two_calls_xfer(f);
		return;
	}

	legs_hangup(f);
	tone_set(f, HAL_TONE_NONE);
	idle_enter(f, ENT_TONE);
}

/*
 * The split of CONF: the leg the join resumed goes back on hold, and the
 * completion gives the two calls back with their pre-join roles. The user
 * split falls back to the conference when the hold fails; the mixer refusal
 * below has no conference to fall back to and clears the line instead.
 */
static void conf_split_start(struct fsm *f, enum fsm_state on_fail)
{
	unsigned int other = f->other_slot;
	unsigned int active = f->active_slot;

	hold_send(f, other);
	pending_enter(f, FSM_OP_CONF_SPLIT, other, active, FSM_TWO_CALLS,
		      on_fail);
}

/*
 * The FLASH cell of CONF, by mode. The drop stops the mixer before the
 * hangup, so no stale mix reaches the surviving call.
 */
static void conf_flash(struct fsm *f)
{
	unsigned int second = f->slot_second;
	unsigned int first = second == f->active_slot ? f->other_slot :
			     f->active_slot;

	switch (f->cfg.conf_mode) {
	case FSM_CONF_SPLIT:
		conf_split_start(f, FSM_CONF);
		return;
	case FSM_CONF_DROP:
		conf_stop(f);
		hangup_send(f, second);
		slot_release(f, second);
		connected_enter(f, first, 0);
		return;
	case FSM_CONF_OFF:
		/* unreachable: the TWO_CALLS guard joins no conference with the
		 * mode off, and a new view lands at IDLE or OOS only */
		return;
	}
}

/*
 * A leg of the conference died. The mixer stops whatever is left, because
 * one call is no conference. Two owned legs enter this state, so the branch
 * with no survivor is defensive.
 */
static void conf_leg_dead(struct fsm *f, unsigned int slot)
{
	if (!slot_owned(f, slot))
		return;

	conf_stop(f);
	slot_release(f, slot);

	if (f->active_slot != FSM_SLOT_NONE) {
		connected_enter(f, f->active_slot, 0);
		return;
	}
	if (f->other_slot != FSM_SLOT_NONE) {
		connected_enter(f, f->other_slot, 0);
		return;
	}

	clearing_enter(f, FSM_CLR_QUIET, 0);
}

static void st_conf(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	unsigned int slot = ev_slot(ev, arg);

	switch (ev) {
	case FSM_EV_RELEASE:
		conf_release(f);
		return;
	case FSM_EV_FLASH:
		conf_flash(f);
		return;
	case FSM_EV_DIGIT:
		/* one request for the conference: the shell fans it out to
		 * both legs */
		dtmf_send(f, (char)arg);
		return;
	case FSM_EV_CALL_INCOMING:
		reject_send(f, arg, FSM_REJECT_BUSY);
		return;
	case FSM_EV_CALL_FAILED:
	case FSM_EV_CALL_REMOTE_BYE:
		conf_leg_dead(f, slot);
		return;
	case FSM_EV_CALL_REINVITE_FAIL:
		/* the mixer refused the conference it had already joined, and
		 * the shell reports that as this event for an owned leg */
		if (!slot_owned(f, slot))
			return;
		f->conf_refused = true;
		conf_split_start(f, FSM_CLEARING);
		return;
	case FSM_EV_CW_NET:
		cw_net_run(f, arg);
		return;
	case FSM_EV_T_CW:
		/* the auto-stop budget of the alert, as in CONNECTED; with no
		 * alert running the stale timer ignore stands */
		cw_net_stop(f);
		return;
	default:
		return;
	}
}

/* --------------------------------------------------------------- PENDING */

/*
 * The record a completion chains to becomes the whole new operation. Its
 * other leg is the one the retiring record was not operating on, which is
 * what the rewrites of a far-end clear and the swap resume step both need.
 */
static void pending_chain_start(struct fsm *f)
{
	struct fsm_pending old = f->pending;
	unsigned int other;

	other = old.chain.slot == old.slot_op ? old.slot_other : old.slot_op;

	if (old.chain.op == FSM_OP_RESUME)
		resume_send(f, old.chain.slot);
	else
		hold_send(f, old.chain.slot);

	pending_enter(f, old.chain.op, old.chain.slot, other, old.chain.on_success,
		      old.chain.on_fail);
}

/*
 * A pending record names a plain state, so the CLEARING it can name carries
 * no class: every CLEARING target of a PENDING record is fastbusy.
 */
static void pending_next_enter(struct fsm *f, enum fsm_state next, unsigned int active,
			       unsigned int held, unsigned int done)
{
	switch (next) {
	case FSM_CONNECTED:
		connected_enter(f, active, done);
		return;
	/* the success of a transfer: the line owns nothing and the entry runs
	 * its hotline test again, so a hotline line dials at once */
	case FSM_OFFHOOK_DIAL:
		offhook_dial_enter(f, done);
		return;
	case FSM_TWO_CALLS:
		two_calls_enter(f, active, held, done);
		return;
	/* the held parameter carries the leg the join resumed, which the
	 * conference keeps as the other role and does not hold */
	case FSM_CONF:
		conf_enter(f, active, held, done);
		return;
	case FSM_HOLD_DIAL:
		hold_dial_enter(f, held, done);
		return;
	case FSM_CLEARING:
		held_set(f, held);
		clearing_enter(f, FSM_CLR_FASTBUSY, done);
		return;
	default:
		pending_clear(f);
		f->state = next;
		return;
	}
}

static void pending_swap_step2(struct fsm *f)
{
	struct fsm_pending old = f->pending;

	resume_send(f, old.slot_other);
	pending_enter(f, FSM_OP_RESUME, old.slot_other, old.slot_op, old.on_success,
		      FSM_PENDING);
	f->pending.chain_on_fail = true;
	f->pending.chain.op = FSM_OP_RESUME;
	f->pending.chain.slot = old.slot_op;
	f->pending.chain.on_success = FSM_TWO_CALLS;
	f->pending.chain.on_fail = FSM_CLEARING;
}

/*
 * A re-INVITE outcome cannot belong to a REFER exchange, so op XFER ignores
 * both of them. The guard comes before the chain step, because that
 * record's chain is the transfer's own fail arm.
 */
static bool pending_reinvite_foreign(const struct fsm *f)
{
	return f->pending.op == FSM_OP_XFER;
}

static void pending_reinvite_ok(struct fsm *f)
{
	struct fsm_pending p = f->pending;

	if (pending_reinvite_foreign(f))
		return;

	if (p.chain_on_success) {
		pending_chain_start(f);
		return;
	}

	switch (p.op) {
	case FSM_OP_HOLD:
		pending_next_enter(f, p.on_success, p.slot_op, p.slot_op, 0);
		return;
	case FSM_OP_RESUME:
		tone_set(f, HAL_TONE_NONE);
		pending_next_enter(f, p.on_success, p.slot_op, p.slot_other, ENT_TONE);
		return;
	case FSM_OP_SWAP:
		pending_swap_step2(f);
		return;
	case FSM_OP_CW_ANSWER:
		answer_send(f, p.slot_other);
		cw_alert_set(f, p.slot_other, false);
		pending_next_enter(f, p.on_success, p.slot_other, p.slot_op, 0);
		return;
	case FSM_OP_CONF:
		/*
		 * The mixer starts here and not in the flash cell. The op
		 * returns nothing, and its only refusal signal is an injected
		 * event that arrives behind the running cell. A mixer first
		 * order could therefore not tell a mixer refusal from the
		 * resume's own.
		 */
		conf_start(f);
		pending_next_enter(f, p.on_success, p.slot_other, p.slot_op, 0);
		return;
	case FSM_OP_CONF_SPLIT:
		conf_stop(f);
		pending_next_enter(f, p.on_success, p.slot_other, p.slot_op, 0);
		return;
	case FSM_OP_XFER:
		/* the guard above already returned; defensive only */
		return;
	}
}

/*
 * The failure of a split, by the state the record names. The user split falls
 * back to the conference; a record the death of the other leg rewrote falls
 * back to the survivor; the double fault of a refused mixer and a refused
 * re-hold clears the line, and that leg is hung up and released first, so no
 * leg is left without a role and without a hangup.
 */
static void pending_conf_split_fail(struct fsm *f, const struct fsm_pending *p)
{
	if (p->on_fail == FSM_CONF) {
		conf_enter(f, p->slot_other, p->slot_op, 0);
		return;
	}
	if (p->on_fail == FSM_CONNECTED) {
		pending_next_enter(f, p->on_fail, p->slot_op, FSM_SLOT_NONE, 0);
		return;
	}

	hangup_send(f, p->slot_op);
	slot_release(f, p->slot_op);
	pending_next_enter(f, p->on_fail, FSM_SLOT_NONE, p->slot_other, 0);
}

static void pending_reinvite_fail(struct fsm *f)
{
	struct fsm_pending p = f->pending;

	if (pending_reinvite_foreign(f))
		return;

	if (p.chain_on_fail) {
		pending_chain_start(f);
		return;
	}

	switch (p.op) {
	case FSM_OP_HOLD:
		tone_set(f, HAL_TONE_NONE);
		pending_next_enter(f, p.on_fail, p.slot_op, FSM_SLOT_NONE, ENT_TONE);
		return;
	case FSM_OP_RESUME:
		pending_next_enter(f, p.on_fail, p.slot_op, p.slot_op, 0);
		f->held_refused = held_has(f);
		return;
	case FSM_OP_SWAP:
		pending_next_enter(f, p.on_fail, p.slot_op, p.slot_other, 0);
		return;
	case FSM_OP_CW_ANSWER:
		if (p.slot_other != FSM_SLOT_NONE) {
			reject_send(f, p.slot_other, FSM_REJECT_BUSY);
			cw_alert_set(f, p.slot_other, false);
		}
		tone_set(f, HAL_TONE_NONE);
		pending_next_enter(f, p.on_fail, p.slot_op, FSM_SLOT_NONE, ENT_TONE);
		return;
	case FSM_OP_CONF:
		/* the resume was refused, so the leg is still held and the two
		 * calls stand exactly as they did before the flash */
		pending_next_enter(f, p.on_fail, p.slot_other, p.slot_op, 0);
		return;
	case FSM_OP_CONF_SPLIT:
		pending_conf_split_fail(f, &p);
		return;
	case FSM_OP_XFER:
		/* the guard above already returned; defensive only */
		return;
	}
}

/*
 * PENDING, the REFER outcome. The record is the only owner of one, and the
 * call.h contract makes an OK the last word on that slot: call.c has already
 * silenced the leg, which holds no UI name and no occupancy while its dialog
 * drains, so the cell takes the roles from that instant and waits for no
 * ending event.
 *
 * No tone runs between the op and the outcome, and none is played here
 * either, because a busy tone there reads as a failed transfer. The fail arm
 * resumes the held leg silently.
 */
static void pending_xfer_ok(struct fsm *f)
{
	struct fsm_pending p = f->pending;

	slot_release(f, p.slot_op);
	pending_next_enter(f, p.on_success, FSM_SLOT_NONE, FSM_SLOT_NONE, 0);
}

/* far-end clear in PENDING, the operated leg died: the re-INVITE died with
 * its dialog */
static void pending_op_leg_dead(struct fsm *f)
{
	struct fsm_pending p = f->pending;

	slot_release(f, p.slot_op);

	if (p.slot_other == FSM_SLOT_NONE) {
		tone_set(f, HAL_TONE_NONE);
		held_set(f, FSM_SLOT_NONE);
		clearing_enter(f, FSM_CLR_QUIET, ENT_TONE);
		return;
	}

	switch (p.op) {
	case FSM_OP_RESUME:
	case FSM_OP_SWAP:
		resume_send(f, p.slot_other);
		pending_enter(f, FSM_OP_RESUME, p.slot_other, FSM_SLOT_NONE,
			      FSM_CONNECTED, FSM_CLEARING);
		return;
	case FSM_OP_CW_ANSWER:
		cw_alert_set(f, p.slot_other, false);
		answer_send(f, p.slot_other);
		connected_enter(f, p.slot_other, 0);
		return;
	case FSM_OP_CONF:
		/* the leg the join was resuming is gone, and the mixer never
		 * started, so the other leg is simply the surviving call */
		connected_enter(f, p.slot_other, 0);
		return;
	case FSM_OP_CONF_SPLIT:
		conf_stop(f);
		connected_enter(f, p.slot_other, 0);
		return;
	case FSM_OP_HOLD:
	case FSM_OP_XFER:
		/* neither ever carries a second leg; defensive only */
		connected_enter(f, p.slot_other, 0);
		return;
	}
}

/* far-end clear in PENDING, the other leg died: the re-INVITE is still
 * outstanding */
static void pending_other_leg_dead(struct fsm *f)
{
	struct fsm_pending p = f->pending;

	slot_release(f, p.slot_other);
	f->pending.slot_other = FSM_SLOT_NONE;

	if (p.op == FSM_OP_CW_ANSWER)
		cw_alert_set(f, p.slot_other, false);

	/*
	 * The join lost the leg it was joining to, so the outstanding resume
	 * is all that is left of it: the record becomes that plain operation
	 * and the mixer never starts.
	 */
	if (p.op == FSM_OP_CONF)
		f->pending.op = FSM_OP_RESUME;

	/*
	 * The split lost the leg that stays active, so the mixer stops now.
	 * The completion the rewritten record names is a chain step, and a
	 * chain step runs no arm of the split.
	 */
	if (p.op == FSM_OP_CONF_SPLIT)
		conf_stop(f);

	if (p.op == FSM_OP_RESUME || p.op == FSM_OP_HOLD ||
	    p.op == FSM_OP_CONF) {
		f->pending.on_success = FSM_CONNECTED;
		f->pending.on_fail = FSM_CLEARING;
		f->pending.chain_on_success = false;
		f->pending.chain_on_fail = false;
		return;
	}

	f->pending.on_success = FSM_PENDING;
	f->pending.on_fail = FSM_CONNECTED;
	f->pending.chain_on_success = true;
	f->pending.chain_on_fail = false;
	f->pending.chain.op = FSM_OP_RESUME;
	f->pending.chain.slot = p.slot_op;
	f->pending.chain.on_success = FSM_CONNECTED;
	f->pending.chain.on_fail = FSM_CLEARING;
}

static void pending_leg_dead(struct fsm *f, unsigned int slot)
{
	if (slot == FSM_SLOT_NONE)
		return;
	if (slot == f->pending.slot_op) {
		pending_op_leg_dead(f);
		return;
	}
	if (slot != f->pending.slot_other)
		return;

	pending_other_leg_dead(f);
}

static void st_pending(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	unsigned int slot = ev_slot(ev, arg);

	switch (ev) {
	case FSM_EV_RELEASE:
		hangup_send(f, f->pending.slot_op);
		if (f->pending.slot_other != FSM_SLOT_NONE)
			hangup_send(f, f->pending.slot_other);
		tone_set(f, HAL_TONE_NONE);
		idle_enter(f, ENT_TONE);
		return;
	case FSM_EV_CALL_INCOMING:
		reject_send(f, arg, FSM_REJECT_BUSY);
		return;
	case FSM_EV_CALL_FAILED:
	case FSM_EV_CALL_REMOTE_BYE:
		pending_leg_dead(f, slot);
		return;
	case FSM_EV_CALL_REMOTE_CANCEL:
		if (f->pending.op != FSM_OP_CW_ANSWER || slot != f->pending.slot_other)
			return;
		pending_other_leg_dead(f);
		return;
	case FSM_EV_CALL_REINVITE_OK:
		pending_reinvite_ok(f);
		return;
	case FSM_EV_CALL_REINVITE_FAIL:
		pending_reinvite_fail(f);
		return;
	case FSM_EV_CALL_XFER_OK:
		if (f->pending.op != FSM_OP_XFER || slot != f->pending.slot_op)
			return;
		pending_xfer_ok(f);
		return;
	case FSM_EV_CALL_XFER_FAIL:
		if (f->pending.op != FSM_OP_XFER || slot != f->pending.slot_op)
			return;
		pending_chain_start(f);
		return;
	default:
		return;
	}
}

/* --------------------------------------------------------------- HOLDING */

static void st_holding(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	unsigned int slot = ev_slot(ev, arg);

	switch (ev) {
	case FSM_EV_SEIZE:
		bell_stop(f);
		resume_send(f, f->other_slot);
		pending_enter(f, FSM_OP_RESUME, f->other_slot, FSM_SLOT_NONE,
			      FSM_CONNECTED, FSM_CLEARING);
		return;
	case FSM_EV_CALL_INCOMING:
		/* a refused offer leaves the held leg and its recall ring alone:
		 * this line is still exactly where it was */
		if (incoming_refused(f, arg))
			return;
		slot_inbound_set(f, arg, true);
		/* the held leg came first, so this offer is the second call, and
		 * the clear comes first for the reason CONNECTED states */
		slot_second_clear(f);
		f->slot_second = arg;
		bell_stop(f);
		ring180_send(f, arg);
		bell_start(f, arg);
		ringing_enter(f, arg);
		return;
	case FSM_EV_CALL_FAILED:
	case FSM_EV_CALL_REMOTE_BYE:
		if (!held_is(f, slot))
			return;
		slot_release(f, slot);
		bell_stop(f);
		idle_enter(f, ENT_BELL);
		return;
	default:
		return;
	}
}

/* -------------------------------------------------------------- CLEARING */

static void clearing_flash(struct fsm *f)
{
	/* The line cannot carry a dial at this flash, so a dial tone here
	 * would only lead to a refused dial; the chain and its timer run on. */
	if (!held_has(f) && !f->dial_ready)
		return;

	timer_stop(f, &f->t_clearing);

	if (!held_has(f)) {
		offhook_dial_enter(f, 0);
		return;
	}

	held_resume(f);
}

/*
 * The class the chain reached says nothing about the held call, so every phase
 * parks it with the recall ring. A leg whose resume the far end refused is hung
 * up instead: the pickup would draw the same refusal, and the subscriber would
 * have no way to end the call.
 */
static void clearing_release(struct fsm *f)
{
	if (held_has(f) && !f->held_refused) {
		holding_enter(f, f->other_slot, ENT_TONE);
		return;
	}

	if (held_has(f))
		hangup_send(f, f->other_slot);
	idle_enter(f, ENT_TONE);
}

static void st_clearing(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	unsigned int slot = ev_slot(ev, arg);

	switch (ev) {
	case FSM_EV_RELEASE:
		tone_set(f, HAL_TONE_NONE);
		timer_stop(f, &f->t_clearing);
		clearing_release(f);
		return;
	case FSM_EV_FLASH:
		clearing_flash(f);
		return;
	case FSM_EV_CALL_INCOMING:
		reject_send(f, arg, FSM_REJECT_BUSY);
		return;
	case FSM_EV_CALL_FAILED:
	case FSM_EV_CALL_REMOTE_BYE:
		if (!held_is(f, slot))
			return;
		slot_release(f, slot);
		return;
	case FSM_EV_T_CLEARING:
		if (f->clearing != FSM_CLR_BUSY && f->clearing != FSM_CLR_QUIET)
			return;
		clearing_enter(f, FSM_CLR_FASTBUSY, 0);
		return;
	case FSM_EV_T_ROH:
		if (f->clearing != FSM_CLR_FASTBUSY &&
		    f->clearing != FSM_CLR_UNOBTAINABLE)
			return;
		tone_set(f, HAL_TONE_NONE);
		roh_enter(f);
		return;
	default:
		return;
	}
}

/* ------------------------------------------------------------------- ROH */

static void st_roh(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	unsigned int slot = ev_slot(ev, arg);

	switch (ev) {
	case FSM_EV_RELEASE:
		tone_set(f, HAL_TONE_NONE);
		timer_stop(f, &f->t_clearing);
		clearing_release(f);
		return;
	case FSM_EV_CALL_INCOMING:
		reject_send(f, arg, FSM_REJECT_BUSY);
		return;
	case FSM_EV_CALL_FAILED:
	case FSM_EV_CALL_REMOTE_BYE:
		if (!held_is(f, slot))
			return;
		slot_release(f, slot);
		return;
	case FSM_EV_FLASH:
		if (!held_has(f))
			return;
		timer_stop(f, &f->t_clearing);
		held_resume(f);
		return;
	case FSM_EV_T_ROH:
		tone_set(f, HAL_TONE_NONE);
		silent_enter(f);
		return;
	default:
		return;
	}
}

/* ---------------------------------------------------------------- SILENT */

/*
 * The quiet off-hook rest after the howler window. The tone
 * is already stopped on entry, so RELEASE stops nothing before it leaves.
 * Incoming calls still meet the busy line, the death of a held leg still
 * releases its slot, and a flash or an on-hook returns the subscriber to a
 * held leg.
 */
static void st_silent(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	unsigned int slot = ev_slot(ev, arg);

	switch (ev) {
	case FSM_EV_RELEASE:
		clearing_release(f);
		return;
	case FSM_EV_FLASH:
		if (!held_has(f))
			return;
		held_resume(f);
		return;
	case FSM_EV_CALL_INCOMING:
		reject_send(f, arg, FSM_REJECT_BUSY);
		return;
	case FSM_EV_CALL_FAILED:
	case FSM_EV_CALL_REMOTE_BYE:
		if (!held_is(f, slot))
			return;
		slot_release(f, slot);
		return;
	default:
		return;
	}
}

/* ------------------------------------------------------------------- OOS */

static void st_oos(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	switch (ev) {
	case FSM_EV_SEIZE:
		tone_set(f, HAL_TONE_REORDER);
		return;
	case FSM_EV_RELEASE:
		tone_set(f, HAL_TONE_NONE);
		return;
	default:
		return;
	}
}

/* ---------------------------------------------------------- XFER_COLLECT */

/*
 * PENDING{XFER}: the REFER is outstanding on the leg it names, and the fail
 * arm is the chained resume of that same leg. The record carries the chain
 * exactly as the swap resume step does, so one mechanism starts both.
 */
static void xfer_pending_enter(struct fsm *f, unsigned int held)
{
	pending_enter(f, FSM_OP_XFER, held, FSM_SLOT_NONE, FSM_OFFHOOK_DIAL,
		      FSM_PENDING);
	f->pending.chain_on_fail = true;
	f->pending.chain.op = FSM_OP_RESUME;
	f->pending.chain.slot = held;
	f->pending.chain.on_success = FSM_CONNECTED;
	f->pending.chain.on_fail = FSM_CLEARING;
}

/*
 * The target is complete. The caller has already stopped the recall tone, and
 * nothing plays between here and the outcome. PENDING changes no tone, so the
 * fast busy of a refused star code stays off this path: a busy tone there
 * reads as a failed transfer.
 *
 * The consult row does not refer anything yet. It arms the transfer for the
 * next on-hook and dials the target as an ordinary second call, which is the
 * arm-before-call order.
 */
static void xfer_complete(struct fsm *f)
{
	unsigned int held = f->other_slot;

	digits_markers_strip(f);

	if (f->xfer_act == FEATURE_ACT_XFER_CONSULT) {
		f->xfer_armed = true;
		dial_start(f, f->digits);
		calling_enter(f, ENT_TONE);
		return;
	}

	xfer_send(f, held, f->xfer_act, f->digits);
	xfer_pending_enter(f, held);
}

/*
 * The DIGIT cell. It is the HOLD_DIAL verdict shape on a bare target, with
 * the SPRE_COLLECT overflow rule: a target that does not fit refuses the
 * transfer instead of referring a cut number.
 */
static void xfer_digit(struct fsm *f, char digit)
{
	tone_set(f, HAL_TONE_NONE);

	if (digit_pound_delim(f, digit)) {
		timer_stop(f, &f->t_digit);
		xfer_complete(f);
		return;
	}

	if (f->ndigits >= FSM_DIGITS_MAX) {
		timer_stop(f, &f->t_digit);
		digits_clear(f);
		clearing_enter(f, FSM_CLR_FASTBUSY, 0);
		return;
	}

	digits_append(f, digit);

	switch (digits_verdict(f)) {
	case DP_ACT_DIAL:
		timer_stop(f, &f->t_digit);
		xfer_complete(f);
		return;
	case DP_ACT_WAIT_SHORT:
		timer_arm(f, &f->t_digit, f->cfg.interdigit_short_ms);
		return;
	case DP_ACT_WAIT_LONG:
		timer_arm(f, &f->t_digit, f->cfg.interdigit_long_ms);
		return;
	case DP_ACT_REORDER:
		timer_stop(f, &f->t_digit);
		digits_clear(f);
		clearing_enter(f, FSM_CLR_FASTBUSY, 0);
		return;
	}
}

/* an abandoned target ends where every abandoned dial string ends */
static void xfer_interdigit(struct fsm *f)
{
	enum dp_action act;

	tone_set(f, HAL_TONE_NONE);

	act = digits_expire(f);
	if (act == DP_ACT_DIAL) {
		xfer_complete(f);
		return;
	}
	if (digits_wait_arm(f, act))
		return;

	digits_clear(f);
	clearing_enter(f, FSM_CLR_FASTBUSY, 0);
}

/*
 * The subject of the transfer is gone, so the digits are a target for
 * nothing. Unlike the dial string of HOLD_DIAL they cannot be dialed on: the
 * whole gesture was about that leg.
 */
static void xfer_leg_dead(struct fsm *f, unsigned int slot, enum fsm_clearing cls)
{
	slot_release(f, slot);
	timer_stop(f, &f->t_digit);
	digits_clear(f);
	tone_set(f, HAL_TONE_NONE);
	clearing_enter(f, cls, clearing_done_tone(cls));
}

static void st_xfer_collect(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	unsigned int slot = ev_slot(ev, arg);

	switch (ev) {
	case FSM_EV_RELEASE:
		tone_set(f, HAL_TONE_NONE);
		timer_stop(f, &f->t_digit);
		digits_clear(f);
		holding_enter(f, f->other_slot, ENT_TONE);
		return;
	case FSM_EV_FLASH:
		tone_set(f, HAL_TONE_NONE);
		timer_stop(f, &f->t_digit);
		digits_clear(f);
		resume_send(f, f->other_slot);
		pending_enter(f, FSM_OP_RESUME, f->other_slot, FSM_SLOT_NONE,
			      FSM_CONNECTED, FSM_CLEARING);
		return;
	case FSM_EV_DIGIT:
		xfer_digit(f, (char)arg);
		return;
	case FSM_EV_CALL_INCOMING:
		reject_send(f, arg, FSM_REJECT_BUSY);
		return;
	case FSM_EV_CALL_FAILED:
		if (!held_is(f, slot))
			return;
		xfer_leg_dead(f, slot, FSM_CLR_FASTBUSY);
		return;
	case FSM_EV_CALL_REMOTE_BYE:
		if (!held_is(f, slot))
			return;
		xfer_leg_dead(f, slot, FSM_CLR_QUIET);
		return;
	case FSM_EV_T_INTERDIGIT:
		xfer_interdigit(f);
		return;
	default:
		return;
	}
}

/* ----------------------------------------------------------------- timers */

/* HOLD_DIAL appends a digit before every re-arm, so an empty buffer there
 * means the object still carries the first-digit window of the entry */
static enum fsm_ev digit_timer_ev(const struct fsm *f)
{
	if (f->state == FSM_HOLD_DIAL && f->ndigits == 0)
		return FSM_EV_T_START_DIGIT;
	if (f->state == FSM_COLLECTING || f->state == FSM_HOLD_DIAL ||
	    f->state == FSM_SPRE_COLLECT || f->state == FSM_XFER_COLLECT)
		return FSM_EV_T_INTERDIGIT;
	if (f->state == FSM_OFFHOOK_DIAL && f->cfg.warmline)
		return FSM_EV_T_WARMLINE;

	return FSM_EV_T_START_DIGIT;
}

/* RINGING is the one state that gives this object to the ring-no-answer
 * window; every other state that arms it means the call waiting budget */
static enum fsm_ev cw_timer_ev(const struct fsm *f)
{
	if (f->state == FSM_RINGING)
		return FSM_EV_T_RING_NOANSWER;

	return FSM_EV_T_CW;
}

static enum fsm_ev clearing_timer_ev(const struct fsm *f)
{
	if (f->state == FSM_ROH)
		return FSM_EV_T_ROH;
	if (f->state != FSM_CLEARING)
		return FSM_EV_T_CLEARING;
	if (f->clearing == FSM_CLR_FASTBUSY || f->clearing == FSM_CLR_UNOBTAINABLE)
		return FSM_EV_T_ROH;

	return FSM_EV_T_CLEARING;
}

static void t_fire(struct sip_timer *t)
{
	struct fsm *f = t->owner;
	enum fsm_ev ev;

	if (t == &f->t_digit)
		ev = digit_timer_ev(f);
	else if (t == &f->t_cw)
		ev = cw_timer_ev(f);
	else
		ev = clearing_timer_ev(f);

	fsm_event_push(f, ev, 0);
}

/* ------------------------------------------------------------------- API */

void fsm_init(struct fsm *f, unsigned int line, struct sip_txn_env *env,
	      const struct fsm_cfg *cfg, const struct fsm_ops *ops, void *priv)
{
	memset(f, 0, sizeof(*f));
	f->line = line;
	f->env = env;
	f->cfg = *cfg;
	f->ops = ops;
	f->priv = priv;
	f->state = FSM_IDLE;
	f->clearing = FSM_CLR_BUSY;
	f->active_slot = FSM_SLOT_NONE;
	f->other_slot = FSM_SLOT_NONE;
	f->dial_ready = true;
	slot_second_clear(f);
	pending_clear(f);

	sip_timer_init(&f->t_digit, f, SIP_TIMER_NONE, t_fire);
	sip_timer_init(&f->t_cw, f, SIP_TIMER_NONE, t_fire);
	sip_timer_init(&f->t_clearing, f, SIP_TIMER_NONE, t_fire);
}

static void state_moved_note(struct fsm *f, enum fsm_state before)
{
	if (f->state != before && f->ops->state_changed)
		f->ops->state_changed(f->priv, before, f->state);
}

/* positional in enum order, so a state with no entry fails the assertion */
static void (*const st_handlers[])(struct fsm *f, enum fsm_ev ev,
				   uint32_t arg) = {
	st_idle, st_offhook_dial, st_collecting, st_spre_collect, st_outbound,
	st_outbound, st_connected, st_ringing, st_cw_offered, st_hold_dial,
	st_two_calls, st_conf, st_pending, st_holding, st_clearing, st_roh,
	st_oos, st_xfer_collect, st_silent,
};

_Static_assert(sizeof(st_handlers) / sizeof(st_handlers[0]) ==
	       FSM_SILENT + 1, "one handler per state");

void fsm_event_push(struct fsm *f, enum fsm_ev ev, uint32_t arg)
{
	enum fsm_state before = f->state;

	if (ev >= FSM_EV_COUNT)
		return;

	if ((unsigned int)f->state <
	    sizeof(st_handlers) / sizeof(st_handlers[0]))
		st_handlers[f->state](f, ev, arg);

	cw_net_settle(f);
	xfer_armed_settle(f);
	conf_settle(f);
	state_moved_note(f, before);
}

/*
 * The waiting leg of an outstanding call waiting offer. CW_OFFERED holds it
 * as the other role, and a CW_ANSWER operation holds it as slot_other, which
 * the far-end clear rewrite clears to FSM_SLOT_NONE once that leg dies.
 */
static unsigned int cw_waiting_get(const struct fsm *f)
{
	if (f->state == FSM_CW_OFFERED)
		return f->other_slot;
	if (f->state == FSM_PENDING && f->pending.op == FSM_OP_CW_ANSWER)
		return f->pending.slot_other;

	return FSM_SLOT_NONE;
}

/*
 * The OOS entry, run by sip/line.c and never by an event. The roles mirror
 * the pending record while an operation is outstanding, so this hangs up both
 * legs of a PENDING machine too. Both alerts stop before the hangups, which is
 * the order every CW_OFFERED cell uses.
 */
void fsm_oos_force(struct fsm *f)
{
	enum fsm_state before = f->state;
	unsigned int waiting;

	if (f->state == FSM_OOS)
		return;

	waiting = cw_waiting_get(f);
	if (waiting != FSM_SLOT_NONE)
		cw_alert_set(f, waiting, false);
	cw_net_stop(f);
	conf_stop(f);

	legs_hangup(f);
	tone_set(f, HAL_TONE_NONE);
	bell_stop(f);
	timers_stop_all(f);

	digits_clear(f);
	pending_clear(f);
	/* the settle below rides an EVENT and this entry is not one, so the
	 * armed consult transfer is dropped here by hand */
	f->xfer_armed = false;
	slot_second_clear(f);
	f->active_slot = FSM_SLOT_NONE;
	held_set(f, FSM_SLOT_NONE);
	f->state = FSM_OOS;
	state_moved_note(f, before);
}

void fsm_oos_clear(struct fsm *f)
{
	enum fsm_state before = f->state;

	if (f->state != FSM_OOS)
		return;

	idle_enter(f, 0);
	state_moved_note(f, before);
}

void fsm_timers_stop(struct fsm *f)
{
	timers_stop_all(f);
}

bool fsm_cfg_update(struct fsm *f, const struct fsm_cfg *cfg)
{
	if (f->state != FSM_IDLE && f->state != FSM_OOS)
		return false;

	f->cfg = *cfg;

	return true;
}

void fsm_call_waiting_set(struct fsm *f, bool on)
{
	f->cfg.call_waiting = on;
}

void fsm_mwi_set(struct fsm *f, bool on)
{
	f->cfg.mwi = on;
}

void fsm_dial_ready_set(struct fsm *f, bool ready)
{
	f->dial_ready = ready;
}

void fsm_slot_inbound_set(struct fsm *f, unsigned int slot, bool inbound)
{
	slot_inbound_set(f, slot, inbound);
}

/*
 * The flag moves in both, and the timer only in RINGING. The waiting call of
 * CW_OFFERED measures its own offer with the same timer, and a bell that
 * starts outside an alerting state belongs to the recall ring of HOLDING,
 * which no forward watches.
 *
 * The hold arrives from inside the RINGING entry, where the state is still
 * the old one and no window is armed yet. ring_noanswer_arm() reads the flag
 * a moment later and arms nothing, which is what covers that order.
 */
void fsm_ring_held(struct fsm *f)
{
	f->ring_held = true;

	if (f->state != FSM_RINGING)
		return;

	timer_stop(f, &f->t_cw);
}

void fsm_ring_started(struct fsm *f)
{
	f->ring_held = false;

	if (f->state != FSM_RINGING)
		return;

	ring_noanswer_arm(f);
}

/*
 * The bell never rings for this call, so the caller is refused instead of left
 * on a ringback that leads nowhere. The 480 goes through reject_send() and is
 * therefore not redirected to the busy forward, which is the rule the
 * unanswered waiting call of CW_OFFERED already follows.
 */
void fsm_ring_failed(struct fsm *f)
{
	unsigned int slot = f->active_slot;

	f->ring_held = false;

	if (f->state != FSM_RINGING)
		return;

	bell_stop(f);
	timer_stop(f, &f->t_cw);
	reject_send(f, slot, FSM_REJECT_NOANSWER);
	ringing_leg_end(f, slot);
}
