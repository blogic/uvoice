// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * MGCP digit map matcher (RFC 3435 2.1.5), the dial plan contract of the uci
 * option digit_map_base64.
 *
 * Dependencies. libc only. No allocation: fixed pools, caller-owned structs.
 *
 * The grammar. Accepted tokens: digits 0-9, DTMF letters A-D, '#' and '*',
 * the wildcard 'x' (any digit 0-9), a range '[...]' with subranges 'n-m',
 * the position '.' meaning zero or more of the preceding construct, the
 * timer 'T', and '|' between alternatives. A '[...]' set takes the wildcard
 * as a member and adds the digits 0 to 9 for it, so a field map that carries
 * '[*#x]' compiles. The wildcard is never the end of a subrange: '[x-9]' and
 * '[0-x]' refuse the map. The timer letter stays out of a set, and a '.'
 * after it refuses the map.
 * Normalization applies: 't' becomes 'T', 'X' becomes 'x'; any other
 * letter refuses the map and the caller falls back to the compiled-in
 * default. 'N' is refused too, because RFC 3435 2.1.5 defines no such
 * token.
 *
 * Matching model. Stateless full-buffer rescan: every dialplan_match() walks
 * every alternative against the whole collected digit string. A '.' loops
 * greedily with bounded backtrack (the buffer is DIALPLAN_DIGITS_MAX at
 * most), so the work per digit is trivially bounded and there is no
 * incremental matcher state to invalidate on a reload. The verdict is four
 * facts, each true when any alternative makes it true. A fully consumed
 * alternative dials at once; interdigit_timeout (short) runs when a timer
 * alone completes an alternative; interdigit_timeout_long runs when at least
 * one more digit, or a timer and more symbols, are needed (RFC 3660 2.2,
 * Tcrit and Tpar; NCS 1.5 7.1.5).
 *
 * Timer events (RFC 3435 2.1.5). The timer 'T' is a symbol of the map like a
 * digit, at the end of an alternative or inside it ("9T11"). A 'T' in the
 * digit buffer is one timer expiry, and the matcher consumes it at a 'T'
 * token alone. The collector contract:
 *   1. the short timer runs under WAIT_SHORT and the long timer under
 *      WAIT_LONG;
 *   2. on the expiry of either timer, a DIAL verdict (a live map reload
 *      while the timer runs) dials at once, with no 'T'. Otherwise, when
 *      complete_on_timer or timer_next is set, the collector appends 'T' to
 *      the buffer and calls dialplan_match() again. It dials on
 *      complete_now, else it keeps collecting under the verdict of
 *      dp_action_of() for the new buffer;
 *   3. when neither is set, or a full buffer has no room for the 'T', the
 *      expiry keeps its own meaning: WAIT_SHORT dials, and the rest is the
 *      reorder of fsm.c;
 *   4. the number it dials or reports leaves the 'T' markers out
 *      (dialplan_digits_strip());
 *   5. an expiry that would dial an empty number is the reorder of the
 *      collector, because an alternative that starts with 'T' completes
 *      on a buffer with no digit.
 * A trailing T completes its alternative on the first expiry, so the
 * collector dials. A 'T' marker counts against DIALPLAN_DIGITS_MAX like a
 * digit.
 *
 * The '#' delimiter. A '#' inside a map alternative is matched like any
 * literal. fsm.c decides whether a dialled '#' belongs to the number by
 * asking this matcher: a '#' behind a digit that leaves no alternative alive
 * is the delimiter, and the number is the buffer in front of it. The uci option
 * digit_esc_enabled never reaches the collector; it selects the URI
 * escaping in call.c.
 */
#ifndef UVOICE_DIALPLAN_H
#define UVOICE_DIALPLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DIALPLAN_ALT_MAX	32
#define DIALPLAN_TOK_MAX	32
#define DIALPLAN_DIGITS_MAX	32

/* the decoded schema default for digit_map_base64 ("Knh4LlR8eC5U") */
#define DIALPLAN_DEFAULT_MAP	"*xx.T|x.T"

enum dp_tok_kind {
	DP_TOK_LIT = 0,			/* one literal: 0-9 A-D # * */
	DP_TOK_CLASS,			/* 'x' or a '[...]' set */
	DP_TOK_TIMER,			/* 'T' */
};

struct dp_token {
	uint8_t kind;			/* enum dp_tok_kind */
	bool star;			/* '.': zero or more of this token */
	uint16_t set;			/* DP_TOK_CLASS: bitset over 0-9,*,#,A-D
					   DP_TOK_LIT: the literal character */
};

struct dialplan {
	struct {
		struct dp_token tok[DIALPLAN_TOK_MAX];
		uint8_t count;
	} alt[DIALPLAN_ALT_MAX];
	unsigned int alts;
};

/*
 * Compile a decoded map string. Applies the normalization above, refuses any
 * other letter, refuses an empty alternative, refuses pool overflow. On
 * false the caller compiles DIALPLAN_DEFAULT_MAP instead, which cannot
 * fail; dialplan.c asserts that in a self test.
 */
bool dialplan_compile(const char *map, struct dialplan *out);

/*
 * The verdict over all alternatives for the collected digits.
 *
 * complete_now       an alternative is fully consumed by the digits.
 * complete_on_timer  a timer alone completes an alternative: its next token
 *                    is a T, and the position after that T accepts ("9T",
 *                    and "9Tx." whose tail can be empty).
 * timer_next         the next token of an alternative is a T, and more
 *                    symbols must follow it (a mid-map T, "9T11").
 * can_extend         an alternative accepts at least one more digit.
 * All four false: no alternative can ever match this buffer.
 */
struct dp_match {
	bool complete_now;
	bool complete_on_timer;
	bool can_extend;
	bool timer_next;
};

void dialplan_match(const struct dialplan *dp, const char *digits,
		    unsigned int len, struct dp_match *out);

/*
 * Copy in without its 'T' and 't' markers into out, NUL terminated, at most
 * olen - 1 characters. Answers the length of the whole stripped text, as
 * snprintf() does, so an answer of olen or more means out holds a prefix.
 */
size_t dialplan_digits_strip(const char *in, char *out, size_t olen);

/*
 * What the collector does after this digit. Each wait runs one of the two
 * interdigit timers of the config. A complete match dials at once, even when
 * another alternative could take more digits; write the short alternative
 * with a T ("911T|9xxx") when the longer one must stay reachable.
 */
enum dp_action {
	DP_ACT_REORDER = 0,		/* nothing can ever match */
	DP_ACT_DIAL,			/* an alternative is fully consumed */
	DP_ACT_WAIT_SHORT,		/* a timer alone completes an
					   alternative: interdigit_timeout */
	DP_ACT_WAIT_LONG,		/* more digits, or a mid-map T, are
					   needed: interdigit_timeout_long */
};

enum dp_action dp_action_of(const struct dp_match *m);
const char *dp_action_name(enum dp_action action);

#endif
