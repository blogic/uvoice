// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Typed SIP header parsing and serialization.
 *
 * The header set is exactly the one the product needs: dialog and transaction
 * headers, registration, digest authentication, the option tag lists, MWI,
 * distinctive ring, session timers, the 420 path and the caller id sources.
 * Anything else stays a raw span in the message index.
 *
 * Same rules as shared/msg.h: libc only, no allocation, every parsed string is
 * a span of the parse buffer. The builders write into a caller owned buffer
 * through struct sip_buf and never allocate either.
 *
 * Two dimensions per header. A header uvoice reads can repeat as several
 * header lines and carry several comma separated values on one line
 * (RFC 3261 7.3.1). hdr_val_first()/hdr_val_next() walk both dimensions in
 * receive order, and the Via, address and route helpers are built on them.
 * Headers whose value must not be comma split (Call-ID, Subject, the
 * authentication headers) have their own accessors.
 *
 * Case. Header names and parameter names are case insensitive; token values
 * that RFC 3261 defines as case insensitive (transport, refresher, subscription
 * state, option tags) are compared that way here. Method names, tags, branch
 * values, Call-ID and quoted strings keep their case.
 */
#ifndef UVOICE_SIP_HDR_H
#define UVOICE_SIP_HDR_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/shared/msg.h"

/*
 * Bounded output buffer. Every writer returns false once the buffer is full
 * and leaves the overflow sticky, so a caller can build a whole message and
 * test once at the end.
 */
struct sip_buf {
	char *buf;
	size_t size;
	size_t used;
	bool overflow;
};

void sip_buf_init(struct sip_buf *b, char *mem, size_t size);
bool sip_buf_add(struct sip_buf *b, const char *s);
bool sip_buf_add_len(struct sip_buf *b, const char *s, size_t len);
bool sip_buf_add_str(struct sip_buf *b, struct sip_str s);
bool sip_buf_addf(struct sip_buf *b, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
bool sip_buf_add_quoted(struct sip_buf *b, const char *s);
bool sip_buf_crlf(struct sip_buf *b);
bool sip_buf_ok(const struct sip_buf *b);
size_t sip_buf_len(const struct sip_buf *b);
const char *sip_buf_str(const struct sip_buf *b);

/* SIP URI (RFC 3261 19.1) plus tel: (RFC 3966) for caller id sources */
#define SIP_URI_F_SECURE	(1u << 0)	/* sips: */
#define SIP_URI_F_TEL		(1u << 1)	/* tel:, user holds the number */
#define SIP_URI_F_IPV6		(1u << 2)	/* host was bracketed */
#define SIP_URI_F_PORT		(1u << 3)
#define SIP_URI_F_LR		(1u << 4)
#define SIP_URI_F_ESCAPED_USER	(1u << 5)	/* userinfo holds a % escape */

struct sip_uri {
	struct sip_str raw;
	struct sip_str scheme;
	struct sip_str user;		/* escapes preserved, never decoded here */
	struct sip_str password;
	struct sip_str host;		/* IPv6 without the brackets */
	struct sip_str params;		/* from the first ';', separators kept */
	struct sip_str headers;		/* after the first '?', kept verbatim */
	struct sip_str transport;
	struct sip_str user_param;
	struct sip_str maddr;
	uint32_t port;
	uint32_t flags;
};

bool uri_parse(struct sip_str s, struct sip_uri *uri);
bool uri_serialize(const struct sip_uri *uri, struct sip_buf *b);

/*
 * The URI headers part (RFC 3261 25.1), which uri_parse() keeps verbatim in
 * sip_uri.headers: hname "=" hvalue pairs separated by '&'. A REFER carries the
 * Replaces of an attended transfer there (RFC 3515 2.4.2), and RFC 3261 25.1
 * escapes every separator inside both halves, so the pairs are read and written
 * through these two calls alone and never through the parameter walkers.
 *
 * uri_headers_get() matches hname case insensitively, takes the first pair that
 * matches, unescapes hvalue into out and NUL terminates it. A value that does
 * not fit olen is refused, not truncated, and so is a broken escape ("%G1", or
 * a '%' at the end). A value that decodes to a control byte (< 0x20 or 0x7F: a
 * %0D %0A that would inject a header line, a %00 that would truncate the field)
 * is refused too. out is cleared on every refusal, because the caller builds a
 * header field out of these bytes.
 *
 * uri_header_escape() writes the other direction. unreserved and hnv-unreserved
 * pass through and everything else becomes %XX with upper case hex, ';', '=',
 * '&', '%' and space included.
 */
bool uri_headers_get(struct sip_str headers, const char *name, char *out, size_t olen);
bool uri_header_escape(struct sip_buf *b, const char *val);

/*
 * uri_user_escape() writes a userinfo (RFC 3261 25.1 user) for a dialled or
 * configured number. It hides the dial symbols the esc mask names as %XX
 * with upper case hex; every other byte passes through unvalidated. A '#'
 * is not a user character, so escaping it is what makes the URI legal; a
 * '*' is a mark and legal either way. Which of the two is hidden is the
 * operator's digit_esc_enabled policy (call.c), not a grammar fact, so the
 * contract here is the mask and not the uci value. A result that does not
 * fit dlen is refused, not truncated, and dst is cleared on the refusal. A
 * NULL or empty user writes "" and returns true; a zero dlen returns false.
 */
#define SIP_URI_ESC_POUND	(1u << 0)
#define SIP_URI_ESC_STAR	(1u << 1)

bool uri_user_escape(char *dst, size_t dlen, const char *user, unsigned int esc);

/* name-addr / addr-spec (RFC 3261 20.10, 20.20, 20.39) */
#define SIP_ADDR_F_STAR		(1u << 0)	/* Contact: * */
#define SIP_ADDR_F_NAME_ADDR	(1u << 1)	/* URI was in angle brackets */
#define SIP_ADDR_F_QUOTED	(1u << 2)	/* display name was quoted */
#define SIP_ADDR_F_TAG		(1u << 3)
#define SIP_ADDR_F_Q		(1u << 4)
#define SIP_ADDR_F_EXPIRES	(1u << 5)

struct sip_addr {
	struct sip_str raw;
	struct sip_str display;		/* quotes stripped, escapes preserved */
	struct sip_str params;		/* header parameters, not URI parameters */
	struct sip_str tag;
	struct sip_uri uri;
	uint32_t q_milli;		/* q=0.7 -> 700 */
	uint32_t expires;
	uint32_t flags;
};

/*
 * hdr_addr_parse() fails only when there is no address at all. A value whose
 * URI does not parse still returns true with the display name, the tag and the
 * header parameters filled in, because a From uvoice cannot read must not cost
 * it the dialog identifiers. A caller that needs the URI must test
 * a->uri.host.len (or SIP_ADDR_F_STAR for a Contact).
 */
bool hdr_addr_parse(struct sip_str v, struct sip_addr *a);
int hdr_addr_display_get(const struct sip_addr *a, char *out, size_t olen);

/* Via (RFC 3261 20.42) */
#define SIP_VIA_F_IPV6		(1u << 0)
#define SIP_VIA_F_PORT		(1u << 1)
#define SIP_VIA_F_BRANCH	(1u << 2)
#define SIP_VIA_F_MAGIC		(1u << 3)	/* branch starts with z9hG4bK */
#define SIP_VIA_F_RPORT		(1u << 4)	/* ;rport present */
#define SIP_VIA_F_RPORT_VAL	(1u << 5)	/* ;rport=<n> */
#define SIP_VIA_F_RECEIVED	(1u << 6)
#define SIP_VIA_F_TTL		(1u << 7)

struct sip_via {
	struct sip_str raw;
	struct sip_str proto;		/* SIP */
	struct sip_str ver;		/* 2.0 */
	struct sip_str transport;	/* UDP, TCP, TLS */
	struct sip_str host;
	struct sip_str params;
	struct sip_str branch;
	struct sip_str received;
	struct sip_str maddr;
	uint32_t port;
	uint32_t rport;
	uint32_t ttl;
	uint32_t flags;
};

bool hdr_via_parse(struct sip_str v, struct sip_via *via);
bool hdr_via_top(const struct sip_msg *msg, struct sip_via *via);
unsigned int hdr_via_count(const struct sip_msg *msg);

/* value walk over header instances and comma separated values */
struct sip_val_iter {
	const struct sip_msg *msg;
	const struct sip_hdr *hdr;
	struct sip_str rest;
};

bool hdr_val_first(const struct sip_msg *msg, enum sip_hdr_id id, struct sip_val_iter *it,
		   struct sip_str *val);
bool hdr_val_next(struct sip_val_iter *it, struct sip_str *val);
unsigned int hdr_val_count(const struct sip_msg *msg, enum sip_hdr_id id);

bool hdr_addr_first(const struct sip_msg *msg, enum sip_hdr_id id, struct sip_val_iter *it,
		    struct sip_addr *a);
bool hdr_addr_next(struct sip_val_iter *it, struct sip_addr *a);
bool hdr_addr_get(const struct sip_msg *msg, enum sip_hdr_id id, struct sip_addr *a);

/* simple headers */

/*
 * RFC 3261 8.1.1.5 keeps the CSeq number below 2**31. A larger value is
 * refused by the parsers below, so the transaction layer never has to defend
 * its own increment against a number the standard forbids.
 *
 * RSeq does not share the bound. RFC 3262 7.1 gives the header the range 1 to
 * 2**32-1, and section 3 bounds only the first RSeq of a transaction to
 * 1..2**31-1. Each further RSeq rises by exactly one and never wraps, so no
 * RSeq is 0. hdr_rseq_get() and the response number of hdr_rack_get()
 * therefore refuse 0 and take the whole range above it.
 */
#define SIP_CSEQ_MAX		2147483647u

bool hdr_call_id_get(const struct sip_msg *msg, struct sip_str *out);
bool hdr_cseq_parse(struct sip_str v, uint32_t *num, enum sip_method *method,
		    struct sip_str *method_name);
bool hdr_cseq_get(const struct sip_msg *msg, uint32_t *num, enum sip_method *method);
bool hdr_uint_get(const struct sip_msg *msg, enum sip_hdr_id id, uint32_t *out);
bool hdr_rseq_get(const struct sip_msg *msg, uint32_t *rseq);
bool hdr_rack_get(const struct sip_msg *msg, uint32_t *rseq, uint32_t *cseq,
		  enum sip_method *method);
bool hdr_retry_after_get(const struct sip_msg *msg, uint32_t *secs, uint32_t *duration);
bool hdr_content_type_get(const struct sip_msg *msg, struct sip_str *type,
			  struct sip_str *params);

/* session timers (RFC 4028) */
enum sip_refresher {
	SIP_REFRESHER_NONE = 0,
	SIP_REFRESHER_UAC,
	SIP_REFRESHER_UAS,
};

bool hdr_session_expires_get(const struct sip_msg *msg, uint32_t *delta,
			     enum sip_refresher *refresher);
bool hdr_min_se_get(const struct sip_msg *msg, uint32_t *delta);

/* option tag lists: Supported, Require, Unsupported, Proxy-Require, Allow */
bool hdr_token_has(const struct sip_msg *msg, enum sip_hdr_id id, const char *token);
bool hdr_privacy_has(const struct sip_msg *msg, const char *value);

/* digest challenge (RFC 3261 22, RFC 7616) */
#define SIP_CHAL_F_DIGEST	(1u << 0)
#define SIP_CHAL_F_STALE	(1u << 1)
#define SIP_CHAL_F_QOP		(1u << 2)
#define SIP_CHAL_F_QOP_AUTH	(1u << 3)
#define SIP_CHAL_F_QOP_AUTH_INT	(1u << 4)

struct sip_challenge {
	struct sip_str scheme;
	struct sip_str realm;		/* quotes stripped, escapes preserved */
	struct sip_str nonce;
	struct sip_str opaque;
	struct sip_str algorithm;
	struct sip_str qop;
	struct sip_str domain;
	uint32_t flags;
};

struct sip_chal_iter {
	const struct sip_msg *msg;
	const struct sip_hdr *hdr;
	enum sip_hdr_id id;
	struct sip_str rest;
};

bool hdr_chal_parse(struct sip_str v, struct sip_chal_iter *it, struct sip_challenge *c);
bool hdr_chal_first(const struct sip_msg *msg, enum sip_hdr_id id, struct sip_chal_iter *it,
		    struct sip_challenge *c);
bool hdr_chal_next(struct sip_chal_iter *it, struct sip_challenge *c);

/*
 * Digest credentials, send side. A NULL field is omitted, except that qop
 * requires cnonce and a non-zero nc: hdr_cred_write() refuses the write
 * otherwise, because the digest was computed over both and a server that
 * receives qop without cnonce rejects the credentials (RFC 3261 22.4,
 * RFC 7616 3.4).
 */
struct sip_cred {
	const char *username;
	const char *realm;
	const char *nonce;
	const char *uri;
	const char *response;
	const char *algorithm;
	const char *cnonce;
	const char *qop;
	const char *opaque;
	uint32_t nc;
};

bool hdr_cred_write(struct sip_buf *b, enum sip_hdr_id id, const struct sip_cred *cred);

/* Event and Subscription-State (RFC 3265) */
struct sip_event {
	struct sip_str package;
	struct sip_str id;
	struct sip_str params;
};

#define SIP_SUB_F_EXPIRES	(1u << 0)
#define SIP_SUB_F_RETRY_AFTER	(1u << 1)

struct sip_sub_state {
	struct sip_str state;
	struct sip_str reason;
	struct sip_str params;
	uint32_t expires;
	uint32_t retry_after;
	uint32_t flags;
};

bool hdr_event_get(const struct sip_msg *msg, struct sip_event *ev);
bool hdr_sub_state_get(const struct sip_msg *msg, struct sip_sub_state *st);

/*
 * Replaces (RFC 3891) and message/sipfrag (RFC 3420), the two grammars a REFER
 * transfer needs beside Refer-To and Referred-By.
 *
 * Both tags are required. RFC 3891 6.1 gives a Replaces exactly one to-tag and
 * exactly one from-tag, so hdr_replaces_parse() refuses a value that omits one
 * or carries one twice: a half identified dialog would replace the wrong call.
 * Unknown parameters are ignored, the way every other parser here treats them.
 *
 * The tags are oriented at the receiver (RFC 3891 3): the to-tag parameter is
 * compared to the local tag of the dialog at the UA that receives the Replaces
 * and the from-tag to its remote tag. A transferor that builds the value out of
 * its own dialog therefore writes its remote tag as to-tag.
 *
 * early-only asks the receiver to replace an early dialog alone (RFC 3891 6.1).
 * It is a flag parameter, so a value behind it is accepted and ignored.
 *
 * An unmatched quote in the parameters fails the value closed. The parameter
 * walk finds its separators with the quote aware scan, which treats an opening
 * quote as a run that swallows every ';' until the closing one, so an
 * unterminated quote hides the whole tail of the value: a parameter behind it,
 * ";early-only" above all, is invisible rather than merely unread, and the two
 * tags in front of it would still have parsed. This one grammar decides which
 * call a request replaces, so a value this side cannot read to its end is
 * refused and its caller answers 400.
 *
 * The rule covers the parameters and not the whole value, because the Call-ID
 * is cut at the first semicolon raw: RFC 3261 25.1 lets a callid word carry a
 * '"', that quote reaches no scanner, and failing such a value would refuse a
 * Call-ID this parser has always accepted.
 *
 * hdr_replaces_write() writes the value only, without the header name, because
 * the same bytes ride a Replaces header field and, escaped by
 * uri_header_escape(), a Refer-To URI header. It refuses a NULL, an empty or a
 * control byte bearing field, symmetric with uri_headers_get(), so a corrupted
 * tag cannot inject a header line.
 *
 * sipfrag_write() emits a status line and nothing else, which is the whole
 * NOTIFY body of a transfer (RFC 3515 2.4.5). sipfrag_status() reads the
 * leading status line of such a body and ignores whatever follows it, because
 * that line carries the outcome and the headers behind it carry nothing uvoice
 * acts on. It sets *code to 0 on every false return, so the caller reads a
 * deterministic code without testing the bool first.
 */
struct sip_replaces {
	struct sip_str call_id;
	struct sip_str to_tag;
	struct sip_str from_tag;
	bool early_only;
};

bool hdr_replaces_parse(struct sip_str v, struct sip_replaces *out);
bool hdr_replaces_write(struct sip_buf *b, const char *call_id, const char *to_tag,
			const char *from_tag, bool early_only);
bool sipfrag_write(struct sip_buf *b, uint32_t code, const char *reason);
bool sipfrag_status(struct sip_str body, uint32_t *code);

/*
 * Alert-Info, the four deployed dialects:
 *
 *   BroadSoft  Alert-Info: <http://127.0.0.1/Bellcore-dr2>
 *              the prefix is the UCI option alert_info_prefix, default
 *              "Bellcore-dr"; the digit behind it selects the ring cadence.
 *   Nortel     Alert-Info: <http://127.0.0.1/nortel/dst_ring_3>
 *              also splash_ring and standard_ring. The three sentinels are
 *              fixed and only the URL carries them.
 *   Sylantro   Alert-Info: <http://host/x.wav>;info=alert-external;delay=0
 *              the pattern token is alert-<name>, in the info parameter or in
 *              the URL itself.
 *   NZ         Alert-Info: <http://127.0.0.1/da3>
 *              the sentinel "da" carries a digit 1 to 4. It starts the URL or
 *              follows '/', '=', '-', '?' or '&', so a file name that ends in
 *              "da2" is not a ring selector.
 *
 * One value is tested in that order and the first dialect that matches wins.
 * alert_info_prefix governs the BroadSoft branch alone; the other three
 * sentinels are fixed, so a configured prefix replaces "Bellcore-dr" and
 * nothing else.
 *
 * The first occurrence of a sentinel decides. The BroadSoft and Nortel
 * branches search once, so a digit outside the range behind the first hit
 * ends that branch instead of sending the search after a second hit: a URL
 * that carries one sentinel twice is a switch defect, not a form to
 * interpret. Only the NZ branch keeps searching, because its two letter
 * sentinel occurs inside ordinary words.
 *
 * Precedence across values is the caller's. hdr_alert_info_first() and
 * hdr_alert_info_next() walk every value of every Alert-Info header in receive
 * order, exactly as hdr_addr_first() and hdr_addr_next() do, and skip a value
 * that carries no URL. Which value wins is a policy question this layer cannot
 * answer: call.c takes the first value that names a ring template it can play,
 * then the first value with a dialect, then the first value that parses.
 * hdr_alert_info_get() serves a caller that needs no choice and returns the
 * first value that parses.
 */
#define SIP_ALERT_PREFIX_DEFAULT	"Bellcore-dr"

/* the two Nortel token forms, which ride sip_alert_info.ring beside its digits */
#define SIP_ALERT_NORTEL_SPLASH		10
#define SIP_ALERT_NORTEL_STANDARD	11

enum sip_alert_dialect {
	SIP_ALERT_NONE = 0,
	SIP_ALERT_BELLCORE,
	SIP_ALERT_NORTEL,
	SIP_ALERT_SYLANTRO,
	SIP_ALERT_NZ,
	SIP_ALERT_OTHER,
};

struct sip_alert_info {
	struct sip_str url;
	struct sip_str pattern;		/* Bellcore-dr2, dst_ring_3, alert-external, da4 */
	struct sip_str params;
	enum sip_alert_dialect dialect;
	/*
	 * The digit as received, scoped to the dialect that received it: 1..9
	 * for BroadSoft, 1..9 or one of the two token forms above for Nortel,
	 * 1..4 for NZ, 0 for the rest. It is not a ring template id; the map
	 * from one dialect to the ring vocabulary belongs to call.c.
	 */
	uint32_t ring;
};

bool hdr_alert_info_first(const struct sip_msg *msg, const char *prefix,
			  struct sip_val_iter *it, struct sip_alert_info *ai);
bool hdr_alert_info_next(struct sip_val_iter *it, const char *prefix,
			 struct sip_alert_info *ai);
bool hdr_alert_info_get(const struct sip_msg *msg, const char *prefix,
			struct sip_alert_info *ai);

/*
 * Caller id source order: P-Asserted-Identity, then Remote-Party-ID, then
 * From. Privacy comes from the Privacy header (RFC 3323), from the
 * Remote-Party-ID privacy parameter, and from the anonymous identity of
 * RFC 3323 5. The anonymous test reads the number and the name below, so a
 * verstat parameter on either one does not hide it.
 */
#define SIP_CID_F_NAME_PRIVATE		(1u << 0)
#define SIP_CID_F_NUMBER_PRIVATE	(1u << 1)
#define SIP_CID_F_ANONYMOUS		(1u << 2)

enum sip_cid_source {
	SIP_CID_NONE = 0,
	SIP_CID_PAI,
	SIP_CID_RPID,
	SIP_CID_FROM,
};

struct sip_cid {
	/*
	 * The display name, escapes preserved, cut at the STIR/SHAKEN
	 * verstat parameter a carrier appends to it. The name
	 * is empty when it carries the number and nothing else; the line
	 * layer then reports the number with no name.
	 */
	struct sip_str name;
	struct sip_str number;		/* userinfo, % escapes preserved */
	enum sip_cid_source source;
	uint32_t flags;
};

bool hdr_cid_get(const struct sip_msg *msg, struct sip_cid *cid);

/* route sets (RFC 3261 12.1.1, 12.2.1.1) */
#define SIP_ROUTE_MAX		10

struct sip_route_set {
	struct sip_str hop[SIP_ROUTE_MAX];	/* full name-addr, re-emittable */
	unsigned int count;
	bool overflow;
};

void route_set_collect(const struct sip_msg *msg, enum sip_hdr_id id,
		       struct sip_route_set *set);
void route_set_reverse(struct sip_route_set *set);
bool route_hop_lr(struct sip_str hop);

/*
 * A route set a request carries, already rendered as "Route: <hop>\r\n" lines.
 * The hops of a response point into the receive buffer, so a caller that keeps
 * them past that buffer has to copy them, and the rendered form makes the copy
 * cost exactly the wire cost. text is NUL terminated and len counts the bytes
 * before the terminator, so both a sip_buf writer and a plain string reader
 * take it.
 */
#define SIP_ROUTE_HDRS_MAX	512

struct sip_route_hdrs {
	char text[SIP_ROUTE_HDRS_MAX];
	uint16_t len;
	uint8_t hops;
};

void route_hdrs_clear(struct sip_route_hdrs *r);

/*
 * Render the route set of header id into out, in receive order.
 *
 * True says out holds every hop the message carried, and an absent header is
 * that case with no hop at all. False says the route is refused whole and out
 * is empty; the hop count of out therefore tells a caller what it stored and
 * the return tells it whether a route was thrown away. out is never truncated:
 * a partial route would send a request to the wrong proxy.
 *
 * Four routes are refused. More hops than SIP_ROUTE_MAX, a rendered block
 * longer than SIP_ROUTE_HDRS_MAX, a first hop without ";lr", and a hop that
 * holds a control byte. A first hop without ";lr" is a strict router, and
 * RFC 3261 section 12.2.1.1 puts its URI in the request line and moves the
 * callee into the last Route header, where leg_ruri_build(), the CDR and the
 * call trace no longer read it. A control byte, a bare CR and a NUL among
 * them, would split or truncate the rendered header line.
 */
bool route_hdrs_take(struct sip_route_hdrs *out, const struct sip_msg *msg,
		     enum sip_hdr_id id);

/* Write the rendered block. A NULL r and an empty block write nothing. */
bool route_hdrs_write(struct sip_buf *b, const struct sip_route_hdrs *r);

/* builders */
#define SIP_RESP_F_RECORD_ROUTE	(1u << 0)	/* copy Record-Route, 2xx to INVITE */

const char *sip_status_reason(uint32_t code);
bool msg_req_line_write(struct sip_buf *b, const char *method, const char *ruri);
bool msg_status_line_write(struct sip_buf *b, uint32_t code, const char *reason);
bool hdr_write(struct sip_buf *b, enum sip_hdr_id id, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));
bool hdr_write_str(struct sip_buf *b, enum sip_hdr_id id, struct sip_str value);

/*
 * True when host is an IPv6 literal that a URI wraps in square brackets
 * (RFC 3261 19.1.1 and 25.1): the host holds a colon and does not already
 * start with a bracket. hdr_via_write() applies it to the sent-by. A
 * builder that writes a host into a URI by hand must ask it too, so one
 * rule serves every URI this stack writes.
 */
bool sip_host_bracket_needed(const char *host);
bool hdr_via_write(struct sip_buf *b, const char *transport, const char *host, uint32_t port,
		   const char *branch, bool rport);
bool hdr_cseq_write(struct sip_buf *b, uint32_t num, const char *method);
bool hdr_rack_write(struct sip_buf *b, uint32_t rseq, uint32_t cseq,
		    const char *method);
bool hdr_addr_write(struct sip_buf *b, enum sip_hdr_id id, const char *display,
		    const char *uri, const char *params);

/*
 * hdr_addr_write() with a tag in place of params. A NULL tag writes no tag
 * parameter, and an empty tag writes ";tag=" with no value. The tag goes into
 * b directly, because RFC 3261 does not bound a tag. A fixed buffer would cut
 * a long tag into a wrong tag, which the peer answers with 481.
 */
bool hdr_addr_tag_write(struct sip_buf *b, enum sip_hdr_id id,
			const char *display, const char *uri, const char *tag);
bool msg_body_write(struct sip_buf *b, const char *ctype, const char *body, size_t len);

/*
 * Build a response to req per RFC 3261 8.2.6.2. The request must carry Via,
 * From, To, Call-ID and CSeq; a request without them cannot be answered and
 * the call fails before anything is written, so a refusal never leaves a
 * partial message in the buffer.
 *
 * to_tag is required for every code above 100 unless the request To already
 * carries a tag, and the call fails without one. The caller owns tag
 * generation, exactly as it owns the Via branch.
 *
 * The builder does not write Content-Length. Finish every response with
 * msg_body_write(), which writes Content-Type, Content-Length and the empty
 * line.
 */
bool msg_response_build(struct sip_buf *b, const struct sip_msg *req, uint32_t code,
			const char *reason, const char *to_tag, uint32_t flags);

#endif
