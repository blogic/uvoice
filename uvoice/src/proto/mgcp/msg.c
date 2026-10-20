// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * MGCP text layer. See mgcp/msg.h for the contracts.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "proto/mgcp/msg.h"

#define ARRAY_LEN(a)		(sizeof(a) / sizeof((a)[0]))

/* RFC 3435 3.2.2.7: connection parameter values hold nine digits at most */
#define MGCP_CP_VALUE_MAX	999999999u

static bool is_wsp(char c)
{
	return c == ' ' || c == '\t';
}

static bool is_eol(char c)
{
	return c == '\r' || c == '\n';
}

static struct sip_str str_mid(struct sip_str s, size_t from, size_t to)
{
	if (to > s.len)
		to = s.len;
	if (from > to)
		from = to;

	return sip_str_init(s.p + from, to - from);
}

static bool digits_parse(struct sip_str s, size_t max_digits, uint32_t *out)
{
	if (!s.len || s.len > max_digits)
		return false;

	return sip_str_uint(s, out);
}

/* ------------------------------------------------------------- the scans */

/*
 * Offset of the first sep outside a quoted string and a parenthesized
 * group, or s.len. Appendix A escapes a quote inside a quoted string by
 * doubling it, and toggling the quoted state on every quote reads that
 * escape correctly. ranges also skips a bracketed range: Appendix A defines
 * one inside an event name alone, and an LCO value can hold a stray "[".
 */
static size_t sep_scan(struct sip_str s, char sep, bool ranges)
{
	unsigned int paren = 0, bracket = 0;
	bool quoted = false;
	size_t i;

	for (i = 0; i < s.len; i++) {
		char c = s.p[i];

		if (c == '"') {
			quoted = !quoted;
			continue;
		}
		if (quoted)
			continue;
		if (c == sep && !paren && !bracket)
			return i;
		if (c == '(')
			paren++;
		else if (c == ')' && paren)
			paren--;
		else if (c == '[' && ranges)
			bracket++;
		else if (c == ']' && bracket)
			bracket--;
	}

	return s.len;
}

static size_t sep_find(struct sip_str s, char sep)
{
	return sep_scan(s, sep, false);
}

static size_t range_sep_find(struct sip_str s, char sep)
{
	return sep_scan(s, sep, true);
}

/* the offset of the ')' that closes the '(' at open, or s.len */
static size_t paren_close(struct sip_str s, size_t open)
{
	unsigned int depth = 0;
	bool quoted = false;
	size_t i;

	for (i = open; i < s.len; i++) {
		char c = s.p[i];

		if (c == '"') {
			quoted = !quoted;
			continue;
		}
		if (quoted)
			continue;
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			if (!--depth)
				return i;
		}
	}

	return s.len;
}

static bool list_split(struct sip_str *rest, struct sip_str *item,
		       bool ranges)
{
	struct sip_str it;
	size_t at;

	while (rest->len) {
		at = sep_scan(*rest, ',', ranges);
		it = sip_str_trim(sip_str_init(rest->p, at));
		if (at < rest->len)
			at++;
		rest->p += at;
		rest->len -= at;
		if (!it.len)
			continue;
		*item = it;
		return true;
	}

	return false;
}

bool mgcp_list_next(struct sip_str *rest, struct sip_str *item)
{
	return list_split(rest, item, false);
}

/* one item of an event or signal list, whose names can hold a range */
static bool event_list_next(struct sip_str *rest, struct sip_str *item)
{
	return list_split(rest, item, true);
}

bool mgcp_event_param_next(struct sip_str *rest, struct sip_str *name,
			   struct sip_str *value)
{
	struct sip_str item;
	size_t eq, open, close;

	if (!mgcp_list_next(rest, &item))
		return false;

	eq = sep_find(item, '=');
	open = sep_find(item, '(');
	if (eq < item.len && eq < open) {
		*name = sip_str_trim(str_mid(item, 0, eq));
		*value = sip_str_trim(str_mid(item, eq + 1, item.len));
		return true;
	}
	if (open < item.len) {
		close = paren_close(item, open);
		*name = sip_str_trim(str_mid(item, 0, open));
		*value = sip_str_trim(str_mid(item, open + 1, close));
		return true;
	}

	*name = sip_str_init(item.p, 0);
	*value = item;

	return true;
}

struct sip_str mgcp_quoted_strip(struct sip_str s)
{
	if (s.len >= 2 && s.p[0] == '"' && s.p[s.len - 1] == '"') {
		s.p++;
		s.len -= 2;
	}

	return s;
}

int mgcp_unquote(struct sip_str in, char *out, size_t olen)
{
	size_t i, used = 0;

	if (!out || !olen)
		return -1;

	in = mgcp_quoted_strip(in);
	for (i = 0; i < in.len; i++) {
		char c = in.p[i];

		if (c == '"' && i + 1 < in.len && in.p[i + 1] == '"')
			i++;
		if (used + 1 >= olen)
			return -1;
		out[used++] = c;
	}
	out[used] = 0;

	return (int)used;
}

bool mgcp_txid_parse(struct sip_str s, uint32_t *out)
{
	uint32_t v;

	if (!digits_parse(s, 9, &v) || !v)
		return false;

	*out = v;

	return true;
}

/* -------------------------------------------------------- the name tables */

static const char *const verb_names[MGCP_VERB_MAX] = {
	[MGCP_VERB_UNKNOWN] = "UNKNOWN",
	[MGCP_VERB_EPCF] = "EPCF",
	[MGCP_VERB_CRCX] = "CRCX",
	[MGCP_VERB_MDCX] = "MDCX",
	[MGCP_VERB_DLCX] = "DLCX",
	[MGCP_VERB_RQNT] = "RQNT",
	[MGCP_VERB_NTFY] = "NTFY",
	[MGCP_VERB_AUEP] = "AUEP",
	[MGCP_VERB_AUCX] = "AUCX",
	[MGCP_VERB_RSIP] = "RSIP",
};

const char *mgcp_verb_name(enum mgcp_verb verb)
{
	if (verb >= MGCP_VERB_MAX)
		return "UNKNOWN";

	return verb_names[verb];
}

enum mgcp_verb mgcp_verb_parse(struct sip_str s)
{
	unsigned int i;

	for (i = MGCP_VERB_UNKNOWN + 1; i < MGCP_VERB_MAX; i++) {
		if (sip_str_eq_ci(s, verb_names[i]))
			return (enum mgcp_verb)i;
	}

	return MGCP_VERB_UNKNOWN;
}

static const char *const param_names[MGCP_PARAM_MAX] = {
	[MGCP_PARAM_UNKNOWN] = "",
	[MGCP_PARAM_B] = "B",
	[MGCP_PARAM_C] = "C",
	[MGCP_PARAM_A] = "A",
	[MGCP_PARAM_I] = "I",
	[MGCP_PARAM_M] = "M",
	[MGCP_PARAM_P] = "P",
	[MGCP_PARAM_T] = "T",
	[MGCP_PARAM_D] = "D",
	[MGCP_PARAM_ES] = "ES",
	[MGCP_PARAM_L] = "L",
	[MGCP_PARAM_MD] = "MD",
	[MGCP_PARAM_N] = "N",
	[MGCP_PARAM_O] = "O",
	[MGCP_PARAM_PL] = "PL",
	[MGCP_PARAM_Q] = "Q",
	[MGCP_PARAM_E] = "E",
	[MGCP_PARAM_R] = "R",
	[MGCP_PARAM_F] = "F",
	[MGCP_PARAM_X] = "X",
	[MGCP_PARAM_K] = "K",
	[MGCP_PARAM_RD] = "RD",
	[MGCP_PARAM_RM] = "RM",
	[MGCP_PARAM_I2] = "I2",
	[MGCP_PARAM_Z2] = "Z2",
	[MGCP_PARAM_S] = "S",
	[MGCP_PARAM_Z] = "Z",
	[MGCP_PARAM_RC] = "RC",
	[MGCP_PARAM_LC] = "LC",
};

const char *mgcp_param_name(enum mgcp_param code)
{
	if (code >= MGCP_PARAM_MAX)
		return "";

	return param_names[code];
}

enum mgcp_param mgcp_param_parse(struct sip_str s)
{
	unsigned int i;

	for (i = MGCP_PARAM_UNKNOWN + 1; i < MGCP_PARAM_MAX; i++) {
		if (sip_str_eq_ci(s, param_names[i]))
			return (enum mgcp_param)i;
	}

	return MGCP_PARAM_UNKNOWN;
}

enum mgcp_rc_class mgcp_rc_class_of(uint32_t code)
{
	if (code < 100)
		return MGCP_RC_ACK;
	if (code < 200)
		return MGCP_RC_PROVISIONAL;
	if (code < 300)
		return MGCP_RC_SUCCESS;
	if (code >= 400 && code < 500)
		return MGCP_RC_TRANSIENT;
	if (code >= 500 && code < 600)
		return MGCP_RC_PERMANENT;
	if (code >= 800 && code < 900)
		return MGCP_RC_PACKAGE;

	return MGCP_RC_UNKNOWN;
}

const char *mgcp_parse_status_name(enum mgcp_parse_status st)
{
	switch (st) {
	case MGCP_PARSE_OK:
		return "ok";
	case MGCP_PARSE_ERR_EMPTY:
		return "empty";
	case MGCP_PARSE_ERR_TOO_LARGE:
		return "too large";
	case MGCP_PARSE_ERR_FIRST_LINE:
		return "bad first line";
	case MGCP_PARSE_ERR_TXID:
		return "bad transaction id";
	case MGCP_PARSE_ERR_PARAM_OVERFLOW:
		return "too many parameters";
	}

	return "unknown";
}

/* ------------------------------------------------------------ the parser */

/*
 * The LF that ends the parameter line starting at p, outside a quoted
 * string, because Appendix A quoteChar admits CR and LF. A DQUOTE opens a
 * quoted string only where a value starts (page 173 parameterString, page
 * 172 eventParameterValue): first in the parameter value after the colon,
 * or first in an event parameter value after "(", or after "," or "="
 * inside parentheses, white space skipped. Any other DQUOTE is literal,
 * and "" inside a quoted string is an escaped quote. A quote that never
 * closes falls back to the first LF, so it cannot swallow the datagram.
 */
static const char *param_lf(const char *p, const char *end,
			    const char *first)
{
	bool colon = false, start = false, quoted = false;
	unsigned int depth = 0;
	const char *q;

	for (q = p; q < end; q++) {
		char c = *q;

		if (quoted) {
			if (c != '"')
				continue;
			if (q + 1 < end && q[1] == '"')
				q++;
			else
				quoted = false;
			continue;
		}
		if (c == '\n')
			return q;
		if (c == '"' && start) {
			quoted = true;
			start = false;
			continue;
		}
		if (!is_wsp(c))
			start = false;
		if (!colon) {
			colon = c == ':';
			start = colon;
		} else if (c == '(') {
			depth++;
			start = true;
		} else if (c == ')' && depth) {
			depth--;
		} else if ((c == ',' || c == '=') && depth) {
			start = true;
		}
	}

	return quoted ? first : end;
}

/*
 * The LF that ends the line starting at p, or end when the line has none.
 * A parameter line (quoted) follows param_lf(); the parser and the split
 * both read lines here, so they always agree on where a line ends.
 */
static const char *line_lf(const char *p, const char *end, bool quoted)
{
	const char *first = memchr(p, '\n', (size_t)(end - p));

	if (!quoted || !first)
		return first ? first : end;

	return param_lf(p, end, first);
}

/* the text of the line [p, lf), its CR removed */
static struct sip_str line_text(const char *p, const char *lf)
{
	if (lf > p && lf[-1] == '\r')
		lf--;

	return sip_str_init(p, (size_t)(lf - p));
}

/* Appendix A tolerates extra white space: such a line is the empty line */
static bool line_blank(struct sip_str line)
{
	size_t i;

	for (i = 0; i < line.len; i++) {
		if (!is_wsp(line.p[i]))
			return false;
	}

	return true;
}

struct line_ctx {
	const char *p;
	const char *end;
	uint32_t *flags;
};

static bool line_take(struct line_ctx *lc, struct sip_str *out, bool quoted)
{
	const char *lf;

	if (lc->p >= lc->end)
		return false;

	lf = line_lf(lc->p, lc->end, quoted);
	*out = line_text(lc->p, lf);
	if (lf == lc->end)
		*lc->flags |= MGCP_MSG_F_NO_FINAL_EOL;
	else if (lf == lc->p || lf[-1] != '\r')
		*lc->flags |= MGCP_MSG_F_BARE_LF;
	lc->p = lf < lc->end ? lf + 1 : lc->end;

	return true;
}

static bool version_valid(struct sip_str v, struct sip_str *profile)
{
	struct sip_str rest = v, word;
	size_t dot, i;

	if (!sip_word_take(&rest, &word) || !sip_str_eq_ci(word, "MGCP"))
		return false;
	if (!sip_word_take(&rest, &word))
		return false;

	dot = 0;
	while (dot < word.len && word.p[dot] != '.')
		dot++;
	if (!dot || dot + 1 >= word.len)
		return false;

	/* 1*(DIGIT) "." 1*(DIGIT) */
	for (i = 0; i < word.len; i++) {
		if (i != dot && !isdigit((unsigned char)word.p[i]))
			return false;
	}

	*profile = sip_str_trim(rest);

	return true;
}

static void endpoint_check(struct mgcp_msg *msg)
{
	struct sip_str ep = msg->endpoint;
	size_t at = 0;

	while (at < ep.len && ep.p[at] != '@')
		at++;
	if (at > MGCP_NAME_PART_MAX ||
	    (at < ep.len && ep.len - at - 1 > MGCP_NAME_PART_MAX))
		msg->flags |= MGCP_MSG_F_NAME_LONG;
}

static bool verb_word_valid(struct sip_str w)
{
	size_t i;

	if (w.len != 4 || !isalpha((unsigned char)w.p[0]))
		return false;
	for (i = 1; i < w.len; i++) {
		if (!isalnum((unsigned char)w.p[i]))
			return false;
	}

	return true;
}

static bool code_word(struct sip_str w, uint32_t *code)
{
	size_t i;

	if (w.len != 3)
		return false;
	for (i = 0; i < 3; i++) {
		if (!isdigit((unsigned char)w.p[i]))
			return false;
	}

	return sip_str_uint(w, code);
}

static enum mgcp_parse_status response_line_parse(struct mgcp_msg *msg,
						  struct sip_str rest)
{
	struct sip_str w;

	if (!sip_word_take(&rest, &w))
		return MGCP_PARSE_ERR_FIRST_LINE;
	if (!mgcp_txid_parse(w, &msg->txid))
		return MGCP_PARSE_ERR_TXID;

	rest = sip_str_trim(rest);
	if (rest.len && rest.p[0] == '/') {
		sip_word_take(&rest, &w);
		msg->package = str_mid(w, 1, w.len);
		rest = sip_str_trim(rest);
	}
	msg->comment = rest;
	msg->is_command = false;

	return MGCP_PARSE_OK;
}

static enum mgcp_parse_status first_line_parse(struct mgcp_msg *msg,
					       struct sip_str line)
{
	struct sip_str rest = sip_str_trim(line), w1, w2, w3;

	if (!sip_word_take(&rest, &w1))
		return MGCP_PARSE_ERR_FIRST_LINE;

	if (code_word(w1, &msg->code))
		return response_line_parse(msg, rest);

	if (!sip_word_take(&rest, &w2) || !sip_word_take(&rest, &w3))
		return MGCP_PARSE_ERR_FIRST_LINE;

	rest = sip_str_trim(rest);
	if (!rest.len)
		return MGCP_PARSE_ERR_FIRST_LINE;
	if (!mgcp_txid_parse(w2, &msg->txid))
		return MGCP_PARSE_ERR_TXID;

	msg->is_command = true;
	msg->verb_name = w1;
	msg->verb = mgcp_verb_parse(w1);
	msg->endpoint = w3;
	msg->version = rest;
	msg->profile = sip_str_init(rest.p + rest.len, 0);
	if (!version_valid(rest, &msg->profile))
		msg->flags |= MGCP_MSG_F_BAD_VERSION;
	if (!verb_word_valid(w1))
		msg->flags |= MGCP_MSG_F_VERB_BAD;
	endpoint_check(msg);

	return MGCP_PARSE_OK;
}

static enum mgcp_parse_status param_add(struct mgcp_msg *msg,
					struct sip_str line)
{
	struct mgcp_param_line *pl;
	const char *colon;

	if (msg->param_count >= MGCP_PARAM_LINE_MAX)
		return MGCP_PARSE_ERR_PARAM_OVERFLOW;

	pl = &msg->param[msg->param_count++];
	memset(pl, 0, sizeof(*pl));

	colon = memchr(line.p, ':', line.len);
	if (!colon) {
		pl->name = sip_str_trim(line);
		pl->value = sip_str_init(line.p + line.len, 0);
		msg->flags |= MGCP_MSG_F_PARAM_BAD;
		return MGCP_PARSE_OK;
	}

	pl->name = sip_str_trim(sip_str_init(line.p,
					     (size_t)(colon - line.p)));
	pl->value = sip_str_trim(sip_str_init(colon + 1,
		line.len - (size_t)(colon - line.p) - 1));
	pl->code = mgcp_param_parse(pl->name);
	if (!pl->name.len)
		msg->flags |= MGCP_MSG_F_PARAM_BAD;

	return MGCP_PARSE_OK;
}

static bool only_eol(struct sip_str s)
{
	size_t i;

	for (i = 0; i < s.len; i++) {
		if (!is_eol(s.p[i]))
			return false;
	}

	return true;
}

static void body_set(struct mgcp_msg *msg, const char *p, const char *end)
{
	struct sip_str body = sip_str_init(p, (size_t)(end - p));

	if (only_eol(body)) {
		msg->sdp = sip_str_init(end, 0);
		return;
	}
	if (body.p[body.len - 1] != '\n')
		msg->flags |= MGCP_MSG_F_NO_FINAL_EOL;

	msg->sdp = body;
}

enum mgcp_parse_status mgcp_msg_parse(struct mgcp_msg *msg, const char *buf,
				      size_t len)
{
	struct line_ctx lc;
	struct sip_str line;
	enum mgcp_parse_status st;

	if (!msg)
		return MGCP_PARSE_ERR_EMPTY;

	memset(msg, 0, sizeof(*msg));
	if (!buf || !len)
		return MGCP_PARSE_ERR_EMPTY;
	if (len > MGCP_MSG_LEN_MAX)
		return MGCP_PARSE_ERR_TOO_LARGE;

	msg->buf = buf;
	msg->len = len;
	msg->verb_name = sip_str_init(buf + len, 0);
	msg->endpoint = msg->verb_name;
	msg->version = msg->verb_name;
	msg->profile = msg->verb_name;
	msg->package = msg->verb_name;
	msg->comment = msg->verb_name;
	msg->sdp = msg->verb_name;
	lc.p = buf;
	lc.end = buf + len;
	lc.flags = &msg->flags;

	while (lc.p < lc.end && is_eol(*lc.p)) {
		msg->flags |= MGCP_MSG_F_LEAD_EOL;
		lc.p++;
	}
	if (lc.p >= lc.end)
		return MGCP_PARSE_ERR_EMPTY;

	line_take(&lc, &line, false);
	st = first_line_parse(msg, line);
	if (st != MGCP_PARSE_OK)
		return st;

	msg->sdp = sip_str_init(lc.end, 0);
	while (line_take(&lc, &line, true)) {
		if (line_blank(line)) {
			body_set(msg, lc.p, lc.end);
			break;
		}

		st = param_add(msg, line);
		if (st != MGCP_PARSE_OK)
			return st;
	}

	return MGCP_PARSE_OK;
}

/* ------------------------------------------------------------- the split */

void mgcp_split_init(struct mgcp_split *it, const char *buf, size_t len)
{
	it->cur = buf;
	it->end = buf ? buf + len : NULL;
}

/* A "." line, white space after the dot tolerated; *next is its successor */
static bool sep_line(const char *p, const char *end, const char **next)
{
	if (p >= end || *p != '.')
		return false;

	p++;
	while (p < end && is_wsp(*p))
		p++;
	if (p < end && *p == '\r')
		p++;
	if (p < end && *p != '\n')
		return false;

	*next = p < end ? p + 1 : end;

	return true;
}

/*
 * The phases of one message, so the split walks the same lines as the
 * parser: the first line, the parameter lines, whose quoted strings can
 * hold a line end, and the session description.
 */
enum split_phase {
	SPLIT_FIRST = 0,
	SPLIT_PARAMS,
	SPLIT_BODY,
};

static const char *split_line_end(const char *ls, const char *end,
				  enum split_phase *phase)
{
	const char *lf = line_lf(ls, end, *phase == SPLIT_PARAMS);
	struct sip_str text = line_text(ls, lf);

	if (*phase == SPLIT_FIRST && text.len)
		*phase = SPLIT_PARAMS;
	else if (*phase == SPLIT_PARAMS && line_blank(text))
		*phase = SPLIT_BODY;

	return lf < end ? lf + 1 : end;
}

bool mgcp_split_next(struct mgcp_split *it, struct sip_str *out)
{
	const char *start, *ls, *next;
	enum split_phase phase;
	struct sip_str msg;

	while (it->cur && it->cur < it->end) {
		start = it->cur;
		ls = start;
		next = it->end;
		phase = SPLIT_FIRST;
		msg = sip_str_init(start, (size_t)(it->end - start));

		while (ls < it->end) {
			if (sep_line(ls, it->end, &next)) {
				msg = sip_str_init(start, (size_t)(ls - start));
				break;
			}
			ls = split_line_end(ls, it->end, &phase);
			next = it->end;
		}

		it->cur = next;
		if (only_eol(msg))
			continue;

		*out = msg;
		return true;
	}

	return false;
}

bool mgcp_msg_version_is(const struct mgcp_msg *msg, const char *token)
{
	struct sip_str a = msg->version, b = sip_str_cstr(token), wa, wb;
	bool ha, hb;

	for (;;) {
		ha = sip_word_take(&a, &wa);
		hb = sip_word_take(&b, &wb);
		if (!ha || !hb)
			return ha == hb;
		if (wa.len != wb.len || strncasecmp(wa.p, wb.p, wa.len))
			return false;
	}
}

/* --------------------------------------------------------- the param index */

const struct mgcp_param_line *mgcp_msg_param(const struct mgcp_msg *msg,
					     enum mgcp_param code)
{
	unsigned int i;

	if (code == MGCP_PARAM_UNKNOWN)
		return NULL;

	for (i = 0; i < msg->param_count; i++) {
		if (msg->param[i].code == code)
			return &msg->param[i];
	}

	return NULL;
}

const struct mgcp_param_line *mgcp_msg_param_next(
	const struct mgcp_msg *msg, const struct mgcp_param_line *cur)
{
	unsigned int i;

	if (!cur || cur->code == MGCP_PARAM_UNKNOWN)
		return NULL;

	for (i = (unsigned int)(cur - msg->param) + 1; i < msg->param_count;
	     i++) {
		if (msg->param[i].code == cur->code)
			return &msg->param[i];
	}

	return NULL;
}

const struct mgcp_param_line *mgcp_msg_param_named(const struct mgcp_msg *msg,
						   const char *name)
{
	unsigned int i;

	for (i = 0; i < msg->param_count; i++) {
		if (sip_str_eq_ci(msg->param[i].name, name))
			return &msg->param[i];
	}

	return NULL;
}

bool mgcp_msg_value(const struct mgcp_msg *msg, enum mgcp_param code,
		    struct sip_str *out)
{
	const struct mgcp_param_line *pl = mgcp_msg_param(msg, code);

	if (!pl)
		return false;
	if (out)
		*out = pl->value;

	return true;
}

/* ------------------------------------------------------ events and signals */

bool mgcp_event_parse(struct sip_str s, struct sip_str default_pkg,
		      struct mgcp_event *ev)
{
	struct sip_str name;
	size_t open, close, at, slash;

	memset(ev, 0, sizeof(*ev));
	s = sip_str_trim(s);
	if (!s.len)
		return false;

	ev->raw = s;
	open = range_sep_find(s, '(');
	name = sip_str_trim(str_mid(s, 0, open));
	ev->params = sip_str_init(s.p + s.len, 0);
	if (open < s.len) {
		close = paren_close(s, open);
		if (close >= s.len || sip_str_trim(str_mid(s, close + 1,
							   s.len)).len)
			return false;
		ev->params = sip_str_trim(str_mid(s, open + 1, close));
		ev->has_params = true;
	}

	at = range_sep_find(name, '@');
	ev->conn = sip_str_init(name.p + name.len, 0);
	if (at < name.len) {
		ev->conn = sip_str_trim(str_mid(name, at + 1, name.len));
		name = sip_str_trim(str_mid(name, 0, at));
		if (!ev->conn.len)
			return false;
	}

	slash = range_sep_find(name, '/');
	if (slash < name.len) {
		ev->pkg = sip_str_trim(str_mid(name, 0, slash));
		ev->name = sip_str_trim(str_mid(name, slash + 1, name.len));
		if (!ev->pkg.len)
			return false;
	} else {
		ev->pkg = default_pkg;
		ev->pkg_default = true;
		ev->name = name;
	}

	return ev->name.len > 0;
}

bool mgcp_sig_next(struct mgcp_sig_iter *it, struct mgcp_event *ev)
{
	struct sip_str item;

	if (it->bad || !event_list_next(&it->rest, &item))
		return false;
	if (mgcp_event_parse(item, it->default_pkg, ev))
		return true;

	it->bad = true;

	return false;
}

bool mgcp_sig_first(struct sip_str list, struct sip_str default_pkg,
		    struct mgcp_sig_iter *it, struct mgcp_event *ev)
{
	it->rest = list;
	it->default_pkg = default_pkg;
	it->bad = false;

	return mgcp_sig_next(it, ev);
}

static bool action_one(struct mgcp_req_event *re, struct sip_str tok)
{
	static const struct {
		char letter;
		uint32_t bit;
	} plain[] = {
		{ 'N', MGCP_ACT_N }, { 'A', MGCP_ACT_A }, { 'D', MGCP_ACT_D },
		{ 'S', MGCP_ACT_S }, { 'I', MGCP_ACT_I }, { 'K', MGCP_ACT_K },
	};
	size_t open, close, i;

	if (tok.len == 1) {
		char c = (char)toupper((unsigned char)tok.p[0]);

		for (i = 0; i < ARRAY_LEN(plain); i++) {
			if (plain[i].letter != c)
				continue;
			re->act |= plain[i].bit;
			return true;
		}
	}

	open = sep_find(tok, '(');
	if (open == 1 && toupper((unsigned char)tok.p[0]) == 'E') {
		close = paren_close(tok, open);
		if (close + 1 != tok.len || (re->act & MGCP_ACT_E))
			return false;
		re->act |= MGCP_ACT_E;
		re->embedded = sip_str_trim(str_mid(tok, open + 1, close));
		return true;
	}

	if (!re->ext.len)
		re->ext = tok;
	re->act |= MGCP_ACT_EXT;

	return true;
}

static bool req_item_parse(struct sip_str item, struct sip_str default_pkg,
			   struct mgcp_req_event *re)
{
	struct sip_str name, after, rest, tok;
	size_t open, close;

	memset(re, 0, sizeof(*re));
	re->actions = sip_str_init(item.p + item.len, 0);
	re->embedded = re->actions;
	re->ext = re->actions;
	open = range_sep_find(item, '(');
	name = str_mid(item, 0, open);
	if (!mgcp_event_parse(name, default_pkg, &re->ev))
		return false;
	re->ev.raw = item;

	if (open >= item.len) {
		re->act = MGCP_ACT_N;
		return true;
	}

	close = paren_close(item, open);
	if (close >= item.len)
		return false;
	re->actions = sip_str_trim(str_mid(item, open + 1, close));

	after = sip_str_trim(str_mid(item, close + 1, item.len));
	if (after.len) {
		if (after.p[0] != '(')
			return false;
		close = paren_close(after, 0);
		if (close + 1 != after.len)
			return false;
		re->ev.params = sip_str_trim(str_mid(after, 1, close));
		re->ev.has_params = true;
	}

	rest = re->actions;
	while (mgcp_list_next(&rest, &tok)) {
		if (!action_one(re, tok))
			return false;
	}

	return re->act != 0;
}

bool mgcp_req_next(struct mgcp_req_iter *it, struct mgcp_req_event *re)
{
	struct sip_str item;

	if (it->bad || !event_list_next(&it->rest, &item))
		return false;
	if (req_item_parse(item, it->default_pkg, re))
		return true;

	it->bad = true;

	return false;
}

bool mgcp_req_first(struct sip_str list, struct sip_str default_pkg,
		    struct mgcp_req_iter *it, struct mgcp_req_event *re)
{
	it->rest = list;
	it->default_pkg = default_pkg;
	it->bad = false;

	return mgcp_req_next(it, re);
}

bool mgcp_embedded_parse(struct sip_str e, struct mgcp_embedded *out)
{
	struct sip_str rest = e, item, *slot;
	size_t open, close;
	bool *seen;

	memset(out, 0, sizeof(*out));

	while (event_list_next(&rest, &item)) {
		open = sep_find(item, '(');
		if (open != 1)
			return false;
		close = paren_close(item, open);
		if (close + 1 != item.len)
			return false;

		switch (toupper((unsigned char)item.p[0])) {
		case 'R':
			slot = &out->r;
			seen = &out->has_r;
			break;
		case 'S':
			slot = &out->s;
			seen = &out->has_s;
			break;
		case 'D':
			slot = &out->d;
			seen = &out->has_d;
			break;
		default:
			return false;
		}
		if (*seen)
			return false;

		*seen = true;
		*slot = sip_str_trim(str_mid(item, open + 1, close));
	}

	return out->has_r || out->has_s || out->has_d;
}

/* ------------------------------------------------- typed parameter values */

bool mgcp_entity_parse(struct sip_str s, struct mgcp_entity *e)
{
	struct sip_str host, port;
	size_t at, i, colon;

	memset(e, 0, sizeof(*e));
	s = sip_str_trim(s);

	at = s.len;
	for (i = 0; i < s.len; i++) {
		if (s.p[i] == '@') {
			at = i;
			break;
		}
	}
	host = s;
	if (at < s.len) {
		e->local = str_mid(s, 0, at);
		host = str_mid(s, at + 1, s.len);
		if (!e->local.len)
			return false;
	}

	port = sip_str_init(host.p + host.len, 0);
	if (host.len && host.p[0] == '[') {
		for (i = 1; i < host.len && host.p[i] != ']'; i++)
			;
		if (i >= host.len)
			return false;
		e->domain = str_mid(host, 0, i + 1);
		if (i + 1 < host.len) {
			if (host.p[i + 1] != ':')
				return false;
			port = str_mid(host, i + 2, host.len);
			if (!port.len)
				return false;
		}
	} else {
		colon = host.len;
		for (i = 0; i < host.len; i++) {
			if (host.p[i] == ':')
				colon = i;
		}
		e->domain = str_mid(host, 0, colon);
		if (colon < host.len) {
			port = str_mid(host, colon + 1, host.len);
			if (!port.len)
				return false;
		}
	}

	if (!e->domain.len || e->domain.len > MGCP_NAME_PART_MAX + 2)
		return false;
	if (port.len && (!digits_parse(port, 5, &e->port) || !e->port ||
			 e->port > 65535))
		return false;

	return true;
}

bool mgcp_hex_id_valid(struct sip_str s)
{
	size_t i;

	if (!s.len || s.len > 32)
		return false;
	for (i = 0; i < s.len; i++) {
		if (!isxdigit((unsigned char)s.p[i]))
			return false;
	}

	return true;
}

static bool hex_param(const struct mgcp_msg *msg, enum mgcp_param code,
		      struct sip_str *out)
{
	struct sip_str v;

	if (!mgcp_msg_value(msg, code, &v) || !mgcp_hex_id_valid(v))
		return false;
	*out = v;

	return true;
}

bool mgcp_msg_call_id(const struct mgcp_msg *msg, struct sip_str *out)
{
	return hex_param(msg, MGCP_PARAM_C, out);
}

bool mgcp_msg_request_id(const struct mgcp_msg *msg, struct sip_str *out)
{
	return hex_param(msg, MGCP_PARAM_X, out);
}

bool mgcp_msg_conn_id(const struct mgcp_msg *msg, struct sip_str *out)
{
	struct sip_str v, first;

	if (!mgcp_msg_value(msg, MGCP_PARAM_I, &v))
		return false;
	if (!mgcp_list_next(&v, &first) || !mgcp_hex_id_valid(first))
		return false;
	*out = first;

	return true;
}

bool mgcp_lco_parse(struct sip_str s, struct mgcp_lco *lco)
{
	static const struct {
		const char *name;
		size_t off;
	} attrs[] = {
		{ "p", offsetof(struct mgcp_lco, p) },
		{ "a", offsetof(struct mgcp_lco, a) },
		{ "b", offsetof(struct mgcp_lco, b) },
		{ "e", offsetof(struct mgcp_lco, e) },
		{ "gc", offsetof(struct mgcp_lco, gc) },
		{ "s", offsetof(struct mgcp_lco, s) },
		{ "t", offsetof(struct mgcp_lco, t) },
		{ "r", offsetof(struct mgcp_lco, r) },
		{ "k", offsetof(struct mgcp_lco, k) },
		{ "nt", offsetof(struct mgcp_lco, nt) },
		{ "v", offsetof(struct mgcp_lco, v) },
		{ "m", offsetof(struct mgcp_lco, m) },
	};
	struct sip_str rest = s, item, name, value;
	size_t colon, i;
	bool known;

	memset(lco, 0, sizeof(*lco));

	while (mgcp_list_next(&rest, &item)) {
		colon = sep_find(item, ':');
		name = sip_str_trim(str_mid(item, 0, colon));
		value = sip_str_trim(str_mid(item, colon + 1, item.len));
		if (!name.len)
			return false;

		known = false;
		for (i = 0; i < ARRAY_LEN(attrs); i++) {
			if (!sip_str_eq_ci(name, attrs[i].name))
				continue;
			*(struct sip_str *)((char *)lco + attrs[i].off) = value;
			known = true;
			break;
		}
		if (known)
			continue;

		lco->unknown++;
		if (!lco->ext_mandatory.len && sip_str_prefix_ci(name, "x+"))
			lco->ext_mandatory = name;
	}

	return true;
}

bool mgcp_msg_lco(const struct mgcp_msg *msg, struct mgcp_lco *lco)
{
	struct sip_str v;

	if (!mgcp_msg_value(msg, MGCP_PARAM_L, &v))
		return false;

	return mgcp_lco_parse(v, lco);
}

bool mgcp_codec_next(struct sip_str *rest, struct sip_str *codec)
{
	struct sip_str it;
	size_t at;

	while (rest->len) {
		at = sep_find(*rest, ';');
		it = sip_str_trim(sip_str_init(rest->p, at));
		if (at < rest->len)
			at++;
		rest->p += at;
		rest->len -= at;
		if (!it.len)
			continue;
		*codec = it;
		return true;
	}

	return false;
}

static const char *const mode_names[] = {
	[MGCP_MODE_UNKNOWN] = "unknown",
	[MGCP_MODE_SENDONLY] = "sendonly",
	[MGCP_MODE_RECVONLY] = "recvonly",
	[MGCP_MODE_SENDRECV] = "sendrecv",
	[MGCP_MODE_CONFRNCE] = "confrnce",
	[MGCP_MODE_INACTIVE] = "inactive",
	[MGCP_MODE_LOOPBACK] = "loopback",
	[MGCP_MODE_CONTTEST] = "conttest",
	[MGCP_MODE_NETWLOOP] = "netwloop",
	[MGCP_MODE_NETWTEST] = "netwtest",
	[MGCP_MODE_EXTENSION] = "extension",
};

enum mgcp_mode mgcp_mode_parse(struct sip_str s)
{
	unsigned int i;

	s = sip_str_trim(s);
	for (i = MGCP_MODE_SENDONLY; i < MGCP_MODE_EXTENSION; i++) {
		if (sip_str_eq_ci(s, mode_names[i]))
			return (enum mgcp_mode)i;
	}
	if (s.len > 2 && memchr(s.p + 1, '/', s.len - 2))
		return MGCP_MODE_EXTENSION;

	return MGCP_MODE_UNKNOWN;
}

const char *mgcp_mode_name(enum mgcp_mode mode)
{
	if ((unsigned int)mode >= ARRAY_LEN(mode_names))
		return "unknown";

	return mode_names[mode];
}

bool mgcp_msg_mode(const struct mgcp_msg *msg, enum mgcp_mode *mode)
{
	struct sip_str v;

	if (!mgcp_msg_value(msg, MGCP_PARAM_M, &v))
		return false;

	*mode = mgcp_mode_parse(v);

	return *mode != MGCP_MODE_UNKNOWN;
}

bool mgcp_msg_digit_map(const struct mgcp_msg *msg, struct sip_str *out)
{
	return mgcp_msg_value(msg, MGCP_PARAM_D, out);
}

bool mgcp_quarantine_parse(struct sip_str s, struct mgcp_quarantine *q)
{
	struct sip_str rest = s, item;
	bool seen_loop = false, seen_proc = false;

	memset(q, 0, sizeof(*q));

	while (mgcp_list_next(&rest, &item)) {
		if (sip_str_eq_ci(item, "step") ||
		    sip_str_eq_ci(item, "loop")) {
			if (seen_loop)
				return false;
			seen_loop = true;
			q->loop = sip_str_eq_ci(item, "loop");
			continue;
		}
		if (sip_str_eq_ci(item, "process") ||
		    sip_str_eq_ci(item, "discard")) {
			if (seen_proc)
				return false;
			seen_proc = true;
			q->discard = sip_str_eq_ci(item, "discard");
			continue;
		}
		return false;
	}

	return seen_loop || seen_proc;
}

void mgcp_ack_init(struct mgcp_ack_iter *it, struct sip_str k)
{
	it->rest = k;
	it->bad = false;
}

bool mgcp_ack_next(struct mgcp_ack_iter *it, uint32_t *lo, uint32_t *hi)
{
	struct sip_str item;
	size_t dash;

	if (it->bad || !mgcp_list_next(&it->rest, &item))
		return false;

	for (dash = 0; dash < item.len && item.p[dash] != '-'; dash++)
		;
	if (dash == item.len) {
		if (mgcp_txid_parse(item, lo)) {
			*hi = *lo;
			return true;
		}
	} else if (mgcp_txid_parse(sip_str_trim(str_mid(item, 0, dash)), lo) &&
		   mgcp_txid_parse(sip_str_trim(str_mid(item, dash + 1,
							 item.len)), hi) &&
		   *lo <= *hi) {
		return true;
	}

	it->bad = true;

	return false;
}

bool mgcp_ack_covers(struct sip_str k, uint32_t txid)
{
	struct mgcp_ack_iter it;
	uint32_t lo, hi;

	mgcp_ack_init(&it, k);
	while (mgcp_ack_next(&it, &lo, &hi)) {
		if (txid >= lo && txid <= hi)
			return true;
	}

	return false;
}

bool mgcp_info_next(struct sip_str *rest, enum mgcp_param *code,
		    struct sip_str *name)
{
	struct sip_str item;

	if (!mgcp_list_next(rest, &item))
		return false;

	*name = item;
	*code = mgcp_param_parse(item);

	return true;
}

bool mgcp_msg_bearer(const struct mgcp_msg *msg, struct mgcp_bearer *b)
{
	struct sip_str v, item;
	size_t colon;

	memset(b, 0, sizeof(*b));
	if (!mgcp_msg_value(msg, MGCP_PARAM_B, &v))
		return false;

	while (mgcp_list_next(&v, &item)) {
		colon = sep_find(item, ':');
		if (!sip_str_eq_ci(sip_str_trim(str_mid(item, 0, colon)), "e"))
			continue;
		b->e = sip_str_trim(str_mid(item, colon + 1, item.len));
	}

	return true;
}

static const char *const rm_names[] = {
	[MGCP_RM_UNKNOWN] = "unknown",
	[MGCP_RM_GRACEFUL] = "graceful",
	[MGCP_RM_FORCED] = "forced",
	[MGCP_RM_RESTART] = "restart",
	[MGCP_RM_DISCONNECTED] = "disconnected",
	[MGCP_RM_CANCEL_GRACEFUL] = "cancel-graceful",
	[MGCP_RM_EXTENSION] = "extension",
};

enum mgcp_rm mgcp_rm_parse(struct sip_str s)
{
	unsigned int i;

	s = sip_str_trim(s);
	for (i = MGCP_RM_GRACEFUL; i < MGCP_RM_EXTENSION; i++) {
		if (sip_str_eq_ci(s, rm_names[i]))
			return (enum mgcp_rm)i;
	}
	if (s.len > 2 && memchr(s.p + 1, '/', s.len - 2))
		return MGCP_RM_EXTENSION;

	return MGCP_RM_UNKNOWN;
}

const char *mgcp_rm_name(enum mgcp_rm rm)
{
	if ((unsigned int)rm >= ARRAY_LEN(rm_names))
		return "unknown";

	return rm_names[rm];
}

bool mgcp_msg_rm(const struct mgcp_msg *msg, enum mgcp_rm *rm)
{
	struct sip_str v;

	if (!mgcp_msg_value(msg, MGCP_PARAM_RM, &v))
		return false;

	*rm = mgcp_rm_parse(v);

	return *rm != MGCP_RM_UNKNOWN;
}

bool mgcp_msg_rd(const struct mgcp_msg *msg, uint32_t *secs)
{
	struct sip_str v;

	return mgcp_msg_value(msg, MGCP_PARAM_RD, &v) &&
	       digits_parse(v, 6, secs);
}

bool mgcp_msg_md(const struct mgcp_msg *msg, uint32_t *bytes)
{
	struct sip_str v;

	return mgcp_msg_value(msg, MGCP_PARAM_MD, &v) &&
	       digits_parse(v, 9, bytes);
}

bool mgcp_reason_parse(struct sip_str s, struct mgcp_reason *r)
{
	struct sip_str rest = sip_str_trim(s), w;

	memset(r, 0, sizeof(*r));
	if (!sip_word_take(&rest, &w) || !code_word(w, &r->code))
		return false;

	rest = sip_str_trim(rest);
	if (rest.len && rest.p[0] == '/') {
		sip_word_take(&rest, &w);
		r->package = str_mid(w, 1, w.len);
		rest = sip_str_trim(rest);
	}
	r->comment = rest;

	return true;
}

bool mgcp_msg_reason(const struct mgcp_msg *msg, struct mgcp_reason *r)
{
	struct sip_str v;

	if (!mgcp_msg_value(msg, MGCP_PARAM_E, &v))
		return false;

	return mgcp_reason_parse(v, r);
}

static const struct {
	const char *name;
	uint32_t bit;
	size_t off;
} cp_fields[] = {
	{ "PS", MGCP_CP_PS, offsetof(struct mgcp_conn_params, ps) },
	{ "OS", MGCP_CP_OS, offsetof(struct mgcp_conn_params, os) },
	{ "PR", MGCP_CP_PR, offsetof(struct mgcp_conn_params, pr) },
	{ "OR", MGCP_CP_OR, offsetof(struct mgcp_conn_params, or_) },
	{ "PL", MGCP_CP_PL, offsetof(struct mgcp_conn_params, pl) },
	{ "JI", MGCP_CP_JI, offsetof(struct mgcp_conn_params, ji) },
	{ "LA", MGCP_CP_LA, offsetof(struct mgcp_conn_params, la) },
};

bool mgcp_conn_params_parse(struct sip_str s, struct mgcp_conn_params *cp)
{
	struct sip_str rest = s, item, name, value;
	uint32_t v;
	size_t eq, i;

	memset(cp, 0, sizeof(*cp));

	while (mgcp_list_next(&rest, &item)) {
		eq = sep_find(item, '=');
		if (eq >= item.len)
			return false;
		name = sip_str_trim(str_mid(item, 0, eq));
		value = sip_str_trim(str_mid(item, eq + 1, item.len));

		for (i = 0; i < ARRAY_LEN(cp_fields); i++) {
			if (!sip_str_eq_ci(name, cp_fields[i].name))
				continue;
			if (!digits_parse(value, 9, &v))
				return false;
			*(uint32_t *)((char *)cp + cp_fields[i].off) = v;
			cp->present |= cp_fields[i].bit;
			break;
		}
	}

	return true;
}

bool mgcp_msg_conn_params(const struct mgcp_msg *msg,
			  struct mgcp_conn_params *cp)
{
	struct sip_str v;

	if (!mgcp_msg_value(msg, MGCP_PARAM_P, &v))
		return false;

	return mgcp_conn_params_parse(v, cp);
}

bool mgcp_sdp_next(struct sip_str *rest, struct sip_str *one)
{
	const char *p = rest->p, *end = rest->p + rest->len, *start, *ls, *lf;

	while (p < end && is_eol(*p))
		p++;
	if (p >= end) {
		rest->p = end;
		rest->len = 0;
		return false;
	}

	start = p;
	ls = p;
	while (ls < end) {
		const char *ce;

		lf = memchr(ls, '\n', (size_t)(end - ls));
		ce = lf ? lf : end;
		if (ce > ls && ce[-1] == '\r')
			ce--;
		if (ce == ls) {
			*one = sip_str_init(start, (size_t)(ls - start));
			ls = lf ? lf + 1 : end;
			rest->p = ls;
			rest->len = (size_t)(end - ls);
			return true;
		}
		ls = lf ? lf + 1 : end;
	}

	*one = sip_str_init(start, (size_t)(end - start));
	rest->p = end;
	rest->len = 0;

	return true;
}

/* ------------------------------------------------------------- builders */

static bool text_valid(const char *s, bool allow_empty)
{
	const unsigned char *p;

	if (!s || (!allow_empty && !*s))
		return false;
	for (p = (const unsigned char *)s; *p; p++) {
		if ((*p < 0x20 && *p != '\t') || *p == 0x7f)
			return false;
	}

	return true;
}

static bool word_valid(const char *s)
{
	const char *p;

	if (!text_valid(s, false))
		return false;
	for (p = s; *p; p++) {
		if (is_wsp(*p))
			return false;
	}

	return true;
}

/* Appendix A responseString: %x20-7E */
static bool comment_valid(const char *s)
{
	const unsigned char *p;

	for (p = (const unsigned char *)s; *p; p++) {
		if (*p < 0x20 || *p > 0x7e)
			return false;
	}

	return true;
}

static bool txid_valid(uint32_t txid)
{
	return txid && txid <= MGCP_TXID_MAX;
}

struct buf_mark {
	size_t used;
	bool overflow;
};

static struct buf_mark mark_take(const struct sip_buf *b)
{
	struct buf_mark m = { b->used, b->overflow };

	return m;
}

/*
 * Every builder ends here. A refusal puts the buffer back in its state
 * before the call, the overflow flag included, so the caller finds no part
 * of a message and can write a shorter one into the same buffer.
 */
static bool build_end(struct sip_buf *b, const struct buf_mark *m, bool ok)
{
	if (ok && sip_buf_ok(b))
		return true;

	if (b->buf && m->used < b->size) {
		b->used = m->used;
		b->buf[m->used] = 0;
	}
	b->overflow = m->overflow;

	return false;
}

static void param_line_add(struct sip_buf *b, const char *name,
			   const char *value)
{
	sip_buf_add(b, name);
	sip_buf_add(b, ":");
	if (*value) {
		sip_buf_add(b, " ");
		sip_buf_add(b, value);
	}
	sip_buf_crlf(b);
}

static bool cmd_line_add(struct sip_buf *b, const char *verb, uint32_t txid,
			 const char *endpoint, const char *version)
{
	if (!txid_valid(txid) || !word_valid(endpoint) ||
	    !text_valid(version, false))
		return false;

	sip_buf_addf(b, "%s %u %s %s", verb, txid, endpoint, version);
	sip_buf_crlf(b);

	return true;
}

bool mgcp_ack_list_write(struct sip_buf *b, const struct mgcp_ack_list *acks)
{
	struct buf_mark m = mark_take(b);
	unsigned int i, j;

	if (!acks || !acks->count)
		return sip_buf_ok(b);
	if (acks->count > MGCP_ACK_LIST_MAX)
		return build_end(b, &m, false);

	for (i = 0; i < acks->count; i++) {
		if (!txid_valid(acks->txid[i]))
			return build_end(b, &m, false);
	}

	sip_buf_add(b, "K: ");
	for (i = 0; i < acks->count; i = j) {
		for (j = i + 1; j < acks->count; j++) {
			if (acks->txid[j] != acks->txid[j - 1] + 1)
				break;
		}
		if (i)
			sip_buf_add(b, ", ");
		if (j - i > 1)
			sip_buf_addf(b, "%u-%u", acks->txid[i],
				     acks->txid[j - 1]);
		else
			sip_buf_addf(b, "%u", acks->txid[i]);
	}
	sip_buf_crlf(b);

	return build_end(b, &m, true);
}

bool mgcp_ntfy_build(struct sip_buf *b, const struct mgcp_ntfy *n)
{
	struct buf_mark m = mark_take(b);
	bool ok;

	ok = n && cmd_line_add(b, "NTFY", n->txid, n->endpoint, n->version) &&
	     mgcp_ack_list_write(b, n->acks) &&
	     (!n->notified_entity || word_valid(n->notified_entity)) &&
	     n->request_id && mgcp_hex_id_valid(sip_str_cstr(n->request_id)) &&
	     text_valid(n->observed, true);
	if (ok) {
		if (n->notified_entity)
			param_line_add(b, "N", n->notified_entity);
		param_line_add(b, "X", n->request_id);
		param_line_add(b, "O", n->observed);
	}

	return build_end(b, &m, ok);
}

static bool reason_add(struct sip_buf *b, uint32_t reason, const char *text)
{
	if (!reason)
		return true;
	if (reason > 999 || (text && !text_valid(text, true)))
		return false;

	sip_buf_addf(b, "E: %03u", reason);
	if (text && *text) {
		sip_buf_add(b, " ");
		sip_buf_add(b, text);
	}
	sip_buf_crlf(b);

	return true;
}

bool mgcp_rsip_build(struct sip_buf *b, const struct mgcp_rsip *r)
{
	struct buf_mark m = mark_take(b);
	enum mgcp_rm rm;
	bool ok;

	ok = r && word_valid(r->method);
	if (ok) {
		rm = mgcp_rm_parse(sip_str_cstr(r->method));
		/* 2.3.12: a forced or cancel-graceful restart has no delay */
		if (r->has_delay && (rm == MGCP_RM_FORCED ||
				     rm == MGCP_RM_CANCEL_GRACEFUL))
			ok = false;
		if (r->has_delay && r->delay > 999999)
			ok = false;
	}
	ok = ok && cmd_line_add(b, "RSIP", r->txid, r->endpoint, r->version) &&
	     mgcp_ack_list_write(b, r->acks);
	if (ok) {
		param_line_add(b, "RM", r->method);
		if (r->has_delay)
			sip_buf_addf(b, "RD: %u\r\n", r->delay);
		ok = reason_add(b, r->reason, r->reason_text);
	}

	return build_end(b, &m, ok);
}

bool mgcp_conn_params_write(struct sip_buf *b,
			    const struct mgcp_conn_params *cp)
{
	struct buf_mark m = mark_take(b);
	bool first = true;
	uint32_t v;
	size_t i;

	if (!cp || !cp->present)
		return sip_buf_ok(b);

	sip_buf_add(b, "P: ");
	for (i = 0; i < ARRAY_LEN(cp_fields); i++) {
		if (!(cp->present & cp_fields[i].bit))
			continue;
		v = *(const uint32_t *)((const char *)cp + cp_fields[i].off);
		/* 3.2.2.7: a counter stops at its maximum, it never wraps */
		if (v > MGCP_CP_VALUE_MAX)
			v = MGCP_CP_VALUE_MAX;
		sip_buf_addf(b, "%s%s=%u", first ? "" : ", ",
			     cp_fields[i].name, v);
		first = false;
	}
	sip_buf_crlf(b);

	return build_end(b, &m, true);
}

bool mgcp_dlcx_build(struct sip_buf *b, const struct mgcp_dlcx *d)
{
	struct buf_mark m = mark_take(b);
	bool ok;

	ok = d && d->call_id && d->conn_id &&
	     mgcp_hex_id_valid(sip_str_cstr(d->call_id)) &&
	     mgcp_hex_id_valid(sip_str_cstr(d->conn_id)) &&
	     cmd_line_add(b, "DLCX", d->txid, d->endpoint, d->version) &&
	     mgcp_ack_list_write(b, d->acks);
	if (ok) {
		param_line_add(b, "C", d->call_id);
		param_line_add(b, "I", d->conn_id);
		ok = reason_add(b, d->reason, d->reason_text) &&
		     mgcp_conn_params_write(b, d->stats);
	}

	return build_end(b, &m, ok);
}

static const char *kv_name(const struct mgcp_param_kv *kv)
{
	return kv->code == MGCP_PARAM_UNKNOWN ? kv->name :
	       mgcp_param_name(kv->code);
}

/* 3.3: a parameter appears once in a response */
static bool kv_repeated(const struct mgcp_resp *r, unsigned int i)
{
	const char *name = kv_name(&r->params[i]);
	unsigned int j;

	if (r->ack_request && r->params[i].code == MGCP_PARAM_K)
		return true;

	for (j = 0; j < i; j++) {
		if (!strcasecmp(kv_name(&r->params[j]), name))
			return true;
	}

	return false;
}

static bool resp_params_add(struct sip_buf *b, const struct mgcp_resp *r)
{
	const struct mgcp_param_kv *kv;
	const char *name;
	unsigned int i;

	if (r->param_count && !r->params)
		return false;

	for (i = 0; i < r->param_count; i++) {
		kv = &r->params[i];
		name = kv_name(kv);
		if (kv->code >= MGCP_PARAM_MAX || !word_valid(name) ||
		    strchr(name, ':') || !text_valid(kv->value, true) ||
		    kv_repeated(r, i))
			return false;
		param_line_add(b, name, kv->value);
	}

	return true;
}

bool mgcp_resp_build(struct sip_buf *b, const struct mgcp_resp *r)
{
	struct buf_mark m = mark_take(b);
	bool ok;

	ok = r && r->code <= 999 && txid_valid(r->txid) &&
	     (!r->comment || comment_valid(r->comment)) &&
	     (!r->ack_request || r->code >= 200) &&
	     (!r->sdp_len || r->sdp);
	if (ok) {
		sip_buf_addf(b, "%03u %u", r->code, r->txid);
		if (r->comment && *r->comment) {
			sip_buf_add(b, " ");
			sip_buf_add(b, r->comment);
		}
		sip_buf_crlf(b);
		if (r->ack_request)
			param_line_add(b, "K", "");
		ok = resp_params_add(b, r);
	}
	if (ok && r->sdp_len) {
		sip_buf_crlf(b);
		sip_buf_add_len(b, r->sdp, r->sdp_len);
		/* Appendix A ends every SDP line with CRLF */
		if (r->sdp[r->sdp_len - 1] != '\n')
			sip_buf_crlf(b);
	}

	return build_end(b, &m, ok);
}

bool mgcp_sep_write(struct sip_buf *b)
{
	struct buf_mark m = mark_take(b);

	sip_buf_add(b, ".");
	sip_buf_crlf(b);

	return build_end(b, &m, true);
}
