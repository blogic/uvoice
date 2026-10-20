// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <ctype.h>
#include <string.h>
#include <strings.h>

#include "proto/shared/msg.h"

struct line_ctx {
	struct sip_msg *msg;
	char *p;
	char *end;
};

static bool is_lws(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

struct sip_str sip_str_init(const char *p, size_t len)
{
	struct sip_str s;

	s.p = p;
	s.len = len;

	return s;
}

struct sip_str sip_str_cstr(const char *s)
{
	return sip_str_init(s, s ? strlen(s) : 0);
}

struct sip_str sip_str_trim(struct sip_str s)
{
	while (s.len && is_lws(s.p[0])) {
		s.p++;
		s.len--;
	}
	while (s.len && is_lws(s.p[s.len - 1]))
		s.len--;

	return s;
}

bool sip_str_eq(struct sip_str s, const char *lit)
{
	size_t len = lit ? strlen(lit) : 0;

	return s.len == len && (!len || !memcmp(s.p, lit, len));
}

bool sip_str_eq_ci(struct sip_str s, const char *lit)
{
	size_t len = lit ? strlen(lit) : 0;

	return s.len == len && (!len || !strncasecmp(s.p, lit, len));
}

bool sip_str_prefix(struct sip_str s, const char *lit)
{
	size_t len = lit ? strlen(lit) : 0;

	return s.len >= len && (!len || !memcmp(s.p, lit, len));
}

bool sip_str_prefix_ci(struct sip_str s, const char *lit)
{
	size_t len = lit ? strlen(lit) : 0;

	return s.len >= len && (!len || !strncasecmp(s.p, lit, len));
}

bool sip_str_find_ci(struct sip_str s, const char *lit, size_t *at)
{
	size_t len = lit ? strlen(lit) : 0;
	size_t i;

	if (!len || s.len < len)
		return false;

	for (i = 0; i + len <= s.len; i++) {
		if (strncasecmp(s.p + i, lit, len))
			continue;
		if (at)
			*at = i;
		return true;
	}

	return false;
}

bool sip_str_uint(struct sip_str s, uint32_t *out)
{
	uint64_t v = 0;
	size_t i;

	if (!s.len)
		return false;

	for (i = 0; i < s.len; i++) {
		if (!isdigit((unsigned char)s.p[i]))
			return false;
		v = v * 10 + (uint64_t)(s.p[i] - '0');
		if (v > UINT32_MAX)
			return false;
	}

	*out = (uint32_t)v;

	return true;
}

bool sip_str_copy(char *dst, size_t dlen, struct sip_str s)
{
	size_t n;

	if (!dst || !dlen)
		return false;

	n = s.len < dlen - 1 ? s.len : dlen - 1;
	if (n)
		memcpy(dst, s.p, n);
	dst[n] = 0;

	return n == s.len;
}

/*
 * Offset of the first sep that is not inside a quoted string and not inside an
 * angle bracketed URI, or s.len. RFC 3261 25.1: a quoted string may contain
 * any octet, backslash escapes included, so the escape state has to be tracked
 * even to find a comma.
 */
size_t sip_sep_find(struct sip_str s, char sep)
{
	bool quoted = false, esc = false;
	unsigned int angle = 0;
	size_t i;

	for (i = 0; i < s.len; i++) {
		char c = s.p[i];

		if (esc) {
			esc = false;
			continue;
		}
		if (quoted) {
			if (c == '\\')
				esc = true;
			else if (c == '"')
				quoted = false;
			continue;
		}
		if (c == '"') {
			quoted = true;
			continue;
		}
		if (c == '<') {
			angle++;
			continue;
		}
		if (c == '>') {
			if (angle)
				angle--;
			continue;
		}
		if (c == sep && !angle)
			return i;
	}

	return s.len;
}

static bool split_next(struct sip_str *rest, struct sip_str *item, char sep)
{
	size_t at;

	if (!rest->len)
		return false;

	at = sip_sep_find(*rest, sep);
	*item = sip_str_trim(sip_str_init(rest->p, at));
	if (at < rest->len)
		at++;
	rest->p += at;
	rest->len -= at;

	return true;
}

bool sip_csv_next(struct sip_str *rest, struct sip_str *item)
{
	return split_next(rest, item, ',');
}

bool sip_value_split(struct sip_str v, struct sip_str *head, struct sip_str *params)
{
	size_t at = sip_sep_find(v, ';');

	if (head)
		*head = sip_str_trim(sip_str_init(v.p, at));
	if (params)
		*params = sip_str_init(v.p + at, v.len - at);

	return at < v.len;
}

bool sip_kv_split(struct sip_str item, struct sip_str *name, struct sip_str *val)
{
	size_t at = sip_sep_find(item, '=');

	*name = sip_str_trim(sip_str_init(item.p, at));
	if (at >= item.len) {
		*val = sip_str_init(item.p + item.len, 0);
		return false;
	}

	/* RFC 4475 3.1.1.1 sends "q = 0.33" and "stale = TRUE", so both sides
	 * are trimmed before the quotes come off. */
	*val = sip_quoted_strip(sip_str_trim(sip_str_init(item.p + at + 1,
							  item.len - at - 1)));

	return true;
}

bool sip_param_next(struct sip_str *rest, struct sip_str *name, struct sip_str *val)
{
	struct sip_str item;

	for (;;) {
		if (!split_next(rest, &item, ';'))
			return false;
		if (item.len)
			break;
	}

	sip_kv_split(item, name, val);

	return true;
}

bool sip_param_find(struct sip_str params, const char *name, struct sip_str *val)
{
	struct sip_str rest = params, n, v;

	while (sip_param_next(&rest, &n, &v)) {
		if (!sip_str_eq_ci(n, name))
			continue;
		if (val)
			*val = v;
		return true;
	}

	return false;
}

bool sip_param_present(struct sip_str params, const char *name)
{
	return sip_param_find(params, name, NULL);
}

struct sip_str sip_quoted_strip(struct sip_str s)
{
	if (s.len >= 2 && s.p[0] == '"' && s.p[s.len - 1] == '"') {
		s.p++;
		s.len -= 2;
	}

	return s;
}

int sip_unquote(struct sip_str in, char *out, size_t olen)
{
	size_t i, used = 0;

	if (!out || !olen)
		return -1;

	in = sip_quoted_strip(in);
	for (i = 0; i < in.len; i++) {
		char c = in.p[i];

		if (c == '\\' && i + 1 < in.len)
			c = in.p[++i];
		if (used + 1 >= olen)
			return -1;
		out[used++] = c;
	}
	out[used] = 0;

	return (int)used;
}

static int hex_val(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return -1;
}

int sip_uri_unescape(struct sip_str in, char *out, size_t olen)
{
	size_t i, used = 0;

	if (!out || !olen)
		return -1;

	for (i = 0; i < in.len; i++) {
		char c = in.p[i];
		int hi, lo;

		if (c == '%' && i + 2 < in.len) {
			hi = hex_val(in.p[i + 1]);
			lo = hex_val(in.p[i + 2]);
			if (hi >= 0 && lo >= 0) {
				c = (char)((hi << 4) | lo);
				i += 2;
			}
		}
		if (used + 1 >= olen)
			return -1;
		out[used++] = c;
	}
	out[used] = 0;

	return (int)used;
}

struct sip_str sip_media_type(struct sip_str ctype)
{
	struct sip_str head;

	sip_value_split(ctype, &head, NULL);

	return head;
}

struct sip_method_map {
	const char *name;
	enum sip_method method;
};

/* RFC 3261 7.1: the method is case sensitive, so every compare here is exact. */
static const struct sip_method_map methods[] = {
	{ "INVITE",	SIP_METHOD_INVITE },
	{ "ACK",	SIP_METHOD_ACK },
	{ "CANCEL",	SIP_METHOD_CANCEL },
	{ "BYE",	SIP_METHOD_BYE },
	{ "REGISTER",	SIP_METHOD_REGISTER },
	{ "OPTIONS",	SIP_METHOD_OPTIONS },
	{ "INFO",	SIP_METHOD_INFO },
	{ "NOTIFY",	SIP_METHOD_NOTIFY },
	{ "SUBSCRIBE",	SIP_METHOD_SUBSCRIBE },
	{ "UPDATE",	SIP_METHOD_UPDATE },
	{ "REFER",	SIP_METHOD_REFER },
	{ "PRACK",	SIP_METHOD_PRACK },
	{ "MESSAGE",	SIP_METHOD_MESSAGE },
};

const char *sip_method_name(enum sip_method method)
{
	size_t i;

	for (i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
		if (methods[i].method == method)
			return methods[i].name;
	}

	return "UNKNOWN";
}

enum sip_method sip_method_parse(struct sip_str s)
{
	size_t i;

	for (i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
		if (sip_str_eq(s, methods[i].name))
			return methods[i].method;
	}

	return SIP_METHOD_UNKNOWN;
}

struct hdr_name_map {
	const char *name;
	char compact;
	enum sip_hdr_id id;
};

/*
 * Compact forms: RFC 3261 table 1 plus o and u (RFC 3265), r and b (RFC 3515,
 * RFC 3892) and x (RFC 4028). Receive accepts every one of them; the builders
 * always emit the long form.
 */
static const struct hdr_name_map hdr_names[] = {
	{ "Via",			'v', SIP_HDR_VIA },
	{ "From",			'f', SIP_HDR_FROM },
	{ "To",				't', SIP_HDR_TO },
	{ "Contact",			'm', SIP_HDR_CONTACT },
	{ "Call-ID",			'i', SIP_HDR_CALL_ID },
	{ "CSeq",			 0,  SIP_HDR_CSEQ },
	{ "Route",			 0,  SIP_HDR_ROUTE },
	{ "Record-Route",		 0,  SIP_HDR_RECORD_ROUTE },
	/* RFC 3608 defines no compact form for Service-Route. */
	{ "Service-Route",		 0,  SIP_HDR_SERVICE_ROUTE },
	{ "Max-Forwards",		 0,  SIP_HDR_MAX_FORWARDS },
	{ "Expires",			 0,  SIP_HDR_EXPIRES },
	{ "Min-Expires",		 0,  SIP_HDR_MIN_EXPIRES },
	{ "Content-Type",		'c', SIP_HDR_CONTENT_TYPE },
	{ "Content-Length",		'l', SIP_HDR_CONTENT_LENGTH },
	{ "Content-Encoding",		'e', SIP_HDR_CONTENT_ENCODING },
	{ "Content-Disposition",	 0,  SIP_HDR_CONTENT_DISPOSITION },
	{ "WWW-Authenticate",		 0,  SIP_HDR_WWW_AUTHENTICATE },
	{ "Proxy-Authenticate",		 0,  SIP_HDR_PROXY_AUTHENTICATE },
	{ "Authorization",		 0,  SIP_HDR_AUTHORIZATION },
	{ "Proxy-Authorization",	 0,  SIP_HDR_PROXY_AUTHORIZATION },
	{ "Authentication-Info",	 0,  SIP_HDR_AUTHENTICATION_INFO },
	{ "Supported",			'k', SIP_HDR_SUPPORTED },
	{ "Require",			 0,  SIP_HDR_REQUIRE },
	{ "Proxy-Require",		 0,  SIP_HDR_PROXY_REQUIRE },
	{ "Unsupported",		 0,  SIP_HDR_UNSUPPORTED },
	{ "Allow",			 0,  SIP_HDR_ALLOW },
	{ "Allow-Events",		'u', SIP_HDR_ALLOW_EVENTS },
	{ "Event",			'o', SIP_HDR_EVENT },
	{ "Subscription-State",		 0,  SIP_HDR_SUBSCRIPTION_STATE },
	{ "Alert-Info",			 0,  SIP_HDR_ALERT_INFO },
	{ "Session-Expires",		'x', SIP_HDR_SESSION_EXPIRES },
	{ "Min-SE",			 0,  SIP_HDR_MIN_SE },
	{ "RSeq",			 0,  SIP_HDR_RSEQ },
	{ "RAck",			 0,  SIP_HDR_RACK },
	{ "Retry-After",		 0,  SIP_HDR_RETRY_AFTER },
	{ "P-Asserted-Identity",	 0,  SIP_HDR_P_ASSERTED_IDENTITY },
	{ "P-Preferred-Identity",	 0,  SIP_HDR_P_PREFERRED_IDENTITY },
	{ "Remote-Party-ID",		 0,  SIP_HDR_REMOTE_PARTY_ID },
	{ "Privacy",			 0,  SIP_HDR_PRIVACY },
	{ "Refer-To",			'r', SIP_HDR_REFER_TO },
	{ "Referred-By",		'b', SIP_HDR_REFERRED_BY },
	{ "Replaces",			 0,  SIP_HDR_REPLACES },
	{ "Subject",			's', SIP_HDR_SUBJECT },
	{ "Accept",			 0,  SIP_HDR_ACCEPT },
	{ "User-Agent",			 0,  SIP_HDR_USER_AGENT },
	{ "Server",			 0,  SIP_HDR_SERVER },
	{ "Date",			 0,  SIP_HDR_DATE },
	{ "Timestamp",			 0,  SIP_HDR_TIMESTAMP },
	{ "Warning",			 0,  SIP_HDR_WARNING },
	{ "Reason",			 0,  SIP_HDR_REASON },
};

const char *sip_hdr_name(enum sip_hdr_id id)
{
	size_t i;

	for (i = 0; i < sizeof(hdr_names) / sizeof(hdr_names[0]); i++) {
		if (hdr_names[i].id == id)
			return hdr_names[i].name;
	}

	return "";
}

enum sip_hdr_id sip_hdr_id_parse(struct sip_str name)
{
	size_t i;

	if (!name.len)
		return SIP_HDR_UNKNOWN;

	/* RFC 3261 7.3.1: header names, compact forms included, are case
	 * insensitive. */
	if (name.len == 1) {
		char c = name.p[0] | 0x20;

		for (i = 0; i < sizeof(hdr_names) / sizeof(hdr_names[0]); i++) {
			if (hdr_names[i].compact && hdr_names[i].compact == c)
				return hdr_names[i].id;
		}
		return SIP_HDR_UNKNOWN;
	}

	for (i = 0; i < sizeof(hdr_names) / sizeof(hdr_names[0]); i++) {
		if (sip_str_eq_ci(name, hdr_names[i].name))
			return hdr_names[i].id;
	}

	return SIP_HDR_UNKNOWN;
}

const char *sip_parse_status_name(enum sip_parse_status st)
{
	switch (st) {
	case SIP_PARSE_OK:
		return "ok";
	case SIP_PARSE_ERR_EMPTY:
		return "empty";
	case SIP_PARSE_ERR_TOO_LARGE:
		return "too large";
	case SIP_PARSE_ERR_START_LINE:
		return "bad start line";
	case SIP_PARSE_ERR_STATUS:
		return "bad status code";
	case SIP_PARSE_ERR_HDR_OVERFLOW:
		return "too many headers";
	case SIP_PARSE_ERR_HDR_TRUNCATED:
		return "truncated header section";
	}

	return "unknown";
}

static char *eol_find(struct line_ctx *lc, char *p, char **next)
{
	char *lf = memchr(p, '\n', (size_t)(lc->end - p));

	if (!lf) {
		*next = lc->end;
		return lc->end;
	}

	*next = lf + 1;
	if (lf > p && lf[-1] == '\r')
		return lf - 1;

	lc->msg->flags |= SIP_MSG_F_BARE_LF;

	return lf;
}

static bool line_take(struct line_ctx *lc, struct sip_str *out, bool *term)
{
	char *next, *ce;

	if (lc->p >= lc->end)
		return false;

	ce = eol_find(lc, lc->p, &next);
	*term = next > ce;
	*out = sip_str_init(lc->p, (size_t)(ce - lc->p));
	lc->p = next;

	return true;
}

/*
 * One header field, folds undone in place. RFC 3261 7.3.1 allows a header
 * value to continue on the next line when that line starts with whitespace;
 * overwriting the CR and the LF with spaces keeps the value contiguous and
 * costs no copy.
 */
static bool hdr_line_take(struct line_ctx *lc, struct sip_str *out, bool *blank, bool *term,
			  bool *folded)
{
	char *start = lc->p, *next, *ce, *f;

	*folded = false;
	if (lc->p >= lc->end)
		return false;

	ce = eol_find(lc, start, &next);
	*term = next > ce;
	if (ce == start) {
		*blank = true;
		*out = sip_str_init(start, 0);
		lc->p = next;
		return true;
	}

	*blank = false;
	while (next < lc->end && (*next == ' ' || *next == '\t')) {
		lc->msg->flags |= SIP_MSG_F_FOLDED;
		*folded = true;
		for (f = ce; f < next; f++)
			*f = ' ';
		ce = eol_find(lc, next, &next);
		*term = next > ce;
	}

	*out = sip_str_init(start, (size_t)(ce - start));
	lc->p = next;

	return true;
}

bool sip_word_take(struct sip_str *rest, struct sip_str *word)
{
	size_t i = 0;

	while (i < rest->len && is_lws(rest->p[i]))
		i++;
	rest->p += i;
	rest->len -= i;
	if (!rest->len)
		return false;

	for (i = 0; i < rest->len; i++) {
		if (is_lws(rest->p[i]))
			break;
	}
	*word = sip_str_init(rest->p, i);
	rest->p += i;
	rest->len -= i;

	return true;
}

static enum sip_parse_status start_line_parse(struct sip_msg *msg, struct sip_str line)
{
	struct sip_str rest = sip_str_trim(line), w1, w2, w3;

	if (!rest.len)
		return SIP_PARSE_ERR_START_LINE;

	if (sip_str_prefix_ci(rest, "SIP/")) {
		if (!sip_word_take(&rest, &w1) || !sip_word_take(&rest, &w2))
			return SIP_PARSE_ERR_START_LINE;
		if (w2.len != 3 || !sip_str_uint(w2, &msg->status))
			return SIP_PARSE_ERR_STATUS;
		if (msg->status < 100 || msg->status > 699)
			return SIP_PARSE_ERR_STATUS;

		msg->is_request = false;
		msg->version = w1;
		msg->reason = sip_str_trim(rest);
		if (!sip_str_eq(w1, "SIP/2.0"))
			msg->flags |= SIP_MSG_F_BAD_VERSION;
		return SIP_PARSE_OK;
	}

	if (!sip_word_take(&rest, &w1) || !sip_word_take(&rest, &w2) || !sip_word_take(&rest, &w3))
		return SIP_PARSE_ERR_START_LINE;
	if (sip_str_trim(rest).len)
		return SIP_PARSE_ERR_START_LINE;
	if (!sip_str_prefix_ci(w3, "SIP/"))
		return SIP_PARSE_ERR_START_LINE;

	msg->is_request = true;
	msg->method_name = w1;
	msg->method = sip_method_parse(w1);
	msg->ruri = w2;
	msg->version = w3;
	if (!sip_str_eq(w3, "SIP/2.0"))
		msg->flags |= SIP_MSG_F_BAD_VERSION;

	return SIP_PARSE_OK;
}

static enum sip_parse_status hdr_add(struct sip_msg *msg, struct sip_str line, uint32_t flags)
{
	struct sip_hdr *h;
	const char *colon;

	if (msg->hdr_count >= SIP_HDR_COUNT_MAX)
		return SIP_PARSE_ERR_HDR_OVERFLOW;

	h = &msg->hdr[msg->hdr_count++];
	memset(h, 0, sizeof(*h));
	h->flags = flags;

	if (line.len > SIP_HDR_LEN_MAX) {
		h->flags |= SIP_HDR_F_OVERSIZE;
		msg->flags |= SIP_MSG_F_HDR_OVERSIZE;
	}

	colon = memchr(line.p, ':', line.len);
	if (!colon) {
		h->name = sip_str_trim(line);
		h->value = sip_str_init(line.p + line.len, 0);
		h->flags |= SIP_HDR_F_MALFORMED;
		msg->flags |= SIP_MSG_F_HDR_MALFORMED;
		return SIP_PARSE_OK;
	}

	/* RFC 4475 3.1.1.1 sends "To :" and "Content-Length   :", so the name
	 * is trimmed before it is looked up. */
	h->name = sip_str_trim(sip_str_init(line.p, (size_t)(colon - line.p)));
	h->value = sip_str_trim(sip_str_init(colon + 1,
					     line.len - (size_t)(colon - line.p) - 1));
	h->id = sip_hdr_id_parse(h->name);
	if (!h->name.len) {
		h->flags |= SIP_HDR_F_MALFORMED;
		msg->flags |= SIP_MSG_F_HDR_MALFORMED;
	}

	return SIP_PARSE_OK;
}

static void body_set(struct sip_msg *msg, const char *body, size_t avail)
{
	struct sip_str v;
	uint32_t clen;

	msg->body = sip_str_init(body, avail);

	if (!msg_hdr_value(msg, SIP_HDR_CONTENT_LENGTH, &v)) {
		msg->flags |= SIP_MSG_F_NO_CLEN;
		return;
	}
	if (!sip_str_uint(v, &clen)) {
		msg->flags |= SIP_MSG_F_BAD_CLEN;
		return;
	}

	msg->clen = clen;
	msg->clen_present = true;

	/* The UDP rule: trust the shorter of Content-Length and the rest of the
	 * datagram. A longer declaration is a truncated or lying sender, a
	 * shorter one leaves trailing bytes we ignore. */
	if (clen > avail) {
		msg->flags |= SIP_MSG_F_BODY_SHORT;
		return;
	}
	if (clen < avail)
		msg->flags |= SIP_MSG_F_BODY_EXTRA;
	msg->body.len = clen;
}

enum sip_parse_status msg_parse(struct sip_msg *msg, char *buf, size_t len)
{
	struct line_ctx lc;
	struct sip_str line;
	enum sip_parse_status st;
	bool blank = false, term = false, folded = false;

	if (!msg)
		return SIP_PARSE_ERR_EMPTY;

	memset(msg, 0, sizeof(*msg));
	if (!buf || !len)
		return SIP_PARSE_ERR_EMPTY;
	if (len > SIP_MSG_LEN_MAX)
		return SIP_PARSE_ERR_TOO_LARGE;

	msg->buf = buf;
	msg->len = len;

	lc.msg = msg;
	lc.p = buf;
	lc.end = buf + len;

	/* RFC 3261 7.5 and the CRLF NAT keepalive: leading empty lines are
	 * skipped, a datagram that holds nothing else is the keepalive. */
	while (lc.p < lc.end && (*lc.p == '\r' || *lc.p == '\n')) {
		msg->flags |= SIP_MSG_F_LEAD_CRLF;
		lc.p++;
	}
	if (lc.p >= lc.end)
		return SIP_PARSE_ERR_EMPTY;

	/* An unterminated start line means a truncated datagram: every real
	 * message carries Via, From, To, Call-ID and CSeq behind it. */
	if (!line_take(&lc, &line, &term) || !term)
		return SIP_PARSE_ERR_START_LINE;

	st = start_line_parse(msg, line);
	if (st != SIP_PARSE_OK)
		return st;

	for (;;) {
		if (!hdr_line_take(&lc, &line, &blank, &term, &folded)) {
			/* No blank line. A last header that ended with CRLF is
			 * a sender that omitted the terminator; an unterminated
			 * one is a truncated datagram we must not act on. */
			if (msg->hdr_count && !term)
				return SIP_PARSE_ERR_HDR_TRUNCATED;
			msg->flags |= SIP_MSG_F_NO_BODY_SEP;
			msg->body = sip_str_init(lc.end, 0);
			return SIP_PARSE_OK;
		}
		if (blank)
			break;

		st = hdr_add(msg, line, folded ? SIP_HDR_F_FOLDED : 0);
		if (st != SIP_PARSE_OK)
			return st;
	}

	body_set(msg, lc.p, (size_t)(lc.end - lc.p));

	return SIP_PARSE_OK;
}

const struct sip_hdr *msg_hdr_first(const struct sip_msg *msg, enum sip_hdr_id id)
{
	unsigned int i;

	if (id == SIP_HDR_UNKNOWN)
		return NULL;

	for (i = 0; i < msg->hdr_count; i++) {
		if (msg->hdr[i].id == id)
			return &msg->hdr[i];
	}

	return NULL;
}

const struct sip_hdr *msg_hdr_next(const struct sip_msg *msg, const struct sip_hdr *cur)
{
	unsigned int i;

	if (!cur)
		return NULL;

	i = (unsigned int)(cur - msg->hdr) + 1;
	for (; i < msg->hdr_count; i++) {
		if (cur->id != SIP_HDR_UNKNOWN) {
			if (msg->hdr[i].id == cur->id)
				return &msg->hdr[i];
			continue;
		}
		if (msg->hdr[i].id == SIP_HDR_UNKNOWN &&
		    msg->hdr[i].name.len == cur->name.len &&
		    !strncasecmp(msg->hdr[i].name.p, cur->name.p, cur->name.len))
			return &msg->hdr[i];
	}

	return NULL;
}

const struct sip_hdr *msg_hdr_name_first(const struct sip_msg *msg, const char *name)
{
	unsigned int i;

	for (i = 0; i < msg->hdr_count; i++) {
		if (sip_str_eq_ci(msg->hdr[i].name, name))
			return &msg->hdr[i];
	}

	return NULL;
}

unsigned int msg_hdr_count(const struct sip_msg *msg, enum sip_hdr_id id)
{
	unsigned int i, n = 0;

	for (i = 0; i < msg->hdr_count; i++) {
		if (msg->hdr[i].id == id)
			n++;
	}

	return n;
}

bool msg_hdr_value(const struct sip_msg *msg, enum sip_hdr_id id, struct sip_str *out)
{
	const struct sip_hdr *h = msg_hdr_first(msg, id);

	if (!h)
		return false;
	if (out)
		*out = h->value;

	return true;
}

bool msg_body_get(const struct sip_msg *msg, struct sip_str *out)
{
	if (out)
		*out = msg->body;

	return msg->body.len > 0;
}

bool sip_ctype_boundary(struct sip_str ctype, struct sip_str *out)
{
	struct sip_str params, val;

	sip_value_split(ctype, NULL, &params);
	if (!sip_param_find(params, "boundary", &val) || !val.len)
		return false;
	if (out)
		*out = val;

	return true;
}

static const char *line_start_next(const char *p, const char *end)
{
	const char *lf = memchr(p, '\n', (size_t)(end - p));

	return lf ? lf + 1 : end;
}

/*
 * A delimiter is a whole line (RFC 2046 5.1.1): two hyphens, the boundary, an
 * optional second pair of hyphens for the terminator, transport padding, then
 * the line end. Matching the prefix alone would cut a part in half at a body
 * line like "--xNOTBOUNDARY" when the boundary is "x".
 */
static bool delim_at(const char *p, const char *end, struct sip_str b)
{
	const char *q;

	if ((size_t)(end - p) < b.len + 2)
		return false;
	if (p[0] != '-' || p[1] != '-')
		return false;
	if (memcmp(p + 2, b.p, b.len))
		return false;

	/* q is formed only after the length check, so it never runs past the
	 * end of the buffer. */
	q = p + 2 + b.len;
	if (end - q >= 2 && q[0] == '-' && q[1] == '-')
		q += 2;
	while (q < end && (*q == ' ' || *q == '\t'))
		q++;

	return q >= end || *q == '\r' || *q == '\n';
}

static bool delim_close(const char *p, const char *end, struct sip_str b)
{
	const char *q;

	if ((size_t)(end - p) < b.len + 2)
		return false;

	q = p + 2 + b.len;

	return end - q >= 2 && q[0] == '-' && q[1] == '-';
}

static void part_ctype_find(struct sip_part *part)
{
	const char *p = part->hdrs.p, *end = part->hdrs.p + part->hdrs.len;
	const char *next, *ce, *colon;
	struct sip_str name;

	while (p < end) {
		next = line_start_next(p, end);
		ce = next;
		if (ce > p && ce[-1] == '\n')
			ce--;
		if (ce > p && ce[-1] == '\r')
			ce--;

		colon = memchr(p, ':', (size_t)(ce - p));
		if (colon) {
			name = sip_str_trim(sip_str_init(p, (size_t)(colon - p)));
			if (sip_str_eq_ci(name, "Content-Type") || sip_str_eq_ci(name, "c")) {
				part->ctype = sip_str_trim(
					sip_str_init(colon + 1, (size_t)(ce - colon - 1)));
				return;
			}
		}
		p = next;
	}
}

static void part_split(struct sip_str raw, struct sip_part *part)
{
	const char *p = raw.p, *end = raw.p + raw.len, *ls = raw.p, *next, *ce;

	memset(part, 0, sizeof(*part));
	part->hdrs = sip_str_init(p, 0);
	part->body = sip_str_init(end, 0);

	while (ls < end) {
		next = line_start_next(ls, end);
		ce = next;
		if (ce > ls && ce[-1] == '\n')
			ce--;
		if (ce > ls && ce[-1] == '\r')
			ce--;

		if (ce == ls) {
			part->hdrs = sip_str_init(p, (size_t)(ls - p));
			part->body = sip_str_init(next, (size_t)(end - next));
			part_ctype_find(part);
			return;
		}
		ls = next;
	}

	/* No blank line. A part that starts with a header keeps its whole body
	 * in the header block; one that does not (an SDP part opens with v=0)
	 * is treated as a body, which is what a sender that forgot the blank
	 * line meant. */
	if (memchr(raw.p, ':', raw.len)) {
		part->hdrs = raw;
		part_ctype_find(part);
		return;
	}
	part->body = raw;
}

bool part_next(struct sip_part_iter *it, struct sip_part *part)
{
	const char *p, *ls, *stop = NULL, *ce;

	memset(part, 0, sizeof(*part));
	if (it->done || !it->cur || it->cur >= it->end)
		return false;

	p = it->cur;
	ls = p;
	while (ls < it->end) {
		if (delim_at(ls, it->end, it->boundary)) {
			stop = ls;
			break;
		}
		ls = line_start_next(ls, it->end);
	}

	ce = stop ? stop : it->end;
	/* RFC 2046 5.1.1: the CRLF in front of a delimiter belongs to the
	 * delimiter, not to the part. */
	if (stop) {
		if (ce > p && ce[-1] == '\n')
			ce--;
		if (ce > p && ce[-1] == '\r')
			ce--;
	}

	part_split(sip_str_init(p, (size_t)(ce - p)), part);

	if (!stop) {
		it->done = true;
		it->cur = it->end;
		return true;
	}

	it->cur = line_start_next(stop, it->end);
	if (delim_close(stop, it->end, it->boundary))
		it->done = true;

	return true;
}

bool part_first(struct sip_str body, struct sip_str boundary, struct sip_part_iter *it,
		struct sip_part *part)
{
	const char *p = body.p, *end = body.p + body.len;

	memset(it, 0, sizeof(*it));
	memset(part, 0, sizeof(*part));
	it->done = true;
	it->boundary = boundary;
	it->end = end;

	if (!boundary.len || !body.len)
		return false;

	while (p < end) {
		if (delim_at(p, end, boundary))
			break;
		p = line_start_next(p, end);
	}
	if (p >= end)
		return false;
	if (delim_close(p, end, boundary))
		return false;

	it->cur = line_start_next(p, end);
	it->done = false;

	return part_next(it, part);
}

bool msg_part_first(const struct sip_msg *msg, struct sip_part_iter *it, struct sip_part *part)
{
	struct sip_str ctype, boundary, body;

	memset(it, 0, sizeof(*it));
	memset(part, 0, sizeof(*part));
	it->done = true;

	if (!msg_hdr_value(msg, SIP_HDR_CONTENT_TYPE, &ctype))
		return false;
	if (!sip_str_prefix_ci(sip_media_type(ctype), "multipart/"))
		return false;
	if (!sip_ctype_boundary(ctype, &boundary))
		return false;
	if (!msg_body_get(msg, &body))
		return false;

	return part_first(body, boundary, it, part);
}

static bool sdp_part_find(struct sip_str body, struct sip_str ctype, struct sip_str *out,
			  unsigned int depth)
{
	struct sip_part_iter it;
	struct sip_part part;
	struct sip_str boundary, type;

	if (!sip_ctype_boundary(ctype, &boundary))
		return false;
	if (!part_first(body, boundary, &it, &part))
		return false;

	do {
		type = sip_media_type(part.ctype);
		if (sip_str_eq_ci(type, "application/sdp")) {
			*out = part.body;
			return true;
		}
		if (depth && sip_str_prefix_ci(type, "multipart/") &&
		    sdp_part_find(part.body, part.ctype, out, depth - 1))
			return true;
	} while (part_next(&it, &part));

	return false;
}

bool msg_body_sdp_get(const struct sip_msg *msg, struct sip_str *out)
{
	struct sip_str ctype, body, type;

	if (!msg_body_get(msg, &body))
		return false;

	if (!msg_hdr_value(msg, SIP_HDR_CONTENT_TYPE, &ctype)) {
		/* A body with no Content-Type is malformed, but a session
		 * description is recognisable and refusing it would drop the
		 * call for a missing header we do not otherwise need. */
		if (!sip_str_prefix(body, "v=0"))
			return false;
		*out = body;
		return true;
	}

	type = sip_media_type(ctype);
	if (sip_str_eq_ci(type, "application/sdp")) {
		*out = body;
		return true;
	}
	if (!sip_str_prefix_ci(type, "multipart/"))
		return false;

	/* One level of nesting only. */
	return sdp_part_find(body, ctype, out, 1);
}
