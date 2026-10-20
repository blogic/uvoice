// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The SIP half of the line shell: the machine of each line (fsm.c), its
 * vtable against the leg layer (call.c) and the core, the callbacks of the
 * leg layer, the fax and T.38 tone policy, the forward disconnect policy and
 * the derivation of the machine view from the config (proto/sip/line.h).
 * The core half is src/line/line.c, and this half reaches it through
 * sip_host alone.
 *
 * Re-entrancy. fsm.c runs one transition at a time and must never be entered
 * from inside its own action frame. Two paths lead there: call.c reports a
 * synchronous failure inside call_dial, and an FSM timer expiry calls
 * fsm_event_push() from fsm.c itself, so the shell has no frame of its own
 * around it. This file therefore counts FSM frames in fsm_depth:
 * line_ev_deliver() raises it around its own push, and every vtable op that
 * reaches call.c raises it too, which covers the timer path. An event that
 * arrives while the count is above zero lands in the per line deferred ring
 * instead and runs after the frame closes. The synchronous drain at the end
 * of line_ev_deliver() covers a shell-driven frame; the 0 ms uloop timeout
 * armed by line_defer_push() covers a timer-driven one.
 *
 * HAL events do not pump from inside an FSM frame. Thus, the one-event
 * duration window cannot mis-attribute data across the deferred ring. If a
 * later change nests the pump, revisit the digit-match guard.
 *
 * Deferred ring depth. A single transition emits at most two leg commands
 * (legs_hangup on both roles) plus one alert stop, so at most three callbacks
 * can fire inside one frame. LINE_DEFER_MAX is 8, more than double the worst
 * case; an overflow is counted and traced rather than silently dropped.
 *
 * Config. sip_lines_init() compiles one dial plan per line and derives one
 * struct fsm_cfg from it. A reload compiles into the staging copy and applies
 * it through fsm_cfg_update(). The fsm.h interface is fixed and refuses the
 * update outside IDLE and OOS; a refused view stays pending and every later
 * drain retries it, so the machine picks it up at the next rest state.
 */
#include <stdio.h>
#include <string.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubox/ulog.h>
#include <libubox/utils.h>

#include "dialplan.h"
#include "posix.h"
#include "proto/proto.h"
#include "proto/proto_trace.h"
#include "proto/sip/account.h"
#include "proto/sip/call.h"
#include "proto/sip/features.h"
#include "proto/sip/fsm.h"
#include "proto/sip/line.h"
#include "proto/sip/txn.h"
#include "str.h"

/* see the re-entrancy note above */
#define LINE_DEFER_MAX		8

/*
 * Forward disconnect, from CONNECTED and CLEARING. The delayed form runs
 * LINE_FWD_DISC_HOLD_MS into a busy, fast busy or unobtainable clearing. The
 * immediate form runs on the far-end clear: the peer BYE of an answered leg,
 * which call.c reports through call_cb.remote_bye and cb_remote_bye()
 * narrows. The core carries the signal itself (proto/proto.h
 * fwd_disc_signal).
 */
#define LINE_FWD_DISC_HOLD_MS	15000

/* RFC 4733 uses 16-bit timestamp units. At 8000 Hz, its 8.19 s ceiling has
 * an 8000 ms whole-second floor. */
#define LINE_DTMF_DURATION_MAX_MS	8000

/* One trace line through the host table, in domain dom. */
#define LTRACE(dom, fmt, ...)	PROTO_TRACE(sip_host, dom, fmt, ##__VA_ARGS__)

struct line_defer {
	uint32_t arg;
	uint8_t ev;
};

/*
 * What the leg layer reported about one caller. The strings hold the identity
 * the SIP layer parsed even when the caller withheld it (call.h): the flags,
 * not the emptiness of a string, decide what the subscriber is shown.
 */
struct line_cid {
	char number[HAL_CID_STR_MAX];
	char name[HAL_CID_STR_MAX];
	bool number_private;
	bool name_private;
	uint8_t ring_id;		/* what the offer carried, 0 = none */
	uint8_t ring_template;		/* what the bell plays */
	uint8_t cw_variant;		/* the call-waiting tone variant, 0 = plain */
};

/*
 * The caller the network call waiting INFO named. It belongs to the line and
 * not to a slot: the switch holds the waiting call, so there is no leg here
 * to hang it on, and the machine that drives the alert never sees the
 * strings. It is dropped on the off edge, at IDLE and by a reload.
 */
struct line_cw_net {
	char number[HAL_CID_STR_MAX];
	char name[HAL_CID_STR_MAX];
	uint8_t tone;
};

/*
 * The deferral behind the end row of a plain answer tone (call.h
 * call_fax_exit, CALL_FAX_EXIT_DEFER_MS). One per physical leg, because the
 * fax latches beside it are the leg's.
 *
 * It carries its own line, stream and leg handle rather than reading them at
 * expiry. The handle is what makes the answer safe: a transfer moves a UI
 * name between two physical legs and a release bumps the generation, so a slot
 * read 1.5 s later can name a call this tone never belonged to.
 */
struct line_fax_exit {
	struct uloop_timeout timer;
	uint32_t handle;
	unsigned int line;
	unsigned int stream;
};

/*
 * The SIP half of one line. The core half (line/line.c struct line) holds the
 * hardware state; this one holds the machine, the views it runs on and the
 * records of the calls it carries.
 */
struct sip_line {
	struct fsm fsm;
	struct dialplan dp;		/* the map fsm.cfg.dp points at */
	struct dialplan dp_new;		/* staged by a reload, applied at IDLE */
	/* the codes fsm.cfg.feature_codes points at, the machine's own view of
	 * the channel; it lands with the core copy (line_cfg_apply) */
	struct cfg_channel chan;
	struct cfg_channel chan_new;	/* staged by a reload, applied at IDLE */
	struct feature_state features;	/* runtime star code state, never persisted */
	struct fsm_cfg fcfg;
	struct fsm_cfg fcfg_new;
	struct line_cid cid[CALL_SLOTS];
	struct line_cw_net cw_net;
	struct uloop_timeout drain;
	/* the delayed forward disconnect window */
	struct uloop_timeout fwd_disc_hold;
	/* the wait behind the end row of a plain answer tone, one per leg */
	struct line_fax_exit fax_exit[CALL_SLOTS];
	struct line_defer q[LINE_DEFER_MAX];
	unsigned int index;
	unsigned int fsm_depth;
	unsigned int deferred_lost;
	uint8_t qhead;
	uint8_t qcount;
	uint8_t fwd_disc_class;		/* the clearing class fwd_disc_hold belongs to */
	uint16_t digit_detected_ms;
	char digit_detected;
	bool digit_detected_valid;
	bool cfg_pending;
	/*
	 * The far-end-clear verdict in the two steps of its life. pending is
	 * the slot plus one, taken when call.c reports the BYE and held until
	 * the event it belongs to actually runs; clear says that event ran, so
	 * only the settle behind it can read it.
	 */
	uint8_t fwd_disc_pending;
	/*
	 * The fax latch, one per leg. A fax answer emits its CED for seconds
	 * and the detector reports every onset, so without this the same call
	 * would be switched, and re-INVITEd, again and again. cb_ended() clears
	 * the slot, which is what re-arms the next call on it, and a return to
	 * voice clears it on the leg it runs on (call.h call_fax_exit): a
	 * genuine answer tone behind a false one converts the same call again.
	 *
	 * Both latches are keyed by the leg and not by the UI name (call.h, the
	 * two-name contract). The tone that asks for the switch names the
	 * machine's active slot and the record edge that re-arms it names the
	 * leg's own, so a transferred call would latch one entry and clear
	 * another if the two were not joined by slot_stream().
	 */
	bool fax_switched[CALL_SLOTS];
	/*
	 * The second stage of the same trigger, with its own latch. The V.21
	 * preamble also repeats while the two terminals negotiate, and the T.38
	 * upgrade must be attempted once per leg: a peer that refused one image
	 * offer would otherwise be re-offered one on every preamble burst. It
	 * is taken behind the latch above and never instead of it, because a
	 * leg with no passthrough starts no upgrade at all (t38_tone_event, the
	 * two rules), and it is spent on every answer but CALL_T38_BUSY: a leg
	 * that is still running its own upspeed had no offer made for it.
	 */
	bool t38_switched[CALL_SLOTS];
	/*
	 * RFC 3960 3.2, one per leg. The answer of an 18x started this leg's
	 * early stream and the far end has not sent on it yet, so the local
	 * ringback still plays; the first packet ends it. It is keyed by the
	 * leg and not by the UI name for the reason the fax latches above
	 * give: the report that clears it names a stream, the machine names
	 * a UI slot, and slot_stream() and slot_ui_of() are the only join.
	 */
	bool early_rx_wait[CALL_SLOTS];
	bool fwd_disc_clear;
	bool fwd_disc_fired;		/* this clearing episode already reversed */
	bool fwd_disc_avail;		/* the backend carries HAL_FEAT_POLARITY */
	bool conf_avail;		/* the backend carries HAL_FEAT_CONF */
};

static struct sip_line sip_lines[CFG_CHANNEL_MAX];
/* the timer environment of every machine: lines outlive accounts, so an
 * account rebuild on reload must not cancel a ring or a clearing chain */
static struct sip_txn_env line_env;

const struct proto_host *sip_host;

/*
 * Whether the backend capability the derive reads is the final one. The
 * first ops->dsp_ready after the init carries the capability read before the
 * DSP starts, so it is provisional, and every later one is final
 * (proto/proto.h dsp_ready). The conference fault waits for it, so the fault
 * names the capability of a started DSP.
 */
static bool sip_cap_final;
static unsigned int sip_dsp_ready_calls;

/* The graceful stop began (ops->shutdown_begin): the daemon is leaving. */
static bool sip_stopping;

static void line_queue_drain(struct sip_line *l);
static void flash_derive(struct sip_line *l, const struct cfg_channel *ch,
			 struct fsm_cfg *out);
static void line_fault_report(const struct sip_line *l, bool loud,
			      const char *text);
static uint32_t sip_line_flags_of(const struct sip_line *l);

static struct sip_line *sip_line_get(unsigned int index)
{
	if (index >= CFG_CHANNEL_MAX)
		return NULL;

	return &sip_lines[index];
}

/* A configuration or hardware fault of the line, through the core. */
static void line_fault_report(const struct sip_line *l, bool loud,
			      const char *text)
{
	sip_host->line_fault(sip_host->ctx, l->index, loud, text);
}

static void cw_net_clear(struct sip_line *l)
{
	memset(&l->cw_net, 0, sizeof(l->cw_net));
}

/*
 * Which call waiting path presents an offer. call_waiting_local 1 selects the
 * local offer in CONNECTED, 0 selects the network INFO alert in CONNECTED and
 * TWO_CALLS, and an unset option keeps both paths.
 *
 * fsm.h owns the enum, because the machine holds the two guards the mode
 * gates: the CALL_INCOMING admission in CONNECTED and the CW_NET play gate.
 * The shell reads the mode from the line's own channel copy, so both views
 * stage together and a reload cannot move the alert source under an alert
 * that is already playing.
 */
static enum fsm_cw_mode cw_mode_of(const struct cfg_channel *ch)
{
	if (!ch->call_waiting_local_set)
		return FSM_CW_BOTH;

	return ch->call_waiting_local ? FSM_CW_LOCAL : FSM_CW_NETWORK;
}

/*
 * The config arms the forward disconnect and the backend can carry it. The
 * availability is resolved at the derive (fwd_disc_avail_take), the way the
 * core resolves its own, so the policy below asks the backend nothing.
 */
static bool fwd_disc_on(const struct sip_line *l)
{
	return l->fwd_disc_avail && cfg_fwd_disc_on(&l->chan);
}

/* false when a guard or the backend dropped the signal, which the
 * once-per-episode rule reads */
static bool fwd_disc_signal(const struct sip_line *l)
{
	return sip_host->fwd_disc_signal(sip_host->ctx, l->index);
}

/* the three classes the delayed form watches: a quiet clearing is the
 * far-end clear, which the immediate form already took */
static bool fwd_disc_delayed_class(enum fsm_clearing cls)
{
	return cls == FSM_CLR_BUSY || cls == FSM_CLR_FASTBUSY ||
	       cls == FSM_CLR_UNOBTAINABLE;
}

/*
 * Either arm marks the episode. Without the mark on the delayed arm, a later
 * event delivered into the same CLEARING would find fwd_disc_fired clear,
 * re-arm the window through fwd_disc_hold_take() and signal the same failed
 * call a second time. The one re-arm left is for a chain that has not
 * signaled at all.
 */
static void fwd_disc_hold_expire(struct uloop_timeout *t)
{
	struct sip_line *l = container_of(t, struct sip_line, fwd_disc_hold);

	if (fwd_disc_signal(l))
		l->fwd_disc_fired = true;
}

/*
 * A class change re-arms. The busy clearing chains to the fast busy one at
 * busy_tone_timer_sec, and each clearing class gets a window of its own, so
 * the second treatment gets a second reversal rather than inheriting the
 * first one's expiry.
 */
static void fwd_disc_hold_take(struct sip_line *l)
{
	if (l->fwd_disc_hold.pending && l->fwd_disc_class == (uint8_t)l->fsm.clearing)
		return;

	l->fwd_disc_class = (uint8_t)l->fsm.clearing;
	l->fwd_disc_hold.cb = fwd_disc_hold_expire;
	uloop_timeout_set(&l->fwd_disc_hold, LINE_FWD_DISC_HOLD_MS);
}

/* HOLDING is on hook too: the on-hook that parks a held call ends the
 * clearing episode it left, as an on-hook into IDLE does */
static bool fwd_disc_at_rest(const struct sip_line *l)
{
	return l->fsm.state == FSM_IDLE || l->fsm.state == FSM_OOS ||
	       l->fsm.state == FSM_HOLDING;
}

/*
 * The whole edge, run wherever the shell brings a line to rest. Reading the
 * state here rather than acting inside a callback is what makes the immediate
 * form correct: the machine may keep the subscriber on a call when the leg
 * the far end ended was not the last one, and only the CLEARING it lands in
 * says the line is really announcing the clear.
 */
static void fwd_disc_settle(struct sip_line *l)
{
	bool clear = l->fwd_disc_clear;
	bool clearing = l->fsm.state == FSM_CLEARING;

	l->fwd_disc_clear = false;

	/* the rest state ends every reversal, whatever armed it and whether or
	 * not the feature is still configured, and it is where one clearing
	 * episode ends and the next one may begin */
	if (fwd_disc_at_rest(l)) {
		uloop_timeout_cancel(&l->fwd_disc_hold);
		sip_host->fwd_disc_abort(sip_host->ctx, l->index);
		l->fwd_disc_fired = false;
		l->fwd_disc_pending = 0;
		return;
	}

	if (!fwd_disc_on(l))
		return;

	/*
	 * Once per clearing episode. A far-end clear reverses immediately and
	 * lands in the quiet class, which chains to the fast busy one 2 s
	 * later; without this the same call would be announced twice, the
	 * second time 15 s after the subscriber already got the signal. The
	 * busy to fast busy chain that never reversed keeps its re-arm, the
	 * per-class window of fwd_disc_hold_take().
	 */
	if (!clearing || l->fwd_disc_fired ||
	    !fwd_disc_delayed_class(l->fsm.clearing))
		uloop_timeout_cancel(&l->fwd_disc_hold);
	else
		fwd_disc_hold_take(l);

	if (clear && clearing && fwd_disc_signal(l))
		l->fwd_disc_fired = true;
}

static uint32_t sec_to_ms(unsigned int sec)
{
	if (sec > LINE_TIMER_MAX_S)
		sec = LINE_TIMER_MAX_S;

	return (uint32_t)sec * 1000u;
}

/*
 * The physical leg behind a UI slot (call.h call_slot_stream, the two-name
 * contract). Everything the core keys by stream is a leg fact: its
 * running-stream flag, its job slot and the call record, and so are the fax
 * latches of this file. Everything the machine and a northbound query name
 * is a UI slot. This is the one place the two are joined.
 *
 * A UI slot no leg holds answers itself, so a caller that reaches this
 * before a leg exists, or after one is gone, reads the slot as the stream,
 * and the refusals behind it read that index.
 */
static unsigned int slot_stream(unsigned int line, unsigned int slot)
{
	unsigned int stream = call_slot_stream(line, slot);

	return stream < CALL_SLOTS ? stream : slot;
}

/*
 * The same join read backwards: which UI slot names a leg. cb.started and
 * cb.ended speak the leg's own slot (call.h, the two-name contract) and the
 * machine speaks only UI slots, so the one callback that has to reach the
 * machine translates here. It answers CALL_SLOTS when no UI slot holds that
 * leg, which is what a leg the line has not named yet answers.
 */
static unsigned int slot_ui_of(unsigned int line, unsigned int stream)
{
	unsigned int slot;

	for (slot = 0; slot < CALL_SLOTS; slot++) {
		if (call_slot_stream(line, slot) == stream)
			return slot;
	}

	return CALL_SLOTS;
}

/* Open the record of the leg on stream, and tell the call trace a call
 * started there. */
static void record_open(unsigned int line, unsigned int stream, bool inbound)
{
	uint32_t seq;

	sip_host->cdr_call_started(sip_host->ctx, line, stream, inbound);
	seq = sip_host->cdr_call_seq(sip_host->ctx, line, stream);
	sip_host->calltrace_call_started(sip_host->ctx, line, seq);
}

/* The length the SIP machine gives a tone, from the line's channel copy. */
static uint32_t tone_ms_of(const struct sip_line *l, enum hal_tone tone)
{
	switch (tone) {
	case HAL_TONE_BUSY:
		return sec_to_ms(l->chan.busy_tone_s);
	case HAL_TONE_REORDER:
	case HAL_TONE_SIT:
	case HAL_TONE_NUMBER_UNOBTAINABLE:
		return FSM_REORDER_MS;
	case HAL_TONE_HOWLER:
		return sec_to_ms(l->chan.roh_s);
	default:
		return 0;
	}
}

/*
 * What the switch said, from the store of the core (proto/proto.h mwi_get). The
 * two presentations are gated separately and the store is written either way: a
 * line with both switched off still knows what the switch said, so switching
 * one on presents it from the next transition onward without asking the
 * switch again.
 */
static bool mwi_stored(const struct sip_line *l)
{
	uint32_t new_msgs, old_msgs;

	return sip_host->mwi_get(sip_host->ctx, l->index, &new_msgs,
				 &old_msgs);
}

static bool mwi_audible(const struct sip_line *l)
{
	return mwi_stored(l) && l->chan.amwi;
}

/*
 * Whether a digit the far end sent may be played now, and on which leg. Three
 * states carry a subscriber who can hear it, and the digit has to belong to
 * the leg facing the phone: the machine's active slot, or either owned leg in
 * the CONF conference, where both face it.
 *
 * A network call waiting alert keeps the machine CONNECTED while the shell
 * plays the subscriber alerting signal on the playout slot a digit would take
 * (op_cw_net_alert), so a digit under it would cut the alert. The offer window
 * of CW_OFFERED and the early media of PROCEEDING are out for the same kind of
 * reason: the line is being alerted, it is not conversing.
 *
 * libudsp is asymmetric here on purpose. A backend that renders the wire event
 * itself renders it in every call state, because that render is the far end's
 * own audio and takes no playout slot of this shell.
 */
static bool digit_play_allowed(const struct sip_line *l, unsigned int slot)
{
	if (l->fsm.state != FSM_CONNECTED && l->fsm.state != FSM_TWO_CALLS &&
	    l->fsm.state != FSM_CONF)
		return false;

	if (l->fsm.cw_net)
		return false;

	if (slot == l->fsm.active_slot)
		return true;

	return l->fsm.state == FSM_CONF && slot == l->fsm.other_slot;
}

/* ---------------------------------------------------------- deferred events */

static void line_ev_run(struct sip_line *l, enum fsm_ev ev, uint32_t arg)
{
	LTRACE(TRACE_FSM, "line %u %s in %s arg %u", l->index, fsm_ev_name(ev),
	       fsm_state_name(l->fsm.state), arg);

	/*
	 * The far-end-clear verdict is latched to its own event. call.c takes
	 * it when the BYE arrives, but the event can be deferred behind an
	 * open FSM frame, and a drain that runs in between would otherwise
	 * carry the verdict into a transition it does not describe. Converting
	 * it here ties it to the one transition it belongs to, so the settle
	 * behind that transition is the only one that can read it.
	 */
	if (ev == FSM_EV_CALL_REMOTE_BYE && l->fwd_disc_pending &&
	    l->fwd_disc_pending == (uint8_t)(arg + 1)) {
		l->fwd_disc_pending = 0;
		l->fwd_disc_clear = true;
	}

	l->fsm_depth++;
	fsm_event_push(&l->fsm, ev, arg);
	l->fsm_depth--;

	sip_host->line_transition(sip_host->ctx, l->index, l->fsm.state,
				  sip_line_flags_of(l));

	/* a one-call override belongs to the call the subscriber dialled it
	 * for, and IDLE is where that call ends however it ended. The network
	 * caller record is dropped on the same edge: it describes a call the
	 * switch offered against a call this line no longer has. */
	if (l->fsm.state != FSM_IDLE)
		return;

	feature_once_clear(&l->features);
	cw_net_clear(l);

	/* The message waiting store of the core is not dropped here: the line
	 * has merely come to rest. The held presentation it may owe is made by
	 * the settle at the end of the drain (proto/proto.h line_settle), which
	 * runs behind the config apply. */
}

static void drain_expire(struct uloop_timeout *t)
{
	struct sip_line *l = container_of(t, struct sip_line, drain);

	line_queue_drain(l);
}

static void line_defer_push(struct sip_line *l, enum fsm_ev ev, uint32_t arg)
{
	struct line_defer *d;

	if (l->qcount >= LINE_DEFER_MAX) {
		l->deferred_lost++;
		LTRACE(TRACE_FSM, "line %u deferred queue full, dropped %s", l->index,
		       fsm_ev_name(ev));
		return;
	}

	d = &l->q[(l->qhead + l->qcount) % LINE_DEFER_MAX];
	d->ev = (uint8_t)ev;
	d->arg = arg;
	l->qcount++;
	LTRACE(TRACE_FSM, "line %u %s deferred, arg %u", l->index, fsm_ev_name(ev), arg);

	/* the frame may be one fsm.c owns (a timer expiry), so the drain cannot
	 * rely on a shell frame closing */
	l->drain.cb = drain_expire;
	uloop_timeout_set(&l->drain, 0);
}

static void line_cfg_apply(struct sip_line *l)
{
	if (!l->cfg_pending)
		return;

	/* fsm.cfg.dp points at l->dp, so the compiled map may only be replaced
	 * once the machine has accepted the new view */
	if (!fsm_cfg_update(&l->fsm, &l->fcfg_new))
		return;

	l->dp = l->dp_new;
	l->fcfg = l->fcfg_new;
	l->chan = l->chan_new;
	sip_host->line_cfg_landed(sip_host->ctx, l->index);

	/*
	 * The view landing here carries the message waiting flag of the moment
	 * it was derived, and a NOTIFY that arrived in between is newer. The
	 * store is the only copy that survives a reload, so the machine is told
	 * again from it, against the amwi value that just landed.
	 */
	fsm_mwi_set(&l->fsm, mwi_audible(l));
	l->cfg_pending = false;

	/* feature toggles are runtime state and a reload drops them. The view
	 * landing here already carries the configured call waiting value, so
	 * the machine and the state agree again, and the do-not-disturb bit and
	 * the forward conditions are seeded from the channel copy that just
	 * landed. The network caller record is dropped with them, for the same
	 * reason. */
	feature_state_reset(&l->features, &l->chan);
	cw_net_clear(l);
	LTRACE(TRACE_FSM, "line %u config applied", l->index);
}

static void line_queue_drain(struct sip_line *l)
{
	if (l->fsm_depth)
		return;

	uloop_timeout_cancel(&l->drain);

	while (l->qcount) {
		struct line_defer d = l->q[l->qhead];

		l->qhead = (uint8_t)((l->qhead + 1) % LINE_DEFER_MAX);
		l->qcount--;
		line_ev_run(l, (enum fsm_ev)d.ev, d.arg);
	}

	line_cfg_apply(l);

	/*
	 * Before the settle below, because the two meet on the same wire: a
	 * line that comes to rest still reversed has its polarity restored
	 * here, and the visual message waiting record then rides a loop that is
	 * back in the forward direction.
	 */
	fwd_disc_settle(l);

	sip_host->line_settle(sip_host->ctx, l->index);
}

/*
 * Order. A queued event is older than this one and the machine must see the
 * two in the order the world produced them. A HAL event can arrive with the
 * queue still loaded, because uloop dispatches descriptors after the timeout
 * sweep that queued it and before the sweep that drains it, so the release of
 * a call the user abandoned could overtake the failure of that same call. The
 * machine would then take the release as the end of a live call, and the
 * failure would never reach its clearing state. Draining first costs one pass
 * through a queue that is empty in the common case and cannot drop anything,
 * which enqueueing behind a full ring could.
 */
static void line_ev_deliver(struct sip_line *l, enum fsm_ev ev, uint32_t arg)
{
	if (l->fsm_depth) {
		line_defer_push(l, ev, arg);
		return;
	}

	if (l->qcount)
		line_queue_drain(l);

	line_ev_run(l, ev, arg);
	line_queue_drain(l);
}

static void line_oos_force(struct sip_line *l)
{
	/* before the machine's own entry, which silences the port: those ops
	 * cross a line whose battery this shell may have removed, so the loop
	 * is put back first. The line is leaving service, so nothing is
	 * re-derived behind it */
	sip_host->fwd_disc_abort(sip_host->ctx, l->index);

	l->fsm_depth++;
	fsm_oos_force(&l->fsm);
	l->fsm_depth--;

	line_queue_drain(l);
}

static void line_oos_clear(struct sip_line *l)
{
	l->fsm_depth++;
	fsm_oos_clear(&l->fsm);
	l->fsm_depth--;

	/* the drain presents what this line came back into service owing */
	line_queue_drain(l);
}

/* ------------------------------------------------------------- the vtable */

/*
 * The presentation of the call about to leave, in exactly this order: the
 * one-call override the subscriber dialled, then the persistent runtime
 * restriction, then the loaded cid_outgoing_enable. The override is consumed
 * here, which is the dial moment and the only moment it can be consumed at:
 * one call takes it, and the line clears whatever is left at IDLE.
 */
static bool cid_present_take(struct sip_line *l)
{
	bool present;

	if (feature_cid_once_take(&l->features, &present))
		return present;

	return feature_cid_presentation(&l->features, &l->chan);
}

static void op_call_dial(void *priv, unsigned int slot, const char *number)
{
	struct sip_line *l = priv;
	bool present = cid_present_take(l);
	bool ok;

	LTRACE(TRACE_FSM, "line %u dial slot %u '%s' caller id %s", l->index, slot, number,
	       present ? "presented" : "withheld");

	l->fsm_depth++;
	ok = call_dial(l->index, slot, number, present);
	l->fsm_depth--;

	/*
	 * The outgoing attempt is counted where a leg exists to count. A dial
	 * the leg layer refused reported its failure from inside the call
	 * above, so counting it here would count an attempt that never left.
	 */
	/* the record is keyed by the leg and the op names a UI slot, so the
	 * dial that just landed is asked where it landed (slot_stream) */
	if (ok)
		record_open(l->index, slot_stream(l->index, slot), false);
}

/*
 * A refused 180 needs no second event from here. The leg layer traces the
 * reason and owns the answer on the wire. Where a leg still exists, that layer
 * fails it, and the report reaches this machine as FSM_EV_CALL_FAILED behind
 * this frame. Where no leg exists, the event that ended it already queued a
 * report of its own. This line states what the shell did with the answer, so
 * the two traces do not repeat each other.
 */
static void op_call_ring(void *priv, unsigned int slot)
{
	struct sip_line *l = priv;
	bool ok;

	l->fsm_depth++;
	ok = call_ring(l->index, slot);
	l->fsm_depth--;

	if (!ok)
		LTRACE(TRACE_FSM, "line %u slot %u: the leg layer sent no 180, "
		       "its report arrives behind this frame", l->index, slot);
}

static void op_call_answer(void *priv, unsigned int slot)
{
	struct sip_line *l = priv;
	bool ok;

	l->fsm_depth++;
	ok = call_answer(l->index, slot);
	l->fsm_depth--;

	/* the answered edge of an inbound call: the leg layer reports
	 * answered() for a call this line placed, never for one it took */
	if (ok)
		sip_host->cdr_call_answered(sip_host->ctx, l->index,
					    slot_stream(l->index, slot));
}

static void op_call_reject(void *priv, unsigned int slot, uint32_t code)
{
	struct sip_line *l = priv;

	l->fsm_depth++;
	call_reject(l->index, slot, code);
	l->fsm_depth--;
}

static void op_call_hangup(void *priv, unsigned int slot)
{
	struct sip_line *l = priv;

	l->fsm_depth++;
	call_hangup(l->index, slot);
	l->fsm_depth--;
}

/*
 * A refused re-INVITE never reaches the wire, so no callback will ever end
 * the PENDING record the machine is about to enter. The refusal becomes the
 * failure event, deferred inside the frame like any other callback, and the
 * machine takes its on_fail branch instead of stranding.
 */
static void op_call_hold(void *priv, unsigned int slot)
{
	struct sip_line *l = priv;

	l->fsm_depth++;
	if (!call_hold(l->index, slot)) {
		LTRACE(TRACE_FSM, "line %u hold of slot %u refused", l->index, slot);
		line_ev_deliver(l, FSM_EV_CALL_REINVITE_FAIL, slot);
	}
	l->fsm_depth--;
}

static void op_call_resume(void *priv, unsigned int slot)
{
	struct sip_line *l = priv;

	l->fsm_depth++;
	if (!call_resume(l->index, slot)) {
		LTRACE(TRACE_FSM, "line %u resume of slot %u refused", l->index, slot);
		line_ev_deliver(l, FSM_EV_CALL_REINVITE_FAIL, slot);
	}
	l->fsm_depth--;
}

/*
 * The vtable carries no slot: a DTMF digit belongs to the leg facing the
 * phone, which is the machine's active slot. In the CONF conference both
 * owned legs face the phone, so the machine emits one request and this shell
 * relays the digit to each of them.
 */
static void op_call_dtmf(void *priv, char digit, uint16_t duration_ms)
{
	struct sip_line *l = priv;
	unsigned int slot = l->fsm.active_slot;
	bool measured;

	if (slot >= CALL_SLOTS) {
		LTRACE(TRACE_FSM, "line %u dtmf '%c' with no active slot", l->index, digit);
		return;
	}

	/* The window plus a length: sip_digit_take() opens the window for an
	 * injected digit too, with duration 0, and opens none for a pulse
	 * digit, so neither of those two reads as measured. */
	measured = l->digit_detected_valid && l->digit_detected == digit &&
		   l->digit_detected_ms != 0;

	/* The FSM zero is a sentinel because only the HAL intake knows the duration. */
	if (!duration_ms && l->digit_detected_valid && l->digit_detected == digit) {
		duration_ms = l->digit_detected_ms;
		if (duration_ms > LINE_DTMF_DURATION_MAX_MS)
			duration_ms = LINE_DTMF_DURATION_MAX_MS;
	}

	l->fsm_depth++;
	call_dtmf(l->index, slot, digit, duration_ms, measured);
	if (l->fsm.state == FSM_CONF && l->fsm.other_slot < CALL_SLOTS)
		call_dtmf(l->index, l->fsm.other_slot, digit, duration_ms,
			  measured);
	l->fsm_depth--;
}

/*
 * The relayed hook flash of a connected leg. The vtable carries no slot for
 * the same reason the DTMF op does not: the flash belongs to the leg facing
 * the phone, which is the machine's active slot. A leg that cannot take it is
 * traced and nothing else happens: the far end owns the feature and this line
 * has no local fallback for it.
 */
static void op_flash_relay(void *priv)
{
	struct sip_line *l = priv;
	unsigned int slot = l->fsm.active_slot;
	bool ok;

	if (slot >= CALL_SLOTS) {
		LTRACE(TRACE_FSM, "line %u flash relay with no active slot", l->index);
		return;
	}

	l->fsm_depth++;
	ok = call_flash_relay(l->index, slot);
	l->fsm_depth--;

	LTRACE(TRACE_FSM, "line %u flash relay slot %u %s", l->index, slot,
	       ok ? "sent" : "refused");
}

/*
 * A clearing the machine reaches from its own timer closes no shell frame.
 * The start digit window expiring into a fast busy, and the busy clearing
 * chaining to the fast busy one, are both fsm.c timer expiries: fsm.c pushes
 * the event to itself, so nothing brings the line to the rest point the
 * delayed form is observed at. These three tones are the only entries that
 * form watches, so a settle is scheduled behind them. It rides the existing
 * deferred drain, and it must be a later pass rather than this frame, because
 * fsm.c assigns the new state after it emits the entry tone.
 */
static void fwd_disc_tone_settle(struct sip_line *l, enum hal_tone tone)
{
	switch (tone) {
	/*
	 * HAL_TONE_REORDER carries the busy to fast busy chain and the start
	 * digit window, and HAL_TONE_HOWLER carries the step out of the
	 * clearing into ROH, which is the one that leaks: without it a window
	 * armed by an unobtainable clearing survives into a state that owns no
	 * clearing class and reverses a line the subscriber abandoned.
	 *
	 * HAL_TONE_BUSY and HAL_TONE_NUMBER_UNOBTAINABLE are defensive members.
	 * No timer frame reaches either, because every entry that plays them
	 * is driven by an event the shell delivered and therefore settles
	 * behind that event. They are listed so the set is "the tones a
	 * clearing entry plays" rather than "the tones a timer reaches".
	 * HAL_TONE_SIT stays in the set: no entry plays it, and the core keeps
	 * its substitution.
	 */
	case HAL_TONE_BUSY:
	case HAL_TONE_REORDER:
	case HAL_TONE_SIT:
	case HAL_TONE_NUMBER_UNOBTAINABLE:
	case HAL_TONE_HOWLER:
		break;
	default:
		return;
	}

	l->drain.cb = drain_expire;
	uloop_timeout_set(&l->drain, 0);
}

static void op_tone(void *priv, enum hal_tone tone)
{
	struct sip_line *l = priv;

	LTRACE(TRACE_FSM, "line %u tone %u", l->index, tone);
	sip_host->tone_play(sip_host->ctx, l->index, tone, tone_ms_of(l, tone));
	fwd_disc_tone_settle(l, tone);
}

/*
 * Two profiles share this op. The recall rings the held leg while the line
 * is on hook in HOLDING; everything else rings for a caller waiting to be
 * answered, including the inbound call that HOLDING passes through to
 * RINGING, which rings its own new slot. State alone would give that caller
 * the recall profile, so the held slot is part of the test.
 */
static bool ring_is_recall(const struct sip_line *l, unsigned int slot)
{
	return l->fsm.state == FSM_HOLDING && l->fsm.other_held &&
	       l->fsm.other_slot == slot;
}

/* The caller of one slot, as the SIP leg layer reported it. */
static void sip_cid_of_slot(const struct sip_line *l, unsigned int slot,
			    struct proto_cid *out)
{
	memset(out, 0, sizeof(*out));
	out->present = true;
	if (slot >= CALL_SLOTS)
		return;

	str_copy(out->number, sizeof(out->number), l->cid[slot].number);
	str_copy(out->name, sizeof(out->name), l->cid[slot].name);
	out->number_private = l->cid[slot].number_private;
	out->name_private = l->cid[slot].name_private;
}

/*
 * Two profiles share this op. The recall rings the held leg while the line
 * is on hook in HOLDING; everything else rings for a caller waiting to be
 * answered, with the template and the caller of its slot.
 */
static void op_ring_start(void *priv, unsigned int slot)
{
	struct sip_line *l = priv;
	struct proto_cid cid;

	if (ring_is_recall(l, slot)) {
		sip_host->ring_start(sip_host->ctx, l->index, PROTO_RING_RECALL,
				     0, 0, NULL);
		return;
	}

	sip_cid_of_slot(l, slot, &cid);
	sip_host->ring_start(sip_host->ctx, l->index, PROTO_RING_CALL,
			     slot < CALL_SLOTS ? l->cid[slot].ring_template : 0,
			     0, &cid);
}

static void op_ring_stop(void *priv)
{
	struct sip_line *l = priv;

	sip_host->ring_stop(sip_host->ctx, l->index);
}

/* The BroadWorks CallWaitingTone digit is the variant directly; a value the
 * region does not define resolves to 0, the plain tone. */
static unsigned int cw_net_variant(const struct sip_line *l)
{
	unsigned int v = l->cw_net.tone;

	return (v >= 1 && v <= sip_host->region(sip_host->ctx)->cw_count) ?
	       v : 0;
}

static void op_cw_alert(void *priv, unsigned int slot, bool on)
{
	struct sip_line *l = priv;
	struct proto_cid cid;

	if (!on) {
		sip_host->cw_alert(sip_host->ctx, l->index, false, 0, NULL);
		return;
	}

	sip_cid_of_slot(l, slot, &cid);
	sip_host->cw_alert(sip_host->ctx, l->index, true,
			   l->cid[slot].cw_variant, &cid);
}

/*
 * The same subscriber alerting signal for a call the switch holds. The record
 * comes from the INFO the switch sent, not from a slot: there is no waiting
 * leg on this line to look one up from. The DSP tone profile owns the cadence
 * and the repeat, so the shell presents the alert once per play.
 */
static void op_cw_net_alert(void *priv, bool on)
{
	struct sip_line *l = priv;
	unsigned int variant;
	struct proto_cid cid;

	if (!on) {
		sip_host->cw_alert(sip_host->ctx, l->index, false, 0, NULL);
		cw_net_clear(l);
		return;
	}

	variant = cw_net_variant(l);
	LTRACE(TRACE_FSM, "line %u network call waiting variant %u name '%s' number '%s'",
	       l->index, variant, l->cw_net.name, l->cw_net.number);

	/* the INFO carries an identity or it carries nothing; it has no way to
	 * say "withheld", so neither privacy flag can be set here */
	memset(&cid, 0, sizeof(cid));
	cid.present = true;
	str_copy(cid.number, sizeof(cid.number), l->cw_net.number);
	str_copy(cid.name, sizeof(cid.name), l->cw_net.name);
	sip_host->cw_alert(sip_host->ctx, l->index, true, variant, &cid);
}

/*
 * The star code the machine matched in OFFHOOK_DIAL or COLLECTING. The shell
 * applies the toggle, re-derives the one cfg field a toggle moves and answers
 * the machine, which plays the confirmation tone or enters CLEARING(fastbusy)
 * on that answer. It plays nothing itself.
 */
static const char *feature_outcome(bool ok, enum feature_action act)
{
	if (ok)
		return "applied";
	if (feature_action_supported(act))
		return "refused";

	return "recognized, not yet supported";
}

/*
 * The second stage of a star code that carries a target. The shell applies it
 * and answers; an empty argument is not an error here, because features.c
 * reads it as "keep the target you already hold", which after a reload is the
 * configured one.
 */
static bool op_feature_invoke_arg(void *priv, const char *code, const char *arg)
{
	struct sip_line *l = priv;
	enum feature_action act;
	bool ok;

	feature_lookup(&l->chan, code, &act);
	ok = feature_execute_arg(&l->features, &l->chan, code, arg);

	LTRACE(TRACE_FSM, "line %u star code '%s' (%s) target '%s' %s", l->index, code,
	       feature_action_name(act), arg, feature_outcome(ok, act));

	return ok;
}

/*
 * The local call forward redirect. The leg answers 302 and ends, so the FSM
 * owes this slot nothing more. A leg that cannot take the redirect is traced
 * and the caller hears whatever the far end does with an unanswered INVITE:
 * inventing a local treatment here would contradict the cell that chose the
 * forward.
 */
static void op_call_forward(void *priv, unsigned int slot, const char *target)
{
	struct sip_line *l = priv;
	bool ok;

	l->fsm_depth++;
	ok = call_forward(l->index, slot, target);
	l->fsm_depth--;

	LTRACE(TRACE_FSM, "line %u forward slot %u to '%s' %s", l->index, slot, target,
	       ok ? "sent" : "refused");
}

/*
 * The two referable rows onto the call.c vocabulary (call.h enum
 * call_xfer_kind). The consult row has an entry and an op of its own, so it
 * can never arrive at this map, and false is what tells the caller it did.
 */
static bool xfer_kind_of(enum feature_action act, enum call_xfer_kind *out)
{
	switch (act) {
	case FEATURE_ACT_XFER_BLIND:
		*out = CALL_XFER_BLIND;
		return true;
	case FEATURE_ACT_XFER_ATTENDED:
		*out = CALL_XFER_ATTENDED;
		return true;
	default:
		return false;
	}
}

/*
 * The REFER of the leg the machine already held (fsm.h item 4, call.h
 * call_xfer). A refusal never reached the wire, so no cb.xfer_done will ever
 * end the PENDING record the machine is entering: the refusal becomes the
 * failure event, and the fail arm resumes the leg.
 *
 * One raise covers the call and the injection, which is the op_call_hold
 * shape. The short interdigit timer of XFER_COLLECT completes a target from a
 * frame fsm.c owns, where the shell holds no frame of its own and the count
 * is zero, so an injection made outside the raise would re-enter the machine
 * from inside its own transition instead of landing in the deferred ring.
 *
 * An empty target is passed on as it is. call.h refuses it, and that refusal
 * takes the same injection as every other one, so one rule answers both.
 */
static void op_call_xfer(void *priv, unsigned int slot, enum feature_action act,
			 const char *target)
{
	struct sip_line *l = priv;
	enum call_xfer_kind kind;
	bool referable = xfer_kind_of(act, &kind);
	bool onhook = false;
	bool ok = false;

	/* the machine reaches this op from the two referable rows alone, so any
	 * other action is a wiring fault and not a path a cell can take */
	if (!referable)
		ULOG_WARN("line %u transfer of slot %u refused: %s refers nothing\n",
			  l->index, slot, feature_action_name(act));

	/*
	 * Which cell is asking. The on-hook blind transfer of CALLING and
	 * PROCEEDING runs this op before its cell enters IDLE, so the machine
	 * still reads CALLING or PROCEEDING here; the star-code form runs it
	 * from XFER_COLLECT. The state at op time therefore names the
	 * caller, the way the bell op reads the state for the recall profile.
	 * The on-hook caller gets the kind whose failure the call layer cleans
	 * up itself: the subscriber is on hook, and the machine may have moved
	 * on by the time the REFER fails, so no cell can be owed that hangup.
	 */
	if (referable && kind == CALL_XFER_BLIND &&
	    (l->fsm.state == FSM_CALLING || l->fsm.state == FSM_PROCEEDING)) {
		kind = CALL_XFER_ONHOOK;
		onhook = true;
	}

	l->fsm_depth++;

	if (referable) {
		ok = call_xfer(l->index, slot, kind, target);
		LTRACE(TRACE_FSM, "line %u transfer slot %u (%s%s) to '%s' %s", l->index,
		       slot, feature_action_name(act), onhook ? ", on hook" : "",
		       target ? target : "", ok ? "sent" : "refused");
	}

	if (!ok) {
		/*
		 * A refusal made no wire exchange. Off hook, the injected failure
		 * resumes the held leg. On hook, the referred leg is still up with
		 * nobody to clear it, and this wrapper is that nobody, the
		 * op_call_xfer_consult shape; the injected failure then lands in
		 * IDLE on a leg the call layer already ended.
		 */
		if (onhook)
			call_hangup(l->index, slot);
		line_ev_deliver(l, FSM_EV_CALL_XFER_FAIL, slot);
	}

	l->fsm_depth--;
}

/*
 * The on-hook consult transfer (fsm.h item 4, call.h call_xfer_consult). It is
 * fire and forget, so no event is owed either way: the cell that ran it has
 * already entered IDLE.
 *
 * The refusal cleanup is this wrapper's. Nothing went on the wire and the two
 * legs are still up with nobody left to clear them, because the machine owns
 * no transfer to fail and no leg to hang up.
 */
static void op_call_xfer_consult(void *priv, unsigned int referee,
				 unsigned int replaced)
{
	struct sip_line *l = priv;
	bool ok;

	l->fsm_depth++;
	ok = call_xfer_consult(l->index, referee, replaced);
	l->fsm_depth--;

	LTRACE(TRACE_FSM, "line %u consult transfer refers slot %u to slot %u %s",
	       l->index, referee, replaced, ok ? "sent" : "refused");

	if (ok)
		return;

	l->fsm_depth++;
	call_hangup(l->index, referee);
	call_hangup(l->index, replaced);
	l->fsm_depth--;
}

static bool op_feature_invoke(void *priv, const char *code)
{
	struct sip_line *l = priv;
	enum feature_action act;
	bool ok;

	feature_lookup(&l->chan, code, &act);
	ok = feature_execute(&l->features, &l->chan, code);

	LTRACE(TRACE_FSM, "line %u star code '%s' (%s) %s", l->index, code,
	       feature_action_name(act), feature_outcome(ok, act));

	if (!ok)
		return false;

	/* the next call must see the new value even when the subscriber never
	 * went back on hook, so this one field bypasses the at-rest view.
	 * l->fcfg stays the last applied config view, which is what the reload
	 * path compares and replaces. */
	fsm_call_waiting_set(&l->fsm, feature_cw_effective(&l->features, &l->chan));

	return true;
}

/*
 * The mixer accepted or refused the join. Both owned legs are the machine's
 * active and other slot: the on runs from the completion of the join's
 * resume, while the record still names them that way.
 *
 * A refusal is answered with the injected event, which is the one refusal
 * signal the mixer has (CONF CALL_REINVITE_FAIL), and the machine splits the
 * conference back into two calls. The latch that keeps the next flash off the
 * join belongs to the machine and is set there.
 */
static void conf_on_take(struct sip_line *l, enum hal_status st)
{
	unsigned int active = l->fsm.active_slot;
	unsigned int other = l->fsm.other_slot;

	if (st != HAL_OK) {
		l->fsm_depth++;
		line_ev_deliver(l, FSM_EV_CALL_REINVITE_FAIL, active);
		l->fsm_depth--;
		return;
	}

	/* the record is keyed by the leg and the machine names UI slots, so
	 * each of the two is asked where it landed (slot_stream) */
	sip_host->cdr_call_conferenced(sip_host->ctx, l->index,
				       slot_stream(l->index, active));
	sip_host->cdr_call_conferenced(sip_host->ctx, l->index,
				       slot_stream(l->index, other));
}

/*
 * The same for the off. An unsupported answer means the library holds no
 * configured stream, and the settle behind two hangups always meets it.
 *
 * Every other refusal is reported and nothing is torn down. A held leg encodes
 * silence whatever the mixer arm says, so an arm left running leaks no audio,
 * and an engine that is really dead ends the daemon through its own stall and
 * dead-stream exits.
 */
static void conf_off_take(const struct sip_line *l, enum hal_status st)
{
	if (st == HAL_OK || st == HAL_UNSUPPORTED)
		return;

	line_fault_report(l, true,
			  "the conference mixer did not stop, so the two "
			  "calls keep the mix the backend holds");
}

static void op_conf_set(void *priv, bool on)
{
	struct sip_line *l = priv;
	enum hal_status st = sip_host->conf_set(sip_host->ctx, l->index, on);

	LTRACE(TRACE_FSM, "line %u conference %s: %s", l->index,
	       on ? "on" : "off", hal_status_name(st));

	if (on)
		conf_on_take(l, st);
	else
		conf_off_take(l, st);
}

/* Every move of the machine reaches the call trace by name (proto/proto.h
 * calltrace_state), which reads the line's flags and oos bit at this call. */
static void op_state_changed(void *priv, enum fsm_state before,
			     enum fsm_state after)
{
	struct sip_line *l = priv;

	sip_host->calltrace_state(sip_host->ctx, l->index,
				  fsm_state_name(before), fsm_state_name(after));
}

static const struct fsm_ops line_ops = {
	.call_dial = op_call_dial,
	.call_ring = op_call_ring,
	.call_answer = op_call_answer,
	.call_reject = op_call_reject,
	.call_hangup = op_call_hangup,
	.call_hold = op_call_hold,
	.call_resume = op_call_resume,
	.call_dtmf = op_call_dtmf,
	.tone = op_tone,
	.ring_start = op_ring_start,
	.ring_stop = op_ring_stop,
	.cw_alert = op_cw_alert,
	.cw_net_alert = op_cw_net_alert,
	.flash_relay = op_flash_relay,
	.feature_invoke = op_feature_invoke,
	.feature_invoke_arg = op_feature_invoke_arg,
	.call_forward = op_call_forward,
	.call_xfer = op_call_xfer,
	.call_xfer_consult = op_call_xfer_consult,
	.conf_set = op_conf_set,
	.state_changed = op_state_changed,
};

/*
 * The number match chooses, and an Alert-Info the offer carried then
 * overrides it. An id of 0 means no dialect recognized the header, so an
 * unmapped token leaves the number match standing instead of silencing it.
 */
static unsigned int ring_template_of(const struct sip_line *l,
				     const char *number, unsigned int ring_id)
{
	unsigned int t = sip_host->ring_template_match(sip_host->ctx,
						       l->index, number);

	if (ring_id)
		t = ring_id;

	return t;
}

/*
 * The Alert-Info template a waiting call carried, mapped into the call-waiting
 * variant space. Bellcore-dr1..4 map to call-waiting variants 1..4 by
 * identity, and every other id takes the plain tone: a template inside
 * 1..cw_count keeps its number, and id 0 or an id above cw_count resolves to
 * 0. The non-BroadSoft dialects fold into the same 1..7 template space in
 * call.c, so they follow the same rule.
 */
static unsigned int cw_variant_of_ring_id(unsigned int ring_id)
{
	unsigned int count = sip_host->region(sip_host->ctx)->cw_count;

	return (ring_id >= 1 && ring_id <= count) ? ring_id : 0;
}

static void cb_incoming(unsigned int line, unsigned int slot, const char *cid_number,
			const char *cid_name, bool number_private, bool name_private,
			unsigned int ring_id)
{
	struct sip_line *l = sip_line_get(line);

	if (!l || slot >= CALL_SLOTS)
		return;

	/* the strings live for this call only (call.h), so they are copied
	 * before the event reaches the machine, privacy verdict included */
	str_copy(l->cid[slot].number, sizeof(l->cid[slot].number), cid_number);
	str_copy(l->cid[slot].name, sizeof(l->cid[slot].name), cid_name);
	l->cid[slot].number_private = number_private;
	l->cid[slot].name_private = name_private;
	l->cid[slot].ring_id = ring_id > UINT8_MAX ? 0 : (uint8_t)ring_id;
	l->cid[slot].ring_template = (uint8_t)ring_template_of(l, cid_number,
							       l->cid[slot].ring_id);
	l->cid[slot].cw_variant =
		(uint8_t)cw_variant_of_ring_id(l->cid[slot].ring_id);

	LTRACE(TRACE_FSM,
	       "line %u slot %u alert id %u rings template %u cw variant %u", line,
	       slot, l->cid[slot].ring_id, l->cid[slot].ring_template,
	       l->cid[slot].cw_variant);

	/*
	 * Before the machine sees the offer, because the cell that takes it may
	 * reject or forward the call inside this frame and the end of a record
	 * cannot precede its start.
	 *
	 * This counts every inbound leg, not every inbound INVITE: an offer
	 * refused 486 for want of a slot never reaches here.
	 */
	record_open(line, slot_stream(line, slot), true);

	line_ev_deliver(l, FSM_EV_CALL_INCOMING, slot);
}

/*
 * The two edges of the ringback gate. The fire path reads a stream index the
 * backend supplied, so both helpers refuse one out of range. It is the bound
 * check fax_exit_cancel() makes on the same kind of index.
 */
static void early_rx_arm(struct sip_line *l, unsigned int stream)
{
	if (stream >= CALL_SLOTS)
		return;

	l->early_rx_wait[stream] = true;
	LTRACE(TRACE_FSM, "line %u stream %u waits for the first early media "
	       "packet", l->index, stream);
}

static void early_rx_clear(struct sip_line *l, unsigned int stream)
{
	if (stream >= CALL_SLOTS)
		return;

	l->early_rx_wait[stream] = false;
}

/*
 * RFC 3960 3.2: a programmed early stream is not audio. The far end may send
 * nothing for seconds, or nothing at all, so the flag arms the gate here and
 * the machine hears the proceeding report with the early bit clear. The
 * ringback plays until rtp_first_rx_event() below reports a packet.
 */
static void cb_proceeding(unsigned int line, unsigned int slot, bool early_media)
{
	struct sip_line *l = sip_line_get(line);

	if (!l)
		return;

	if (early_media)
		early_rx_arm(l, slot_stream(line, slot));

	line_ev_deliver(l, FSM_EV_CALL_PROCEEDING, slot << 1);
}

static void cb_answered(unsigned int line, unsigned int slot)
{
	struct sip_line *l = sip_line_get(line);

	if (!l)
		return;

	/*
	 * The backend can report a first packet again behind the 200: libudsp
	 * clears rx_seen in stream_engine_adopt(), which a reprogram takes
	 * when the stream is not running or its codec clock moved. The machine
	 * in CONNECTED ignores the event either way; the clear keeps the gate
	 * honest.
	 */
	early_rx_clear(l, slot_stream(line, slot));

	/* the far end took the call this line placed */
	sip_host->cdr_call_answered(sip_host->ctx, line,
				    slot_stream(line, slot));

	line_ev_deliver(l, FSM_EV_CALL_ANSWERED, slot);
}

static void cb_failed(unsigned int line, unsigned int slot, enum call_fail_class cls)
{
	struct sip_line *l = sip_line_get(line);

	if (!l)
		return;

	line_ev_deliver(l, FSM_EV_CALL_FAILED, (slot << 8) | (uint32_t)cls);
}

/*
 * A peer BYE, which is also the far-end clear the forward disconnect of
 * CONNECTED runs on. call.c raises this for an answered leg only: a peer
 * that ends an early dialog is reported through failed() instead, so a BYE
 * arriving here already means a call the subscriber had.
 *
 * Two narrowings beyond that, both here rather than in call.c, because both
 * are questions about the line and not about the leg.
 *
 * The slot must be the one facing the phone. A held leg the far end drops
 * leaves the subscriber talking on the other one, and a leg a clearing
 * already released is not a call at all; neither is a disconnect to signal.
 *
 * The machine must then announce the clear, which fwd_disc_settle() decides
 * behind the transition. RINGING is why that second test is not optional: its
 * entry makes the ringing leg the active slot, so a caller who sends BYE
 * instead of CANCEL passes the filter above, and only the state the cell
 * lands in (IDLE or HOLDING, never CLEARING) keeps the bell from being
 * answered with a battery reversal.
 */
static void cb_remote_bye(unsigned int line, unsigned int slot)
{
	struct sip_line *l = sip_line_get(line);

	if (!l)
		return;

	if (slot == l->fsm.active_slot) {
		l->fwd_disc_pending = (uint8_t)(slot + 1);
		LTRACE(TRACE_FSM, "line %u slot %u cleared by the far end", line, slot);
	}

	line_ev_deliver(l, FSM_EV_CALL_REMOTE_BYE, slot);
}

static void cb_remote_cancel(unsigned int line, unsigned int slot)
{
	struct sip_line *l = sip_line_get(line);

	if (!l)
		return;

	line_ev_deliver(l, FSM_EV_CALL_REMOTE_CANCEL, slot);
}

/* No matrix cell consumes a far end hold: the media direction is call.c work
 * and the user interface does not change. */
static void cb_hold_changed(unsigned int line, unsigned int slot, bool held)
{
	LTRACE(TRACE_FSM, "line %u slot %u remote hold %d", line, slot, held);
}

static void cb_reinvite_done(unsigned int line, unsigned int slot, bool ok)
{
	struct sip_line *l = sip_line_get(line);

	if (!l)
		return;

	line_ev_deliver(l, ok ? FSM_EV_CALL_REINVITE_OK : FSM_EV_CALL_REINVITE_FAIL, slot);
}

/*
 * The outcome of one REFER (call.h xfer_done, fsm.h event 2). ok is the whole
 * verdict: true is FSM_EV_CALL_XFER_OK and false is FSM_EV_CALL_XFER_FAIL, with
 * the slot as the argument, and the slot guards of the machine make a late
 * outcome inert once the cell re-roled. The status code carries why, which is
 * a fact for the log and for nothing else: no cell reads it and no status key
 * holds it.
 */
static void cb_xfer_done(unsigned int line, unsigned int slot, bool ok, uint32_t code)
{
	struct sip_line *l = sip_line_get(line);

	if (!l)
		return;

	LTRACE(TRACE_FSM, "line %u slot %u transfer %s, code %u", line, slot,
	       ok ? "completed" : "failed", code);

	line_ev_deliver(l, ok ? FSM_EV_CALL_XFER_OK : FSM_EV_CALL_XFER_FAIL, slot);
}

/*
 * The caller identity of a network alert stops here. The machine gets one
 * event whose argument is the command (0 stop, 1 to 4 play that pattern) and
 * the strings stay in the shell, which renders them when the machine asks for
 * the alert. The trace carries the effective call waiting value beside the
 * command, because that value is what decides whether the play is presented
 * or dropped, and the decision itself belongs to the machine.
 */
static void cb_cw_net(unsigned int line, unsigned int slot, bool on, unsigned int tone,
		      const char *name, const char *number)
{
	struct sip_line *l = sip_line_get(line);

	if (!l)
		return;

	/*
	 * Mode FSM_CW_LOCAL gives the service to this line, so the switch
	 * driven play is dropped here as well as by the machine's own play
	 * gate. The gate is the policy; this drop only saves the event churn
	 * of a play the machine would refuse. The SIP answer is untouched:
	 * call.c has already answered the INFO with 200, and only the
	 * presentation goes.
	 *
	 * A stop is never dropped. Its cell is unconditional by design, so it
	 * stays idempotent for a line that never presented an alert, and a mode
	 * that changed under a running alert still gets that alert ended.
	 */
	if (on && cw_mode_of(&l->chan) == FSM_CW_LOCAL) {
		LTRACE(TRACE_FSM, "line %u slot %u network call waiting play dropped, "
		       "call_waiting_local 1 presents the local offer only", line, slot);
		return;
	}

	/* the strings live for this call only (call.h) */
	str_copy(l->cw_net.number, sizeof(l->cw_net.number), number);
	str_copy(l->cw_net.name, sizeof(l->cw_net.name), name);
	l->cw_net.tone = on ? (uint8_t)tone : 0;

	LTRACE(TRACE_FSM, "line %u slot %u network call waiting %s tone %u, call waiting %d",
	       line, slot, on ? "play" : "stop", tone,
	       feature_cw_effective(&l->features, &l->chan));

	line_ev_deliver(l, FSM_EV_CW_NET, on ? tone : 0);
}

/*
 * The digit a far end sent in an INFO body (call.h cb.digit_net). It takes
 * the path the backend's wire report takes, because the two are one event on
 * two transports, and the slot it carries is already the UI one.
 */
static void cb_digit_net(unsigned int line, unsigned int slot, char digit,
			 uint16_t duration_ms)
{
	if (!sip_line_get(line) || slot >= CALL_SLOTS)
		return;

	sip_host->digit_play(sip_host->ctx, line, slot_stream(line, slot), digit,
			     duration_ms, true);
}

/*
 * The start of a leg the leg layer made by itself (call.h cb.started): the
 * child INVITE of a transfer this line was asked to take. No op of this shell
 * placed it and no offer rang it, so this is the only edge that can open the
 * record it is owed. The slot is the leg's own, which is the key the two edges
 * below use for the same leg, and the machine hears no call event from here.
 *
 * One fact does reach the machine, and it is not a call event. The new leg
 * takes over the UI name of the leg it replaces, and the two cells that read
 * a per slot direction must then read the direction of the new call. Nothing
 * else can state it: this leg arrives without a CALL_INCOMING and without a
 * call_dial. The name has not moved yet at this moment (call.h, the two-name
 * contract), so the UI slot to write is the one that still holds the leg
 * being taken over.
 */
static void cb_started(unsigned int line, unsigned int slot, bool inbound,
		       unsigned int from_slot)
{
	struct sip_line *l = sip_line_get(line);
	unsigned int ui;

	LTRACE(TRACE_FSM, "line %u slot %u transferred call started from slot %u",
	       line, slot, from_slot);

	record_open(line, slot, inbound);
	sip_host->calltrace_event(sip_host->ctx, line, "leg_started");
	/* the record of the leg this one took the call over from is still
	 * open, which is the one moment the correlation can be written */
	sip_host->cdr_call_correlate(sip_host->ctx, line, slot, from_slot);

	if (!l || from_slot >= CALL_SLOTS)
		return;

	ui = slot_ui_of(line, from_slot);
	if (ui >= CALL_SLOTS)
		return;

	LTRACE(TRACE_FSM, "line %u slot %u takes name %u, direction %s", line,
	       slot, ui, inbound ? "in" : "out");
	fsm_slot_inbound_set(&l->fsm, ui, inbound);
}

/*
 * The deferral behind the end row of a plain answer tone. The leg owns the
 * rule (call.h call_fax_exit) and this shell owns the wait, the way it owns
 * every other timer of the line.
 */
static void fax_exit_cancel(struct sip_line *l, unsigned int stream)
{
	if (stream >= CALL_SLOTS)
		return;

	uloop_timeout_cancel(&l->fax_exit[stream].timer);
}

static void fax_exit_expire(struct uloop_timeout *t)
{
	struct line_fax_exit *e = container_of(t, struct line_fax_exit, timer);
	struct sip_line *l = sip_line_get(e->line);

	if (!l || e->stream >= CALL_SLOTS)
		return;

	if (!call_fax_exit(e->handle)) {
		LTRACE(TRACE_HOOK, "line %u stream %u keeps the passthrough it "
		       "took from the answer tone", e->line, e->stream);
		return;
	}

	/*
	 * The leg carries voice again, so the stage 1 latch has to re-arm: a
	 * second answer tone on the same leg is a new conversion and not the
	 * burst this one already spent.
	 */
	l->fax_switched[e->stream] = false;
	LTRACE(TRACE_FSM, "line %u stream %u returned to voice after a short "
	       "answer tone", e->line, e->stream);
}

static void fax_exit_arm(struct sip_line *l, unsigned int stream,
			 unsigned int slot)
{
	struct line_fax_exit *e;

	if (stream >= CALL_SLOTS)
		return;

	e = &l->fax_exit[stream];
	e->line = l->index;
	e->stream = stream;
	e->handle = call_leg_handle_get(l->index, slot);
	e->timer.cb = fax_exit_expire;
	uloop_timeout_set(&e->timer, CALL_FAX_EXIT_DEFER_MS);
}

/*
 * The end of one leg, whatever ended it. It closes the record and counts the
 * failure classes; the machine hears nothing here, because every outcome it
 * acts on already reached it through its own callback.
 */
static void cb_ended(unsigned int line, unsigned int slot,
		     const struct call_ended *info)
{
	struct sip_line *l = sip_line_get(line);

	/* The fax latches are the leg's. This report fires exactly once per leg
	 * that was in use, whatever ended it, so it is the one edge that
	 * re-arms both switches for the next call on the slot. The deferral of
	 * that leg goes with them: its handle names a leg that is gone. The
	 * ringback gate of the leg goes the same way, and for the same reason:
	 * a failure, a CANCEL and a BYE all pass here, so the next call on the
	 * slot starts with no gate of a leg that is gone. */
	if (l && slot < CALL_SLOTS) {
		l->fax_switched[slot] = false;
		l->t38_switched[slot] = false;
		early_rx_clear(l, slot);
		fax_exit_cancel(l, slot);
	}

	sip_host->cdr_call_ended(sip_host->ctx, line, slot, info);
	sip_host->calltrace_event(sip_host->ctx, line, "leg_ended");
}

static const struct call_cb line_call_cb = {
	.incoming = cb_incoming,
	.proceeding = cb_proceeding,
	.answered = cb_answered,
	.failed = cb_failed,
	.remote_bye = cb_remote_bye,
	.remote_cancel = cb_remote_cancel,
	.hold_changed = cb_hold_changed,
	.reinvite_done = cb_reinvite_done,
	.xfer_done = cb_xfer_done,
	.cw_net = cb_cw_net,
	.started = cb_started,
	.ended = cb_ended,
	.digit_net = cb_digit_net,
};

/*
 * The answer family identifies a data device, not whether it carries fax.
 * V.152 10.1.1 and 10.3 permit media policy changes on either feed.
 * A remote answer tone arms the kind; a local answer tone also starts the
 * G.711 profile and offer. CNG identifies the caller and starts neither.
 * The local V.21 preamble supplies the fax evidence separately.
 * The DSP's G.168 clause 7 disabler runs independently of these offer gates.
 *
 * chan_fax_latch() in udsp/src/core/chan.c reads answer tones from either feed.
 * With ec_tone_force_disable at zero, phase-reversed tones disable the whole
 * canceller; plain tones disable its non-linear processor.
 */
static bool fax_tone_starts(uint16_t code)
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
 * Both fax terminals send V.21 flags during T.30 negotiation.
 * The preamble identifies fax, but only the local answer-tone latch permits
 * this side to offer T.38 instead of waiting for the far end's offer.
 */
static bool t38_tone_starts(uint16_t code)
{
	return code == HAL_TONE_DET_V21_PREAMBLE;
}

/*
 * A tone whose start row says the answer tone before it was real, so the
 * deferral of call.h call_fax_exit has nothing left to undo. The leg counts
 * the same rows as evidence; this half is the timer alone.
 */
static bool fax_tone_evidence(uint16_t code)
{
	return fax_tone_starts(code) || t38_tone_starts(code) ||
	       code == HAL_TONE_DET_MODEM;
}

/*
 * The end row of a tone the two stages act on. The detector reports both edges
 * (hal.h u.tone.end) and the gates below read a start: without this drop the
 * end row of a CED would run a second upspeed and the end row of a preamble a
 * second image offer.
 */
static bool fax_tone_end_drops(const struct proto_line_event *ev)
{
	return ev->u.tone.end &&
	       (fax_tone_starts(ev->u.tone.code) ||
		t38_tone_starts(ev->u.tone.code));
}

/*
 * Every tone row reaches the return-to-voice rule before any other gate reads
 * it (call.h call_fax_tone_note), because the rule counts rows the gates below
 * drop. A start row that says the tone was real cancels a deferral that
 * stands, and the end row of the plain answer tone arms one.
 *
 * False says the row is an end row the two stages must not see: it reaches
 * neither the hint, nor the kind, nor the upspeed, nor the T.38 gates.
 */
static bool fax_tone_note(struct sip_line *l, const struct proto_line_event *ev,
			  unsigned int slot)
{
	unsigned int stream = slot_stream(l->index, slot);
	enum call_fax_tone note;

	note = call_fax_tone_note(l->index, slot, ev->u.tone.code,
				  ev->u.tone.direction, ev->u.tone.end != 0,
				  ev->u.tone.duration_ms);

	if (!ev->u.tone.end) {
		if (fax_tone_evidence(ev->u.tone.code))
			fax_exit_cancel(l, stream);

		return true;
	}

	if (note == CALL_FAX_TONE_ENDED && ev->u.tone.code == HAL_TONE_DET_CED)
		fax_exit_arm(l, stream, slot);

	if (!fax_tone_end_drops(ev))
		return true;

	LTRACE(TRACE_HOOK, "line %u the %s end row after %u ms starts nothing",
	       l->index, hal_tone_detect_name(ev->u.tone.code),
	       ev->u.tone.duration_ms);

	return false;
}

/*
 * The local preamble selects the fax kind before the T.38 gates.
 * A remote tone can arm that kind without a profile or a local latch.
 * A followed upspeed also carries a kind without the latch. Both paths need
 * the fax policy even when fax_t38_enable is zero.
 *
 * The local answer-tone latch alone permits an image offer: the detector
 * cannot distinguish the called fax's DIS flags from the calling fax's DCS.
 * call_t38_start() also requires the G.711 profile, because the latch records
 * an attempt and the backend can refuse the profile.
 * The leg reads HAL_FEAT_T38 and the channel option at its start, using the
 * feature mask from calls_hal_features_set(). call_t38_start() refuses when
 * either is missing, so this handler needs no backend capability test.
 *
 * A refused image offer spends the T.38 latch to avoid repeated offers.
 * CALL_T38_BUSY preserves it: no offer reached the peer, so a later preamble
 * can retry after the current exchange ends. The kind needs no latch because
 * call_fax_kind_set() skips a description the stream already carries.
 */
static void t38_tone_event(struct sip_line *l,
			   const struct proto_line_event *ev, unsigned int slot)
{
	enum call_t38_answer answer;
	unsigned int stream;
	bool kind_set;

	if (!t38_tone_starts(ev->u.tone.code)) {
		LTRACE(TRACE_HOOK, "line %u tone %s starts no fax", l->index,
		       hal_tone_detect_name(ev->u.tone.code));
		return;
	}

	if (ev->u.tone.direction != HAL_TONE_DIR_LOCAL) {
		LTRACE(TRACE_HOOK, "line %u tone %s came from the wire, the far end "
		       "owns that switch", l->index,
		       hal_tone_detect_name(ev->u.tone.code));
		return;
	}

	if (l->fsm.state != FSM_CONNECTED || slot >= CALL_SLOTS) {
		LTRACE(TRACE_HOOK, "line %u V.21 preamble dropped in %s", l->index,
		       fsm_state_name(l->fsm.state));
		return;
	}

	stream = slot_stream(l->index, slot);
	if (!l->fax_switched[stream] &&
	    call_fax_kind_get(l->index, slot) == HAL_PASSTHROUGH_OFF) {
		LTRACE(TRACE_HOOK, "line %u slot %u V.21 preamble dropped, no "
		       "passthrough on this leg, the far end owns the T.38 offer",
		       l->index, slot);
		return;
	}

	/* the call runs outside the trace, which evaluates nothing while its
	 * domain is off */
	kind_set = call_fax_kind_set(l->index, slot, HAL_PASSTHROUGH_FAX);
	LTRACE(TRACE_FSM, "line %u slot %u V.21 preamble, the fax kind %s",
	       l->index, slot, kind_set ? "reached the leg" : "was refused");

	/* A remote tone or followed offer takes no local answer-tone latch. */
	if (!l->fax_switched[stream]) {
		LTRACE(TRACE_HOOK, "line %u slot %u V.21 preamble without a "
		       "local answer tone, the far end owns the T.38 offer",
		       l->index, slot);
		return;
	}

	if (!l->chan.fax_t38_enable) {
		LTRACE(TRACE_HOOK, "line %u V.21 preamble dropped, fax_t38_enable is 0",
		       l->index);
		return;
	}

	if (l->t38_switched[stream]) {
		LTRACE(TRACE_HOOK, "line %u slot %u already tried the T.38 upgrade",
		       l->index, slot);
		return;
	}

	answer = call_t38_start(l->index, slot);
	if (answer == CALL_T38_BUSY) {
		LTRACE(TRACE_HOOK, "line %u slot %u V.21 preamble held, an exchange of "
		       "that leg is still open", l->index, slot);
		return;
	}

	l->t38_switched[stream] = true;
	if (answer == CALL_T38_STARTED) {
		LTRACE(TRACE_FSM, "line %u slot %u V.21 preamble, upgrading to T.38",
		       l->index, slot);
		return;
	}

	LTRACE(TRACE_FSM, "line %u slot %u refused the T.38 upgrade, the call keeps "
	       "the media it has", l->index, slot);
}

/*
 * A remote answer tone records the hint and arms the modem kind on the
 * current codec.
 * It takes no latch and sends no offer. The kind cannot wait: a proxy, for
 * example Asterisk, can answer the far upspeed itself and pass no offer to the
 * caller. V.152 10.1.1 and 10.3 permit this transition.
 *
 * A local answer tone still converts a kind-only leg. T.30 6.1.7 permits the
 * calling terminal to answer a manual fax procedure. An existing profile
 * instead drops the tone before the latch, so a followed leg cannot offer T.38.
 *
 * This handler sends no FSM event because the switch changes nothing the
 * subscriber sees.
 * Only CONNECTED has one answered leg with running media. The local latch
 * records the attempt, even when an exchange in progress refuses it.
 * This avoids repeated offers after a peer refusal, but that state refusal
 * can cost the leg its upspeed if no later answer tone arrives.
 */
static void tone_event(struct sip_line *l, const struct proto_line_event *ev)
{
	unsigned int slot = l->fsm.active_slot;
	unsigned int stream;

	if (!fax_tone_note(l, ev, slot))
		return;

	if (!fax_tone_starts(ev->u.tone.code)) {
		t38_tone_event(l, ev, slot);
		return;
	}

	if (ev->u.tone.direction != HAL_TONE_DIR_LOCAL) {
		bool kind_set;

		if (l->fsm.state != FSM_CONNECTED || slot >= CALL_SLOTS) {
			LTRACE(TRACE_HOOK, "line %u fax tone %s from the wire "
			       "dropped in %s", l->index,
			       hal_tone_detect_name(ev->u.tone.code),
			       fsm_state_name(l->fsm.state));
			return;
		}

		call_fax_hint(l->index, slot);
		kind_set = call_fax_kind_set(l->index, slot,
					     HAL_PASSTHROUGH_MODEM);
		LTRACE(TRACE_HOOK, "line %u fax tone %s from the wire: "
		       "the modem kind %s, no offer", l->index,
		       hal_tone_detect_name(ev->u.tone.code),
		       kind_set ? "recorded on the leg" : "refused by the leg");
		return;
	}

	if (l->fsm.state != FSM_CONNECTED || slot >= CALL_SLOTS) {
		LTRACE(TRACE_HOOK, "line %u fax tone %s dropped in %s", l->index,
		       hal_tone_detect_name(ev->u.tone.code),
		       fsm_state_name(l->fsm.state));
		return;
	}

	if (call_fax_on(l->index, slot)) {
		LTRACE(TRACE_HOOK, "line %u slot %u fax tone %s dropped, "
		       "the profile already applies", l->index, slot,
		       hal_tone_detect_name(ev->u.tone.code));
		return;
	}

	stream = slot_stream(l->index, slot);
	if (l->fax_switched[stream]) {
		LTRACE(TRACE_HOOK, "line %u slot %u already switched to fax", l->index,
		       slot);
		return;
	}

	l->fax_switched[stream] = true;
	LTRACE(TRACE_FSM, "line %u slot %u fax tone %s, switching to passthrough",
	       l->index, slot, hal_tone_detect_name(ev->u.tone.code));

	if (call_fax_start(l->index, slot))
		return;

	LTRACE(TRACE_FSM, "line %u slot %u refused the fax switch, the call keeps "
	       "the voice path", l->index, slot);
}

/*
 * The first packet of an early stream (hal.h HAL_EVENT_RTP_FIRST_RX), which is
 * the moment RFC 3960 3.2 asks for: the far end is sending, so the local
 * ringback stops and the caller hears the wire.
 *
 * The core traces every record in a fixed shape (line_hal_event), because the
 * on-target procedure reads that line against the rtp_first_tx one. Only a
 * leg cb_proceeding() armed goes further; every other report names a stream
 * no gate waits on.
 */
static void rtp_first_rx_event(struct sip_line *l,
			       const struct proto_line_event *ev)
{
	unsigned int slot;

	if (ev->stream >= CALL_SLOTS || !l->early_rx_wait[ev->stream])
		return;

	slot = slot_ui_of(l->index, ev->stream);
	if (slot >= CALL_SLOTS)
		return;

	/* the gate stays armed when the core names no UI slot for the leg, the
	 * rule cb_started() follows on the same join */
	early_rx_clear(l, ev->stream);

	LTRACE(TRACE_FSM, "line %u slot %u early media flowing, so the "
	       "ringback stops", l->index, slot);

	line_ev_deliver(l, FSM_EV_CALL_PROCEEDING, (slot << 1) | 1u);
}

/*
 * The line's private copy of the channel, with the two derive-time guards on
 * its star codes. Both report a configuration fault; neither resolves one.
 *
 * 1. A code longer than the machine's digit buffer is blanked. The collector
 *    could never hold it, so it would drive a collection that can never
 *    complete and then hand the dial plan a truncated buffer. Blanked, the
 *    table behaves as if the option were unset and those digits reach the
 *    dial plan as ordinary digits.
 * 2. A code no subscriber can reach is named: two options carrying one
 *    string, where the table abstains and neither runs, and a code a shorter
 *    one shadows, where the shorter one fires first (features.h "Precedence").
 *
 * The cid_delivery_local mode selection uses the same blanking mechanism, and
 * is not a fault: see cid_codes_drop().
 */

/*
 * cid_delivery_local 0 gives the caller-id service to the network, so the
 * four CID codes must dial through as ordinary digits and the CPE must
 * synthesise no privacy of its own. Blanking them in this copy is exactly what
 * the overlength guard does: the table then behaves as if the options were
 * unset, no local toggle can fire, and the presentation of an outgoing call
 * follows cid_outgoing_enable alone.
 *
 * An unset option keeps the local service, because the local service is the
 * only one that works without a network subscription.
 */
static void cid_codes_drop(const struct sip_line *l, struct cfg_channel *dst)
{
	if (!dst->cid_delivery_local_set || dst->cid_delivery_local)
		return;

	dst->cid_single_call_enable_exten[0] = 0;
	dst->cid_single_call_disable_exten[0] = 0;
	dst->cid_enable_exten[0] = 0;
	dst->cid_disable_exten[0] = 0;
	LTRACE(TRACE_FSM, "line %u caller id restriction is a network service, "
	       "its four codes dial through", l->index);
}

/*
 * The same mechanism for the call forward service. call_fwd_local 0 gives it
 * to the network, so the thirteen codes dial through as ordinary digits and
 * this line arms no condition of its own; the shipped default is 1, the local
 * service. The enforcement half of the gate is in features.c, which seeds no
 * condition from a channel whose call_fwd_local is 0.
 *
 * The two do-not-disturb codes are not gated: no option selects a network
 * service for them, so the feature is local or it is nothing.
 */
static void cf_codes_drop(const struct sip_line *l, struct cfg_channel *dst)
{
	if (dst->call_fwd_local)
		return;

	dst->call_fwd_uncond_exten[0] = 0;
	dst->call_fwd_uncond_cancel_exten[0] = 0;
	dst->call_fwd_uncond_query_exten[0] = 0;
	dst->call_fwd_busy_exten[0] = 0;
	dst->call_fwd_busy_cancel_exten[0] = 0;
	dst->call_fwd_busy_query_exten[0] = 0;
	dst->call_fwd_no_answer_exten[0] = 0;
	dst->call_fwd_no_answer_cancel_exten[0] = 0;
	dst->call_fwd_no_answer_query_exten[0] = 0;
	dst->call_fwd_unreachable_exten[0] = 0;
	dst->call_fwd_unreachable_cancel_exten[0] = 0;
	dst->call_fwd_all_exten[0] = 0;
	dst->call_fwd_all_cancel_exten[0] = 0;
	LTRACE(TRACE_FSM, "line %u call forwarding is a network service, "
	       "its thirteen codes dial through", l->index);
}

/*
 * A condition the config arms with no target to send the call to. The machine
 * lets such an offer through to the treatment it would otherwise have had, and
 * this is where the operator hears about it: once per reload, per condition,
 * like every other derive-time fault of this file.
 */
static void cf_targets_check(const struct sip_line *l,
			     const struct cfg_channel *ch)
{
	static const struct {
		uint8_t bit;
		size_t uri_off;
		const char *name;
	} conds[] = {
		{ FEATURE_CF_BIT(FEATURE_CF_UNCOND),
		  offsetof(struct cfg_channel, call_fwd_uncond_uri),
		  "call_fwd_uncond_uri" },
		{ FEATURE_CF_BIT(FEATURE_CF_BUSY),
		  offsetof(struct cfg_channel, call_fwd_busy_uri),
		  "call_fwd_busy_uri" },
		{ FEATURE_CF_BIT(FEATURE_CF_NOANSWER),
		  offsetof(struct cfg_channel, call_fwd_no_answer_uri),
		  "call_fwd_no_answer_uri" },
	};
	unsigned int i;

	if (!ch->call_fwd_local)
		return;

	for (i = 0; i < sizeof(conds) / sizeof(conds[0]); i++) {
		const char *uri = (const char *)ch + conds[i].uri_off;
		char text[160];

		if (!(ch->call_fwd_local_mode & conds[i].bit) || *uri)
			continue;

		snprintf(text, sizeof(text),
			 "call_fwd_local_mode arms a condition with no %s, "
			 "so those calls are not forwarded", conds[i].name);
		line_fault_report(l, true, text);
	}
}

/*
 * A service the config arms whose cancel code the config does not carry, so
 * no subscriber can turn it off from the handset. Under star_codes_local 0
 * that is an ordinary deployment, the service on the CPE and every code on
 * the switch, so the report is quiet. The all-forward cancel clears the busy
 * and the no-answer condition too (FEATURE_ACT_ASCF_CLEAR), so it counts for
 * both.
 */
static void cancel_codes_check(const struct sip_line *l,
			       const struct cfg_channel *ch)
{
	static const struct {
		uint8_t bit;
		size_t code_off;
		bool all_cancels;
		const char *name;
	} conds[] = {
		{ FEATURE_CF_BIT(FEATURE_CF_UNCOND),
		  offsetof(struct cfg_channel, call_fwd_uncond_cancel_exten),
		  false, "unconditional" },
		{ FEATURE_CF_BIT(FEATURE_CF_BUSY),
		  offsetof(struct cfg_channel, call_fwd_busy_cancel_exten),
		  true, "busy" },
		{ FEATURE_CF_BIT(FEATURE_CF_NOANSWER),
		  offsetof(struct cfg_channel, call_fwd_no_answer_cancel_exten),
		  true, "no answer" },
	};
	unsigned int i;

	if (ch->do_not_disturb_enabled && !*ch->do_not_disturb_off_exten)
		line_fault_report(l, false, "do_not_disturb_enabled arms the "
				  "service and no code cancels it");

	if (!ch->call_fwd_local)
		return;

	for (i = 0; i < sizeof(conds) / sizeof(conds[0]); i++) {
		const char *code = (const char *)ch + conds[i].code_off;
		char text[160];

		if (!(ch->call_fwd_local_mode & conds[i].bit) || *code)
			continue;
		if (conds[i].all_cancels && *ch->call_fwd_all_cancel_exten)
			continue;

		snprintf(text, sizeof(text),
			 "call_fwd_local_mode arms the %s forward and no code "
			 "cancels it", conds[i].name);
		line_fault_report(l, false, text);
	}
}

/*
 * Whether the backend can carry the forward disconnect, resolved at the
 * derive so the policy asks the backend nothing. The core resolves its own
 * copy, and it reports a board that cannot carry the signal and a delay that
 * lands in a hook band (line/line.c fwd_disc_check).
 */
static void fwd_disc_avail_take(struct sip_line *l,
				const struct cfg_channel *ch)
{
	if (!cfg_fwd_disc_on(ch))
		return;

	l->fwd_disc_avail = (sip_host->hal_features(sip_host->ctx) &
			     HAL_FEAT_POLARITY) != 0;
}

/*
 * The SIP half of the region settle (ops->region_changed), the
 * conf_cap_settle shape: stage the live machine view when nothing is staged
 * and re-derive the fields the region moves. The forward disconnect is taken
 * again because the effective hook window can move the delay band, and the
 * flash re-derive runs because the flash service can move the conference
 * gate; a missing mixer already reported at derive time reports again, which
 * is the accepted cost.
 */
void sip_line_region_changed(unsigned int line,
			     const struct hal_region *region)
{
	struct sip_line *l = sip_line_get(line);

	if (!l)
		return;

	if (!l->cfg_pending) {
		l->dp_new = l->dp;
		l->fcfg_new = l->fcfg;
		l->chan_new = l->chan;
		l->cfg_pending = true;
	}

	l->fcfg_new.cw_offer_ms = region->cw_sas_total_ms;
	/* the staged channel copy, never the running one: a reload staged
	 * under a live call is the config that will land */
	flash_derive(l, &l->chan_new, &l->fcfg_new);
	fwd_disc_avail_take(l, &l->chan_new);
	line_queue_drain(l);
}

/*
 * The mixer capability, resolved and stored the way fwd_disc_avail_take()
 * resolves the polarity one: the join then asks the backend nothing, and a
 * board that cannot mix says so once per reload instead of once per flash.
 * No HAL at all carries no optional feature.
 */
static void conf_avail_take(struct sip_line *l)
{
	l->conf_avail = (sip_host->hal_features(sip_host->ctx) &
			 HAL_FEAT_CONF) != 0;
}

/* the flash service of the region, the same for every line */
static uint8_t flash_service(void)
{
	return sip_host->region(sip_host->ctx)->flash_service;
}

/* whether the channel asks for the local conference at all: the service and
 * the flash that reaches it (the TWO_CALLS FLASH cell) */
static bool conf_cfg_on(const struct cfg_channel *ch)
{
	return ch->local_conf_enabled != 0 &&
	       flash_service() != HAL_FLASH_SERVICE_OFF;
}

/*
 * What a flash does with the two calls (fsm.h enum fsm_conf_mode). The split
 * service splits the conference back into two calls and the drop service
 * drops the second one; a channel that asks for neither, and a board with no
 * mixer, leave the view at FSM_CONF_OFF, where the flash swaps the two
 * calls.
 */
static enum fsm_conf_mode conf_mode_of(const struct sip_line *l,
				       const struct cfg_channel *ch)
{
	if (!conf_cfg_on(ch) || !l->conf_avail)
		return FSM_CONF_OFF;

	return flash_service() == HAL_FLASH_SERVICE_DROP ?
	       FSM_CONF_DROP : FSM_CONF_SPLIT;
}

/*
 * The config asks for a conference the board cannot mix. It waits for the
 * final capability, which is what sip_cap_final says, so the report names
 * the capability of a started DSP.
 */
static void conf_check(const struct sip_line *l, const struct cfg_channel *ch)
{
	if (!sip_cap_final || l->conf_avail || !conf_cfg_on(ch))
		return;

	line_fault_report(l, true,
			  "the local conference is configured but this "
			  "backend carries no mixer, so the hook flash "
			  "swaps the two calls");
}

static void line_codes_take(struct sip_line *l, const struct cfg_channel *src,
			    struct cfg_channel *dst)
{
	unsigned int i;

	*dst = *src;
	cid_codes_drop(l, dst);
	cf_codes_drop(l, dst);
	cf_targets_check(l, dst);
	cancel_codes_check(l, dst);
	fwd_disc_avail_take(l, dst);
	conf_avail_take(l);

	for (i = FEATURE_ACT_NONE + 1; i < FEATURE_ACT_COUNT; i++) {
		enum feature_action act = (enum feature_action)i;
		char text[160];

		if (!feature_code_clamp(dst, act, FSM_DIGITS_MAX))
			continue;

		snprintf(text, sizeof(text),
			 "star code %s is longer than the %d digit buffer, ignored",
			 feature_action_name(act), FSM_DIGITS_MAX);
		line_fault_report(l, true, text);
	}

	for (i = FEATURE_ACT_NONE + 1; i < FEATURE_ACT_COUNT; i++) {
		enum feature_action act = (enum feature_action)i;
		enum feature_action other;
		char text[160];
		bool loud = false;

		switch (feature_fault_of(dst, act, &other)) {
		case FEATURE_FAULT_DUPLICATE:
			loud = feature_action_supported(act) ||
			       feature_action_supported(other);
			snprintf(text, sizeof(text),
				 "star codes %s and %s are both '%s', so neither runs",
				 feature_action_name(act), feature_action_name(other),
				 feature_code_of(dst, act));
			break;
		case FEATURE_FAULT_SHADOWED:
			loud = feature_action_supported(act);
			snprintf(text, sizeof(text),
				 "star code %s '%s' is unreachable: %s '%s' fires first",
				 feature_action_name(act), feature_code_of(dst, act),
				 feature_action_name(other), feature_code_of(dst, other));
			break;
		case FEATURE_FAULT_NONE:
			continue;
		}

		line_fault_report(l, loud, text);
	}
}

/*
 * Who owns the hook flash of a connected leg. State first, and the CONNECTED
 * FLASH cell is the state gate; here two config gates decide, in this order.
 *
 * 1. Local feature ownership wins. local_conf_enabled other than 0 keeps the
 *    flash for the local conference service and no INFO reaches the wire.
 *    With the shipped defaults, sip_flash_relay=info and
 *    local_conf_enabled=1, that is exactly what happens.
 * 2. sip_flash_relay then picks the form. rfc2833 has no path on any backend:
 *    stream_dtmf_send carries one DTMF character, and the udsp
 *    telephone-event sender maps events 0 to 15 only. So it relays as info,
 *    with one warning here rather than a silent failure at run time.
 *
 * The region's flash service gates the local branch alone and takes no part
 * in this.
 */
static bool flash_relay_derive(struct sip_line *l, const struct cfg_channel *ch)
{
	if (ch->local_conf_enabled != 0)
		return false;

	if (ch->sip_flash_relay == CFG_FLASH_RELAY_RFC2833)
		line_fault_report(l, true,
				  "sip_flash_relay rfc2833 has no path on this hardware, "
				  "relaying the hook flash as info");

	return ch->sip_flash_relay != CFG_FLASH_RELAY_OFF;
}

/*
 * CFG_STR_MAX is wider than the machine's digit buffer, so a warmline number
 * the subscriber can configure is longer than one the line can dial.
 *
 * A number that does not fit is treated as absent, which is the same
 * mechanism the overlength star codes take: the view carries an empty string,
 * so both places that dial a warmline see "no number" and the subscriber gets
 * the fast busy an unusable warmline earns. Dialling a cut number is the worse
 * answer, because it reaches a different party than the config names, which
 * is the same hazard the star code target collector guards against. The
 * operator hears about it once per reload.
 */
static bool warmline_fits(const struct cfg_channel *ch)
{
	return strlen(ch->warmline_number) <= FSM_DIGITS_MAX;
}

static void warmline_check(const struct sip_line *l,
			   const struct cfg_channel *ch)
{
	char text[160];

	if (!ch->warmline_enable || warmline_fits(ch))
		return;

	snprintf(text, sizeof(text),
		 "warmline_number is longer than the %d digit buffer, so the line "
		 "gives reorder instead of dialling a cut number", FSM_DIGITS_MAX);
	line_fault_report(l, true, text);
}

static void line_cfg_derive(struct sip_line *l, const struct cfg *cfg,
			    struct dialplan *dp, struct fsm_cfg *out)
{
	const struct cfg_channel *ch = &cfg->channel[l->index];

	/* fsm.h: cfg.dp is never NULL, so the default map is the unconditional
	 * fallback for every refused map. The config loader reports a refused
	 * map with a warning that names the section, so this path keeps the
	 * trace alone. */
	if (!dialplan_compile(ch->digit_map, dp)) {
		LTRACE(TRACE_DIALPLAN, "line %u digit map refused, using \"%s\"", l->index,
		       DIALPLAN_DEFAULT_MAP);
		dialplan_compile(DIALPLAN_DEFAULT_MAP, dp);
	}

	memset(out, 0, sizeof(*out));
	out->dp = &l->dp;
	/*
	 * Both star code hooks point at storage this line owns, so a staged
	 * view carries the same two pointers as the live one and the swap in
	 * line_cfg_apply moves the content, never the pointer.
	 */
	out->features = &l->features;
	out->feature_codes = &l->chan;
	out->start_digit_ms = sec_to_ms(ch->start_digit_s);
	out->interdigit_short_ms = sec_to_ms(ch->interdigit_s);
	out->interdigit_long_ms = sec_to_ms(ch->interdigit_long_s);
	out->busy_tone_ms = sec_to_ms(ch->busy_tone_s);
	out->roh_ms = sec_to_ms(ch->roh_s);
	out->warmline_ms = sec_to_ms(ch->warmline_timeout_s);
	/* ch->digit_esc travels to call.c in the leg's cfg_channel snapshot */
	out->cw_offer_ms = sip_host->region(sip_host->ctx)->cw_sas_total_ms;
	/* the configured value, not the effective one: this view lands with the
	 * feature state reset, and op_feature_invoke owns the runtime override
	 * in between */
	out->call_waiting = ch->call_waiting_enabled;
	/* the alert source rides the same staged view, so it lands at the rest
	 * state the rest of the config lands at */
	out->cw_mode = cw_mode_of(ch);
	/* the effective audible indication: what the switch said, gated by the
	 * amwi of the config being derived. line_cfg_apply pushes it again from
	 * the store, because a NOTIFY may land between the two. */
	out->mwi = mwi_stored(l) && ch->amwi;
	out->warmline = ch->warmline_enable;
	/* an overlength number lands empty, which is what turns both fire
	 * points into the reorder branch (warmline_check above) */
	if (warmline_fits(ch))
		str_copy(out->warmline_number, sizeof(out->warmline_number),
			 ch->warmline_number);
	warmline_check(l, ch);

	out->flash_relay = flash_relay_derive(l, ch);
	flash_derive(l, ch, out);
}

/*
 * The three fields the region's flash service derives, in one place because
 * a region read-back moves them under a running config
 * (sip_line_region_changed) and a reload moves them with the rest of the
 * view (line_cfg_derive). The relay stays out of it: it does not read the
 * flash service, and a re-derive here must not report the rfc2833 fault a
 * second time.
 */
static void flash_derive(struct sip_line *l, const struct cfg_channel *ch,
			 struct fsm_cfg *out)
{
	/* the off service hands the hook flash to the network as a plain
	 * event, so the local flash-hold path of CONNECTED is off; split and
	 * drop are the two local services and keep it on. */
	out->flash_features = flash_service() != HAL_FLASH_SERVICE_OFF;
	/*
	 * The on-hook consult transfer of TWO_CALLS without a star code: the
	 * conference service in its transfer mode (local_conf_enabled 2) and
	 * the drop flash service of the region. The third condition, the held
	 * leg being the incoming one, is a cell guard the machine reads from
	 * its own slot_inbound[] and never a config value, so it is not
	 * derived here.
	 */
	out->xfer_on_hook = ch->local_conf_enabled == 2 &&
			    flash_service() == HAL_FLASH_SERVICE_DROP;
	/* the conference the TWO_CALLS flash offers, and what the CONF flash
	 * then does with it */
	out->conf_mode = conf_mode_of(l, ch);
	conf_check(l, ch);
}

/*
 * The one derived value the two halves of the bring-up can disagree about.
 * The init reads the capability before the DSP starts, so the mode it
 * derived is provisional; each ops->dsp_ready that carries the final
 * capability settles the mode and the fault here (sip_lines_dsp_ready).
 *
 * Two shapes, by whether a reload is already staged. A staged view is the one
 * that will land, so the mode is derived from its channel copy and written
 * into it alone: seeding the staged copies again would replace the reload with
 * the running config. With nothing staged the applied copies are the ones that
 * will land, so they seed a staging of their own and exactly one value of the
 * view moves. No other derive-time fault is reported a second time either way.
 *
 * The view lands through fsm_cfg_update() like every other one, which is at
 * IDLE or OOS, and a machine that refuses it keeps the pending view for the
 * next drain. It lands through the apply alone and not through a drain: the
 * core runs its own service sync and message waiting presentation behind
 * ops->dsp_ready, and a settle here would present a held record in front of
 * them. The core runs the settle of a landed view behind its presentation
 * (line/line.c lines_start()). A landing view also resets the runtime star code
 * state, which every applied view does; at the bring-up that state was just
 * initialised, so a subscriber loses no toggle they could have made.
 */
static void conf_cap_settle(struct sip_line *l)
{
	/* conf_mode alone moves here, not the whole flash_derive: of the three
	 * flash-derived fields it is the only one that reads conf_avail, and
	 * the mode == live return below is what keeps a bring-up from landing
	 * a view for nothing */
	bool staged = l->cfg_pending;
	const struct cfg_channel *ch = staged ? &l->chan_new : &l->chan;
	enum fsm_conf_mode live = staged ? l->fcfg_new.conf_mode :
				  l->fcfg.conf_mode;
	enum fsm_conf_mode mode;

	conf_avail_take(l);
	conf_check(l, ch);

	mode = conf_mode_of(l, ch);
	if (mode == live)
		return;

	if (!staged) {
		l->dp_new = l->dp;
		l->fcfg_new = l->fcfg;
		l->chan_new = l->chan;
		l->cfg_pending = true;
	}

	l->fcfg_new.conf_mode = mode;
	line_cfg_apply(l);
}

/*
 * The SIP half of the cancel: the deferred drain, the delayed forward
 * disconnect window, the answer tone deferral of each leg and the three
 * timers of the machine.
 */
void sip_line_reset(unsigned int line)
{
	struct sip_line *l = sip_line_get(line);
	unsigned int stream;

	if (!l)
		return;

	uloop_timeout_cancel(&l->drain);
	uloop_timeout_cancel(&l->fwd_disc_hold);
	/* in front of the re-init memset that would corrupt the uloop list an
	 * armed timer is linked into */
	for (stream = 0; stream < CALL_SLOTS; stream++)
		fax_exit_cancel(l, stream);
	fsm_timers_stop(&l->fsm);
}

/*
 * The clearing class the machine is holding, for the status page: busy tone,
 * reorder, number-unobtainable tone or silence, which is the difference between
 * "the far end was busy" and "the call never got out" on a line that reads as
 * CLEARING either way. fsm.h owns the enum and fsm.clearing is the only copy.
 */
static const char *clearing_name(enum fsm_clearing cls)
{
	switch (cls) {
	case FSM_CLR_BUSY:
		return "busy";
	case FSM_CLR_FASTBUSY:
		return "fastbusy";
	case FSM_CLR_UNOBTAINABLE:
		return "unobtainable";
	case FSM_CLR_QUIET:
		return "quiet";
	}

	return "?";
}

/*
 * The armed forward conditions, one key each, present only while the condition
 * is armed. The value is the target the enforcement uses, which is empty for
 * the config that arms a condition without one.
 */
static void cf_dump(struct blob_buf *b, const struct feature_state *st)
{
	static const char *const keys[FEATURE_CF_COUNT] = {
		"cf_uncond", "cf_busy", "cf_no_answer",
	};
	unsigned int i;

	for (i = 0; i < FEATURE_CF_COUNT; i++) {
		const char *target;

		if (!feature_cf_armed(st, (enum feature_cf)i, &target))
			continue;

		blobmsg_add_string(b, keys[i], target);
	}
}

/*
 * One per-slot latch, written as an array so the slot survives the render, and
 * absent while no slot holds it: under the presence rule of this dump, a key
 * that is there describes something, and a line that never faxed describes
 * nothing.
 */
static void latch_dump(struct blob_buf *b, const char *key, const bool *latch)
{
	unsigned int i;
	void *arr;

	for (i = 0; i < CALL_SLOTS; i++) {
		if (latch[i])
			break;
	}

	if (i == CALL_SLOTS)
		return;

	arr = blobmsg_open_array(b, key);
	for (i = 0; i < CALL_SLOTS; i++)
		blobmsg_add_u8(b, NULL, latch[i]);
	blobmsg_close_array(b, arr);
}

/* ------------------------------------------ the entries of proto/sip/line.h */

/*
 * The machines of every line, over the views derived from cfg. The core
 * cancelled every timer of the line (ops->line_reset) in front of this, so
 * the storage can be cleared.
 */
void sip_lines_init(const struct proto_host *host, const struct cfg *cfg)
{
	unsigned int i;

	sip_host = host;
	sip_stopping = false;
	sip_cap_final = false;
	sip_dsp_ready_calls = 0;
	sip_txn_env_uloop_init(&line_env);

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct sip_line *l = &sip_lines[i];

		memset(l, 0, sizeof(*l));
		l->index = i;
		line_codes_take(l, &cfg->channel[i], &l->chan);
		feature_state_init(&l->features, &l->chan);
		line_cfg_derive(l, cfg, &l->dp, &l->fcfg);
		fsm_init(&l->fsm, i, &line_env, &l->fcfg, &line_ops, l);
	}
}

/*
 * The first call after the init carries the provisional capability of the
 * attach, and every later one the final capability (proto/proto.h dsp_ready).
 * The conference mode is settled against the final one alone.
 */
void sip_lines_dsp_ready(void)
{
	unsigned int i;

	if (++sip_dsp_ready_calls < 2)
		return;

	sip_cap_final = true;

	for (i = 0; i < CFG_CHANNEL_MAX; i++)
		conf_cap_settle(&sip_lines[i]);
}

void sip_lines_stop_begin(void)
{
	sip_stopping = true;
}

const struct call_cb *sip_line_call_cb(void)
{
	return &line_call_cb;
}

static void sip_hook_take(struct sip_line *l, uint8_t state)
{
	bool dial_ready;

	switch (state) {
	case HAL_HOOK_SEIZE:
		dial_ready = calls_line_dial_ready(l->index);
		if (!dial_ready)
			LTRACE(TRACE_FSM, "line %u seized with no account, "
			       "target or live registration to dial "
			       "through", l->index);
		fsm_dial_ready_set(&l->fsm, dial_ready);

		line_ev_deliver(l, FSM_EV_SEIZE, 0);
		return;
	case HAL_HOOK_RELEASE:
		/* the held visual indication of an on-hook edge is presented by
		 * the drain inside this call, behind the config apply */
		line_ev_deliver(l, FSM_EV_RELEASE, 0);
		return;
	case HAL_HOOK_FLASH:
		/* the CLEARING FLASH cell reads the account as it is now, the
		 * way the IDLE SEIZE cell does */
		fsm_dial_ready_set(&l->fsm, calls_line_dial_ready(l->index));
		line_ev_deliver(l, FSM_EV_FLASH, 0);
		return;
	}
}

/*
 * A digit from the phone. A pulse digit carries no detector window. A tone
 * digit opens the one window op_call_dtmf() reads for the length the detector
 * measured, for the frame of the delivery.
 */
static void sip_digit_take(struct sip_line *l,
			   const struct proto_line_event *ev)
{
	uint32_t arg = (uint32_t)(unsigned char)ev->u.digit.digit;

	if (ev->u.digit.pulse) {
		line_ev_deliver(l, FSM_EV_DIGIT, arg);
		return;
	}

	l->digit_detected = ev->u.digit.digit;
	l->digit_detected_ms = ev->u.digit.duration_ms;
	l->digit_detected_valid = true;
	line_ev_deliver(l, FSM_EV_DIGIT, arg);
	l->digit_detected_valid = false;
}

void sip_line_event(unsigned int line, const struct proto_line_event *ev)
{
	struct sip_line *l = sip_line_get(line);

	if (!l)
		return;

	switch (ev->type) {
	case PROTO_EV_HOOK:
		sip_hook_take(l, ev->u.hook.state);
		return;
	case PROTO_EV_DIGIT:
		sip_digit_take(l, ev);
		return;
	case PROTO_EV_TONE:
		tone_event(l, ev);
		return;
	case PROTO_EV_FIRST_RTP:
		rtp_first_rx_event(l, ev);
		return;
	case PROTO_EV_RING_END:
	case PROTO_EV_LINE_STATE:
		return;
	}
}

/* The mapping table of proto/proto.h enum proto_line_flag. */
static uint32_t sip_line_flags_of(const struct sip_line *l)
{
	uint32_t held = l->fsm.other_held ? PROTO_LF_CALL : 0;

	switch (l->fsm.state) {
	case FSM_IDLE:
	case FSM_OOS:
		return 0;
	case FSM_OFFHOOK_DIAL:
	case FSM_COLLECTING:
	case FSM_SPRE_COLLECT:
	case FSM_CLEARING:
		return PROTO_LF_OFFHOOK | PROTO_LF_SESSION | held;
	case FSM_ROH:
	case FSM_SILENT:
		return PROTO_LF_OFFHOOK | held;
	case FSM_RINGING:
	case FSM_HOLDING:
		return PROTO_LF_CALL;
	default:
		return PROTO_LF_OFFHOOK | PROTO_LF_CALL;
	}
}

uint32_t sip_line_flags(unsigned int line)
{
	const struct sip_line *l = sip_line_get(line);

	return l ? sip_line_flags_of(l) : 0;
}

const char *sip_line_state_name(unsigned int line)
{
	const struct sip_line *l = sip_line_get(line);

	return l ? fsm_state_name(l->fsm.state) : NULL;
}

/*
 * The service move of the core. Out for a fault: the machine's own force
 * alone, which ends each leg the way its state asks. Out for any other
 * reason: the legs go first, then the machine is forced out. Inside the
 * daemon stop the legs end at once, because the backend closes behind
 * lines_done() and a leg left to finish its BYE would stop its media on a
 * closed handle. In: the machine is cleared, and the drain inside presents
 * what the line came back owing.
 */
void sip_line_service(unsigned int line, bool in_service,
		      enum proto_service_reason reason)
{
	struct sip_line *l = sip_line_get(line);

	if (!l)
		return;

	if (in_service) {
		line_oos_clear(l);
		return;
	}

	if (reason == PROTO_SERVICE_FAULT) {
		line_oos_force(l);
		return;
	}

	if (sip_stopping)
		calls_line_end(line);
	else
		calls_line_teardown(line);
	line_oos_force(l);
}

/*
 * The core staged its channel copy for a reload (lines_reload). The machine
 * view is derived from the same config and lands with it at the next rest
 * state, through the drain.
 */
void sip_line_cfg_changed(unsigned int line)
{
	struct sip_line *l = sip_line_get(line);
	const struct cfg *cfg = sip_host->cfg(sip_host->ctx);

	if (!l || !cfg)
		return;

	line_codes_take(l, &cfg->channel[line], &l->chan_new);
	line_cfg_derive(l, cfg, &l->dp_new, &l->fcfg_new);
	l->cfg_pending = true;
	line_queue_drain(l);
}

void sip_line_ring_held(unsigned int line)
{
	struct sip_line *l = sip_line_get(line);

	if (l)
		fsm_ring_held(&l->fsm);
}

void sip_line_ring_started(unsigned int line)
{
	struct sip_line *l = sip_line_get(line);

	if (l)
		fsm_ring_started(&l->fsm);
}

void sip_line_ring_failed(unsigned int line)
{
	struct sip_line *l = sip_line_get(line);

	if (!l)
		return;

	l->fsm_depth++;
	fsm_ring_failed(&l->fsm);
	l->fsm_depth--;

	line_queue_drain(l);
}

bool sip_line_digit_play_allowed(unsigned int line, unsigned int stream)
{
	const struct sip_line *l = sip_line_get(line);

	return l && digit_play_allowed(l, slot_ui_of(line, stream));
}

unsigned int sip_line_stream_of_slot(unsigned int line, unsigned int slot)
{
	unsigned int stream;

	if (slot >= CALL_SLOTS)
		return HAL_STREAM_MAX;

	stream = call_slot_stream(line, slot);

	return stream >= CALL_SLOTS ? HAL_STREAM_MAX : stream;
}

/*
 * The audible half of a NOTIFY: the machine holds the effective audible
 * flag, so the next dial tone knows about a NOTIFY that arrived while the
 * line was off hook. The core stored the record first (proto/proto.h mwi_set).
 */
void sip_line_mwi_push(unsigned int line)
{
	struct sip_line *l = sip_line_get(line);

	if (l)
		fsm_mwi_set(&l->fsm, mwi_audible(l));
}

/*
 * The collected number as dialplan.h reports it: without the timer markers.
 * A buffer of markers alone reports no key, as an empty buffer does.
 */
static void digits_dump(struct blob_buf *b, const struct fsm *f)
{
	char number[FSM_DIGITS_MAX + 1];

	if (dialplan_digits_strip(f->digits, number, sizeof(number)))
		blobmsg_add_string(b, "digits", number);
}

/* The machine keys of one call[] entry (proto/proto.h status_dump_call). */
void sip_line_status_dump(struct blob_buf *b, unsigned int line)
{
	const struct sip_line *l = sip_line_get(line);
	bool cid_once, cid_restrict;

	if (!l)
		return;

	/* the delayed forward disconnect window a clearing armed, present only
	 * while it is armed; it is not a uci value, so this dump is the only
	 * place it can be read */
	if (l->fwd_disc_hold.pending)
		blobmsg_add_u8(b, "fwd_disc_armed", 1);
	blobmsg_add_string(b, "state", fsm_state_name(l->fsm.state));
	/* the effective value, so a star code the subscriber dialled is
	 * visible next to the state it changes; runtime feature state is kept
	 * out of uci, so this dump is the only place it can be read */
	blobmsg_add_u8(b, "call_waiting", feature_cw_effective(&l->features,
							      &l->chan));
	/* the do-not-disturb bit and the armed forward conditions, for the
	 * same reason call_waiting is here */
	blobmsg_add_u8(b, "dnd", feature_dnd_is(&l->features));
	cf_dump(b, &l->features);
	if (feature_cid_once_peek(&l->features, &cid_once))
		blobmsg_add_u8(b, "cid_once", cid_once);
	/* the standing restriction, absent until the subscriber dials one of
	 * the persistent codes: true is the restriction on, so the number is
	 * withheld, which is the opposite polarity to cid_once above and the
	 * reason the two keys carry different names */
	if (feature_cid_restrict_peek(&l->features, &cid_restrict))
		blobmsg_add_u8(b, "cid_restrict", cid_restrict);
	if (l->fsm.state == FSM_CLEARING)
		blobmsg_add_string(b, "clearing", clearing_name(l->fsm.clearing));
	/* the local three-way conference, present only in that state. The
	 * mixer also runs through the PENDING of an outstanding split, which
	 * this key does not name */
	if (l->fsm.state == FSM_CONF)
		blobmsg_add_u8(b, "conf", 1);
	/* the two fax latches, each present only while this line holds one.
	 * The array index is the slot, which is what tells a two-call line
	 * which of its legs switched */
	latch_dump(b, "fax_switched", l->fax_switched);
	latch_dump(b, "t38_switched", l->t38_switched);
	digits_dump(b, &l->fsm);
	if (l->qcount)
		blobmsg_add_u32(b, "deferred", l->qcount);
	if (l->deferred_lost)
		blobmsg_add_u32(b, "deferred_lost", l->deferred_lost);
}
