// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the matcher of MGCP digit maps (src/dialplan.c).
 *
 * The grammar cases follow RFC 3435 2.1.5. The normalization cases accept
 * the letter case that provisioned maps carry: 't' becomes 'T' and 'X'
 * becomes 'x'. The uppercase DTMF letters A to D stay literals. Any other
 * letter refuses the map.
 *
 * The verdict cases walk a map digit by digit. They check the four facts of
 * struct dp_match (complete_now, complete_on_timer, can_extend and
 * timer_next) and the dp_action_of() result that the collector acts on.
 *
 * The timer cases put a 'T' in the buffer for an expiry of either timer when
 * complete_on_timer or timer_next is set, as the collector contract of
 * dialplan.h asks, and read the dialed number through
 * dialplan_digits_strip().
 */
#include "dialplan.h"
#include "test_util.h"

static struct dialplan dp;

/* a map that a field push writes */
static const char field_map[] =
	"[2-9]11|[2-9]xxxxxxxxx|1[2-9]xxxxxxxxx|*55|*56|*57|*69|*72|*73"
	"|*81|*90|*98|xx.T|[*#][*#x].T|xx.#";

static bool map_compile(const char *map)
{
	return dialplan_compile(map, &dp);
}

static struct dp_match verdict(const char *digits)
{
	struct dp_match m;

	dialplan_match(&dp, digits, (unsigned int)strlen(digits), &m);

	return m;
}

#define CHECK_VERDICT4(digits, now, timer, ext, next, act)		\
	do {								\
		struct dp_match mv = verdict(digits);			\
									\
		CHECK(mv.complete_now == (now));			\
		CHECK(mv.complete_on_timer == (timer));			\
		CHECK(mv.can_extend == (ext));				\
		CHECK(mv.timer_next == (next));				\
		CHECK(dp_action_of(&mv) == (act));			\
	} while (0)

/* a map without a mid-map timer never sets timer_next */
#define CHECK_VERDICT(digits, now, timer, ext, act)			\
	CHECK_VERDICT4(digits, now, timer, ext, false, act)

static const char *alts_make(unsigned int n)
{
	static char buf[512];
	size_t len = 0;

	for (unsigned int i = 0; i < n; i++) {
		if (i)
			buf[len++] = '|';

		buf[len++] = 'x';
	}

	buf[len] = '\0';

	return buf;
}

static const char *toks_make(unsigned int n)
{
	static char buf[64];

	memset(buf, 'x', n);
	buf[n] = '\0';

	return buf;
}

/* n tokens whose last one carries the '.', which takes no token slot */
static const char *star_tail_make(unsigned int n)
{
	static char buf[64];

	memset(buf, 'x', n);
	buf[n] = '.';
	buf[n + 1] = '\0';

	return buf;
}

static void compile_accept_tests(void)
{
	CASE("DIALPLAN_DEFAULT_MAP is the decoded schema default");
	CHECK(strcmp(DIALPLAN_DEFAULT_MAP, "*xx.T|x.T") == 0);

	/* the self test dialplan.h names: the fallback map cannot fail */
	CASE("the default map compiles");
	CHECK(map_compile(DIALPLAN_DEFAULT_MAP));
	CHECK_UINT(dp.alts, 2);

	CASE("the default map first alternative is '*' x x. T");
	CHECK(map_compile(DIALPLAN_DEFAULT_MAP));
	CHECK_UINT(dp.alt[0].count, 4);
	CHECK_UINT(dp.alt[0].tok[0].kind, DP_TOK_LIT);
	CHECK_UINT(dp.alt[0].tok[0].set, '*');
	CHECK(!dp.alt[0].tok[0].star);
	CHECK_UINT(dp.alt[0].tok[1].kind, DP_TOK_CLASS);
	CHECK_UINT(dp.alt[0].tok[1].set, 0x03ff);
	CHECK(!dp.alt[0].tok[1].star);
	CHECK_UINT(dp.alt[0].tok[2].kind, DP_TOK_CLASS);
	CHECK(dp.alt[0].tok[2].star);
	CHECK_UINT(dp.alt[0].tok[3].kind, DP_TOK_TIMER);
	CHECK(!dp.alt[0].tok[3].star);

	CASE("the default map second alternative is x. T");
	CHECK(map_compile(DIALPLAN_DEFAULT_MAP));
	CHECK_UINT(dp.alt[1].count, 2);
	CHECK_UINT(dp.alt[1].tok[0].kind, DP_TOK_CLASS);
	CHECK(dp.alt[1].tok[0].star);
	CHECK_UINT(dp.alt[1].tok[1].kind, DP_TOK_TIMER);

	CASE("a NANP style map compiles with all six alternatives");
	CHECK(map_compile("0T|011x.T|[2-9]xxxxxx|1[2-9]xxxxxxxxx|911|x.#"));
	CHECK_UINT(dp.alts, 6);
	CHECK_UINT(dp.alt[0].count, 2);
	CHECK_UINT(dp.alt[1].count, 5);
	CHECK_UINT(dp.alt[2].count, 7);
	CHECK_UINT(dp.alt[3].count, 11);
	CHECK_UINT(dp.alt[4].count, 3);
	CHECK_UINT(dp.alt[5].count, 2);

	CASE("a subrange sets one bit per digit between the ends");
	CHECK(map_compile("[2-9]"));
	CHECK_UINT(dp.alt[0].tok[0].kind, DP_TOK_CLASS);
	CHECK_UINT(dp.alt[0].tok[0].set, 0x03fc);

	CASE("two subranges in one class accumulate");
	CHECK(map_compile("[0-24-6]"));
	CHECK_UINT(dp.alt[0].tok[0].set, 0x0077);

	CASE("a class mixes digits, '*', '#' and DTMF letters");
	CHECK(map_compile("[15*#AD]"));
	CHECK_UINT(dp.alt[0].tok[0].set, 0x9c22);

	CASE("a repeated class member is harmless");
	CHECK(map_compile("[550-1]"));
	CHECK_UINT(dp.alt[0].tok[0].set, 0x0023);

	CASE("a class that ends the map compiles");
	CHECK(map_compile("x[2-9]"));
	CHECK_UINT(dp.alt[0].count, 2);
	CHECK_UINT(dp.alt[0].tok[1].set, 0x03fc);

	CASE("a class that ends the last alternative compiles");
	CHECK(map_compile("x|[2-9]"));
	CHECK_UINT(dp.alts, 2);
	CHECK_UINT(dp.alt[1].tok[0].set, 0x03fc);

	CASE("a class closed right before a '|' compiles");
	CHECK(map_compile("[2-9]|x"));
	CHECK_UINT(dp.alts, 2);
	CHECK_UINT(dp.alt[0].tok[0].set, 0x03fc);

	CASE("'x' is the class of 0-9");
	CHECK(map_compile("x"));
	CHECK_UINT(dp.alt[0].tok[0].kind, DP_TOK_CLASS);
	CHECK_UINT(dp.alt[0].tok[0].set, 0x03ff);

	CASE("the DTMF letters A-D are literals");
	CHECK(map_compile("ABCD"));
	CHECK_UINT(dp.alt[0].count, 4);
	CHECK_UINT(dp.alt[0].tok[0].set, 'A');
	CHECK_UINT(dp.alt[0].tok[1].set, 'B');
	CHECK_UINT(dp.alt[0].tok[2].set, 'C');
	CHECK_UINT(dp.alt[0].tok[3].set, 'D');

	CASE("'#' and '*' are literals");
	CHECK(map_compile("*#"));
	CHECK_UINT(dp.alt[0].tok[0].kind, DP_TOK_LIT);
	CHECK_UINT(dp.alt[0].tok[0].set, '*');
	CHECK_UINT(dp.alt[0].tok[1].set, '#');

	CASE("normalization turns 't' into the timer");
	CHECK(map_compile("x.t"));
	CHECK_UINT(dp.alt[0].count, 2);
	CHECK_UINT(dp.alt[0].tok[1].kind, DP_TOK_TIMER);

	CASE("normalization turns 'X' into the wildcard");
	CHECK(map_compile("XX"));
	CHECK_UINT(dp.alt[0].tok[0].kind, DP_TOK_CLASS);
	CHECK_UINT(dp.alt[0].tok[0].set, 0x03ff);
	CHECK_UINT(dp.alt[0].tok[1].set, 0x03ff);

	CASE("the wildcard inside a class is the digit set");
	CHECK(map_compile("[x]"));
	CHECK_UINT(dp.alt[0].tok[0].kind, DP_TOK_CLASS);
	CHECK_UINT(dp.alt[0].tok[0].set, 0x03ff);

	CASE("an uppercase wildcard inside a class is the digit set");
	CHECK(map_compile("[X]"));
	CHECK_UINT(dp.alt[0].tok[0].kind, DP_TOK_CLASS);
	CHECK_UINT(dp.alt[0].tok[0].set, 0x03ff);

	CASE("a class mixes the wildcard with '*' and '#'");
	CHECK(map_compile("[*#x]"));
	CHECK_UINT(dp.alt[0].tok[0].set, 0x0fff);

	CASE("the wildcard adds nothing a subrange already set");
	CHECK(map_compile("[0-9x]"));
	CHECK_UINT(dp.alt[0].tok[0].set, 0x03ff);

	CASE("the wildcard joins a subrange and a DTMF letter in one class");
	CHECK(map_compile("[2-9xA]"));
	CHECK_UINT(dp.alt[0].tok[0].set, 0x13ff);

	CASE("a field map compiles with an uppercase wildcard");
	CHECK(map_compile("[2-9]11|[2-9]xxxxxxxxx|1[2-9]xxxxxxxxx|XX.T"));
	CHECK_UINT(dp.alts, 4);
	CHECK_UINT(dp.alt[3].count, 3);
	CHECK_UINT(dp.alt[3].tok[0].kind, DP_TOK_CLASS);
	CHECK_UINT(dp.alt[3].tok[0].set, 0x03ff);
	CHECK_UINT(dp.alt[3].tok[2].kind, DP_TOK_TIMER);

	CASE("the field value 'XXx.t' compiles to three classes and a "
	     "timer");
	CHECK(map_compile("XXx.t"));
	CHECK_UINT(dp.alt[0].count, 4);
	CHECK_UINT(dp.alt[0].tok[0].kind, DP_TOK_CLASS);
	CHECK_UINT(dp.alt[0].tok[1].kind, DP_TOK_CLASS);
	CHECK_UINT(dp.alt[0].tok[2].kind, DP_TOK_CLASS);
	CHECK(dp.alt[0].tok[2].star);
	CHECK_UINT(dp.alt[0].tok[3].kind, DP_TOK_TIMER);

	CASE("the field map compiles with all fifteen alternatives");
	CHECK(map_compile(field_map));
	CHECK_UINT(dp.alts, 15);
	/* '[*#][*#x].T' is the star string rule the field map relies on */
	CHECK_UINT(dp.alt[13].count, 3);
	CHECK_UINT(dp.alt[13].tok[0].set, 0x0c00);
	CHECK_UINT(dp.alt[13].tok[1].set, 0x0fff);
	CHECK(dp.alt[13].tok[1].star);

	CASE("the field map's longest alternative is eleven tokens");
	CHECK(map_compile(field_map));
	CHECK_UINT(dp.alt[2].count, 11);
	for (unsigned int a = 0; a < dp.alts; a++)
		CHECK(dp.alt[a].count <= 11);

	CASE("a bare 'T' alternative is legal");
	CHECK(map_compile("T"));
	CHECK_UINT(dp.alt[0].count, 1);
	CHECK_UINT(dp.alt[0].tok[0].kind, DP_TOK_TIMER);

	CASE("'.' sets the star flag on a literal");
	CHECK(map_compile("5.T"));
	CHECK_UINT(dp.alt[0].tok[0].kind, DP_TOK_LIT);
	CHECK(dp.alt[0].tok[0].star);

	CASE("'.' sets the star flag on a class");
	CHECK(map_compile("[2-9].T"));
	CHECK_UINT(dp.alt[0].tok[0].kind, DP_TOK_CLASS);
	CHECK(dp.alt[0].tok[0].star);

	CASE("DIALPLAN_ALT_MAX alternatives fit");
	CHECK(map_compile(alts_make(DIALPLAN_ALT_MAX)));
	CHECK_UINT(dp.alts, DIALPLAN_ALT_MAX);

	CASE("DIALPLAN_TOK_MAX tokens fit in one alternative");
	CHECK(map_compile(toks_make(DIALPLAN_TOK_MAX)));
	CHECK_UINT(dp.alt[0].count, DIALPLAN_TOK_MAX);
}

static void compile_refuse_tests(void)
{
	CASE("'N' is outside the RFC 3435 grammar and is refused");
	CHECK(!map_compile("N"));

	CASE("'M' is refused");
	CHECK(!map_compile("Mxx"));

	CASE("'S' is refused");
	CHECK(!map_compile("xxS"));

	/*
	 * Normalization covers 't' and 'X' only. A map that carries a lowercase
	 * a to d is refused, so the line falls back to the default map instead
	 * of dialling a DTMF letter.
	 */
	CASE("a lowercase DTMF letter is refused");
	CHECK(!map_compile("a"));

	CASE("lowercase a-d are refused as literals");
	CHECK(!map_compile("abcd"));
	CHECK(!map_compile("5b"));
	CHECK(!map_compile("xxc"));
	CHECK(!map_compile("d.T"));

	CASE("lowercase a-d are refused inside a class");
	CHECK(!map_compile("[ad]"));
	CHECK(!map_compile("[2-9a]"));
	CHECK(!map_compile("[a-d]"));

	CASE("a letter inside a class is refused");
	CHECK(!map_compile("[2-9N]"));

	/* RFC 3435 2.1.5 joins two digits, and the wildcard is neither */
	CASE("a subrange that starts at the wildcard is refused");
	CHECK(!map_compile("[x-9]"));

	CASE("a subrange that ends at the wildcard is refused");
	CHECK(!map_compile("[0-x]"));

	CASE("the timer letter inside a class is refused");
	CHECK(!map_compile("[T]"));
	CHECK(!map_compile("[xt]"));

	CASE("leading whitespace is refused");
	CHECK(!map_compile(" x"));

	CASE("embedded whitespace is refused");
	CHECK(!map_compile("x x"));

	CASE("an empty map is refused");
	CHECK(!map_compile(""));

	CASE("a NULL map is refused");
	CHECK(!dialplan_compile(NULL, &dp));

	CASE("an empty alternative is refused");
	CHECK(!map_compile("x||x"));

	CASE("a trailing '|' leaves an empty alternative and is refused");
	CHECK(!map_compile("x.T|"));

	CASE("a leading '|' is refused");
	CHECK(!map_compile("|x.T"));

	CASE("a leading '.' has no preceding construct and is refused");
	CHECK(!map_compile(".x"));

	CASE("a lone '.' is refused");
	CHECK(!map_compile("."));

	CASE("'..' repeats a repeat and is refused");
	CHECK(!map_compile("x.."));

	CASE("'.' after the timer is refused");
	CHECK(!map_compile("T."));

	CASE("an unclosed class is refused");
	CHECK(!map_compile("[2-9"));

	CASE("a lone '[' is refused");
	CHECK(!map_compile("["));

	CASE("a class cut off after the hyphen is refused");
	CHECK(!map_compile("[2-"));

	CASE("a subrange with no lower end is refused");
	CHECK(!map_compile("[-9]"));

	CASE("a bare ']' is refused");
	CHECK(!map_compile("]"));

	/* the class parser must stop at the alternative, not run into the next */
	CASE("a class that runs past a '|' is refused");
	CHECK(!map_compile("[2-9|x]"));

	CASE("a class left open at the end of the map is refused");
	CHECK(!map_compile("x|[2-9"));

	CASE("an empty class is refused");
	CHECK(!map_compile("[]"));

	CASE("a reversed subrange is refused");
	CHECK(!map_compile("[9-2]"));

	CASE("a subrange with no upper end is refused");
	CHECK(!map_compile("[2-]"));

	CASE("a nested class is refused");
	CHECK(!map_compile("[[2-3]]"));

	CASE("a class closed twice is refused");
	CHECK(!map_compile("[2-3]]"));

	/* RFC 3435 2.1.5 defines a subrange over two digits only */
	CASE("a subrange between DTMF letters is refused");
	CHECK(!map_compile("[A-D]"));

	CASE("more than DIALPLAN_ALT_MAX alternatives are refused");
	CHECK(!map_compile(alts_make(DIALPLAN_ALT_MAX + 1)));

	CASE("more than DIALPLAN_TOK_MAX tokens are refused");
	CHECK(!map_compile(toks_make(DIALPLAN_TOK_MAX + 1)));

	CASE("a refused map leaves no alternative behind");
	CHECK(!map_compile("x|N"));
	CHECK_UINT(dp.alts, 0);
}

static void default_map_tests(void)
{
	CHECK(map_compile(DIALPLAN_DEFAULT_MAP));

	/*
	 * "x.T" accepts zero digits, so the empty buffer already stands at the
	 * timer. The collector runs the start digit timer before the first
	 * digit and never asks the matcher here.
	 */
	CASE("default map: the empty buffer stands at the timer of \"x.T\"");
	CHECK_VERDICT("", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("default map: '5' is a candidate that can still grow");
	CHECK_VERDICT("5", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("default map: '55' stays a candidate");
	CHECK_VERDICT("55", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("default map: '5555555555' stays a candidate");
	CHECK_VERDICT("5555555555", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("default map: '*' needs the first x of \"*xx.T\"");
	CHECK_VERDICT("*", false, false, true, DP_ACT_WAIT_LONG);

	/*
	 * "*xx.T": the '.' repeats the second x, so the first x is mandatory
	 * and the timer is reachable from two digits on.
	 */
	CASE("default map: '*6' fills the mandatory x and reaches the timer");
	CHECK_VERDICT("*6", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("default map: '*67' stays on the repeated x");
	CHECK_VERDICT("*67", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("default map: '*6789' stays on the repeated x");
	CHECK_VERDICT("*6789", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("default map: 'A' matches no alternative");
	CHECK_VERDICT("A", false, false, false, DP_ACT_REORDER);

	CASE("default map: '#' matches no alternative");
	CHECK_VERDICT("#", false, false, false, DP_ACT_REORDER);

	CASE("default map: '*A' dies on the wildcard");
	CHECK_VERDICT("*A", false, false, false, DP_ACT_REORDER);

	CASE("default map: '**' dies on the wildcard");
	CHECK_VERDICT("**", false, false, false, DP_ACT_REORDER);
}

static void emergency_map_tests(void)
{
	CHECK(map_compile("112|911|9xxT"));

	/* 112 and 911 stay ordinary entries in the same map. */
	CASE("emergency map: '112' dials immediately as an ordinary entry");
	CHECK_VERDICT("112", true, false, false, DP_ACT_DIAL);

	CHECK(map_compile("911|9xxT"));
	CASE("emergency map: '112' has no treatment without an entry");
	CHECK_VERDICT("112", false, false, false, DP_ACT_REORDER);

	CASE("emergency map: '9' has no candidate yet");
	CHECK_VERDICT("9", false, false, true, DP_ACT_WAIT_LONG);

	CASE("emergency map: '91' has no candidate yet");
	CHECK_VERDICT("91", false, false, true, DP_ACT_WAIT_LONG);

	/*
	 * "911" is complete and nothing can extend it: "9xxT" stands at its
	 * timer and a timer consumes no digit. The emergency number dials at
	 * once instead of waiting for the interdigit timer.
	 */
	CASE("emergency map: '911' dials immediately although \"9xxT\" would wait");
	CHECK_VERDICT("911", true, true, false, DP_ACT_DIAL);

	CASE("emergency map: '912' only reaches the timer of \"9xxT\"");
	CHECK_VERDICT("912", false, true, false, DP_ACT_WAIT_SHORT);

	CASE("emergency map: '9112' is past every alternative");
	CHECK_VERDICT("9112", false, false, false, DP_ACT_REORDER);

	CASE("emergency map: 'A' matches nothing");
	CHECK_VERDICT("A", false, false, false, DP_ACT_REORDER);
}

static void nanp_map_tests(void)
{
	CHECK(map_compile("1[2-9]xxxxxxxxx|[2-9]xxxxxx"));

	CASE("NANP map: '2' opens the seven digit alternative only");
	CHECK_VERDICT("2", false, false, true, DP_ACT_WAIT_LONG);

	CASE("NANP map: '234567' is one digit short");
	CHECK_VERDICT("234567", false, false, true, DP_ACT_WAIT_LONG);

	CASE("NANP map: '2345678' is complete and cannot grow");
	CHECK_VERDICT("2345678", true, false, false, DP_ACT_DIAL);

	CASE("NANP map: '23456789' is past every alternative");
	CHECK_VERDICT("23456789", false, false, false, DP_ACT_REORDER);

	CASE("NANP map: '1234567' walks the eleven digit alternative");
	CHECK_VERDICT("1234567", false, false, true, DP_ACT_WAIT_LONG);

	CASE("NANP map: '1234567890' is one digit short");
	CHECK_VERDICT("1234567890", false, false, true, DP_ACT_WAIT_LONG);

	CASE("NANP map: '12345678901' is complete and cannot grow");
	CHECK_VERDICT("12345678901", true, false, false, DP_ACT_DIAL);

	CASE("NANP map: '11' fails the [2-9] area code");
	CHECK_VERDICT("11", false, false, false, DP_ACT_REORDER);

	CASE("NANP map: '0' matches no alternative");
	CHECK_VERDICT("0", false, false, false, DP_ACT_REORDER);
}

static void terminator_map_tests(void)
{
	CHECK(map_compile("x.#"));

	CASE("'x.#' map: '5' still needs the '#' literal");
	CHECK_VERDICT("5", false, false, true, DP_ACT_WAIT_LONG);

	CASE("'x.#' map: '5#' completes on the literal '#'");
	CHECK_VERDICT("5#", true, false, false, DP_ACT_DIAL);

	CASE("'x.#' map: a bare '#' completes with zero digits");
	CHECK_VERDICT("#", true, false, false, DP_ACT_DIAL);

	CASE("'x.#' map: '1234567890#' completes after the repeat");
	CHECK_VERDICT("1234567890#", true, false, false, DP_ACT_DIAL);

	CASE("'x.#' map: '5#5' is past the alternative");
	CHECK_VERDICT("5#5", false, false, false, DP_ACT_REORDER);
}

static void timer_map_tests(void)
{
	CHECK(map_compile("0T"));

	CASE("'0T' map: the empty buffer needs the '0'");
	CHECK_VERDICT("", false, false, true, DP_ACT_WAIT_LONG);

	CASE("'0T' map: '0' dials when the short timer expires");
	CHECK_VERDICT("0", false, true, false, DP_ACT_WAIT_SHORT);

	CASE("'0T' map: '00' is past the alternative");
	CHECK_VERDICT("00", false, false, false, DP_ACT_REORDER);

	CASE("'0T' map: '1' matches nothing");
	CHECK_VERDICT("1", false, false, false, DP_ACT_REORDER);

	CHECK(map_compile("T"));

	CASE("'T' map: the empty buffer is complete on the timer");
	CHECK_VERDICT("", false, true, false, DP_ACT_WAIT_SHORT);

	CASE("'T' map: any digit is past the alternative");
	CHECK_VERDICT("5", false, false, false, DP_ACT_REORDER);
}

static void star_walk_tests(void)
{
	CHECK(map_compile("5.T"));

	CASE("'5.T' map: zero occurrences already reach the timer");
	CHECK_VERDICT("", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("'5.T' map: '5' repeats the literal");
	CHECK_VERDICT("5", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("'5.T' map: '55555' repeats the literal");
	CHECK_VERDICT("55555", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("'5.T' map: '56' leaves the literal");
	CHECK_VERDICT("56", false, false, false, DP_ACT_REORDER);

	CHECK(map_compile("[2-9].T"));

	CASE("'[2-9].T' map: zero occurrences already reach the timer");
	CHECK_VERDICT("", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("'[2-9].T' map: '2' stays inside the class");
	CHECK_VERDICT("2", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("'[2-9].T' map: '299' stays inside the class");
	CHECK_VERDICT("299", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("'[2-9].T' map: '20' leaves the class");
	CHECK_VERDICT("20", false, false, false, DP_ACT_REORDER);

	/*
	 * A greedy star with no backtrack would eat all three digits and miss
	 * the trailing literal. The position set keeps both readings alive.
	 */
	CHECK(map_compile("x.5"));

	CASE("'x.5' map: '555' is complete and dials at once");
	CHECK_VERDICT("555", true, false, true, DP_ACT_DIAL);

	CASE("'x.5' map: '123' is not complete yet");
	CHECK_VERDICT("123", false, false, true, DP_ACT_WAIT_LONG);

	CASE("'x.5' map: '1235' is complete and dials at once");
	CHECK_VERDICT("1235", true, false, true, DP_ACT_DIAL);
}

/*
 * Two starred constructs in a row, from the empty buffer on. The skip edge of
 * the first must reach the second. The skip edge of the second must reach
 * what follows it.
 */
static void star_chain_tests(void)
{
	CASE("two starred wildcards compile to two starred tokens");
	CHECK(map_compile("x.x.T"));
	CHECK_UINT(dp.alt[0].count, 3);
	CHECK(dp.alt[0].tok[0].star);
	CHECK(dp.alt[0].tok[1].star);
	CHECK_UINT(dp.alt[0].tok[2].kind, DP_TOK_TIMER);

	CASE("'x.x.T' map: the empty buffer reaches the timer through both stars");
	CHECK_VERDICT("", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("'x.x.T' map: '5' stays inside the chain");
	CHECK_VERDICT("5", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("'x.x.T' map: '12345' stays inside the chain");
	CHECK_VERDICT("12345", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("'x.x.T' map: 'A' matches neither star");
	CHECK_VERDICT("A", false, false, false, DP_ACT_REORDER);

	CHECK(map_compile("[2-9].x.T"));

	CASE("'[2-9].x.T' map: the empty buffer reaches the timer");
	CHECK_VERDICT("", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("'[2-9].x.T' map: '2' can still take either star");
	CHECK_VERDICT("2", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("'[2-9].x.T' map: '0' skips the class star and enters the wildcard");
	CHECK_VERDICT("0", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("'[2-9].x.T' map: '20' leaves the class star behind");
	CHECK_VERDICT("20", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("'[2-9].x.T' map: '022' never returns to the class star");
	CHECK_VERDICT("022", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("'[2-9].x.T' map: '#' matches neither star");
	CHECK_VERDICT("#", false, false, false, DP_ACT_REORDER);

	/* the same chain with a mandatory tail, so the verdicts separate */
	CHECK(map_compile("[2-9].x.#"));

	CASE("'[2-9].x.#' map: the empty buffer still needs the '#'");
	CHECK_VERDICT("", false, false, true, DP_ACT_WAIT_LONG);

	CASE("'[2-9].x.#' map: a bare '#' skips both stars");
	CHECK_VERDICT("#", true, false, false, DP_ACT_DIAL);

	CASE("'[2-9].x.#' map: '20#' walks both stars");
	CHECK_VERDICT("20#", true, false, false, DP_ACT_DIAL);

	CASE("'[2-9].x.#' map: '02#' takes the wildcard star twice");
	CHECK_VERDICT("02#", true, false, false, DP_ACT_DIAL);

	CASE("'[2-9].x.#' map: '2A#' dies on the letter");
	CHECK_VERDICT("2A#", false, false, false, DP_ACT_REORDER);
}

/*
 * The position set is a 64 bit word. The accept position of a full
 * alternative is bit DIALPLAN_TOK_MAX, so these cases walk the last token
 * slot, which is the boundary.
 */
static void tok_cap_star_tests(void)
{
	char digits[DIALPLAN_DIGITS_MAX + 16];
	/* sized for the compiler's view of toks_make(), not the runtime length:
	 * -Os turns the format-truncation analysis into an error */
	char map[DIALPLAN_TOK_MAX * 2 + 16];
	struct dp_match m;

	memset(digits, '5', sizeof(digits) - 1);
	digits[sizeof(digits) - 1] = '\0';

	CASE("a '.' on the last token takes no token slot");
	CHECK(map_compile(star_tail_make(DIALPLAN_TOK_MAX)));
	CHECK_UINT(dp.alts, 1);
	CHECK_UINT(dp.alt[0].count, DIALPLAN_TOK_MAX);
	CHECK(dp.alt[0].tok[DIALPLAN_TOK_MAX - 1].star);

	CASE("starred last token: 30 digits cannot complete yet");
	dialplan_match(&dp, digits, DIALPLAN_TOK_MAX - 2, &m);
	CHECK(!m.complete_now);
	CHECK(!m.complete_on_timer);
	CHECK(m.can_extend);
	CHECK(dp_action_of(&m) == DP_ACT_WAIT_LONG);

	CASE("starred last token: 31 digits set the accept bit at position 32");
	dialplan_match(&dp, digits, DIALPLAN_TOK_MAX - 1, &m);
	CHECK(m.complete_now);
	CHECK(m.can_extend);
	CHECK(dp_action_of(&m) == DP_ACT_DIAL);

	CASE("starred last token: a full buffer keeps the accept bit");
	dialplan_match(&dp, digits, DIALPLAN_DIGITS_MAX, &m);
	CHECK(m.complete_now);
	CHECK(m.can_extend);
	CHECK(dp_action_of(&m) == DP_ACT_DIAL);

	snprintf(map, sizeof(map), "%sT", toks_make(DIALPLAN_TOK_MAX - 1));

	CASE("a timer in the last token slot compiles");
	CHECK(map_compile(map));
	CHECK_UINT(dp.alt[0].count, DIALPLAN_TOK_MAX);
	CHECK_UINT(dp.alt[0].tok[DIALPLAN_TOK_MAX - 1].kind, DP_TOK_TIMER);

	CASE("a timer in the last token slot is reachable at 31 digits");
	dialplan_match(&dp, digits, DIALPLAN_TOK_MAX - 1, &m);
	CHECK(!m.complete_now);
	CHECK(m.complete_on_timer);
	CHECK(!m.can_extend);
	CHECK(dp_action_of(&m) == DP_ACT_WAIT_SHORT);

	CASE("a timer in the last token slot needs all 31 digits");
	dialplan_match(&dp, digits, DIALPLAN_TOK_MAX - 2, &m);
	CHECK(!m.complete_on_timer);
	CHECK(m.can_extend);
	CHECK(dp_action_of(&m) == DP_ACT_WAIT_LONG);
}

static void digit_cap_tests(void)
{
	char digits[DIALPLAN_DIGITS_MAX + 16];
	struct dp_match m;

	CHECK(map_compile(toks_make(DIALPLAN_TOK_MAX)));
	memset(digits, '5', sizeof(digits) - 1);
	digits[sizeof(digits) - 1] = '\0';

	CASE("a buffer one digit short of the cap can still grow");
	dialplan_match(&dp, digits, DIALPLAN_DIGITS_MAX - 1, &m);
	CHECK(!m.complete_now);
	CHECK(!m.complete_on_timer);
	CHECK(m.can_extend);
	CHECK(dp_action_of(&m) == DP_ACT_WAIT_LONG);

	CASE("a buffer at the cap completes the 32 token alternative");
	dialplan_match(&dp, digits, DIALPLAN_DIGITS_MAX, &m);
	CHECK(m.complete_now);
	CHECK(!m.can_extend);
	CHECK(dp_action_of(&m) == DP_ACT_DIAL);

	CASE("a length beyond the cap is clamped to the cap");
	dialplan_match(&dp, digits, DIALPLAN_DIGITS_MAX + 15, &m);
	CHECK(m.complete_now);
	CHECK(!m.can_extend);
	CHECK(dp_action_of(&m) == DP_ACT_DIAL);

	CASE("a NULL digit buffer is read as zero digits");
	CHECK(map_compile("x.T"));
	dialplan_match(&dp, NULL, 5, &m);
	CHECK(!m.complete_now);
	CHECK(m.complete_on_timer);
	CHECK(m.can_extend);
}

static void action_tests(void)
{
	struct dp_match m = { false, false, false, false };

	CASE("no fact at all is a reorder");
	CHECK(dp_action_of(&m) == DP_ACT_REORDER);
	CHECK(strcmp(dp_action_name(DP_ACT_REORDER), "reorder") == 0);

	CASE("complete with no extension dials");
	m.complete_now = true;
	CHECK(dp_action_of(&m) == DP_ACT_DIAL);
	CHECK(strcmp(dp_action_name(DP_ACT_DIAL), "dial") == 0);

	CASE("a complete match dials even when an alternative could still grow");
	m.can_extend = true;
	CHECK(dp_action_of(&m) == DP_ACT_DIAL);

	CASE("a complete match dials over a reachable timer");
	m.complete_on_timer = true;
	m.can_extend = false;
	CHECK(dp_action_of(&m) == DP_ACT_DIAL);

	CASE("a complete match dials over a timer and an open alternative");
	m.can_extend = true;
	CHECK(dp_action_of(&m) == DP_ACT_DIAL);

	CASE("a timer candidate waits on the short timer");
	m.complete_now = false;
	m.complete_on_timer = true;
	m.can_extend = false;
	CHECK(dp_action_of(&m) == DP_ACT_WAIT_SHORT);
	CHECK(strcmp(dp_action_name(DP_ACT_WAIT_SHORT), "wait-short") == 0);

	CASE("no candidate yet waits on the long timer");
	m.complete_on_timer = false;
	m.can_extend = true;
	CHECK(dp_action_of(&m) == DP_ACT_WAIT_LONG);
	CHECK(strcmp(dp_action_name(DP_ACT_WAIT_LONG), "wait-long") == 0);

	CASE("complete on the timer and still growing waits on the short timer");
	m.complete_on_timer = true;
	m.can_extend = true;
	CHECK(dp_action_of(&m) == DP_ACT_WAIT_SHORT);
}

static void multi_alt_tests(void)
{
	CHECK(map_compile("0T|011x.T|[2-9]xxxxxx|1[2-9]xxxxxxxxx|911|x.#"));

	/* "011x.T" accepts zero x, so the timer is reachable from "011" on */
	CASE("full NANP map: '011' already reaches the international timer");
	CHECK_VERDICT("011", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("full NANP map: '0114930' reaches the international timer");
	CHECK_VERDICT("0114930", false, true, true, DP_ACT_WAIT_SHORT);

	/* the complete "911" wins; the "x.#" reading of it is unreachable */
	CASE("full NANP map: '911' is complete and dials at once");
	CHECK_VERDICT("911", true, false, true, DP_ACT_DIAL);

	CASE("full NANP map: '0' waits on the short timer for the operator");
	CHECK_VERDICT("0", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("full NANP map: '5551234' completes and dials at once");
	CHECK_VERDICT("5551234", true, false, true, DP_ACT_DIAL);

	CASE("full NANP map: '5551234#' terminates on the '#' literal");
	CHECK_VERDICT("5551234#", true, false, false, DP_ACT_DIAL);

	CASE("full NANP map: 'A' matches nothing");
	CHECK_VERDICT("A", false, false, false, DP_ACT_REORDER);
}

/* the operator acceptance matrix, one verdict per dialed string */
static void expert_map_tests(void)
{
	CHECK(map_compile("*xx.T|x.T|911|411|[2-9]xxxxxx|1xxxxxxxxxx"
			  "|011x.T|*81|*83*|*84#|#85#|*72x.T"));

	CASE("expert map: '*82' waits for the star code timer");
	CHECK_VERDICT("*82", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("expert map: '1234' waits for the open number timer");
	CHECK_VERDICT("1234", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("expert map: '0111234567890' waits for the international timer");
	CHECK_VERDICT("0111234567890", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("expert map: '*72012345' waits for the forward target timer");
	CHECK_VERDICT("*72012345", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("expert map: '411' dials at once over the open alternatives");
	CHECK_VERDICT("411", true, true, true, DP_ACT_DIAL);

	CASE("expert map: '7770005' dials at once on the seven digit rule");
	CHECK_VERDICT("7770005", true, true, true, DP_ACT_DIAL);

	CASE("expert map: '15552345678' dials at once on the NANP rule");
	CHECK_VERDICT("15552345678", true, true, true, DP_ACT_DIAL);

	CASE("expert map: '*81' dials at once over '*xx.T'");
	CHECK_VERDICT("*81", true, true, true, DP_ACT_DIAL);

	CASE("expert map: '*83*' dials at once on its trailing star");
	CHECK_VERDICT("*83*", true, false, false, DP_ACT_DIAL);

	CASE("expert map: '*84#' dials at once with the '#' in the number");
	CHECK_VERDICT("*84#", true, false, false, DP_ACT_DIAL);

	CASE("expert map: '#85#' dials at once with both '#' in the number");
	CHECK_VERDICT("#85#", true, false, false, DP_ACT_DIAL);

	/* the collector reads this reorder as the '#' delimiter (fsm.c) */
	CASE("expert map: '1234#' leaves no alternative alive");
	CHECK_VERDICT("1234#", false, false, false, DP_ACT_REORDER);

	CASE("expert map: a bare '#' can still become '#85#'");
	CHECK_VERDICT("#", false, false, true, DP_ACT_WAIT_LONG);

	CASE("expert map: '#9' kills the last '#' alternative");
	CHECK_VERDICT("#9", false, false, false, DP_ACT_REORDER);
}

/*
 * The field map, one verdict per dialed string. Two points belong to
 * the map and not to the matcher. First, '*5' reaches the star string timer,
 * so '*55' needs its third key inside the short window. Second, a single
 * digit dials on the timer of 'xx.T'.
 */
static void field_map_tests(void)
{
	CHECK(map_compile(field_map));

	CASE("field map: '911' dials at once on the [2-9]11 rule");
	CHECK_VERDICT("911", true, true, true, DP_ACT_DIAL);

	CASE("field map: '411' dials at once on the same rule");
	CHECK_VERDICT("411", true, true, true, DP_ACT_DIAL);

	CASE("field map: '2125551234' dials at once on the ten digit rule");
	CHECK_VERDICT("2125551234", true, true, true, DP_ACT_DIAL);

	CASE("field map: '12125551234' dials at once on the eleven digit "
	     "rule");
	CHECK_VERDICT("12125551234", true, true, true, DP_ACT_DIAL);

	CASE("field map: '*72' dials at once as a network star code");
	CHECK_VERDICT("*72", true, true, true, DP_ACT_DIAL);

	CASE("field map: '*723' only reaches the star string timer, so the "
	     "collector never asks");
	CHECK_VERDICT("*723", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("field map: '*5' waits on the short timer, not the long one");
	CHECK_VERDICT("*5", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("field map: '*55' dials at once over the star string "
	     "alternative");
	CHECK_VERDICT("*55", true, true, true, DP_ACT_DIAL);

	CASE("field map: a bare '*' already reaches the star string timer");
	CHECK_VERDICT("*", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("field map: a bare '#' already reaches the star string timer");
	CHECK_VERDICT("#", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("field map: '##' stays inside the star string");
	CHECK_VERDICT("##", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("field map: '55#' dials at once on the 'xx.#' rule");
	CHECK_VERDICT("55#", true, false, false, DP_ACT_DIAL);

	CASE("field map: '5551234' waits on the short timer of 'xx.T'");
	CHECK_VERDICT("5551234", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("field map: '0' waits on the short timer of 'xx.T'");
	CHECK_VERDICT("0", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("field map: 'A' matches no alternative");
	CHECK_VERDICT("A", false, false, false, DP_ACT_REORDER);

	CASE("field map: '5*' leaves no alternative alive");
	CHECK_VERDICT("5*", false, false, false, DP_ACT_REORDER);
}

/* the service code map shapes from the operator acceptance matrix */
static void service_map_tests(void)
{
	CHECK(map_compile("*xx*.x.#.T"));

	CASE("service map '*xx*.x.#.T': '*38' waits on the timer");
	CHECK_VERDICT("*38", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("service map '*xx*.x.#.T': '*38*' waits on the timer");
	CHECK_VERDICT("*38*", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("service map '*xx*.x.#.T': '*39*123' waits on the timer");
	CHECK_VERDICT("*39*123", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("service map '*xx*.x.#.T': '*61*123#' waits on the timer");
	CHECK_VERDICT("*61*123#", false, true, true, DP_ACT_WAIT_SHORT);

	CHECK(map_compile("*03*xx.*xx.*xx.#"));

	CASE("service map '*03*...#': the full code dials at once");
	CHECK_VERDICT("*03*12*34*56#", true, false, false, DP_ACT_DIAL);

	CASE("service map '*03*...#': without the '#' it can only grow");
	CHECK_VERDICT("*03*12*34*56", false, false, true, DP_ACT_WAIT_LONG);

	CHECK(map_compile("*#xx*x.#"));

	CASE("service map '*#xx*x.#': the full code dials at once");
	CHECK_VERDICT("*#61*22#", true, false, false, DP_ACT_DIAL);

	CASE("service map '*#xx*x.#': without the '#' it can only grow");
	CHECK_VERDICT("*#61*22", false, false, true, DP_ACT_WAIT_LONG);

	CHECK(map_compile("#xx*.x.T"));

	CASE("service map '#xx*.x.T': '#38' waits on the timer");
	CHECK_VERDICT("#38", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("service map '#xx*.x.T': '#33*123' waits on the timer");
	CHECK_VERDICT("#33*123", false, true, true, DP_ACT_WAIT_SHORT);
}

/* RFC 3435 2.1.5: the timer is a symbol of the map, at any position */
static void mid_map_timer_compile_tests(void)
{
	CASE("a mid-map timer compiles: '9T11'");
	CHECK(map_compile("9T11"));
	CHECK_UINT(dp.alt[0].count, 4);
	CHECK_UINT(dp.alt[0].tok[0].set, '9');
	CHECK_UINT(dp.alt[0].tok[1].kind, DP_TOK_TIMER);
	CHECK_UINT(dp.alt[0].tok[2].set, '1');
	CHECK_UINT(dp.alt[0].tok[3].set, '1');

	CASE("a mid-map timer compiles: 'x.T#'");
	CHECK(map_compile("x.T#"));
	CHECK_UINT(dp.alt[0].count, 3);
	CHECK(dp.alt[0].tok[0].star);
	CHECK_UINT(dp.alt[0].tok[1].kind, DP_TOK_TIMER);
	CHECK(!dp.alt[0].tok[1].star);
	CHECK_UINT(dp.alt[0].tok[2].kind, DP_TOK_LIT);
	CHECK_UINT(dp.alt[0].tok[2].set, '#');

	CASE("a mid-map timer compiles: '[2-9]xxT1'");
	CHECK(map_compile("[2-9]xxT1"));
	CHECK_UINT(dp.alt[0].count, 5);
	CHECK_UINT(dp.alt[0].tok[0].set, 0x03fc);
	CHECK_UINT(dp.alt[0].tok[3].kind, DP_TOK_TIMER);
	CHECK_UINT(dp.alt[0].tok[4].set, '1');

	CASE("a digit after the timer compiles");
	CHECK(map_compile("Tx"));
	CHECK_UINT(dp.alt[0].count, 2);
	CHECK_UINT(dp.alt[0].tok[0].kind, DP_TOK_TIMER);
	CHECK_UINT(dp.alt[0].tok[1].kind, DP_TOK_CLASS);

	CASE("a literal after the timer compiles");
	CHECK(map_compile("xT5"));
	CHECK_UINT(dp.alt[0].count, 3);
	CHECK_UINT(dp.alt[0].tok[1].kind, DP_TOK_TIMER);
	CHECK_UINT(dp.alt[0].tok[2].set, '5');

	CASE("two timers in a row compile");
	CHECK(map_compile("TT"));
	CHECK_UINT(dp.alt[0].count, 2);
	CHECK_UINT(dp.alt[0].tok[0].kind, DP_TOK_TIMER);
	CHECK_UINT(dp.alt[0].tok[1].kind, DP_TOK_TIMER);

	CASE("a lowercase mid-map timer compiles to the timer");
	CHECK(map_compile("9t11"));
	CHECK_UINT(dp.alt[0].tok[1].kind, DP_TOK_TIMER);

	CASE("a mid-map timer in one alternative leaves the others alone");
	CHECK(map_compile("911T|9T11|9xxx"));
	CHECK_UINT(dp.alts, 3);
	CHECK_UINT(dp.alt[0].count, 4);
	CHECK_UINT(dp.alt[0].tok[3].kind, DP_TOK_TIMER);
	CHECK_UINT(dp.alt[1].count, 4);
	CHECK_UINT(dp.alt[1].tok[1].kind, DP_TOK_TIMER);
	CHECK_UINT(dp.alt[2].count, 4);
}

/*
 * A 'T' in the buffer is one timer expiry (dialplan.h). A mid-map T sets
 * timer_next and runs the long timer (RFC 3660 2.2 Tpar, NCS 1.5 7.1.5).
 */
static void mid_map_timer_match_tests(void)
{
	CHECK(map_compile("9T11"));

	CASE("'9T11' map: the empty buffer needs the '9'");
	CHECK_VERDICT("", false, false, true, DP_ACT_WAIT_LONG);

	CASE("'9T11' map: '9' expects the mid-map timer and waits long");
	CHECK_VERDICT4("9", false, false, false, true, DP_ACT_WAIT_LONG);

	CASE("'9T11' map: '9T' consumes the timer token");
	CHECK_VERDICT("9T", false, false, true, DP_ACT_WAIT_LONG);

	CASE("'9T11' map: '9T1' walks past the timer");
	CHECK_VERDICT("9T1", false, false, true, DP_ACT_WAIT_LONG);

	CASE("'9T11' map: '9T11' is complete");
	CHECK_VERDICT("9T11", true, false, false, DP_ACT_DIAL);

	CASE("'9T11' map: a lowercase 't' in the buffer is the timer too");
	CHECK_VERDICT("9t11", true, false, false, DP_ACT_DIAL);

	CASE("'9T11' map: a digit where the timer belongs is a reorder");
	CHECK_VERDICT("91", false, false, false, DP_ACT_REORDER);

	CASE("'9T11' map: a second timer is unexpected and a reorder");
	CHECK_VERDICT("9TT", false, false, false, DP_ACT_REORDER);

	CASE("'9T11' map: a timer after the match is past the alternative");
	CHECK_VERDICT("9T11T", false, false, false, DP_ACT_REORDER);

	CHECK(map_compile("x.T#"));

	CASE("'x.T#' map: '55' can grow or take the timer");
	CHECK_VERDICT4("55", false, false, true, true, DP_ACT_WAIT_LONG);

	CASE("'x.T#' map: '55T' needs the '#'");
	CHECK_VERDICT("55T", false, false, true, DP_ACT_WAIT_LONG);

	CASE("'x.T#' map: '55T#' is complete");
	CHECK_VERDICT("55T#", true, false, false, DP_ACT_DIAL);

	CASE("'x.T#' map: '55#' skips the timer and is a reorder");
	CHECK_VERDICT("55#", false, false, false, DP_ACT_REORDER);

	CASE("'x.T#' map: '55T5' leaves the repeat behind the timer");
	CHECK_VERDICT("55T5", false, false, false, DP_ACT_REORDER);

	CHECK(map_compile("[2-9]xxT1"));

	CASE("'[2-9]xxT1' map: '234' expects the timer");
	CHECK_VERDICT4("234", false, false, false, true, DP_ACT_WAIT_LONG);

	CASE("'[2-9]xxT1' map: '234T' needs the last digit");
	CHECK_VERDICT("234T", false, false, true, DP_ACT_WAIT_LONG);

	CASE("'[2-9]xxT1' map: '234T1' is complete");
	CHECK_VERDICT("234T1", true, false, false, DP_ACT_DIAL);

	CASE("'[2-9]xxT1' map: '2345' is a reorder");
	CHECK_VERDICT("2345", false, false, false, DP_ACT_REORDER);

	CHECK(map_compile("911T|9T11|9xxx"));

	CASE("mixed map: '9' takes the timer of '9T11' or grows '9xxx'");
	CHECK_VERDICT4("9", false, false, true, true, DP_ACT_WAIT_LONG);

	CASE("mixed map: '911' waits on the trailing timer of '911T'");
	CHECK_VERDICT("911", false, true, true, DP_ACT_WAIT_SHORT);

	CASE("mixed map: '911T' dials on the trailing timer");
	CHECK_VERDICT("911T", true, false, false, DP_ACT_DIAL);

	CASE("mixed map: '9T' leaves '9T11' alone");
	CHECK_VERDICT("9T", false, false, true, DP_ACT_WAIT_LONG);

	CHECK(map_compile("911T|9T11"));

	CASE("'911T|9T11' map: '9' is a mid-map timer, not a completing one");
	CHECK_VERDICT4("9", false, false, true, true, DP_ACT_WAIT_LONG);

	CASE("'911T|9T11' map: '911' completes on the timer alone");
	CHECK_VERDICT4("911", false, true, false, false, DP_ACT_WAIT_SHORT);

	CHECK(map_compile("TT"));

	CASE("'TT' map: the empty buffer expects a mid-map timer");
	CHECK_VERDICT4("", false, false, false, true, DP_ACT_WAIT_LONG);

	CASE("'TT' map: one timer is not enough");
	CHECK_VERDICT("T", false, true, false, DP_ACT_WAIT_SHORT);

	CASE("'TT' map: two timers complete it");
	CHECK_VERDICT("TT", true, false, false, DP_ACT_DIAL);

	/* RFC 3660 2.2: a starred tail can be empty, so the timer is enough */
	CHECK(map_compile("9Tx."));

	CASE("'9Tx.' map: '9' completes on the timer alone");
	CHECK_VERDICT4("9", false, true, false, false, DP_ACT_WAIT_SHORT);

	CASE("'9Tx.' map: '9T' is complete and can grow");
	CHECK_VERDICT4("9T", true, false, true, false, DP_ACT_DIAL);

	CHECK(map_compile("9T[2-9]"));

	CASE("'9T[2-9]' map: '9' expects a mid-map timer");
	CHECK_VERDICT4("9", false, false, false, true, DP_ACT_WAIT_LONG);
}

/* a map with no timer at all: a 'T' in the buffer matches nothing */
static void timer_unexpected_tests(void)
{
	CHECK(map_compile("1[2-9]xxxxxxxxx|[2-9]xxxxxx"));

	CASE("a 'T' where no alternative has a timer answers REORDER");
	CHECK_VERDICT("555T", false, false, false, DP_ACT_REORDER);

	CASE("a bare 'T' where no alternative has a timer answers REORDER");
	CHECK_VERDICT("T", false, false, false, DP_ACT_REORDER);

	CHECK(map_compile("0T"));

	CASE("'0T' map: a second 'T' is past the alternative");
	CHECK_VERDICT("0TT", false, false, false, DP_ACT_REORDER);

	CASE("'0T' map: a 'T' before the '0' is a reorder");
	CHECK_VERDICT("T0", false, false, false, DP_ACT_REORDER);
}

/*
 * The collector of dialplan.h: append a digit and act on the verdict of the
 * whole buffer. On the expiry of either timer it appends a 'T' when the last
 * verdict expects one, and otherwise the expiry keeps its own meaning.
 */
struct collector {
	char buf[DIALPLAN_DIGITS_MAX + 1];
	unsigned int n;
	struct dp_match m;
};

static enum dp_action collect_push(struct collector *c, char ev)
{
	c->buf[c->n++] = ev;
	c->buf[c->n] = '\0';
	dialplan_match(&dp, c->buf, c->n, &c->m);

	return dp_action_of(&c->m);
}

/* false: no 'T' was appended, the expiry keeps its own meaning */
static bool collect_expire(struct collector *c, enum dp_action *act)
{
	if (!c->m.complete_on_timer && !c->m.timer_next)
		return false;

	*act = collect_push(c, 'T');

	return true;
}

static void collector_tests(void)
{
	struct collector c = { .n = 0 };
	char number[DIALPLAN_DIGITS_MAX + 1];
	enum dp_action act = DP_ACT_REORDER;

	CASE("collector: digits, a long expiry, more digits, then the dial");
	CHECK(map_compile("9T11|9xxx"));
	c.n = 0;
	CHECK(collect_push(&c, '9') == DP_ACT_WAIT_LONG);
	CHECK(c.m.timer_next);
	CHECK(collect_expire(&c, &act));
	CHECK(act == DP_ACT_WAIT_LONG);
	CHECK(collect_push(&c, '1') == DP_ACT_WAIT_LONG);
	CHECK(collect_push(&c, '1') == DP_ACT_DIAL);
	CHECK(strcmp(c.buf, "9T11") == 0);
	CHECK_UINT(dialplan_digits_strip(c.buf, number, sizeof(number)), 3);
	CHECK(strcmp(number, "911") == 0);

	CASE("collector: a digit inside the long window takes the other path");
	c.n = 0;
	CHECK(collect_push(&c, '9') == DP_ACT_WAIT_LONG);
	CHECK(collect_push(&c, '5') == DP_ACT_WAIT_LONG);
	CHECK(collect_push(&c, '5') == DP_ACT_WAIT_LONG);
	CHECK(collect_push(&c, '5') == DP_ACT_DIAL);
	CHECK(strcmp(c.buf, "9555") == 0);

	CASE("collector: an expiry that completes another alternative dials");
	CHECK(map_compile("9T11|x.T"));
	c.n = 0;
	CHECK(collect_push(&c, '9') == DP_ACT_WAIT_SHORT);
	CHECK(collect_expire(&c, &act));
	CHECK(act == DP_ACT_DIAL);
	CHECK_UINT(dialplan_digits_strip(c.buf, number, sizeof(number)), 1);
	CHECK(strcmp(number, "9") == 0);

	CASE("collector: a trailing T dials on the first expiry");
	CHECK(map_compile("0T|011x.T"));
	c.n = 0;
	CHECK(collect_push(&c, '0') == DP_ACT_WAIT_SHORT);
	CHECK(collect_expire(&c, &act));
	CHECK(act == DP_ACT_DIAL);
	CHECK_UINT(dialplan_digits_strip(c.buf, number, sizeof(number)), 1);
	CHECK(strcmp(number, "0") == 0);

	CASE("collector: the default map dials on the first expiry");
	CHECK(map_compile(DIALPLAN_DEFAULT_MAP));
	c.n = 0;
	CHECK(collect_push(&c, '5') == DP_ACT_WAIT_SHORT);
	CHECK(collect_push(&c, '5') == DP_ACT_WAIT_SHORT);
	CHECK(collect_expire(&c, &act));
	CHECK(act == DP_ACT_DIAL);
	CHECK_UINT(dialplan_digits_strip(c.buf, number, sizeof(number)), 2);
	CHECK(strcmp(number, "55") == 0);

	CASE("collector: a long expiry with no timer expected appends nothing");
	CHECK(map_compile("1[2-9]xxxxxxxxx|[2-9]xxxxxx"));
	c.n = 0;
	CHECK(collect_push(&c, '5') == DP_ACT_WAIT_LONG);
	CHECK(!collect_expire(&c, &act));
	CHECK(strcmp(c.buf, "5") == 0);
}

/* dp_action_of() with the fourth fact */
static void action_timer_next_tests(void)
{
	struct dp_match m = { false, false, false, true };

	CASE("a mid-map timer alone waits on the long timer");
	CHECK(dp_action_of(&m) == DP_ACT_WAIT_LONG);

	CASE("a completing timer beside a mid-map timer waits on the short "
	     "timer");
	m.complete_on_timer = true;
	CHECK(dp_action_of(&m) == DP_ACT_WAIT_SHORT);

	CASE("a complete match dials over a mid-map timer");
	m.complete_on_timer = false;
	m.complete_now = true;
	CHECK(dp_action_of(&m) == DP_ACT_DIAL);
}

#define STRIP_GUARD	'Z'

/* every byte of buf from 'from' on still holds the guard */
static bool strip_guard_intact(const char *buf, size_t from, size_t len)
{
	for (size_t i = from; i < len; i++) {
		if (buf[i] != STRIP_GUARD)
			return false;
	}

	return true;
}

static void digits_strip_tests(void)
{
	char out[8];

	CASE("the strip helper drops every timer marker");
	CHECK_UINT(dialplan_digits_strip("9T1T1", out, sizeof(out)), 3);
	CHECK(strcmp(out, "911") == 0);

	CASE("the strip helper drops a lowercase 't'");
	CHECK_UINT(dialplan_digits_strip("9t11", out, sizeof(out)), 3);
	CHECK(strcmp(out, "911") == 0);

	CASE("the strip helper keeps digits, DTMF letters, '*' and '#'");
	CHECK_UINT(dialplan_digits_strip("*7A#T", out, sizeof(out)), 4);
	CHECK(strcmp(out, "*7A#") == 0);

	CASE("the strip helper copies a buffer without markers unchanged");
	CHECK_UINT(dialplan_digits_strip("5551234", out, sizeof(out)), 7);
	CHECK(strcmp(out, "5551234") == 0);

	CASE("the strip helper answers the length a short buffer needs");
	memset(out, STRIP_GUARD, sizeof(out));
	CHECK_UINT(dialplan_digits_strip("12T345", out, 3), 5);
	CHECK(strcmp(out, "12") == 0);
	CHECK(strip_guard_intact(out, 3, sizeof(out)));

	CASE("the strip helper writes an empty string into a one byte buffer");
	memset(out, STRIP_GUARD, sizeof(out));
	CHECK_UINT(dialplan_digits_strip("9T11", out, 1), 3);
	CHECK(out[0] == '\0');
	CHECK(strip_guard_intact(out, 1, sizeof(out)));

	CASE("the strip helper reads a NULL input as empty");
	out[0] = 'x';
	CHECK_UINT(dialplan_digits_strip(NULL, out, sizeof(out)), 0);
	CHECK(out[0] == '\0');

	CASE("the strip helper writes nothing into a zero length buffer");
	memset(out, STRIP_GUARD, sizeof(out));
	CHECK_UINT(dialplan_digits_strip("9T1", out, 0), 2);
	CHECK(strip_guard_intact(out, 0, sizeof(out)));
}

int main(void)
{
	compile_accept_tests();
	compile_refuse_tests();
	default_map_tests();
	emergency_map_tests();
	nanp_map_tests();
	terminator_map_tests();
	timer_map_tests();
	star_walk_tests();
	star_chain_tests();
	tok_cap_star_tests();
	digit_cap_tests();
	action_tests();
	multi_alt_tests();
	expert_map_tests();
	field_map_tests();
	service_map_tests();
	mid_map_timer_compile_tests();
	mid_map_timer_match_tests();
	timer_unexpected_tests();
	collector_tests();
	action_timer_next_tests();
	digits_strip_tests();

	return tu_summary("test_dialplan");
}
