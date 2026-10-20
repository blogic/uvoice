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
 * Dependencies. libc only. No allocation, no uloop, no HAL, no SIP. The
 * caller owns every struct, so test_features runs in the libc-only suite
 * group and fsm.c can consult the table from inside its purity contract.
 *
 * A separate table, not dial plan entries: the star codes stay out of the
 * digit map. fsm.c consults this table per digit before the '#' escape and
 * before the dial plan verdict, so a code that carries its own '#' (the
 * shipped "*43#") consumes it instead of losing it to the escape.
 *
 * Fixed strings, with seven exceptions. Every configured exten carries the
 * complete code, so most rows need no argument collection. Seven rows are the
 * exception, and feature_takes_arg() names all seven.
 *
 * The four call forward set rows collect for this module: the subscriber
 * dials the target behind the code as *21#<target># and the collector has to
 * take it, which is what makes FSM_SPRE_COLLECT reachable.
 * feature_execute_arg() applies them.
 *
 * The three transfer rows collect for the FSM and are match-only here: both
 * feature_execute() and feature_execute_arg() refuse them, because the whole
 * effect of such a row is the call op the FSM runs with the collected target.
 * The transfer block below carries that contract.
 *
 * Runtime state only. A toggle lives in struct feature_state, dies
 * with the daemon and is dropped by a reload. Nothing here writes uci, and
 * struct cfg stays immutable. The do-not-disturb bit, the forward mask and the
 * forward targets follow the same rule with one addition: a reload seeds them
 * from the loaded config again (feature_state_reset), so the config is the
 * value the line comes up with and the subscriber's changes live until the
 * next reload. The targets and the mode follow one rule, so a reload never
 * keeps one without the other.
 *
 * One state copy. The query codes answer from the very fields the enforcement
 * of fsm.c reads, so a query and the enforcement cannot disagree.
 *
 * The persistent caller-id pair (cid_enable_exten, cid_disable_exten) is in
 * the table, in the restriction direction, and ships unseeded; see the caller
 * id polarity note below.
 */
#ifndef UVOICE_CALL_FEATURES_H
#define UVOICE_CALL_FEATURES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cfg_channel;

/*
 * The width of a stored forward target. It is CFG_STR_MAX, the width every
 * loaded exten and uri field carries, and features.c static-asserts the tie so
 * the two cannot drift; cfg.h stays out of this header, which is libc only.
 */
#define FEATURE_TARGET_MAX	64

/*
 * The ceiling of a dialled ring-no-answer window, in seconds. It is the same
 * one day sip/line.c applies to every configured second value
 * (cfg.h LINE_TIMER_MAX_S), restated here because this header stays libc only.
 */
#define FEATURE_CFNR_MAX_S	86400u

/*
 * The verdict for the digit buffer the subscriber has dialled so far.
 * FEATURE_PARTIAL means at least one configured code extends this buffer, so
 * the collector must keep collecting instead of letting the dial plan act.
 * MATCH is returned only when exactly one code equals the buffer: two codes
 * that share a string cannot be attributed, so the table abstains and the
 * buffer falls through to the '#' escape and the dial plan, as if no code were
 * configured. An operator can create this case by configuring two table
 * options with the same string.
 *
 * Precedence. An exact match fires, even when a longer code extends it.
 * Configure "*1" and "*12" and the subscriber can never reach "*12": this
 * returns MATCH at "*1" and the caller acts on that digit. A complete code
 * runs the moment it is complete, which keeps a correct config off the
 * interdigit timer. The shadowed code is a configuration fault, not a state
 * this module resolves: feature_fault_of() reports it and sip/line.c warns
 * once per reload when a supported action is the one lost. No shipped pair
 * collides this way.
 */
enum feature_verdict {
	FEATURE_NONE = 0,
	FEATURE_PARTIAL,
	FEATURE_MATCH,
};

/*
 * One row per consulted uci option. The order is the table order in
 * features.c, which is also the order feature_lookup() reports a collision
 * from.
 */
enum feature_action {
	FEATURE_ACT_NONE = 0,
	FEATURE_ACT_CW_ENABLE,
	FEATURE_ACT_CW_DISABLE,
	FEATURE_ACT_CW_QUERY,
	FEATURE_ACT_CID_ONCE_ENABLE,
	FEATURE_ACT_CID_ONCE_DISABLE,
	FEATURE_ACT_CID_PERSIST_ENABLE,
	FEATURE_ACT_CID_PERSIST_DISABLE,
	FEATURE_ACT_DND_ON,
	FEATURE_ACT_DND_OFF,
	FEATURE_ACT_CFU_SET,
	FEATURE_ACT_CFU_CLEAR,
	FEATURE_ACT_CFU_QUERY,
	FEATURE_ACT_CFB_SET,
	FEATURE_ACT_CFB_CLEAR,
	FEATURE_ACT_CFB_QUERY,
	FEATURE_ACT_CFNR_SET,
	FEATURE_ACT_CFNR_CLEAR,
	FEATURE_ACT_CFNR_QUERY,
	FEATURE_ACT_CFNRC_SET,
	FEATURE_ACT_CFNRC_CLEAR,
	FEATURE_ACT_ASCF_SET,
	FEATURE_ACT_ASCF_CLEAR,
	FEATURE_ACT_XFER_BLIND,
	FEATURE_ACT_XFER_ATTENDED,
	FEATURE_ACT_XFER_CONSULT,
	FEATURE_ACT_COUNT,
};

/*
 * ------------------------------------------------------------------------
 * The three transfer rows.
 *
 *   FEATURE_ACT_XFER_BLIND     blind_transfer_exten      default "*072"
 *   FEATURE_ACT_XFER_ATTENDED  attended_transfer_exten   default "*073"
 *   FEATURE_ACT_XFER_CONSULT   consult_transfer_exten    default "*074"
 *
 * The defaults are the defaults of the uci schema.
 *
 * The option names are shifted one slot against the transfer they name.
 * Restated from call.h so this table is readable on its own:
 *
 *   blind_transfer_exten     -> REFER, outcome at 202
 *   attended_transfer_exten  -> REFER, wait for NOTIFY
 *   consult_transfer_exten   -> on-hook consult
 *
 * So the "attended" option is a REFER that waits for its verdict, and the
 * transfer that RFC 5589 calls attended is the consult option.
 *
 * Match-only rows. All three behave like the four forward set rows at the
 * table surface and like nothing else beneath it:
 *   - feature_match() returns MATCH on the complete code, as for any row;
 *   - feature_takes_arg() returns true, because the transfer target is
 *     dialled behind the code and the FSM has to collect it (the
 *     FSM_XFER_COLLECT and HOLD_DIAL branches of fsm.h);
 *   - feature_execute() and feature_execute_arg() refuse them. There is no
 *     runtime toggle to apply: the row's whole effect is the ops->call_xfer
 *     or ops->call_xfer_consult the FSM cell runs with the collected target,
 *     and this module stays libc-pure and knows nothing about a call leg.
 *
 * The tone rule: no busy tone runs between the call_xfer op and its outcome
 * event, because a busy tone there reads as a failed transfer. A refused
 * execute answers fast busy everywhere else in this table, so the transfer
 * cells must never reach feature_execute at all.
 * feature_action_get() and feature_xfer_prefix() are how the FSM tells
 * the three rows apart before it can get there. test_features carries the
 * refusal of both execute entry points as a case of its own, and test_fsm
 * carries the case for the tone rule itself.
 * ------------------------------------------------------------------------
 */

/*
 * The three local forward conditions. The bit of a condition is
 * FEATURE_CF_BIT(kind), which equals the uci call_fwd_local_mode bitmask
 * exactly: 1 unconditional, 2 busy, 4 no answer, and the shipped 6 is busy
 * plus no answer. The fourth condition, forward on unreachable, has no
 * local arm at all and is not a member here (its two rows ship unsupported).
 */
enum feature_cf {
	FEATURE_CF_UNCOND = 0,
	FEATURE_CF_BUSY,
	FEATURE_CF_NOANSWER,
	FEATURE_CF_COUNT,
};

#define FEATURE_CF_BIT(kind)	((uint8_t)(1u << (kind)))
#define FEATURE_CF_MASK_ALL	((uint8_t)7u)

/*
 * The per line runtime state. The fields are private to features.c: every
 * consumer reads them through the accessors below, and sip/line.c only owns the
 * storage, because the no-allocation rule forbids an incomplete type here.
 *
 * The one-call override is the only one-call scope there is. It survives
 * until the call that consumes it, and the line clears it on the way back
 * to IDLE. The persistent caller-id restriction lives beside it and
 * survives every call; only a reload drops it.
 */
struct feature_state {
	bool cw_set;
	bool cw_on;
	bool cid_once_set;
	bool cid_once_on;
	bool cid_persist_set;
	bool cid_persist_on;		/* true = restriction on = withhold */
	bool dnd;
	uint8_t cf_mask;		/* FEATURE_CF_BIT() of each armed condition */
	unsigned int cfnr_timeout_s;	/* 0 takes call_fwd_no_answer_timeout */
	char cf_target[FEATURE_CF_COUNT][FEATURE_TARGET_MAX];
};

/*
 * Both entry points seed the do-not-disturb bit, the forward mask, the three
 * targets and the ring-no-answer window from the loaded channel; the call
 * waiting and caller-id overrides start unset, because those two answer from
 * the config through their own accessors instead.
 *
 * The forward seed is gated on call_fwd_local: with the service handed to the
 * network, this line arms no local condition at all, which is the enforcement
 * half of the gate. The table half is sip/line.c blanking the codes in its own
 * channel copy, exactly as it does for the caller-id codes.
 */
void feature_state_init(struct feature_state *st, const struct cfg_channel *ch);

/* reload scope: every runtime toggle goes and the seed runs again */
void feature_state_reset(struct feature_state *st, const struct cfg_channel *ch);

/* idle scope: the one-call overrides only */
void feature_once_clear(struct feature_state *st);

/*
 * Pure: no side effect, and no dependency on the runtime state either. The
 * state parameter is part of the stable contract and stays so a later change
 * can gate a code on the state it toggles (a query code that only exists
 * while the feature is on, for example) without moving every caller.
 */
enum feature_verdict feature_match(const struct feature_state *st,
				   const struct cfg_channel *ch, const char *buf);

/*
 * Apply the code the buffer names. true means the subscriber gets the
 * confirmation tone, false means fast busy: an unknown or ambiguous buffer
 * and the query code with the feature effectively off both return false. The
 * only side effect is the state this module owns; tones and FSM transitions
 * stay with the caller.
 */
bool feature_execute(struct feature_state *st, const struct cfg_channel *ch,
		     const char *buf);

/*
 * Pure: whether the code this buffer names collects a target behind itself.
 * The four supported forward set rows and the three transfer rows do. The
 * caller uses it to choose between the two invoke ops: a row that takes no
 * argument runs on the digit that completes it, and a row that takes one opens
 * the second stage. A transfer row collects for the FSM alone; the second
 * stage of this module refuses it.
 */
bool feature_takes_arg(const struct cfg_channel *ch, const char *buf);

/*
 * Apply a code together with the target the second stage collected. Same
 * answer as feature_execute: true is the confirmation tone, false the fast
 * busy.
 *
 * An empty argument falls back to the target the state already holds, which
 * after a reload is the configured one.
 * An empty argument with no target to fall back on is refused, and so is a
 * target that does not fit the field. Nothing is armed on a refusal: an ASCF
 * code that can only arm one of its two conditions arms neither.
 *
 * The ring-no-answer row also accepts a window suffix, <target>*<seconds>. A
 * suffix that carries no digits refuses the code
 * rather than dropping the window the subscriber asked for; a value above
 * FEATURE_CFNR_MAX_S is clamped to it. A set code with no suffix drops any
 * window a previous one left, so the code says the whole service state.
 */
bool feature_execute_arg(struct feature_state *st, const struct cfg_channel *ch,
			 const char *code, const char *arg);

/* the enforcement reads, which are the same fields the query codes answer from */
bool feature_dnd_is(const struct feature_state *st);

/*
 * True when the condition is armed. target, when given, is written with the
 * stored target, which can be empty: a config that sets call_fwd_local_mode
 * without the matching uri arms a condition with nowhere to send the call, and
 * this module reports that state rather than resolving it. The caller treats
 * an empty target as not armed and warns once at the derive.
 */
bool feature_cf_armed(const struct feature_state *st, enum feature_cf kind,
		      const char **target);

/* the effective ring-no-answer window: the dialled override, else the config */
unsigned int feature_cfnr_timeout_s(const struct feature_state *st,
				    const struct cfg_channel *ch);

/*
 * The number of configured codes that equal buf, with the first one written
 * to act (FEATURE_ACT_NONE when the count is zero). A count above one is a
 * configuration collision: the caller traces it and the table abstains.
 */
unsigned int feature_lookup(const struct cfg_channel *ch, const char *buf,
			    enum feature_action *act);

/*
 * Pure: the action of the row buf names exactly, and FEATURE_ACT_NONE for
 * everything else. A collision answers NONE too, because the table abstains on
 * one there as it does in feature_match().
 */
enum feature_action feature_action_get(const struct cfg_channel *ch,
				       const char *buf);

/*
 * Pure: whether buf is a non-empty strict prefix of at least one of the three
 * transfer extens, that is, a buffer that can still become one and is not one
 * yet. An exact code is not a prefix of itself here, and an unset exten
 * matches nothing.
 *
 * Why the table owes the FSM both. The HOLD_DIAL branch has to classify a
 * transfer code, and a buffer on its way to one, without putting either in
 * front of the dial plan. Neither existing call can do it:
 * feature_match() returns a verdict and never names the action, so a MATCH
 * cannot be told from any other row's MATCH, and its FEATURE_PARTIAL covers
 * every configured code rather than the three transfer ones. feature_lookup()
 * carries the collision count the FSM has no use for. Both calls are pure and
 * read the loaded channel alone, so fsm.c consults them from inside its purity
 * contract exactly as it consults feature_match().
 */
bool feature_xfer_prefix(const struct cfg_channel *ch, const char *buf);

/* the configured code of one action, "" when the option is unset */
const char *feature_code_of(const struct cfg_channel *ch, enum feature_action act);

/*
 * The two ways a configured code becomes unreachable. Both are configuration
 * faults, and both are reported rather than resolved: this module never picks
 * a winner for the subscriber.
 *
 * Duplicate: another option carries the same string, so the table abstains
 * and neither code runs. It is reported once, against the first action of the
 * group, with the next member written to `other`.
 *
 * Shadowed: a shorter code is a prefix of this one and is itself reachable,
 * so it fires first and this code can never be dialled to the end. `other`
 * names the code that fires instead.
 */
enum feature_fault {
	FEATURE_FAULT_NONE = 0,
	FEATURE_FAULT_DUPLICATE,
	FEATURE_FAULT_SHADOWED,
};

enum feature_fault feature_fault_of(const struct cfg_channel *ch,
				    enum feature_action act,
				    enum feature_action *other);

/*
 * Blank a code the caller's digit collector could never hold, and say whether
 * it did. A blank code matches nothing, so the table then behaves as if the
 * option were unset and the digits reach the dial plan untouched. The limit
 * is the caller's, passed in: this module stays libc-pure and knows
 * nothing about the FSM digit buffer. The channel must be the caller's own
 * copy, because a loaded struct cfg is immutable.
 */
bool feature_code_clamp(struct cfg_channel *ch, enum feature_action act, size_t max);

/* the canonical uci option name, so a trace is greppable in /etc/config/voip */
const char *feature_action_name(enum feature_action act);

/*
 * Whether the daemon carries an action for the row, wherever that action
 * lives. The two forward-on-unreachable rows return false: the daemon has no
 * local arm for that condition at all, so the codes are recognized and
 * answered with the fast busy instead of dialling through. sip/line.c consumes
 * this predicate generically and keeps its warnings quiet for a row no
 * subscriber can lose anything to.
 *
 * The three transfer rows return true. The predicate does not promise the
 * action lives in this module: feature_takes_arg() and feature_execute_arg()
 * both read it, so a false here would refuse the collection the transfer rows
 * exist to open, and sip/line.c would call a lost transfer code a row no
 * subscriber misses. What the transfer rows have and the unreachable rows do
 * not is an action somewhere in the daemon, which the FSM runs through
 * ops->call_xfer.
 *
 * The third consumer is a trace. sip/line.c feature_outcome() picks the word
 * one FSM trace line carries, and it picks no tone: the tone follows the bool
 * the invoke op returned. A transfer code that reaches an execute at all
 * therefore traces "refused", which is the true answer, rather than the
 * "recognized, not yet supported" a false here would print for a service the
 * daemon runs.
 */
bool feature_action_supported(enum feature_action act);

/*
 * The effective call waiting value: the runtime override when the subscriber
 * set one, the loaded config otherwise. sip/line.c pushes this into the
 * machine's own call_waiting guard, which decides whether the next incoming
 * call becomes an offer or a 486.
 */
bool feature_cw_effective(const struct feature_state *st, const struct cfg_channel *ch);

/*
 * Caller id polarity.
 *
 * One pair of strings for all four extens would carry two opposite meanings.
 * The one-call pair reads "*31#" as "present my number for this call", and
 * the persistent pair reads "*31#" as "withhold my number from now on".
 *
 * This table fixes the direction and the seeds: cid_enable_exten enables the
 * persistent originating-id restriction, so dialling it withholds from then
 * on, cid_disable_exten lifts it, and both options ship unseeded. The strings
 * therefore do not collide, the table carries all four rows, and the one-call
 * pair keeps "*31#" and "#31#".
 *
 * feature_cid_presentation() is the persistent presentation state: the
 * runtime restriction when the subscriber set one, the loaded
 * cid_outgoing_enable otherwise. The one-call override sits above it and is
 * taken, once, with feature_cid_once_take(); sip/line.c applies that order at
 * the dial moment.
 *
 * A code here is a whole buffer, not a prefix. 3GPP reads "*31#" as a prefix
 * and dials the rest of the digits behind it. This table fires on an exact
 * match instead, and reaches the same subscriber flow in two stages: the code
 * fires on its own final '#', the caller plays CONFIRM, the entry dial tone
 * returns with an empty buffer, and the number is then dialled with the
 * override still pending. A subscriber who
 * dials "*31#5551234" in one gesture therefore gets the 3GPP result.
 */
bool feature_cid_presentation(const struct feature_state *st,
			      const struct cfg_channel *ch);

/*
 * Consume the one-call override. true means an override was pending and its
 * value is written to presentation; the override is then gone, so a second
 * take on the same call returns false.
 */
bool feature_cid_once_take(struct feature_state *st, bool *presentation);

/* the same read without consuming it, which is what a status page needs */
bool feature_cid_once_peek(const struct feature_state *st, bool *presentation);

/*
 * The standing restriction, for that same status page. false means the
 * subscriber dialled neither persistent code, so presentation follows the
 * config alone. true means they did, and withheld says which way it went:
 * true is the restriction on, that is, the number is hidden from here on.
 * This state never reaches uci, so a status dump is the only place it can be
 * read.
 */
bool feature_cid_restrict_peek(const struct feature_state *st, bool *withheld);

#endif
