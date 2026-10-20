// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "proto/shared/hdr.h"

void sip_buf_init(struct sip_buf *b, char *mem, size_t size)
{
	b->buf = mem;
	b->size = size;
	b->used = 0;
	b->overflow = !mem || !size;
	if (mem && size)
		mem[0] = 0;
}

bool sip_buf_add_len(struct sip_buf *b, const char *s, size_t len)
{
	if (b->overflow)
		return false;
	if (!s || !len)
		return true;

	/* b->used < b->size holds whenever the buffer is not in overflow, so
	 * capacity is compared by subtraction. Adding len first would wrap for
	 * a length near SIZE_MAX and let the memcpy through. */
	if (len >= b->size - b->used) {
		b->overflow = true;
		return false;
	}

	memcpy(b->buf + b->used, s, len);
	b->used += len;
	b->buf[b->used] = 0;

	return true;
}

bool sip_buf_add(struct sip_buf *b, const char *s)
{
	return sip_buf_add_len(b, s, s ? strlen(s) : 0);
}

bool sip_buf_add_str(struct sip_buf *b, struct sip_str s)
{
	return sip_buf_add_len(b, s.p, s.len);
}

bool sip_buf_addf(struct sip_buf *b, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (b->overflow)
		return false;

	va_start(ap, fmt);
	n = vsnprintf(b->buf + b->used, b->size - b->used, fmt, ap);
	va_end(ap);

	if (n < 0 || (size_t)n >= b->size - b->used) {
		b->buf[b->used] = 0;
		b->overflow = true;
		return false;
	}
	b->used += (size_t)n;

	return true;
}

bool sip_buf_add_quoted(struct sip_buf *b, const char *s)
{
	const char *p;

	if (!sip_buf_add(b, "\""))
		return false;

	for (p = s; p && *p; p++) {
		if ((*p == '"' || *p == '\\') && !sip_buf_add_len(b, "\\", 1))
			return false;
		if (!sip_buf_add_len(b, p, 1))
			return false;
	}

	return sip_buf_add(b, "\"");
}

bool sip_buf_crlf(struct sip_buf *b)
{
	return sip_buf_add_len(b, "\r\n", 2);
}

bool sip_buf_ok(const struct sip_buf *b)
{
	return !b->overflow;
}

size_t sip_buf_len(const struct sip_buf *b)
{
	return b->used;
}

const char *sip_buf_str(const struct sip_buf *b)
{
	return b->overflow ? NULL : b->buf;
}

static bool hostport_parse(struct sip_str s, struct sip_str *host, uint32_t *port, bool *ipv6,
			   bool *has_port)
{
	const char *colon, *rb;
	struct sip_str p;

	*ipv6 = false;
	*has_port = false;
	*port = 0;

	s = sip_str_trim(s);
	if (!s.len)
		return false;

	if (s.p[0] == '[') {
		rb = memchr(s.p, ']', s.len);
		if (!rb || rb == s.p + 1)
			return false;
		*host = sip_str_init(s.p + 1, (size_t)(rb - s.p) - 1);
		*ipv6 = true;
		p = sip_str_init(rb + 1, s.len - (size_t)(rb + 1 - s.p));
		if (!p.len)
			return true;
		if (p.p[0] != ':')
			return false;
		p = sip_str_init(p.p + 1, p.len - 1);
	} else {
		colon = memchr(s.p, ':', s.len);
		if (!colon) {
			*host = s;
			return true;
		}
		*host = sip_str_init(s.p, (size_t)(colon - s.p));
		p = sip_str_init(colon + 1, s.len - (size_t)(colon - s.p) - 1);
	}

	if (!host->len)
		return false;

	p = sip_str_trim(p);
	if (!sip_str_uint(p, port) || *port > 65535)
		return false;
	*has_port = true;

	return true;
}

bool uri_parse(struct sip_str s, struct sip_uri *uri)
{
	struct sip_str rest, region, hostport, head, val;
	const char *colon, *at, *q, *pw;
	bool ipv6, has_port;

	memset(uri, 0, sizeof(*uri));

	s = sip_str_trim(s);
	if (!s.len)
		return false;
	uri->raw = s;

	colon = memchr(s.p, ':', s.len);
	if (!colon)
		return false;

	uri->scheme = sip_str_init(s.p, (size_t)(colon - s.p));
	rest = sip_str_init(colon + 1, s.len - (size_t)(colon - s.p) - 1);
	if (!rest.len)
		return false;

	if (sip_str_eq_ci(uri->scheme, "sips"))
		uri->flags |= SIP_URI_F_SECURE;
	else if (sip_str_eq_ci(uri->scheme, "tel"))
		uri->flags |= SIP_URI_F_TEL;
	else if (!sip_str_eq_ci(uri->scheme, "sip"))
		return false;

	if (uri->flags & SIP_URI_F_TEL) {
		/* RFC 3966: the number carries its own parameters and there is
		 * no host. P-Asserted-Identity is the reason this scheme is
		 * here at all. */
		sip_value_split(rest, &head, &uri->params);
		uri->user = head;
		return head.len > 0;
	}

	/*
	 * Order matters. RFC 3261 25.1 lets the user part hold ';' unescaped
	 * (user-unreserved), so cutting at the first ';' would split
	 * "sip:+15551234;verstat=TN-Validation-Passed@carrier" in the wrong
	 * place. The '@' is found first instead. A '?' inside the user part
	 * must be escaped as %3F; unescaped it reads as the start of the URI
	 * headers, the same choice every deployed stack makes.
	 */
	q = memchr(rest.p, '?', rest.len);
	if (q) {
		region = sip_str_init(rest.p, (size_t)(q - rest.p));
		uri->headers = sip_str_init(q + 1, rest.len - (size_t)(q - rest.p) - 1);
	} else {
		region = rest;
	}

	at = memchr(region.p, '@', region.len);
	if (at) {
		struct sip_str userinfo = sip_str_init(region.p, (size_t)(at - region.p));

		pw = memchr(userinfo.p, ':', userinfo.len);
		if (pw) {
			uri->user = sip_str_init(userinfo.p, (size_t)(pw - userinfo.p));
			uri->password = sip_str_init(pw + 1, userinfo.len -
						     (size_t)(pw - userinfo.p) - 1);
		} else {
			uri->user = userinfo;
		}
		if (memchr(uri->user.p, '%', uri->user.len))
			uri->flags |= SIP_URI_F_ESCAPED_USER;
		hostport = sip_str_init(at + 1, region.len - (size_t)(at - region.p) - 1);
	} else {
		hostport = region;
	}

	sip_value_split(hostport, &head, &uri->params);
	if (!hostport_parse(head, &uri->host, &uri->port, &ipv6, &has_port))
		return false;
	if (ipv6)
		uri->flags |= SIP_URI_F_IPV6;
	if (has_port)
		uri->flags |= SIP_URI_F_PORT;

	if (sip_param_find(uri->params, "transport", &val))
		uri->transport = val;
	if (sip_param_find(uri->params, "user", &val))
		uri->user_param = val;
	if (sip_param_find(uri->params, "maddr", &val))
		uri->maddr = val;
	if (sip_param_present(uri->params, "lr"))
		uri->flags |= SIP_URI_F_LR;

	return true;
}

bool uri_serialize(const struct sip_uri *uri, struct sip_buf *b)
{
	sip_buf_add_str(b, uri->scheme);
	sip_buf_add(b, ":");

	if (uri->flags & SIP_URI_F_TEL) {
		sip_buf_add_str(b, uri->user);
		sip_buf_add_str(b, uri->params);
		return sip_buf_ok(b);
	}

	if (uri->user.len) {
		sip_buf_add_str(b, uri->user);
		if (uri->password.len) {
			sip_buf_add(b, ":");
			sip_buf_add_str(b, uri->password);
		}
		sip_buf_add(b, "@");
	}

	if (uri->flags & SIP_URI_F_IPV6) {
		sip_buf_add(b, "[");
		sip_buf_add_str(b, uri->host);
		sip_buf_add(b, "]");
	} else {
		sip_buf_add_str(b, uri->host);
	}

	if (uri->flags & SIP_URI_F_PORT)
		sip_buf_addf(b, ":%u", uri->port);

	sip_buf_add_str(b, uri->params);

	if (uri->headers.len) {
		sip_buf_add(b, "?");
		sip_buf_add_str(b, uri->headers);
	}

	return sip_buf_ok(b);
}

/*
 * One hname "=" hvalue pair. Both scans are raw, unlike sip_sep_find(): RFC 3261
 * 25.1 lets neither half hold a quote, an angle bracket or a separator unescaped,
 * so a '&' or an '=' that reached this layer is the separator whatever sits
 * around it. A pair without an '=', or with an empty name, is malformed and the
 * walk steps over it instead of ending, so a good pair behind it is still found.
 */
static bool uri_header_next(struct sip_str *rest, struct sip_str *name, struct sip_str *val)
{
	const char *amp, *eq;
	struct sip_str item;

	while (rest->len) {
		amp = memchr(rest->p, '&', rest->len);
		item = sip_str_init(rest->p, amp ? (size_t)(amp - rest->p) : rest->len);
		rest->p += item.len + (amp ? 1 : 0);
		rest->len -= item.len + (amp ? 1 : 0);

		eq = memchr(item.p, '=', item.len);
		if (!eq || eq == item.p)
			continue;

		*name = sip_str_init(item.p, (size_t)(eq - item.p));
		*val = sip_str_init(eq + 1, item.len - (size_t)(eq - item.p) - 1);

		return true;
	}

	return false;
}

/*
 * RFC 3261 25.1 spells an escaped octet as "%" and two hex digits.
 * sip_uri_unescape() is tolerant and copies a broken escape through, which is
 * right for a userinfo uvoice only ever echoes back; a URI header value
 * becomes a header field of the next request, so a broken escape is refused
 * here instead.
 */
static bool uri_escapes_ok(struct sip_str s)
{
	size_t i;

	for (i = 0; i < s.len; i++) {
		if (s.p[i] != '%')
			continue;
		if (i + 2 >= s.len)
			return false;
		if (!isxdigit((unsigned char)s.p[i + 1]) ||
		    !isxdigit((unsigned char)s.p[i + 2]))
			return false;
		i += 2;
	}

	return true;
}

/*
 * A decoded URI header value, and a Replaces field, becomes a header field of
 * the next request. A control byte inside it would inject a header line (a
 * decoded %0D %0A) or truncate the field (a decoded %00), so every byte below
 * 0x20 and the 0x7F byte is refused. The scan runs after the unescape, because
 * a raw span scan cannot see a %0D.
 */
static bool ctl_present(const char *s, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];

		if (c < 0x20 || c == 0x7F)
			return true;
	}

	return false;
}

bool uri_headers_get(struct sip_str headers, const char *name, char *out, size_t olen)
{
	struct sip_str rest = headers, hname, hval;
	int n;

	if (!out || !olen)
		return false;
	out[0] = 0;

	while (uri_header_next(&rest, &hname, &hval)) {
		if (!sip_str_eq_ci(hname, name))
			continue;
		if (!uri_escapes_ok(hval))
			return false;

		n = sip_uri_unescape(hval, out, olen);
		if (n < 0 || ctl_present(out, (size_t)n)) {
			out[0] = 0;
			return false;
		}

		return true;
	}

	return false;
}

/* unreserved (RFC 3261 25.1 alphanum and mark) plus hnv-unreserved */
static bool hnv_plain(char c)
{
	if (isalnum((unsigned char)c))
		return true;

	return strchr("-_.!~*'()[]/?:+$", c) != NULL;
}

bool uri_header_escape(struct sip_buf *b, const char *val)
{
	const char *p;

	for (p = val; p && *p; p++) {
		if (hnv_plain(*p))
			sip_buf_add_len(b, p, 1);
		else
			sip_buf_addf(b, "%%%02X", (unsigned char)*p);
	}

	return sip_buf_ok(b);
}

static bool user_esc_hit(char c, unsigned int esc)
{
	if (c == '#')
		return esc & SIP_URI_ESC_POUND;
	if (c == '*')
		return esc & SIP_URI_ESC_STAR;

	return false;
}

bool uri_user_escape(char *dst, size_t dlen, const char *user, unsigned int esc)
{
	size_t n = 0;

	if (!dst || !dlen)
		return false;

	for (const char *p = user; p && *p; p++) {
		size_t need = user_esc_hit(*p, esc) ? 3 : 1;

		if (n + need >= dlen) {
			dst[0] = 0;
			return false;
		}

		if (need == 1) {
			dst[n++] = *p;
			continue;
		}

		snprintf(dst + n, dlen - n, "%%%02X", (unsigned char)*p);
		n += 3;
	}

	dst[n] = 0;

	return true;
}

static uint32_t q_parse(struct sip_str s)
{
	uint32_t v = 0, mult = 100;
	size_t i;

	s = sip_str_trim(s);
	for (i = 0; i < s.len && isdigit((unsigned char)s.p[i]); i++)
		v = v * 1000 + (uint32_t)(s.p[i] - '0') * 1000;

	if (i < s.len && s.p[i] == '.') {
		for (i++; i < s.len && isdigit((unsigned char)s.p[i]) && mult; i++) {
			v += (uint32_t)(s.p[i] - '0') * mult;
			mult /= 10;
		}
	}

	return v > 1000 ? 1000 : v;
}

static void addr_params_read(struct sip_addr *a)
{
	struct sip_str rest = a->params, name, val;

	/* A repeated parameter is malformed. The first one wins, which is what
	 * sip_param_find() does everywhere else. */
	while (sip_param_next(&rest, &name, &val)) {
		if (sip_str_eq_ci(name, "tag") && !(a->flags & SIP_ADDR_F_TAG)) {
			a->tag = val;
			a->flags |= SIP_ADDR_F_TAG;
		} else if (sip_str_eq_ci(name, "q") && !(a->flags & SIP_ADDR_F_Q)) {
			a->q_milli = q_parse(val);
			a->flags |= SIP_ADDR_F_Q;
		} else if (sip_str_eq_ci(name, "expires") &&
			   !(a->flags & SIP_ADDR_F_EXPIRES)) {
			if (sip_str_uint(val, &a->expires))
				a->flags |= SIP_ADDR_F_EXPIRES;
		}
	}
}

bool hdr_addr_parse(struct sip_str v, struct sip_addr *a)
{
	struct sip_str rest = sip_str_trim(v), uri_span;
	const char *lt, *gt;
	size_t i;
	bool esc = false;

	memset(a, 0, sizeof(*a));
	if (!rest.len)
		return false;
	a->raw = rest;

	if (rest.p[0] == '"') {
		for (i = 1; i < rest.len; i++) {
			if (esc) {
				esc = false;
				continue;
			}
			if (rest.p[i] == '\\')
				esc = true;
			else if (rest.p[i] == '"')
				break;
		}
		if (i >= rest.len)
			return false;
		a->display = sip_str_init(rest.p + 1, i - 1);
		a->flags |= SIP_ADDR_F_QUOTED;
		rest = sip_str_trim(sip_str_init(rest.p + i + 1, rest.len - i - 1));
		if (!rest.len)
			return false;
	}

	lt = memchr(rest.p, '<', rest.len);
	if (lt) {
		if (!(a->flags & SIP_ADDR_F_QUOTED))
			a->display = sip_str_trim(sip_str_init(rest.p,
							       (size_t)(lt - rest.p)));
		gt = memchr(lt, '>', rest.len - (size_t)(lt - rest.p));
		if (!gt)
			return false;
		uri_span = sip_str_init(lt + 1, (size_t)(gt - lt) - 1);
		a->params = sip_str_init(gt + 1, rest.len - (size_t)(gt + 1 - rest.p));
		a->flags |= SIP_ADDR_F_NAME_ADDR;
	} else if (rest.p[0] == '*') {
		a->flags |= SIP_ADDR_F_STAR;
		a->params = sip_str_init(rest.p + 1, rest.len - 1);
		addr_params_read(a);
		return true;
	} else {
		/* RFC 3261 20.10: without angle brackets every semicolon
		 * parameter belongs to the header, not to the URI. */
		sip_value_split(rest, &uri_span, &a->params);
	}

	/* A URI we cannot parse still leaves a usable tag and display name.
	 * Failing the whole header here would answer a call 603 for a From
	 * this side cannot read. */
	uri_parse(uri_span, &a->uri);
	addr_params_read(a);

	return true;
}

int hdr_addr_display_get(const struct sip_addr *a, char *out, size_t olen)
{
	return sip_unquote(a->display, out, olen);
}

bool hdr_via_parse(struct sip_str v, struct sip_via *via)
{
	struct sip_str rest = sip_str_trim(v), head, sentby, val;
	const char *s1, *s2;
	bool ipv6, has_port;

	memset(via, 0, sizeof(*via));
	if (!rest.len)
		return false;
	via->raw = rest;

	sip_value_split(rest, &head, &via->params);

	/* RFC 4475 3.1.1.1 folds "SIP  /   2.0  / UDP" across three lines, so
	 * every field here is trimmed after the split. */
	s1 = memchr(head.p, '/', head.len);
	if (!s1)
		return false;
	s2 = memchr(s1 + 1, '/', head.len - (size_t)(s1 + 1 - head.p));
	if (!s2)
		return false;

	via->proto = sip_str_trim(sip_str_init(head.p, (size_t)(s1 - head.p)));
	via->ver = sip_str_trim(sip_str_init(s1 + 1, (size_t)(s2 - s1) - 1));
	sentby = sip_str_init(s2 + 1, head.len - (size_t)(s2 + 1 - head.p));

	if (!sip_word_take(&sentby, &via->transport))
		return false;
	if (!hostport_parse(sentby, &via->host, &via->port, &ipv6, &has_port))
		return false;
	if (ipv6)
		via->flags |= SIP_VIA_F_IPV6;
	if (has_port)
		via->flags |= SIP_VIA_F_PORT;

	if (sip_param_find(via->params, "branch", &val) && val.len) {
		via->branch = val;
		via->flags |= SIP_VIA_F_BRANCH;
		if (sip_str_prefix(val, "z9hG4bK"))
			via->flags |= SIP_VIA_F_MAGIC;
	}
	if (sip_param_find(via->params, "received", &val) && val.len) {
		via->received = val;
		via->flags |= SIP_VIA_F_RECEIVED;
	}
	if (sip_param_find(via->params, "rport", &val)) {
		via->flags |= SIP_VIA_F_RPORT;
		if (val.len && sip_str_uint(val, &via->rport))
			via->flags |= SIP_VIA_F_RPORT_VAL;
	}
	if (sip_param_find(via->params, "ttl", &val) && sip_str_uint(val, &via->ttl))
		via->flags |= SIP_VIA_F_TTL;
	if (sip_param_find(via->params, "maddr", &val))
		via->maddr = val;

	return true;
}

static bool val_advance(struct sip_val_iter *it, struct sip_str *val)
{
	struct sip_str item;

	while (it->hdr) {
		while (sip_csv_next(&it->rest, &item)) {
			if (!item.len)
				continue;
			*val = item;
			return true;
		}
		it->hdr = msg_hdr_next(it->msg, it->hdr);
		it->rest = it->hdr ? it->hdr->value : sip_str_init(NULL, 0);
	}

	return false;
}

bool hdr_val_first(const struct sip_msg *msg, enum sip_hdr_id id, struct sip_val_iter *it,
		   struct sip_str *val)
{
	memset(it, 0, sizeof(*it));
	it->msg = msg;
	it->hdr = msg_hdr_first(msg, id);
	it->rest = it->hdr ? it->hdr->value : sip_str_init(NULL, 0);

	return val_advance(it, val);
}

bool hdr_val_next(struct sip_val_iter *it, struct sip_str *val)
{
	return val_advance(it, val);
}

unsigned int hdr_val_count(const struct sip_msg *msg, enum sip_hdr_id id)
{
	struct sip_val_iter it;
	struct sip_str val;
	unsigned int n = 0;

	if (!hdr_val_first(msg, id, &it, &val))
		return 0;

	do {
		n++;
	} while (hdr_val_next(&it, &val));

	return n;
}

bool hdr_addr_first(const struct sip_msg *msg, enum sip_hdr_id id, struct sip_val_iter *it,
		    struct sip_addr *a)
{
	struct sip_str val;

	if (!hdr_val_first(msg, id, it, &val))
		return false;
	if (hdr_addr_parse(val, a))
		return true;

	return hdr_addr_next(it, a);
}

bool hdr_addr_next(struct sip_val_iter *it, struct sip_addr *a)
{
	struct sip_str val;

	while (hdr_val_next(it, &val)) {
		if (hdr_addr_parse(val, a))
			return true;
	}

	return false;
}

bool hdr_addr_get(const struct sip_msg *msg, enum sip_hdr_id id, struct sip_addr *a)
{
	struct sip_val_iter it;

	return hdr_addr_first(msg, id, &it, a);
}

bool hdr_via_top(const struct sip_msg *msg, struct sip_via *via)
{
	struct sip_val_iter it;
	struct sip_str val;

	if (!hdr_val_first(msg, SIP_HDR_VIA, &it, &val))
		return false;

	return hdr_via_parse(val, via);
}

unsigned int hdr_via_count(const struct sip_msg *msg)
{
	return hdr_val_count(msg, SIP_HDR_VIA);
}

bool hdr_call_id_get(const struct sip_msg *msg, struct sip_str *out)
{
	/* Never comma split: RFC 3261 25.1 callid is one token that may hold
	 * commas. */
	return msg_hdr_value(msg, SIP_HDR_CALL_ID, out);
}

bool hdr_cseq_parse(struct sip_str v, uint32_t *num, enum sip_method *method,
		    struct sip_str *method_name)
{
	struct sip_str rest = sip_str_trim(v), w1, w2;

	/* RFC 3261 8.1.1.5: the sequence number is below 2**31. The
	 * transaction layer increments it, so a value the standard forbids
	 * never reaches the code that would overflow on it. */
	if (!sip_word_take(&rest, &w1) || !sip_str_uint(w1, num) || *num > SIP_CSEQ_MAX)
		return false;
	if (!sip_word_take(&rest, &w2))
		return false;

	if (method)
		*method = sip_method_parse(w2);
	if (method_name)
		*method_name = w2;

	return true;
}

bool hdr_cseq_get(const struct sip_msg *msg, uint32_t *num, enum sip_method *method)
{
	struct sip_str v;

	if (!msg_hdr_value(msg, SIP_HDR_CSEQ, &v))
		return false;

	return hdr_cseq_parse(v, num, method, NULL);
}

bool hdr_uint_get(const struct sip_msg *msg, enum sip_hdr_id id, uint32_t *out)
{
	struct sip_str v, head;

	if (!msg_hdr_value(msg, id, &v))
		return false;

	sip_value_split(v, &head, NULL);

	return sip_str_uint(head, out);
}

bool hdr_rseq_get(const struct sip_msg *msg, uint32_t *rseq)
{
	/* RFC 3262 3: no RSeq is 0, because the series starts at 1 or above and
	 * rises by one without a wrap. The 2**32-1 bound of RFC 3262 7.1 is the
	 * one the integer parser already applies. */
	return hdr_uint_get(msg, SIP_HDR_RSEQ, rseq) && *rseq != 0;
}

bool hdr_rack_get(const struct sip_msg *msg, uint32_t *rseq, uint32_t *cseq,
		  enum sip_method *method)
{
	struct sip_str v, w1, w2, w3;

	if (!msg_hdr_value(msg, SIP_HDR_RACK, &v))
		return false;

	v = sip_str_trim(v);
	/* The response number carries the RSeq range of RFC 3262 7.1; the CSeq
	 * number beside it keeps the RFC 3261 8.1.1.5 bound. */
	if (!sip_word_take(&v, &w1) || !sip_str_uint(w1, rseq) || *rseq == 0)
		return false;
	if (!sip_word_take(&v, &w2) || !sip_str_uint(w2, cseq) || *cseq > SIP_CSEQ_MAX)
		return false;
	if (!sip_word_take(&v, &w3))
		return false;
	if (method)
		*method = sip_method_parse(w3);

	return true;
}

bool hdr_retry_after_get(const struct sip_msg *msg, uint32_t *secs, uint32_t *duration)
{
	struct sip_str v, digits, params, val;
	size_t i = 0, depth = 0;

	if (!msg_hdr_value(msg, SIP_HDR_RETRY_AFTER, &v))
		return false;

	v = sip_str_trim(v);
	while (i < v.len && isdigit((unsigned char)v.p[i]))
		i++;
	digits = sip_str_init(v.p, i);
	if (!sip_str_uint(digits, secs))
		return false;

	if (duration)
		*duration = 0;

	/* RFC 3261 20.33 allows a comment in parentheses between the delta and
	 * the parameters; it may hold any character, separators included. */
	for (; i < v.len; i++) {
		if (v.p[i] == '(') {
			depth++;
			continue;
		}
		if (v.p[i] == ')') {
			if (depth)
				depth--;
			continue;
		}
		if (!depth && v.p[i] == ';')
			break;
	}
	if (i >= v.len || !duration)
		return true;

	params = sip_str_init(v.p + i, v.len - i);
	if (sip_param_find(params, "duration", &val))
		sip_str_uint(val, duration);

	return true;
}

bool hdr_content_type_get(const struct sip_msg *msg, struct sip_str *type,
			  struct sip_str *params)
{
	struct sip_str v;

	if (!msg_hdr_value(msg, SIP_HDR_CONTENT_TYPE, &v))
		return false;

	sip_value_split(v, type, params);

	return true;
}

bool hdr_session_expires_get(const struct sip_msg *msg, uint32_t *delta,
			     enum sip_refresher *refresher)
{
	struct sip_str v, head, params, val;

	if (refresher)
		*refresher = SIP_REFRESHER_NONE;
	if (!msg_hdr_value(msg, SIP_HDR_SESSION_EXPIRES, &v))
		return false;

	sip_value_split(v, &head, &params);
	if (!sip_str_uint(head, delta))
		return false;

	if (refresher && sip_param_find(params, "refresher", &val)) {
		if (sip_str_eq_ci(val, "uac"))
			*refresher = SIP_REFRESHER_UAC;
		else if (sip_str_eq_ci(val, "uas"))
			*refresher = SIP_REFRESHER_UAS;
	}

	return true;
}

bool hdr_min_se_get(const struct sip_msg *msg, uint32_t *delta)
{
	return hdr_uint_get(msg, SIP_HDR_MIN_SE, delta);
}

bool hdr_token_has(const struct sip_msg *msg, enum sip_hdr_id id, const char *token)
{
	struct sip_val_iter it;
	struct sip_str val, head;

	if (!hdr_val_first(msg, id, &it, &val))
		return false;

	do {
		/* An option tag carries no parameters, but a q value on an
		 * Accept style list must not defeat the compare. */
		sip_value_split(val, &head, NULL);
		if (sip_str_eq_ci(head, token))
			return true;
	} while (hdr_val_next(&it, &val));

	return false;
}

/* RFC 3323 4.2 separates priv-values with ';'. Deployed proxies also send them
 * comma separated, so both split here. */
static bool priv_value_has(struct sip_str value, const char *want)
{
	struct sip_str rest = value, item, sub, tok, ignored;

	while (sip_csv_next(&rest, &item)) {
		sub = item;
		while (sip_param_next(&sub, &tok, &ignored)) {
			if (sip_str_eq_ci(tok, want))
				return true;
		}
	}

	return false;
}

bool hdr_privacy_has(const struct sip_msg *msg, const char *value)
{
	const struct sip_hdr *h;

	for (h = msg_hdr_first(msg, SIP_HDR_PRIVACY); h; h = msg_hdr_next(msg, h)) {
		if (priv_value_has(h->value, value))
			return true;
	}

	return false;
}

bool hdr_event_get(const struct sip_msg *msg, struct sip_event *ev)
{
	struct sip_str v, val;

	memset(ev, 0, sizeof(*ev));
	if (!msg_hdr_value(msg, SIP_HDR_EVENT, &v))
		return false;

	sip_value_split(v, &ev->package, &ev->params);
	if (sip_param_find(ev->params, "id", &val))
		ev->id = val;

	return ev->package.len > 0;
}

bool hdr_sub_state_get(const struct sip_msg *msg, struct sip_sub_state *st)
{
	struct sip_str v, val;

	memset(st, 0, sizeof(*st));
	if (!msg_hdr_value(msg, SIP_HDR_SUBSCRIPTION_STATE, &v))
		return false;

	sip_value_split(v, &st->state, &st->params);
	if (sip_param_find(st->params, "expires", &val) && sip_str_uint(val, &st->expires))
		st->flags |= SIP_SUB_F_EXPIRES;
	if (sip_param_find(st->params, "retry-after", &val) &&
	    sip_str_uint(val, &st->retry_after))
		st->flags |= SIP_SUB_F_RETRY_AFTER;
	if (sip_param_find(st->params, "reason", &val))
		st->reason = val;

	return st->state.len > 0;
}

/*
 * Whether every quoted string in this span is closed. The separator scan of
 * sip_sep_find() treats an opening quote as the start of a run that swallows
 * every separator until the closing one, exactly as RFC 3261 25.1 asks, so an
 * unterminated quote swallows the whole tail of the value instead: a parameter
 * behind it is not merely unread, it is invisible.
 *
 * The state machine is the one sip_sep_find() runs, and nothing else here can
 * share it, because that function answers about a separator and this one about
 * the value as a whole.
 */
static bool quotes_closed(struct sip_str s)
{
	bool quoted = false, esc = false;
	size_t i;

	for (i = 0; i < s.len; i++) {
		char c = s.p[i];

		if (esc)
			esc = false;
		else if (!quoted)
			quoted = c == '"';
		else if (c == '\\')
			esc = true;
		else if (c == '"')
			quoted = false;
	}

	return !quoted;
}

bool hdr_replaces_parse(struct sip_str v, struct sip_replaces *out)
{
	struct sip_str rest = sip_str_trim(v), params, name, val;
	const char *semi;

	memset(out, 0, sizeof(*out));
	if (!rest.len)
		return false;

	/*
	 * The Call-ID is cut raw at the first semicolon, not through
	 * sip_value_split(): RFC 3261 25.1 lets a callid word hold a double
	 * quote and an angle bracket, and a quote aware scan would follow one of
	 * those past the parameters. No production of RFC 3891 6.1 puts a
	 * semicolon inside the callid, so the first one always ends it. The
	 * parameters behind it are walked quote aware, because a generic
	 * parameter value may be a quoted string.
	 */
	semi = memchr(rest.p, ';', rest.len);
	if (!semi)
		return false;

	out->call_id = sip_str_trim(sip_str_init(rest.p, (size_t)(semi - rest.p)));
	if (!out->call_id.len)
		return false;

	params = sip_str_init(semi, rest.len - (size_t)(semi - rest.p));

	/*
	 * An unmatched quote in the parameters fails the value closed. The
	 * walk below finds its separators through sip_sep_find(), which treats
	 * an opening quote as a run that swallows every ';' until the closing
	 * one: an unterminated quote therefore swallows the whole tail,
	 * ";early-only" included, and the two tags in front of it would still
	 * have parsed. This one grammar decides which call a request replaces,
	 * so a value this side cannot read to its end is refused and the caller
	 * answers 400 rather than replacing a dialog on half a value.
	 *
	 * The test covers the parameters and not the whole value, because the
	 * Call-ID above was cut raw: RFC 3261 25.1 lets a callid word carry a
	 * '"', that
	 * quote reaches no scanner, and failing such a value would refuse a
	 * legitimate Call-ID this parser has always accepted.
	 */
	if (!quotes_closed(params))
		return false;

	while (sip_param_next(&params, &name, &val)) {
		if (sip_str_eq_ci(name, "to-tag")) {
			if (out->to_tag.len || !val.len)
				return false;
			out->to_tag = val;
		} else if (sip_str_eq_ci(name, "from-tag")) {
			if (out->from_tag.len || !val.len)
				return false;
			out->from_tag = val;
		} else if (sip_str_eq_ci(name, "early-only")) {
			out->early_only = true;
		}
	}

	return out->to_tag.len > 0 && out->from_tag.len > 0;
}

bool hdr_replaces_write(struct sip_buf *b, const char *call_id, const char *to_tag,
			const char *from_tag, bool early_only)
{
	/* Checked before a byte is written, so a refusal leaves no half value
	 * behind: RFC 3891 6.1 has no form without the two tags. A control byte
	 * in any field is refused too, symmetric with uri_headers_get(), so a
	 * corrupted tag cannot inject a header line into the request that carries
	 * this value. A ';' inside call_id stays out of scope: RFC 3261 word
	 * excludes it, so a wire Call-ID never holds one and the write assumes a
	 * well formed span. */
	if (!call_id || !*call_id || !to_tag || !*to_tag || !from_tag || !*from_tag)
		return false;
	if (ctl_present(call_id, strlen(call_id)) || ctl_present(to_tag, strlen(to_tag)) ||
	    ctl_present(from_tag, strlen(from_tag)))
		return false;

	sip_buf_add(b, call_id);
	sip_buf_add(b, ";to-tag=");
	sip_buf_add(b, to_tag);
	sip_buf_add(b, ";from-tag=");
	sip_buf_add(b, from_tag);
	if (early_only)
		sip_buf_add(b, ";early-only");

	return sip_buf_ok(b);
}

/* RFC 3420: the fragment uvoice sends is one status line, which is the exact
 * shape msg_status_line_write() already writes. */
bool sipfrag_write(struct sip_buf *b, uint32_t code, const char *reason)
{
	return msg_status_line_write(b, code, reason);
}

bool sipfrag_status(struct sip_str body, uint32_t *code)
{
	struct sip_str line, ver, num;
	const char *lf;

	/* *code is 0 on every false return, so a caller reads a deterministic
	 * value without testing the bool first. */
	*code = 0;

	/* Leading empty lines are skipped the way msg_parse() skips them, and
	 * the trim takes the CR of a CRLF and the whole missing terminator case
	 * with it. */
	body = sip_str_trim(body);
	if (!body.len)
		return false;

	lf = memchr(body.p, '\n', body.len);
	line = sip_str_init(body.p, lf ? (size_t)(lf - body.p) : body.len);

	/* The version token is literal (RFC 3261 7.2). shared/msg.c finds the
	 * status line with a case blind compare and only flags a version that
	 * differs in case; a fragment has no flags word, so the same tolerance
	 * is spent on accepting it. */
	if (!sip_word_take(&line, &ver) || !sip_str_eq_ci(ver, "SIP/2.0"))
		return false;
	if (!sip_word_take(&line, &num) || num.len != 3 || !sip_str_uint(num, code))
		return false;
	if (*code < 100 || *code > 699) {
		*code = 0;
		return false;
	}

	return true;
}

static struct sip_str alert_token_take(struct sip_str s, size_t at)
{
	size_t end = at;

	while (end < s.len) {
		char c = s.p[end];

		if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.')
			end++;
		else
			break;
	}

	return sip_str_init(s.p + at, end - at);
}

/*
 * A sentinel with the ring digit right behind it, the shape of both the
 * BroadSoft prefix and the Nortel dst_ring_ form. The digit is kept as
 * received, 1 to 9. This layer names the dialect and call.c owns the fold to a
 * ring template.
 *
 * The first occurrence of the sentinel decides, and a bad digit behind it does
 * not send the search after a second occurrence: a URL that carries the
 * sentinel twice is a switch defect, not a form to interpret. The NZ branch
 * below is the one place that keeps searching, because its sentinel is two
 * letters long and occurs inside ordinary words.
 */
static bool alert_digit_take(struct sip_alert_info *ai, struct sip_str url, const char *sentinel,
			     enum sip_alert_dialect dialect)
{
	size_t at, d;

	if (!sip_str_find_ci(url, sentinel, &at))
		return false;

	d = at + strlen(sentinel);
	if (d >= url.len || url.p[d] < '1' || url.p[d] > '9')
		return false;

	ai->dialect = dialect;
	ai->ring = (uint32_t)(url.p[d] - '0');
	ai->pattern = sip_str_init(url.p + at, d - at + 1);

	return true;
}

static bool alert_nortel_word(struct sip_alert_info *ai, struct sip_str url, const char *word,
			      uint32_t ring)
{
	size_t at;

	if (!sip_str_find_ci(url, word, &at))
		return false;

	ai->dialect = SIP_ALERT_NORTEL;
	ai->ring = ring;
	ai->pattern = sip_str_init(url.p + at, strlen(word));

	return true;
}

static bool alert_nortel_take(struct sip_alert_info *ai, struct sip_str url)
{
	if (alert_digit_take(ai, url, "dst_ring_", SIP_ALERT_NORTEL))
		return true;

	/*
	 * A dst_ring_ with a digit outside the range does not hide the two
	 * token forms: a URL that carries both is answered, not dropped.
	 */
	if (alert_nortel_word(ai, url, "splash_ring", SIP_ALERT_NORTEL_SPLASH))
		return true;

	return alert_nortel_word(ai, url, "standard_ring", SIP_ALERT_NORTEL_STANDARD);
}

/* the bare "alert-" names nothing, so it is not the Sylantro dialect */
static bool alert_sylantro_token(struct sip_alert_info *ai, struct sip_str token)
{
	if (token.len <= sizeof("alert-") - 1)
		return false;

	ai->dialect = SIP_ALERT_SYLANTRO;
	ai->pattern = token;

	return true;
}

static bool alert_sylantro_take(struct sip_alert_info *ai, struct sip_str url)
{
	struct sip_str val;
	size_t at;

	/* Sylantro puts the pattern in ;info=, other gateways put it in the
	 * URL itself; both are accepted. */
	if (sip_param_find(ai->params, "info", &val) && sip_str_prefix_ci(val, "alert-") &&
	    alert_sylantro_token(ai, alert_token_take(val, 0)))
		return true;
	if (!sip_str_find_ci(url, "alert-", &at))
		return false;

	return alert_sylantro_token(ai, alert_token_take(url, at));
}

/*
 * The NZ sentinel is two letters, so a bare substring search lets a file name
 * select a ring: veranda3.wav carries "da3". The sentinel is anchored
 * instead: it starts the URL or follows a character that ends a path element,
 * a query name or a word. An unanchored hit does not end the search, because
 * the value may carry the real sentinel behind it.
 */
static bool alert_nz_anchored(struct sip_str url, size_t at)
{
	char c;

	if (!at)
		return true;

	c = url.p[at - 1];

	return c == '/' || c == '=' || c == '-' || c == '?' || c == '&';
}

static bool alert_nz_take(struct sip_alert_info *ai, struct sip_str url)
{
	size_t i = 0, at, d;

	while (i < url.len && sip_str_find_ci(sip_str_init(url.p + i, url.len - i), "da", &at)) {
		size_t off = i + at;

		i = off + 1;
		d = off + 2;
		if (!alert_nz_anchored(url, off))
			continue;
		if (d >= url.len || url.p[d] < '1' || url.p[d] > '4')
			continue;

		ai->dialect = SIP_ALERT_NZ;
		ai->ring = (uint32_t)(url.p[d] - '0');
		ai->pattern = sip_str_init(url.p + off, 3);

		return true;
	}

	return false;
}

/* false for a value that carries no URL, which is the only unparseable form */
static bool alert_info_one(struct sip_str v, const char *prefix, struct sip_alert_info *ai)
{
	struct sip_str url = sip_str_trim(v);
	const char *lt, *gt;

	memset(ai, 0, sizeof(*ai));
	if (!url.len)
		return false;

	/*
	 * The bracket scan is quote blind, unlike sip_sep_find(). RFC 3261 20.4
	 * gives alert-param no display name and no quoted string, so no legal
	 * production puts an angle bracket inside quotes here, and a value that
	 * does is a malformed value whichever bracket wins. Both searches stay
	 * inside the value either way.
	 */
	lt = memchr(url.p, '<', url.len);
	gt = lt ? memchr(lt, '>', url.len - (size_t)(lt - url.p)) : NULL;
	if (lt && gt) {
		ai->params = sip_str_init(gt + 1, url.len - (size_t)(gt + 1 - url.p));
		url = sip_str_init(lt + 1, (size_t)(gt - lt) - 1);
	} else {
		sip_value_split(url, &url, &ai->params);
	}
	if (!url.len)
		return false;

	ai->url = url;
	ai->dialect = SIP_ALERT_OTHER;

	if (alert_digit_take(ai, url, prefix, SIP_ALERT_BELLCORE))
		return true;
	if (alert_nortel_take(ai, url))
		return true;
	if (alert_sylantro_take(ai, url))
		return true;

	alert_nz_take(ai, url);

	return true;
}

static const char *alert_prefix_of(const char *prefix)
{
	return prefix && *prefix ? prefix : SIP_ALERT_PREFIX_DEFAULT;
}

bool hdr_alert_info_first(const struct sip_msg *msg, const char *prefix,
			  struct sip_val_iter *it, struct sip_alert_info *ai)
{
	struct sip_str val;

	memset(ai, 0, sizeof(*ai));
	if (!hdr_val_first(msg, SIP_HDR_ALERT_INFO, it, &val))
		return false;
	if (alert_info_one(val, alert_prefix_of(prefix), ai))
		return true;

	return hdr_alert_info_next(it, prefix, ai);
}

bool hdr_alert_info_next(struct sip_val_iter *it, const char *prefix, struct sip_alert_info *ai)
{
	struct sip_str val;

	while (hdr_val_next(it, &val)) {
		if (alert_info_one(val, alert_prefix_of(prefix), ai))
			return true;
	}

	return false;
}

bool hdr_alert_info_get(const struct sip_msg *msg, const char *prefix,
			struct sip_alert_info *ai)
{
	struct sip_val_iter it;

	return hdr_alert_info_first(msg, prefix, &it, ai);
}

static bool chal_take(struct sip_str *rest, struct sip_challenge *c)
{
	struct sip_str r = sip_str_trim(*rest), scheme, item, name, val, save;
	bool qop_open = false;

	memset(c, 0, sizeof(*c));
	if (!r.len) {
		*rest = r;
		return false;
	}

	if (!sip_word_take(&r, &scheme)) {
		*rest = r;
		return false;
	}
	c->scheme = scheme;
	if (sip_str_eq_ci(scheme, "Digest"))
		c->flags |= SIP_CHAL_F_DIGEST;

	for (;;) {
		save = r;
		if (!sip_csv_next(&r, &item))
			break;
		if (!item.len)
			continue;

		if (!sip_kv_split(item, &name, &val)) {
			/* RFC 3261 25.1 quotes the qop list, but carriers ship
			 * qop=auth,auth-int unquoted, and the comma split above
			 * then leaves the remaining tokens on their own. Growing
			 * the recorded span over them keeps every token. */
			if (qop_open && name.len) {
				c->qop.len = (size_t)(name.p + name.len - c->qop.p);
				continue;
			}
		}
		/* A comma separated list may hold several challenges
		 * (RFC 3261 20.44). The next one starts where an item carries
		 * a scheme token in front of its first parameter name, which
		 * shows up as whitespace inside the name. */
		if (memchr(name.p, ' ', name.len) || memchr(name.p, '\t', name.len)) {
			r = save;
			break;
		}

		qop_open = false;
		if (sip_str_eq_ci(name, "realm"))
			c->realm = val;
		else if (sip_str_eq_ci(name, "nonce"))
			c->nonce = val;
		else if (sip_str_eq_ci(name, "opaque"))
			c->opaque = val;
		else if (sip_str_eq_ci(name, "algorithm"))
			c->algorithm = val;
		else if (sip_str_eq_ci(name, "domain"))
			c->domain = val;
		else if (sip_str_eq_ci(name, "qop")) {
			c->qop = val;
			/* Only an unquoted list can continue past the comma. */
			qop_open = val.len > 0 && item.p[item.len - 1] != '"';
		} else if (sip_str_eq_ci(name, "stale") && sip_str_eq_ci(val, "true"))
			c->flags |= SIP_CHAL_F_STALE;
	}

	if (c->qop.len) {
		struct sip_str list = c->qop, tok;

		c->flags |= SIP_CHAL_F_QOP;
		while (sip_csv_next(&list, &tok)) {
			if (sip_str_eq_ci(tok, "auth"))
				c->flags |= SIP_CHAL_F_QOP_AUTH;
			else if (sip_str_eq_ci(tok, "auth-int"))
				c->flags |= SIP_CHAL_F_QOP_AUTH_INT;
		}
	}

	*rest = r;

	return true;
}

bool hdr_chal_parse(struct sip_str v, struct sip_chal_iter *it, struct sip_challenge *c)
{
	memset(it, 0, sizeof(*it));
	it->rest = v;

	return chal_take(&it->rest, c);
}

bool hdr_chal_first(const struct sip_msg *msg, enum sip_hdr_id id, struct sip_chal_iter *it,
		    struct sip_challenge *c)
{
	memset(it, 0, sizeof(*it));
	it->msg = msg;
	it->id = id;
	it->hdr = msg_hdr_first(msg, id);
	it->rest = it->hdr ? it->hdr->value : sip_str_init(NULL, 0);

	return hdr_chal_next(it, c);
}

bool hdr_chal_next(struct sip_chal_iter *it, struct sip_challenge *c)
{
	for (;;) {
		if (chal_take(&it->rest, c))
			return true;
		if (!it->msg || !it->hdr)
			return false;

		it->hdr = msg_hdr_next(it->msg, it->hdr);
		it->rest = it->hdr ? it->hdr->value : sip_str_init(NULL, 0);
		if (!it->hdr)
			return false;
	}
}

static bool cred_quoted(struct sip_buf *b, const char *name, const char *val, bool *first)
{
	if (!val)
		return true;
	if (!*first)
		sip_buf_add(b, ", ");
	*first = false;
	sip_buf_add(b, name);
	sip_buf_add(b, "=");

	return sip_buf_add_quoted(b, val);
}

static bool cred_token(struct sip_buf *b, const char *name, const char *val, bool *first)
{
	if (!val)
		return true;
	if (!*first)
		sip_buf_add(b, ", ");
	*first = false;
	sip_buf_add(b, name);
	sip_buf_add(b, "=");

	return sip_buf_add(b, val);
}

bool hdr_cred_write(struct sip_buf *b, enum sip_hdr_id id, const struct sip_cred *cred)
{
	const char *qop = (cred->qop && *cred->qop) ? cred->qop : NULL;
	bool first = true;

	if (!cred->username || !cred->realm || !cred->nonce || !cred->uri || !cred->response)
		return false;

	/* RFC 3261 22.4 and RFC 7616 3.4: cnonce and nc are mandatory once qop
	 * is used, and the digest was computed over them. A server that gets
	 * qop without cnonce rejects the credentials, so refuse to build them
	 * here rather than register with a request that cannot succeed. */
	if (qop && (!cred->cnonce || !*cred->cnonce || !cred->nc))
		return false;

	sip_buf_add(b, sip_hdr_name(id));
	sip_buf_add(b, ": Digest ");

	cred_quoted(b, "username", cred->username, &first);
	cred_quoted(b, "realm", cred->realm, &first);
	cred_quoted(b, "nonce", cred->nonce, &first);
	cred_quoted(b, "uri", cred->uri, &first);
	cred_quoted(b, "response", cred->response, &first);
	/* RFC 3261 22.4: algorithm, qop and nc are tokens, never quoted. */
	cred_token(b, "algorithm", cred->algorithm, &first);
	if (qop) {
		cred_quoted(b, "cnonce", cred->cnonce, &first);
		cred_token(b, "qop", qop, &first);
		if (!first)
			sip_buf_add(b, ", ");
		first = false;
		sip_buf_addf(b, "nc=%08x", cred->nc);
	}
	cred_quoted(b, "opaque", cred->opaque, &first);
	sip_buf_crlf(b);

	return sip_buf_ok(b);
}

static struct sip_str uri_number(const struct sip_uri *u)
{
	struct sip_str head;

	/* Carriers append STIR/SHAKEN state inside the user part, for example
	 * sip:+15551234;verstat=No-TN-Validation@carrier.net. */
	sip_value_split(u->user, &head, NULL);

	return head;
}

/* RFC 3966 3: a telephone number may carry visual separators, which hold no
 * digit and no meaning of their own. */
static bool tel_visual_sep(char c)
{
	return c == '-' || c == '.' || c == '(' || c == ')';
}

static bool tel_number_form(struct sip_str s)
{
	size_t i = 0;
	bool digit = false;

	if (!s.len)
		return false;
	if (s.p[0] == '+')
		i = 1;

	for (; i < s.len; i++) {
		if (isdigit((unsigned char)s.p[i])) {
			digit = true;
			continue;
		}
		if (!tel_visual_sep(s.p[i]))
			return false;
	}

	return digit;
}

static size_t tel_sep_skip(struct sip_str s, size_t i)
{
	while (i < s.len && tel_visual_sep(s.p[i]))
		i++;

	return i;
}

/* The number form accepts visual separators, so +1(212)555-1234 and
 * +12125551234 are one number. A leading + is dropped on each side, because
 * the display name and the user part often disagree about it. */
static bool cid_number_same(struct sip_str a, struct sip_str b)
{
	size_t i = tel_sep_skip(a, 0), j = tel_sep_skip(b, 0);

	if (i < a.len && a.p[i] == '+')
		i++;
	if (j < b.len && b.p[j] == '+')
		j++;

	for (;;) {
		i = tel_sep_skip(a, i);
		j = tel_sep_skip(b, j);
		if (i >= a.len || j >= b.len)
			break;
		if (a.p[i] != b.p[j])
			return false;
		i++;
		j++;
	}

	return i >= a.len && j >= b.len;
}

/*
 * The caller id name a carrier sends. A STIR/SHAKEN gateway appends
 * verstat to the display name as well as to the user part, so the raw name
 * reads 2015550123;verstat=No-TN-Validation on the phone.
 *
 * A display name that is a telephone number up to its first semicolon keeps
 * that number, and keeps nothing at all when it only repeats the number the
 * caller already sends. Any other name loses a verstat parameter and nothing
 * else, so "Smith; John" stays whole.
 */
static struct sip_str cid_display_take(struct sip_str display,
				       struct sip_str number)
{
	struct sip_str head;
	const char *semi;
	size_t at;

	if (!display.len)
		return display;

	semi = memchr(display.p, ';', display.len);
	if (!semi)
		return display;

	head = sip_str_init(display.p, (size_t)(semi - display.p));
	if (tel_number_form(head))
		return cid_number_same(head, number) ?
		       sip_str_init(display.p, 0) : head;

	if (sip_str_find_ci(display, ";verstat=", &at))
		return sip_str_init(display.p, at);

	return display;
}

/* The identity is read after the cut, so a gateway that appends verstat to
 * the RFC 3323 anonymous form still withholds it. The host carries no
 * parameter of its own and is read raw. */
static bool cid_anonymous(const struct sip_addr *a, const struct sip_cid *cid)
{
	return sip_str_eq_ci(cid->number, "anonymous") ||
	       sip_str_eq_ci(a->uri.host, "anonymous.invalid") ||
	       sip_str_eq_ci(cid->name, "anonymous");
}

static bool cid_pick(const struct sip_msg *msg, enum sip_hdr_id id, struct sip_addr *out)
{
	struct sip_val_iter it;
	struct sip_addr a;
	bool have = false;

	if (!hdr_addr_first(msg, id, &it, &a))
		return false;

	do {
		if (!have) {
			*out = a;
			have = true;
		}
		/* P-Asserted-Identity often carries a sip and a tel form; the
		 * one with a number wins. */
		if (uri_number(&a.uri).len) {
			*out = a;
			return true;
		}
	} while (hdr_addr_next(&it, &a));

	return have;
}

bool hdr_cid_get(const struct sip_msg *msg, struct sip_cid *cid)
{
	struct sip_addr a;
	struct sip_str val;
	bool priv;

	memset(cid, 0, sizeof(*cid));

	if (cid_pick(msg, SIP_HDR_P_ASSERTED_IDENTITY, &a)) {
		cid->source = SIP_CID_PAI;
	} else if (cid_pick(msg, SIP_HDR_REMOTE_PARTY_ID, &a)) {
		cid->source = SIP_CID_RPID;
	} else if (cid_pick(msg, SIP_HDR_FROM, &a)) {
		cid->source = SIP_CID_FROM;
	} else {
		return false;
	}

	/* the number first: the name is cut against it */
	cid->number = uri_number(&a.uri);
	cid->name = cid_display_take(a.display, cid->number);

	if (cid_anonymous(&a, cid))
		cid->flags |= SIP_CID_F_ANONYMOUS | SIP_CID_F_NAME_PRIVATE |
			      SIP_CID_F_NUMBER_PRIVATE;

	priv = hdr_privacy_has(msg, "id") || hdr_privacy_has(msg, "full");
	if (priv)
		cid->flags |= SIP_CID_F_NAME_PRIVATE | SIP_CID_F_NUMBER_PRIVATE;

	if (cid->source == SIP_CID_RPID && sip_param_find(a.params, "privacy", &val)) {
		if (sip_str_eq_ci(val, "full"))
			cid->flags |= SIP_CID_F_NAME_PRIVATE | SIP_CID_F_NUMBER_PRIVATE;
		else if (sip_str_eq_ci(val, "name"))
			cid->flags |= SIP_CID_F_NAME_PRIVATE;
		else if (sip_str_eq_ci(val, "uri"))
			cid->flags |= SIP_CID_F_NUMBER_PRIVATE;
	}

	return true;
}

void route_set_collect(const struct sip_msg *msg, enum sip_hdr_id id,
		       struct sip_route_set *set)
{
	struct sip_val_iter it;
	struct sip_str val;

	memset(set, 0, sizeof(*set));
	if (!hdr_val_first(msg, id, &it, &val))
		return;

	do {
		if (set->count >= SIP_ROUTE_MAX) {
			set->overflow = true;
			return;
		}
		set->hop[set->count++] = val;
	} while (hdr_val_next(&it, &val));
}

void route_set_reverse(struct sip_route_set *set)
{
	struct sip_str tmp;
	unsigned int i, j;

	if (set->count < 2)
		return;

	for (i = 0, j = set->count - 1; i < j; i++, j--) {
		tmp = set->hop[i];
		set->hop[i] = set->hop[j];
		set->hop[j] = tmp;
	}
}

bool route_hop_lr(struct sip_str hop)
{
	struct sip_addr a;

	if (!hdr_addr_parse(hop, &a))
		return false;

	return (a.uri.flags & SIP_URI_F_LR) != 0;
}

void route_hdrs_clear(struct sip_route_hdrs *r)
{
	r->text[0] = 0;
	r->len = 0;
	r->hops = 0;
}

/*
 * A hop becomes a header field of the next request. The message parser ends a
 * line at the LF alone and keeps every other control byte of the value, so a
 * bare CR inside a hop would split the rendered block into two header lines
 * and a NUL would truncate it. Both are refused here, on the rule of
 * uri_headers_get().
 */
static bool route_set_ctl(const struct sip_route_set *set)
{
	unsigned int i;

	for (i = 0; i < set->count; i++) {
		if (ctl_present(set->hop[i].p, set->hop[i].len))
			return true;
	}

	return false;
}

bool route_hdrs_take(struct sip_route_hdrs *out, const struct sip_msg *msg,
		     enum sip_hdr_id id)
{
	struct sip_route_set set;
	struct sip_buf b;
	unsigned int i;

	route_hdrs_clear(out);
	route_set_collect(msg, id, &set);
	if (!set.count)
		return true;

	if (set.overflow || !route_hop_lr(set.hop[0]) || route_set_ctl(&set))
		return false;

	sip_buf_init(&b, out->text, sizeof(out->text));
	for (i = 0; i < set.count; i++)
		hdr_write_str(&b, SIP_HDR_ROUTE, set.hop[i]);

	if (!sip_buf_ok(&b)) {
		route_hdrs_clear(out);
		return false;
	}

	out->len = (uint16_t)sip_buf_len(&b);
	out->hops = (uint8_t)set.count;

	return true;
}

bool route_hdrs_write(struct sip_buf *b, const struct sip_route_hdrs *r)
{
	if (!r || !r->len)
		return sip_buf_ok(b);

	return sip_buf_add_len(b, r->text, r->len);
}

struct status_reason {
	uint32_t code;
	const char *reason;
};

static const struct status_reason reasons[] = {
	{ 100, "Trying" },
	{ 180, "Ringing" },
	{ 181, "Call Is Being Forwarded" },
	{ 182, "Queued" },
	{ 183, "Session Progress" },
	{ 200, "OK" },
	{ 202, "Accepted" },
	{ 302, "Moved Temporarily" },
	{ 400, "Bad Request" },
	{ 401, "Unauthorized" },
	{ 403, "Forbidden" },
	{ 404, "Not Found" },
	{ 405, "Method Not Allowed" },
	{ 406, "Not Acceptable" },
	{ 407, "Proxy Authentication Required" },
	{ 408, "Request Timeout" },
	{ 410, "Gone" },
	{ 413, "Request Entity Too Large" },
	{ 415, "Unsupported Media Type" },
	{ 420, "Bad Extension" },
	{ 422, "Session Interval Too Small" },
	{ 423, "Interval Too Brief" },
	{ 480, "Temporarily Unavailable" },
	{ 481, "Call/Transaction Does Not Exist" },
	{ 482, "Loop Detected" },
	{ 483, "Too Many Hops" },
	{ 484, "Address Incomplete" },
	{ 486, "Busy Here" },
	{ 487, "Request Terminated" },
	{ 488, "Not Acceptable Here" },
	{ 489, "Bad Event" },
	{ 491, "Request Pending" },
	{ 493, "Undecipherable" },
	{ 500, "Server Internal Error" },
	{ 501, "Not Implemented" },
	{ 502, "Bad Gateway" },
	{ 503, "Service Unavailable" },
	{ 504, "Server Time-out" },
	{ 505, "Version Not Supported" },
	{ 513, "Message Too Large" },
	{ 580, "Precondition Failure" },
	{ 600, "Busy Everywhere" },
	{ 603, "Decline" },
	{ 604, "Does Not Exist Anywhere" },
	{ 606, "Not Acceptable" },
};

const char *sip_status_reason(uint32_t code)
{
	size_t i;

	for (i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
		if (reasons[i].code == code)
			return reasons[i].reason;
	}

	switch (code / 100) {
	case 1:
		return "Provisional";
	case 2:
		return "OK";
	case 3:
		return "Redirection";
	case 4:
		return "Client Error";
	case 5:
		return "Server Error";
	case 6:
		return "Global Failure";
	}

	return "Unknown";
}

bool msg_req_line_write(struct sip_buf *b, const char *method, const char *ruri)
{
	if (!method || !ruri)
		return false;

	sip_buf_add(b, method);
	sip_buf_add(b, " ");
	sip_buf_add(b, ruri);
	sip_buf_add(b, " SIP/2.0");
	sip_buf_crlf(b);

	return sip_buf_ok(b);
}

bool msg_status_line_write(struct sip_buf *b, uint32_t code, const char *reason)
{
	sip_buf_addf(b, "SIP/2.0 %u ", code);
	sip_buf_add(b, reason ? reason : sip_status_reason(code));
	sip_buf_crlf(b);

	return sip_buf_ok(b);
}

bool hdr_write(struct sip_buf *b, enum sip_hdr_id id, const char *fmt, ...)
{
	va_list ap;
	char val[SIP_HDR_LEN_MAX];
	int n;

	va_start(ap, fmt);
	n = vsnprintf(val, sizeof(val), fmt, ap);
	va_end(ap);

	if (n < 0 || (size_t)n >= sizeof(val)) {
		b->overflow = true;
		return false;
	}

	sip_buf_add(b, sip_hdr_name(id));
	sip_buf_add(b, ": ");
	sip_buf_add_len(b, val, (size_t)n);
	sip_buf_crlf(b);

	return sip_buf_ok(b);
}

bool hdr_write_str(struct sip_buf *b, enum sip_hdr_id id, struct sip_str value)
{
	sip_buf_add(b, sip_hdr_name(id));
	sip_buf_add(b, ": ");
	sip_buf_add_str(b, value);
	sip_buf_crlf(b);

	return sip_buf_ok(b);
}

bool sip_host_bracket_needed(const char *host)
{
	return host && strchr(host, ':') && host[0] != '[';
}

bool hdr_via_write(struct sip_buf *b, const char *transport, const char *host, uint32_t port,
		   const char *branch, bool rport)
{
	bool bracket = sip_host_bracket_needed(host);

	sip_buf_add(b, sip_hdr_name(SIP_HDR_VIA));
	sip_buf_add(b, ": SIP/2.0/");
	sip_buf_add(b, transport ? transport : "UDP");
	sip_buf_add(b, " ");
	if (bracket)
		sip_buf_add(b, "[");
	sip_buf_add(b, host);
	if (bracket)
		sip_buf_add(b, "]");
	if (port)
		sip_buf_addf(b, ":%u", port);
	if (branch) {
		sip_buf_add(b, ";branch=");
		sip_buf_add(b, branch);
	}
	if (rport)
		sip_buf_add(b, ";rport");
	sip_buf_crlf(b);

	return sip_buf_ok(b);
}

bool hdr_cseq_write(struct sip_buf *b, uint32_t num, const char *method)
{
	return hdr_write(b, SIP_HDR_CSEQ, "%u %s", num, method ? method : "");
}

/*
 * RFC 3262 7.2: the RSeq acknowledged, then the CSeq number and method of the
 * response that carried it. The writer takes no guard against a NULL method,
 * because a NULL is a caller bug and the guard of hdr_cseq_write() would write
 * a header no peer can parse instead of failing here.
 */
bool hdr_rack_write(struct sip_buf *b, uint32_t rseq, uint32_t cseq,
		    const char *method)
{
	return hdr_write(b, SIP_HDR_RACK, "%u %u %s", rseq, cseq, method);
}

bool hdr_addr_write(struct sip_buf *b, enum sip_hdr_id id, const char *display, const char *uri,
		    const char *params)
{
	if (!uri)
		return false;

	sip_buf_add(b, sip_hdr_name(id));
	sip_buf_add(b, ": ");
	if (display && *display) {
		sip_buf_add_quoted(b, display);
		sip_buf_add(b, " ");
	}
	sip_buf_add(b, "<");
	sip_buf_add(b, uri);
	sip_buf_add(b, ">");
	sip_buf_add(b, params ? params : "");
	sip_buf_crlf(b);

	return sip_buf_ok(b);
}

bool hdr_addr_tag_write(struct sip_buf *b, enum sip_hdr_id id,
			const char *display, const char *uri, const char *tag)
{
	if (!uri)
		return false;

	sip_buf_add(b, sip_hdr_name(id));
	if (display && *display) {
		sip_buf_add(b, ": ");
		sip_buf_add_quoted(b, display);
		sip_buf_add(b, " <");
	} else {
		sip_buf_add(b, ": <");
	}
	sip_buf_add(b, uri);
	sip_buf_add(b, ">");
	if (tag) {
		sip_buf_add(b, ";tag=");
		sip_buf_add(b, tag);
	}
	sip_buf_crlf(b);

	return sip_buf_ok(b);
}

bool msg_body_write(struct sip_buf *b, const char *ctype, const char *body, size_t len)
{
	if (body && len && ctype)
		hdr_write(b, SIP_HDR_CONTENT_TYPE, "%s", ctype);

	hdr_write(b, SIP_HDR_CONTENT_LENGTH, "%zu", body ? len : 0);
	sip_buf_crlf(b);
	if (body && len)
		sip_buf_add_len(b, body, len);

	return sip_buf_ok(b);
}

bool msg_response_build(struct sip_buf *b, const struct sip_msg *req, uint32_t code,
			const char *reason, const char *to_tag, uint32_t flags)
{
	const struct sip_hdr *h;
	struct sip_addr to;
	struct sip_str from, to_val, call_id, cseq;
	bool tagged;

	if (!req || !req->is_request)
		return false;

	/*
	 * Everything the response needs is checked before a single byte is
	 * written, so a refusal never leaves half a message in the buffer.
	 * RFC 3261 8.2.6.2 makes Via, From, To, Call-ID and CSeq mandatory in
	 * the response, so a request that lacks any of them cannot be answered
	 * at the UA layer at all.
	 */
	if (!msg_hdr_first(req, SIP_HDR_VIA))
		return false;
	if (!msg_hdr_value(req, SIP_HDR_FROM, &from))
		return false;
	if (!msg_hdr_value(req, SIP_HDR_TO, &to_val))
		return false;
	if (!msg_hdr_value(req, SIP_HDR_CALL_ID, &call_id))
		return false;
	if (!msg_hdr_value(req, SIP_HDR_CSEQ, &cseq))
		return false;

	/* RFC 3261 8.2.6.2: every response above 100 carries a To tag. The
	 * caller owns tag generation, so a missing one is a caller bug, not
	 * something to paper over with an untagged To. */
	tagged = hdr_addr_parse(to_val, &to) && (to.flags & SIP_ADDR_F_TAG);
	if (code > 100 && !tagged && (!to_tag || !*to_tag))
		return false;

	msg_status_line_write(b, code, reason);

	for (h = msg_hdr_first(req, SIP_HDR_VIA); h; h = msg_hdr_next(req, h))
		hdr_write_str(b, SIP_HDR_VIA, h->value);

	if (flags & SIP_RESP_F_RECORD_ROUTE) {
		for (h = msg_hdr_first(req, SIP_HDR_RECORD_ROUTE); h;
		     h = msg_hdr_next(req, h))
			hdr_write_str(b, SIP_HDR_RECORD_ROUTE, h->value);
	}

	hdr_write_str(b, SIP_HDR_FROM, from);

	if (code > 100 && !tagged) {
		/* Written from the parts, not through hdr_write(), so a To
		 * header longer than SIP_HDR_LEN_MAX still round trips. */
		sip_buf_add(b, sip_hdr_name(SIP_HDR_TO));
		sip_buf_add(b, ": ");
		sip_buf_add_str(b, to_val);
		sip_buf_add(b, ";tag=");
		sip_buf_add(b, to_tag);
		sip_buf_crlf(b);
	} else {
		hdr_write_str(b, SIP_HDR_TO, to_val);
	}

	hdr_write_str(b, SIP_HDR_CALL_ID, call_id);
	hdr_write_str(b, SIP_HDR_CSEQ, cseq);

	return sip_buf_ok(b);
}
