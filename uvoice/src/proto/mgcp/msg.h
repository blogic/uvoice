// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * MGCP message object, tolerant zero copy parser and builders (RFC 3435
 * section 3 and Appendix A).
 *
 * Dependencies. libc, plus the span and the bounded buffer of the SIP text
 * layer (struct sip_str in shared/msg.h, struct sip_buf in shared/hdr.h). No
 * uloop, no socket, no allocation.
 *
 * Buffer contract. mgcp_msg_parse() reads the caller's buffer and never
 * writes it, because MGCP has no line folding. Every string the layer
 * produces is a span of that buffer, so the buffer must outlive every
 * struct mgcp_msg that points into it. After a successful parse no span of
 * struct mgcp_msg has a NULL pointer; len 0 means empty or absent.
 *
 * Lines. A parameter line ends at the first line end outside a quoted
 * string, because Appendix A quoteChar admits CR and LF; a quote that never
 * closes ends the line at its first line end. A DQUOTE opens a quoted
 * string only where a value starts: first in the parameter value, or first
 * in an event parameter value after "(", or after "," or "=" inside
 * parentheses (Appendix A pages 172 and 173). Any other DQUOTE is a
 * literal character. The first line and the session description end at
 * every line end. The parser and the split share this rule.
 *
 * Datagram split (3.5.5). One datagram can carry several messages, each
 * separated from the next by a line that holds a single ".". A separator
 * line is recognized at the start of any line, inside a session description
 * too, but not inside a quoted string of a parameter line. RFC 3435 3.4
 * makes a sender emit strictly conformant SDP, and every SDP line has the
 * form "<type>=<value>", so a "." line is never part of a body.
 *
 * Limits.
 *
 *   MGCP_MSG_LEN_MAX    65507  a longer buffer is refused: it is the largest
 *                              MGCP message UDP can carry (3.5.4).
 *   MGCP_PARAM_LINE_MAX 64     a 65th parameter line refuses the message, so
 *                              that no decision rests on half a message.
 *   MGCP_TXID_MAX       999999999, the upper bound of 3.2.1.2.
 *
 * Tolerance policy. Verbs, parameter codes and keywords compare case
 * insensitively (3.2). Bare LF and CRLF are both accepted (Appendix A, EOL).
 * Extra white space between the words of the first line is accepted (3.2.1),
 * and a line that holds only white space is the empty line that ends the
 * parameters (Appendix A). A parameter line without a colon is indexed as an
 * unknown parameter with an empty value and flagged. An unknown verb and an
 * unknown parameter code keep their raw names. A version other than
 * "MGCP <d>.<d>" parses and is flagged, so the caller can answer 528
 * (3.2.1.4). A last line without its line end, a parameter line or an SDP
 * line, is accepted and flagged MGCP_MSG_F_NO_FINAL_EOL: a UDP datagram
 * arrives whole, so its end ends the message. The parser refuses only what
 * makes the message unusable:
 *   - an unreadable first line;
 *   - a transaction id outside 1 to MGCP_TXID_MAX, or with more than nine
 *     digits (3.2.1.2);
 *   - more than MGCP_PARAM_LINE_MAX parameter lines.
 *
 * Event and signal names (3.2.2.4). A name is [package "/"] event
 * ["@" connection]. A name without a package takes the default package of
 * the endpoint type, which the caller supplies.
 *
 * Builders. The builders write CRLF line ends into a struct sip_buf. A
 * builder refuses a value that holds a control character, a quoted CR or LF
 * included, because a gateway never sends one and it would split a line. A
 * response comment admits %x20-7E alone (Appendix A responseString). A
 * builder that runs out of buffer or refuses a value refuses the whole
 * message: it puts the buffer back in its state before the call, the
 * overflow flag included, and returns false. A short buffer therefore never
 * holds a truncated message, and the caller can write a shorter message
 * into the same buffer.
 */
#ifndef UVOICE_MGCP_MSG_H
#define UVOICE_MGCP_MSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/shared/hdr.h"
#include "proto/shared/msg.h"

#define MGCP_MSG_LEN_MAX	65507
#define MGCP_PARAM_LINE_MAX	64
#define MGCP_TXID_MAX		999999999u

/* 3.2.1.3: each half of an endpoint name holds up to 255 characters. */
#define MGCP_NAME_PART_MAX	255

/* The two version tokens of RFC 3435 3.2.1.4 and NCS 1.5 8.2.1.4. */
#define MGCP_VERSION_RFC3435	"MGCP 1.0"
#define MGCP_VERSION_NCS	"MGCP 1.0 NCS 1.0"

/* 3.2.1.1 */
enum mgcp_verb {
	MGCP_VERB_UNKNOWN = 0,
	MGCP_VERB_EPCF,
	MGCP_VERB_CRCX,
	MGCP_VERB_MDCX,
	MGCP_VERB_DLCX,
	MGCP_VERB_RQNT,
	MGCP_VERB_NTFY,
	MGCP_VERB_AUEP,
	MGCP_VERB_AUCX,
	MGCP_VERB_RSIP,
	MGCP_VERB_MAX,
};

/* 3.2.2, Appendix A ParameterValue and infoCode */
enum mgcp_param {
	MGCP_PARAM_UNKNOWN = 0,
	MGCP_PARAM_B,		/* BearerInformation */
	MGCP_PARAM_C,		/* CallId */
	MGCP_PARAM_A,		/* Capabilities */
	MGCP_PARAM_I,		/* ConnectionId */
	MGCP_PARAM_M,		/* ConnectionMode */
	MGCP_PARAM_P,		/* ConnectionParameters */
	MGCP_PARAM_T,		/* DetectEvents */
	MGCP_PARAM_D,		/* DigitMap */
	MGCP_PARAM_ES,		/* EventStates */
	MGCP_PARAM_L,		/* LocalConnectionOptions */
	MGCP_PARAM_MD,		/* MaxMGCPDatagram */
	MGCP_PARAM_N,		/* NotifiedEntity */
	MGCP_PARAM_O,		/* ObservedEvents */
	MGCP_PARAM_PL,		/* PackageList */
	MGCP_PARAM_Q,		/* QuarantineHandling */
	MGCP_PARAM_E,		/* ReasonCode */
	MGCP_PARAM_R,		/* RequestedEvents */
	MGCP_PARAM_F,		/* RequestedInfo */
	MGCP_PARAM_X,		/* RequestIdentifier */
	MGCP_PARAM_K,		/* ResponseAck */
	MGCP_PARAM_RD,		/* RestartDelay */
	MGCP_PARAM_RM,		/* RestartMethod */
	MGCP_PARAM_I2,		/* SecondConnectionId */
	MGCP_PARAM_Z2,		/* SecondEndpointId */
	MGCP_PARAM_S,		/* SignalRequests */
	MGCP_PARAM_Z,		/* SpecificEndpointId */
	MGCP_PARAM_RC,		/* RemoteConnectionDescriptor, F: only */
	MGCP_PARAM_LC,		/* LocalConnectionDescriptor, F: only */
	MGCP_PARAM_MAX,
};

/* 2.4: the class of a return code, by its range */
enum mgcp_rc_class {
	MGCP_RC_UNKNOWN = 0,
	MGCP_RC_ACK,		/* 000 to 099, response acknowledgment */
	MGCP_RC_PROVISIONAL,	/* 100 to 199 */
	MGCP_RC_SUCCESS,	/* 200 to 299 */
	MGCP_RC_TRANSIENT,	/* 400 to 499 */
	MGCP_RC_PERMANENT,	/* 500 to 599 */
	MGCP_RC_PACKAGE,	/* 800 to 899 */
};

enum mgcp_rc_class mgcp_rc_class_of(uint32_t code);

/* message level parse state; a set flag never fails the message */
#define MGCP_MSG_F_BARE_LF	(1u << 0)	/* a line ends with LF alone */
#define MGCP_MSG_F_LEAD_EOL	(1u << 1)	/* empty lines before line 1 */
#define MGCP_MSG_F_BAD_VERSION	(1u << 2)	/* not "MGCP <d>.<d>", 528 */
#define MGCP_MSG_F_PARAM_BAD	(1u << 3)	/* a line without a colon */
#define MGCP_MSG_F_NAME_LONG	(1u << 4)	/* an endpoint half over 255 */
#define MGCP_MSG_F_VERB_BAD	(1u << 5)	/* verb is not four letters */
#define MGCP_MSG_F_NO_FINAL_EOL	(1u << 6)	/* last line has no line end */

struct mgcp_param_line {
	struct sip_str name;		/* as received, case preserved */
	struct sip_str value;		/* trimmed */
	enum mgcp_param code;
};

struct mgcp_msg {
	const char *buf;
	size_t len;

	bool is_command;
	uint32_t txid;

	/* command (3.2.1) */
	enum mgcp_verb verb;
	struct sip_str verb_name;	/* case preserved */
	struct sip_str endpoint;
	struct sip_str version;		/* "MGCP 1.0" up to the line end */
	struct sip_str profile;		/* "NCS 1.0", empty without one */

	/* response (3.3) */
	uint32_t code;
	struct sip_str package;		/* the "/pkg" of an 8xx, no slash */
	struct sip_str comment;		/* may be empty */

	struct mgcp_param_line param[MGCP_PARAM_LINE_MAX];
	unsigned int param_count;

	/* the session descriptions behind the empty line (3.4), or empty */
	struct sip_str sdp;

	uint32_t flags;
};

enum mgcp_parse_status {
	MGCP_PARSE_OK = 0,
	MGCP_PARSE_ERR_EMPTY = -1,
	MGCP_PARSE_ERR_TOO_LARGE = -2,
	MGCP_PARSE_ERR_FIRST_LINE = -3,
	MGCP_PARSE_ERR_TXID = -4,
	MGCP_PARSE_ERR_PARAM_OVERFLOW = -5,
};

const char *mgcp_parse_status_name(enum mgcp_parse_status st);

enum mgcp_parse_status mgcp_msg_parse(struct mgcp_msg *msg, const char *buf,
				      size_t len);

/*
 * Split a datagram into its messages (3.5.5). Each call yields the next
 * message, its final line end included, and skips a part that holds only
 * line ends. false means the datagram holds no further message.
 */
struct mgcp_split {
	const char *cur;
	const char *end;
};

void mgcp_split_init(struct mgcp_split *it, const char *buf, size_t len);
bool mgcp_split_next(struct mgcp_split *it, struct sip_str *out);

const char *mgcp_verb_name(enum mgcp_verb verb);
enum mgcp_verb mgcp_verb_parse(struct sip_str s);
const char *mgcp_param_name(enum mgcp_param code);
enum mgcp_param mgcp_param_parse(struct sip_str s);

/*
 * True when the version of msg names token, compared case insensitively and
 * with any run of white space equal to one space (3.2.1, 3.2.1.4).
 */
bool mgcp_msg_version_is(const struct mgcp_msg *msg, const char *token);

/* ------------------------------------------------------- parameter index */

const struct mgcp_param_line *mgcp_msg_param(const struct mgcp_msg *msg,
					     enum mgcp_param code);
const struct mgcp_param_line *mgcp_msg_param_next(
	const struct mgcp_msg *msg, const struct mgcp_param_line *cur);
const struct mgcp_param_line *mgcp_msg_param_named(const struct mgcp_msg *msg,
						   const char *name);
bool mgcp_msg_value(const struct mgcp_msg *msg, enum mgcp_param code,
		    struct sip_str *out);

/* -------------------------------------------------------------- lexers */

/*
 * One item of a comma separated list. The scan treats a quoted string
 * (Appendix A quotedString, "" as an escaped quote) and a parenthesized
 * group as opaque, so a comma inside L/ci(time, "a, b") never splits the
 * item. Empty items are skipped and every item is trimmed.
 */
bool mgcp_list_next(struct sip_str *rest, struct sip_str *item);

/*
 * One event or signal parameter (Appendix A eventParameter): name=value,
 * name(values) or a bare value. A bare value leaves name empty; the
 * parenthesized form returns the text inside the parentheses as value.
 */
bool mgcp_event_param_next(struct sip_str *rest, struct sip_str *name,
			   struct sip_str *value);

/* Strip the outer double quotes; "" escapes stay in the span. */
struct sip_str mgcp_quoted_strip(struct sip_str s);

/*
 * Copy a quoted string without its quotes and with each "" reduced to one
 * quote, NUL terminated. -1 when it does not fit olen, else the length.
 */
int mgcp_unquote(struct sip_str in, char *out, size_t olen);

/* 1*9 DIGIT inside 1 to MGCP_TXID_MAX (3.2.1.2) */
bool mgcp_txid_parse(struct sip_str s, uint32_t *out);

/* ---------------------------------------------------- events, signals */

/*
 * An event or signal name with its parameters. pkg is the default package
 * when the name carries none, and pkg_default says so. conn is the part
 * after "@", or empty. params is the text inside the parentheses that
 * follow the name, or empty.
 */
struct mgcp_event {
	struct sip_str raw;
	struct sip_str pkg;
	struct sip_str name;
	struct sip_str conn;
	struct sip_str params;
	bool pkg_default;
	bool has_params;
};

bool mgcp_event_parse(struct sip_str s, struct sip_str default_pkg,
		      struct mgcp_event *ev);

/*
 * The signal list of S: (3.2.2.21), and with the same grammar the lists of
 * O:, T: and ES: (Appendix A ObservedEvents, DetectEvents, EventStates).
 * mgcp_sig_first() reads the first item of list; false means the list is
 * empty or an item does not parse, and bad tells the two apart.
 */
struct mgcp_sig_iter {
	struct sip_str rest;
	struct sip_str default_pkg;
	bool bad;
};

bool mgcp_sig_first(struct sip_str list, struct sip_str default_pkg,
		    struct mgcp_sig_iter *it, struct mgcp_event *ev);
bool mgcp_sig_next(struct mgcp_sig_iter *it, struct mgcp_event *ev);

/* 3.2.2.16 actions */
#define MGCP_ACT_N		(1u << 0)	/* notify immediately */
#define MGCP_ACT_A		(1u << 1)	/* accumulate */
#define MGCP_ACT_D		(1u << 2)	/* treat by the digit map */
#define MGCP_ACT_S		(1u << 3)	/* swap */
#define MGCP_ACT_I		(1u << 4)	/* ignore */
#define MGCP_ACT_K		(1u << 5)	/* keep signals active */
#define MGCP_ACT_E		(1u << 6)	/* embedded request */
#define MGCP_ACT_EXT		(1u << 7)	/* an extension action */

/*
 * One requested event (Appendix A requestedEvent). An event without an
 * action list carries MGCP_ACT_N, the default of 3.2.2.16. embedded is the
 * text inside E(...); ext is the first extension action as received. The
 * event parameters of X/foobar(N)(epar=2) go to ev.params.
 */
struct mgcp_req_event {
	struct mgcp_event ev;
	struct sip_str actions;
	struct sip_str embedded;
	struct sip_str ext;
	uint32_t act;
};

struct mgcp_req_iter {
	struct sip_str rest;
	struct sip_str default_pkg;
	bool bad;
};

bool mgcp_req_first(struct sip_str list, struct sip_str default_pkg,
		    struct mgcp_req_iter *it, struct mgcp_req_event *re);
bool mgcp_req_next(struct mgcp_req_iter *it, struct mgcp_req_event *re);

/*
 * The groups of an embedded request (3.2.2.16, Appendix A EmbeddedRequest).
 * The groups are accepted in any order, as 3.2.2.16 asks of a receiver. A
 * repeated group, an unknown group letter or an empty embedded request is
 * refused.
 */
struct mgcp_embedded {
	struct sip_str r;
	struct sip_str s;
	struct sip_str d;
	bool has_r;
	bool has_s;
	bool has_d;
};

bool mgcp_embedded_parse(struct sip_str e, struct mgcp_embedded *out);

/* ------------------------------------------------- typed parameter values */

/* N: (3.2.1.3, Appendix A NotifiedEntity); port 0 means none given */
struct mgcp_entity {
	struct sip_str local;
	struct sip_str domain;		/* brackets kept for an address */
	uint32_t port;
};

bool mgcp_entity_parse(struct sip_str s, struct mgcp_entity *e);

/* 1*32 HEXDIG: CallId, ConnectionId, RequestIdentifier (3.2.2.2, .5, .18) */
bool mgcp_hex_id_valid(struct sip_str s);

/* C:, X:, the first id of I:; false when absent or not 1*32 HEXDIG */
bool mgcp_msg_call_id(const struct mgcp_msg *msg, struct sip_str *out);
bool mgcp_msg_request_id(const struct mgcp_msg *msg, struct sip_str *out);
bool mgcp_msg_conn_id(const struct mgcp_msg *msg, struct sip_str *out);

/*
 * L: and A: (3.2.2.10, 3.2.2.3). Each known attribute is a span of its value;
 * an empty span means the attribute is absent. ext_mandatory is the first
 * "x+" attribute, which a gateway refuses with 525 when it does not know it.
 * v and m are the Capabilities attributes.
 */
struct mgcp_lco {
	struct sip_str p;
	struct sip_str a;
	struct sip_str b;
	struct sip_str e;
	struct sip_str gc;
	struct sip_str s;
	struct sip_str t;
	struct sip_str r;
	struct sip_str k;
	struct sip_str nt;
	struct sip_str v;
	struct sip_str m;
	struct sip_str ext_mandatory;
	unsigned int unknown;
};

bool mgcp_lco_parse(struct sip_str s, struct mgcp_lco *lco);
bool mgcp_msg_lco(const struct mgcp_msg *msg, struct mgcp_lco *lco);

/* one codec of the a: list, separated by ';' (3.2.2.10) */
bool mgcp_codec_next(struct sip_str *rest, struct sip_str *codec);

/* M: (3.2.2.6) */
enum mgcp_mode {
	MGCP_MODE_UNKNOWN = 0,
	MGCP_MODE_SENDONLY,
	MGCP_MODE_RECVONLY,
	MGCP_MODE_SENDRECV,
	MGCP_MODE_CONFRNCE,
	MGCP_MODE_INACTIVE,
	MGCP_MODE_LOOPBACK,
	MGCP_MODE_CONTTEST,
	MGCP_MODE_NETWLOOP,
	MGCP_MODE_NETWTEST,
	MGCP_MODE_EXTENSION,		/* package/mode */
};

enum mgcp_mode mgcp_mode_parse(struct sip_str s);
const char *mgcp_mode_name(enum mgcp_mode mode);
bool mgcp_msg_mode(const struct mgcp_msg *msg, enum mgcp_mode *mode);

/* D: the digit map as received, trimmed (3.2.2, Appendix A DigitMap) */
bool mgcp_msg_digit_map(const struct mgcp_msg *msg, struct sip_str *out);

/* Q: (3.2.2.14). false on a keyword outside the four, the 508 case. */
struct mgcp_quarantine {
	bool loop;			/* else step, the default */
	bool discard;			/* else process, the default */
};

bool mgcp_quarantine_parse(struct sip_str s, struct mgcp_quarantine *q);

/*
 * K: (3.2.2.19). mgcp_ack_next() yields one confirmed range. It stops with
 * bad set on an item that is not txid or txid-txid with lo <= hi.
 */
struct mgcp_ack_iter {
	struct sip_str rest;
	bool bad;
};

void mgcp_ack_init(struct mgcp_ack_iter *it, struct sip_str k);
bool mgcp_ack_next(struct mgcp_ack_iter *it, uint32_t *lo, uint32_t *hi);
bool mgcp_ack_covers(struct sip_str k, uint32_t txid);

/* F: (3.2.2.17): one info code; name keeps an unknown code */
bool mgcp_info_next(struct sip_str *rest, enum mgcp_param *code,
		    struct sip_str *name);

/* B: (3.2.2.1); e is "A" or "mu" when present */
struct mgcp_bearer {
	struct sip_str e;
};

bool mgcp_msg_bearer(const struct mgcp_msg *msg, struct mgcp_bearer *b);

/* RM: (3.2.2.20) */
enum mgcp_rm {
	MGCP_RM_UNKNOWN = 0,
	MGCP_RM_GRACEFUL,
	MGCP_RM_FORCED,
	MGCP_RM_RESTART,
	MGCP_RM_DISCONNECTED,
	MGCP_RM_CANCEL_GRACEFUL,
	MGCP_RM_EXTENSION,		/* package/method */
};

enum mgcp_rm mgcp_rm_parse(struct sip_str s);
const char *mgcp_rm_name(enum mgcp_rm rm);
bool mgcp_msg_rm(const struct mgcp_msg *msg, enum mgcp_rm *rm);

/* RD: 1*6 DIGIT (Appendix A RestartDelay); MD: 1*9 DIGIT (3.2.2.11) */
bool mgcp_msg_rd(const struct mgcp_msg *msg, uint32_t *secs);
bool mgcp_msg_md(const struct mgcp_msg *msg, uint32_t *bytes);

/* E: (3.2.2.15, Appendix A ReasonCode) */
struct mgcp_reason {
	uint32_t code;
	struct sip_str package;
	struct sip_str comment;
};

bool mgcp_reason_parse(struct sip_str s, struct mgcp_reason *r);
bool mgcp_msg_reason(const struct mgcp_msg *msg, struct mgcp_reason *r);

/*
 * P: (3.2.2.7). present holds one bit per field in the order below. A value
 * holds nine digits at most; an unknown type is ignored, as 3.2.2.7 asks.
 */
#define MGCP_CP_PS		(1u << 0)
#define MGCP_CP_OS		(1u << 1)
#define MGCP_CP_PR		(1u << 2)
#define MGCP_CP_OR		(1u << 3)
#define MGCP_CP_PL		(1u << 4)
#define MGCP_CP_JI		(1u << 5)
#define MGCP_CP_LA		(1u << 6)

struct mgcp_conn_params {
	uint32_t ps;
	uint32_t os;
	uint32_t pr;
	uint32_t or_;
	uint32_t pl;
	uint32_t ji;
	uint32_t la;
	uint32_t present;
};

bool mgcp_conn_params_parse(struct sip_str s, struct mgcp_conn_params *cp);
bool mgcp_msg_conn_params(const struct mgcp_msg *msg,
			  struct mgcp_conn_params *cp);

/*
 * The session descriptions of a response, one at a time: a body can carry
 * a local and a remote description separated by an empty line (3.3).
 */
bool mgcp_sdp_next(struct sip_str *rest, struct sip_str *one);

/* ------------------------------------------------------------- builders */

/* parameter lines a response or a command carries beyond its own fields */
struct mgcp_param_kv {
	enum mgcp_param code;
	const char *name;		/* for MGCP_PARAM_UNKNOWN, else NULL */
	const char *value;		/* may be empty, never NULL */
};

/*
 * The K: list a command carries (3.2.2.19). The writer joins consecutive
 * ascending ids into one range and writes nothing for an empty list.
 */
#define MGCP_ACK_LIST_MAX	32

struct mgcp_ack_list {
	uint32_t txid[MGCP_ACK_LIST_MAX];
	unsigned int count;
};

bool mgcp_ack_list_write(struct sip_buf *b, const struct mgcp_ack_list *acks);

/* NTFY (2.3.5, 3.2.2): X: and O: are mandatory */
struct mgcp_ntfy {
	uint32_t txid;
	const char *endpoint;
	const char *version;
	const struct mgcp_ack_list *acks;
	const char *notified_entity;	/* NULL: no N: line */
	const char *request_id;		/* "0" for persistent events */
	const char *observed;
};

bool mgcp_ntfy_build(struct sip_buf *b, const struct mgcp_ntfy *n);

/*
 * RSIP (2.3.12). RM: is mandatory. RD: is written only when has_delay, and
 * is refused with "forced" and "cancel-graceful", which forbid it.
 */
struct mgcp_rsip {
	uint32_t txid;
	const char *endpoint;
	const char *version;
	const struct mgcp_ack_list *acks;
	const char *method;
	uint32_t delay;
	bool has_delay;
	uint32_t reason;		/* 0: no E: line */
	const char *reason_text;
};

bool mgcp_rsip_build(struct sip_buf *b, const struct mgcp_rsip *r);

/* DLCX from the gateway (2.3.8, F.6): C: and I: are mandatory */
struct mgcp_dlcx {
	uint32_t txid;
	const char *endpoint;
	const char *version;
	const struct mgcp_ack_list *acks;
	const char *call_id;
	const char *conn_id;
	uint32_t reason;		/* 0: no E: line */
	const char *reason_text;
	const struct mgcp_conn_params *stats;	/* NULL: no P: line */
};

bool mgcp_dlcx_build(struct sip_buf *b, const struct mgcp_dlcx *d);

/* P: value of the fields that present names, in the order of 3.2.2.7 */
bool mgcp_conn_params_write(struct sip_buf *b,
			    const struct mgcp_conn_params *cp);

/*
 * A response (3.3). code 0 is the response acknowledgment "000 <txid>".
 * ack_request writes the empty K: line of a final that follows a
 * provisional (3.5.6). sdp, when not NULL, follows an empty line and
 * passes through unchanged. A params list that names one parameter twice,
 * or that names K while ack_request is set, is refused, because a response
 * parameter appears once (3.3).
 */
struct mgcp_resp {
	uint32_t code;
	uint32_t txid;
	const char *comment;		/* NULL: none */
	bool ack_request;
	const struct mgcp_param_kv *params;
	unsigned int param_count;
	const char *sdp;
	size_t sdp_len;
};

bool mgcp_resp_build(struct sip_buf *b, const struct mgcp_resp *r);

/* the piggyback separator line (3.5.5) */
bool mgcp_sep_write(struct sip_buf *b);

#endif
