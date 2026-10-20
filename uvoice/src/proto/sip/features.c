// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Star codes: the per line feature table and the runtime feature state.
 *
 * The table is the contract. One row per consulted uci option, indexed by
 * enum feature_action so the row and the id cannot drift apart. Each row
 * reaches its string through the offset of the loaded field in struct
 * cfg_channel, so the strings live in exactly one place: the config the
 * loader already built.
 *
 * No pointers are optional. Every parameter below is required, which is the
 * fsm.c vtable rule: a half wired caller must crash at bring-up rather than
 * fall silent. An empty code is different and is a matching rule, not a
 * wiring fault: an unset option must never match anything.
 *
 * Purity. libc only, no allocation, no logging. feature_execute() touches
 * the runtime state and nothing else; the confirmation tone, the fast busy
 * and the state transition belong to the caller.
 */
#include <stddef.h>
#include <string.h>

#include "cfg/cfg.h"
#include "proto/sip/features.h"
#include "str.h"

#define CODE_OFF(field)		offsetof(struct cfg_channel, field)

struct feature_entry {
	const char *name;
	size_t code_off;
	bool supported;
	bool takes_arg;
};

_Static_assert(FEATURE_TARGET_MAX == CFG_STR_MAX,
	       "a stored forward target is one loaded uri field wide");

/*
 * The DND and call forward rows have local actions, so their codes run here
 * and never reach the softswitch as ordinary digits. Whether a row holds a
 * code at all is the loader's decision (uci.c opt_code(), the base
 * star_codes_local): an empty row matches nothing and its digits dial
 * through.
 *
 * Two rows return UNSUPPORTED. Forward on unreachable has no local arm, so
 * its two codes, when the config carries them, are recognized and
 * answered with the fast busy rather than dialled through: a subscriber who
 * dials one learns that the service is not there instead of reaching a
 * switch that was never asked for it.
 *
 * The four forward set rows and the three transfer rows take an argument. The
 * rest of the table is exact strings that run on the digit that completes them.
 */
static const struct feature_entry table[FEATURE_ACT_COUNT] = {
	[FEATURE_ACT_NONE] = { "none", 0, false, false },
	[FEATURE_ACT_CW_ENABLE] = {
		"call_waiting_enable_exten", CODE_OFF(cw_enable_code), true, false },
	[FEATURE_ACT_CW_DISABLE] = {
		"call_waiting_disable_exten", CODE_OFF(cw_disable_code), true, false },
	[FEATURE_ACT_CW_QUERY] = {
		"call_waiting_query_exten", CODE_OFF(cw_query_code), true, false },
	[FEATURE_ACT_CID_ONCE_ENABLE] = {
		"cid_single_call_enable_exten",
		CODE_OFF(cid_single_call_enable_exten), true, false },
	[FEATURE_ACT_CID_ONCE_DISABLE] = {
		"cid_single_call_disable_exten",
		CODE_OFF(cid_single_call_disable_exten), true, false },
	[FEATURE_ACT_CID_PERSIST_ENABLE] = {
		"cid_enable_exten", CODE_OFF(cid_enable_exten), true, false },
	[FEATURE_ACT_CID_PERSIST_DISABLE] = {
		"cid_disable_exten", CODE_OFF(cid_disable_exten), true, false },
	[FEATURE_ACT_DND_ON] = {
		"do_not_disturb_on_exten", CODE_OFF(do_not_disturb_on_exten),
		true, false },
	[FEATURE_ACT_DND_OFF] = {
		"do_not_disturb_off_exten", CODE_OFF(do_not_disturb_off_exten),
		true, false },
	[FEATURE_ACT_CFU_SET] = {
		"call_fwd_uncond_exten", CODE_OFF(call_fwd_uncond_exten),
		true, true },
	[FEATURE_ACT_CFU_CLEAR] = {
		"call_fwd_uncond_cancel_exten",
		CODE_OFF(call_fwd_uncond_cancel_exten), true, false },
	[FEATURE_ACT_CFU_QUERY] = {
		"call_fwd_uncond_query_exten",
		CODE_OFF(call_fwd_uncond_query_exten), true, false },
	[FEATURE_ACT_CFB_SET] = {
		"call_fwd_busy_exten", CODE_OFF(call_fwd_busy_exten), true, true },
	[FEATURE_ACT_CFB_CLEAR] = {
		"call_fwd_busy_cancel_exten",
		CODE_OFF(call_fwd_busy_cancel_exten), true, false },
	[FEATURE_ACT_CFB_QUERY] = {
		"call_fwd_busy_query_exten",
		CODE_OFF(call_fwd_busy_query_exten), true, false },
	[FEATURE_ACT_CFNR_SET] = {
		"call_fwd_no_answer_exten", CODE_OFF(call_fwd_no_answer_exten),
		true, true },
	[FEATURE_ACT_CFNR_CLEAR] = {
		"call_fwd_no_answer_cancel_exten",
		CODE_OFF(call_fwd_no_answer_cancel_exten), true, false },
	[FEATURE_ACT_CFNR_QUERY] = {
		"call_fwd_no_answer_query_exten",
		CODE_OFF(call_fwd_no_answer_query_exten), true, false },
	[FEATURE_ACT_CFNRC_SET] = {
		"call_fwd_unreachable_exten",
		CODE_OFF(call_fwd_unreachable_exten), false, false },
	[FEATURE_ACT_CFNRC_CLEAR] = {
		"call_fwd_unreachable_cancel_exten",
		CODE_OFF(call_fwd_unreachable_cancel_exten), false, false },
	[FEATURE_ACT_ASCF_SET] = {
		"call_fwd_all_exten", CODE_OFF(call_fwd_all_exten), true, true },
	[FEATURE_ACT_ASCF_CLEAR] = {
		"call_fwd_all_cancel_exten", CODE_OFF(call_fwd_all_cancel_exten),
		true, false },
	/*
	 * The three transfer rows are MATCH-ONLY: they take a target, and both
	 * execute entry points refuse them, because the transfer itself is the
	 * ops->call_xfer the FSM cell runs and this module knows nothing about
	 * a call leg. They are supported all the same: the action exists, it
	 * just does not live here, and feature_takes_arg() reads the flag.
	 */
	[FEATURE_ACT_XFER_BLIND] = {
		"blind_transfer_exten", CODE_OFF(blind_transfer_exten),
		true, true },
	[FEATURE_ACT_XFER_ATTENDED] = {
		"attended_transfer_exten", CODE_OFF(attended_transfer_exten),
		true, true },
	[FEATURE_ACT_XFER_CONSULT] = {
		"consult_transfer_exten", CODE_OFF(consult_transfer_exten),
		true, true },
};

/*
 * The three transfer rows are the last of the table, so feature_xfer_prefix()
 * can name them as one range. Appending a row behind them would silently widen
 * that range.
 */
_Static_assert(FEATURE_ACT_XFER_CONSULT + 1 == FEATURE_ACT_COUNT,
	       "the three transfer rows end the table");

static bool act_valid(enum feature_action act)
{
	if (act <= FEATURE_ACT_NONE || act >= FEATURE_ACT_COUNT)
		return false;

	return table[act].name != NULL;
}

const char *feature_code_of(const struct cfg_channel *ch, enum feature_action act)
{
	if (!act_valid(act))
		return "";

	return (const char *)ch + table[act].code_off;
}

const char *feature_action_name(enum feature_action act)
{
	if (!act_valid(act))
		return "none";

	return table[act].name;
}

bool feature_action_supported(enum feature_action act)
{
	if (!act_valid(act))
		return false;

	return table[act].supported;
}

/*
 * The seed. call_fwd_local 0 hands the whole service to the network, so no
 * local condition is armed and no target is stored: the line then forwards
 * nothing itself, whatever call_fwd_local_mode says. The do-not-disturb bit
 * has no such selector and is seeded unconditionally.
 */
static void state_seed(struct feature_state *st, const struct cfg_channel *ch)
{
	st->dnd = ch->do_not_disturb_enabled;

	if (!ch->call_fwd_local)
		return;

	st->cf_mask = (uint8_t)(ch->call_fwd_local_mode & FEATURE_CF_MASK_ALL);
	str_copy(st->cf_target[FEATURE_CF_UNCOND], FEATURE_TARGET_MAX,
		 ch->call_fwd_uncond_uri);
	str_copy(st->cf_target[FEATURE_CF_BUSY], FEATURE_TARGET_MAX,
		 ch->call_fwd_busy_uri);
	str_copy(st->cf_target[FEATURE_CF_NOANSWER], FEATURE_TARGET_MAX,
		 ch->call_fwd_no_answer_uri);
}

void feature_state_init(struct feature_state *st, const struct cfg_channel *ch)
{
	feature_state_reset(st, ch);
}

void feature_state_reset(struct feature_state *st, const struct cfg_channel *ch)
{
	memset(st, 0, sizeof(*st));
	state_seed(st, ch);
}

void feature_once_clear(struct feature_state *st)
{
	st->cid_once_set = false;
	st->cid_once_on = false;
}

unsigned int feature_lookup(const struct cfg_channel *ch, const char *buf,
			    enum feature_action *act)
{
	unsigned int i, n = 0;

	*act = FEATURE_ACT_NONE;
	if (!*buf)
		return 0;

	for (i = FEATURE_ACT_NONE + 1; i < FEATURE_ACT_COUNT; i++) {
		const char *code = feature_code_of(ch, (enum feature_action)i);

		if (!*code || strcmp(code, buf))
			continue;
		if (!n)
			*act = (enum feature_action)i;

		n++;
	}

	return n;
}

/*
 * Whether one of the rows in [first, end) carries a code that buf is a strict
 * prefix of. An unset code is shorter than any buffer and extends nothing; an
 * empty buf belongs to the caller, because every configured code extends it.
 */
static bool code_extends(const struct cfg_channel *ch, const char *buf,
			 enum feature_action first, enum feature_action end)
{
	size_t len = strlen(buf);
	unsigned int i;

	for (i = first; i < end; i++) {
		const char *code = feature_code_of(ch, (enum feature_action)i);

		if (strlen(code) <= len)
			continue;
		if (!strncmp(code, buf, len))
			return true;
	}

	return false;
}

/*
 * The next member of a duplicate group. Reporting a group against its first
 * member keeps one fault to one message however many options share the
 * string.
 */
static enum feature_action dup_next(const struct cfg_channel *ch, const char *code,
				    enum feature_action after)
{
	unsigned int i;

	for (i = after + 1; i < FEATURE_ACT_COUNT; i++) {
		if (!strcmp(feature_code_of(ch, (enum feature_action)i), code))
			return (enum feature_action)i;
	}

	return FEATURE_ACT_NONE;
}

/*
 * A shorter code shadows this one only when it is reachable itself: a shorter
 * code that some other option duplicates never fires, so it hides nothing.
 */
static enum feature_action shadow_find(const struct cfg_channel *ch, const char *code)
{
	size_t len = strlen(code);
	unsigned int i;

	for (i = FEATURE_ACT_NONE + 1; i < FEATURE_ACT_COUNT; i++) {
		const char *other = feature_code_of(ch, (enum feature_action)i);
		enum feature_action first;
		size_t olen = strlen(other);

		if (!olen || olen >= len || strncmp(other, code, olen))
			continue;
		if (feature_lookup(ch, other, &first) != 1)
			continue;

		return (enum feature_action)i;
	}

	return FEATURE_ACT_NONE;
}

enum feature_fault feature_fault_of(const struct cfg_channel *ch,
				    enum feature_action act, enum feature_action *other)
{
	const char *code = feature_code_of(ch, act);
	enum feature_action first;

	*other = FEATURE_ACT_NONE;
	if (!*code)
		return FEATURE_FAULT_NONE;

	if (feature_lookup(ch, code, &first) > 1) {
		if (first != act)
			return FEATURE_FAULT_NONE;

		*other = dup_next(ch, code, act);

		return FEATURE_FAULT_DUPLICATE;
	}

	*other = shadow_find(ch, code);
	if (*other == FEATURE_ACT_NONE)
		return FEATURE_FAULT_NONE;

	return FEATURE_FAULT_SHADOWED;
}

bool feature_code_clamp(struct cfg_channel *ch, enum feature_action act, size_t max)
{
	char *code;

	if (!act_valid(act))
		return false;

	code = (char *)ch + table[act].code_off;
	if (!*code || strlen(code) <= max)
		return false;

	*code = 0;

	return true;
}

enum feature_verdict feature_match(const struct feature_state *st,
				   const struct cfg_channel *ch, const char *buf)
{
	enum feature_action act;

	if (!*buf)
		return FEATURE_NONE;
	if (feature_lookup(ch, buf, &act) == 1)
		return FEATURE_MATCH;
	if (code_extends(ch, buf, FEATURE_ACT_NONE + 1, FEATURE_ACT_COUNT))
		return FEATURE_PARTIAL;

	return FEATURE_NONE;
}

static bool cw_override_set(struct feature_state *st, bool on)
{
	st->cw_set = true;
	st->cw_on = on;

	return true;
}

static bool cid_once_override_set(struct feature_state *st, bool on)
{
	st->cid_once_set = true;
	st->cid_once_on = on;

	return true;
}

/*
 * The enable code turns the restriction on, which withholds the number, so
 * the presentation this stores is its inverse.
 */
static bool cid_persist_restrict_set(struct feature_state *st, bool on)
{
	st->cid_persist_set = true;
	st->cid_persist_on = on;

	return true;
}

static bool dnd_set(struct feature_state *st, bool on)
{
	st->dnd = on;

	return true;
}

static bool cf_clear(struct feature_state *st, uint8_t mask)
{
	st->cf_mask &= (uint8_t)~mask;
	if (mask & FEATURE_CF_BIT(FEATURE_CF_NOANSWER))
		st->cfnr_timeout_s = 0;

	return true;
}

static bool cf_query(const struct feature_state *st, enum feature_cf kind)
{
	return (st->cf_mask & FEATURE_CF_BIT(kind)) != 0;
}

/*
 * The window suffix of the ring-no-answer code, <target>*<seconds>. The
 * first '*' splits, so a target can hold none. A suffix with no digit or with
 * a non-digit refuses the whole code: the subscriber asked for a window this
 * line would otherwise silently ignore.
 */
static bool arg_split(const char *arg, char *target, size_t tlen, unsigned int *secs,
		      bool suffix_ok)
{
	const char *split = suffix_ok ? strchr(arg, '*') : NULL;
	size_t len = split ? (size_t)(split - arg) : strlen(arg);
	unsigned long value = 0;
	const char *p;

	if (len >= tlen)
		return false;

	memcpy(target, arg, len);
	target[len] = 0;

	if (!split)
		return true;

	p = split + 1;
	if (!*p)
		return false;

	for (; *p; p++) {
		if (*p < '0' || *p > '9')
			return false;
		if (value <= FEATURE_CFNR_MAX_S)
			value = value * 10 + (unsigned long)(*p - '0');
	}

	*secs = value > FEATURE_CFNR_MAX_S ? FEATURE_CFNR_MAX_S : (unsigned int)value;

	return true;
}

/*
 * Arm every condition of the mask on one target. The whole set is validated
 * before anything is written, so a refusal leaves the state exactly as it was.
 */
static bool cf_arm(struct feature_state *st, uint8_t mask, const char *arg,
		   bool suffix_ok)
{
	char target[FEATURE_TARGET_MAX];
	unsigned int secs = 0;
	unsigned int i;

	if (!arg_split(arg, target, sizeof(target), &secs, suffix_ok))
		return false;

	for (i = 0; i < FEATURE_CF_COUNT; i++) {
		if (!(mask & FEATURE_CF_BIT(i)))
			continue;
		if (!*target && !st->cf_target[i][0])
			return false;
	}

	for (i = 0; i < FEATURE_CF_COUNT; i++) {
		if (!(mask & FEATURE_CF_BIT(i)))
			continue;
		if (*target)
			str_copy(st->cf_target[i], FEATURE_TARGET_MAX, target);
	}

	st->cf_mask |= mask;
	if (mask & FEATURE_CF_BIT(FEATURE_CF_NOANSWER))
		st->cfnr_timeout_s = secs;

	return true;
}

bool feature_execute(struct feature_state *st, const struct cfg_channel *ch,
		     const char *buf)
{
	enum feature_action act;

	if (feature_lookup(ch, buf, &act) != 1)
		return false;

	switch (act) {
	case FEATURE_ACT_CW_ENABLE:
		return cw_override_set(st, true);
	case FEATURE_ACT_CW_DISABLE:
		return cw_override_set(st, false);
	case FEATURE_ACT_CW_QUERY:
		return feature_cw_effective(st, ch);
	case FEATURE_ACT_CID_ONCE_ENABLE:
		return cid_once_override_set(st, true);
	case FEATURE_ACT_CID_ONCE_DISABLE:
		return cid_once_override_set(st, false);
	case FEATURE_ACT_CID_PERSIST_ENABLE:
		return cid_persist_restrict_set(st, true);
	case FEATURE_ACT_CID_PERSIST_DISABLE:
		return cid_persist_restrict_set(st, false);
	case FEATURE_ACT_DND_ON:
		return dnd_set(st, true);
	case FEATURE_ACT_DND_OFF:
		return dnd_set(st, false);
	case FEATURE_ACT_CFU_CLEAR:
		return cf_clear(st, FEATURE_CF_BIT(FEATURE_CF_UNCOND));
	case FEATURE_ACT_CFB_CLEAR:
		return cf_clear(st, FEATURE_CF_BIT(FEATURE_CF_BUSY));
	case FEATURE_ACT_CFNR_CLEAR:
		return cf_clear(st, FEATURE_CF_BIT(FEATURE_CF_NOANSWER));
	case FEATURE_ACT_ASCF_CLEAR:
		return cf_clear(st, FEATURE_CF_BIT(FEATURE_CF_BUSY) |
				FEATURE_CF_BIT(FEATURE_CF_NOANSWER));
	/*
	 * The query answers the confirmation tone when the condition is armed
	 * and the fast busy when it is not, which is the call waiting query
	 * convention: two tones are the whole answer channel this product
	 * has.
	 */
	case FEATURE_ACT_CFU_QUERY:
		return cf_query(st, FEATURE_CF_UNCOND);
	case FEATURE_ACT_CFB_QUERY:
		return cf_query(st, FEATURE_CF_BUSY);
	case FEATURE_ACT_CFNR_QUERY:
		return cf_query(st, FEATURE_CF_NOANSWER);
	/* recognized, no local arm exists: the fast busy is the answer */
	case FEATURE_ACT_CFNRC_SET:
	case FEATURE_ACT_CFNRC_CLEAR:
		return false;
	/* the four rows that need a target reach feature_execute_arg instead */
	case FEATURE_ACT_CFU_SET:
	case FEATURE_ACT_CFB_SET:
	case FEATURE_ACT_CFNR_SET:
	case FEATURE_ACT_ASCF_SET:
		return false;
	/*
	 * The transfer rows never belong here at all. The FSM classifies them
	 * with feature_action_get() and runs the call op itself. A cell that
	 * reached this refusal would answer fast busy, and a busy tone between
	 * a transfer request and its outcome reads as a failed transfer.
	 */
	case FEATURE_ACT_XFER_BLIND:
	case FEATURE_ACT_XFER_ATTENDED:
	case FEATURE_ACT_XFER_CONSULT:
		return false;
	case FEATURE_ACT_NONE:
	case FEATURE_ACT_COUNT:
		return false;
	}

	return false;
}

bool feature_takes_arg(const struct cfg_channel *ch, const char *buf)
{
	enum feature_action act;

	if (feature_lookup(ch, buf, &act) != 1)
		return false;

	return table[act].takes_arg && table[act].supported;
}

bool feature_execute_arg(struct feature_state *st, const struct cfg_channel *ch,
			 const char *code, const char *arg)
{
	enum feature_action act;

	if (feature_lookup(ch, code, &act) != 1)
		return false;
	if (!table[act].takes_arg || !table[act].supported)
		return false;

	switch (act) {
	case FEATURE_ACT_CFU_SET:
		return cf_arm(st, FEATURE_CF_BIT(FEATURE_CF_UNCOND), arg, false);
	case FEATURE_ACT_CFB_SET:
		return cf_arm(st, FEATURE_CF_BIT(FEATURE_CF_BUSY), arg, false);
	case FEATURE_ACT_CFNR_SET:
		return cf_arm(st, FEATURE_CF_BIT(FEATURE_CF_NOANSWER), arg, true);
	/* one code arms both busy and no answer, on one target */
	case FEATURE_ACT_ASCF_SET:
		return cf_arm(st, FEATURE_CF_BIT(FEATURE_CF_BUSY) |
			      FEATURE_CF_BIT(FEATURE_CF_NOANSWER), arg, false);
	/* the collected transfer target goes to the FSM, never through here */
	case FEATURE_ACT_XFER_BLIND:
	case FEATURE_ACT_XFER_ATTENDED:
	case FEATURE_ACT_XFER_CONSULT:
		return false;
	default:
		return false;
	}
}

enum feature_action feature_action_get(const struct cfg_channel *ch,
				       const char *buf)
{
	enum feature_action act;

	if (feature_lookup(ch, buf, &act) != 1)
		return FEATURE_ACT_NONE;

	return act;
}

bool feature_xfer_prefix(const struct cfg_channel *ch, const char *buf)
{
	if (!*buf)
		return false;

	return code_extends(ch, buf, FEATURE_ACT_XFER_BLIND, FEATURE_ACT_COUNT);
}

bool feature_dnd_is(const struct feature_state *st)
{
	return st->dnd;
}

bool feature_cf_armed(const struct feature_state *st, enum feature_cf kind,
		      const char **target)
{
	if (kind >= FEATURE_CF_COUNT)
		return false;

	if (target)
		*target = st->cf_target[kind];

	return (st->cf_mask & FEATURE_CF_BIT(kind)) != 0;
}

unsigned int feature_cfnr_timeout_s(const struct feature_state *st,
				    const struct cfg_channel *ch)
{
	if (st->cfnr_timeout_s)
		return st->cfnr_timeout_s;

	if (ch->call_fwd_no_answer_timeout > FEATURE_CFNR_MAX_S)
		return FEATURE_CFNR_MAX_S;

	return ch->call_fwd_no_answer_timeout;
}

bool feature_cw_effective(const struct feature_state *st, const struct cfg_channel *ch)
{
	if (st->cw_set)
		return st->cw_on;

	return ch->call_waiting_enabled;
}

bool feature_cid_presentation(const struct feature_state *st, const struct cfg_channel *ch)
{
	if (st->cid_persist_set)
		return !st->cid_persist_on;

	return ch->cid_outgoing_enable;
}

bool feature_cid_once_peek(const struct feature_state *st, bool *presentation)
{
	if (!st->cid_once_set)
		return false;

	*presentation = st->cid_once_on;

	return true;
}

bool feature_cid_restrict_peek(const struct feature_state *st, bool *withheld)
{
	if (!st->cid_persist_set)
		return false;

	*withheld = st->cid_persist_on;

	return true;
}

bool feature_cid_once_take(struct feature_state *st, bool *presentation)
{
	if (!feature_cid_once_peek(st, presentation))
		return false;

	feature_once_clear(st);

	return true;
}
