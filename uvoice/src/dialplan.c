// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * MGCP digit map matcher (RFC 3435 2.1.5). See dialplan.h for the grammar and
 * the caller contract.
 *
 * Class bit order. A DP_TOK_CLASS set is a bitset over the 16 members the
 * grammar allows, in this private order:
 *	bits 0-9	the digits '0' to '9'
 *	bit 10		'*'
 *	bit 11		'#'
 *	bits 12-15	'A' to 'D'
 * DP_TOK_LIT keeps the literal character in the same field instead, so a
 * literal never needs the bit order.
 *
 * The walk. dialplan_match() keeps, per alternative, the set of token
 * positions the digits can leave it standing at, one bit per position. It
 * advances the whole set by one digit at a time. A '.' token loops on itself
 * and carries a skip edge to the next position, which covers the "zero
 * occurrences" case without backtracking. The set is exact and the work per
 * digit is bounded by DIALPLAN_TOK_MAX, so an adversarial map cannot make the
 * matcher expensive. A 'T' in the buffer is a timer expiry: it advances a
 * DP_TOK_TIMER position and no other, so a timer that no alternative expects
 * leaves the set empty (RFC 3435 2.1.5, "Unexpected timers ... can cause
 * impossible matches").
 */
#include <string.h>

#include "dialplan.h"

#define DP_CLASS_DIGITS		0x03ffu

/*
 * Only 't' and 'X' normalize. Every other letter keeps its case, lowercase a-d
 * included, so such a map refuses here and the caller falls back to
 * DIALPLAN_DEFAULT_MAP.
 */
static char dp_char_norm(char c)
{
	if (c == 't')
		return 'T';

	if (c == 'X')
		return 'x';

	return c;
}

static int dp_member_bit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';

	if (c == '*')
		return 10;

	if (c == '#')
		return 11;

	if (c >= 'A' && c <= 'D')
		return 12 + (c - 'A');

	return -1;
}

static bool dp_class_parse(const char *s, size_t len, size_t *pos, uint16_t *set)
{
	uint16_t bits = 0;
	size_t i = *pos;

	while (i < len && s[i] != ']') {
		char lo = dp_char_norm(s[i]);
		int lo_bit = dp_member_bit(lo);
		char hi;

		/* field maps carry '[*#x]', so a class wildcard expands to the
		 * digits 0 to 9 */
		if (lo == 'x') {
			bits |= DP_CLASS_DIGITS;
			i++;
			continue;
		}

		if (lo_bit < 0)
			return false;

		i++;
		if (i >= len || s[i] != '-') {
			bits |= (uint16_t)(1u << lo_bit);
			continue;
		}

		/* RFC 3435 2.1.5: a subrange joins two digits, nothing else */
		if (lo < '0' || lo > '9' || i + 1 >= len)
			return false;

		hi = dp_char_norm(s[i + 1]);
		if (hi < '0' || hi > '9' || hi < lo)
			return false;

		for (char d = lo; d <= hi; d++)
			bits |= (uint16_t)(1u << (d - '0'));

		i += 2;
	}

	if (i >= len || bits == 0)
		return false;

	*pos = i + 1;
	*set = bits;

	return true;
}

static bool dp_tok_parse(const char *s, size_t len, size_t *pos, struct dp_token *tok)
{
	char c = dp_char_norm(s[*pos]);

	tok->star = false;
	tok->set = 0;

	if (c == 'x') {
		tok->kind = DP_TOK_CLASS;
		tok->set = DP_CLASS_DIGITS;
		*pos += 1;

		return true;
	}

	if (c == 'T') {
		tok->kind = DP_TOK_TIMER;
		*pos += 1;

		return true;
	}

	if (c == '[') {
		size_t next = *pos + 1;
		uint16_t set;

		if (!dp_class_parse(s, len, &next, &set))
			return false;

		tok->kind = DP_TOK_CLASS;
		tok->set = set;
		*pos = next;

		return true;
	}

	if (dp_member_bit(c) < 0)
		return false;

	tok->kind = DP_TOK_LIT;
	tok->set = (uint16_t)(unsigned char)c;
	*pos += 1;

	return true;
}

static bool dp_alt_compile(const char *s, size_t len, struct dp_token *tok, uint8_t *count)
{
	unsigned int n = 0;
	size_t i = 0;

	while (i < len) {
		/* a '.' repeats the construct before it, so it takes no slot */
		if (dp_char_norm(s[i]) == '.') {
			if (n == 0 || tok[n - 1].star || tok[n - 1].kind == DP_TOK_TIMER)
				return false;

			tok[n - 1].star = true;
			i++;
			continue;
		}

		if (n >= DIALPLAN_TOK_MAX)
			return false;

		if (!dp_tok_parse(s, len, &i, &tok[n]))
			return false;

		n++;
	}

	if (n == 0)
		return false;

	*count = (uint8_t)n;

	return true;
}

bool dialplan_compile(const char *map, struct dialplan *out)
{
	unsigned int alts = 0;
	const char *p = map;

	if (!map || !out)
		return false;

	out->alts = 0;

	if (!*map)
		return false;

	for (;;) {
		const char *bar = strchr(p, '|');
		size_t len = bar ? (size_t)(bar - p) : strlen(p);

		if (alts >= DIALPLAN_ALT_MAX)
			return false;

		if (!dp_alt_compile(p, len, out->alt[alts].tok, &out->alt[alts].count))
			return false;

		alts++;
		if (!bar)
			break;

		p = bar + 1;
	}

	out->alts = alts;

	return true;
}

/*
 * The skip edges of a '.' only ever run forward by one position, so one
 * ascending pass reaches every position a chain of them can reach.
 */
static uint64_t dp_pos_closure(const struct dp_token *tok, unsigned int count, uint64_t set)
{
	for (unsigned int i = 0; i < count; i++) {
		if ((set & (1ull << i)) && tok[i].star)
			set |= 1ull << (i + 1);
	}

	return set;
}

static bool dp_tok_matches(const struct dp_token *tok, char c, int bit)
{
	if (tok->kind == DP_TOK_LIT)
		return tok->set == (uint16_t)(unsigned char)c;

	if (tok->kind == DP_TOK_CLASS)
		return bit >= 0 && ((tok->set >> bit) & 1u) != 0;

	return tok->kind == DP_TOK_TIMER && c == 'T';
}

static uint64_t dp_pos_step(const struct dp_token *tok, unsigned int count, uint64_t set,
			    char c)
{
	int bit = dp_member_bit(c);
	uint64_t next = 0;

	for (unsigned int i = 0; i < count; i++) {
		if (!(set & (1ull << i)) || !dp_tok_matches(&tok[i], c, bit))
			continue;

		next |= tok[i].star ? (1ull << i) : (1ull << (i + 1));
	}

	return dp_pos_closure(tok, count, next);
}

/*
 * RFC 3660 2.2, T-critical: "a timer is all that is required to produce a
 * match" when the position after the timer at i is an accept position, a
 * starred tail included ("9Tx.").
 */
static bool dp_timer_completes(const struct dp_token *tok, unsigned int count,
			       unsigned int i)
{
	uint64_t after = dp_pos_closure(tok, count, 1ull << (i + 1));

	return (after & (1ull << count)) != 0;
}

static void dp_alt_match(const struct dp_token *tok, unsigned int count, const char *digits,
			 unsigned int len, struct dp_match *out)
{
	uint64_t set = dp_pos_closure(tok, count, 1);

	for (unsigned int i = 0; i < len && set; i++)
		set = dp_pos_step(tok, count, set, dp_char_norm(digits[i]));

	if (!set)
		return;

	if (set & (1ull << count))
		out->complete_now = true;

	for (unsigned int i = 0; i < count; i++) {
		if (!(set & (1ull << i)))
			continue;

		if (tok[i].kind != DP_TOK_TIMER)
			out->can_extend = true;
		else if (dp_timer_completes(tok, count, i))
			out->complete_on_timer = true;
		else
			out->timer_next = true;
	}
}

void dialplan_match(const struct dialplan *dp, const char *digits, unsigned int len,
		    struct dp_match *out)
{
	if (!out)
		return;

	out->complete_now = false;
	out->complete_on_timer = false;
	out->can_extend = false;
	out->timer_next = false;

	if (!dp)
		return;

	if (!digits)
		len = 0;

	/* the collector caps the buffer; clamp anyway so the walk stays bounded */
	if (len > DIALPLAN_DIGITS_MAX)
		len = DIALPLAN_DIGITS_MAX;

	for (unsigned int a = 0; a < dp->alts && a < DIALPLAN_ALT_MAX; a++)
		dp_alt_match(dp->alt[a].tok, dp->alt[a].count, digits, len, out);
}

size_t dialplan_digits_strip(const char *in, char *out, size_t olen)
{
	size_t n = 0;

	for (; in && *in; in++) {
		if (dp_char_norm(*in) == 'T')
			continue;

		if (n + 1 < olen)
			out[n] = *in;
		n++;
	}

	if (olen)
		out[n < olen ? n : olen - 1] = '\0';

	return n;
}

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

const char *dp_action_name(enum dp_action action)
{
	switch (action) {
	case DP_ACT_REORDER:
		return "reorder";
	case DP_ACT_DIAL:
		return "dial";
	case DP_ACT_WAIT_SHORT:
		return "wait-short";
	case DP_ACT_WAIT_LONG:
		return "wait-long";
	}

	return "reorder";
}
