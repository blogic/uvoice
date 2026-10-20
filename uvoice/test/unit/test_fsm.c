// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the FXS call control machine (src/proto/sip/fsm.c).
 *
 * The suite mirrors the matrix of the machine, and the matrix is the
 * contract that this file implements. The matrix has 19
 * states by 22 events, which is 418 cells. The contract asks for at least one
 * case per cell and one case per guard branch. The runner checks a coverage
 * table at the end. A case sets the state, the slot roles, the pending record
 * and the digit buffer directly, and injects exactly one event. Then it asserts
 * the exact action list in order and the exact resulting state.
 *
 * The environment is the virtual clock of test_sip_txn.c: a timer table that
 * the case advances by hand. Every FSM timer is therefore observable to the
 * millisecond without a sleep. The action vtable records each request and does
 * not act on it, so the tone, ring and call requests are observable in order
 * too. The fake timer ops write each timer arm and each cancel to the same log
 * as the vtable calls. A case can therefore assert the order of an action and a
 * timer.
 *
 * The suite stubs the dial plan on purpose, because src/dialplan.c is a
 * separate module. The four-line mapping of dp_action_of is part of the stable
 * contract of dialplan.h. A copy of that mapping keeps this suite independent
 * of the matcher.
 *
 * The suite stubs the star code table for the same reason:
 * src/proto/sip/features.c is its own module with its own suite. From the
 * table, the matrix needs the three verdicts of a match, whether a row takes a
 * target, and the transfer code lookups. It also needs the answer of the two
 * invoke ops, the ring-no-answer window, and the do-not-disturb and call
 * forward state that the enforcement reads. A case scripts each of those
 * directly.
 */
#include <stdlib.h>

#include "proto/sip/call.h"
#include "proto/sip/fsm.h"
#include "cfg/cfg.h"
#include "test_util.h"

#define FSM_STATES		19

_Static_assert(FSM_STATES * FSM_EV_COUNT == 418, "the matrix has 418 cells");
_Static_assert(FSM_EV_COUNT == 22, "the matrix has 22 events");
_Static_assert(FSM_SILENT == FSM_STATES - 1, "the matrix has 19 states");

#define FAKE_TIMER_MAX		8
#define ACT_MAX			16

/* ------------------------------------------------------------ environment */

static struct {
	struct sip_txn_env env;
	uint64_t now;
	struct sip_timer *timer[FAKE_TIMER_MAX];
	unsigned int timers;
	unsigned int fired;
} fk;

static struct fsm f;

enum timer_idx {
	TI_DIGIT = 0,
	TI_CW,
	TI_CLEARING,
	TI_NONE,
};

static const char *const timer_names[] = { "t_digit", "t_cw", "t_clearing", "?" };

static enum timer_idx timer_idx_of(const struct sip_timer *t)
{
	if (t == &f.t_digit)
		return TI_DIGIT;
	if (t == &f.t_cw)
		return TI_CW;
	if (t == &f.t_clearing)
		return TI_CLEARING;

	return TI_NONE;
}

/* --------------------------------------------------------------- action log */

enum act_kind {
	A_DIAL = 0,
	A_RING180,
	A_ANSWER,
	A_REJECT,
	A_HANGUP,
	A_HOLD,
	A_RESUME,
	A_DTMF,
	A_TONE,
	A_BELL_START,
	A_BELL_STOP,
	A_CW,
	A_CW_NET,
	A_FLASH_RELAY,
	A_FEATURE,
	A_FEATURE_ARG,
	A_FORWARD,
	A_XFER,
	A_XFER_CONSULT,
	A_CONF,
	A_ARM,
	A_CANCEL,
};

static const char *const act_names[] = {
	"call_dial", "call_ring", "call_answer", "call_reject", "call_hangup",
	"call_hold", "call_resume", "call_dtmf", "tone", "ring_start", "ring_stop",
	"cw_alert", "cw_net_alert", "flash_relay", "feature_invoke",
	"feature_invoke_arg", "call_forward", "call_xfer", "call_xfer_consult",
	"conf_set", "timer_arm", "timer_cancel",
};

static const char *const tone_names[] = {
	"NONE", "DIAL", "STUTTER", "SECOND_DIAL", "RINGBACK", "BUSY", "REORDER",
	"SIT", "CALL_WAITING", "CONFIRM", "HOWLER", "SHORT_STUTTER_DIAL",
	"NUMBER_UNOBTAINABLE",
};

struct act {
	enum act_kind kind;
	unsigned int slot;		/* timer index for A_ARM and A_CANCEL */
	uint32_t val;			/* code, tone, ms, digit, cw on/off */
	char num[FSM_DIGITS_MAX + 1];	/* number, star code, forward target */
	char arg[FSM_DIGITS_MAX + 1];	/* the collected target of A_FEATURE_ARG */
};

static struct act got[ACT_MAX];
static struct act want[ACT_MAX];
static unsigned int ngot;
static unsigned int nwant;
static unsigned int got_overflow;

static void acts_reset(void)
{
	ngot = 0;
	nwant = 0;
	got_overflow = 0;
}

static struct act *got_add(enum act_kind kind)
{
	static struct act sink;
	struct act *a;

	if (ngot >= ACT_MAX) {
		got_overflow++;
		a = &sink;
	} else {
		a = &got[ngot++];
	}

	memset(a, 0, sizeof(*a));
	a->kind = kind;

	return a;
}

static struct act *want_add(enum act_kind kind)
{
	struct act *a = &want[nwant < ACT_MAX ? nwant++ : ACT_MAX - 1];

	memset(a, 0, sizeof(*a));
	a->kind = kind;

	return a;
}

static void w_dial(unsigned int slot, const char *number)
{
	struct act *a = want_add(A_DIAL);

	a->slot = slot;
	snprintf(a->num, sizeof(a->num), "%s", number);
}

static void w_ring180(unsigned int slot)
{
	want_add(A_RING180)->slot = slot;
}

static void w_answer(unsigned int slot)
{
	want_add(A_ANSWER)->slot = slot;
}

static void w_reject(unsigned int slot, uint32_t code)
{
	struct act *a = want_add(A_REJECT);

	a->slot = slot;
	a->val = code;
}

static void w_hangup(unsigned int slot)
{
	want_add(A_HANGUP)->slot = slot;
}

static void w_hold(unsigned int slot)
{
	want_add(A_HOLD)->slot = slot;
}

static void w_resume(unsigned int slot)
{
	want_add(A_RESUME)->slot = slot;
}

static void w_dtmf(char digit)
{
	want_add(A_DTMF)->val = (uint32_t)(unsigned char)digit;
}

static void w_tone(enum hal_tone tone)
{
	want_add(A_TONE)->val = (uint32_t)tone;
}

static void w_bell_start(unsigned int slot)
{
	want_add(A_BELL_START)->slot = slot;
}

static void w_bell_stop(void)
{
	want_add(A_BELL_STOP);
}

static void w_cw(unsigned int slot, bool on)
{
	struct act *a = want_add(A_CW);

	a->slot = slot;
	a->val = on ? 1 : 0;
}

static void w_cw_net(bool on)
{
	want_add(A_CW_NET)->val = on ? 1 : 0;
}

static void w_flash_relay(void)
{
	want_add(A_FLASH_RELAY);
}

static void w_feature(const char *code)
{
	struct act *a = want_add(A_FEATURE);

	snprintf(a->num, sizeof(a->num), "%s", code);
}

static void w_feature_arg(const char *code, const char *arg)
{
	struct act *a = want_add(A_FEATURE_ARG);

	snprintf(a->num, sizeof(a->num), "%s", code);
	snprintf(a->arg, sizeof(a->arg), "%s", arg);
}

static void w_forward(unsigned int slot, const char *target)
{
	struct act *a = want_add(A_FORWARD);

	a->slot = slot;
	snprintf(a->num, sizeof(a->num), "%s", target);
}

static void w_xfer(unsigned int slot, enum feature_action act, const char *target)
{
	struct act *a = want_add(A_XFER);

	a->slot = slot;
	a->val = (uint32_t)act;
	snprintf(a->num, sizeof(a->num), "%s", target);
}

static void w_xfer_consult(unsigned int referee, unsigned int replaced)
{
	struct act *a = want_add(A_XFER_CONSULT);

	a->slot = referee;
	a->val = replaced;
}

static void w_conf(bool on)
{
	want_add(A_CONF)->val = on ? 1 : 0;
}

static void w_arm(enum timer_idx ti, uint32_t ms)
{
	struct act *a = want_add(A_ARM);

	a->slot = ti;
	a->val = ms;
}

static void w_cancel(enum timer_idx ti)
{
	want_add(A_CANCEL)->slot = ti;
}

static bool act_eq(const struct act *a, const struct act *b)
{
	if (a->kind != b->kind || a->slot != b->slot || a->val != b->val)
		return false;
	if (strcmp(a->arg, b->arg))
		return false;

	return strcmp(a->num, b->num) == 0;
}

static void act_fmt(const struct act *a, char *out, size_t olen)
{
	switch (a->kind) {
	case A_DIAL:
		snprintf(out, olen, "call_dial(%u, \"%s\")", a->slot, a->num);
		return;
	case A_TONE:
		snprintf(out, olen, "tone(%s)",
			 a->val < sizeof(tone_names) / sizeof(tone_names[0]) ?
			 tone_names[a->val] : "?");
		return;
	case A_REJECT:
		snprintf(out, olen, "call_reject(%u, %u)", a->slot, a->val);
		return;
	case A_DTMF:
		snprintf(out, olen, "call_dtmf('%c')", (char)a->val);
		return;
	case A_CW:
		snprintf(out, olen, "cw_alert(%u, %s)", a->slot, a->val ? "on" : "off");
		return;
	case A_CW_NET:
		snprintf(out, olen, "cw_net_alert(%s)", a->val ? "on" : "off");
		return;
	case A_FLASH_RELAY:
		snprintf(out, olen, "flash_relay()");
		return;
	case A_FEATURE:
		snprintf(out, olen, "feature_invoke(\"%s\")", a->num);
		return;
	case A_FEATURE_ARG:
		snprintf(out, olen, "feature_invoke_arg(\"%s\", \"%s\")", a->num,
			 a->arg);
		return;
	case A_FORWARD:
		snprintf(out, olen, "call_forward(%u, \"%s\")", a->slot, a->num);
		return;
	case A_XFER:
		snprintf(out, olen, "call_xfer(%u, act %u, \"%s\")", a->slot, a->val,
			 a->num);
		return;
	case A_XFER_CONSULT:
		snprintf(out, olen, "call_xfer_consult(%u, %u)", a->slot, a->val);
		return;
	case A_CONF:
		snprintf(out, olen, "conf_set(%s)", a->val ? "on" : "off");
		return;
	case A_ARM:
		snprintf(out, olen, "arm %s %u ms", timer_names[a->slot], a->val);
		return;
	case A_CANCEL:
		snprintf(out, olen, "cancel %s", timer_names[a->slot]);
		return;
	case A_BELL_STOP:
		snprintf(out, olen, "ring_stop()");
		return;
	default:
		snprintf(out, olen, "%s(%u)", act_names[a->kind], a->slot);
		return;
	}
}

static void acts_check_at(const char *file, int line)
{
	char buf[96];
	unsigned int i;
	bool ok = ngot == nwant && got_overflow == 0;

	for (i = 0; ok && i < ngot; i++)
		ok = act_eq(&got[i], &want[i]);

	tu_check(ok, "action list", file, line);
	if (ok)
		return;

	for (i = 0; i < nwant; i++) {
		act_fmt(&want[i], buf, sizeof(buf));
		printf("      want[%u] %s\n", i, buf);
	}
	for (i = 0; i < ngot; i++) {
		act_fmt(&got[i], buf, sizeof(buf));
		printf("      got [%u] %s\n", i, buf);
	}
}

#define ACTS()			acts_check_at(__FILE__, __LINE__)
#define STATE(st)		tu_check_uint(f.state, (st), "state", __FILE__, __LINE__)
#define CLASS(cl)		tu_check_uint(f.clearing, (cl), "clearing class", \
					      __FILE__, __LINE__)

/* --------------------------------------------------------------- the vtable */

static void op_dial(void *priv, unsigned int slot, const char *number)
{
	struct act *a = got_add(A_DIAL);

	a->slot = slot;
	snprintf(a->num, sizeof(a->num), "%s", number ? number : "");
}

static void op_ring(void *priv, unsigned int slot)
{
	got_add(A_RING180)->slot = slot;
}

static void op_answer(void *priv, unsigned int slot)
{
	got_add(A_ANSWER)->slot = slot;
}

static void op_reject(void *priv, unsigned int slot, uint32_t code)
{
	struct act *a = got_add(A_REJECT);

	a->slot = slot;
	a->val = code;
}

static void op_hangup(void *priv, unsigned int slot)
{
	got_add(A_HANGUP)->slot = slot;
}

static void op_hold(void *priv, unsigned int slot)
{
	got_add(A_HOLD)->slot = slot;
}

static void op_resume(void *priv, unsigned int slot)
{
	got_add(A_RESUME)->slot = slot;
}

static void op_dtmf(void *priv, char digit, uint16_t duration_ms)
{
	struct act *a = got_add(A_DTMF);

	a->val = (uint32_t)(unsigned char)digit;
	a->slot = duration_ms;
}

static void op_tone(void *priv, enum hal_tone tone)
{
	got_add(A_TONE)->val = (uint32_t)tone;
}

static enum fsm_state bell_seen_state;
static unsigned int bell_seen_other;
static bool bell_seen_held;

static void op_ring_start(void *priv, unsigned int slot)
{
	got_add(A_BELL_START)->slot = slot;
	bell_seen_state = f.state;
	bell_seen_other = f.other_slot;
	bell_seen_held = f.other_held;
}

static void op_ring_stop(void *priv)
{
	got_add(A_BELL_STOP);
}

static void op_cw_alert(void *priv, unsigned int slot, bool on)
{
	struct act *a = got_add(A_CW);

	a->slot = slot;
	a->val = on ? 1 : 0;
}

static void op_cw_net_alert(void *priv, bool on)
{
	got_add(A_CW_NET)->val = on ? 1 : 0;
}

static void op_flash_relay(void *priv)
{
	got_add(A_FLASH_RELAY);
}

/* ------------------------------------------------- the star code stub */

static struct feature_state feat_state;
static struct cfg_channel feat_codes;
static enum feature_verdict feat_scripted;
static char feat_seen[FSM_DIGITS_MAX + 2];
static unsigned int feat_calls;
static bool feat_ok;
static bool feat_arg_wanted;
static bool feat_dnd;
static bool feat_cf_on[FEATURE_CF_COUNT];
static char feat_cf_to[FEATURE_CF_COUNT][FEATURE_TARGET_MAX];
static unsigned int feat_cfnr_s;

enum feature_verdict feature_match(const struct feature_state *st,
				   const struct cfg_channel *ch, const char *buf)
{
	feat_calls++;
	snprintf(feat_seen, sizeof(feat_seen), "%s", buf ? buf : "");

	return feat_scripted;
}

bool feature_takes_arg(const struct cfg_channel *ch, const char *buf)
{
	return feat_arg_wanted;
}

bool feature_dnd_is(const struct feature_state *st)
{
	return feat_dnd;
}

bool feature_cf_armed(const struct feature_state *st, enum feature_cf kind,
		      const char **target)
{
	if (target)
		*target = feat_cf_to[kind];

	return feat_cf_on[kind];
}

unsigned int feature_cfnr_timeout_s(const struct feature_state *st,
				    const struct cfg_channel *ch)
{
	return feat_cfnr_s;
}

/*
 * The three transfer rows, scripted as a tiny table because the two lookups
 * the HOLD_DIAL branch needs answer per string: one names the action of an
 * exact code, the other says whether a buffer can still become one.
 * features.c carries the real table and its own suite; the matrix only cares
 * about the two answers.
 */
static struct {
	char code[FSM_DIGITS_MAX + 1];
	enum feature_action act;
} xfer_row[3];
static unsigned int xfer_rows;

enum feature_action feature_action_get(const struct cfg_channel *ch, const char *buf)
{
	unsigned int i;

	for (i = 0; i < xfer_rows; i++) {
		if (strcmp(xfer_row[i].code, buf) == 0)
			return xfer_row[i].act;
	}

	return FEATURE_ACT_NONE;
}

bool feature_xfer_prefix(const struct cfg_channel *ch, const char *buf)
{
	size_t n = strlen(buf);
	unsigned int i;

	if (!n)
		return false;

	for (i = 0; i < xfer_rows; i++) {
		if (strlen(xfer_row[i].code) > n && strncmp(xfer_row[i].code, buf, n) == 0)
			return true;
	}

	return false;
}

/* the shipped defaults of the three rows */
static void xfer_codes_arm(void)
{
	static const struct {
		const char *code;
		enum feature_action act;
	} rows[] = {
		{ "*072", FEATURE_ACT_XFER_BLIND },
		{ "*073", FEATURE_ACT_XFER_ATTENDED },
		{ "*074", FEATURE_ACT_XFER_CONSULT },
	};
	unsigned int i;

	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		snprintf(xfer_row[i].code, sizeof(xfer_row[i].code), "%s", rows[i].code);
		xfer_row[i].act = rows[i].act;
	}

	xfer_rows = i;
}

/* the condition armed, with the target the enforcement is to use */
static void feat_cf_arm(enum feature_cf kind, const char *target)
{
	feat_cf_on[kind] = true;
	snprintf(feat_cf_to[kind], FEATURE_TARGET_MAX, "%s", target);
}

static bool op_feature_invoke(void *priv, const char *code)
{
	struct act *a = got_add(A_FEATURE);

	snprintf(a->num, sizeof(a->num), "%s", code ? code : "");

	return feat_ok;
}

static bool op_feature_invoke_arg(void *priv, const char *code, const char *arg)
{
	struct act *a = got_add(A_FEATURE_ARG);

	snprintf(a->num, sizeof(a->num), "%s", code ? code : "");
	snprintf(a->arg, sizeof(a->arg), "%s", arg ? arg : "");

	return feat_ok;
}

static void op_call_forward(void *priv, unsigned int slot, const char *target)
{
	struct act *a = got_add(A_FORWARD);

	a->slot = slot;
	snprintf(a->num, sizeof(a->num), "%s", target ? target : "");
}

static void op_call_xfer(void *priv, unsigned int slot, enum feature_action act,
			 const char *target)
{
	struct act *a = got_add(A_XFER);

	a->slot = slot;
	a->val = (uint32_t)act;
	snprintf(a->num, sizeof(a->num), "%s", target ? target : "");
}

static void op_call_xfer_consult(void *priv, unsigned int referee, unsigned int replaced)
{
	struct act *a = got_add(A_XFER_CONSULT);

	a->slot = referee;
	a->val = replaced;
}

static void op_conf_set(void *priv, bool on)
{
	got_add(A_CONF)->val = on ? 1 : 0;
}

static struct {
	unsigned int count;
	enum fsm_state before;
	enum fsm_state after;
	unsigned int actions;
	bool conf_on;
	bool xfer_armed;
} state_note;

static void op_state_changed(void *priv, enum fsm_state before,
			     enum fsm_state after)
{
	CHECK(priv == &state_note);
	state_note.count++;
	state_note.before = before;
	state_note.after = after;
	state_note.actions = ngot;
	state_note.conf_on = f.conf_on;
	state_note.xfer_armed = f.xfer_armed;
}

static const struct fsm_ops ops = {
	.call_dial = op_dial,
	.call_ring = op_ring,
	.call_answer = op_answer,
	.call_reject = op_reject,
	.call_hangup = op_hangup,
	.call_hold = op_hold,
	.call_resume = op_resume,
	.call_dtmf = op_dtmf,
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
 * The same shell with one or both transfer ops missing, which is every half
 * wired shell. The transfer hook gate reads both ops and the loaded table, so
 * each prerequisite gets a case of its own and each asserts the cell keeps
 * its plain digit path.
 */
static struct fsm_ops ops_part;

static void ops_xfer_set(bool xfer, bool consult)
{
	ops_part = ops;
	if (!xfer)
		ops_part.call_xfer = NULL;
	if (!consult)
		ops_part.call_xfer_consult = NULL;

	f.ops = &ops_part;
}

/* the same shell with no mixer, which is every shell that offers no local
 * conference: the eligibility guard of TWO_CALLS reads the op itself */
static void ops_conf_clear(void)
{
	ops_part = ops;
	ops_part.conf_set = NULL;
	f.ops = &ops_part;
}

/* ------------------------------------------------------------- fake timers */

#include "test_clock.h"

/* A timer request is an action of the FSM, so the log records it next to the
 * ops it calls. */
static void act_timer_set(const struct sip_timer *t, unsigned int ms)
{
	struct act *a = got_add(A_ARM);

	a->slot = timer_idx_of(t);
	a->val = ms;
}

static void act_timer_cancel(const struct sip_timer *t)
{
	got_add(A_CANCEL)->slot = timer_idx_of(t);
}

/* -------------------------------------------------------- the dial plan stub */

static struct dialplan dp_stub;
static struct dp_match dp_scripted;
/* the verdict for a buffer that ends in a timer marker; fsm_reset() makes
 * it a complete match, which is what a trailing T gives */
static struct dp_match dp_after_timer;
/* the verdict for a buffer that ends in two timer markers, when a case sets
 * one: the map "TT" gives "T" and "TT" two different verdicts */
static struct dp_match dp_after_timers2;
static bool dp_after_timers2_on;
static char dp_seen[FSM_DIGITS_MAX + 1];
static unsigned int dp_seen_len;
static unsigned int dp_calls;

void dialplan_match(const struct dialplan *dp, const char *digits, unsigned int len,
		    struct dp_match *out)
{
	dp_calls++;
	dp_seen_len = len;
	snprintf(dp_seen, sizeof(dp_seen), "%.*s", (int)len, digits ? digits : "");
	if (dp_after_timers2_on && len >= 2 && digits[len - 2] == 'T' &&
	    digits[len - 1] == 'T') {
		*out = dp_after_timers2;
		return;
	}
	*out = len && digits[len - 1] == 'T' ? dp_after_timer : dp_scripted;
}

/* the four line mapping documented in dialplan.h */
enum dp_action dp_action_of(const struct dp_match *m)
{
	if (m->complete_now)
		return DP_ACT_DIAL;
	if (m->complete_on_timer)
		return DP_ACT_WAIT_SHORT;
	if (m->can_extend || m->timer_next)
		return DP_ACT_WAIT_LONG;

	return DP_ACT_REORDER;
}

size_t dialplan_digits_strip(const char *in, char *out, size_t olen)
{
	size_t n = 0;

	for (; *in; in++)
		if (*in != 'T' && n + 1 < olen)
			out[n++] = *in;
	out[n] = 0;

	return n;
}

static void dp_set(bool now, bool on_timer, bool extend, bool timer_next)
{
	dp_scripted.complete_now = now;
	dp_scripted.complete_on_timer = on_timer;
	dp_scripted.can_extend = extend;
	dp_scripted.timer_next = timer_next;
}

static void dp_after_timer_set(bool now, bool on_timer, bool extend)
{
	memset(&dp_after_timer, 0, sizeof(dp_after_timer));
	dp_after_timer.complete_now = now;
	dp_after_timer.complete_on_timer = on_timer;
	dp_after_timer.can_extend = extend;
}

static void dp_after_timers2_set(bool now, bool on_timer, bool extend)
{
	memset(&dp_after_timers2, 0, sizeof(dp_after_timers2));
	dp_after_timers2.complete_now = now;
	dp_after_timers2.complete_on_timer = on_timer;
	dp_after_timers2.can_extend = extend;
	dp_after_timers2_on = true;
}

static void dp_dial(void)
{
	dp_set(true, false, false, false);
}

/* a complete match now dials, so the short timer is the timer verdict alone */
static void dp_short(void)
{
	dp_set(false, true, true, false);
}

static void dp_long(void)
{
	dp_set(false, false, true, false);
}

/* a mid-map T ("9T11" after "9"): the long timer, and its expiry is a symbol */
static void dp_timer_next(void)
{
	dp_set(false, false, false, true);
}

static void dp_reorder(void)
{
	dp_set(false, false, false, false);
}

/* ------------------------------------------------------------ the coverage */

static bool cover[FSM_STATES][FSM_EV_COUNT];

static void cover_mark(enum fsm_state st, enum fsm_ev ev)
{
	if ((unsigned int)st >= FSM_STATES || (unsigned int)ev >= FSM_EV_COUNT)
		return;

	cover[st][ev] = true;
}

/* ---------------------------------------------------------------- fixtures */

static struct fsm_cfg cfg;

static void cfg_default(void)
{
	memset(&cfg, 0, sizeof(cfg));
	cfg.dp = &dp_stub;
	cfg.start_digit_ms = 16000;
	cfg.interdigit_short_ms = 4000;
	cfg.interdigit_long_ms = 16000;
	cfg.busy_tone_ms = 40000;
	cfg.roh_ms = 60000;
	cfg.warmline_ms = 8000;
	cfg.call_waiting = true;
	cfg.flash_features = true;
	cfg.warmline = false;
	cfg.warmline_number[0] = 0;
}

static void fsm_reset(void)
{
	memset(&fk, 0, sizeof(fk));
	fk.now = 100000;
	fake_clock_bind(&fk.env);
	fake_timer_hook.set = act_timer_set;
	fake_timer_hook.cancel = act_timer_cancel;

	dp_long();
	dp_after_timer_set(true, false, false);
	dp_after_timers2_on = false;
	feat_scripted = FEATURE_NONE;
	feat_calls = 0;
	feat_ok = true;
	feat_seen[0] = 0;
	feat_arg_wanted = false;
	feat_dnd = false;
	memset(feat_cf_on, 0, sizeof(feat_cf_on));
	memset(feat_cf_to, 0, sizeof(feat_cf_to));
	memset(xfer_row, 0, sizeof(xfer_row));
	xfer_rows = 0;
	feat_cfnr_s = 15;
	memset(&state_note, 0, sizeof(state_note));
	fsm_init(&f, 0, &fk.env, &cfg, &ops, &state_note);
	acts_reset();
}

/*
 * The table is off in the default view, so every cell keeps the plain digit
 * path; a case that wants the star code hooks turns it on.
 */
static void feat_on(enum feature_verdict v)
{
	f.cfg.features = &feat_state;
	f.cfg.feature_codes = &feat_codes;
	feat_scripted = v;
}

static void digits_set(const char *s)
{
	snprintf(f.digits, sizeof(f.digits), "%s", s);
	f.ndigits = (unsigned int)strlen(f.digits);
}

static void roles_set(unsigned int active, unsigned int other, bool held)
{
	f.active_slot = active;
	f.other_slot = other;
	f.other_held = held;
}

static void pend_set(enum fsm_pending_op op, unsigned int slot_op, unsigned int slot_other,
		     enum fsm_state on_success, enum fsm_state on_fail)
{
	memset(&f.pending, 0, sizeof(f.pending));
	f.pending.op = op;
	f.pending.slot_op = slot_op;
	f.pending.slot_other = slot_other;
	f.pending.on_success = on_success;
	f.pending.on_fail = on_fail;
	f.state = FSM_PENDING;
	/* the three operations whose other leg is not a held one: the waiting
	 * call of an answer alerts, and the two conference operations run
	 * while both legs face the phone */
	roles_set(slot_op, slot_other,
		  slot_other != FSM_SLOT_NONE && op != FSM_OP_CW_ANSWER &&
		  op != FSM_OP_CONF && op != FSM_OP_CONF_SPLIT);
}

static void chain_set(enum fsm_pending_op op, unsigned int slot, enum fsm_state on_success,
		      enum fsm_state on_fail, bool on_ok_side)
{
	f.pending.chain.op = op;
	f.pending.chain.slot = slot;
	f.pending.chain.on_success = on_success;
	f.pending.chain.on_fail = on_fail;
	f.pending.chain_on_success = on_ok_side;
	f.pending.chain_on_fail = !on_ok_side;
}

static void pend_check_at(enum fsm_pending_op op, unsigned int slot_op,
			  unsigned int slot_other, enum fsm_state on_success,
			  enum fsm_state on_fail, const char *file, int line)
{
	tu_check_uint(f.pending.op, op, "pending.op", file, line);
	tu_check_uint(f.pending.slot_op, slot_op, "pending.slot_op", file, line);
	tu_check_uint(f.pending.slot_other, slot_other, "pending.slot_other", file, line);
	tu_check_uint(f.pending.on_success, on_success, "pending.on_success", file, line);
	tu_check_uint(f.pending.on_fail, on_fail, "pending.on_fail", file, line);
}

static void chain_check_at(bool on_ok, bool on_fail, enum fsm_pending_op op,
			   unsigned int slot, enum fsm_state ok_state,
			   enum fsm_state fail_state, const char *file, int line)
{
	tu_check(f.pending.chain_on_success == on_ok, "chain_on_success", file, line);
	tu_check(f.pending.chain_on_fail == on_fail, "chain_on_fail", file, line);
	if (!on_ok && !on_fail)
		return;

	tu_check_uint(f.pending.chain.op, op, "chain.op", file, line);
	tu_check_uint(f.pending.chain.slot, slot, "chain.slot", file, line);
	tu_check_uint(f.pending.chain.on_success, ok_state, "chain.on_success", file, line);
	tu_check_uint(f.pending.chain.on_fail, fail_state, "chain.on_fail", file, line);
}

static void roles_check_at(unsigned int active, unsigned int other, bool held,
			   const char *file, int line)
{
	tu_check_uint(f.active_slot, active, "active_slot", file, line);
	tu_check_uint(f.other_slot, other, "other_slot", file, line);
	tu_check(f.other_held == held, "other_held", file, line);
}

#define PEND(op, so, sx, ok, fail) \
	pend_check_at(op, so, sx, ok, fail, __FILE__, __LINE__)
#define CHAIN(ok, fail, op, slot, oks, fails) \
	chain_check_at(ok, fail, op, slot, oks, fails, __FILE__, __LINE__)
#define NOCHAIN() \
	chain_check_at(false, false, 0, 0, 0, 0, __FILE__, __LINE__)
#define ROLES(a, o, h)		roles_check_at(a, o, h, __FILE__, __LINE__)
#define DIGITS(s)		tu_check(strcmp(f.digits, s) == 0 && \
					 f.ndigits == strlen(s), "digit buffer", \
					 __FILE__, __LINE__)

/*
 * The representative role, timer and buffer layout of a state, assigned
 * directly: a case sets the machine up, it does not walk it there.
 */
static void st_set(enum fsm_state st)
{
	fsm_reset();
	f.state = st;

	switch (st) {
	case FSM_OFFHOOK_DIAL:
		sip_timer_arm(&fk.env, &f.t_digit, cfg.start_digit_ms);
		break;
	case FSM_COLLECTING:
		digits_set("5");
		sip_timer_arm(&fk.env, &f.t_digit, cfg.interdigit_short_ms);
		break;
	/* the second stage holds the code that opened it, the length that
	 * splits the buffer, and the window between two digits of the target */
	case FSM_SPRE_COLLECT:
		digits_set("*21#");
		f.spre_code_len = 4;
		sip_timer_arm(&fk.env, &f.t_digit, cfg.interdigit_long_ms);
		break;
	case FSM_CALLING:
	case FSM_PROCEEDING:
	case FSM_CONNECTED:
	case FSM_RINGING:
		roles_set(0, FSM_SLOT_NONE, false);
		break;
	case FSM_CW_OFFERED:
		roles_set(0, 1, false);
		sip_timer_arm(&fk.env, &f.t_cw, FSM_CW_TIMEOUT_MS);
		break;
	case FSM_HOLD_DIAL:
		roles_set(FSM_SLOT_NONE, 1, true);
		sip_timer_arm(&fk.env, &f.t_digit, cfg.start_digit_ms);
		break;
	/* one slot, held, the buffer empty and the long window between two
	 * digits of the target; the row that opened the state is remembered */
	case FSM_XFER_COLLECT:
		roles_set(FSM_SLOT_NONE, 1, true);
		f.xfer_act = FEATURE_ACT_XFER_BLIND;
		sip_timer_arm(&fk.env, &f.t_digit, cfg.interdigit_long_ms);
		break;
	case FSM_TWO_CALLS:
		roles_set(0, 1, true);
		break;
	/* both legs face the phone, the mixer runs, and the line took the
	 * held leg of the two-call state second */
	case FSM_CONF:
		roles_set(0, 1, false);
		f.conf_on = true;
		f.slot_second = 1;
		break;
	case FSM_PENDING:
		pend_set(FSM_OP_HOLD, 0, FSM_SLOT_NONE, FSM_HOLD_DIAL, FSM_CONNECTED);
		break;
	case FSM_HOLDING:
		roles_set(FSM_SLOT_NONE, 1, true);
		break;
	case FSM_CLEARING:
		f.clearing = FSM_CLR_BUSY;
		sip_timer_arm(&fk.env, &f.t_clearing, cfg.busy_tone_ms);
		break;
	case FSM_ROH:
		sip_timer_arm(&fk.env, &f.t_clearing, cfg.roh_ms);
		break;
	default:
		break;
	}

	acts_reset();
}

static void ev_push(enum fsm_ev ev, uint32_t arg)
{
	cover_mark(f.state, ev);
	acts_reset();
	fsm_event_push(&f, ev, arg);
}

static void case_start(const char *name, enum fsm_state st)
{
	tu_case(name);
	st_set(st);
}

/* an armed timer that the matrix says fired: drop it before the event */
static void armed_drop(struct sip_timer *t)
{
	sip_timer_cancel(&fk.env, t);
	acts_reset();
}

struct ign {
	enum fsm_ev ev;
	uint32_t arg;
};

/*
 * An ignore cell changes nothing: no action, no state, no role, no timer, no
 * record. The snapshot compare says exactly that.
 */
static void ignores_run(enum fsm_state st, const struct ign *list, unsigned int n)
{
	static struct fsm snap;
	char name[160];
	unsigned int i;

	for (i = 0; i < n; i++) {
		snprintf(name, sizeof(name), "%s x %s: ignore", fsm_state_name(st),
			 fsm_ev_name(list[i].ev));
		case_start(name, st);
		memcpy(&snap, &f, sizeof(snap));
		ev_push(list[i].ev, list[i].arg);
		ACTS();
		STATE(st);
		CHECK(memcmp(&snap, &f, sizeof(snap)) == 0);
	}
}

#define IGNORES(st, list)	ignores_run(st, list, sizeof(list) / sizeof(list[0]))

/*
 * CALL_FAILED carries (slot << 8) | class, CALL_PROCEEDING (slot << 1) |
 * early. The early bit means early media is flowing, and line/line.c raises it
 * on the first packet of the early stream, never on an 18x (RFC 3960 3.2).
 */
static uint32_t fail_arg(unsigned int slot, enum call_fail_class cls)
{
	return (slot << 8) | (uint32_t)cls;
}

static uint32_t prog_arg(unsigned int slot, bool early)
{
	return (slot << 1) | (early ? 1u : 0u);
}

/* ----------------------------------------------------------------- IDLE */

static void test_idle(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_RELEASE, 0 }, { FSM_EV_FLASH, 0 }, { FSM_EV_DIGIT, '5' },
		{ FSM_EV_CALL_PROCEEDING, 0 }, { FSM_EV_CALL_ANSWERED, 0 },
		{ FSM_EV_CALL_FAILED, 0 }, { FSM_EV_CALL_REMOTE_BYE, 0 },
		{ FSM_EV_CALL_REMOTE_CANCEL, 0 }, { FSM_EV_CALL_REINVITE_OK, 0 },
		{ FSM_EV_CALL_REINVITE_FAIL, 0 }, { FSM_EV_CW_NET, 0 },
		{ FSM_EV_CW_NET, 2 }, { FSM_EV_T_START_DIGIT, 0 },
		{ FSM_EV_T_INTERDIGIT, 0 }, { FSM_EV_T_WARMLINE, 0 },
		{ FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 },
	};

	case_start("IDLE x SEIZE: dial tone and the start digit window", FSM_IDLE);
	ev_push(FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);
	DIGITS("");
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("IDLE x SEIZE: the warmline window replaces the start digit one",
		   FSM_IDLE);
	f.cfg.warmline = true;
	ev_push(FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 8000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);

	/* message-waiting: the same cell, the other branch of its entry tone.
	 * The window and every other action are unchanged, so only the tone
	 * is asserted differently. */
	case_start("IDLE x SEIZE: a waiting message gives the stutter dial tone",
		   FSM_IDLE);
	f.cfg.mwi = true;
	ev_push(FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_STUTTER);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);

	/* no account, no registrar target or no live registration: every dial
	 * would be refused, so the seize enters the fast busy chain that refusal
	 * would have entered */
	case_start("IDLE x SEIZE: no dial target gives the fast busy chain",
		   FSM_IDLE);
	fsm_dial_ready_set(&f, false);
	ev_push(FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("IDLE x SEIZE: no dial target outranks a waiting message",
		   FSM_IDLE);
	f.cfg.mwi = true;
	fsm_dial_ready_set(&f, false);
	ev_push(FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);

	case_start("IDLE x SEIZE: no dial target keeps a hotline from dialing",
		   FSM_IDLE);
	f.cfg.warmline = true;
	f.cfg.warmline_ms = 0;
	snprintf(f.cfg.warmline_number, sizeof(f.cfg.warmline_number), "18005551212");
	fsm_dial_ready_set(&f, false);
	ev_push(FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);

	case_start("IDLE x SEIZE: a dial target set again gives the dial tone",
		   FSM_IDLE);
	fsm_dial_ready_set(&f, false);
	fsm_dial_ready_set(&f, true);
	ev_push(FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);

	case_start("IDLE x CALL_INCOMING: no dial target still rings the phone",
		   FSM_IDLE);
	fsm_dial_ready_set(&f, false);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_ring180(1);
	w_bell_start(1);
	ACTS();
	STATE(FSM_RINGING);

	/* the 180 precedes the bell: the caller needs ringback and the early
	 * dialog before the phone starts alerting (RFC 3261 13.3.1.1) */
	case_start("IDLE x CALL_INCOMING: 180 to the caller, then ring the phone",
		   FSM_IDLE);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_ring180(1);
	w_bell_start(1);
	ACTS();
	STATE(FSM_RINGING);
	ROLES(1, FSM_SLOT_NONE, false);

	/*
	 * The on-hook blind transfer of CALLING and PROCEEDING was refused
	 * after the line went idle. The referred leg is still up with nobody on
	 * the phone, so the cell hangs it up; the slot is the argument, because
	 * IDLE owns no role to read it from.
	 */
	case_start("IDLE x CALL_XFER_FAIL: the referred leg of an on-hook transfer is "
		   "hung up", FSM_IDLE);
	ev_push(FSM_EV_CALL_XFER_FAIL, 1);
	w_hangup(1);
	ACTS();
	STATE(FSM_IDLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	/*
	 * The cell reads only the slot argument, so a consult referred leg
	 * whose conference already ended (here slot 0) is hung up exactly as
	 * the blind transfer's leg above is; nothing in the fail event names
	 * which of the two ran.
	 */
	case_start("IDLE x CALL_XFER_FAIL: a consult failure after the "
		   "conference ended hangs the referred leg up, nothing else",
		   FSM_IDLE);
	ev_push(FSM_EV_CALL_XFER_FAIL, 0);
	w_hangup(0);
	ACTS();
	STATE(FSM_IDLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	IGNORES(FSM_IDLE, ignores);
}

/* --------------------------------------------------------- OFFHOOK_DIAL */

static void test_offhook_dial(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 }, { FSM_EV_CALL_PROCEEDING, 0 },
		{ FSM_EV_CALL_ANSWERED, 0 }, { FSM_EV_CALL_FAILED, 0 },
		{ FSM_EV_CALL_REMOTE_BYE, 0 }, { FSM_EV_CALL_REMOTE_CANCEL, 0 },
		{ FSM_EV_CALL_REINVITE_OK, 0 }, { FSM_EV_CALL_REINVITE_FAIL, 0 },
		{ FSM_EV_CW_NET, 0 }, { FSM_EV_CW_NET, 2 }, { FSM_EV_T_INTERDIGIT, 0 },
		{ FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("OFFHOOK_DIAL x RELEASE: back to idle", FSM_OFFHOOK_DIAL);
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	case_start("OFFHOOK_DIAL x FLASH: restart the dial tone", FSM_OFFHOOK_DIAL);
	ev_push(FSM_EV_FLASH, 0);
	w_tone(HAL_TONE_DIAL);
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);
	DIGITS("");

	/* message-waiting: the re-issue follows the entry it re-enters, or a
	 * subscriber who flashed would lose the indication for the rest of
	 * the seizure */
	case_start("OFFHOOK_DIAL x FLASH: the stutter comes back with the window",
		   FSM_OFFHOOK_DIAL);
	f.cfg.mwi = true;
	ev_push(FSM_EV_FLASH, 0);
	w_tone(HAL_TONE_STUTTER);
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);
	DIGITS("");

	case_start("OFFHOOK_DIAL x DIGIT: WAIT_SHORT collects", FSM_OFFHOOK_DIAL);
	dp_short();
	ev_push(FSM_EV_DIGIT, '5');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 4000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("5");

	case_start("OFFHOOK_DIAL x DIGIT: WAIT_LONG collects", FSM_OFFHOOK_DIAL);
	dp_long();
	ev_push(FSM_EV_DIGIT, '5');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("5");

	case_start("OFFHOOK_DIAL x DIGIT: DIAL originates at once", FSM_OFFHOOK_DIAL);
	dp_dial();
	ev_push(FSM_EV_DIGIT, '0');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_dial(0, "0");
	ACTS();
	STATE(FSM_CALLING);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("OFFHOOK_DIAL x DIGIT: NO_MATCH clears fast busy", FSM_OFFHOOK_DIAL);
	dp_reorder();
	ev_push(FSM_EV_DIGIT, '9');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");

	case_start("OFFHOOK_DIAL x DIGIT: '#' on an empty buffer is an ordinary "
		   "symbol", FSM_OFFHOOK_DIAL);
	dp_long();
	ev_push(FSM_EV_DIGIT, '#');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("#");

	/* the delimiter needs a number in front of it; an empty buffer whose
	 * '#' kills the map clears instead */
	case_start("OFFHOOK_DIAL x DIGIT: '#' on an empty buffer with a dead "
		   "map clears", FSM_OFFHOOK_DIAL);
	dp_reorder();
	ev_push(FSM_EV_DIGIT, '#');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");

	/*
	 * The star code branches of the DIGIT cell. The table is consulted
	 * before the dial plan, and only on a buffer whose first character is
	 * '*' or '#'.
	 */
	case_start("OFFHOOK_DIAL x DIGIT: a matched code confirms and restarts "
		   "the dial tone", FSM_OFFHOOK_DIAL);
	feat_on(FEATURE_MATCH);
	ev_push(FSM_EV_DIGIT, '*');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_feature("*");
	w_tone(HAL_TONE_CONFIRM);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);
	DIGITS("");
	CHECK(strcmp(feat_seen, "*") == 0);

	/* the same cell, the target branch: a row that carries a target opens
	 * the second stage instead of running on this digit */
	case_start("OFFHOOK_DIAL x DIGIT: a matched code that takes a target "
		   "opens the second stage", FSM_OFFHOOK_DIAL);
	feat_on(FEATURE_MATCH);
	feat_arg_wanted = true;
	digits_set("*21");
	acts_reset();
	ev_push(FSM_EV_DIGIT, '#');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_SECOND_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_SPRE_COLLECT);
	DIGITS("*21#");
	CHECK_UINT(f.spre_code_len, 4);

	case_start("OFFHOOK_DIAL x DIGIT: a refused code clears fast busy",
		   FSM_OFFHOOK_DIAL);
	feat_on(FEATURE_MATCH);
	feat_ok = false;
	ev_push(FSM_EV_DIGIT, '*');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_feature("*");
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");

	case_start("OFFHOOK_DIAL x DIGIT: a partial code collects on the dial "
		   "plan's own timer", FSM_OFFHOOK_DIAL);
	feat_on(FEATURE_PARTIAL);
	dp_short();
	ev_push(FSM_EV_DIGIT, '*');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 4000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("*");

	case_start("OFFHOOK_DIAL x DIGIT: a partial code with no dial plan "
		   "candidate takes the long timer", FSM_OFFHOOK_DIAL);
	feat_on(FEATURE_PARTIAL);
	dp_long();
	ev_push(FSM_EV_DIGIT, '*');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("*");

	/* the rule the mutation check pins: an unfinished code must not be
	 * cleared by the dial plan verdict of its own prefix */
	case_start("OFFHOOK_DIAL x DIGIT: a partial code survives a dial plan "
		   "NO_MATCH", FSM_OFFHOOK_DIAL);
	feat_on(FEATURE_PARTIAL);
	dp_reorder();
	ev_push(FSM_EV_DIGIT, '*');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("*");

	case_start("OFFHOOK_DIAL x DIGIT: no code matches, so the dial plan "
		   "decides", FSM_OFFHOOK_DIAL);
	feat_on(FEATURE_NONE);
	dp_short();
	ev_push(FSM_EV_DIGIT, '*');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 4000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("*");

	case_start("OFFHOOK_DIAL x DIGIT: a number never reaches the table",
		   FSM_OFFHOOK_DIAL);
	feat_on(FEATURE_MATCH);
	dp_short();
	ev_push(FSM_EV_DIGIT, '5');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 4000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("5");
	CHECK_UINT(feat_calls, 0);

	case_start("OFFHOOK_DIAL x CALL_INCOMING: 486, the line is busy dialing",
		   FSM_OFFHOOK_DIAL);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_reject(1, 486);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);

	case_start("OFFHOOK_DIAL x T_START_DIGIT: fast busy", FSM_OFFHOOK_DIAL);
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_START_DIGIT, 0);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);

	case_start("OFFHOOK_DIAL x T_WARMLINE: dial the warmline number",
		   FSM_OFFHOOK_DIAL);
	f.cfg.warmline = true;
	snprintf(f.cfg.warmline_number, sizeof(f.cfg.warmline_number), "5551234");
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_WARMLINE, 0);
	w_tone(HAL_TONE_NONE);
	w_dial(0, "5551234");
	ACTS();
	STATE(FSM_CALLING);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("OFFHOOK_DIAL x T_WARMLINE: an empty number gives reorder",
		   FSM_OFFHOOK_DIAL);
	f.cfg.warmline = true;
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_WARMLINE, 0);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);

	IGNORES(FSM_OFFHOOK_DIAL, ignores);
}

/* ----------------------------------------------------------- COLLECTING */

static void test_collecting(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 }, { FSM_EV_CALL_PROCEEDING, 0 },
		{ FSM_EV_CALL_ANSWERED, 0 }, { FSM_EV_CALL_FAILED, 0 },
		{ FSM_EV_CALL_REMOTE_BYE, 0 }, { FSM_EV_CALL_REMOTE_CANCEL, 0 },
		{ FSM_EV_CALL_REINVITE_OK, 0 }, { FSM_EV_CALL_REINVITE_FAIL, 0 },
		{ FSM_EV_CW_NET, 0 }, { FSM_EV_CW_NET, 2 }, { FSM_EV_T_START_DIGIT, 0 },
		{ FSM_EV_T_WARMLINE, 0 }, { FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 },
		{ FSM_EV_T_ROH, 0 }, { FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("COLLECTING x RELEASE: back to idle", FSM_COLLECTING);
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);
	DIGITS("");

	case_start("COLLECTING x FLASH: restart the dial tone", FSM_COLLECTING);
	ev_push(FSM_EV_FLASH, 0);
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);
	DIGITS("");

	case_start("COLLECTING x DIGIT: a '#' the map cannot take ends the number",
		   FSM_COLLECTING);
	dp_reorder();
	ev_push(FSM_EV_DIGIT, '#');
	w_cancel(TI_DIGIT);
	w_dial(0, "5");
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CALLING);
	DIGITS("5");

	case_start("COLLECTING x DIGIT: a '#' the map can take is an ordinary "
		   "symbol", FSM_COLLECTING);
	dp_long();
	ev_push(FSM_EV_DIGIT, '#');
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("5#");

	case_start("COLLECTING x DIGIT: DIAL originates", FSM_COLLECTING);
	dp_dial();
	ev_push(FSM_EV_DIGIT, '6');
	w_cancel(TI_DIGIT);
	w_dial(0, "56");
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CALLING);
	DIGITS("56");

	case_start("COLLECTING x DIGIT: WAIT_SHORT re-arms the short timer",
		   FSM_COLLECTING);
	dp_short();
	ev_push(FSM_EV_DIGIT, '6');
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 4000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("56");

	case_start("COLLECTING x DIGIT: WAIT_LONG re-arms the long timer",
		   FSM_COLLECTING);
	dp_long();
	ev_push(FSM_EV_DIGIT, '6');
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("56");

	case_start("COLLECTING x DIGIT: NO_MATCH clears fast busy", FSM_COLLECTING);
	dp_reorder();
	ev_push(FSM_EV_DIGIT, '6');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");

	/*
	 * The star code branches of COLLECTING. The first one is the ordering
	 * rule: a code that carries its own '#' must consume it, so the table
	 * runs before the delimiter that would otherwise dial the buffer
	 * without it.
	 */
	case_start("COLLECTING x DIGIT: a '#' bearing code consumes its '#' "
		   "instead of delimiting", FSM_COLLECTING);
	digits_set("*43");
	feat_on(FEATURE_MATCH);
	ev_push(FSM_EV_DIGIT, '#');
	w_feature("*43#");
	w_tone(HAL_TONE_CONFIRM);
	w_tone(HAL_TONE_DIAL);
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);
	DIGITS("");

	/* the same ordering rule for a code that takes a target: the '#' that
	 * completes the code is consumed here too, and the recall dial tone
	 * asks for the target */
	case_start("COLLECTING x DIGIT: a code that takes a target opens the "
		   "second stage", FSM_COLLECTING);
	digits_set("*21");
	feat_on(FEATURE_MATCH);
	feat_arg_wanted = true;
	ev_push(FSM_EV_DIGIT, '#');
	w_tone(HAL_TONE_SECOND_DIAL);
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_SPRE_COLLECT);
	DIGITS("*21#");
	CHECK_UINT(f.spre_code_len, 4);
	CHECK(strcmp(feat_seen, "*21#") == 0);

	case_start("COLLECTING x DIGIT: a refused code clears fast busy",
		   FSM_COLLECTING);
	digits_set("*43");
	feat_on(FEATURE_MATCH);
	feat_ok = false;
	ev_push(FSM_EV_DIGIT, '#');
	w_feature("*43#");
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");

	case_start("COLLECTING x DIGIT: a partial code survives a dial plan "
		   "NO_MATCH", FSM_COLLECTING);
	digits_set("*4");
	feat_on(FEATURE_PARTIAL);
	dp_reorder();
	ev_push(FSM_EV_DIGIT, '3');
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("*43");

	case_start("COLLECTING x DIGIT: a partial code keeps a dial plan "
		   "candidate on the short timer", FSM_COLLECTING);
	digits_set("*4");
	feat_on(FEATURE_PARTIAL);
	dp_dial();
	ev_push(FSM_EV_DIGIT, '3');
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 4000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("*43");

	/* the abstention path: a star string the table does not carry keeps
	 * the plain digit path, delimiter included */
	case_start("COLLECTING x DIGIT: an unknown star string still ends on a "
		   "'#' the map cannot take", FSM_COLLECTING);
	digits_set("*99");
	feat_on(FEATURE_NONE);
	dp_reorder();
	ev_push(FSM_EV_DIGIT, '#');
	w_cancel(TI_DIGIT);
	w_dial(0, "*99");
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CALLING);
	DIGITS("*99");

	case_start("COLLECTING x DIGIT: a number buffer never reaches the table",
		   FSM_COLLECTING);
	feat_on(FEATURE_MATCH);
	dp_short();
	ev_push(FSM_EV_DIGIT, '6');
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 4000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("56");
	CHECK_UINT(feat_calls, 0);

	case_start("COLLECTING x T_INTERDIGIT: an abandoned star code ends where "
		   "the dial plan says", FSM_COLLECTING);
	digits_set("*4");
	feat_on(FEATURE_PARTIAL);
	dp_long();
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");

	case_start("COLLECTING x CALL_INCOMING: 486", FSM_COLLECTING);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_reject(1, 486);
	ACTS();
	STATE(FSM_COLLECTING);

	case_start("COLLECTING x T_INTERDIGIT: the short timer dials", FSM_COLLECTING);
	dp_short();
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_dial(0, "5");
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CALLING);

	case_start("COLLECTING x T_INTERDIGIT: the long timer clears fast busy",
		   FSM_COLLECTING);
	dp_long();
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");

	/* the collector contract of dialplan.h over the map "9T11" */
	case_start("COLLECTING x T_INTERDIGIT: \"9T11\" takes the long "
		   "timer as a T and dials 911", FSM_COLLECTING);
	digits_set("9");
	dp_timer_next();
	dp_after_timer_set(false, false, true);
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("9T");
	dp_long();
	ev_push(FSM_EV_DIGIT, '1');
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("9T1");
	dp_dial();
	ev_push(FSM_EV_DIGIT, '1');
	w_cancel(TI_DIGIT);
	w_dial(0, "911");
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CALLING);
	DIGITS("911");
	CHECK(strcmp(dp_seen, "9T11") == 0);

	case_start("COLLECTING x T_INTERDIGIT: \"911T|9T11\" after 911 "
		   "dials on the short timer", FSM_COLLECTING);
	digits_set("91");
	dp_set(false, true, false, false);
	ev_push(FSM_EV_DIGIT, '1');
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 4000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("911");
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_dial(0, "911");
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CALLING);
	DIGITS("911");
	CHECK(strcmp(dp_seen, "911T") == 0);

	/* the map "9TT" after "9": the T it takes leaves a timer alone to
	 * complete, so the short window runs next */
	case_start("COLLECTING x T_INTERDIGIT: a T that leaves a timer alone "
		   "to complete arms the short window", FSM_COLLECTING);
	digits_set("9");
	dp_timer_next();
	dp_after_timer_set(false, true, false);
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_arm(TI_DIGIT, 4000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("9T");

	/* one match alone: a second one would mean a T was appended */
	case_start("COLLECTING x T_INTERDIGIT: a long timer that no T expects "
		   "appends none", FSM_COLLECTING);
	digits_set("91");
	dp_long();
	armed_drop(&f.t_digit);
	dp_calls = 0;
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");
	CHECK_UINT(dp_calls, 1);
	CHECK(strcmp(dp_seen, "91") == 0);

	/*
	 * A map reload to "5|5T1" while the timer runs: "5" is complete and
	 * also expects a mid-map T, and the complete match dials at once.
	 */
	case_start("COLLECTING x T_INTERDIGIT: a DIAL verdict dials the buffer "
		   "without a T", FSM_COLLECTING);
	dp_set(true, false, false, true);
	dp_after_timer_set(false, false, true);
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_dial(0, "5");
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CALLING);
	DIGITS("5");
	CHECK(strcmp(dp_seen, "5") == 0);

	/* the buffer cannot take the T, so the expiry keeps its own meaning */
	case_start("COLLECTING x T_INTERDIGIT: a full buffer dials on the "
		   "short timer without a T", FSM_COLLECTING);
	digits_set("12345678901234567890123456789012");
	dp_short();
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_dial(0, "12345678901234567890123456789012");
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CALLING);
	DIGITS("12345678901234567890123456789012");

	/* the map "TT" on the empty buffer: the first T leaves a timer alone
	 * to complete, the second completes it with no digit to dial */
	case_start("COLLECTING x T_INTERDIGIT: \"TT\" dials no empty number",
		   FSM_COLLECTING);
	digits_set("");
	dp_timer_next();
	dp_after_timer_set(false, true, false);
	dp_after_timers2_set(true, false, false);
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_arm(TI_DIGIT, 4000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("T");
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");
	CHECK(strcmp(dp_seen, "TT") == 0);

	IGNORES(FSM_COLLECTING, ignores);
}

/* --------------------------------------------------------- SPRE_COLLECT */

static void test_spre_collect(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 }, { FSM_EV_FLASH, 0 },
		{ FSM_EV_CALL_PROCEEDING, 0 }, { FSM_EV_CALL_ANSWERED, 0 },
		{ FSM_EV_CALL_REMOTE_CANCEL, 0 }, { FSM_EV_CALL_REINVITE_OK, 0 },
		{ FSM_EV_CALL_REINVITE_FAIL, 0 }, { FSM_EV_CW_NET, 0 },
		{ FSM_EV_CW_NET, 2 }, { FSM_EV_T_START_DIGIT, 0 },
		{ FSM_EV_T_WARMLINE, 0 },
		{ FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	/* ------------------------------------------------ the second stage */

	case_start("SPRE_COLLECT x DIGIT: the recall tone stops on the first "
		   "target digit", FSM_SPRE_COLLECT);
	ev_push(FSM_EV_DIGIT, '5');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_SPRE_COLLECT);
	DIGITS("*21#5");

	case_start("SPRE_COLLECT x DIGIT: a target digit extends the buffer and "
		   "re-arms the window", FSM_SPRE_COLLECT);
	ev_push(FSM_EV_DIGIT, '5');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_SPRE_COLLECT);
	DIGITS("*21#5");

	case_start("SPRE_COLLECT x DIGIT: '#' ends the target and invokes the code",
		   FSM_SPRE_COLLECT);
	digits_set("*21#5551234");
	f.spre_code_len = 4;
	acts_reset();
	ev_push(FSM_EV_DIGIT, '#');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_feature_arg("*21#", "5551234");
	w_tone(HAL_TONE_CONFIRM);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);
	DIGITS("");

	case_start("SPRE_COLLECT x DIGIT: an EMPTY target still invokes the code, "
		   "which is the configured one", FSM_SPRE_COLLECT);
	ev_push(FSM_EV_DIGIT, '#');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_feature_arg("*21#", "");
	w_tone(HAL_TONE_CONFIRM);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);

	case_start("SPRE_COLLECT x DIGIT: a refused code is the fast busy",
		   FSM_SPRE_COLLECT);
	feat_ok = false;
	digits_set("*21#5551234");
	f.spre_code_len = 4;
	acts_reset();
	ev_push(FSM_EV_DIGIT, '#');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_feature_arg("*21#", "5551234");
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");

	/*
	 * A target that does not fit refuses the code. Holding the buffer and
	 * collecting on would arm the forward on the number minus its last
	 * digits and confirm that, which is the silent truncation features.c
	 * refuses at its own layer.
	 */
	case_start("SPRE_COLLECT x DIGIT: a target that does not fit is refused, "
		   "not truncated", FSM_SPRE_COLLECT);
	digits_set("*21#1234567890123456789012345678");
	f.spre_code_len = 4;
	CHECK_UINT(f.ndigits, FSM_DIGITS_MAX);
	acts_reset();
	ev_push(FSM_EV_DIGIT, '9');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");

	case_start("SPRE_COLLECT x DIGIT: the '#' behind it confirms nothing",
		   FSM_SPRE_COLLECT);
	digits_set("*21#1234567890123456789012345678");
	f.spre_code_len = 4;
	acts_reset();
	ev_push(FSM_EV_DIGIT, '9');
	acts_reset();
	ev_push(FSM_EV_DIGIT, '#');
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);

	case_start("SPRE_COLLECT x T_INTERDIGIT: an abandoned target is the fast busy",
		   FSM_SPRE_COLLECT);
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");

	case_start("SPRE_COLLECT x RELEASE: every timer and every leg goes",
		   FSM_SPRE_COLLECT);
	roles_set(0, 1, true);
	sip_timer_arm(&fk.env, &f.t_digit, 4000);
	acts_reset();
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_hangup(0);
	w_hangup(1);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("SPRE_COLLECT x CALL_INCOMING: 486, never leave an INVITE open",
		   FSM_SPRE_COLLECT);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_reject(1, 486);
	ACTS();
	STATE(FSM_SPRE_COLLECT);

	case_start("SPRE_COLLECT x CALL_FAILED: fast busy", FSM_SPRE_COLLECT);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_BUSY));
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);

	case_start("SPRE_COLLECT x CALL_REMOTE_BYE: quiet clearing", FSM_SPRE_COLLECT);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 0);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_CLEARING, 2000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_QUIET);

	IGNORES(FSM_SPRE_COLLECT, ignores);
}

/* -------------------------------------------------------------- CALLING */

static void test_calling(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 }, { FSM_EV_DIGIT, '5' },
		{ FSM_EV_CALL_REMOTE_CANCEL, 0 }, { FSM_EV_CALL_REINVITE_OK, 0 },
		{ FSM_EV_CALL_REINVITE_FAIL, 0 }, { FSM_EV_CW_NET, 0 },
		{ FSM_EV_CW_NET, 2 }, { FSM_EV_T_START_DIGIT, 0 },
		{ FSM_EV_T_INTERDIGIT, 0 }, { FSM_EV_T_WARMLINE, 0 },
		{ FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("CALLING x RELEASE: no held leg, back to idle", FSM_CALLING);
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	case_start("CALLING x RELEASE: a held leg keeps the line in holding",
		   FSM_CALLING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	/*
	 * The on-hook transfer of a ringing second call.
	 * The guard is the TWO_CALLS predicate: the option on with the held leg
	 * incoming and this origination outgoing, or an armed consult code. The
	 * ringing leg is cancelled first, the held leg is referred blind to the
	 * number still in the buffer, and the line is idle: no recall ring.
	 */
	case_start("CALLING x RELEASE: the configured gate refers the held leg blind",
		   FSM_CALLING);
	roles_set(0, 1, true);
	f.cfg.xfer_on_hook = true;
	f.slot_inbound[1] = true;
	f.slot_inbound[0] = false;
	digits_set("5552000");
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_xfer(1, FEATURE_ACT_XFER_BLIND, "5552000");
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);
	DIGITS("");

	/* no confirmed dialog exists to name in a Replaces, so the armed consult
	 * degrades to the same blind referral, and the arm is spent */
	case_start("CALLING x RELEASE: an armed consult degrades to the blind referral",
		   FSM_CALLING);
	roles_set(0, 1, true);
	f.xfer_armed = true;
	digits_set("5552000");
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_xfer(1, FEATURE_ACT_XFER_BLIND, "5552000");
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);
	CHECK(!f.xfer_armed);

	case_start("CALLING x RELEASE: the right directions with the option off park",
		   FSM_CALLING);
	roles_set(0, 1, true);
	f.slot_inbound[1] = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);

	case_start("CALLING x RELEASE: a held leg this line dialled parks with the "
		   "option on", FSM_CALLING);
	roles_set(0, 1, true);
	f.cfg.xfer_on_hook = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);

	/* the guard is true on the arm alone, and an origination with no held
	 * leg has nothing to refer: the held leg test comes first */
	case_start("CALLING x RELEASE: an armed transfer with no held leg is a plain "
		   "hangup", FSM_CALLING);
	f.xfer_armed = true;
	f.cfg.xfer_on_hook = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	case_start("CALLING x FLASH: no held leg, restart the dial tone", FSM_CALLING);
	ev_push(FSM_EV_FLASH, 0);
	w_hangup(0);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);

	case_start("CALLING x FLASH: a held leg is reconnected", FSM_CALLING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_FLASH, 0);
	w_hangup(0);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);
	NOCHAIN();

	case_start("CALLING x CALL_INCOMING: 486", FSM_CALLING);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_reject(1, 486);
	ACTS();
	STATE(FSM_CALLING);

	case_start("CALLING x CALL_PROCEEDING: no early media flowing plays "
		   "ringback", FSM_CALLING);
	ev_push(FSM_EV_CALL_PROCEEDING, prog_arg(0, false));
	w_tone(HAL_TONE_RINGBACK);
	ACTS();
	STATE(FSM_PROCEEDING);

	case_start("CALLING x CALL_PROCEEDING: early media already flowing "
		   "stays silent", FSM_CALLING);
	ev_push(FSM_EV_CALL_PROCEEDING, prog_arg(0, true));
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_PROCEEDING);

	case_start("CALLING x CALL_PROCEEDING: a foreign slot is ignored", FSM_CALLING);
	ev_push(FSM_EV_CALL_PROCEEDING, prog_arg(1, false));
	ACTS();
	STATE(FSM_CALLING);

	case_start("CALLING x CALL_ANSWERED: connected", FSM_CALLING);
	ev_push(FSM_EV_CALL_ANSWERED, 0);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("CALLING x CALL_ANSWERED: a held leg gives two calls", FSM_CALLING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_CALL_ANSWERED, 0);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_TWO_CALLS);
	ROLES(0, 1, true);

	case_start("CALLING x CALL_FAILED: busy", FSM_CALLING);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_BUSY));
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_BUSY);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_BUSY);

	case_start("CALLING x CALL_FAILED: congestion", FSM_CALLING);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_CONGESTION));
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("CALLING x CALL_FAILED: unobtainable", FSM_CALLING);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_UNOBTAINABLE));
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_NUMBER_UNOBTAINABLE);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_UNOBTAINABLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("CALLING x CALL_FAILED: quiet", FSM_CALLING);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_QUIET));
	w_tone(HAL_TONE_NONE);
	w_arm(TI_CLEARING, 2000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_QUIET);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("CALLING x CALL_FAILED: the held leg survives the clearing",
		   FSM_CALLING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_BUSY));
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_BUSY);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("CALLING x CALL_FAILED: the held leg died, the origination runs on",
		   FSM_CALLING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_QUIET));
	ACTS();
	STATE(FSM_CALLING);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("CALLING x CALL_FAILED: a slot this state does not own is ignored",
		   FSM_CALLING);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_BUSY));
	ACTS();
	STATE(FSM_CALLING);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("CALLING x CALL_REMOTE_BYE: the held slot is released",
		   FSM_CALLING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	ACTS();
	STATE(FSM_CALLING);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("CALLING x CALL_REMOTE_BYE: the outbound slot is ignored",
		   FSM_CALLING);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 0);
	ACTS();
	STATE(FSM_CALLING);
	ROLES(0, FSM_SLOT_NONE, false);

	IGNORES(FSM_CALLING, ignores);
}

/* ----------------------------------------------------------- PROCEEDING */

static void test_proceeding(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 }, { FSM_EV_DIGIT, '5' },
		{ FSM_EV_CALL_REMOTE_CANCEL, 0 }, { FSM_EV_CALL_REINVITE_OK, 0 },
		{ FSM_EV_CALL_REINVITE_FAIL, 0 }, { FSM_EV_CW_NET, 0 },
		{ FSM_EV_CW_NET, 2 }, { FSM_EV_T_START_DIGIT, 0 },
		{ FSM_EV_T_INTERDIGIT, 0 }, { FSM_EV_T_WARMLINE, 0 },
		{ FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("PROCEEDING x RELEASE: no held leg, back to idle", FSM_PROCEEDING);
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	case_start("PROCEEDING x RELEASE: a held leg keeps the line in holding",
		   FSM_PROCEEDING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);

	/* the same transfer guard as CALLING: the target rings and the
	 * subscriber goes on hook, so the ringback stops with the tone stop */
	case_start("PROCEEDING x RELEASE: the configured gate refers the held leg blind",
		   FSM_PROCEEDING);
	roles_set(0, 1, true);
	f.cfg.xfer_on_hook = true;
	f.slot_inbound[1] = true;
	digits_set("5552000");
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_xfer(1, FEATURE_ACT_XFER_BLIND, "5552000");
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);
	DIGITS("");

	case_start("PROCEEDING x RELEASE: an armed consult degrades to the blind referral",
		   FSM_PROCEEDING);
	roles_set(0, 1, true);
	f.xfer_armed = true;
	digits_set("5552000");
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_xfer(1, FEATURE_ACT_XFER_BLIND, "5552000");
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);
	CHECK(!f.xfer_armed);

	case_start("PROCEEDING x RELEASE: the directions the other way round park",
		   FSM_PROCEEDING);
	roles_set(0, 1, true);
	f.cfg.xfer_on_hook = true;
	f.slot_inbound[0] = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);

	case_start("PROCEEDING x FLASH: no held leg, restart the dial tone",
		   FSM_PROCEEDING);
	ev_push(FSM_EV_FLASH, 0);
	w_hangup(0);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);

	case_start("PROCEEDING x FLASH: a held leg is reconnected", FSM_PROCEEDING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_FLASH, 0);
	w_hangup(0);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);

	case_start("PROCEEDING x CALL_INCOMING: 486", FSM_PROCEEDING);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_reject(1, 486);
	ACTS();
	STATE(FSM_PROCEEDING);

	case_start("PROCEEDING x CALL_PROCEEDING: the first early media packet "
		   "stops the ringback", FSM_PROCEEDING);
	ev_push(FSM_EV_CALL_PROCEEDING, prog_arg(0, true));
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_PROCEEDING);

	case_start("PROCEEDING x CALL_PROCEEDING: a repeated 18x leaves the "
		   "ringback alone", FSM_PROCEEDING);
	ev_push(FSM_EV_CALL_PROCEEDING, prog_arg(0, false));
	ACTS();
	STATE(FSM_PROCEEDING);

	case_start("PROCEEDING x CALL_ANSWERED: connected", FSM_PROCEEDING);
	ev_push(FSM_EV_CALL_ANSWERED, 0);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);

	case_start("PROCEEDING x CALL_ANSWERED: a held leg gives two calls",
		   FSM_PROCEEDING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_CALL_ANSWERED, 0);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_TWO_CALLS);
	ROLES(0, 1, true);

	case_start("PROCEEDING x CALL_FAILED: busy", FSM_PROCEEDING);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_BUSY));
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_BUSY);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_BUSY);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("PROCEEDING x CALL_FAILED: congestion", FSM_PROCEEDING);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_CONGESTION));
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("PROCEEDING x CALL_FAILED: unobtainable", FSM_PROCEEDING);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_UNOBTAINABLE));
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_NUMBER_UNOBTAINABLE);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_UNOBTAINABLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("PROCEEDING x CALL_FAILED: quiet", FSM_PROCEEDING);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_QUIET));
	w_tone(HAL_TONE_NONE);
	w_arm(TI_CLEARING, 2000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_QUIET);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("PROCEEDING x CALL_FAILED: the held leg died, the call runs on",
		   FSM_PROCEEDING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_QUIET));
	ACTS();
	STATE(FSM_PROCEEDING);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("PROCEEDING x CALL_FAILED: a slot this state does not own is ignored",
		   FSM_PROCEEDING);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_BUSY));
	ACTS();
	STATE(FSM_PROCEEDING);

	case_start("PROCEEDING x CALL_FAILED: the held leg survives an outbound failure",
		   FSM_PROCEEDING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_BUSY));
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_BUSY);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("PROCEEDING x CALL_REMOTE_BYE: the held slot is released",
		   FSM_PROCEEDING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	ACTS();
	STATE(FSM_PROCEEDING);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("PROCEEDING x CALL_REMOTE_BYE: the outbound slot is ignored",
		   FSM_PROCEEDING);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 0);
	ACTS();
	STATE(FSM_PROCEEDING);

	IGNORES(FSM_PROCEEDING, ignores);
}

/* ------------------------------------------------------------ CONNECTED */

static void test_connected(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 }, { FSM_EV_CALL_PROCEEDING, 0 },
		{ FSM_EV_CALL_ANSWERED, 0 }, { FSM_EV_CALL_REMOTE_CANCEL, 0 },
		{ FSM_EV_CALL_REINVITE_OK, 0 }, { FSM_EV_CALL_REINVITE_FAIL, 0 },
		{ FSM_EV_T_START_DIGIT, 0 }, { FSM_EV_T_INTERDIGIT, 0 },
		{ FSM_EV_T_WARMLINE, 0 }, { FSM_EV_T_CW, 0 },
		{ FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("CONNECTED x RELEASE: hang up and go idle", FSM_CONNECTED);
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	case_start("CONNECTED x FLASH: hold the active leg", FSM_CONNECTED);
	ev_push(FSM_EV_FLASH, 0);
	w_hold(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_HOLD, 0, FSM_SLOT_NONE, FSM_HOLD_DIAL, FSM_CONNECTED);
	NOCHAIN();

	case_start("CONNECTED x FLASH: flash services off swallows the flash",
		   FSM_CONNECTED);
	f.cfg.flash_features = false;
	ev_push(FSM_EV_FLASH, 0);
	ACTS();
	STATE(FSM_CONNECTED);

	/*
	 * The relay branch of the same cell. It is the first guard
	 * and it is exclusive: the view carries the local hold path too (the
	 * default view has flash_features on and a free slot), and the flash
	 * still leaves for the far end. No leg is held, the roles do not move
	 * and the state does not change.
	 */
	case_start("CONNECTED x FLASH: the relay wins over the local hold path",
		   FSM_CONNECTED);
	f.cfg.flash_relay = true;
	ev_push(FSM_EV_FLASH, 0);
	w_flash_relay();
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);

	/* the region's flash service gates the local branch alone, so the relay
	 * fires whatever it says */
	case_start("CONNECTED x FLASH: the relay ignores the local flash service gate",
		   FSM_CONNECTED);
	f.cfg.flash_relay = true;
	f.cfg.flash_features = false;
	ev_push(FSM_EV_FLASH, 0);
	w_flash_relay();
	ACTS();
	STATE(FSM_CONNECTED);

	/* the state does not change, so the network alert exit rule never
	 * runs here and the alert the switch raised keeps playing */
	case_start("CONNECTED x FLASH: a relayed flash leaves a network alert alone",
		   FSM_CONNECTED);
	f.cfg.flash_relay = true;
	ev_push(FSM_EV_CW_NET, 2);
	ev_push(FSM_EV_FLASH, 0);
	w_flash_relay();
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(f.cw_net);
	CHECK(f.t_cw.armed);

	case_start("CONNECTED x DIGIT: the digit goes to the far end", FSM_CONNECTED);
	ev_push(FSM_EV_DIGIT, '7');
	w_dtmf('7');
	ACTS();
	STATE(FSM_CONNECTED);

	case_start("CONNECTED x CALL_INCOMING: the call waiting offer", FSM_CONNECTED);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_ring180(1);
	w_cw(1, true);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_CW_OFFERED);
	ROLES(0, 1, false);

	case_start("CONNECTED x CALL_INCOMING: a short region alert keeps the "
		   "offer floor", FSM_CONNECTED);
	f.cfg.cw_offer_ms = 20600;
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_ring180(1);
	w_cw(1, true);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_CW_OFFERED);

	case_start("CONNECTED x CALL_INCOMING: a long region alert stretches "
		   "the offer past it", FSM_CONNECTED);
	f.cfg.cw_offer_ms = 70000;
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_ring180(1);
	w_cw(1, true);
	w_arm(TI_CW, 72000);
	ACTS();
	STATE(FSM_CW_OFFERED);

	case_start("CONNECTED x CALL_INCOMING: call waiting off answers 486",
		   FSM_CONNECTED);
	f.cfg.call_waiting = false;
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_reject(1, 486);
	ACTS();
	STATE(FSM_CONNECTED);

	/*
	 * The alert source branch of the same cell (section 5 item 13). The
	 * network mode gives the waiting call to the switch, so this line
	 * answers a second leg the way a line with the service off answers it.
	 */
	case_start("CONNECTED x CALL_INCOMING: the network mode answers 486 too",
		   FSM_CONNECTED);
	f.cfg.cw_mode = FSM_CW_NETWORK;
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_reject(1, 486);
	ACTS();
	STATE(FSM_CONNECTED);

	/* the service stays enabled: it is only served elsewhere, and the query
	 * code answers from this field */
	case_start("CONNECTED x CALL_INCOMING: the network mode leaves the enable alone",
		   FSM_CONNECTED);
	f.cfg.cw_mode = FSM_CW_NETWORK;
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_reject(1, 486);
	ACTS();
	CHECK(f.cfg.call_waiting);

	case_start("CONNECTED x CALL_INCOMING: the local mode still offers",
		   FSM_CONNECTED);
	f.cfg.cw_mode = FSM_CW_LOCAL;
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_ring180(1);
	w_cw(1, true);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_CW_OFFERED);
	ROLES(0, 1, false);

	case_start("CONNECTED x CALL_FAILED: busy", FSM_CONNECTED);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_BUSY));
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_BUSY);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_BUSY);

	case_start("CONNECTED x CALL_FAILED: the quiet class of a dead far end",
		   FSM_CONNECTED);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_QUIET));
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_CLEARING, 2000);
	ACTS();
	CLASS(FSM_CLR_QUIET);

	case_start("CONNECTED x CALL_FAILED: a foreign slot is ignored", FSM_CONNECTED);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_BUSY));
	ACTS();
	STATE(FSM_CONNECTED);

	case_start("CONNECTED x CALL_REMOTE_BYE: quiet clearing", FSM_CONNECTED);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 0);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_CLEARING, 2000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_QUIET);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	IGNORES(FSM_CONNECTED, ignores);
}

/* -------------------------------------------------------------- RINGING */

static void test_ringing(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_RELEASE, 0 }, { FSM_EV_FLASH, 0 }, { FSM_EV_DIGIT, '5' },
		{ FSM_EV_CALL_PROCEEDING, 0 }, { FSM_EV_CALL_ANSWERED, 0 },
		{ FSM_EV_CALL_REINVITE_OK, 0 }, { FSM_EV_CALL_REINVITE_FAIL, 0 },
		{ FSM_EV_CW_NET, 0 }, { FSM_EV_CW_NET, 2 }, { FSM_EV_T_START_DIGIT, 0 },
		{ FSM_EV_T_INTERDIGIT, 0 }, { FSM_EV_T_WARMLINE, 0 },
		{ FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("RINGING x SEIZE: answer the call", FSM_RINGING);
	ev_push(FSM_EV_SEIZE, 0);
	w_bell_stop();
	w_answer(0);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);

	/* the dial target gates the IDLE seize and the CLEARING flash, never an
	 * answer */
	case_start("RINGING x SEIZE: no dial target still answers the call",
		   FSM_RINGING);
	fsm_dial_ready_set(&f, false);
	ev_push(FSM_EV_SEIZE, 0);
	w_bell_stop();
	w_answer(0);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("RINGING x SEIZE: a held leg gives two calls", FSM_RINGING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_SEIZE, 0);
	w_bell_stop();
	w_answer(0);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_TWO_CALLS);
	ROLES(0, 1, true);

	case_start("RINGING x CALL_INCOMING: 486 for the second offer", FSM_RINGING);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_reject(1, 486);
	ACTS();
	STATE(FSM_RINGING);

	case_start("RINGING x CALL_FAILED: the ringing leg died", FSM_RINGING);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_CONGESTION));
	w_bell_stop();
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_IDLE);

	case_start("RINGING x CALL_FAILED: the ringing leg died with a held leg",
		   FSM_RINGING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_CONGESTION));
	w_bell_stop();
	w_tone(HAL_TONE_NONE);
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("RINGING x CALL_FAILED: the held leg died, keep ringing",
		   FSM_RINGING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_QUIET));
	ACTS();
	STATE(FSM_RINGING);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("RINGING x CALL_REMOTE_BYE: the ringing leg cleared", FSM_RINGING);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 0);
	w_bell_stop();
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_IDLE);

	case_start("RINGING x CALL_REMOTE_BYE: the held leg cleared, keep ringing",
		   FSM_RINGING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	ACTS();
	STATE(FSM_RINGING);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("RINGING x CALL_REMOTE_CANCEL: the caller gave up", FSM_RINGING);
	ev_push(FSM_EV_CALL_REMOTE_CANCEL, 0);
	w_bell_stop();
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_IDLE);

	case_start("RINGING x CALL_REMOTE_CANCEL: a held leg keeps the line",
		   FSM_RINGING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_CALL_REMOTE_CANCEL, 0);
	w_bell_stop();
	w_tone(HAL_TONE_NONE);
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);

	case_start("RINGING x CALL_REMOTE_CANCEL: a foreign slot is ignored",
		   FSM_RINGING);
	roles_set(0, 1, true);
	ev_push(FSM_EV_CALL_REMOTE_CANCEL, 1);
	ACTS();
	STATE(FSM_RINGING);
	ROLES(0, 1, true);

	/* ------------------------------------- the ring no answer window */

	case_start("RINGING x T_RING_NOANSWER: the caller goes to the no answer "
		   "target", FSM_RINGING);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_NOANSWER, "5553333");
	ev_push(FSM_EV_T_RING_NOANSWER, 0);
	w_bell_stop();
	w_forward(0, "5553333");
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_IDLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("RINGING x T_RING_NOANSWER: a held leg keeps the line holding",
		   FSM_RINGING);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_NOANSWER, "5553333");
	roles_set(0, 1, true);
	ev_push(FSM_EV_T_RING_NOANSWER, 0);
	w_bell_stop();
	w_forward(0, "5553333");
	w_tone(HAL_TONE_NONE);
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	/* the timer only ever runs while the condition is armed, so this is the
	 * defensive branch of the same cell */
	case_start("RINGING x T_RING_NOANSWER: with no target armed the bell "
		   "keeps ringing", FSM_RINGING);
	feat_on(FEATURE_NONE);
	ev_push(FSM_EV_T_RING_NOANSWER, 0);
	ACTS();
	STATE(FSM_RINGING);
	ROLES(0, FSM_SLOT_NONE, false);

	IGNORES(FSM_RINGING, ignores);
}

/* ------------------------------------------------- the bell that never rang */

/*
 * fsm_ring_failed() is an entry and not an event, as fsm_ring_held() is. It
 * therefore marks no cell, and the cell count of the matrix does not change.
 * line/line.c calls it when the backend refuses the bell for good and the
 * caller must be told.
 */
static void test_ring_failed(void)
{
	case_start("fsm_ring_failed: RINGING refuses the caller 480 and the "
		   "line goes idle", FSM_RINGING);
	f.ring_held = true;
	fsm_ring_failed(&f);
	w_bell_stop();
	w_reject(0, 480);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_IDLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);
	CHECK(!f.ring_held);

	case_start("fsm_ring_failed: a held leg is kept and the line falls "
		   "back to HOLDING", FSM_RINGING);
	roles_set(0, 1, true);
	fsm_ring_failed(&f);
	w_bell_stop();
	w_reject(0, 480);
	w_tone(HAL_TONE_NONE);
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	/* the window is armed by the RINGING entry the offer went through, so
	 * this case walks the machine there instead of arming the timer */
	case_start("fsm_ring_failed: the ring no-answer window is stopped, "
		   "never forwarded", FSM_IDLE);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_NOANSWER, "5553333");
	ev_push(FSM_EV_CALL_INCOMING, 0);
	STATE(FSM_RINGING);
	acts_reset();
	fsm_ring_failed(&f);
	w_bell_stop();
	w_cancel(TI_CW);
	w_reject(0, 480);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_IDLE);
	acts_reset();
	fake_advance((uint64_t)feat_cfnr_s * 1000 + 1000);
	ACTS();
	CHECK_UINT(fk.fired, 0);

	case_start("fsm_ring_failed: a machine that is not ringing emits "
		   "nothing", FSM_CONNECTED);
	f.ring_held = true;
	fsm_ring_failed(&f);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);
	CHECK(!f.ring_held);

	/* the recall ring of HOLDING is the other bell a line can have on */
	st_set(FSM_HOLDING);
	f.ring_held = true;
	fsm_ring_failed(&f);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);
	CHECK(!f.ring_held);

	case_start("fsm_ring_failed: the 480 is not redirected to the busy "
		   "forward target", FSM_RINGING);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_BUSY, "5554444");
	fsm_ring_failed(&f);
	w_bell_stop();
	w_reject(0, 480);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_IDLE);
}

/* ----------------------------------------------------------- CW_OFFERED */

static void test_cw_offered(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 }, { FSM_EV_CALL_PROCEEDING, 0 },
		{ FSM_EV_CALL_ANSWERED, 0 }, { FSM_EV_CALL_REINVITE_OK, 0 },
		{ FSM_EV_CALL_REINVITE_FAIL, 0 }, { FSM_EV_CW_NET, 0 },
		{ FSM_EV_CW_NET, 2 }, { FSM_EV_T_START_DIGIT, 0 },
		{ FSM_EV_T_INTERDIGIT, 0 }, { FSM_EV_T_WARMLINE, 0 },
		{ FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("CW_OFFERED x RELEASE: the waiting call rings the phone",
		   FSM_CW_OFFERED);
	ev_push(FSM_EV_RELEASE, 0);
	w_cancel(TI_CW);
	w_cw(1, false);
	w_hangup(0);
	w_bell_start(1);
	ACTS();
	STATE(FSM_RINGING);
	ROLES(1, FSM_SLOT_NONE, false);

	/* the active-gone shape plays the reorder of the active clear below, and
	 * the on-hook stops it before the bell presents the waiting call */
	case_start("CW_OFFERED x RELEASE: the active leg was already gone",
		   FSM_CW_OFFERED);
	f.active_slot = FSM_SLOT_NONE;
	ev_push(FSM_EV_RELEASE, 0);
	w_cancel(TI_CW);
	w_cw(1, false);
	w_tone(HAL_TONE_NONE);
	w_bell_start(1);
	ACTS();
	STATE(FSM_RINGING);
	ROLES(1, FSM_SLOT_NONE, false);

	case_start("CW_OFFERED x FLASH: hold the active leg, then answer",
		   FSM_CW_OFFERED);
	ev_push(FSM_EV_FLASH, 0);
	w_cancel(TI_CW);
	w_cw(1, false);
	w_hold(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_CW_ANSWER, 0, 1, FSM_TWO_CALLS, FSM_CONNECTED);
	NOCHAIN();

	case_start("CW_OFFERED x FLASH: the active leg is gone, answer directly",
		   FSM_CW_OFFERED);
	f.active_slot = FSM_SLOT_NONE;
	ev_push(FSM_EV_FLASH, 0);
	w_cancel(TI_CW);
	w_cw(1, false);
	w_answer(1);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(1, FSM_SLOT_NONE, false);

	case_start("CW_OFFERED x DIGIT: the active leg still takes digits",
		   FSM_CW_OFFERED);
	ev_push(FSM_EV_DIGIT, '4');
	w_dtmf('4');
	ACTS();
	STATE(FSM_CW_OFFERED);

	case_start("CW_OFFERED x DIGIT: no active leg, no media path", FSM_CW_OFFERED);
	f.active_slot = FSM_SLOT_NONE;
	ev_push(FSM_EV_DIGIT, '4');
	ACTS();
	STATE(FSM_CW_OFFERED);

	case_start("CW_OFFERED x CALL_INCOMING: 486, both slots are taken",
		   FSM_CW_OFFERED);
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_reject(0, 486);
	ACTS();
	STATE(FSM_CW_OFFERED);

	case_start("CW_OFFERED x CALL_FAILED: the waiting leg died", FSM_CW_OFFERED);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_CONGESTION));
	w_cancel(TI_CW);
	w_cw(1, false);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);

	/* with the active leg gone the reorder is playing, and the fast busy
	 * clearing continues it into the howler: its entry re-issues the tone so
	 * the tone duration restarts beside the clearing timer */
	case_start("CW_OFFERED x CALL_FAILED: the waiting leg died with no active leg",
		   FSM_CW_OFFERED);
	f.active_slot = FSM_SLOT_NONE;
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_CONGESTION));
	w_cancel(TI_CW);
	w_cw(1, false);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);

	/*
	 * The active party is gone while a call waits: the leg is
	 * hung up because it failed, the alert stops, the reorder announces the
	 * end of the call, and the waiting call is kept for the on-hook or the
	 * flash. The offer window is re-armed, bounded by the reorder duration.
	 */
	case_start("CW_OFFERED x CALL_FAILED: the active leg died, reorder and keep the "
		   "waiting call", FSM_CW_OFFERED);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_QUIET));
	w_hangup(0);
	w_cw(1, false);
	w_tone(HAL_TONE_REORDER);
	w_cancel(TI_CW);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_CW_OFFERED);
	ROLES(FSM_SLOT_NONE, 1, false);

	case_start("CW_OFFERED x CALL_REMOTE_BYE: the waiting leg cleared",
		   FSM_CW_OFFERED);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	w_cancel(TI_CW);
	w_cw(1, false);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);

	/* a far-end BYE has nothing left to hang up; the rest is the FAILED cell */
	case_start("CW_OFFERED x CALL_REMOTE_BYE: the active leg cleared, reorder and "
		   "keep the waiting call", FSM_CW_OFFERED);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 0);
	w_cw(1, false);
	w_tone(HAL_TONE_REORDER);
	w_cancel(TI_CW);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_CW_OFFERED);
	ROLES(FSM_SLOT_NONE, 1, false);

	/* the reorder is a fixed FSM_REORDER_MS window, longer than the offer
	 * budget, so the budget bounds the re-armed window and no silence opens */
	case_start("CW_OFFERED x CALL_REMOTE_BYE: the fixed reorder outlasts the "
		   "offer window", FSM_CW_OFFERED);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 0);
	w_cw(1, false);
	w_tone(HAL_TONE_REORDER);
	w_cancel(TI_CW);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_CW_OFFERED);

	/*
	 * The whole sequence in one action log, with nothing constructed by
	 * hand: the active party hangs up, the subscriber hears the reorder, goes
	 * on hook, the phone rings for the waiting call, and the seize answers it.
	 */
	case_start("CW_OFFERED CALL_REMOTE_BYE then RELEASE then SEIZE: reorder, ring, "
		   "answer", FSM_CW_OFFERED);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 0);
	fsm_event_push(&f, FSM_EV_RELEASE, 0);
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	w_cw(1, false);
	w_tone(HAL_TONE_REORDER);
	w_cancel(TI_CW);
	w_arm(TI_CW, 30000);
	w_cancel(TI_CW);
	w_cw(1, false);
	w_tone(HAL_TONE_NONE);
	w_bell_start(1);
	w_bell_stop();
	w_answer(1);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(1, FSM_SLOT_NONE, false);

	case_start("CW_OFFERED x CALL_REMOTE_CANCEL: the waiting caller gave up",
		   FSM_CW_OFFERED);
	ev_push(FSM_EV_CALL_REMOTE_CANCEL, 1);
	w_cancel(TI_CW);
	w_cw(1, false);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);

	case_start("CW_OFFERED x CALL_REMOTE_CANCEL: no active leg left",
		   FSM_CW_OFFERED);
	f.active_slot = FSM_SLOT_NONE;
	ev_push(FSM_EV_CALL_REMOTE_CANCEL, 1);
	w_cancel(TI_CW);
	w_cw(1, false);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);

	case_start("CW_OFFERED x CALL_REMOTE_CANCEL: the active slot is ignored",
		   FSM_CW_OFFERED);
	ev_push(FSM_EV_CALL_REMOTE_CANCEL, 0);
	ACTS();
	STATE(FSM_CW_OFFERED);
	ROLES(0, 1, false);

	case_start("CW_OFFERED x T_CW: 480 so upstream forwarding runs",
		   FSM_CW_OFFERED);
	armed_drop(&f.t_cw);
	ev_push(FSM_EV_T_CW, 0);
	w_reject(1, 480);
	w_cw(1, false);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("CW_OFFERED x T_CW: no active leg left, the reorder runs on to the "
		   "howler", FSM_CW_OFFERED);
	f.active_slot = FSM_SLOT_NONE;
	armed_drop(&f.t_cw);
	ev_push(FSM_EV_T_CW, 0);
	w_reject(1, 480);
	w_cw(1, false);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);

	IGNORES(FSM_CW_OFFERED, ignores);
}

/* ------------------------------------------------------------ HOLD_DIAL */

static void test_hold_dial(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 }, { FSM_EV_CALL_PROCEEDING, 0 },
		{ FSM_EV_CALL_ANSWERED, 0 }, { FSM_EV_CALL_REMOTE_CANCEL, 0 },
		{ FSM_EV_CALL_REINVITE_OK, 0 }, { FSM_EV_CALL_REINVITE_FAIL, 0 },
		{ FSM_EV_CW_NET, 0 }, { FSM_EV_CW_NET, 2 },
		{ FSM_EV_T_WARMLINE, 0 }, { FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 },
		{ FSM_EV_T_ROH, 0 }, { FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("HOLD_DIAL x RELEASE: the held leg starts the recall ring",
		   FSM_HOLD_DIAL);
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);
	CHECK(!f.t_digit.armed);

	case_start("HOLD_DIAL x FLASH: reconnect the held leg", FSM_HOLD_DIAL);
	ev_push(FSM_EV_FLASH, 0);
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);

	case_start("HOLD_DIAL x DIGIT: WAIT_SHORT keeps collecting here",
		   FSM_HOLD_DIAL);
	dp_short();
	ev_push(FSM_EV_DIGIT, '5');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 4000);
	ACTS();
	STATE(FSM_HOLD_DIAL);
	DIGITS("5");
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("HOLD_DIAL x DIGIT: WAIT_LONG keeps collecting here", FSM_HOLD_DIAL);
	dp_long();
	ev_push(FSM_EV_DIGIT, '5');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_HOLD_DIAL);

	case_start("HOLD_DIAL x DIGIT: DIAL originates and keeps the held leg",
		   FSM_HOLD_DIAL);
	dp_dial();
	ev_push(FSM_EV_DIGIT, '5');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_dial(0, "5");
	ACTS();
	STATE(FSM_CALLING);
	ROLES(0, 1, true);

	case_start("HOLD_DIAL x DIGIT: a '#' the map cannot take dials the buffer",
		   FSM_HOLD_DIAL);
	digits_set("5");
	sip_timer_arm(&fk.env, &f.t_digit, cfg.interdigit_short_ms);
	acts_reset();
	dp_reorder();
	ev_push(FSM_EV_DIGIT, '#');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_dial(0, "5");
	ACTS();
	STATE(FSM_CALLING);
	ROLES(0, 1, true);

	case_start("HOLD_DIAL x DIGIT: NO_MATCH clears fast busy, held leg kept",
		   FSM_HOLD_DIAL);
	dp_reorder();
	ev_push(FSM_EV_DIGIT, '5');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	ROLES(FSM_SLOT_NONE, 1, true);

	/* the ordinary table is not consulted here: the HOLD_DIAL branch reads
	 * the two transfer lookups instead, and feature_match never runs */
	case_start("HOLD_DIAL x DIGIT: the ordinary star code table is not consulted "
		   "here", FSM_HOLD_DIAL);
	feat_on(FEATURE_MATCH);
	dp_long();
	ev_push(FSM_EV_DIGIT, '*');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_HOLD_DIAL);
	DIGITS("*");
	CHECK_UINT(feat_calls, 0);

	case_start("HOLD_DIAL x CALL_INCOMING: 486", FSM_HOLD_DIAL);
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_reject(0, 486);
	ACTS();
	STATE(FSM_HOLD_DIAL);

	/* the loss of the held leg grants no second first-digit window */
	case_start("HOLD_DIAL x CALL_FAILED: the held leg died, empty buffer",
		   FSM_HOLD_DIAL);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_QUIET));
	w_tone(HAL_TONE_DIAL);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);
	CHECK(f.t_digit.armed);
	CHECK_UINT(f.t_digit.due_ms, fk.now + cfg.start_digit_ms);

	case_start("HOLD_DIAL x CALL_FAILED: the held leg died mid number",
		   FSM_HOLD_DIAL);
	digits_set("5");
	sip_timer_arm(&fk.env, &f.t_digit, cfg.interdigit_short_ms);
	acts_reset();
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_QUIET));
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("5");
	CHECK(f.t_digit.armed);
	CHECK_UINT(f.t_digit.due_ms, fk.now + cfg.interdigit_short_ms);

	case_start("HOLD_DIAL x CALL_REMOTE_BYE: the held leg cleared, empty buffer",
		   FSM_HOLD_DIAL);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	w_tone(HAL_TONE_DIAL);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);
	CHECK(f.t_digit.armed);
	CHECK_UINT(f.t_digit.due_ms, fk.now + cfg.start_digit_ms);

	case_start("HOLD_DIAL x CALL_REMOTE_BYE: the held leg cleared mid number",
		   FSM_HOLD_DIAL);
	digits_set("56");
	sip_timer_arm(&fk.env, &f.t_digit, cfg.interdigit_short_ms);
	acts_reset();
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("56");
	CHECK(f.t_digit.armed);
	CHECK_UINT(f.t_digit.due_ms, fk.now + cfg.interdigit_short_ms);

	case_start("HOLD_DIAL x T_INTERDIGIT: the short timer dials", FSM_HOLD_DIAL);
	digits_set("5");
	dp_short();
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_dial(0, "5");
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CALLING);
	ROLES(0, 1, true);

	case_start("HOLD_DIAL x T_INTERDIGIT: the long timer clears fast busy",
		   FSM_HOLD_DIAL);
	digits_set("5");
	dp_long();
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("HOLD_DIAL x T_INTERDIGIT: a mid-map T takes the long "
		   "timer as a T, held leg kept", FSM_HOLD_DIAL);
	digits_set("9");
	dp_timer_next();
	dp_after_timer_set(false, false, true);
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_HOLD_DIAL);
	DIGITS("9T");
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("HOLD_DIAL x T_START_DIGIT: no digit, reorder with the held leg kept",
		   FSM_HOLD_DIAL);
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_START_DIGIT, 0);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	ROLES(FSM_SLOT_NONE, 1, true);

	IGNORES(FSM_HOLD_DIAL, ignores);
}

/* ------------------------------------------------------------ TWO_CALLS */

static void test_two_calls(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 }, { FSM_EV_CALL_PROCEEDING, 0 },
		{ FSM_EV_CALL_ANSWERED, 0 }, { FSM_EV_CALL_REMOTE_CANCEL, 0 },
		{ FSM_EV_CALL_REINVITE_OK, 0 }, { FSM_EV_CALL_REINVITE_FAIL, 0 },
		{ FSM_EV_T_START_DIGIT, 0 }, { FSM_EV_T_INTERDIGIT, 0 },
		{ FSM_EV_T_WARMLINE, 0 }, { FSM_EV_T_CW, 0 },
		{ FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("TWO_CALLS x RELEASE: the held leg survives on hook",
		   FSM_TWO_CALLS);
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("TWO_CALLS x RELEASE: the bell callback sees HOLDING and the "
		   "held role", FSM_TWO_CALLS);
	bell_seen_state = FSM_IDLE;
	bell_seen_other = FSM_SLOT_NONE;
	bell_seen_held = false;
	ev_push(FSM_EV_RELEASE, 0);
	STATE(FSM_HOLDING);
	CHECK(bell_seen_state == FSM_HOLDING);
	CHECK(bell_seen_other == 1);
	CHECK(bell_seen_held);

	case_start("TWO_CALLS x FLASH: the swap holds the active leg first",
		   FSM_TWO_CALLS);
	ev_push(FSM_EV_FLASH, 0);
	w_hold(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);
	NOCHAIN();

	case_start("TWO_CALLS x DIGIT: the digit goes to the active leg",
		   FSM_TWO_CALLS);
	ev_push(FSM_EV_DIGIT, '2');
	w_dtmf('2');
	ACTS();
	STATE(FSM_TWO_CALLS);

	case_start("TWO_CALLS x CALL_INCOMING: 486, both slots are taken",
		   FSM_TWO_CALLS);
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_reject(0, 486);
	ACTS();
	STATE(FSM_TWO_CALLS);

	case_start("TWO_CALLS x CALL_FAILED: the active leg died, retrieve the held one",
		   FSM_TWO_CALLS);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_QUIET));
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);

	case_start("TWO_CALLS x CALL_FAILED: the held leg died", FSM_TWO_CALLS);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_QUIET));
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("TWO_CALLS x CALL_REMOTE_BYE: the active leg cleared",
		   FSM_TWO_CALLS);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 0);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);

	case_start("TWO_CALLS x CALL_REMOTE_BYE: the held leg cleared", FSM_TWO_CALLS);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);

	IGNORES(FSM_TWO_CALLS, ignores);
}

/* -------------------- CONNECTED and TWO_CALLS CW_NET, the network alert */

/*
 * The BroadWorks alert. It belongs to an answered call, it names no
 * slot, and its caller identity never reaches this layer: the machine sees
 * the command alone (0 stop, 1 to 4 play that GR-506 pattern) and line/line.c
 * renders what the INFO carried.
 */
/*
 * ----------------------------------- do not disturb and call forwarding
 *
 * The guard branches of do not disturb and call forwarding in cells that
 * already exist. No cell is created here: the coverage table counts the same
 * (state, event) pairs and these cases mark them again.
 *
 * Two rules are under test. The first fixes the order, unconditional forward
 * before the do-not-disturb filter, and makes a do-not-disturb refusal reach
 * the busy forward. The second arms the ring window only for a line that
 * carries the service.
 */
static void test_dnd_callfwd(void)
{
	case_start("IDLE x CALL_INCOMING: an unconditional forward redirects "
		   "instead of ringing", FSM_IDLE);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_UNCOND, "5551111");
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_forward(0, "5551111");
	ACTS();
	STATE(FSM_IDLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("IDLE x CALL_INCOMING: a forward with no target rings as usual",
		   FSM_IDLE);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_UNCOND, "");
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_ring180(0);
	w_bell_start(0);
	ACTS();
	STATE(FSM_RINGING);

	case_start("IDLE x CALL_INCOMING: do not disturb answers busy", FSM_IDLE);
	feat_on(FEATURE_NONE);
	feat_dnd = true;
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_reject(0, 486);
	ACTS();
	STATE(FSM_IDLE);

	/* the precedence: the redirect happens at the acceptance point, which
	 * is before the filter that would have refused the call */
	case_start("IDLE x CALL_INCOMING: the unconditional forward beats do not "
		   "disturb", FSM_IDLE);
	feat_on(FEATURE_NONE);
	feat_dnd = true;
	feat_cf_arm(FEATURE_CF_UNCOND, "5551111");
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_forward(0, "5551111");
	ACTS();
	STATE(FSM_IDLE);

	/* the interception: the refusal goes through the shared helper, so the
	 * busy condition catches it */
	case_start("IDLE x CALL_INCOMING: do not disturb with a busy forward "
		   "redirects instead of refusing", FSM_IDLE);
	feat_on(FEATURE_NONE);
	feat_dnd = true;
	feat_cf_arm(FEATURE_CF_BUSY, "5552222");
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_forward(0, "5552222");
	ACTS();
	STATE(FSM_IDLE);

	case_start("IDLE x CALL_INCOMING: a busy forward with no target still "
		   "refuses", FSM_IDLE);
	feat_on(FEATURE_NONE);
	feat_dnd = true;
	feat_cf_arm(FEATURE_CF_BUSY, "");
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_reject(0, 486);
	ACTS();
	STATE(FSM_IDLE);

	/* the busy condition rides the one helper every 486 of the matrix goes
	 * through, so a line busy for any other reason redirects too */
	case_start("OFFHOOK_DIAL x CALL_INCOMING: the busy forward takes the 486 "
		   "of a line that is dialling", FSM_OFFHOOK_DIAL);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_BUSY, "5552222");
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_forward(1, "5552222");
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);

	case_start("RINGING x CALL_INCOMING: and the 486 of a line already alerting",
		   FSM_RINGING);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_BUSY, "5552222");
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_forward(1, "5552222");
	ACTS();
	STATE(FSM_RINGING);

	/* the negative of the same rule: the waiting call the subscriber never
	 * took is not the busy condition, so its 480 stays a 480 */
	case_start("CW_OFFERED x T_CW: the busy forward does NOT take the 480",
		   FSM_CW_OFFERED);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_BUSY, "5552222");
	armed_drop(&f.t_cw);
	ev_push(FSM_EV_T_CW, 0);
	w_reject(1, 480);
	w_cw(1, false);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);

	case_start("CONNECTED x CALL_INCOMING: the unconditional forward takes "
		   "the call before the waiting offer", FSM_CONNECTED);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_UNCOND, "5551111");
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_forward(1, "5551111");
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("CONNECTED x CALL_INCOMING: do not disturb refuses the waiting "
		   "offer", FSM_CONNECTED);
	feat_on(FEATURE_NONE);
	feat_dnd = true;
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_reject(1, 486);
	ACTS();
	STATE(FSM_CONNECTED);

	case_start("CONNECTED x CALL_INCOMING: and redirects it when the busy "
		   "condition is armed", FSM_CONNECTED);
	feat_on(FEATURE_NONE);
	feat_dnd = true;
	feat_cf_arm(FEATURE_CF_BUSY, "5552222");
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_forward(1, "5552222");
	ACTS();
	STATE(FSM_CONNECTED);

	/*
	 * Do not disturb does not touch the network alert, and this case pins
	 * the absence of that gate. The filter belongs on the incoming call
	 * event, and the switch holds this call, so no such event exists to
	 * filter. Gating it here would silence the alert and
	 * keep the busy forward from ever firing, because no offer would reach
	 * this line to be refused.
	 */
	case_start("CONNECTED x CW_NET: do not disturb leaves the network alert "
		   "alone", FSM_CONNECTED);
	feat_on(FEATURE_NONE);
	feat_dnd = true;
	ev_push(FSM_EV_CW_NET, 2);
	w_cw_net(true);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(f.cw_net);

	case_start("CONNECTED x CW_NET: the long region alert stretches the "
		   "network offer too", FSM_CONNECTED);
	feat_on(FEATURE_NONE);
	feat_dnd = true;
	f.cfg.cw_offer_ms = 70000;
	ev_push(FSM_EV_CW_NET, 2);
	w_cw_net(true);
	w_arm(TI_CW, 72000);
	ACTS();
	CHECK(f.cw_net);

	case_start("CONNECTED x CW_NET: and the stop still ends it", FSM_CONNECTED);
	feat_on(FEATURE_NONE);
	feat_dnd = true;
	ev_push(FSM_EV_CW_NET, 2);
	acts_reset();
	ev_push(FSM_EV_CW_NET, 0);
	w_cw_net(false);
	w_cancel(TI_CW);
	ACTS();
	CHECK(!f.cw_net);

	/* the same line with the busy forward armed diverts nothing here: the
	 * waiting call is the switch's, and the alert is all this line owes */
	case_start("CONNECTED x CW_NET: a busy forward does not take a call this "
		   "line was never offered", FSM_CONNECTED);
	feat_on(FEATURE_NONE);
	feat_dnd = true;
	feat_cf_arm(FEATURE_CF_BUSY, "5552222");
	ev_push(FSM_EV_CW_NET, 2);
	w_cw_net(true);
	w_arm(TI_CW, 30000);
	ACTS();
	CHECK(f.cw_net);

	case_start("HOLDING x CALL_INCOMING: the forward leaves the held leg and "
		   "its recall ring alone", FSM_HOLDING);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_UNCOND, "5551111");
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_forward(0, "5551111");
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("HOLDING x CALL_INCOMING: do not disturb refuses it and holds",
		   FSM_HOLDING);
	feat_on(FEATURE_NONE);
	feat_dnd = true;
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_reject(0, 486);
	ACTS();
	STATE(FSM_HOLDING);

	/* ---------------------------------------------- the ring window */

	case_start("IDLE x CALL_INCOMING: the ring window is armed for a line "
		   "that forwards on no answer", FSM_IDLE);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_NOANSWER, "5553333");
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_ring180(0);
	w_bell_start(0);
	w_arm(TI_CW, 15000);
	ACTS();
	STATE(FSM_RINGING);

	case_start("IDLE x CALL_INCOMING: a line without the service rings with "
		   "no window at all", FSM_IDLE);
	feat_on(FEATURE_NONE);
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_ring180(0);
	w_bell_start(0);
	ACTS();
	STATE(FSM_RINGING);
	CHECK(!f.t_cw.armed);

	case_start("IDLE x CALL_INCOMING: armed with no target arms no window",
		   FSM_IDLE);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_NOANSWER, "");
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_ring180(0);
	w_bell_start(0);
	ACTS();
	CHECK(!f.t_cw.armed);

	case_start("IDLE x CALL_INCOMING: a zero window arms nothing either",
		   FSM_IDLE);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_NOANSWER, "5553333");
	feat_cfnr_s = 0;
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_ring180(0);
	w_bell_start(0);
	ACTS();
	CHECK(!f.t_cw.armed);

	case_start("IDLE x CALL_INCOMING: the dialled window is the one armed",
		   FSM_IDLE);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_NOANSWER, "5553333");
	feat_cfnr_s = 45;
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_ring180(0);
	w_bell_start(0);
	w_arm(TI_CW, 45000);
	ACTS();

	case_start("HOLDING x CALL_INCOMING: the window rides that entry too",
		   FSM_HOLDING);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_NOANSWER, "5553333");
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_bell_stop();
	w_ring180(0);
	w_bell_start(0);
	w_arm(TI_CW, 15000);
	ACTS();
	STATE(FSM_RINGING);

	case_start("RINGING x T_RING_NOANSWER: the forward restores the recall ring "
		   "of the held leg", FSM_HOLDING);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_NOANSWER, "5553333");
	ev_push(FSM_EV_CALL_INCOMING, 0);
	STATE(FSM_RINGING);
	armed_drop(&f.t_cw);
	ev_push(FSM_EV_T_RING_NOANSWER, 0);
	w_bell_stop();
	w_forward(0, "5553333");
	w_tone(HAL_TONE_NONE);
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);
	CHECK(!f.t_cw.armed);

	case_start("RINGING x SEIZE: answering cancels the window", FSM_RINGING);
	sip_timer_arm(&fk.env, &f.t_cw, 15000);
	acts_reset();
	ev_push(FSM_EV_SEIZE, 0);
	w_bell_stop();
	w_cancel(TI_CW);
	w_answer(0);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(!f.t_cw.armed);

	case_start("RINGING x CALL_REMOTE_CANCEL: a caller who gives up cancels it",
		   FSM_RINGING);
	sip_timer_arm(&fk.env, &f.t_cw, 15000);
	acts_reset();
	ev_push(FSM_EV_CALL_REMOTE_CANCEL, 0);
	w_bell_stop();
	w_cancel(TI_CW);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_IDLE);
	CHECK(!f.t_cw.armed);

	/* the timer object is shared, so the event it fires as has to follow
	 * the state: RINGING is the one state that reads it as the window */
	case_start("the ring window fires as its own event from RINGING alone",
		   FSM_RINGING);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_NOANSWER, "5553333");
	sip_timer_arm(&fk.env, &f.t_cw, 15000);
	acts_reset();
	fake_advance(15000);
	w_bell_stop();
	w_forward(0, "5553333");
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_IDLE);

	case_start("and as the call waiting budget from CONNECTED", FSM_CONNECTED);
	ev_push(FSM_EV_CW_NET, 2);
	acts_reset();
	fake_advance(30000);
	w_cw_net(false);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(!f.cw_net);

	/*
	 * The third entry into RINGING. The subscriber goes on hook while a
	 * waiting call is offered, so CW_OFFERED hands that call back to the
	 * bell. It is a ringing inbound call like any other and it gets the
	 * window; the arm rides the state entry, so no caller of it can forget.
	 */
	case_start("CW_OFFERED x RELEASE: the waiting call rings with the window "
		   "armed", FSM_CW_OFFERED);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_NOANSWER, "5553333");
	ev_push(FSM_EV_RELEASE, 0);
	w_cancel(TI_CW);
	w_cw(1, false);
	w_hangup(0);
	w_bell_start(1);
	w_arm(TI_CW, 15000);
	ACTS();
	STATE(FSM_RINGING);
	ROLES(1, FSM_SLOT_NONE, false);
	CHECK(f.t_cw.armed);

	/* the held leg dying is not this call ending, so the window survives */
	case_start("RINGING x CALL_FAILED: the held leg died, the window keeps "
		   "running", FSM_RINGING);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_NOANSWER, "5553333");
	roles_set(0, 1, true);
	sip_timer_arm(&fk.env, &f.t_cw, 15000);
	acts_reset();
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_QUIET));
	ACTS();
	STATE(FSM_RINGING);
	ROLES(0, FSM_SLOT_NONE, false);
	CHECK(f.t_cw.armed);

	/*
	 * The busy forward through the PENDING record: the subscriber flashed
	 * to answer a waiting call, the hold of the answered one failed, and
	 * PENDING refuses the waiting caller busy. That refusal goes through
	 * the shared helper like every other, so the caller diverts instead.
	 */
	case_start("PENDING x CALL_REINVITE_FAIL: the refused waiting caller "
		   "takes the busy forward", FSM_PENDING);
	feat_on(FEATURE_NONE);
	feat_cf_arm(FEATURE_CF_BUSY, "5552222");
	pend_set(FSM_OP_CW_ANSWER, 0, 1, FSM_TWO_CALLS, FSM_CONNECTED);
	acts_reset();
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 0);
	w_forward(1, "5552222");
	w_cw(1, false);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);
}

static void test_cw_net(void)
{
	case_start("CONNECTED x CW_NET: a play raises the alert and arms the budget",
		   FSM_CONNECTED);
	ev_push(FSM_EV_CW_NET, 2);
	w_cw_net(true);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(f.cw_net);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("CONNECTED x CW_NET: a repeated play re-presents and restarts it",
		   FSM_CONNECTED);
	ev_push(FSM_EV_CW_NET, 1);
	acts_reset();
	fake_advance(20000);
	ev_push(FSM_EV_CW_NET, 4);
	w_cw_net(true);
	w_cancel(TI_CW);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(f.cw_net);

	case_start("CONNECTED x CW_NET: call waiting off drops the play", FSM_CONNECTED);
	f.cfg.call_waiting = false;
	ev_push(FSM_EV_CW_NET, 2);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(!f.cw_net);
	CHECK(!f.t_cw.armed);

	/*
	 * The alert source branch of the same cell (section 5 item 13). The
	 * local mode serves the waiting call from this line, so a switch driven
	 * alert would announce a second caller that this line has no leg for.
	 */
	case_start("CONNECTED x CW_NET: the local mode drops the play", FSM_CONNECTED);
	f.cfg.cw_mode = FSM_CW_LOCAL;
	ev_push(FSM_EV_CW_NET, 2);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(!f.cw_net);
	CHECK(!f.t_cw.armed);

	/* the enable is untouched by the mode, here as at the local offer */
	case_start("CONNECTED x CW_NET: the local mode leaves the enable alone",
		   FSM_CONNECTED);
	f.cfg.cw_mode = FSM_CW_LOCAL;
	ev_push(FSM_EV_CW_NET, 2);
	ACTS();
	CHECK(f.cfg.call_waiting);

	/* the mode the option exists for: the switch serves the waiting call and
	 * this line presents its alert */
	case_start("CONNECTED x CW_NET: the network mode plays it", FSM_CONNECTED);
	f.cfg.cw_mode = FSM_CW_NETWORK;
	ev_push(FSM_EV_CW_NET, 2);
	w_cw_net(true);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(f.cw_net);

	/* the stop sits above the gate, so a mode that refuses every play still
	 * answers a stop off */
	case_start("CONNECTED x CW_NET: the local mode still answers a stop off",
		   FSM_CONNECTED);
	f.cfg.cw_mode = FSM_CW_LOCAL;
	ev_push(FSM_EV_CW_NET, 0);
	w_cw_net(false);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(!f.cw_net);

	case_start("CONNECTED x CW_NET: the stop ends the alert and the budget",
		   FSM_CONNECTED);
	ev_push(FSM_EV_CW_NET, 3);
	acts_reset();
	ev_push(FSM_EV_CW_NET, 0);
	w_cw_net(false);
	w_cancel(TI_CW);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(!f.cw_net);

	/* the cell is unconditional: the switch may stop an alert this line
	 * never presented, and the answer is the same off */
	case_start("CONNECTED x CW_NET: a stop with no alert running still answers off",
		   FSM_CONNECTED);
	ev_push(FSM_EV_CW_NET, 0);
	w_cw_net(false);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(!f.cw_net);

	case_start("CONNECTED x T_CW: the budget ends an alert that runs", FSM_CONNECTED);
	ev_push(FSM_EV_CW_NET, 2);
	acts_reset();
	armed_drop(&f.t_cw);
	ev_push(FSM_EV_T_CW, 0);
	w_cw_net(false);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(!f.cw_net);

	tu_case("CONNECTED x T_CW: the 30 s budget runs on the clock");
	cover_mark(FSM_CONNECTED, FSM_EV_T_CW);
	st_set(FSM_CONNECTED);
	fsm_event_push(&f, FSM_EV_CW_NET, 2);
	acts_reset();
	fake_advance(29999);
	ACTS();
	CHECK(f.cw_net);
	fake_advance(1);
	w_cw_net(false);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(!f.cw_net);

	case_start("CONNECTED x RELEASE: on hook ends the alert too", FSM_CONNECTED);
	ev_push(FSM_EV_CW_NET, 2);
	acts_reset();
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_cw_net(false);
	w_cancel(TI_CW);
	ACTS();
	STATE(FSM_IDLE);
	CHECK(!f.cw_net);

	case_start("CONNECTED x FLASH: the hold ends the alert too", FSM_CONNECTED);
	ev_push(FSM_EV_CW_NET, 2);
	acts_reset();
	ev_push(FSM_EV_FLASH, 0);
	w_hold(0);
	w_cw_net(false);
	w_cancel(TI_CW);
	ACTS();
	STATE(FSM_PENDING);
	CHECK(!f.cw_net);

	case_start("CONNECTED x CALL_REMOTE_BYE: the clear ends the alert too",
		   FSM_CONNECTED);
	ev_push(FSM_EV_CW_NET, 2);
	acts_reset();
	ev_push(FSM_EV_CALL_REMOTE_BYE, 0);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_CLEARING, 2000);
	w_cw_net(false);
	w_cancel(TI_CW);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_QUIET);
	CHECK(!f.cw_net);

	/* the local offer owns the alert and the timer from here on, so the
	 * network one has to end before the offer arms them */
	case_start("CONNECTED x CALL_INCOMING: the local offer takes the alert over",
		   FSM_CONNECTED);
	ev_push(FSM_EV_CW_NET, 2);
	acts_reset();
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_cw_net(false);
	w_cancel(TI_CW);
	w_ring180(1);
	w_cw(1, true);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_CW_OFFERED);
	CHECK(!f.cw_net);
	CHECK(f.t_cw.armed);

	case_start("TWO_CALLS x CW_NET: a play raises the alert and arms the budget",
		   FSM_TWO_CALLS);
	ev_push(FSM_EV_CW_NET, 1);
	w_cw_net(true);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_TWO_CALLS);
	CHECK(f.cw_net);
	ROLES(0, 1, true);

	case_start("TWO_CALLS x CW_NET: call waiting off drops the play", FSM_TWO_CALLS);
	f.cfg.call_waiting = false;
	ev_push(FSM_EV_CW_NET, 1);
	ACTS();
	STATE(FSM_TWO_CALLS);
	CHECK(!f.cw_net);

	case_start("TWO_CALLS x CW_NET: the local mode drops the play", FSM_TWO_CALLS);
	f.cfg.cw_mode = FSM_CW_LOCAL;
	ev_push(FSM_EV_CW_NET, 1);
	ACTS();
	STATE(FSM_TWO_CALLS);
	CHECK(!f.cw_net);

	case_start("TWO_CALLS x CW_NET: the network mode plays it", FSM_TWO_CALLS);
	f.cfg.cw_mode = FSM_CW_NETWORK;
	ev_push(FSM_EV_CW_NET, 1);
	w_cw_net(true);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_TWO_CALLS);
	CHECK(f.cw_net);

	case_start("TWO_CALLS x CW_NET: the stop ends the alert and the budget",
		   FSM_TWO_CALLS);
	ev_push(FSM_EV_CW_NET, 1);
	acts_reset();
	ev_push(FSM_EV_CW_NET, 0);
	w_cw_net(false);
	w_cancel(TI_CW);
	ACTS();
	STATE(FSM_TWO_CALLS);
	CHECK(!f.cw_net);

	case_start("TWO_CALLS x T_CW: the budget ends an alert that runs", FSM_TWO_CALLS);
	ev_push(FSM_EV_CW_NET, 1);
	acts_reset();
	armed_drop(&f.t_cw);
	ev_push(FSM_EV_T_CW, 0);
	w_cw_net(false);
	ACTS();
	STATE(FSM_TWO_CALLS);
	CHECK(!f.cw_net);

	case_start("TWO_CALLS x RELEASE: on hook ends the alert too", FSM_TWO_CALLS);
	ev_push(FSM_EV_CW_NET, 1);
	acts_reset();
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_bell_start(1);
	w_cw_net(false);
	w_cancel(TI_CW);
	ACTS();
	STATE(FSM_HOLDING);
	CHECK(!f.cw_net);

	/* CONNECTED and TWO_CALLS are one answered domain: the alert survives
	 * the step between them, because the call it belongs to survives it */
	case_start("TWO_CALLS x CALL_REMOTE_BYE: the held leg goes, the alert stays",
		   FSM_TWO_CALLS);
	ev_push(FSM_EV_CW_NET, 1);
	acts_reset();
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(f.cw_net);
	CHECK(f.t_cw.armed);

	tu_case("fsm_oos_force: an active network alert stops before the hangups");
	st_set(FSM_CONNECTED);
	fsm_event_push(&f, FSM_EV_CW_NET, 2);
	acts_reset();
	fsm_oos_force(&f);
	w_cw_net(false);
	w_cancel(TI_CW);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_OOS);
	CHECK(!f.cw_net);
	CHECK(!f.t_cw.armed);
}

/* ----------------------------------------------------------------- CONF */

static void test_conf(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 },
		{ FSM_EV_CALL_PROCEEDING, 0 }, { FSM_EV_CALL_ANSWERED, 0 },
		{ FSM_EV_CALL_REMOTE_CANCEL, 0 }, { FSM_EV_CALL_REINVITE_OK, 0 },
		{ FSM_EV_T_START_DIGIT, 0 },
		{ FSM_EV_T_INTERDIGIT, 0 }, { FSM_EV_T_WARMLINE, 0 },
		{ FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("CONF x RELEASE: the mixer stops, both legs go", FSM_CONF);
	ev_push(FSM_EV_RELEASE, 0);
	w_conf(false);
	w_hangup(0);
	w_hangup(1);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);
	CHECK(!f.conf_on);
	CHECK_UINT(f.slot_second, FSM_SLOT_NONE);

	/* the on-hook transfer predicate of TWO_CALLS, reused: it reads the
	 * two roles, and the conference fills both */
	case_start("CONF x RELEASE: an armed transfer joins the far parties",
		   FSM_CONF);
	f.xfer_armed = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_conf(false);
	w_tone(HAL_TONE_NONE);
	w_xfer_consult(1, 0);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);
	CHECK(!f.conf_on);
	CHECK(!f.xfer_armed);

	case_start("CONF x RELEASE: the configured form needs the directions",
		   FSM_CONF);
	f.cfg.xfer_on_hook = true;
	f.slot_inbound[1] = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_conf(false);
	w_tone(HAL_TONE_NONE);
	w_xfer_consult(1, 0);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	/* the same option with the directions the other way round: the on-hook
	 * is the ordinary end of the conference */
	case_start("CONF x RELEASE: the wrong directions hang both legs up",
		   FSM_CONF);
	f.cfg.xfer_on_hook = true;
	f.slot_inbound[0] = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_conf(false);
	w_hangup(0);
	w_hangup(1);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	/* the guard of the CONF RELEASE cell needs the other leg inbound; a
	 * subscriber who dialled both parties fails it with neither leg set,
	 * not by the swapped pair the case above uses */
	case_start("CONF x RELEASE: two outbound legs hang up although the "
		   "option is set", FSM_CONF);
	f.cfg.xfer_on_hook = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_conf(false);
	w_hangup(0);
	w_hangup(1);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	case_start("CONF x FLASH: the drop ends the second call", FSM_CONF);
	f.cfg.conf_mode = FSM_CONF_DROP;
	ev_push(FSM_EV_FLASH, 0);
	w_conf(false);
	w_hangup(1);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);
	CHECK(!f.conf_on);
	CHECK_UINT(f.slot_second, FSM_SLOT_NONE);

	/* the same cell after a swap: the second call is the active one now,
	 * and the drop follows the slot and not the role */
	case_start("CONF x FLASH: the drop follows the slot after a swap",
		   FSM_CONF);
	f.cfg.conf_mode = FSM_CONF_DROP;
	f.slot_second = 0;
	ev_push(FSM_EV_FLASH, 0);
	w_conf(false);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(1, FSM_SLOT_NONE, false);

	case_start("CONF x FLASH: the split holds the leg the join resumed",
		   FSM_CONF);
	f.cfg.conf_mode = FSM_CONF_SPLIT;
	ev_push(FSM_EV_FLASH, 0);
	w_hold(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_CONF_SPLIT, 1, 0, FSM_TWO_CALLS, FSM_CONF);
	NOCHAIN();
	ROLES(1, 0, false);
	/* the mixer runs until the hold answers */
	CHECK(f.conf_on);

	case_start("CONF x FLASH: the mode off leaves the conference alone",
		   FSM_CONF);
	ev_push(FSM_EV_FLASH, 0);
	ACTS();
	STATE(FSM_CONF);
	ROLES(0, 1, false);
	CHECK(f.conf_on);

	case_start("CONF x DIGIT: one request for both legs", FSM_CONF);
	ev_push(FSM_EV_DIGIT, '2');
	w_dtmf('2');
	ACTS();
	STATE(FSM_CONF);
	CHECK(f.conf_on);

	case_start("CONF x CALL_INCOMING: 486", FSM_CONF);
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_reject(0, 486);
	ACTS();
	STATE(FSM_CONF);

	case_start("CONF x CALL_FAILED: the survivor becomes the active leg",
		   FSM_CONF);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_QUIET));
	w_conf(false);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(1, FSM_SLOT_NONE, false);
	CHECK(!f.conf_on);

	case_start("CONF x CALL_FAILED: no leg survives", FSM_CONF);
	roles_set(0, FSM_SLOT_NONE, false);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_QUIET));
	w_conf(false);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_CLEARING, 2000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_QUIET);

	case_start("CONF x CALL_FAILED: a foreign slot is ignored", FSM_CONF);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(7, CALL_FAIL_QUIET));
	ACTS();
	STATE(FSM_CONF);
	ROLES(0, 1, false);
	CHECK(f.conf_on);

	case_start("CONF x CALL_REMOTE_BYE: the survivor becomes active",
		   FSM_CONF);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	w_conf(false);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("CONF x CALL_REMOTE_BYE: no leg survives", FSM_CONF);
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	w_conf(false);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_CLEARING, 2000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_QUIET);

	case_start("CONF x CALL_REMOTE_BYE: a foreign slot is ignored",
		   FSM_CONF);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 7);
	ACTS();
	STATE(FSM_CONF);
	CHECK(f.conf_on);

	/*
	 * The mixer refusal. It arrives as a re-INVITE failure for an owned
	 * leg after the join, so the machine splits and latches the refusal:
	 * the next flash in TWO_CALLS swaps.
	 */
	case_start("CONF x CALL_REINVITE_FAIL: a refused mixer splits it",
		   FSM_CONF);
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 0);
	w_hold(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_CONF_SPLIT, 1, 0, FSM_TWO_CALLS, FSM_CLEARING);
	NOCHAIN();
	ROLES(1, 0, false);
	CHECK(f.conf_refused);
	CHECK(f.conf_on);

	case_start("CONF x CALL_REINVITE_FAIL: a foreign slot is ignored",
		   FSM_CONF);
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 7);
	ACTS();
	STATE(FSM_CONF);
	CHECK(!f.conf_refused);
	CHECK(f.conf_on);

	/* the alert of CONNECTED runs in the conference too, and the conference
	 * survives it: CONNECTED, TWO_CALLS and CONF are one answered domain */
	case_start("CONF x CW_NET: a play raises the alert and arms the budget",
		   FSM_CONF);
	ev_push(FSM_EV_CW_NET, 2);
	w_cw_net(true);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_CONF);
	CHECK(f.cw_net);
	CHECK(f.conf_on);

	case_start("CONF x CW_NET: call waiting off drops the play", FSM_CONF);
	f.cfg.call_waiting = false;
	ev_push(FSM_EV_CW_NET, 2);
	ACTS();
	STATE(FSM_CONF);
	CHECK(!f.cw_net);

	case_start("CONF x CW_NET: the stop ends the alert and the budget",
		   FSM_CONF);
	ev_push(FSM_EV_CW_NET, 2);
	acts_reset();
	ev_push(FSM_EV_CW_NET, 0);
	w_cw_net(false);
	w_cancel(TI_CW);
	ACTS();
	STATE(FSM_CONF);
	CHECK(!f.cw_net);

	case_start("CONF x T_CW: the budget ends an alert that runs", FSM_CONF);
	ev_push(FSM_EV_CW_NET, 2);
	acts_reset();
	armed_drop(&f.t_cw);
	ev_push(FSM_EV_T_CW, 0);
	w_cw_net(false);
	ACTS();
	STATE(FSM_CONF);
	CHECK(!f.cw_net);
	CHECK(f.conf_on);

	IGNORES(FSM_CONF, ignores);
}

/*
 * -------------------------------------- TWO_CALLS FLASH, the guard fork
 *
 * The join and its completion. The fork adds guard branches to the FLASH
 * cell of TWO_CALLS and creates no cell: every case below marks that same
 * pair.
 */
static void conf_two_calls_set(enum fsm_conf_mode mode, unsigned int second)
{
	f.cfg.conf_mode = mode;
	f.slot_second = second;
}

static void test_conf_join(void)
{
	case_start("TWO_CALLS x FLASH: an eligible second call joins",
		   FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_DROP, 1);
	ev_push(FSM_EV_FLASH, 0);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_CONF, 1, 0, FSM_CONF, FSM_TWO_CALLS);
	NOCHAIN();
	/* the resume goes first and the mixer stays out of this cell */
	ROLES(1, 0, false);
	CHECK(!f.conf_on);

	/* the same guard after a swap: the second call is the active leg now,
	 * and the eligibility is the slot's and not the role's */
	case_start("TWO_CALLS x FLASH: a swap keeps the eligibility",
		   FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_DROP, 0);
	ev_push(FSM_EV_FLASH, 0);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_CONF, 1, 0, FSM_CONF, FSM_TWO_CALLS);

	/*
	 * The same property with nothing set up by hand. A pair whose second
	 * call came in swaps, the swap runs its two steps, and the flash after
	 * it swaps again: the second call is still the slot it always was, and
	 * the roles it now holds change nothing.
	 */
	case_start("TWO_CALLS x FLASH: a real swap keeps the second call",
		   FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_DROP, 1);
	f.slot_inbound[1] = true;
	ev_push(FSM_EV_FLASH, 0);
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);
	ev_push(FSM_EV_CALL_REINVITE_OK, 0);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	STATE(FSM_TWO_CALLS);
	ROLES(1, 0, true);
	CHECK_UINT(f.slot_second, 1);
	acts_reset();
	ev_push(FSM_EV_FLASH, 0);
	w_hold(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 1, 0, FSM_TWO_CALLS, FSM_TWO_CALLS);

	/*
	 * The same walk, with the shell stating the new direction of a slot it
	 * replaced the call in. The guard reads that slot and joins, so the
	 * eligibility follows the second call through a real swap.
	 */
	case_start("TWO_CALLS x FLASH: the second call joins after a real swap",
		   FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_DROP, 1);
	f.slot_inbound[1] = true;
	ev_push(FSM_EV_FLASH, 0);
	ev_push(FSM_EV_CALL_REINVITE_OK, 0);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	STATE(FSM_TWO_CALLS);
	ROLES(1, 0, true);
	fsm_slot_inbound_set(&f, 1, false);
	acts_reset();
	ev_push(FSM_EV_FLASH, 0);
	w_resume(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_CONF, 0, 1, FSM_CONF, FSM_TWO_CALLS);
	ROLES(0, 1, false);

	case_start("TWO_CALLS x FLASH: the split mode joins two outgoing calls",
		   FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_SPLIT, 1);
	ev_push(FSM_EV_FLASH, 0);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_CONF, 1, 0, FSM_CONF, FSM_TWO_CALLS);

	/*
	 * The first leg of the split mode, both ways round. The second call is
	 * the active one in the first case and the held one in the second, so
	 * the first call is the other of the two each time, and the mode
	 * refuses the join whichever slot the incoming call sits in.
	 */
	case_start("TWO_CALLS x FLASH: the split mode swaps when the first "
		   "call came in", FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_SPLIT, 0);
	f.slot_inbound[1] = true;
	ev_push(FSM_EV_FLASH, 0);
	w_hold(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);

	case_start("TWO_CALLS x FLASH: the split mode swaps on a mixed pair",
		   FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_SPLIT, 1);
	f.slot_inbound[0] = true;
	ev_push(FSM_EV_FLASH, 0);
	w_hold(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);

	/* the drop mode asks the first leg nothing, so the same pair joins */
	case_start("TWO_CALLS x FLASH: the drop mode joins a mixed pair",
		   FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_DROP, 1);
	f.slot_inbound[0] = true;
	ev_push(FSM_EV_FLASH, 0);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_CONF, 1, 0, FSM_CONF, FSM_TWO_CALLS);

	case_start("TWO_CALLS x FLASH: a second call that came in swaps",
		   FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_DROP, 1);
	f.slot_inbound[1] = true;
	ev_push(FSM_EV_FLASH, 0);
	w_hold(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);

	/*
	 * The same branch with nothing set by hand: the subscriber answers a
	 * waiting call, so the second call of the pair came in and the flash
	 * that follows swaps.
	 */
	case_start("TWO_CALLS x FLASH: a call waiting answer swaps, end to end",
		   FSM_CONNECTED);
	f.cfg.conf_mode = FSM_CONF_DROP;
	ev_push(FSM_EV_CALL_INCOMING, 1);
	STATE(FSM_CW_OFFERED);
	ev_push(FSM_EV_FLASH, 0);
	ev_push(FSM_EV_CALL_REINVITE_OK, 0);
	STATE(FSM_TWO_CALLS);
	ROLES(1, 0, true);
	CHECK_UINT(f.slot_second, 1);
	CHECK(f.slot_inbound[1]);
	ev_push(FSM_EV_FLASH, 0);
	w_hold(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 1, 0, FSM_TWO_CALLS, FSM_TWO_CALLS);

	case_start("TWO_CALLS x FLASH: the mode off swaps", FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_OFF, 1);
	ev_push(FSM_EV_FLASH, 0);
	w_hold(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);

	case_start("TWO_CALLS x FLASH: a shell with no mixer swaps",
		   FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_DROP, 1);
	ops_conf_clear();
	ev_push(FSM_EV_FLASH, 0);
	w_hold(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);

	case_start("TWO_CALLS x FLASH: no second call swaps", FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_DROP, FSM_SLOT_NONE);
	ev_push(FSM_EV_FLASH, 0);
	w_hold(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);

	case_start("TWO_CALLS x FLASH: an unowned second call swaps",
		   FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_DROP, 7);
	ev_push(FSM_EV_FLASH, 0);
	w_hold(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);

	case_start("PENDING x REINVITE_OK: the join starts the mixer",
		   FSM_PENDING);
	pend_set(FSM_OP_CONF, 1, 0, FSM_CONF, FSM_TWO_CALLS);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	w_conf(true);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONF);
	ROLES(0, 1, false);
	CHECK(f.conf_on);

	case_start("PENDING x REINVITE_FAIL: a refused resume keeps the calls",
		   FSM_PENDING);
	pend_set(FSM_OP_CONF, 1, 0, FSM_CONF, FSM_TWO_CALLS);
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 1);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_TWO_CALLS);
	ROLES(0, 1, true);

	/*
	 * The whole flow in one action log, with nothing built by hand: the
	 * resume goes out, the far end refuses it, and the subscriber hears
	 * one tone(NONE) and no busy, reorder or number-unobtainable tone
	 * anywhere.
	 */
	case_start("TWO_CALLS FLASH then REINVITE_FAIL: no busy in the flow",
		   FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_DROP, 1);
	ev_push(FSM_EV_FLASH, 0);
	STATE(FSM_PENDING);
	/* not ev_push: the action log survives so the flow is one list */
	fsm_event_push(&f, FSM_EV_CALL_REINVITE_FAIL, 1);
	w_resume(1);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_TWO_CALLS);
	ROLES(0, 1, true);

	/* the whole join, end to end */
	case_start("TWO_CALLS FLASH then REINVITE_OK: the conference runs",
		   FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_DROP, 1);
	ev_push(FSM_EV_FLASH, 0);
	fsm_event_push(&f, FSM_EV_CALL_REINVITE_OK, 1);
	w_resume(1);
	w_conf(true);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONF);
	ROLES(0, 1, false);
	CHECK(f.conf_on);
}

/* the split, its completion and the double fault */
static void test_conf_split(void)
{
	case_start("PENDING x REINVITE_OK: the split ends the mixer and "
		   "gives the calls back", FSM_PENDING);
	pend_set(FSM_OP_CONF_SPLIT, 1, 0, FSM_TWO_CALLS, FSM_CONF);
	f.conf_on = true;
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	w_conf(false);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_TWO_CALLS);
	ROLES(0, 1, true);
	CHECK(!f.conf_on);

	case_start("PENDING x REINVITE_FAIL: the user split returns to CONF",
		   FSM_PENDING);
	pend_set(FSM_OP_CONF_SPLIT, 1, 0, FSM_TWO_CALLS, FSM_CONF);
	f.conf_on = true;
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 1);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONF);
	ROLES(0, 1, false);
	CHECK(f.conf_on);

	/*
	 * The double fault: the mixer refused the conference and the re-hold
	 * of the leg the join resumed failed as well. That leg is hung up and
	 * released, so the line clears with the other leg held and no leg is
	 * left without a role.
	 */
	case_start("PENDING x REINVITE_FAIL: the double fault clears the line",
		   FSM_PENDING);
	pend_set(FSM_OP_CONF_SPLIT, 1, 0, FSM_TWO_CALLS, FSM_CLEARING);
	f.conf_on = true;
	f.conf_refused = true;
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 1);
	w_hangup(1);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	w_conf(false);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	ROLES(FSM_SLOT_NONE, 0, true);
	CHECK(!f.conf_on);

	/* the refusal end to end: the split runs, the two calls come back and
	 * the next flash swaps instead of joining again */
	case_start("CONF REINVITE_FAIL then FLASH: the latch swaps the flash",
		   FSM_CONF);
	f.cfg.conf_mode = FSM_CONF_DROP;
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 0);
	STATE(FSM_PENDING);
	CHECK(f.conf_refused);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	STATE(FSM_TWO_CALLS);
	ROLES(0, 1, true);
	CHECK(f.conf_refused);
	CHECK_UINT(f.slot_second, 1);
	ev_push(FSM_EV_FLASH, 0);
	w_hold(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);

	case_start("CONF REINVITE_FAIL then a dead leg: the latch clears",
		   FSM_CONF);
	f.cfg.conf_mode = FSM_CONF_DROP;
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 0);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	STATE(FSM_TWO_CALLS);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	STATE(FSM_CONNECTED);
	CHECK(!f.conf_refused);
	CHECK_UINT(f.slot_second, FSM_SLOT_NONE);
}

/* the far-end clear in PENDING: a leg of a conference operation dies inside
 * the round trip */
static void test_conf_dead(enum fsm_ev ev)
{
	const char *tag = ev == FSM_EV_CALL_FAILED ? "CALL_FAILED" :
			  "CALL_REMOTE_BYE";
	char name[160];
	uint32_t a0 = ev == FSM_EV_CALL_FAILED ?
		      fail_arg(0, CALL_FAIL_QUIET) : 0;
	uint32_t a1 = ev == FSM_EV_CALL_FAILED ?
		      fail_arg(1, CALL_FAIL_QUIET) : 1;

	snprintf(name, sizeof(name),
		 "PENDING x %s: the join lost the leg it resumed", tag);
	case_start(name, FSM_PENDING);
	pend_set(FSM_OP_CONF, 1, 0, FSM_CONF, FSM_TWO_CALLS);
	ev_push(ev, a1);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);

	snprintf(name, sizeof(name),
		 "PENDING x %s: the join lost the other leg", tag);
	case_start(name, FSM_PENDING);
	pend_set(FSM_OP_CONF, 1, 0, FSM_CONF, FSM_TWO_CALLS);
	ev_push(ev, a0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);
	NOCHAIN();
	ROLES(1, FSM_SLOT_NONE, false);

	snprintf(name, sizeof(name),
		 "PENDING x %s: the split lost the leg it held", tag);
	case_start(name, FSM_PENDING);
	pend_set(FSM_OP_CONF_SPLIT, 1, 0, FSM_TWO_CALLS, FSM_CONF);
	f.conf_on = true;
	ev_push(ev, a1);
	w_conf(false);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);
	CHECK(!f.conf_on);

	snprintf(name, sizeof(name),
		 "PENDING x %s: the split lost the other leg", tag);
	case_start(name, FSM_PENDING);
	pend_set(FSM_OP_CONF_SPLIT, 1, 0, FSM_TWO_CALLS, FSM_CONF);
	f.conf_on = true;
	ev_push(ev, a0);
	w_conf(false);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_CONF_SPLIT, 1, FSM_SLOT_NONE, FSM_PENDING, FSM_CONNECTED);
	CHAIN(true, false, FSM_OP_RESUME, 1, FSM_CONNECTED, FSM_CLEARING);
	CHECK(!f.conf_on);

	/* the rewritten record, both ways out: the hold answers and the chain
	 * retrieves the survivor, or it fails and the survivor is already
	 * unheld, which is CONNECTED */
	snprintf(name, sizeof(name),
		 "PENDING x %s: the rewritten split retrieves the leg", tag);
	case_start(name, FSM_PENDING);
	pend_set(FSM_OP_CONF_SPLIT, 1, 0, FSM_TWO_CALLS, FSM_CONF);
	f.conf_on = true;
	ev_push(ev, a0);
	acts_reset();
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);
	NOCHAIN();

	snprintf(name, sizeof(name),
		 "PENDING x %s: the rewritten split fails onto the leg", tag);
	case_start(name, FSM_PENDING);
	pend_set(FSM_OP_CONF_SPLIT, 1, 0, FSM_TWO_CALLS, FSM_CONF);
	f.conf_on = true;
	ev_push(ev, a0);
	acts_reset();
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 1);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(1, FSM_SLOT_NONE, false);
	CHECK(!f.conf_on);
}

/*
 * The second call of a pair, the settles that carry the conference, and the
 * public direction setter. These are properties across cells, so the cases
 * walk the machine instead of setting it up.
 */
static void test_conf_second(void)
{
	struct fsm before;

	tu_case("second call: fsm_init leaves the line without one");
	fsm_reset();
	CHECK_UINT(f.slot_second, FSM_SLOT_NONE);
	CHECK(!f.conf_on);
	CHECK(!f.conf_refused);

	case_start("second call: the first call of a seizure is not one",
		   FSM_IDLE);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	STATE(FSM_RINGING);
	CHECK_UINT(f.slot_second, FSM_SLOT_NONE);

	case_start("second call: a call dialled against a held one",
		   FSM_HOLD_DIAL);
	dp_dial();
	ev_push(FSM_EV_DIGIT, '5');
	STATE(FSM_CALLING);
	CHECK_UINT(f.slot_second, 0);

	case_start("second call: a call dialled with no held leg is not",
		   FSM_OFFHOOK_DIAL);
	dp_dial();
	ev_push(FSM_EV_DIGIT, '5');
	STATE(FSM_CALLING);
	CHECK_UINT(f.slot_second, FSM_SLOT_NONE);

	case_start("second call: the waiting call of CONNECTED is the second",
		   FSM_CONNECTED);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	STATE(FSM_CW_OFFERED);
	CHECK_UINT(f.slot_second, 1);

	case_start("second call: an offer against a held leg is the second",
		   FSM_HOLDING);
	ev_push(FSM_EV_CALL_INCOMING, 0);
	STATE(FSM_RINGING);
	CHECK_UINT(f.slot_second, 0);

	/*
	 * The refusal latch describes one pair, so the two acceptances that
	 * start a new pair clear it before they name the new second call.
	 */
	case_start("second call: a new pair in CONNECTED drops the old refusal",
		   FSM_CONNECTED);
	f.slot_second = 0;
	f.conf_refused = true;
	ev_push(FSM_EV_CALL_INCOMING, 1);
	STATE(FSM_CW_OFFERED);
	CHECK_UINT(f.slot_second, 1);
	CHECK(!f.conf_refused);

	case_start("second call: a new pair in HOLDING drops the old refusal",
		   FSM_HOLDING);
	f.slot_second = 1;
	f.conf_refused = true;
	ev_push(FSM_EV_CALL_INCOMING, 0);
	STATE(FSM_RINGING);
	CHECK_UINT(f.slot_second, 0);
	CHECK(!f.conf_refused);

	case_start("second call: a new seizure forgets the last pair",
		   FSM_IDLE);
	f.slot_second = 1;
	f.conf_refused = true;
	ev_push(FSM_EV_SEIZE, 0);
	STATE(FSM_OFFHOOK_DIAL);
	CHECK_UINT(f.slot_second, FSM_SLOT_NONE);
	CHECK(!f.conf_refused);

	case_start("second call: one call left is no pair", FSM_TWO_CALLS);
	f.slot_second = 1;
	f.conf_refused = true;
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	STATE(FSM_CONNECTED);
	CHECK_UINT(f.slot_second, FSM_SLOT_NONE);
	CHECK(!f.conf_refused);

	case_start("second call: the on-hook of a conference forgets it too",
		   FSM_CONF);
	f.conf_refused = true;
	ev_push(FSM_EV_RELEASE, 0);
	STATE(FSM_IDLE);
	CHECK_UINT(f.slot_second, FSM_SLOT_NONE);
	CHECK(!f.conf_refused);

	/*
	 * The alert of CONNECTED in the conference. It survives the join's own
	 * state, so an alert that arrives in CONF keeps running, and a running
	 * alert dies at the join, because the join goes through PENDING and
	 * that state carries no alert.
	 */
	case_start("settle: an alert that arrives in the conference runs on",
		   FSM_CONF);
	ev_push(FSM_EV_CW_NET, 3);
	w_cw_net(true);
	w_arm(TI_CW, 30000);
	ACTS();
	STATE(FSM_CONF);
	CHECK(f.cw_net);
	CHECK(f.t_cw.armed);

	case_start("settle: a running alert dies at the join", FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_DROP, 1);
	ev_push(FSM_EV_CW_NET, 1);
	acts_reset();
	ev_push(FSM_EV_FLASH, 0);
	w_resume(1);
	w_cw_net(false);
	w_cancel(TI_CW);
	ACTS();
	STATE(FSM_PENDING);
	CHECK(!f.cw_net);

	/* the armed transfer survives the join, so the on-hook of CONF fires
	 * it exactly as the on-hook of TWO_CALLS would have */
	case_start("settle: the armed transfer survives into the conference",
		   FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_DROP, 1);
	f.xfer_armed = true;
	ev_push(FSM_EV_FLASH, 0);
	CHECK(f.xfer_armed);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	STATE(FSM_CONF);
	CHECK(f.xfer_armed);
	ev_push(FSM_EV_RELEASE, 0);
	w_conf(false);
	w_tone(HAL_TONE_NONE);
	w_xfer_consult(1, 0);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	/* the on-hook of PENDING owns no mixer action, so the off lands at the
	 * settle, after both hangups */
	case_start("settle: an on-hook during a split ends the mixer last",
		   FSM_PENDING);
	pend_set(FSM_OP_CONF_SPLIT, 1, 0, FSM_TWO_CALLS, FSM_CONF);
	f.conf_on = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(1);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_conf(false);
	ACTS();
	STATE(FSM_IDLE);
	CHECK(!f.conf_on);

	tu_case("fsm_oos_force: from CONF the mixer stops before the hangups");
	st_set(FSM_CONF);
	f.conf_refused = true;
	acts_reset();
	fsm_oos_force(&f);
	w_conf(false);
	w_hangup(0);
	w_hangup(1);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_OOS);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);
	CHECK(!f.conf_on);
	CHECK_UINT(f.slot_second, FSM_SLOT_NONE);
	CHECK(!f.conf_refused);

	/*
	 * The public direction setter. It moves one slot's flag and nothing
	 * else, so a shell that puts another call into a slot the machine
	 * owns can state that call's direction.
	 */
	tu_case("fsm_slot_inbound_set: it moves one flag and nothing else");
	st_set(FSM_TWO_CALLS);
	acts_reset();
	memcpy(&before, &f, sizeof(before));
	fsm_slot_inbound_set(&f, 1, true);
	ACTS();
	CHECK(f.slot_inbound[1]);
	CHECK(!f.slot_inbound[0]);
	before.slot_inbound[1] = true;
	CHECK(memcmp(&before, &f, sizeof(before)) == 0);
	fsm_slot_inbound_set(&f, 1, false);
	CHECK(memcmp(&before, &f, sizeof(before)) != 0);
	before.slot_inbound[1] = false;
	CHECK(memcmp(&before, &f, sizeof(before)) == 0);

	tu_case("fsm_slot_inbound_set: a slot outside the pair is dropped");
	memcpy(&before, &f, sizeof(before));
	fsm_slot_inbound_set(&f, 7, true);
	ACTS();
	CHECK(memcmp(&before, &f, sizeof(before)) == 0);

	/* the direction the setter states is the one the eligibility reads */
	case_start("fsm_slot_inbound_set: the guard reads what the shell says",
		   FSM_TWO_CALLS);
	conf_two_calls_set(FSM_CONF_DROP, 1);
	fsm_slot_inbound_set(&f, 1, true);
	acts_reset();
	ev_push(FSM_EV_FLASH, 0);
	w_hold(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);
}

/* -------------------------------------------------------------- PENDING */

static void test_pending_basics(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 }, { FSM_EV_FLASH, 0 }, { FSM_EV_DIGIT, '5' },
		{ FSM_EV_CALL_PROCEEDING, 0 }, { FSM_EV_CALL_ANSWERED, 0 },
		{ FSM_EV_CW_NET, 0 }, { FSM_EV_CW_NET, 2 }, { FSM_EV_T_START_DIGIT, 0 },
		{ FSM_EV_T_INTERDIGIT, 0 }, { FSM_EV_T_WARMLINE, 0 },
		{ FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("PENDING x RELEASE: one leg torn down", FSM_PENDING);
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("PENDING x RELEASE: both legs torn down", FSM_PENDING);
	pend_set(FSM_OP_CW_ANSWER, 0, 1, FSM_TWO_CALLS, FSM_CONNECTED);
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_hangup(1);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	case_start("PENDING x CALL_INCOMING: 486 keeps the depth one rule",
		   FSM_PENDING);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_reject(1, 486);
	ACTS();
	STATE(FSM_PENDING);

	IGNORES(FSM_PENDING, ignores);
}

static void test_pending_ok(void)
{
	case_start("PENDING x REINVITE_OK: HOLD gives the recall dial tone",
		   FSM_PENDING);
	ev_push(FSM_EV_CALL_REINVITE_OK, 0);
	w_tone(HAL_TONE_SHORT_STUTTER_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_HOLD_DIAL);
	ROLES(FSM_SLOT_NONE, 0, true);
	DIGITS("");

	/* message-waiting: the stutter belongs to the first dial tone of a
	 * seizure. The recall tone of HOLD_DIAL announces a held call, and a
	 * subscriber already off hook has heard the indication once. */
	case_start("PENDING x REINVITE_OK: the recall tone ignores a waiting message",
		   FSM_PENDING);
	f.cfg.mwi = true;
	ev_push(FSM_EV_CALL_REINVITE_OK, 0);
	w_tone(HAL_TONE_SHORT_STUTTER_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_HOLD_DIAL);

	case_start("PENDING x REINVITE_OK: RESUME reconnects", FSM_PENDING);
	pend_set(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(1, FSM_SLOT_NONE, false);

	case_start("PENDING x REINVITE_OK: the swap resume step swaps the roles",
		   FSM_PENDING);
	pend_set(FSM_OP_RESUME, 1, 0, FSM_TWO_CALLS, FSM_PENDING);
	chain_set(FSM_OP_RESUME, 0, FSM_TWO_CALLS, FSM_CLEARING, false);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_TWO_CALLS);
	ROLES(1, 0, true);

	case_start("PENDING x REINVITE_OK: SWAP starts the resume step", FSM_PENDING);
	pend_set(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);
	ev_push(FSM_EV_CALL_REINVITE_OK, 0);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, 0, FSM_TWO_CALLS, FSM_PENDING);
	CHAIN(false, true, FSM_OP_RESUME, 0, FSM_TWO_CALLS, FSM_CLEARING);

	case_start("PENDING x REINVITE_OK: CW_ANSWER answers the waiting call",
		   FSM_PENDING);
	pend_set(FSM_OP_CW_ANSWER, 0, 1, FSM_TWO_CALLS, FSM_CONNECTED);
	ev_push(FSM_EV_CALL_REINVITE_OK, 0);
	w_answer(1);
	w_cw(1, false);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_TWO_CALLS);
	ROLES(1, 0, true);

	case_start("PENDING x REINVITE_OK: a success chain starts the next operation",
		   FSM_PENDING);
	pend_set(FSM_OP_SWAP, 0, FSM_SLOT_NONE, FSM_PENDING, FSM_CONNECTED);
	chain_set(FSM_OP_RESUME, 0, FSM_CONNECTED, FSM_CLEARING, true);
	ev_push(FSM_EV_CALL_REINVITE_OK, 0);
	w_resume(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 0, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);
	NOCHAIN();
}

static void test_pending_fail(void)
{
	case_start("PENDING x REINVITE_FAIL: HOLD leaves the call connected",
		   FSM_PENDING);
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 0);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("PENDING x REINVITE_FAIL: RESUME clears fast busy", FSM_PENDING);
	pend_set(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 1);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	ROLES(FSM_SLOT_NONE, 1, true);
	CHECK(f.held_refused);

	case_start("PENDING x REINVITE_FAIL: SWAP keeps the roles", FSM_PENDING);
	pend_set(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 0);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_TWO_CALLS);
	ROLES(0, 1, true);

	case_start("PENDING x REINVITE_FAIL: CW_ANSWER rejects the waiting call",
		   FSM_PENDING);
	pend_set(FSM_OP_CW_ANSWER, 0, 1, FSM_TWO_CALLS, FSM_CONNECTED);
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 0);
	w_reject(1, 486);
	w_cw(1, false);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);

	case_start("PENDING x REINVITE_FAIL: the swap fallback resumes the first leg",
		   FSM_PENDING);
	pend_set(FSM_OP_RESUME, 1, 0, FSM_TWO_CALLS, FSM_PENDING);
	chain_set(FSM_OP_RESUME, 0, FSM_TWO_CALLS, FSM_CLEARING, false);
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 1);
	w_resume(0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 0, 1, FSM_TWO_CALLS, FSM_CLEARING);
	NOCHAIN();
}

/* the far end clears in PENDING while an operation is outstanding */
static void test_pending_dead(enum fsm_ev ev)
{
	const char *tag = ev == FSM_EV_CALL_FAILED ? "CALL_FAILED" : "CALL_REMOTE_BYE";
	char name[160];
	uint32_t a0 = ev == FSM_EV_CALL_FAILED ? fail_arg(0, CALL_FAIL_QUIET) : 0;
	uint32_t a1 = ev == FSM_EV_CALL_FAILED ? fail_arg(1, CALL_FAIL_QUIET) : 1;

	snprintf(name, sizeof(name), "PENDING x %s: HOLD lost its only leg", tag);
	case_start(name, FSM_PENDING);
	ev_push(ev, a0);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_CLEARING, 2000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_QUIET);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	snprintf(name, sizeof(name), "PENDING x %s: RESUME lost its only leg", tag);
	case_start(name, FSM_PENDING);
	pend_set(FSM_OP_RESUME, 0, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);
	ev_push(ev, a0);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_CLEARING, 2000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_QUIET);

	snprintf(name, sizeof(name),
		 "PENDING x %s: RESUME lost slot_op, the other leg is retrieved", tag);
	case_start(name, FSM_PENDING);
	pend_set(FSM_OP_RESUME, 0, 1, FSM_TWO_CALLS, FSM_PENDING);
	chain_set(FSM_OP_RESUME, 1, FSM_TWO_CALLS, FSM_CLEARING, false);
	ev_push(ev, a0);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);
	NOCHAIN();

	snprintf(name, sizeof(name), "PENDING x %s: RESUME lost slot_other", tag);
	case_start(name, FSM_PENDING);
	pend_set(FSM_OP_RESUME, 0, 1, FSM_TWO_CALLS, FSM_PENDING);
	chain_set(FSM_OP_RESUME, 1, FSM_TWO_CALLS, FSM_CLEARING, false);
	ev_push(ev, a1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 0, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);
	NOCHAIN();

	snprintf(name, sizeof(name), "PENDING x %s: SWAP lost the leg it was holding",
		 tag);
	case_start(name, FSM_PENDING);
	pend_set(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);
	ev_push(ev, a0);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);
	NOCHAIN();

	snprintf(name, sizeof(name), "PENDING x %s: SWAP lost the held leg", tag);
	case_start(name, FSM_PENDING);
	pend_set(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);
	ev_push(ev, a1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 0, FSM_SLOT_NONE, FSM_PENDING, FSM_CONNECTED);
	CHAIN(true, false, FSM_OP_RESUME, 0, FSM_CONNECTED, FSM_CLEARING);

	snprintf(name, sizeof(name), "PENDING x %s: CW_ANSWER lost the active leg",
		 tag);
	case_start(name, FSM_PENDING);
	pend_set(FSM_OP_CW_ANSWER, 0, 1, FSM_TWO_CALLS, FSM_CONNECTED);
	ev_push(ev, a0);
	w_cw(1, false);
	w_answer(1);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(1, FSM_SLOT_NONE, false);

	snprintf(name, sizeof(name), "PENDING x %s: CW_ANSWER lost the waiting leg",
		 tag);
	case_start(name, FSM_PENDING);
	pend_set(FSM_OP_CW_ANSWER, 0, 1, FSM_TWO_CALLS, FSM_CONNECTED);
	ev_push(ev, a1);
	w_cw(1, false);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_CW_ANSWER, 0, FSM_SLOT_NONE, FSM_PENDING, FSM_CONNECTED);
	CHAIN(true, false, FSM_OP_RESUME, 0, FSM_CONNECTED, FSM_CLEARING);

	snprintf(name, sizeof(name), "PENDING x %s: a foreign slot is ignored", tag);
	case_start(name, FSM_PENDING);
	ev_push(ev, a1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_HOLD, 0, FSM_SLOT_NONE, FSM_HOLD_DIAL, FSM_CONNECTED);
	NOCHAIN();
	ROLES(0, FSM_SLOT_NONE, false);

	snprintf(name, sizeof(name),
		 "PENDING x %s: a foreign slot leaves a chained record alone", tag);
	case_start(name, FSM_PENDING);
	pend_set(FSM_OP_RESUME, 0, 1, FSM_TWO_CALLS, FSM_PENDING);
	chain_set(FSM_OP_RESUME, 1, FSM_TWO_CALLS, FSM_CLEARING, false);
	ev_push(ev, ev == FSM_EV_CALL_FAILED ? fail_arg(7, CALL_FAIL_QUIET) : 7);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 0, 1, FSM_TWO_CALLS, FSM_PENDING);
	CHAIN(false, true, FSM_OP_RESUME, 1, FSM_TWO_CALLS, FSM_CLEARING);
	ROLES(0, 1, true);
}

static void test_pending_cancel(void)
{
	case_start("PENDING x CALL_REMOTE_CANCEL: the waiting call withdrew",
		   FSM_PENDING);
	pend_set(FSM_OP_CW_ANSWER, 0, 1, FSM_TWO_CALLS, FSM_CONNECTED);
	ev_push(FSM_EV_CALL_REMOTE_CANCEL, 1);
	w_cw(1, false);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_CW_ANSWER, 0, FSM_SLOT_NONE, FSM_PENDING, FSM_CONNECTED);
	CHAIN(true, false, FSM_OP_RESUME, 0, FSM_CONNECTED, FSM_CLEARING);

	case_start("PENDING x CALL_REMOTE_CANCEL: the operated leg is ignored",
		   FSM_PENDING);
	pend_set(FSM_OP_CW_ANSWER, 0, 1, FSM_TWO_CALLS, FSM_CONNECTED);
	ev_push(FSM_EV_CALL_REMOTE_CANCEL, 0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_CW_ANSWER, 0, 1, FSM_TWO_CALLS, FSM_CONNECTED);

	case_start("PENDING x CALL_REMOTE_CANCEL: another operation ignores it",
		   FSM_PENDING);
	pend_set(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);
	ev_push(FSM_EV_CALL_REMOTE_CANCEL, 1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);
}

/* ---------------------------------------------------------- XFER_COLLECT */

/*
 * The transferor surface. The XFER_COLLECT state, the two outcome columns,
 * and the guard branches the transfer adds to the DIGIT cells of
 * OFFHOOK_DIAL, COLLECTING and HOLD_DIAL and to the RELEASE cell of
 * TWO_CALLS. No cell is created by a guard branch: those cases mark the same
 * (state, event) pair (test contract rule 2).
 */
static void pend_xfer_set(unsigned int held)
{
	pend_set(FSM_OP_XFER, held, FSM_SLOT_NONE, FSM_OFFHOOK_DIAL, FSM_PENDING);
	chain_set(FSM_OP_RESUME, held, FSM_CONNECTED, FSM_CLEARING, false);
}

static void test_xfer_collect(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 }, { FSM_EV_CALL_PROCEEDING, 0 },
		{ FSM_EV_CALL_ANSWERED, 0 }, { FSM_EV_CALL_REMOTE_CANCEL, 0 },
		{ FSM_EV_CALL_REINVITE_OK, 0 }, { FSM_EV_CALL_REINVITE_FAIL, 0 },
		{ FSM_EV_CW_NET, 0 }, { FSM_EV_CW_NET, 2 },
		{ FSM_EV_T_START_DIGIT, 0 }, { FSM_EV_T_WARMLINE, 0 },
		{ FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("XFER_COLLECT x RELEASE: the held leg is parked with the recall "
		   "ring", FSM_XFER_COLLECT);
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);
	DIGITS("");

	case_start("XFER_COLLECT x FLASH: the transfer is abandoned and the leg comes "
		   "back", FSM_XFER_COLLECT);
	digits_set("555");
	acts_reset();
	ev_push(FSM_EV_FLASH, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);
	NOCHAIN();
	DIGITS("");

	case_start("XFER_COLLECT x DIGIT: WAIT_SHORT collects the target",
		   FSM_XFER_COLLECT);
	dp_short();
	ev_push(FSM_EV_DIGIT, '5');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 4000);
	ACTS();
	STATE(FSM_XFER_COLLECT);
	DIGITS("5");
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("XFER_COLLECT x DIGIT: WAIT_LONG collects the target",
		   FSM_XFER_COLLECT);
	dp_long();
	ev_push(FSM_EV_DIGIT, '5');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_XFER_COLLECT);
	DIGITS("5");

	/*
	 * The blind completion. The op runs and the machine parks in
	 * PENDING{XFER} with nothing in the action list behind it. The wait is
	 * silent: a busy tone between a transfer request and its outcome reads
	 * as a failed transfer.
	 */
	case_start("XFER_COLLECT x DIGIT: a DIAL verdict refers the held leg",
		   FSM_XFER_COLLECT);
	dp_dial();
	ev_push(FSM_EV_DIGIT, '5');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_xfer(1, FEATURE_ACT_XFER_BLIND, "5");
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_XFER, 1, FSM_SLOT_NONE, FSM_OFFHOOK_DIAL, FSM_PENDING);
	CHAIN(false, true, FSM_OP_RESUME, 1, FSM_CONNECTED, FSM_CLEARING);
	ROLES(1, FSM_SLOT_NONE, false);

	case_start("XFER_COLLECT x DIGIT: a '#' the map cannot take ends the "
		   "target too", FSM_XFER_COLLECT);
	f.xfer_act = FEATURE_ACT_XFER_ATTENDED;
	digits_set("555");
	acts_reset();
	dp_reorder();
	ev_push(FSM_EV_DIGIT, '#');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_xfer(1, FEATURE_ACT_XFER_ATTENDED, "555");
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_XFER, 1, FSM_SLOT_NONE, FSM_OFFHOOK_DIAL, FSM_PENDING);

	/* a full target cannot take another character, so its '#' completes
	 * without a map probe; the overflow branch below it never sees a '#' */
	case_start("XFER_COLLECT x DIGIT: a '#' on a full target completes it",
		   FSM_XFER_COLLECT);
	f.xfer_act = FEATURE_ACT_XFER_BLIND;
	digits_set("01234567890123456789012345678901");
	acts_reset();
	dp_long();
	ev_push(FSM_EV_DIGIT, '#');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_xfer(1, FEATURE_ACT_XFER_BLIND,
	       "01234567890123456789012345678901");
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_XFER, 1, FSM_SLOT_NONE, FSM_OFFHOOK_DIAL, FSM_PENDING);

	/* the consult row refers nothing yet: it arms the on-hook transfer and
	 * dials the target as an ordinary second call, keeping the held leg */
	case_start("XFER_COLLECT x DIGIT: the consult row dials instead of referring",
		   FSM_XFER_COLLECT);
	f.xfer_act = FEATURE_ACT_XFER_CONSULT;
	dp_dial();
	acts_reset();
	ev_push(FSM_EV_DIGIT, '5');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_dial(0, "5");
	ACTS();
	STATE(FSM_CALLING);
	ROLES(0, 1, true);
	CHECK(f.xfer_armed);

	case_start("XFER_COLLECT x DIGIT: NO_MATCH clears fast busy, held leg kept",
		   FSM_XFER_COLLECT);
	dp_reorder();
	ev_push(FSM_EV_DIGIT, '9');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");
	ROLES(FSM_SLOT_NONE, 1, true);

	/* the SPRE_COLLECT overflow rule: a target that does not fit refuses
	 * the whole transfer instead of referring a cut number */
	case_start("XFER_COLLECT x DIGIT: a target that does not fit is refused",
		   FSM_XFER_COLLECT);
	digits_set("12345678901234567890123456789012");
	CHECK_UINT(f.ndigits, FSM_DIGITS_MAX);
	acts_reset();
	ev_push(FSM_EV_DIGIT, '9');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");

	case_start("XFER_COLLECT x CALL_INCOMING: 486, the line owns a call",
		   FSM_XFER_COLLECT);
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_reject(0, 486);
	ACTS();
	STATE(FSM_XFER_COLLECT);

	case_start("XFER_COLLECT x CALL_FAILED: the subject of the transfer died",
		   FSM_XFER_COLLECT);
	digits_set("555");
	acts_reset();
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_QUIET));
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("XFER_COLLECT x CALL_FAILED: a slot this state does not own is "
		   "ignored", FSM_XFER_COLLECT);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_QUIET));
	ACTS();
	STATE(FSM_XFER_COLLECT);
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("XFER_COLLECT x CALL_REMOTE_BYE: the far end cleared, quiet",
		   FSM_XFER_COLLECT);
	digits_set("555");
	acts_reset();
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_CLEARING, 2000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_QUIET);
	DIGITS("");

	case_start("XFER_COLLECT x CALL_REMOTE_BYE: a foreign slot is ignored",
		   FSM_XFER_COLLECT);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 0);
	ACTS();
	STATE(FSM_XFER_COLLECT);
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("XFER_COLLECT x T_INTERDIGIT: the short timer completes the target",
		   FSM_XFER_COLLECT);
	digits_set("5");
	dp_short();
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_tone(HAL_TONE_NONE);
	w_xfer(1, FEATURE_ACT_XFER_BLIND, "5");
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_XFER, 1, FSM_SLOT_NONE, FSM_OFFHOOK_DIAL, FSM_PENDING);

	case_start("XFER_COLLECT x T_INTERDIGIT: the long timer abandons the target",
		   FSM_XFER_COLLECT);
	digits_set("5");
	dp_long();
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("XFER_COLLECT x T_INTERDIGIT: an empty buffer ends the same way",
		   FSM_XFER_COLLECT);
	dp_long();
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);

	/* the map "T" on the empty buffer: the T completes it, and the target
	 * without its marker is empty */
	case_start("XFER_COLLECT x T_INTERDIGIT: \"T\" refers no empty target",
		   FSM_XFER_COLLECT);
	dp_set(false, true, false, false);
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");
	ROLES(FSM_SLOT_NONE, 1, true);
	CHECK(strcmp(dp_seen, "T") == 0);

	/* the map "TT": the first expiry leaves "T", and a '#' behind it ends
	 * no number, so the map that refuses "T#" clears the collector */
	case_start("XFER_COLLECT x DIGIT: a '#' behind a T alone refers no "
		   "empty target", FSM_XFER_COLLECT);
	dp_timer_next();
	dp_after_timer_set(false, true, false);
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 4000);
	ACTS();
	STATE(FSM_XFER_COLLECT);
	DIGITS("T");
	dp_reorder();
	ev_push(FSM_EV_DIGIT, '#');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");
	ROLES(FSM_SLOT_NONE, 1, true);
	CHECK(strcmp(dp_seen, "T#") == 0);

	case_start("XFER_COLLECT x T_INTERDIGIT: \"9T11\" refers 911 "
		   "without the T", FSM_XFER_COLLECT);
	digits_set("9");
	dp_timer_next();
	dp_after_timer_set(false, false, true);
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_XFER_COLLECT);
	DIGITS("9T");
	dp_long();
	ev_push(FSM_EV_DIGIT, '1');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	DIGITS("9T1");
	dp_dial();
	ev_push(FSM_EV_DIGIT, '1');
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_xfer(1, FEATURE_ACT_XFER_BLIND, "911");
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_XFER, 1, FSM_SLOT_NONE, FSM_OFFHOOK_DIAL, FSM_PENDING);
	DIGITS("911");

	IGNORES(FSM_XFER_COLLECT, ignores);
}

/* the HOLD_DIAL DIGIT and T_INTERDIGIT guard branches: the transfer codes */
static void test_xfer_hold_dial(void)
{
	static const struct {
		const char *code;
		enum feature_action act;
		const char *name;
	} exact[] = {
		{ "*072", FEATURE_ACT_XFER_BLIND, "blind" },
		{ "*073", FEATURE_ACT_XFER_ATTENDED, "attended" },
		{ "*074", FEATURE_ACT_XFER_CONSULT, "consult" },
	};
	char name[160];
	unsigned int i;

	for (i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
		snprintf(name, sizeof(name),
			 "HOLD_DIAL x DIGIT: the %s transfer code opens the second stage",
			 exact[i].name);
		case_start(name, FSM_HOLD_DIAL);
		feat_on(FEATURE_NONE);
		xfer_codes_arm();
		digits_set("*07");
		sip_timer_arm(&fk.env, &f.t_digit, cfg.interdigit_long_ms);
		acts_reset();
		ev_push(FSM_EV_DIGIT, exact[i].code[3]);
		w_cancel(TI_DIGIT);
		w_tone(HAL_TONE_NONE);
		w_tone(HAL_TONE_SECOND_DIAL);
		w_arm(TI_DIGIT, 16000);
		ACTS();
		STATE(FSM_XFER_COLLECT);
		DIGITS("");
		ROLES(FSM_SLOT_NONE, 1, true);
		CHECK_UINT(f.xfer_act, exact[i].act);
	}

	/*
	 * The precedence rule. A buffer that can still become a transfer code
	 * does not reach the dial plan, so a dial plan that would clear the
	 * line right here does not get the chance.
	 */
	case_start("HOLD_DIAL x DIGIT: a transfer prefix keeps the dial plan out",
		   FSM_HOLD_DIAL);
	feat_on(FEATURE_NONE);
	xfer_codes_arm();
	dp_reorder();
	dp_calls = 0;
	ev_push(FSM_EV_DIGIT, '*');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_HOLD_DIAL);
	DIGITS("*");
	ROLES(FSM_SLOT_NONE, 1, true);
	CHECK_UINT(dp_calls, 0);

	case_start("HOLD_DIAL x T_INTERDIGIT: an abandoned transfer prefix that the "
		   "dial plan can dial", FSM_HOLD_DIAL);
	feat_on(FEATURE_NONE);
	xfer_codes_arm();
	digits_set("*07");
	dp_dial();
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_dial(0, "*07");
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CALLING);
	ROLES(0, 1, true);

	/*
	 * Only a DIAL verdict commits such a buffer. The ordinary cell would
	 * dial a WAIT_SHORT verdict too, because that is what its own short
	 * timer means; this buffer never armed a short timer.
	 */
	case_start("HOLD_DIAL x T_INTERDIGIT: any other verdict on an abandoned "
		   "prefix is the fast busy", FSM_HOLD_DIAL);
	feat_on(FEATURE_NONE);
	xfer_codes_arm();
	digits_set("*07");
	dp_short();
	armed_drop(&f.t_digit);
	ev_push(FSM_EV_T_INTERDIGIT, 0);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");
	ROLES(FSM_SLOT_NONE, 1, true);

	/* the other half of the rule: no other code is legal here, so a
	 * complete non-transfer code is an ordinary dial string */
	case_start("HOLD_DIAL x DIGIT: a non-transfer code still falls through",
		   FSM_HOLD_DIAL);
	feat_on(FEATURE_MATCH);
	xfer_codes_arm();
	digits_set("*4");
	dp_long();
	ev_push(FSM_EV_DIGIT, '3');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_HOLD_DIAL);
	DIGITS("*43");
	CHECK_UINT(feat_calls, 0);

	case_start("HOLD_DIAL x DIGIT: a prefix of a non-transfer code falls through "
		   "too", FSM_HOLD_DIAL);
	feat_on(FEATURE_PARTIAL);
	xfer_codes_arm();
	digits_set("*4");
	dp_reorder();
	ev_push(FSM_EV_DIGIT, '9');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	CHECK_UINT(feat_calls, 0);

	/*
	 * The transfer hook gate, one case per prerequisite. The classification
	 * needs both ops and the loaded table, so a shell that is missing any
	 * one of the three keeps the plain digit path in this cell. That is
	 * what keeps a half wired shell away from a NULL op and a NULL table.
	 */
	case_start("HOLD_DIAL x DIGIT: with call_xfer absent the cell is unchanged",
		   FSM_HOLD_DIAL);
	feat_on(FEATURE_NONE);
	xfer_codes_arm();
	ops_xfer_set(false, true);
	digits_set("*07");
	dp_long();
	ev_push(FSM_EV_DIGIT, '2');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_HOLD_DIAL);
	DIGITS("*072");

	case_start("HOLD_DIAL x DIGIT: with call_xfer_consult absent the cell is "
		   "unchanged", FSM_HOLD_DIAL);
	feat_on(FEATURE_NONE);
	xfer_codes_arm();
	ops_xfer_set(true, false);
	digits_set("*07");
	dp_long();
	ev_push(FSM_EV_DIGIT, '2');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_HOLD_DIAL);
	DIGITS("*072");

	/*
	 * The third prerequisite: both ops wired and no feature table at all.
	 * The buffer here is a strict prefix of a transfer exten, so the
	 * classification would hold it and stay; the dial plan clears the line
	 * instead, which is the plain digit path of the cell.
	 */
	case_start("HOLD_DIAL x DIGIT: with no feature table the dial plan still "
		   "rules", FSM_HOLD_DIAL);
	xfer_codes_arm();
	digits_set("*");
	dp_reorder();
	ev_push(FSM_EV_DIGIT, '0');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");
	ROLES(FSM_SLOT_NONE, 1, true);
}

/* the OFFHOOK_DIAL and COLLECTING DIGIT guard branch: a transfer code with no
 * held call */
static void test_xfer_no_held_call(void)
{
	case_start("OFFHOOK_DIAL x DIGIT: a transfer code with no held call refuses "
		   "at once", FSM_OFFHOOK_DIAL);
	feat_on(FEATURE_MATCH);
	xfer_codes_arm();
	/* features.c refuses to execute the three rows, and the row would open
	 * the second stage if the exclusion were not there */
	feat_arg_wanted = true;
	feat_ok = false;
	digits_set("*07");
	acts_reset();
	ev_push(FSM_EV_DIGIT, '2');
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_NONE);
	w_feature("*072");
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");

	case_start("COLLECTING x DIGIT: the same refusal mid number", FSM_COLLECTING);
	feat_on(FEATURE_MATCH);
	xfer_codes_arm();
	feat_arg_wanted = true;
	feat_ok = false;
	digits_set("*07");
	acts_reset();
	ev_push(FSM_EV_DIGIT, '3');
	w_feature("*073");
	w_cancel(TI_DIGIT);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	DIGITS("");

	/* the negative: a forward set code still opens the second stage */
	case_start("COLLECTING x DIGIT: a forward code still opens the second stage",
		   FSM_COLLECTING);
	feat_on(FEATURE_MATCH);
	xfer_codes_arm();
	feat_arg_wanted = true;
	digits_set("*21");
	acts_reset();
	ev_push(FSM_EV_DIGIT, '#');
	w_tone(HAL_TONE_SECOND_DIAL);
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_SPRE_COLLECT);
	DIGITS("*21#");
}

/* the TWO_CALLS RELEASE guard branch: the on-hook consult transfer */
static void test_xfer_on_hook(void)
{
	case_start("TWO_CALLS x RELEASE: an armed transfer joins the two far parties",
		   FSM_TWO_CALLS);
	f.xfer_armed = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_xfer_consult(1, 0);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);
	CHECK(!f.xfer_armed);

	case_start("TWO_CALLS x RELEASE: the configured form needs the leg directions",
		   FSM_TWO_CALLS);
	f.cfg.xfer_on_hook = true;
	f.slot_inbound[1] = true;
	f.slot_inbound[0] = false;
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_xfer_consult(1, 0);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	case_start("TWO_CALLS x RELEASE: both legs outbound is the ordinary park",
		   FSM_TWO_CALLS);
	f.cfg.xfer_on_hook = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("TWO_CALLS x RELEASE: both legs inbound is the ordinary park too",
		   FSM_TWO_CALLS);
	f.cfg.xfer_on_hook = true;
	f.slot_inbound[0] = true;
	f.slot_inbound[1] = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);

	case_start("TWO_CALLS x RELEASE: the directions the other way round park too",
		   FSM_TWO_CALLS);
	f.cfg.xfer_on_hook = true;
	f.slot_inbound[0] = true;
	f.slot_inbound[1] = false;
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);

	case_start("TWO_CALLS x RELEASE: the right directions with the option off park",
		   FSM_TWO_CALLS);
	f.slot_inbound[1] = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);

	/*
	 * The cell lists neither network-alert action: leaving TWO_CALLS rides
	 * the choke point of the network alert exit like every other exit, so
	 * the alert off and the cancel land at the end of the list.
	 */
	case_start("TWO_CALLS x RELEASE: the transfer still ends a network alert",
		   FSM_TWO_CALLS);
	f.xfer_armed = true;
	ev_push(FSM_EV_CW_NET, 1);
	acts_reset();
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_xfer_consult(1, 0);
	w_bell_stop();
	w_cw_net(false);
	w_cancel(TI_CW);
	ACTS();
	STATE(FSM_IDLE);
	CHECK(!f.cw_net);

	/* the star code bypasses the direction rule, which the configured form
	 * needs */
	case_start("TWO_CALLS x RELEASE: an armed transfer ignores the directions",
		   FSM_TWO_CALLS);
	f.xfer_armed = true;
	f.slot_inbound[0] = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_xfer_consult(1, 0);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);
}

/* PENDING with op XFER, and the XFER row of the far-end clear in PENDING */
static void test_xfer_pending(void)
{
	case_start("PENDING x CALL_XFER_OK: the leg is gone and the line gets a fresh "
		   "dial tone", FSM_PENDING);
	pend_xfer_set(1);
	acts_reset();
	ev_push(FSM_EV_CALL_XFER_OK, 1);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);
	DIGITS("");

	/* the on_success entry re-runs its hotline test, so such a line dials */
	case_start("PENDING x CALL_XFER_OK: a hotline line dials again at once",
		   FSM_PENDING);
	pend_xfer_set(1);
	f.cfg.warmline = true;
	f.cfg.warmline_ms = 0;
	snprintf(f.cfg.warmline_number, sizeof(f.cfg.warmline_number), "18005551212");
	acts_reset();
	ev_push(FSM_EV_CALL_XFER_OK, 1);
	w_tone(HAL_TONE_NONE);
	w_dial(0, "18005551212");
	ACTS();
	STATE(FSM_CALLING);

	case_start("PENDING x CALL_XFER_OK: a foreign slot is ignored", FSM_PENDING);
	pend_xfer_set(1);
	acts_reset();
	ev_push(FSM_EV_CALL_XFER_OK, 0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_XFER, 1, FSM_SLOT_NONE, FSM_OFFHOOK_DIAL, FSM_PENDING);

	/*
	 * The tone rule. The transfer failed and the call did not, so the leg
	 * comes back and the line plays no tone. The fast busy that a refused
	 * star code earns elsewhere would tell the subscriber that the call
	 * failed.
	 */
	case_start("PENDING x CALL_XFER_FAIL: the held leg is resumed with no tone",
		   FSM_PENDING);
	pend_xfer_set(1);
	acts_reset();
	ev_push(FSM_EV_CALL_XFER_FAIL, 1);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);
	NOCHAIN();

	/*
	 * The same rule end to end, with nothing constructed by hand. The
	 * completion of XFER_COLLECT and the failure that answers it run in one
	 * action log, so the whole sequence a subscriber would hear is asserted
	 * at once: the recall tone stops, the op goes out, the leg comes back,
	 * and there is no second tone anywhere in between.
	 */
	case_start("XFER_COLLECT DIGIT then PENDING CALL_XFER_FAIL: one tone in the "
		   "whole flow", FSM_XFER_COLLECT);
	dp_dial();
	ev_push(FSM_EV_DIGIT, '5');
	STATE(FSM_PENDING);
	/* not ev_push: the action log survives so the flow is one list */
	fsm_event_push(&f, FSM_EV_CALL_XFER_FAIL, 1);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_DIGIT);
	w_xfer(1, FEATURE_ACT_XFER_BLIND, "5");
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);
	NOCHAIN();

	case_start("PENDING x CALL_XFER_FAIL: a foreign slot is ignored", FSM_PENDING);
	pend_xfer_set(1);
	acts_reset();
	ev_push(FSM_EV_CALL_XFER_FAIL, 0);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_XFER, 1, FSM_SLOT_NONE, FSM_OFFHOOK_DIAL, FSM_PENDING);
	CHAIN(false, true, FSM_OP_RESUME, 1, FSM_CONNECTED, FSM_CLEARING);

	case_start("PENDING x REINVITE_OK: op XFER owns no re-INVITE", FSM_PENDING);
	pend_xfer_set(1);
	acts_reset();
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_XFER, 1, FSM_SLOT_NONE, FSM_OFFHOOK_DIAL, FSM_PENDING);
	CHAIN(false, true, FSM_OP_RESUME, 1, FSM_CONNECTED, FSM_CLEARING);

	/* the guard has to sit above the chain step, or a re-INVITE failure
	 * would run the transfer's own fail arm */
	case_start("PENDING x REINVITE_FAIL: op XFER owns no re-INVITE either",
		   FSM_PENDING);
	pend_xfer_set(1);
	acts_reset();
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_XFER, 1, FSM_SLOT_NONE, FSM_OFFHOOK_DIAL, FSM_PENDING);
	CHAIN(false, true, FSM_OP_RESUME, 1, FSM_CONNECTED, FSM_CLEARING);

	/*
	 * The XFER row of the far-end clear in PENDING. It is also where an
	 * attended transferee's early BYE lands: call.h voids the pending
	 * outcome when the leg dies first, so the death arrives through the
	 * ordinary far-end path.
	 */
	case_start("PENDING x CALL_REMOTE_BYE: op XFER lost the leg it referred",
		   FSM_PENDING);
	pend_xfer_set(1);
	acts_reset();
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	w_tone(HAL_TONE_NONE);
	w_arm(TI_CLEARING, 2000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_QUIET);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("PENDING x CALL_FAILED: op XFER lost the leg it referred",
		   FSM_PENDING);
	pend_xfer_set(1);
	acts_reset();
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_QUIET));
	w_tone(HAL_TONE_NONE);
	w_arm(TI_CLEARING, 2000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_QUIET);

	/* rule 4 stands for XFER too */
	case_start("PENDING x RELEASE: on hook tears the transfer down", FSM_PENDING);
	pend_xfer_set(1);
	acts_reset();
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(1);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);
}

/*
 * The per slot direction flag and the exit discipline of the armed transfer.
 * Both are properties across cells, so these cases walk the machine instead
 * of setting it up.
 */
static void test_xfer_direction(void)
{
	case_start("direction: the IDLE acceptance marks the slot incoming", FSM_IDLE);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	CHECK(f.slot_inbound[1]);
	CHECK(!f.slot_inbound[0]);

	case_start("direction: the CONNECTED acceptance marks the waiting slot "
		   "incoming", FSM_CONNECTED);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	STATE(FSM_CW_OFFERED);
	CHECK(f.slot_inbound[1]);

	case_start("direction: the HOLDING acceptance marks it too", FSM_HOLDING);
	ev_push(FSM_EV_CALL_INCOMING, 0);
	STATE(FSM_RINGING);
	CHECK(f.slot_inbound[0]);

	case_start("direction: call_dial clears the slot it takes", FSM_OFFHOOK_DIAL);
	f.slot_inbound[0] = true;
	dp_dial();
	ev_push(FSM_EV_DIGIT, '5');
	STATE(FSM_CALLING);
	CHECK(!f.slot_inbound[0]);

	/*
	 * The flag travels with the slot, not with the role. After a swap the
	 * incoming leg is the active one, so the direction rule no longer
	 * holds and the on-hook parks instead of transferring.
	 */
	case_start("direction: a swap moves the roles and the flags stay with the "
		   "slots", FSM_TWO_CALLS);
	f.cfg.xfer_on_hook = true;
	f.slot_inbound[1] = true;
	ev_push(FSM_EV_FLASH, 0);
	ev_push(FSM_EV_CALL_REINVITE_OK, 0);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	STATE(FSM_TWO_CALLS);
	ROLES(1, 0, true);
	CHECK(f.slot_inbound[1]);
	CHECK(!f.slot_inbound[0]);
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(1);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_bell_start(0);
	ACTS();
	STATE(FSM_HOLDING);

	/*
	 * The flag survives a park. The subscriber answers a call, holds it,
	 * goes on hook, picks the line up again, calls a second party and then
	 * hangs up: the held leg is still the incoming one and the transfer
	 * fires.
	 */
	case_start("direction: an inbound leg parked in HOLDING keeps its flag",
		   FSM_IDLE);
	f.cfg.xfer_on_hook = true;
	ev_push(FSM_EV_CALL_INCOMING, 1);
	STATE(FSM_RINGING);
	ev_push(FSM_EV_SEIZE, 0);
	STATE(FSM_CONNECTED);
	ev_push(FSM_EV_FLASH, 0);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	STATE(FSM_HOLD_DIAL);
	ev_push(FSM_EV_RELEASE, 0);
	STATE(FSM_HOLDING);
	ev_push(FSM_EV_SEIZE, 0);
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	STATE(FSM_CONNECTED);
	ev_push(FSM_EV_FLASH, 0);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	STATE(FSM_HOLD_DIAL);
	dp_dial();
	ev_push(FSM_EV_DIGIT, '5');
	STATE(FSM_CALLING);
	ev_push(FSM_EV_CALL_ANSWERED, 0);
	STATE(FSM_TWO_CALLS);
	ROLES(0, 1, true);
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_xfer_consult(1, 0);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	/*
	 * The arm has no disarm gesture, and it needs none: the consult call
	 * failing takes the machine to a state that does not keep the arm, so
	 * the subscriber who reconnects and calls somebody else on hooks into
	 * the ordinary park.
	 */
	case_start("xfer_armed: a failed consult call drops the arm at the settle",
		   FSM_XFER_COLLECT);
	f.xfer_act = FEATURE_ACT_XFER_CONSULT;
	dp_dial();
	ev_push(FSM_EV_DIGIT, '5');
	STATE(FSM_CALLING);
	CHECK(f.xfer_armed);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_CONGESTION));
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	CHECK(!f.xfer_armed);
	ev_push(FSM_EV_FLASH, 0);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	STATE(FSM_CONNECTED);
	ev_push(FSM_EV_FLASH, 0);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	STATE(FSM_HOLD_DIAL);
	dp_dial();
	ev_push(FSM_EV_DIGIT, '5');
	ev_push(FSM_EV_CALL_ANSWERED, 0);
	STATE(FSM_TWO_CALLS);
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);

	/* the swap inside TWO_CALLS keeps the arm: the REFER's leg is not the
	 * one the subscriber is talking to (RFC 3891) */
	case_start("xfer_armed: the swap of TWO_CALLS keeps the arm", FSM_TWO_CALLS);
	f.xfer_armed = true;
	ev_push(FSM_EV_FLASH, 0);
	STATE(FSM_PENDING);
	CHECK(f.xfer_armed);
	ev_push(FSM_EV_CALL_REINVITE_OK, 0);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	STATE(FSM_TWO_CALLS);
	CHECK(f.xfer_armed);

	/*
	 * The second clearing point. The settle rides an event and the forced
	 * out of service entry is not one, so that entry drops the arm itself.
	 *
	 * The assertion on the state is the pin, and the walk behind it is not:
	 * the line comes back through IDLE and OFFHOOK_DIAL, and the settle
	 * clears the arm at the first of those events whatever this entry did.
	 * So the walk holds with the clear and without it, and it is here to
	 * state the end-to-end property rather than to pin the mechanism.
	 */
	case_start("xfer_armed: the forced out of service entry drops the arm too",
		   FSM_XFER_COLLECT);
	f.xfer_act = FEATURE_ACT_XFER_CONSULT;
	dp_dial();
	ev_push(FSM_EV_DIGIT, '5');
	STATE(FSM_CALLING);
	CHECK(f.xfer_armed);
	fsm_oos_force(&f);
	STATE(FSM_OOS);
	CHECK(!f.xfer_armed);
	fsm_oos_clear(&f);
	STATE(FSM_IDLE);
	ev_push(FSM_EV_SEIZE, 0);
	dp_dial();
	ev_push(FSM_EV_DIGIT, '5');
	STATE(FSM_CALLING);
	ev_push(FSM_EV_CALL_ANSWERED, 0);
	STATE(FSM_CONNECTED);
	ev_push(FSM_EV_FLASH, 0);
	ev_push(FSM_EV_CALL_REINVITE_OK, 0);
	STATE(FSM_HOLD_DIAL);
	dp_dial();
	ev_push(FSM_EV_DIGIT, '5');
	ev_push(FSM_EV_CALL_ANSWERED, 1);
	STATE(FSM_TWO_CALLS);
	ROLES(1, 0, true);
	CHECK(!f.xfer_armed);
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(1);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_bell_start(0);
	ACTS();
	STATE(FSM_HOLDING);
}

/* -------------------------------------------------------------- HOLDING */

static void test_holding(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_RELEASE, 0 }, { FSM_EV_FLASH, 0 }, { FSM_EV_DIGIT, '5' },
		{ FSM_EV_CALL_PROCEEDING, 0 }, { FSM_EV_CALL_ANSWERED, 0 },
		{ FSM_EV_CALL_REMOTE_CANCEL, 0 }, { FSM_EV_CALL_REINVITE_OK, 0 },
		{ FSM_EV_CALL_REINVITE_FAIL, 0 }, { FSM_EV_CW_NET, 0 },
		{ FSM_EV_CW_NET, 2 }, { FSM_EV_T_START_DIGIT, 0 },
		{ FSM_EV_T_INTERDIGIT, 0 }, { FSM_EV_T_WARMLINE, 0 },
		{ FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("HOLDING x SEIZE: retrieve the held call", FSM_HOLDING);
	ev_push(FSM_EV_SEIZE, 0);
	w_bell_stop();
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);

	/* the pickup draws a refusal, so the next on-hook ends the held call
	 * instead of ringing the subscriber back into the same refusal */
	case_start("HOLDING x SEIZE: a refused resume, then the on-hook hangs up",
		   FSM_HOLDING);
	ev_push(FSM_EV_SEIZE, 0);
	ev_push(FSM_EV_CALL_REINVITE_FAIL, 1);
	STATE(FSM_CLEARING);
	ROLES(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_CLEARING);
	w_hangup(1);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	case_start("HOLDING x CALL_INCOMING: the recall ring stops, 180, ring the "
		   "new call, keep the held one", FSM_HOLDING);
	ev_push(FSM_EV_CALL_INCOMING, 0);
	w_bell_stop();
	w_ring180(0);
	w_bell_start(0);
	ACTS();
	STATE(FSM_RINGING);
	ROLES(0, 1, true);

	case_start("HOLDING x CALL_INCOMING: the caller gives up and the recall "
		   "ring comes back", FSM_HOLDING);
	ev_push(FSM_EV_CALL_INCOMING, 0);
	STATE(FSM_RINGING);
	ev_push(FSM_EV_CALL_REMOTE_CANCEL, 0);
	w_bell_stop();
	w_tone(HAL_TONE_NONE);
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("HOLDING x CALL_FAILED: the held leg died", FSM_HOLDING);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_QUIET));
	w_bell_stop();
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_IDLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("HOLDING x CALL_FAILED: a foreign slot is ignored", FSM_HOLDING);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(0, CALL_FAIL_QUIET));
	ACTS();
	STATE(FSM_HOLDING);

	case_start("HOLDING x CALL_REMOTE_BYE: the held leg cleared", FSM_HOLDING);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	w_bell_stop();
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_IDLE);

	IGNORES(FSM_HOLDING, ignores);
}

/* ------------------------------------------------------------- CLEARING */

static void test_clearing(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 }, { FSM_EV_DIGIT, '5' },
		{ FSM_EV_CALL_PROCEEDING, 0 }, { FSM_EV_CALL_ANSWERED, 0 },
		{ FSM_EV_CALL_REMOTE_CANCEL, 0 }, { FSM_EV_CALL_REINVITE_OK, 0 },
		{ FSM_EV_CALL_REINVITE_FAIL, 0 }, { FSM_EV_CW_NET, 0 },
		{ FSM_EV_CW_NET, 2 }, { FSM_EV_T_START_DIGIT, 0 },
		{ FSM_EV_T_INTERDIGIT, 0 }, { FSM_EV_T_WARMLINE, 0 },
		{ FSM_EV_T_CW, 0 }, { FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("CLEARING x RELEASE: back to idle", FSM_CLEARING);
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_CLEARING);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	/* one rule for every class: the busy and quiet classes ring back too */
	case_start("CLEARING x RELEASE: a held leg rings back from busy", FSM_CLEARING);
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_CLEARING);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("CLEARING x RELEASE: a held leg rings back from fast busy",
		   FSM_CLEARING);
	f.clearing = FSM_CLR_FASTBUSY;
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_CLEARING);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("CLEARING x RELEASE: a held leg rings back from quiet", FSM_CLEARING);
	f.clearing = FSM_CLR_QUIET;
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_CLEARING);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("CLEARING x RELEASE: a held leg rings back from unobtainable",
		   FSM_CLEARING);
	f.clearing = FSM_CLR_UNOBTAINABLE;
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_CLEARING);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	/* a recall ring would only lead to the same refusal, so the on-hook is
	 * the subscriber's one way to end the held call */
	case_start("CLEARING x RELEASE: a held leg whose resume was refused is hung up",
		   FSM_CLEARING);
	roles_set(FSM_SLOT_NONE, 1, true);
	f.held_refused = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_CLEARING);
	w_hangup(1);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("CLEARING x FLASH: a resume that succeeds clears the refusal",
		   FSM_CLEARING);
	roles_set(FSM_SLOT_NONE, 1, true);
	f.held_refused = true;
	ev_push(FSM_EV_FLASH, 0);
	ev_push(FSM_EV_CALL_REINVITE_OK, 1);
	STATE(FSM_CONNECTED);
	CHECK(!f.held_refused);

	case_start("CLEARING x FLASH: no held leg, the dial tone shortcut",
		   FSM_CLEARING);
	ev_push(FSM_EV_FLASH, 0);
	w_cancel(TI_CLEARING);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);

	case_start("CLEARING x FLASH: a held leg is reconnected", FSM_CLEARING);
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_FLASH, 0);
	w_cancel(TI_CLEARING);
	w_tone(HAL_TONE_NONE);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);

	case_start("CLEARING x FLASH: the quiet class grants the shortcut too",
		   FSM_CLEARING);
	f.clearing = FSM_CLR_QUIET;
	ev_push(FSM_EV_FLASH, 0);
	w_cancel(TI_CLEARING);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);

	/* the IDLE seize found no dial readiness: the flash emits no action at
	 * all, and the reorder timer of that seize runs on */
	case_start("CLEARING x FLASH: no dial readiness keeps the fast busy chain",
		   FSM_IDLE);
	fsm_dial_ready_set(&f, false);
	ev_push(FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	ev_push(FSM_EV_FLASH, 0);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	CHECK(f.t_clearing.armed);

	case_start("CLEARING x FLASH: a held leg is reconnected with no dial readiness",
		   FSM_CLEARING);
	roles_set(FSM_SLOT_NONE, 1, true);
	fsm_dial_ready_set(&f, false);
	ev_push(FSM_EV_FLASH, 0);
	w_cancel(TI_CLEARING);
	w_tone(HAL_TONE_NONE);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);

	case_start("CLEARING x CALL_INCOMING: 486", FSM_CLEARING);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_reject(1, 486);
	ACTS();
	STATE(FSM_CLEARING);

	case_start("CLEARING x CALL_FAILED: the held leg died, the tone plays on",
		   FSM_CLEARING);
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_QUIET));
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_BUSY);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("CLEARING x CALL_REMOTE_BYE: the held leg cleared", FSM_CLEARING);
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	ACTS();
	STATE(FSM_CLEARING);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("CLEARING x T_CLEARING: busy chains to the reorder step",
		   FSM_CLEARING);
	armed_drop(&f.t_clearing);
	ev_push(FSM_EV_T_CLEARING, 0);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);

	case_start("CLEARING x T_CLEARING: quiet chains to the reorder step",
		   FSM_CLEARING);
	f.clearing = FSM_CLR_QUIET;
	armed_drop(&f.t_clearing);
	ev_push(FSM_EV_T_CLEARING, 0);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	CLASS(FSM_CLR_FASTBUSY);

	case_start("CLEARING x T_CLEARING: stale in the fast busy class", FSM_CLEARING);
	f.clearing = FSM_CLR_FASTBUSY;
	armed_drop(&f.t_clearing);
	ev_push(FSM_EV_T_CLEARING, 0);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);

	case_start("CLEARING x T_CLEARING: stale in the unobtainable class",
		   FSM_CLEARING);
	f.clearing = FSM_CLR_UNOBTAINABLE;
	armed_drop(&f.t_clearing);
	ev_push(FSM_EV_T_CLEARING, 0);
	ACTS();
	CLASS(FSM_CLR_UNOBTAINABLE);

	case_start("CLEARING x T_ROH: fast busy ends in the howler", FSM_CLEARING);
	f.clearing = FSM_CLR_FASTBUSY;
	armed_drop(&f.t_clearing);
	ev_push(FSM_EV_T_ROH, 0);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_HOWLER);
	w_arm(TI_CLEARING, 60000);
	ACTS();
	STATE(FSM_ROH);

	case_start("CLEARING x T_ROH: unobtainable ends in the howler", FSM_CLEARING);
	f.clearing = FSM_CLR_UNOBTAINABLE;
	armed_drop(&f.t_clearing);
	ev_push(FSM_EV_T_ROH, 0);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_HOWLER);
	w_arm(TI_CLEARING, 60000);
	ACTS();
	STATE(FSM_ROH);

	case_start("CLEARING x T_ROH: a zero howler timer is not armed", FSM_CLEARING);
	f.clearing = FSM_CLR_FASTBUSY;
	f.cfg.roh_ms = 0;
	armed_drop(&f.t_clearing);
	ev_push(FSM_EV_T_ROH, 0);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_HOWLER);
	ACTS();
	STATE(FSM_ROH);

	case_start("CLEARING x T_ROH: stale in the busy class", FSM_CLEARING);
	armed_drop(&f.t_clearing);
	ev_push(FSM_EV_T_ROH, 0);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_BUSY);

	case_start("CLEARING x T_ROH: stale in the quiet class", FSM_CLEARING);
	f.clearing = FSM_CLR_QUIET;
	armed_drop(&f.t_clearing);
	ev_push(FSM_EV_T_ROH, 0);
	ACTS();
	CLASS(FSM_CLR_QUIET);

	IGNORES(FSM_CLEARING, ignores);
}

/* ------------------------------------------------------------------ ROH */

static void test_roh(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 }, { FSM_EV_DIGIT, '5' },
		{ FSM_EV_CALL_PROCEEDING, 0 }, { FSM_EV_CALL_ANSWERED, 0 },
		{ FSM_EV_CALL_REMOTE_CANCEL, 0 }, { FSM_EV_CALL_REINVITE_OK, 0 },
		{ FSM_EV_CALL_REINVITE_FAIL, 0 }, { FSM_EV_CW_NET, 0 },
		{ FSM_EV_CW_NET, 2 }, { FSM_EV_T_START_DIGIT, 0 },
		{ FSM_EV_T_INTERDIGIT, 0 }, { FSM_EV_T_WARMLINE, 0 },
		{ FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("ROH x RELEASE: the handset went back on hook", FSM_ROH);
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_CLEARING);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	case_start("ROH x RELEASE: a held leg rings back", FSM_ROH);
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	w_cancel(TI_CLEARING);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("ROH x FLASH: no held leg, the howler runs on", FSM_ROH);
	ev_push(FSM_EV_FLASH, 0);
	ACTS();
	STATE(FSM_ROH);
	CHECK(f.t_clearing.armed);

	case_start("ROH x FLASH: a held leg is resumed", FSM_ROH);
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_FLASH, 0);
	w_cancel(TI_CLEARING);
	w_tone(HAL_TONE_NONE);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);

	case_start("ROH x CALL_INCOMING: 486 instead of a dangling INVITE", FSM_ROH);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_reject(1, 486);
	ACTS();
	STATE(FSM_ROH);

	case_start("ROH x CALL_FAILED: the held leg died", FSM_ROH);
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_QUIET));
	ACTS();
	STATE(FSM_ROH);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("ROH x CALL_REMOTE_BYE: the held leg cleared", FSM_ROH);
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	ACTS();
	STATE(FSM_ROH);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("ROH x T_ROH: the howler window ends in silence", FSM_ROH);
	armed_drop(&f.t_clearing);
	ev_push(FSM_EV_T_ROH, 0);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_SILENT);

	IGNORES(FSM_ROH, ignores);
}

/* ----------------------------------------------------------------- SILENT */

static void test_silent(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_SEIZE, 0 }, { FSM_EV_DIGIT, '5' },
		{ FSM_EV_CALL_PROCEEDING, 0 }, { FSM_EV_CALL_ANSWERED, 0 },
		{ FSM_EV_CALL_REMOTE_CANCEL, 0 }, { FSM_EV_CALL_REINVITE_OK, 0 },
		{ FSM_EV_CALL_REINVITE_FAIL, 0 }, { FSM_EV_CW_NET, 0 },
		{ FSM_EV_CW_NET, 2 }, { FSM_EV_T_START_DIGIT, 0 },
		{ FSM_EV_T_INTERDIGIT, 0 }, { FSM_EV_T_WARMLINE, 0 },
		{ FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("SILENT x RELEASE: the handset went back on hook", FSM_SILENT);
	ev_push(FSM_EV_RELEASE, 0);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	case_start("SILENT x RELEASE: a held leg rings back", FSM_SILENT);
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_RELEASE, 0);
	w_bell_stop();
	w_bell_start(1);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	case_start("SILENT x RELEASE: a held leg whose resume was refused is hung up",
		   FSM_SILENT);
	roles_set(FSM_SLOT_NONE, 1, true);
	f.held_refused = true;
	ev_push(FSM_EV_RELEASE, 0);
	w_hangup(1);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);

	case_start("SILENT x FLASH: no held leg, the silence runs on", FSM_SILENT);
	ev_push(FSM_EV_FLASH, 0);
	ACTS();
	STATE(FSM_SILENT);

	case_start("SILENT x FLASH: a held leg is resumed", FSM_SILENT);
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_FLASH, 0);
	w_tone(HAL_TONE_NONE);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);

	case_start("SILENT x CALL_INCOMING: 486 instead of a dangling INVITE",
		   FSM_SILENT);
	ev_push(FSM_EV_CALL_INCOMING, 1);
	w_reject(1, 486);
	ACTS();
	STATE(FSM_SILENT);

	case_start("SILENT x CALL_FAILED: the held leg died", FSM_SILENT);
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_CALL_FAILED, fail_arg(1, CALL_FAIL_QUIET));
	ACTS();
	STATE(FSM_SILENT);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	case_start("SILENT x CALL_REMOTE_BYE: the held leg cleared", FSM_SILENT);
	roles_set(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_CALL_REMOTE_BYE, 1);
	ACTS();
	STATE(FSM_SILENT);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	IGNORES(FSM_SILENT, ignores);
}

/* ------------------------------------------------------------------ OOS */

static void test_oos(void)
{
	static const struct ign ignores[] = {
		{ FSM_EV_FLASH, 0 }, { FSM_EV_DIGIT, '5' }, { FSM_EV_CALL_INCOMING, 0 },
		{ FSM_EV_CALL_PROCEEDING, 0 }, { FSM_EV_CALL_ANSWERED, 0 },
		{ FSM_EV_CALL_FAILED, 0 }, { FSM_EV_CALL_REMOTE_BYE, 0 },
		{ FSM_EV_CALL_REMOTE_CANCEL, 0 }, { FSM_EV_CALL_REINVITE_OK, 0 },
		{ FSM_EV_CALL_REINVITE_FAIL, 0 }, { FSM_EV_CW_NET, 0 },
		{ FSM_EV_CW_NET, 2 }, { FSM_EV_T_START_DIGIT, 0 },
		{ FSM_EV_T_INTERDIGIT, 0 }, { FSM_EV_T_WARMLINE, 0 },
		{ FSM_EV_T_CW, 0 }, { FSM_EV_T_CLEARING, 0 }, { FSM_EV_T_ROH, 0 },
		{ FSM_EV_T_RING_NOANSWER, 0 },
		{ FSM_EV_CALL_XFER_OK, 0 }, { FSM_EV_CALL_XFER_FAIL, 0 },
	};

	case_start("OOS x SEIZE: reorder with no timer", FSM_OOS);
	ev_push(FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_REORDER);
	ACTS();
	STATE(FSM_OOS);
	CHECK(!f.t_clearing.armed);

	case_start("OOS x RELEASE: stop the tone", FSM_OOS);
	ev_push(FSM_EV_RELEASE, 0);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_OOS);

	IGNORES(FSM_OOS, ignores);
}

/* ------------------------------------- outside the matrix: the real timers */

static void test_timer_paths(void)
{
	tu_case("timer path: the start digit window expires into fast busy");
	fsm_reset();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	CHECK_UINT(f.state, FSM_OFFHOOK_DIAL);
	CHECK(f.t_digit.armed);
	acts_reset();
	fake_advance(15999);
	ACTS();
	cover_mark(FSM_OFFHOOK_DIAL, FSM_EV_T_START_DIGIT);
	acts_reset();
	fake_advance(1);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);

	tu_case("timer path: a seize with no dial target reaches the howler");
	fsm_reset();
	fsm_dial_ready_set(&f, false);
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	CHECK_UINT(f.state, FSM_CLEARING);
	acts_reset();
	fake_advance(39999);
	ACTS();
	acts_reset();
	fake_advance(1);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_HOWLER);
	w_arm(TI_CLEARING, 60000);
	ACTS();
	STATE(FSM_ROH);

	tu_case("timer path: the same object fires as the warmline instead");
	cfg.warmline = true;
	snprintf(cfg.warmline_number, sizeof(cfg.warmline_number), "18005551212");
	fsm_reset();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	CHECK_UINT(f.state, FSM_OFFHOOK_DIAL);
	acts_reset();
	fake_advance(7999);
	ACTS();
	cover_mark(FSM_OFFHOOK_DIAL, FSM_EV_T_WARMLINE);
	acts_reset();
	fake_advance(1);
	w_tone(HAL_TONE_NONE);
	w_dial(0, "18005551212");
	ACTS();
	STATE(FSM_CALLING);
	cfg_default();

	/*
	 * The hotline is not a timer. The entry dials before any window
	 * exists, so no clock pass can move it and no digit can reach it. A
	 * zero length arm would pass the first case and fail the second, which
	 * is the whole reason the entry owns this.
	 */
	tu_case("hotline: a zero warmline window dials at the entry, not on a timer");
	cfg.warmline = true;
	cfg.warmline_ms = 0;
	snprintf(cfg.warmline_number, sizeof(cfg.warmline_number), "18005551212");
	fsm_reset();
	acts_reset();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_NONE);
	w_dial(0, "18005551212");
	ACTS();
	STATE(FSM_CALLING);
	CHECK(!f.t_digit.armed);
	cfg_default();

	tu_case("hotline: a digit cannot reach the call it already started");
	cfg.warmline = true;
	cfg.warmline_ms = 0;
	snprintf(cfg.warmline_number, sizeof(cfg.warmline_number), "18005551212");
	fsm_reset();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	STATE(FSM_CALLING);
	acts_reset();
	fsm_event_push(&f, FSM_EV_DIGIT, '5');
	ACTS();
	STATE(FSM_CALLING);
	cfg_default();

	/* the guard that keeps an unusable warmline on the fast busy branch of
	 * the OFFHOOK_DIAL T_WARMLINE cell instead of dialling nothing */
	tu_case("hotline: a zero window with no number still arms and still reorders");
	cfg.warmline = true;
	cfg.warmline_ms = 0;
	fsm_reset();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	STATE(FSM_OFFHOOK_DIAL);
	CHECK(f.t_digit.armed);
	acts_reset();
	fake_advance(0);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	cfg_default();

	/*
	 * The timer path of "a digit cancels the warmline". Every DIGIT branch
	 * of OFFHOOK_DIAL cancels T_WARMLINE. This case proves that the cancel
	 * reached the object: it runs the clock well past the window the seize
	 * armed, and finds no dial behind it.
	 */
	tu_case("timer path: a digit cancels the warmline window it interrupted");
	cfg.warmline = true;
	snprintf(cfg.warmline_number, sizeof(cfg.warmline_number), "18005551212");
	fsm_reset();
	dp_long();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	STATE(FSM_OFFHOOK_DIAL);
	CHECK(f.t_digit.armed);
	fake_advance(4000);
	fsm_event_push(&f, FSM_EV_DIGIT, '5');
	STATE(FSM_COLLECTING);
	acts_reset();
	fake_advance(5000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("5");
	cfg_default();

	tu_case("timer path: the interdigit timer dials the collected number");
	fsm_reset();
	dp_long();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	fsm_event_push(&f, FSM_EV_DIGIT, '5');
	CHECK_UINT(f.state, FSM_COLLECTING);
	dp_short();
	cover_mark(FSM_COLLECTING, FSM_EV_T_INTERDIGIT);
	acts_reset();
	fake_advance(16000);
	w_dial(0, "5");
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CALLING);

	tu_case("timer path: the long interdigit timer clears fast busy");
	fsm_reset();
	dp_long();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	fsm_event_push(&f, FSM_EV_DIGIT, '5');
	cover_mark(FSM_COLLECTING, FSM_EV_T_INTERDIGIT);
	acts_reset();
	fake_advance(16000);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);

	tu_case("timer path: the whole busy, reorder, howler and silence chain");
	cover_mark(FSM_CLEARING, FSM_EV_T_CLEARING);
	cover_mark(FSM_CLEARING, FSM_EV_T_ROH);
	cover_mark(FSM_ROH, FSM_EV_T_ROH);
	st_set(FSM_CLEARING);
	acts_reset();
	fake_advance(40000);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	acts_reset();
	fake_advance(40000);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_HOWLER);
	w_arm(TI_CLEARING, 60000);
	ACTS();
	STATE(FSM_ROH);
	acts_reset();
	fake_advance(60000);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_SILENT);

	tu_case("timer path: the recall ring never restarts on the old deadlines");
	st_set(FSM_TWO_CALLS);
	ev_push(FSM_EV_RELEASE, 0);
	STATE(FSM_HOLDING);
	acts_reset();
	fake_advance(10000);
	ACTS();
	fake_advance(30000);
	ACTS();
	fake_advance(40000);
	ACTS();
	STATE(FSM_HOLDING);
	ROLES(FSM_SLOT_NONE, 1, true);

	/* the machine has no post-flash digit menu timer, so nothing fires
	 * before the start digit window. The held leg stays through every step
	 * of the chain until a flash takes it back. The dial plan would dial
	 * the empty buffer, so only the T_START_DIGIT mapping gives the
	 * reorder: a T_INTERDIGIT expiry would dial "" */
	tu_case("timer path: an abandoned flash runs the off-hook cycle with the "
		"held leg kept");
	st_set(FSM_HOLD_DIAL);
	dp_dial();
	acts_reset();
	fake_advance(15999);
	ACTS();
	STATE(FSM_HOLD_DIAL);
	cover_mark(FSM_HOLD_DIAL, FSM_EV_T_START_DIGIT);
	acts_reset();
	fake_advance(1);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);
	ROLES(FSM_SLOT_NONE, 1, true);
	acts_reset();
	fake_advance(40000);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_HOWLER);
	w_arm(TI_CLEARING, 60000);
	ACTS();
	STATE(FSM_ROH);
	ROLES(FSM_SLOT_NONE, 1, true);
	acts_reset();
	fake_advance(60000);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_SILENT);
	ROLES(FSM_SLOT_NONE, 1, true);
	ev_push(FSM_EV_FLASH, 0);
	w_tone(HAL_TONE_NONE);
	w_resume(1);
	ACTS();
	STATE(FSM_PENDING);
	PEND(FSM_OP_RESUME, 1, FSM_SLOT_NONE, FSM_CONNECTED, FSM_CLEARING);

	/* the digit comes a second in, so the entry deadline passes silently
	 * and only the window the digit armed fires */
	tu_case("timer path: a digit in HOLD_DIAL turns the object into interdigit");
	cover_mark(FSM_HOLD_DIAL, FSM_EV_T_INTERDIGIT);
	st_set(FSM_HOLD_DIAL);
	dp_long();
	fake_advance(1000);
	fsm_event_push(&f, FSM_EV_DIGIT, '5');
	CHECK_UINT(f.state, FSM_HOLD_DIAL);
	acts_reset();
	fake_advance(15000);
	ACTS();
	STATE(FSM_HOLD_DIAL);
	acts_reset();
	fake_advance(1000);
	w_tone(HAL_TONE_NONE);
	w_tone(HAL_TONE_REORDER);
	w_arm(TI_CLEARING, 40000);
	ACTS();
	STATE(FSM_CLEARING);
	CLASS(FSM_CLR_FASTBUSY);

	tu_case("timer path: the call waiting offer times out at 30 s");
	cover_mark(FSM_CW_OFFERED, FSM_EV_T_CW);
	st_set(FSM_CW_OFFERED);
	acts_reset();
	fake_advance(30000);
	w_reject(1, 480);
	w_cw(1, false);
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CONNECTED);

	tu_case("timer path: a cancelled timer never fires");
	st_set(FSM_OFFHOOK_DIAL);
	fsm_event_push(&f, FSM_EV_RELEASE, 0);
	acts_reset();
	fake_advance(600000);
	ACTS();
	STATE(FSM_IDLE);
	CHECK_UINT(fake_timer_dropped, 0);
}

/* -------------------------- outside the matrix: the other entry points */

static void test_extras(void)
{
	unsigned int i;

	tu_case("fsm_init: idle, no leg, no timer, no action");
	fsm_reset();
	CHECK_UINT(f.state, FSM_IDLE);
	CHECK_UINT(f.line, 0);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);
	CHECK_UINT(f.ndigits, 0);
	CHECK(!f.t_digit.armed && !f.t_cw.armed && !f.t_clearing.armed);
	ACTS();

	tu_case("fsm_init: SEIZE can be the very first event the machine sees");
	fsm_reset();
	acts_reset();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);

	tu_case("fsm_cfg_update: accepted in IDLE");
	fsm_reset();
	cfg.start_digit_ms = 12000;
	CHECK(fsm_cfg_update(&f, &cfg));
	CHECK_UINT(f.cfg.start_digit_ms, 12000);
	ACTS();
	acts_reset();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 12000);
	ACTS();
	cfg_default();

	tu_case("fsm_cfg_update: accepted in OOS");
	fsm_reset();
	f.state = FSM_OOS;
	cfg.roh_ms = 45000;
	CHECK(fsm_cfg_update(&f, &cfg));
	CHECK_UINT(f.cfg.roh_ms, 45000);
	cfg_default();

	/*
	 * The narrow entry of message-waiting. A NOTIFY arrives when the switch
	 * says so, including in the middle of a call. So this one field
	 * bypasses the at-rest view exactly as call waiting does. It changes no
	 * state and emits no action; the next OFFHOOK_DIAL entry is where it is
	 * heard.
	 */
	tu_case("fsm_mwi_set: it moves the flag off a rest state and emits nothing");
	fsm_reset();
	f.state = FSM_CONNECTED;
	acts_reset();
	fsm_mwi_set(&f, true);
	ACTS();
	STATE(FSM_CONNECTED);
	CHECK(f.cfg.mwi);

	tu_case("fsm_mwi_set: the next seizure hears it");
	fsm_reset();
	fsm_mwi_set(&f, true);
	acts_reset();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_STUTTER);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);

	tu_case("fsm_mwi_set: clearing it returns the plain dial tone");
	fsm_mwi_set(&f, false);
	st_set(FSM_IDLE);
	fsm_mwi_set(&f, false);
	acts_reset();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();

	tu_case("fsm_cfg_update: refused anywhere else");
	fsm_reset();
	cfg.start_digit_ms = 9000;
	f.state = FSM_CONNECTED;
	CHECK(!fsm_cfg_update(&f, &cfg));
	CHECK_UINT(f.cfg.start_digit_ms, 16000);
	f.state = FSM_OFFHOOK_DIAL;
	CHECK(!fsm_cfg_update(&f, &cfg));
	f.state = FSM_PENDING;
	CHECK(!fsm_cfg_update(&f, &cfg));
	f.state = FSM_RINGING;
	CHECK(!fsm_cfg_update(&f, &cfg));
	CHECK_UINT(f.cfg.start_digit_ms, 16000);
	cfg_default();

	/*
	 * line/line.c owns the OOS entry and the exit, and it drives both
	 * through the two calls, never by reaching into the struct.
	 */
	tu_case("fsm_oos_force: from CONNECTED the leg goes and the line falls silent");
	st_set(FSM_CONNECTED);
	acts_reset();
	fsm_oos_force(&f);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_OOS);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);

	tu_case("fsm_oos_force: from CW_OFFERED the alert stops before the hangups");
	st_set(FSM_CW_OFFERED);
	acts_reset();
	fsm_oos_force(&f);
	w_cw(1, false);
	w_hangup(0);
	w_hangup(1);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_cancel(TI_CW);
	ACTS();
	STATE(FSM_OOS);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);
	CHECK(!f.t_cw.armed);

	tu_case("fsm_oos_force: from PENDING with a swap in flight, both legs go");
	st_set(FSM_PENDING);
	pend_set(FSM_OP_SWAP, 0, 1, FSM_TWO_CALLS, FSM_TWO_CALLS);
	acts_reset();
	fsm_oos_force(&f);
	w_hangup(0);
	w_hangup(1);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_OOS);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);
	PEND(FSM_OP_HOLD, FSM_SLOT_NONE, FSM_SLOT_NONE, FSM_IDLE, FSM_IDLE);
	NOCHAIN();

	tu_case("fsm_oos_force: from PENDING answering a waiting call, the alert stops");
	st_set(FSM_PENDING);
	pend_set(FSM_OP_CW_ANSWER, 0, 1, FSM_TWO_CALLS, FSM_CONNECTED);
	acts_reset();
	fsm_oos_force(&f);
	w_cw(1, false);
	w_hangup(0);
	w_hangup(1);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_OOS);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);
	PEND(FSM_OP_HOLD, FSM_SLOT_NONE, FSM_SLOT_NONE, FSM_IDLE, FSM_IDLE);
	NOCHAIN();

	tu_case("fsm_oos_force: a CW_ANSWER whose waiting leg died raises no alert");
	st_set(FSM_PENDING);
	pend_set(FSM_OP_CW_ANSWER, 0, FSM_SLOT_NONE, FSM_PENDING, FSM_CONNECTED);
	acts_reset();
	fsm_oos_force(&f);
	w_hangup(0);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_OOS);

	tu_case("fsm_oos_force: from HOLDING the held leg goes and the recall ring "
		"stops");
	st_set(FSM_HOLDING);
	acts_reset();
	fsm_oos_force(&f);
	w_hangup(1);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_OOS);

	tu_case("fsm_oos_force: from COLLECTING the timer and the buffer go");
	st_set(FSM_COLLECTING);
	acts_reset();
	fsm_oos_force(&f);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	w_cancel(TI_DIGIT);
	ACTS();
	STATE(FSM_OOS);
	DIGITS("");
	CHECK(!f.t_digit.armed);

	tu_case("fsm_oos_force: the OOS row runs after the force");
	acts_reset();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_REORDER);
	ACTS();
	STATE(FSM_OOS);

	tu_case("fsm_oos_force: forcing again is a no-op");
	acts_reset();
	fsm_oos_force(&f);
	ACTS();
	STATE(FSM_OOS);

	tu_case("fsm_oos_clear: back to IDLE with the idle entry actions");
	acts_reset();
	fsm_oos_clear(&f);
	w_tone(HAL_TONE_NONE);
	w_bell_stop();
	ACTS();
	STATE(FSM_IDLE);
	ROLES(FSM_SLOT_NONE, FSM_SLOT_NONE, false);
	acts_reset();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	w_tone(HAL_TONE_DIAL);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_OFFHOOK_DIAL);

	tu_case("fsm_oos_clear: a clear outside OOS is a no-op");
	st_set(FSM_CONNECTED);
	acts_reset();
	fsm_oos_clear(&f);
	ACTS();
	STATE(FSM_CONNECTED);
	ROLES(0, FSM_SLOT_NONE, false);
	fsm_reset();
	acts_reset();
	fsm_oos_clear(&f);
	ACTS();
	STATE(FSM_IDLE);

	tu_case("a full digit buffer refuses to overflow");
	fsm_reset();
	f.state = FSM_COLLECTING;
	digits_set("12345678901234567890123456789012");
	CHECK_UINT(f.ndigits, FSM_DIGITS_MAX);
	dp_long();
	sip_timer_arm(&fk.env, &f.t_digit, 16000);
	acts_reset();
	/* 'A' is a map literal that the buffer above does not already carry */
	fsm_event_push(&f, FSM_EV_DIGIT, 'A');
	/* the digit is refused, so the verdict repeats on the unchanged buffer */
	w_cancel(TI_DIGIT);
	w_arm(TI_DIGIT, 16000);
	ACTS();
	STATE(FSM_COLLECTING);
	DIGITS("12345678901234567890123456789012");
	CHECK_UINT(f.ndigits, FSM_DIGITS_MAX);

	tu_case("a '#' on a full digit buffer dials it");
	fsm_reset();
	f.state = FSM_COLLECTING;
	digits_set("12345678901234567890123456789012");
	CHECK_UINT(f.ndigits, FSM_DIGITS_MAX);
	dp_long();
	sip_timer_arm(&fk.env, &f.t_digit, 16000);
	acts_reset();
	/* the buffer cannot take another character, so the '#' delimits it
	 * without a map probe */
	fsm_event_push(&f, FSM_EV_DIGIT, '#');
	w_cancel(TI_DIGIT);
	w_dial(0, "12345678901234567890123456789012");
	w_tone(HAL_TONE_NONE);
	ACTS();
	STATE(FSM_CALLING);
	DIGITS("12345678901234567890123456789012");
	CHECK(f.digits[FSM_DIGITS_MAX] == 0);
	CHECK(strchr(f.digits, 'A') == NULL);
	CHECK_UINT(dp_seen_len, FSM_DIGITS_MAX);
	CHECK(strcmp(dp_seen, "12345678901234567890123456789012") == 0);

	/*
	 * The two name tables are the one place where a new enum member can be
	 * forgotten silently. A miss is a "?" in a trace, not a build break.
	 * The static assertions and the coverage table do not read the tables.
	 */
	tu_case("names: the tables cover every state and every event");
	for (i = 0; i < FSM_STATES; i++)
		CHECK(strcmp(fsm_state_name((enum fsm_state)i), "?") != 0);
	for (i = 0; i < FSM_EV_COUNT; i++)
		CHECK(strcmp(fsm_ev_name((enum fsm_ev)i), "?") != 0);

	tu_case("names: every state and every event has one");
	CHECK(strcmp(fsm_state_name(FSM_IDLE), "IDLE") == 0);
	CHECK(strcmp(fsm_state_name(FSM_OOS), "OOS") == 0);
	CHECK(strcmp(fsm_state_name((enum fsm_state)99), "?") == 0);
	CHECK(strcmp(fsm_ev_name(FSM_EV_SEIZE), "SEIZE") == 0);
	CHECK(strcmp(fsm_ev_name(FSM_EV_T_RING_NOANSWER),
		     "T_RING_NOANSWER") == 0);
	CHECK(strcmp(fsm_ev_name(FSM_EV_COUNT), "?") == 0);

	tu_case("an out of range event is dropped");
	fsm_reset();
	acts_reset();
	fsm_event_push(&f, FSM_EV_COUNT, 0);
	fsm_event_push(&f, (enum fsm_ev)77, 0);
	ACTS();
	STATE(FSM_IDLE);
}

/* ---------------------------------------------------------- the coverage */

static void test_state_changed(void)
{
	tu_case("state_changed: event reports the transition");
	fsm_reset();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	CHECK_UINT(state_note.count, 1);
	CHECK_UINT(state_note.before, FSM_IDLE);
	CHECK_UINT(state_note.after, FSM_OFFHOOK_DIAL);
	CHECK_UINT(state_note.actions, ngot);

	tu_case("state_changed: ignored event emits no notification");
	fsm_reset();
	fsm_event_push(&f, FSM_EV_RELEASE, 0);
	CHECK_UINT(state_note.count, 0);

	tu_case("state_changed: timer expiry reports the transition");
	fsm_reset();
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	state_note.count = 0;
	fake_advance(f.cfg.start_digit_ms);
	CHECK_UINT(fk.fired, 1);
	CHECK_UINT(state_note.count, 1);
	CHECK_UINT(state_note.before, FSM_OFFHOOK_DIAL);
	CHECK_UINT(state_note.after, FSM_CLEARING);

	tu_case("state_changed: event notification follows the settle calls");
	fsm_reset();
	f.state = FSM_PENDING;
	roles_set(0, 1, false);
	pend_set(FSM_OP_CONF_SPLIT, 1, 0, FSM_TWO_CALLS, FSM_CONF);
	f.conf_on = true;
	f.xfer_armed = true;
	fsm_event_push(&f, FSM_EV_RELEASE, 0);
	CHECK_UINT(state_note.count, 1);
	CHECK_UINT(state_note.before, FSM_PENDING);
	CHECK_UINT(state_note.after, FSM_IDLE);
	CHECK_UINT(state_note.actions, ngot);
	CHECK(!state_note.conf_on);
	CHECK(!state_note.xfer_armed);

	tu_case("state_changed: forced OOS reports the non-idle transition");
	fsm_reset();
	f.state = FSM_CONNECTED;
	roles_set(0, FSM_SLOT_NONE, false);
	fsm_oos_force(&f);
	CHECK_UINT(state_note.count, 1);
	CHECK_UINT(state_note.before, FSM_CONNECTED);
	CHECK_UINT(state_note.after, FSM_OOS);

	tu_case("state_changed: repeated OOS force emits no notification");
	fsm_reset();
	f.state = FSM_OOS;
	fsm_oos_force(&f);
	CHECK_UINT(state_note.count, 0);

	tu_case("state_changed: OOS clear reports the idle transition");
	fsm_reset();
	f.state = FSM_OOS;
	fsm_oos_clear(&f);
	CHECK_UINT(state_note.count, 1);
	CHECK_UINT(state_note.before, FSM_OOS);
	CHECK_UINT(state_note.after, FSM_IDLE);

	tu_case("state_changed: a NULL notification op is safe");
	fsm_reset();
	ops_part = ops;
	ops_part.state_changed = NULL;
	f.ops = &ops_part;
	fsm_event_push(&f, FSM_EV_SEIZE, 0);
	STATE(FSM_OFFHOOK_DIAL);
	fsm_oos_force(&f);
	STATE(FSM_OOS);
	fsm_oos_clear(&f);
	STATE(FSM_IDLE);
	CHECK_UINT(state_note.count, 0);
}

static void test_coverage(void)
{
	unsigned int st, ev, n = 0;

	for (st = 0; st < FSM_STATES; st++) {
		for (ev = 0; ev < FSM_EV_COUNT; ev++) {
			if (cover[st][ev]) {
				n++;
				continue;
			}

			printf("    fail uncovered cell: %s x %s\n",
			       fsm_state_name(st), fsm_ev_name(ev));
		}
	}

	tu_case("matrix coverage: every one of the 418 cells");
	CHECK_UINT(n, FSM_STATES * FSM_EV_COUNT);
	printf("# coverage: %u/%u cells\n", n, FSM_STATES * FSM_EV_COUNT);
}

int main(void)
{
	cfg_default();

	test_idle();
	test_offhook_dial();
	test_collecting();
	test_spre_collect();
	test_calling();
	test_proceeding();
	test_connected();
	test_ringing();
	test_ring_failed();
	test_cw_offered();
	test_hold_dial();
	test_two_calls();
	test_cw_net();
	test_dnd_callfwd();
	test_conf();
	test_conf_join();
	test_conf_split();
	test_conf_dead(FSM_EV_CALL_FAILED);
	test_conf_dead(FSM_EV_CALL_REMOTE_BYE);
	test_conf_second();
	test_pending_basics();
	test_pending_ok();
	test_pending_fail();
	test_pending_dead(FSM_EV_CALL_FAILED);
	test_pending_dead(FSM_EV_CALL_REMOTE_BYE);
	test_pending_cancel();
	test_xfer_collect();
	test_xfer_hold_dial();
	test_xfer_no_held_call();
	test_xfer_on_hook();
	test_xfer_pending();
	test_xfer_direction();
	test_holding();
	test_clearing();
	test_roh();
	test_silent();
	test_oos();
	test_timer_paths();
	test_extras();
	test_state_changed();
	test_coverage();

	return tu_summary("test_fsm");
}
