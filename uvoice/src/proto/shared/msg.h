// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * SIP message object and tolerant zero copy parser (RFC 3261 section 7).
 *
 * Dependencies. libc only. No uloop, no ubus, no HAL, no allocation. Every
 * string this layer produces is a pointer into the caller's buffer plus a
 * length, so the same code runs unchanged in uvoiced, in a unit test and in a
 * fuzz harness.
 *
 * Buffer contract. msg_parse() takes a writable buffer. Header folding
 * (RFC 3261 7.3.1) is undone in place: the CR and the LF of a fold become
 * spaces, so a folded value stays one contiguous run and needs no copy.
 * Nothing else in the buffer is written. The buffer must outlive every
 * struct sip_msg that points into it. The parse buffer and the retransmit
 * buffer are separate for this reason.
 *
 * Limits. Three hard numbers, each with a different enforcement rule:
 *
 *   SIP_MSG_LEN_MAX   65535  a longer buffer is rejected. It is the recvmsg
 *                            bound of the UDP transport (64 KB receive buffer,
 *                            MSG_TRUNC drop and trace) and the largest
 *                            datagram UDP can carry.
 *   SIP_HDR_COUNT_MAX 64     a 65th header rejects the message. An index that
 *                            silently forgets headers would make routing and
 *                            authentication decisions on half a message.
 *   SIP_HDR_LEN_MAX   4096   a longer header line is flagged, not rejected
 *                            (SIP_HDR_F_OVERSIZE). uvoice must pass an
 *                            oversized Identity header (RFC 8224) through
 *                            unharmed, and nothing here copies a header into a
 *                            fixed buffer; the copy helpers all take a caller
 *                            length. The flag exists so a consumer that does
 *                            need a bounded copy can refuse early.
 *
 * Tolerance policy. A malformed header uvoice does not need must never fail
 * the message: a header without a colon is indexed with SIP_HDR_F_MALFORMED and
 * an empty value, unknown URI and header parameters are preserved verbatim, and
 * a bare LF terminator is accepted (RFC 3261 7.5 requires CRLF; deployed SBCs
 * send bare LF). The parser rejects only what makes the message unusable: an
 * unreadable start line, a missing header terminator with an incomplete last
 * header, and the two count limits above.
 */
#ifndef UVOICE_SIP_MSG_H
#define UVOICE_SIP_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIP_MSG_LEN_MAX		65535
#define SIP_HDR_COUNT_MAX	64
#define SIP_HDR_LEN_MAX		4096

/*
 * A span of the parse buffer. p is never NULL after a successful parse; len 0
 * means the element was present and empty (an empty Subject, for example).
 */
struct sip_str {
	const char *p;
	size_t len;
};

#define SIP_STR_PRI		"%.*s"
#define SIP_STR_ARG(s)		(int)(s).len, (s).p

enum sip_method {
	SIP_METHOD_UNKNOWN = 0,
	SIP_METHOD_INVITE,
	SIP_METHOD_ACK,
	SIP_METHOD_CANCEL,
	SIP_METHOD_BYE,
	SIP_METHOD_REGISTER,
	SIP_METHOD_OPTIONS,
	SIP_METHOD_INFO,
	SIP_METHOD_NOTIFY,
	SIP_METHOD_SUBSCRIBE,
	SIP_METHOD_UPDATE,
	SIP_METHOD_REFER,
	SIP_METHOD_PRACK,
	SIP_METHOD_MESSAGE,
	SIP_METHOD_MAX,
};

/*
 * Header identity. Every header uvoice reads or writes has an id; anything
 * else indexes as SIP_HDR_UNKNOWN and keeps its raw name and value.
 */
enum sip_hdr_id {
	SIP_HDR_UNKNOWN = 0,
	SIP_HDR_VIA,
	SIP_HDR_FROM,
	SIP_HDR_TO,
	SIP_HDR_CONTACT,
	SIP_HDR_CALL_ID,
	SIP_HDR_CSEQ,
	SIP_HDR_ROUTE,
	SIP_HDR_RECORD_ROUTE,
	SIP_HDR_SERVICE_ROUTE,
	SIP_HDR_MAX_FORWARDS,
	SIP_HDR_EXPIRES,
	SIP_HDR_MIN_EXPIRES,
	SIP_HDR_CONTENT_TYPE,
	SIP_HDR_CONTENT_LENGTH,
	SIP_HDR_CONTENT_ENCODING,
	SIP_HDR_CONTENT_DISPOSITION,
	SIP_HDR_WWW_AUTHENTICATE,
	SIP_HDR_PROXY_AUTHENTICATE,
	SIP_HDR_AUTHORIZATION,
	SIP_HDR_PROXY_AUTHORIZATION,
	SIP_HDR_AUTHENTICATION_INFO,
	SIP_HDR_SUPPORTED,
	SIP_HDR_REQUIRE,
	SIP_HDR_PROXY_REQUIRE,
	SIP_HDR_UNSUPPORTED,
	SIP_HDR_ALLOW,
	SIP_HDR_ALLOW_EVENTS,
	SIP_HDR_EVENT,
	SIP_HDR_SUBSCRIPTION_STATE,
	SIP_HDR_ALERT_INFO,
	SIP_HDR_SESSION_EXPIRES,
	SIP_HDR_MIN_SE,
	SIP_HDR_RSEQ,
	SIP_HDR_RACK,
	SIP_HDR_RETRY_AFTER,
	SIP_HDR_P_ASSERTED_IDENTITY,
	SIP_HDR_P_PREFERRED_IDENTITY,
	SIP_HDR_REMOTE_PARTY_ID,
	SIP_HDR_PRIVACY,
	SIP_HDR_REFER_TO,
	SIP_HDR_REFERRED_BY,
	SIP_HDR_REPLACES,
	SIP_HDR_SUBJECT,
	SIP_HDR_ACCEPT,
	SIP_HDR_USER_AGENT,
	SIP_HDR_SERVER,
	SIP_HDR_DATE,
	SIP_HDR_TIMESTAMP,
	SIP_HDR_WARNING,
	SIP_HDR_REASON,
	SIP_HDR_ID_MAX,
};

/* per-header parse state; a set flag never fails the message */
#define SIP_HDR_F_MALFORMED	(1u << 0)	/* no colon, or an empty name */
#define SIP_HDR_F_OVERSIZE	(1u << 1)	/* line longer than SIP_HDR_LEN_MAX */
#define SIP_HDR_F_FOLDED	(1u << 2)	/* value was unfolded in place */

struct sip_hdr {
	struct sip_str name;		/* as received, case preserved */
	struct sip_str value;		/* unfolded and trimmed */
	enum sip_hdr_id id;
	uint32_t flags;
};

/* message level parse state */
#define SIP_MSG_F_BARE_LF	(1u << 0)	/* a line ended with LF, no CR */
#define SIP_MSG_F_LEAD_CRLF	(1u << 1)	/* CRLF before the start line */
#define SIP_MSG_F_HDR_MALFORMED	(1u << 2)
#define SIP_MSG_F_HDR_OVERSIZE	(1u << 3)
#define SIP_MSG_F_FOLDED	(1u << 4)
#define SIP_MSG_F_NO_CLEN	(1u << 5)	/* body is rest of datagram */
#define SIP_MSG_F_BAD_CLEN	(1u << 6)	/* Content-Length is not a number */
#define SIP_MSG_F_BODY_SHORT	(1u << 7)	/* Content-Length beyond the buffer */
#define SIP_MSG_F_BODY_EXTRA	(1u << 8)	/* bytes after the declared body */
#define SIP_MSG_F_NO_BODY_SEP	(1u << 9)	/* headers ran to the buffer end */
#define SIP_MSG_F_BAD_VERSION	(1u << 10)	/* not SIP/2.0, answer 505 */

struct sip_msg {
	char *buf;
	size_t len;

	bool is_request;
	enum sip_method method;		/* request only */
	struct sip_str method_name;	/* request only, case preserved */
	struct sip_str ruri;		/* request only */
	struct sip_str version;		/* both, "SIP/2.0" */
	uint32_t status;		/* response only */
	struct sip_str reason;		/* response only, may be empty */

	struct sip_hdr hdr[SIP_HDR_COUNT_MAX];
	unsigned int hdr_count;

	struct sip_str body;
	uint32_t clen;			/* declared, valid if clen_present */
	bool clen_present;

	uint32_t flags;
};

enum sip_parse_status {
	SIP_PARSE_OK = 0,
	SIP_PARSE_ERR_EMPTY = -1,	/* only CRLF: a NAT keepalive ping */
	SIP_PARSE_ERR_TOO_LARGE = -2,
	SIP_PARSE_ERR_START_LINE = -3,
	SIP_PARSE_ERR_STATUS = -4,
	SIP_PARSE_ERR_HDR_OVERFLOW = -5,
	SIP_PARSE_ERR_HDR_TRUNCATED = -6,
};

enum sip_parse_status msg_parse(struct sip_msg *msg, char *buf, size_t len);
const char *sip_parse_status_name(enum sip_parse_status st);

const char *sip_method_name(enum sip_method method);
enum sip_method sip_method_parse(struct sip_str s);
const char *sip_hdr_name(enum sip_hdr_id id);
enum sip_hdr_id sip_hdr_id_parse(struct sip_str name);

const struct sip_hdr *msg_hdr_first(const struct sip_msg *msg, enum sip_hdr_id id);
const struct sip_hdr *msg_hdr_next(const struct sip_msg *msg, const struct sip_hdr *cur);
const struct sip_hdr *msg_hdr_name_first(const struct sip_msg *msg, const char *name);
unsigned int msg_hdr_count(const struct sip_msg *msg, enum sip_hdr_id id);
bool msg_hdr_value(const struct sip_msg *msg, enum sip_hdr_id id, struct sip_str *out);

/* body access */
bool msg_body_get(const struct sip_msg *msg, struct sip_str *out);
bool msg_body_sdp_get(const struct sip_msg *msg, struct sip_str *out);

struct sip_part {
	struct sip_str hdrs;
	struct sip_str body;
	struct sip_str ctype;		/* full Content-Type value, may be empty */
};

struct sip_part_iter {
	const char *cur;
	const char *end;
	struct sip_str boundary;
	bool done;
};

bool sip_ctype_boundary(struct sip_str ctype, struct sip_str *out);
bool part_first(struct sip_str body, struct sip_str boundary, struct sip_part_iter *it,
		struct sip_part *part);
bool part_next(struct sip_part_iter *it, struct sip_part *part);
bool msg_part_first(const struct sip_msg *msg, struct sip_part_iter *it, struct sip_part *part);

/*
 * Lexer primitives shared with the typed header layer (shared/hdr.h). They are
 * here because the message parser needs them first and because duplicating a
 * quote aware scanner is how parsers grow interop bugs.
 */
struct sip_str sip_str_init(const char *p, size_t len);
struct sip_str sip_str_cstr(const char *s);
struct sip_str sip_str_trim(struct sip_str s);
bool sip_str_eq(struct sip_str s, const char *lit);
bool sip_str_eq_ci(struct sip_str s, const char *lit);
bool sip_str_prefix(struct sip_str s, const char *lit);
bool sip_str_prefix_ci(struct sip_str s, const char *lit);
bool sip_str_find_ci(struct sip_str s, const char *lit, size_t *at);
bool sip_str_uint(struct sip_str s, uint32_t *out);
bool sip_str_copy(char *dst, size_t dlen, struct sip_str s);

/* one whitespace separated word, leading whitespace skipped */
bool sip_word_take(struct sip_str *rest, struct sip_str *word);

/*
 * Every scan below treats a quoted string (RFC 3261 25.1, backslash escapes
 * included) and an angle bracketed URI as opaque, so a comma inside
 * qop="auth,auth-int" or a semicolon inside <sip:a@b;lr> never splits a value.
 */
size_t sip_sep_find(struct sip_str s, char sep);
bool sip_kv_split(struct sip_str item, struct sip_str *name, struct sip_str *val);
bool sip_csv_next(struct sip_str *rest, struct sip_str *item);
bool sip_value_split(struct sip_str v, struct sip_str *head, struct sip_str *params);
bool sip_param_next(struct sip_str *rest, struct sip_str *name, struct sip_str *val);
bool sip_param_find(struct sip_str params, const char *name, struct sip_str *val);
bool sip_param_present(struct sip_str params, const char *name);
struct sip_str sip_quoted_strip(struct sip_str s);
int sip_unquote(struct sip_str in, char *out, size_t olen);
int sip_uri_unescape(struct sip_str in, char *out, size_t olen);
struct sip_str sip_media_type(struct sip_str ctype);

#endif
