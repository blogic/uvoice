// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the star code table and the runtime feature state
 * (src/proto/sip/features.c).
 *
 * The fixture holds the schema defaults of src/cfg/schema.h, which the case
 * labels call the shipped config. A /etc/config/voip without star code
 * options loads these defaults only under star_codes_local 1. opt_code() in
 * cfg/uci.c then falls back to the descriptor default. Under
 * star_codes_local 0, the shipped value, an absent code loads empty.
 *
 * Under star_codes_local 1, the strings below are therefore the ones that a
 * subscriber dials. The schema seeds no pair of codes with one string,
 * because two codes with one string cancel each other in the table.
 *
 * The suite checks the polarity of the caller ID codes in both directions.
 * In the uci schema, cid_single_call_enable_exten "enables outgoing CID
 * for one call" and cid_single_call_disable_exten "disables outgoing CID for
 * one call". The persistent pair has the opposite sense. cid_enable_exten
 * enables the originating-id restriction, so a subscriber who dials it
 * withholds the number from then on. cid_disable_exten lifts the restriction.
 *
 * The schema seeds neither persistent option, so the fixture leaves both
 * empty. A reversed polarity publishes a number that the subscriber asked to
 * withhold. Each of these directions is therefore a case of its own.
 */
#include <stdlib.h>

#include "proto/sip/features.h"
#include "cfg/cfg.h"
#include "dialplan.h"
#include "test_util.h"

/* the caller's limit, which fsm.h defines FSM_DIGITS_MAX as */
#define CODE_MAX	DIALPLAN_DIGITS_MAX

static struct cfg_channel ch;
static struct feature_state st;

/* the src/cfg/schema.h defaults, which are the US strings */
static void ch_default(void)
{
	memset(&ch, 0, sizeof(ch));
	ch.call_waiting_enabled = true;
	ch.cid_outgoing_enable = true;

	snprintf(ch.cw_enable_code, sizeof(ch.cw_enable_code), "*43#");
	snprintf(ch.cw_disable_code, sizeof(ch.cw_disable_code), "#43#");
	snprintf(ch.cw_query_code, sizeof(ch.cw_query_code), "*#43#");
	snprintf(ch.cid_single_call_enable_exten,
		 sizeof(ch.cid_single_call_enable_exten), "*31#");
	snprintf(ch.cid_single_call_disable_exten,
		 sizeof(ch.cid_single_call_disable_exten), "#31#");
	/* cid_enable_exten and cid_disable_exten stay empty: the schema seeds
	 * neither, so the shipped table carries the persistent rows with
	 * nothing to match */
	snprintf(ch.do_not_disturb_on_exten, sizeof(ch.do_not_disturb_on_exten), "*78");
	snprintf(ch.do_not_disturb_off_exten, sizeof(ch.do_not_disturb_off_exten), "#78");
	snprintf(ch.call_fwd_uncond_exten, sizeof(ch.call_fwd_uncond_exten), "*21#");
	snprintf(ch.call_fwd_uncond_cancel_exten,
		 sizeof(ch.call_fwd_uncond_cancel_exten), "#21#");
	snprintf(ch.call_fwd_uncond_query_exten,
		 sizeof(ch.call_fwd_uncond_query_exten), "*#21#");
	snprintf(ch.call_fwd_busy_exten, sizeof(ch.call_fwd_busy_exten), "*67#");
	snprintf(ch.call_fwd_busy_cancel_exten,
		 sizeof(ch.call_fwd_busy_cancel_exten), "#67#");
	snprintf(ch.call_fwd_busy_query_exten,
		 sizeof(ch.call_fwd_busy_query_exten), "*#67#");
	snprintf(ch.call_fwd_no_answer_exten,
		 sizeof(ch.call_fwd_no_answer_exten), "*61#");
	snprintf(ch.call_fwd_no_answer_cancel_exten,
		 sizeof(ch.call_fwd_no_answer_cancel_exten), "#61#");
	snprintf(ch.call_fwd_no_answer_query_exten,
		 sizeof(ch.call_fwd_no_answer_query_exten), "*#61#");
	snprintf(ch.call_fwd_unreachable_exten,
		 sizeof(ch.call_fwd_unreachable_exten), "*62#");
	snprintf(ch.call_fwd_unreachable_cancel_exten,
		 sizeof(ch.call_fwd_unreachable_cancel_exten), "#62#");
	snprintf(ch.call_fwd_all_exten, sizeof(ch.call_fwd_all_exten), "*004#");
	/* two options carrying one string cancel each other in this table,
	 * so the cancel seed differs from the "*004#" of the set code */
	snprintf(ch.call_fwd_all_cancel_exten,
		 sizeof(ch.call_fwd_all_cancel_exten), "#004#");
	/* the factory transfer codes, which the schema seeds */
	snprintf(ch.blind_transfer_exten, sizeof(ch.blind_transfer_exten), "*072");
	snprintf(ch.attended_transfer_exten, sizeof(ch.attended_transfer_exten),
		 "*073");
	snprintf(ch.consult_transfer_exten, sizeof(ch.consult_transfer_exten),
		 "*074");
	/* the schema seeds the local forward service, with no condition armed,
	 * no target and the fifteen second window */
	ch.call_fwd_local = true;
	ch.call_fwd_no_answer_timeout = 15;
}

static void fixture_reset(void)
{
	ch_default();
	feature_state_init(&st, &ch);
}

static enum feature_verdict verdict(const char *buf)
{
	return feature_match(&st, &ch, buf);
}

static unsigned int hits(const char *buf, enum feature_action *act)
{
	return feature_lookup(&ch, buf, act);
}

/* ------------------------------------------------------------ the table */

static void t_table(void)
{
	static const struct {
		const char *code;
		enum feature_action act;
	} returned_codes[] = {
		{ "*78", FEATURE_ACT_DND_ON },
		{ "#78", FEATURE_ACT_DND_OFF },
		{ "*21#", FEATURE_ACT_CFU_SET },
		{ "#21#", FEATURE_ACT_CFU_CLEAR },
		{ "*#21#", FEATURE_ACT_CFU_QUERY },
		{ "*67#", FEATURE_ACT_CFB_SET },
		{ "#67#", FEATURE_ACT_CFB_CLEAR },
		{ "*#67#", FEATURE_ACT_CFB_QUERY },
		{ "*61#", FEATURE_ACT_CFNR_SET },
		{ "#61#", FEATURE_ACT_CFNR_CLEAR },
		{ "*#61#", FEATURE_ACT_CFNR_QUERY },
		{ "*62#", FEATURE_ACT_CFNRC_SET },
		{ "#62#", FEATURE_ACT_CFNRC_CLEAR },
		{ "*004#", FEATURE_ACT_ASCF_SET },
		{ "#004#", FEATURE_ACT_ASCF_CLEAR },
	};
	enum feature_action act;
	unsigned int i;

	fixture_reset();

	CASE("every supported code the schema seeds is one exact match");
	CHECK_UINT(hits("*43#", &act), 1);
	CHECK_UINT(act, FEATURE_ACT_CW_ENABLE);
	CHECK_UINT(hits("#43#", &act), 1);
	CHECK_UINT(act, FEATURE_ACT_CW_DISABLE);
	CHECK_UINT(hits("*#43#", &act), 1);
	CHECK_UINT(act, FEATURE_ACT_CW_QUERY);
	CHECK_UINT(hits("*31#", &act), 1);
	CHECK_UINT(act, FEATURE_ACT_CID_ONCE_ENABLE);
	CHECK_UINT(hits("#31#", &act), 1);
	CHECK_UINT(act, FEATURE_ACT_CID_ONCE_DISABLE);

	/* the fifteen DND and call forward strings are rows of the table */
	CASE("the DND and call forward codes are table rows again");
	CHECK_UINT(sizeof(returned_codes) / sizeof(returned_codes[0]), 15);
	for (i = 0; i < sizeof(returned_codes) / sizeof(returned_codes[0]); i++) {
		CHECK_UINT(hits(returned_codes[i].code, &act), 1);
		CHECK_UINT(act, returned_codes[i].act);
		CHECK_UINT(verdict(returned_codes[i].code), FEATURE_MATCH);
	}

	CASE("the two forward on unreachable rows are recognized, not supported");
	CHECK(!feature_action_supported(FEATURE_ACT_CFNRC_SET));
	CHECK(!feature_action_supported(FEATURE_ACT_CFNRC_CLEAR));
	CHECK(!feature_execute(&st, &ch, "*62#"));
	CHECK(!feature_execute(&st, &ch, "#62#"));

	CASE("every other table row is a supported action");
	for (i = FEATURE_ACT_NONE + 1; i < FEATURE_ACT_COUNT; i++) {
		if (i == FEATURE_ACT_CFNRC_SET || i == FEATURE_ACT_CFNRC_CLEAR)
			continue;
		CHECK(feature_action_supported((enum feature_action)i));
	}
	CHECK(!feature_action_supported(FEATURE_ACT_NONE));
	CHECK(!feature_action_supported(FEATURE_ACT_COUNT));

	CASE("the four forward set codes and the three transfer codes take a target");
	CHECK(feature_takes_arg(&ch, "*21#"));
	CHECK(feature_takes_arg(&ch, "*67#"));
	CHECK(feature_takes_arg(&ch, "*61#"));
	CHECK(feature_takes_arg(&ch, "*004#"));
	CHECK(feature_takes_arg(&ch, "*072"));
	CHECK(feature_takes_arg(&ch, "*073"));
	CHECK(feature_takes_arg(&ch, "*074"));
	CHECK(!feature_takes_arg(&ch, "#21#"));
	CHECK(!feature_takes_arg(&ch, "*#21#"));
	CHECK(!feature_takes_arg(&ch, "*43#"));
	CHECK(!feature_takes_arg(&ch, "*78"));
	CHECK(!feature_takes_arg(&ch, "5551234"));
	CHECK(!feature_takes_arg(&ch, ""));

	/* an unsupported row must never open a collection that cannot end */
	CASE("the unreachable set code takes no target either");
	CHECK(!feature_takes_arg(&ch, "*62#"));

	CASE("a row names its uci option, and a non-row names nothing");
	CHECK(strcmp(feature_action_name(FEATURE_ACT_CW_ENABLE),
		     "call_waiting_enable_exten") == 0);
	CHECK(strcmp(feature_action_name(FEATURE_ACT_CID_ONCE_DISABLE),
		     "cid_single_call_disable_exten") == 0);
	CHECK(strcmp(feature_action_name(FEATURE_ACT_NONE), "none") == 0);
	CHECK(strcmp(feature_action_name(FEATURE_ACT_COUNT), "none") == 0);

	CASE("a row reads its code out of the loaded channel");
	CHECK(strcmp(feature_code_of(&ch, FEATURE_ACT_CW_QUERY), "*#43#") == 0);
	CHECK(strcmp(feature_code_of(&ch, FEATURE_ACT_NONE), "") == 0);
	CHECK(strcmp(feature_code_of(&ch, FEATURE_ACT_COUNT), "") == 0);
}

/* --------------------------------------------------------- the verdicts */

static void t_match(void)
{
	fixture_reset();

	CASE("a complete code matches, '#' bearing or not");
	CHECK_UINT(verdict("*43#"), FEATURE_MATCH);
	CHECK_UINT(verdict("#43#"), FEATURE_MATCH);
	CHECK_UINT(verdict("*#43#"), FEATURE_MATCH);

	CASE("every prefix of a code keeps the collector collecting");
	CHECK_UINT(verdict("*"), FEATURE_PARTIAL);
	CHECK_UINT(verdict("*4"), FEATURE_PARTIAL);
	CHECK_UINT(verdict("*43"), FEATURE_PARTIAL);
	CHECK_UINT(verdict("#"), FEATURE_PARTIAL);
	CHECK_UINT(verdict("#4"), FEATURE_PARTIAL);
	CHECK_UINT(verdict("#43"), FEATURE_PARTIAL);
	CHECK_UINT(verdict("*#"), FEATURE_PARTIAL);
	CHECK_UINT(verdict("*#4"), FEATURE_PARTIAL);

	CASE("prefixes of the returned codes hold the digit collector again");
	CHECK_UINT(verdict("*7"), FEATURE_PARTIAL);
	CHECK_UINT(verdict("#7"), FEATURE_PARTIAL);
	CHECK_UINT(verdict("*2"), FEATURE_PARTIAL);
	CHECK_UINT(verdict("*6"), FEATURE_PARTIAL);
	CHECK_UINT(verdict("*0"), FEATURE_PARTIAL);
	CHECK_UINT(verdict("*00"), FEATURE_PARTIAL);
	CHECK_UINT(verdict("#0"), FEATURE_PARTIAL);

	CASE("a star string no code carries is not the table's business");
	CHECK_UINT(verdict("*99"), FEATURE_NONE);
	CHECK_UINT(verdict("*99#"), FEATURE_NONE);
	CHECK_UINT(verdict("#9"), FEATURE_NONE);
	CHECK_UINT(verdict("*430"), FEATURE_NONE);

	CASE("a code with one digit too many is not a match either");
	CHECK_UINT(verdict("*43#9"), FEATURE_NONE);

	CASE("an ordinary number never reaches a feature");
	CHECK_UINT(verdict("5"), FEATURE_NONE);
	CHECK_UINT(verdict("5551234"), FEATURE_NONE);
	CHECK_UINT(verdict("911"), FEATURE_NONE);

	CASE("an empty buffer matches nothing");
	CHECK_UINT(verdict(""), FEATURE_NONE);

	CASE("an unset option never matches, and never makes a prefix");
	memset(&ch, 0, sizeof(ch));
	CHECK_UINT(verdict(""), FEATURE_NONE);
	CHECK_UINT(verdict("*"), FEATURE_NONE);
	CHECK_UINT(verdict("*43#"), FEATURE_NONE);
	CHECK_UINT(verdict("5"), FEATURE_NONE);

	CASE("a string two options share is reported as a collision");
	fixture_reset();
	snprintf(ch.cw_enable_code, sizeof(ch.cw_enable_code), "*77#");
	snprintf(ch.cid_single_call_enable_exten,
		 sizeof(ch.cid_single_call_enable_exten), "*77#");
	{
		enum feature_action act;

		CHECK_UINT(hits("*77#", &act), 2);
		CHECK_UINT(act, FEATURE_ACT_CW_ENABLE);
	}

	CASE("the table abstains from a collision instead of picking a winner");
	CHECK_UINT(verdict("*77#"), FEATURE_NONE);
	CHECK(!feature_execute(&st, &ch, "*77#"));
	CHECK(!st.cw_set);
	CHECK(!st.cid_once_set);

	CASE("a collision still collects while a longer code can match");
	snprintf(ch.cw_query_code, sizeof(ch.cw_query_code), "*77#9");
	CHECK_UINT(verdict("*77#"), FEATURE_PARTIAL);

	CASE("an operator collision on a supported code abstains the same way");
	fixture_reset();
	snprintf(ch.cid_single_call_enable_exten,
		 sizeof(ch.cid_single_call_enable_exten), "*43#");
	CHECK_UINT(verdict("*43#"), FEATURE_NONE);
	CHECK(!feature_execute(&st, &ch, "*43#"));
	CHECK(!st.cw_set);
	CHECK(!st.cid_once_set);
}

/*
 * The persistent pair, in the direction that the cid_enable_exten and
 * cid_disable_exten options fix. A flip of the direction fails here, not on
 * the line of a subscriber.
 */
static void t_cid_persistent(void)
{
	enum feature_action act;

	fixture_reset();
	snprintf(ch.cid_enable_exten, sizeof(ch.cid_enable_exten), "*99#");
	snprintf(ch.cid_disable_exten, sizeof(ch.cid_disable_exten), "#99#");

	CASE("cid_enable_exten is a table row of its own");
	CHECK_UINT(hits("*99#", &act), 1);
	CHECK_UINT(act, FEATURE_ACT_CID_PERSIST_ENABLE);
	CHECK_UINT(verdict("*99#"), FEATURE_MATCH);
	CHECK(strcmp(feature_action_name(FEATURE_ACT_CID_PERSIST_ENABLE),
		     "cid_enable_exten") == 0);

	CASE("cid_disable_exten is the other one");
	CHECK_UINT(hits("#99#", &act), 1);
	CHECK_UINT(act, FEATURE_ACT_CID_PERSIST_DISABLE);
	CHECK_UINT(verdict("#99#"), FEATURE_MATCH);
	CHECK(strcmp(feature_action_name(FEATURE_ACT_CID_PERSIST_DISABLE),
		     "cid_disable_exten") == 0);

	/* enable turns the restriction on, which withholds the number */
	CASE("the enable code turns the restriction on, so the number is withheld");
	CHECK(feature_execute(&st, &ch, "*99#"));
	CHECK(st.cid_persist_set);
	CHECK(st.cid_persist_on);
	CHECK(!feature_cid_presentation(&st, &ch));

	CASE("the disable code lifts it again");
	CHECK(feature_execute(&st, &ch, "#99#"));
	CHECK(st.cid_persist_set);
	CHECK(!st.cid_persist_on);
	CHECK(feature_cid_presentation(&st, &ch));

	CASE("both codes answer CONFIRM, they never fast busy");
	fixture_reset();
	snprintf(ch.cid_enable_exten, sizeof(ch.cid_enable_exten), "*99#");
	snprintf(ch.cid_disable_exten, sizeof(ch.cid_disable_exten), "#99#");
	CHECK(feature_execute(&st, &ch, "#99#"));
	CHECK(feature_execute(&st, &ch, "*99#"));

	CASE("the restriction overrides a config that presents the number");
	CHECK(ch.cid_outgoing_enable);
	CHECK(!feature_cid_presentation(&st, &ch));

	CASE("and lifting it overrides a config that withholds it");
	ch.cid_outgoing_enable = false;
	CHECK(feature_execute(&st, &ch, "#99#"));
	CHECK(feature_cid_presentation(&st, &ch));

	CASE("the persistent pair leaves the one-call override alone");
	{
		bool present = false;

		CHECK(!feature_cid_once_take(&st, &present));
	}

	CASE("and the persistent pair leaves call waiting alone");
	CHECK(!st.cw_set);

	CASE("the shipped config carries the rows with nothing to match");
	fixture_reset();
	CHECK(strcmp(feature_code_of(&ch, FEATURE_ACT_CID_PERSIST_ENABLE), "") == 0);
	CHECK(strcmp(feature_code_of(&ch, FEATURE_ACT_CID_PERSIST_DISABLE), "") == 0);
	CHECK_UINT(hits("*31#", &act), 1);
	CHECK_UINT(act, FEATURE_ACT_CID_ONCE_ENABLE);
	CHECK_UINT(hits("#31#", &act), 1);
	CHECK_UINT(act, FEATURE_ACT_CID_ONCE_DISABLE);
}

/* ------------------------------------------------------- call waiting */

static void t_call_waiting(void)
{
	fixture_reset();

	CASE("with no override the effective value is the loaded one");
	CHECK(feature_cw_effective(&st, &ch));
	ch.call_waiting_enabled = false;
	CHECK(!feature_cw_effective(&st, &ch));

	CASE("the enable code turns call waiting on over a config that says off");
	CHECK(feature_execute(&st, &ch, "*43#"));
	CHECK(feature_cw_effective(&st, &ch));

	CASE("the disable code turns it off over a config that says on");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "#43#"));
	CHECK(!feature_cw_effective(&st, &ch));

	CASE("the last code dialled wins");
	CHECK(feature_execute(&st, &ch, "*43#"));
	CHECK(feature_cw_effective(&st, &ch));
	CHECK(feature_execute(&st, &ch, "#43#"));
	CHECK(!feature_cw_effective(&st, &ch));

	/*
	 * The query answers with the outcome, which the caller turns into a
	 * confirmation tone or a fast busy.
	 */
	CASE("the query confirms while call waiting is effectively on");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "*#43#"));

	CASE("the query refuses while it is effectively off");
	CHECK(feature_execute(&st, &ch, "#43#"));
	CHECK(!feature_execute(&st, &ch, "*#43#"));

	CASE("the query reads the config when no code was dialled");
	fixture_reset();
	ch.call_waiting_enabled = false;
	CHECK(!feature_execute(&st, &ch, "*#43#"));

	CASE("the query changes nothing itself");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "*#43#"));
	CHECK(!st.cw_set);
}

/* ---------------------------------------------------------- caller id */

static void cid_persist_codes(void)
{
	snprintf(ch.cid_enable_exten, sizeof(ch.cid_enable_exten), "*99#");
	snprintf(ch.cid_disable_exten, sizeof(ch.cid_disable_exten), "#99#");
}

static void t_cid(void)
{
	bool present = false;

	fixture_reset();

	CASE("with no code dialled the presentation is the loaded one");
	CHECK(feature_cid_presentation(&st, &ch));
	ch.cid_outgoing_enable = false;
	CHECK(!feature_cid_presentation(&st, &ch));

	/* cid_single_call_enable_exten enables outgoing CID for one call */
	CASE("the one-call enable code presents the number for one call");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "*31#"));
	CHECK(feature_cid_once_take(&st, &present));
	CHECK(present);

	/* cid_single_call_disable_exten disables outgoing CID for one call */
	CASE("the one-call disable code withholds it for one call");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "#31#"));
	present = true;
	CHECK(feature_cid_once_take(&st, &present));
	CHECK(!present);

	CASE("the override is consumed once");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "#31#"));
	CHECK(feature_cid_once_take(&st, &present));
	CHECK(!feature_cid_once_take(&st, &present));

	CASE("with no code dialled there is nothing to take");
	fixture_reset();
	CHECK(!feature_cid_once_take(&st, &present));

	CASE("the last one-call code dialled wins");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "#31#"));
	CHECK(feature_execute(&st, &ch, "*31#"));
	present = false;
	CHECK(feature_cid_once_take(&st, &present));
	CHECK(present);

	CASE("a one-call code leaves the persistent state alone");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "#31#"));
	CHECK(feature_cid_presentation(&st, &ch));
	CHECK(!st.cid_persist_set);

	CASE("and a one-call code leaves call waiting alone");
	CHECK(feature_cw_effective(&st, &ch));
	CHECK(!st.cw_set);

	/*
	 * The precedence, the whole ladder in one place: the one-call
	 * override first, the persistent restriction under it, the loaded
	 * config under that. line/line.c applies exactly this order at the dial
	 * moment, so a flip of any rung is visible here.
	 */
	CASE("the one-call override beats a persistent restriction");
	fixture_reset();
	cid_persist_codes();
	CHECK(feature_execute(&st, &ch, "*99#"));
	CHECK(!feature_cid_presentation(&st, &ch));
	CHECK(feature_execute(&st, &ch, "*31#"));
	present = false;
	CHECK(feature_cid_once_take(&st, &present));
	CHECK(present);

	CASE("the restriction is still there for the call after that one");
	CHECK(!feature_cid_once_take(&st, &present));
	CHECK(!feature_cid_presentation(&st, &ch));

	CASE("the one-call override also beats a lifted restriction");
	fixture_reset();
	cid_persist_codes();
	ch.cid_outgoing_enable = false;
	CHECK(feature_execute(&st, &ch, "#99#"));
	CHECK(feature_cid_presentation(&st, &ch));
	CHECK(feature_execute(&st, &ch, "#31#"));
	present = true;
	CHECK(feature_cid_once_take(&st, &present));
	CHECK(!present);
	CHECK(feature_cid_presentation(&st, &ch));

	CASE("a persistent restriction beats the config, in both directions");
	fixture_reset();
	cid_persist_codes();
	CHECK(feature_execute(&st, &ch, "*99#"));
	CHECK(!feature_cid_presentation(&st, &ch));
	ch.cid_outgoing_enable = false;
	CHECK(feature_execute(&st, &ch, "#99#"));
	CHECK(feature_cid_presentation(&st, &ch));

	CASE("the last persistent code dialled wins");
	fixture_reset();
	cid_persist_codes();
	CHECK(feature_execute(&st, &ch, "*99#"));
	CHECK(feature_execute(&st, &ch, "#99#"));
	CHECK(feature_execute(&st, &ch, "*99#"));
	CHECK(!feature_cid_presentation(&st, &ch));
}

/* ------------------------------------------------------ do not disturb */

static void t_dnd(void)
{
	fixture_reset();

	CASE("the config seeds the bit the enforcement reads");
	CHECK(!feature_dnd_is(&st));
	ch.do_not_disturb_enabled = true;
	feature_state_init(&st, &ch);
	CHECK(feature_dnd_is(&st));

	CASE("the on code sets it and confirms");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "*78"));
	CHECK(feature_dnd_is(&st));

	CASE("the off code clears it and confirms");
	CHECK(feature_execute(&st, &ch, "#78"));
	CHECK(!feature_dnd_is(&st));

	CASE("both codes confirm however often they are dialled");
	CHECK(feature_execute(&st, &ch, "#78"));
	CHECK(feature_execute(&st, &ch, "*78"));
	CHECK(feature_execute(&st, &ch, "*78"));
	CHECK(feature_dnd_is(&st));

	CASE("the off code beats a config that seeded it on");
	ch.do_not_disturb_enabled = true;
	feature_state_init(&st, &ch);
	CHECK(feature_execute(&st, &ch, "#78"));
	CHECK(!feature_dnd_is(&st));

	CASE("do not disturb touches neither call waiting nor the caller id");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "*78"));
	CHECK(feature_cw_effective(&st, &ch));
	CHECK(feature_cid_presentation(&st, &ch));
	CHECK(!st.cw_set);
	CHECK(!st.cid_once_set);

	/*
	 * The table matches whole buffers. A prefix match would let a longer
	 * string that starts with the code enable the service too.
	 */
	CASE("a longer string that starts with the on code is not the on code");
	fixture_reset();
	CHECK(!feature_execute(&st, &ch, "*789"));
	CHECK(!feature_dnd_is(&st));
	CHECK_UINT(verdict("*789"), FEATURE_NONE);
}

/* ------------------------------------------------------ call forwarding */

static const char *cf_target(enum feature_cf kind)
{
	const char *target = NULL;

	feature_cf_armed(&st, kind, &target);

	return target;
}

static void t_cf_seed(void)
{
	fixture_reset();

	CASE("an unconfigured line arms no condition");
	CHECK(!feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_BUSY, NULL));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_NOANSWER, NULL));

	CASE("the mode bitmask seeds the conditions one bit each");
	ch.call_fwd_local_mode = 1;
	snprintf(ch.call_fwd_uncond_uri, sizeof(ch.call_fwd_uncond_uri), "5551111");
	feature_state_init(&st, &ch);
	CHECK(feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_BUSY, NULL));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_NOANSWER, NULL));
	CHECK(strcmp(cf_target(FEATURE_CF_UNCOND), "5551111") == 0);

	CASE("the shipped 6 is busy plus no answer, each on its own uri");
	fixture_reset();
	ch.call_fwd_local_mode = 6;
	snprintf(ch.call_fwd_busy_uri, sizeof(ch.call_fwd_busy_uri), "5552222");
	snprintf(ch.call_fwd_no_answer_uri, sizeof(ch.call_fwd_no_answer_uri),
		 "5553333");
	feature_state_init(&st, &ch);
	CHECK(!feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));
	CHECK(feature_cf_armed(&st, FEATURE_CF_BUSY, NULL));
	CHECK(feature_cf_armed(&st, FEATURE_CF_NOANSWER, NULL));
	CHECK(strcmp(cf_target(FEATURE_CF_BUSY), "5552222") == 0);
	CHECK(strcmp(cf_target(FEATURE_CF_NOANSWER), "5553333") == 0);

	CASE("a condition armed with no uri is armed with an empty target");
	fixture_reset();
	ch.call_fwd_local_mode = 1;
	feature_state_init(&st, &ch);
	CHECK(feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));
	CHECK(strcmp(cf_target(FEATURE_CF_UNCOND), "") == 0);

	/* the gate: with the service on the network nothing local is armed */
	CASE("call_fwd_local 0 seeds no condition whatever the mode says");
	fixture_reset();
	ch.call_fwd_local = false;
	ch.call_fwd_local_mode = 6;
	snprintf(ch.call_fwd_busy_uri, sizeof(ch.call_fwd_busy_uri), "5552222");
	snprintf(ch.call_fwd_no_answer_uri, sizeof(ch.call_fwd_no_answer_uri),
		 "5553333");
	feature_state_init(&st, &ch);
	CHECK(!feature_cf_armed(&st, FEATURE_CF_BUSY, NULL));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_NOANSWER, NULL));
	CHECK(strcmp(cf_target(FEATURE_CF_BUSY), "") == 0);

	CASE("a mode bit no condition owns is dropped");
	fixture_reset();
	ch.call_fwd_local_mode = 0xff;
	feature_state_init(&st, &ch);
	CHECK_UINT(st.cf_mask, 7);

	CASE("the seeded window is the configured one until a code moves it");
	fixture_reset();
	CHECK_UINT(feature_cfnr_timeout_s(&st, &ch), 15);
	ch.call_fwd_no_answer_timeout = 40;
	CHECK_UINT(feature_cfnr_timeout_s(&st, &ch), 40);

	CASE("an absurd configured window is clamped");
	ch.call_fwd_no_answer_timeout = 999999999;
	CHECK_UINT(feature_cfnr_timeout_s(&st, &ch), FEATURE_CFNR_MAX_S);
}

static void t_cf_codes(void)
{
	fixture_reset();

	CASE("the unconditional set code arms its condition on the dialled target");
	CHECK(feature_execute_arg(&st, &ch, "*21#", "5551234"));
	CHECK(feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));
	CHECK(strcmp(cf_target(FEATURE_CF_UNCOND), "5551234") == 0);
	CHECK(!feature_cf_armed(&st, FEATURE_CF_BUSY, NULL));

	CASE("the query confirms while it is armed");
	CHECK(feature_execute(&st, &ch, "*#21#"));

	CASE("the clear code drops the bit and keeps the target");
	CHECK(feature_execute(&st, &ch, "#21#"));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));
	CHECK(strcmp(cf_target(FEATURE_CF_UNCOND), "5551234") == 0);

	CASE("and the query then refuses, which is the fast busy");
	CHECK(!feature_execute(&st, &ch, "*#21#"));

	CASE("an empty target falls back to the one the state holds");
	CHECK(feature_execute_arg(&st, &ch, "*21#", ""));
	CHECK(feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));
	CHECK(strcmp(cf_target(FEATURE_CF_UNCOND), "5551234") == 0);

	CASE("an empty target with nothing to fall back on is refused");
	fixture_reset();
	CHECK(!feature_execute_arg(&st, &ch, "*21#", ""));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));

	CASE("the configured uri is what an empty target falls back to");
	fixture_reset();
	snprintf(ch.call_fwd_uncond_uri, sizeof(ch.call_fwd_uncond_uri), "5559999");
	feature_state_init(&st, &ch);
	CHECK(!feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));
	CHECK(feature_execute_arg(&st, &ch, "*21#", ""));
	CHECK(strcmp(cf_target(FEATURE_CF_UNCOND), "5559999") == 0);

	CASE("the busy and no answer codes arm their own condition alone");
	fixture_reset();
	CHECK(feature_execute_arg(&st, &ch, "*67#", "5552222"));
	CHECK(feature_execute_arg(&st, &ch, "*61#", "5553333"));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));
	CHECK(strcmp(cf_target(FEATURE_CF_BUSY), "5552222") == 0);
	CHECK(strcmp(cf_target(FEATURE_CF_NOANSWER), "5553333") == 0);

	CASE("each query answers its own condition");
	CHECK(feature_execute(&st, &ch, "*#67#"));
	CHECK(feature_execute(&st, &ch, "*#61#"));
	CHECK(!feature_execute(&st, &ch, "*#21#"));

	CASE("each clear code drops its own bit only");
	CHECK(feature_execute(&st, &ch, "#67#"));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_BUSY, NULL));
	CHECK(feature_cf_armed(&st, FEATURE_CF_NOANSWER, NULL));
	CHECK(feature_execute(&st, &ch, "#61#"));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_NOANSWER, NULL));

	CASE("a set code with no target field cannot be invoked without one");
	fixture_reset();
	CHECK(!feature_execute(&st, &ch, "*21#"));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));

	CASE("a target longer than the field is refused, not truncated");
	{
		char big[FEATURE_TARGET_MAX + 8];

		memset(big, '1', sizeof(big));
		big[sizeof(big) - 1] = 0;
		fixture_reset();
		CHECK(!feature_execute_arg(&st, &ch, "*21#", big));
		CHECK(!feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));
	}

	CASE("an unknown or unsupported code takes no target either");
	fixture_reset();
	CHECK(!feature_execute_arg(&st, &ch, "*99#", "5551234"));
	CHECK(!feature_execute_arg(&st, &ch, "*62#", "5551234"));
	CHECK(!feature_execute_arg(&st, &ch, "#21#", "5551234"));
	CHECK(!feature_execute_arg(&st, &ch, "", "5551234"));
}

static void t_cf_ascf(void)
{
	fixture_reset();

	CASE("the all-forward code arms busy AND no answer on one target");
	CHECK(feature_execute_arg(&st, &ch, "*004#", "5554444"));
	CHECK(feature_cf_armed(&st, FEATURE_CF_BUSY, NULL));
	CHECK(feature_cf_armed(&st, FEATURE_CF_NOANSWER, NULL));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));
	CHECK(strcmp(cf_target(FEATURE_CF_BUSY), "5554444") == 0);
	CHECK(strcmp(cf_target(FEATURE_CF_NOANSWER), "5554444") == 0);

	CASE("its cancel code drops both bits");
	CHECK(feature_execute(&st, &ch, "#004#"));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_BUSY, NULL));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_NOANSWER, NULL));

	CASE("and leaves the unconditional condition alone");
	CHECK(feature_execute_arg(&st, &ch, "*21#", "5551234"));
	CHECK(feature_execute_arg(&st, &ch, "*004#", "5554444"));
	CHECK(feature_execute(&st, &ch, "#004#"));
	CHECK(feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));

	/*
	 * Each condition answers for itself. A guard on the unconditional
	 * target would stop a subscriber who never set one from arming the
	 * two conditions that the ASCF code owns.
	 */
	CASE("no unconditional target is needed to arm it");
	fixture_reset();
	CHECK(strcmp(cf_target(FEATURE_CF_UNCOND), "") == 0);
	CHECK(feature_execute_arg(&st, &ch, "*004#", "5554444"));
	CHECK(feature_cf_armed(&st, FEATURE_CF_BUSY, NULL));

	CASE("an empty target falls back per condition");
	fixture_reset();
	snprintf(ch.call_fwd_busy_uri, sizeof(ch.call_fwd_busy_uri), "5552222");
	snprintf(ch.call_fwd_no_answer_uri, sizeof(ch.call_fwd_no_answer_uri),
		 "5553333");
	feature_state_init(&st, &ch);
	CHECK(feature_execute_arg(&st, &ch, "*004#", ""));
	CHECK(strcmp(cf_target(FEATURE_CF_BUSY), "5552222") == 0);
	CHECK(strcmp(cf_target(FEATURE_CF_NOANSWER), "5553333") == 0);

	CASE("one missing fallback refuses the whole code and arms neither bit");
	fixture_reset();
	snprintf(ch.call_fwd_busy_uri, sizeof(ch.call_fwd_busy_uri), "5552222");
	feature_state_init(&st, &ch);
	CHECK(!feature_execute_arg(&st, &ch, "*004#", ""));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_BUSY, NULL));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_NOANSWER, NULL));
}

static void t_cf_window(void)
{
	fixture_reset();

	CASE("the no answer code accepts the window suffix");
	CHECK(feature_execute_arg(&st, &ch, "*61#", "5553333*30"));
	CHECK(strcmp(cf_target(FEATURE_CF_NOANSWER), "5553333") == 0);
	CHECK_UINT(feature_cfnr_timeout_s(&st, &ch), 30);

	CASE("a set code with no suffix goes back to the configured window");
	CHECK(feature_execute_arg(&st, &ch, "*61#", "5553333"));
	CHECK_UINT(feature_cfnr_timeout_s(&st, &ch), 15);

	CASE("the suffix works with an empty target too");
	CHECK(feature_execute_arg(&st, &ch, "*61#", "*45"));
	CHECK(strcmp(cf_target(FEATURE_CF_NOANSWER), "5553333") == 0);
	CHECK_UINT(feature_cfnr_timeout_s(&st, &ch), 45);

	CASE("the clear code drops the window with the condition");
	CHECK(feature_execute(&st, &ch, "#61#"));
	CHECK_UINT(feature_cfnr_timeout_s(&st, &ch), 15);

	CASE("a suffix with no digits refuses the code and arms nothing");
	fixture_reset();
	CHECK(!feature_execute_arg(&st, &ch, "*61#", "5553333*"));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_NOANSWER, NULL));
	CHECK(!feature_execute_arg(&st, &ch, "*61#", "5553333*4a"));
	CHECK(!feature_cf_armed(&st, FEATURE_CF_NOANSWER, NULL));

	CASE("an absurd window is clamped, not refused");
	CHECK(feature_execute_arg(&st, &ch, "*61#", "5553333*99999999999"));
	CHECK_UINT(feature_cfnr_timeout_s(&st, &ch), FEATURE_CFNR_MAX_S);

	CASE("only the no answer code reads a suffix");
	fixture_reset();
	CHECK(feature_execute_arg(&st, &ch, "*21#", "5551234*30"));
	CHECK(strcmp(cf_target(FEATURE_CF_UNCOND), "5551234*30") == 0);
	CHECK_UINT(feature_cfnr_timeout_s(&st, &ch), 15);
}

/* ------------------------------------------------------- the two scopes */

static void t_scopes(void)
{
	bool present = false;

	CASE("init leaves no override behind");
	fixture_reset();
	CHECK(feature_cw_effective(&st, &ch));
	CHECK(!feature_cid_once_take(&st, &present));

	CASE("a reload drops the call waiting override");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "#43#"));
	CHECK(!feature_cw_effective(&st, &ch));
	feature_state_reset(&st, &ch);
	CHECK(feature_cw_effective(&st, &ch));

	CASE("a reload drops the one-call override too");
	CHECK(feature_execute(&st, &ch, "#31#"));
	feature_state_reset(&st, &ch);
	CHECK(!feature_cid_once_take(&st, &present));

	CASE("the idle clear drops the one-call override");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "*31#"));
	feature_once_clear(&st);
	CHECK(!feature_cid_once_take(&st, &present));

	CASE("the idle clear KEEPS the call waiting override, which is persistent");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "#43#"));
	feature_once_clear(&st);
	CHECK(!feature_cw_effective(&st, &ch));

	CASE("the idle clear KEEPS the caller id restriction, which is persistent too");
	fixture_reset();
	cid_persist_codes();
	CHECK(feature_execute(&st, &ch, "*99#"));
	feature_once_clear(&st);
	CHECK(st.cid_persist_set);
	CHECK(!feature_cid_presentation(&st, &ch));

	CASE("a reload drops the caller id restriction");
	feature_state_reset(&st, &ch);
	CHECK(!st.cid_persist_set);
	CHECK(feature_cid_presentation(&st, &ch));

	CASE("both clears are idempotent");
	feature_once_clear(&st);
	feature_state_reset(&st, &ch);
	feature_state_reset(&st, &ch);
	CHECK(feature_cw_effective(&st, &ch));
	CHECK(!feature_cid_once_take(&st, &present));

	/*
	 * A reload drops what the subscriber dialled and seeds the config
	 * again, so the line comes back exactly as /etc/config/voip describes
	 * it. The dialled targets and the mode go together.
	 */
	CASE("a reload drops a dialled do not disturb and takes the config again");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "*78"));
	CHECK(feature_dnd_is(&st));
	feature_state_reset(&st, &ch);
	CHECK(!feature_dnd_is(&st));

	CASE("and the other way round: a config that says on comes back on");
	ch.do_not_disturb_enabled = true;
	CHECK(feature_execute(&st, &ch, "#78"));
	CHECK(!feature_dnd_is(&st));
	feature_state_reset(&st, &ch);
	CHECK(feature_dnd_is(&st));

	CASE("a reload drops a dialled forward, target and window together");
	fixture_reset();
	CHECK(feature_execute_arg(&st, &ch, "*61#", "5553333*30"));
	feature_state_reset(&st, &ch);
	CHECK(!feature_cf_armed(&st, FEATURE_CF_NOANSWER, NULL));
	CHECK(strcmp(cf_target(FEATURE_CF_NOANSWER), "") == 0);
	CHECK_UINT(feature_cfnr_timeout_s(&st, &ch), 15);

	CASE("a reload re-arms what the config arms");
	ch.call_fwd_local_mode = 1;
	snprintf(ch.call_fwd_uncond_uri, sizeof(ch.call_fwd_uncond_uri), "5551111");
	CHECK(feature_execute(&st, &ch, "#21#"));
	feature_state_reset(&st, &ch);
	CHECK(feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));
	CHECK(strcmp(cf_target(FEATURE_CF_UNCOND), "5551111") == 0);

	CASE("the idle clear KEEPS both, which belong to the line and not the call");
	fixture_reset();
	CHECK(feature_execute(&st, &ch, "*78"));
	CHECK(feature_execute_arg(&st, &ch, "*21#", "5551234"));
	feature_once_clear(&st);
	CHECK(feature_dnd_is(&st));
	CHECK(feature_cf_armed(&st, FEATURE_CF_UNCOND, NULL));
}

/*
 * Precedence (features.h): a complete code fires on the digit that completes
 * it. A longer code that starts with it can therefore never be dialed to the
 * end. The table does not resolve that; it reports it.
 */
static void t_precedence(void)
{
	enum feature_action other;

	fixture_reset();
	snprintf(ch.cw_enable_code, sizeof(ch.cw_enable_code), "*1");
	snprintf(ch.cid_single_call_enable_exten,
		 sizeof(ch.cid_single_call_enable_exten), "*12");

	CASE("an exact code fires even when a longer code extends it");
	CHECK_UINT(verdict("*1"), FEATURE_MATCH);
	CHECK(feature_execute(&st, &ch, "*1"));
	CHECK(feature_cw_effective(&st, &ch));

	/* the collector never gets to ask about "*12": "*1" already fired */
	CASE("the longer code is therefore unreachable, and is reported as such");
	CHECK_UINT(feature_fault_of(&ch, FEATURE_ACT_CID_ONCE_ENABLE, &other),
		   FEATURE_FAULT_SHADOWED);
	CHECK_UINT(other, FEATURE_ACT_CW_ENABLE);

	CASE("the code that fires carries no fault of its own");
	CHECK_UINT(feature_fault_of(&ch, FEATURE_ACT_CW_ENABLE, &other),
		   FEATURE_FAULT_NONE);
	CHECK_UINT(other, FEATURE_ACT_NONE);

	CASE("a shorter code that is itself unreachable shadows nothing");
	snprintf(ch.cw_disable_code, sizeof(ch.cw_disable_code), "*1");
	CHECK_UINT(feature_fault_of(&ch, FEATURE_ACT_CID_ONCE_ENABLE, &other),
		   FEATURE_FAULT_NONE);

	CASE("no shipped code shadows another");
	fixture_reset();
	{
		unsigned int i, n = 0;

		for (i = FEATURE_ACT_NONE + 1; i < FEATURE_ACT_COUNT; i++) {
			if (feature_fault_of(&ch, (enum feature_action)i, &other) ==
			    FEATURE_FAULT_SHADOWED)
				n++;
		}

		CHECK_UINT(n, 0);
	}
}

static void t_faults(void)
{
	enum feature_action other;

	fixture_reset();

	snprintf(ch.cw_enable_code, sizeof(ch.cw_enable_code), "*77#");
	snprintf(ch.cid_single_call_enable_exten,
		 sizeof(ch.cid_single_call_enable_exten), "*77#");
	CASE("an operator-created duplicate is reported against its first option");
	CHECK_UINT(feature_fault_of(&ch, FEATURE_ACT_CW_ENABLE, &other),
		   FEATURE_FAULT_DUPLICATE);
	CHECK_UINT(other, FEATURE_ACT_CID_ONCE_ENABLE);
	CHECK_UINT(feature_fault_of(&ch, FEATURE_ACT_CID_ONCE_ENABLE, &other),
		   FEATURE_FAULT_NONE);

	CASE("both actions in the duplicate remain supported");
	CHECK(feature_action_supported(FEATURE_ACT_CW_ENABLE));
	CHECK(feature_action_supported(FEATURE_ACT_CID_ONCE_ENABLE));

	CASE("a second operator-created duplicate is reported the same way");
	fixture_reset();
	snprintf(ch.cw_disable_code, sizeof(ch.cw_disable_code), "*43#");
	CHECK_UINT(feature_fault_of(&ch, FEATURE_ACT_CW_ENABLE, &other),
		   FEATURE_FAULT_DUPLICATE);
	CHECK_UINT(other, FEATURE_ACT_CW_DISABLE);
	CHECK(feature_action_supported(FEATURE_ACT_CW_ENABLE));

	CASE("and the code it names really does stop working");
	CHECK_UINT(verdict("*43#"), FEATURE_NONE);
	CHECK(!feature_execute(&st, &ch, "*43#"));

	CASE("an unset option carries no fault");
	fixture_reset();
	ch.cw_enable_code[0] = 0;
	CHECK_UINT(feature_fault_of(&ch, FEATURE_ACT_CW_ENABLE, &other),
		   FEATURE_FAULT_NONE);

	CASE("the shipped config has no table fault");
	fixture_reset();
	{
		unsigned int i, n = 0;

		for (i = FEATURE_ACT_NONE + 1; i < FEATURE_ACT_COUNT; i++) {
			if (feature_fault_of(&ch, (enum feature_action)i, &other) !=
			    FEATURE_FAULT_NONE)
				n++;
		}

		CHECK_UINT(n, 0);
	}
}

/*
 * The three transfer rows. They are match-only: the table recognizes the
 * code and collects the target behind it, and the FSM runs the REFER through
 * ops->call_xfer. Both execute entry points refuse them. A cell that reached
 * either would therefore answer the fast busy. The tone rule of features.h
 * prevents that, because a busy tone between a transfer request and its
 * outcome reads as a failed transfer.
 */
static void t_xfer(void)
{
	static const struct {
		const char *code;
		enum feature_action act;
	} rows[] = {
		{ "*072", FEATURE_ACT_XFER_BLIND },
		{ "*073", FEATURE_ACT_XFER_ATTENDED },
		{ "*074", FEATURE_ACT_XFER_CONSULT },
	};
	struct feature_state before;
	enum feature_action act;
	unsigned int i;

	fixture_reset();

	CASE("each transfer code is one exact match on its own row");
	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		CHECK_UINT(hits(rows[i].code, &act), 1);
		CHECK_UINT(act, rows[i].act);
		CHECK_UINT(verdict(rows[i].code), FEATURE_MATCH);
	}

	CASE("the three rows are supported and every one takes a target");
	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		CHECK(feature_action_supported(rows[i].act));
		CHECK(feature_takes_arg(&ch, rows[i].code));
	}

	CASE("neither execute entry point runs a transfer");
	memcpy(&before, &st, sizeof(before));
	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		CHECK(!feature_execute(&st, &ch, rows[i].code));
		CHECK(!feature_execute_arg(&st, &ch, rows[i].code, "5551234"));
		CHECK(!feature_execute_arg(&st, &ch, rows[i].code, ""));
	}

	CASE("and a refused transfer moves no runtime state");
	CHECK(!memcmp(&st, &before, sizeof(st)));

	CASE("a row names its uci option");
	CHECK(strcmp(feature_action_name(FEATURE_ACT_XFER_BLIND),
		     "blind_transfer_exten") == 0);
	CHECK(strcmp(feature_action_name(FEATURE_ACT_XFER_ATTENDED),
		     "attended_transfer_exten") == 0);
	CHECK(strcmp(feature_action_name(FEATURE_ACT_XFER_CONSULT),
		     "consult_transfer_exten") == 0);

	CASE("feature_action_get names the row a buffer equals exactly");
	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
		CHECK_UINT(feature_action_get(&ch, rows[i].code), rows[i].act);

	CASE("it names a non-transfer row the same way");
	CHECK_UINT(feature_action_get(&ch, "*43#"), FEATURE_ACT_CW_ENABLE);
	CHECK_UINT(feature_action_get(&ch, "*62#"), FEATURE_ACT_CFNRC_SET);

	CASE("and a buffer that is not a code names nothing");
	CHECK_UINT(feature_action_get(&ch, "*07"), FEATURE_ACT_NONE);
	CHECK_UINT(feature_action_get(&ch, "*0721"), FEATURE_ACT_NONE);
	CHECK_UINT(feature_action_get(&ch, "5551234"), FEATURE_ACT_NONE);
	CHECK_UINT(feature_action_get(&ch, ""), FEATURE_ACT_NONE);

	CASE("every strict prefix of a transfer code is a transfer prefix");
	CHECK(feature_xfer_prefix(&ch, "*"));
	CHECK(feature_xfer_prefix(&ch, "*0"));
	CHECK(feature_xfer_prefix(&ch, "*07"));

	CASE("the complete code is not a prefix of itself");
	for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
		CHECK(!feature_xfer_prefix(&ch, rows[i].code));

	CASE("nor is a buffer no transfer code extends");
	CHECK(!feature_xfer_prefix(&ch, "#"));
	CHECK(!feature_xfer_prefix(&ch, "*1"));
	CHECK(!feature_xfer_prefix(&ch, "*0721"));
	CHECK(!feature_xfer_prefix(&ch, "5"));
	CHECK(!feature_xfer_prefix(&ch, ""));

	/* the answer is about the three transfer rows alone: "*0" still leads
	 * to the all-forwarding code, and that is not a transfer prefix */
	CASE("an unset transfer exten matches neither lookup");
	fixture_reset();
	ch.blind_transfer_exten[0] = 0;
	ch.attended_transfer_exten[0] = 0;
	ch.consult_transfer_exten[0] = 0;
	CHECK(!feature_xfer_prefix(&ch, "*"));
	CHECK(!feature_xfer_prefix(&ch, "*0"));
	CHECK_UINT(feature_action_get(&ch, "*072"), FEATURE_ACT_NONE);
	CHECK_UINT(verdict("*00"), FEATURE_PARTIAL);
	CHECK_UINT(feature_action_get(&ch, "*004#"), FEATURE_ACT_ASCF_SET);

	CASE("one configured exten of the three is enough to answer");
	fixture_reset();
	ch.attended_transfer_exten[0] = 0;
	ch.consult_transfer_exten[0] = 0;
	CHECK(feature_xfer_prefix(&ch, "*07"));
	CHECK(!feature_xfer_prefix(&ch, "*072"));

	/* an operator can shadow one transfer code with another. The prefix
	 * answer and the exact answer are then both true of the same buffer,
	 * and the exact-match precedence of this table resolves it. */
	CASE("a transfer code another transfer code extends is both");
	fixture_reset();
	snprintf(ch.attended_transfer_exten, sizeof(ch.attended_transfer_exten),
		 "*0721");
	CHECK(feature_xfer_prefix(&ch, "*072"));
	CHECK_UINT(feature_action_get(&ch, "*072"), FEATURE_ACT_XFER_BLIND);
	CHECK_UINT(feature_fault_of(&ch, FEATURE_ACT_XFER_ATTENDED, &act),
		   FEATURE_FAULT_SHADOWED);
	CHECK_UINT(act, FEATURE_ACT_XFER_BLIND);

	CASE("two transfer options carrying one string cancel each other");
	fixture_reset();
	snprintf(ch.attended_transfer_exten, sizeof(ch.attended_transfer_exten),
		 "*072");
	/* the duplicated pair is the only code that extends "*07" here, so the
	 * prefix answer below is about the collision and nothing else */
	ch.consult_transfer_exten[0] = 0;
	CHECK_UINT(hits("*072", &act), 2);
	CHECK_UINT(feature_action_get(&ch, "*072"), FEATURE_ACT_NONE);
	CHECK(!feature_takes_arg(&ch, "*072"));
	CHECK_UINT(verdict("*072"), FEATURE_NONE);
	/* the prefix answer ignores the collision, as the FEATURE_PARTIAL
	 * verdict of feature_match() does: a buffer on its way to a duplicated
	 * code is still on its way to one, and the exact match is where the
	 * table abstains */
	CHECK(feature_xfer_prefix(&ch, "*07"));
	CHECK_UINT(verdict("*07"), FEATURE_PARTIAL);
	CHECK_UINT(feature_fault_of(&ch, FEATURE_ACT_XFER_BLIND, &act),
		   FEATURE_FAULT_DUPLICATE);
	CHECK_UINT(act, FEATURE_ACT_XFER_ATTENDED);

	CASE("the shipped transfer codes carry no fault of their own");
	fixture_reset();
	CHECK_UINT(feature_fault_of(&ch, FEATURE_ACT_XFER_BLIND, &act),
		   FEATURE_FAULT_NONE);
	CHECK_UINT(feature_fault_of(&ch, FEATURE_ACT_XFER_ATTENDED, &act),
		   FEATURE_FAULT_NONE);
	CHECK_UINT(feature_fault_of(&ch, FEATURE_ACT_XFER_CONSULT, &act),
		   FEATURE_FAULT_NONE);
}

/*
 * A code the caller's digit collector cannot hold. features.c never learns
 * the limit; the caller passes it and blanks its own copy.
 */
static void code_fill(char *dst, size_t len)
{
	memset(dst, '1', len);
	dst[0] = '*';
	dst[len] = 0;
}

static void t_clamp(void)
{
	fixture_reset();

	CASE("a code of exactly the buffer length survives and still matches");
	code_fill(ch.cw_enable_code, CODE_MAX);
	CHECK(!feature_code_clamp(&ch, FEATURE_ACT_CW_ENABLE, CODE_MAX));
	CHECK_UINT(strlen(ch.cw_enable_code), CODE_MAX);
	CHECK_UINT(verdict(ch.cw_enable_code), FEATURE_MATCH);
	CHECK(feature_execute(&st, &ch, ch.cw_enable_code));

	/* unblanked it would collect toward a code the caller can never hold,
	 * which is the fault the clamp removes */
	CASE("one digit more is blanked");
	fixture_reset();
	code_fill(ch.cw_enable_code, CODE_MAX + 1);
	CHECK_UINT(verdict("*1"), FEATURE_PARTIAL);
	CHECK(feature_code_clamp(&ch, FEATURE_ACT_CW_ENABLE, CODE_MAX));
	CHECK_UINT(strlen(ch.cw_enable_code), 0);

	CASE("a blanked code produces no partial collection and no match");
	CHECK_UINT(verdict("*1"), FEATURE_NONE);
	CHECK_UINT(verdict("*11"), FEATURE_NONE);
	CHECK(!feature_execute(&st, &ch, "*1"));

	CASE("blanking one code leaves the others alone");
	CHECK_UINT(verdict("#43#"), FEATURE_MATCH);

	CASE("the clamp is idempotent and ignores an unset or unknown option");
	CHECK(!feature_code_clamp(&ch, FEATURE_ACT_CW_ENABLE, CODE_MAX));
	CHECK(!feature_code_clamp(&ch, FEATURE_ACT_NONE, CODE_MAX));
	CHECK(!feature_code_clamp(&ch, FEATURE_ACT_COUNT, CODE_MAX));

	CASE("no shipped code is longer than the buffer");
	fixture_reset();
	{
		unsigned int i, n = 0;

		for (i = FEATURE_ACT_NONE + 1; i < FEATURE_ACT_COUNT; i++) {
			if (feature_code_clamp(&ch, (enum feature_action)i, CODE_MAX))
				n++;
		}

		CHECK_UINT(n, 0);
	}
}

int main(void)
{
	t_table();
	t_match();
	t_cid_persistent();
	t_call_waiting();
	t_cid();
	t_dnd();
	t_cf_seed();
	t_cf_codes();
	t_cf_ascf();
	t_cf_window();
	t_scopes();
	t_precedence();
	t_faults();
	t_xfer();
	t_clamp();

	return tu_summary("test_features");
}
