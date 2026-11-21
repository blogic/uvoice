// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The client to server stream of one XMPP account (src/proto/xmpp/stream.c),
 * RFC 6120 section 4 to section 9.
 *
 * What it does. It resolves the server, connects, secures the connection
 * with TLS, authenticates with SASL, binds the resource, enables stream
 * management, enables message carbons and then carries stanzas both ways
 * until the connection breaks. When it breaks, the stream reconnects on its
 * own after a backoff that doubles up to XMPP_BACKOFF_MAX_MS. The owner
 * starts it once and stops it once.
 *
 * TLS is required. A server that offers no STARTTLS on a plain connection
 * ends the attempt (RFC 6120 section 5.3.1 and section 13.8). The direct
 * TLS of XEP-0368 is the alternative: the stream then negotiates TLS on the
 * socket before the first byte of XML, with the ALPN name "xmpp-client".
 * The peer certificate is checked against the domain of the JID when the
 * configuration asks for verification (RFC 6120 section 13.7.2).
 *
 * Resolution. With no host configured the stream asks the SRV records of
 * the domain, _xmpp-client._tcp for STARTTLS or _xmpps-client._tcp for
 * direct TLS, through the shared resolver, which falls back to the address
 * of the domain with the default port when the zone holds none
 * (RFC 6120 section 3.2.1). A configured host is resolved as an address.
 * The targets are tried in the order the resolver delivered them.
 *
 * What the stream answers itself. An iq ping (XEP-0199), a disco#info query
 * (XEP-0030), the <r/> of stream management (XEP-0198) and an iq of type get
 * or set that the owner did not handle, which RFC 6120 section 8.2.3 says
 * must get an error. Every other stanza reaches the owner through the stanza
 * callback. An iq of type result or error reaches the callback the owner
 * registered with xmpp_stream_iq_send(), or nobody when no id matches.
 *
 * Liveness. Every XMPP_PING_S seconds of silence the stream sends an <r/>
 * when stream management is on, or an iq ping otherwise. Any element that
 * arrives counts as the answer. XMPP_PONG_MS without one ends the
 * connection, which starts the reconnect.
 *
 * Resumption of stream management is not used. The phone sits on a wired
 * network where a connection that drops is a rare event, so a new session
 * with a fresh presence costs nothing worth a replay queue.
 *
 * One stanza at a time. The transmit side stages into XMPP_TX_MAX bytes
 * without allocation. A send that does not fit is refused with -ENOSPC, so
 * the owner treats it as a failed send. Nothing on this path allocates
 * except the expat parser, which xml_stream_init() makes once.
 *
 * The callbacks run from inside uloop. The owner may call every function of
 * this header from inside them, xmpp_stream_stop() included: the stream
 * notices that its connection went and stops touching it.
 */
#ifndef UVOICE_XMPP_STREAM_H
#define UVOICE_XMPP_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libubox/uloop.h>

#include "proto/proto.h"
#include "proto/shared/resolve.h"
#include "proto/shared/tls.h"
#include "proto/xmpp/jid.h"
#include "proto/xmpp/sasl.h"
#include "proto/xmpp/xml.h"

#define XMPP_PORT		5222
#define XMPP_PORT_TLS		5223
#define XMPP_SRV_STARTTLS	"_xmpp-client._tcp"
#define XMPP_SRV_TLS		"_xmpps-client._tcp"
#define XMPP_ALPN		"xmpp-client"
#define XMPP_RESOURCE_DEFAULT	"uvoice"

#define XMPP_NS_TLS		"urn:ietf:params:xml:ns:xmpp-tls"
#define XMPP_NS_SASL		"urn:ietf:params:xml:ns:xmpp-sasl"
#define XMPP_NS_BIND		"urn:ietf:params:xml:ns:xmpp-bind"
#define XMPP_NS_SESSION		"urn:ietf:params:xml:ns:xmpp-session"
#define XMPP_NS_STANZAS		"urn:ietf:params:xml:ns:xmpp-stanzas"
#define XMPP_NS_STREAMS		"urn:ietf:params:xml:ns:xmpp-streams"
#define XMPP_NS_SM		"urn:xmpp:sm:3"
#define XMPP_NS_PING		"urn:xmpp:ping"
#define XMPP_NS_CARBONS		"urn:xmpp:carbons:2"

/* The transmit staging buffer and the read chunk. */
#define XMPP_TX_MAX		16384U
#define XMPP_RX_CHUNK		4096U
/* Read passes per readable event, so one peer cannot own the loop. */
#define XMPP_RX_BURST		8U
/* The sweep period of the deadlines. */
#define XMPP_TICK_MS		1000U
/* The bound of a connect, a handshake and each negotiation step. */
#define XMPP_STEP_MS		20000U
#define XMPP_PING_S		60U
#define XMPP_PONG_MS		15000U
/* Pending iq requests of the owner and the stream together. */
#define XMPP_IQ_MAX		8U
#define XMPP_IQ_MS		30000U
#define XMPP_BACKOFF_MS		2000U
#define XMPP_BACKOFF_MAX_MS	120000U
/* The rung of the backoff ladder a refused password jumps to. */
#define XMPP_AUTH_FAILS		5U
#define XMPP_ID_MAX		64U
#define XMPP_REASON_MAX		96U
#define XMPP_STREAM_ID_MAX	64U

/*
 * What a parser callback asked the read loop to do once the parser returned:
 * start the TLS handshake after <proceed/>, or a new XML document after the
 * SASL success. Neither may run inside the callback, because the document
 * restart resets the parser the callback runs in.
 */
enum xmpp_pending {
	XMPP_PENDING_NONE = 0,
	XMPP_PENDING_TLS,
	XMPP_PENDING_RESTART,
};

enum xmpp_stream_state {
	XMPP_ST_IDLE = 0,	/* not started, or stopped */
	XMPP_ST_RESOLVE,
	XMPP_ST_CONNECT,
	XMPP_ST_TLS,		/* the handshake runs */
	XMPP_ST_FEATURES,	/* the header went, the features are awaited */
	XMPP_ST_STARTTLS,	/* <starttls/> went, <proceed/> is awaited */
	XMPP_ST_SASL,
	XMPP_ST_BIND,
	XMPP_ST_SESSION,
	XMPP_ST_SM,
	XMPP_ST_CARBONS,
	XMPP_ST_READY,
	XMPP_ST_BACKOFF,	/* the reconnect waits */
	XMPP_ST_MAX,
};

const char *xmpp_stream_state_name(enum xmpp_stream_state state);

struct xmpp_stream;

/**
 * typedef xmpp_iq_cb - the answer to an iq the owner sent
 * @xs: the stream
 * @reply: the iq of type result or error, NULL when XMPP_IQ_MS passed or
 *         the connection went
 * @priv: the pointer given to xmpp_stream_iq_send()
 */
typedef void (*xmpp_iq_cb)(struct xmpp_stream *xs, const struct xml_doc *reply,
			   void *priv);

/*
 * state   every transition, with a static reason text for the ones that
 *         end a connection, "" otherwise.
 * stanza  a message, a presence or an iq of type get or set. The tree is
 *         valid for the call only. The return says whether the owner
 *         handled an iq, so the stream answers service-unavailable for one
 *         it did not. The return of a message or a presence is ignored.
 */
struct xmpp_stream_cb {
	void (*state)(struct xmpp_stream *xs, enum xmpp_stream_state state,
		      const char *reason);
	bool (*stanza)(struct xmpp_stream *xs, const struct xml_doc *doc);
};

/*
 * host       the trace sink and the log, NULL for neither.
 * dns        the shared resolver. Its done callback hands the answers of
 *            queries that carry dns_owner to xmpp_stream_dns_done().
 * tls        the client context the owner loaded with the trust store and
 *            the verify flag of the account.
 */
struct xmpp_stream_env {
	const struct proto_host *host;
	struct sip_dns *dns;
	uint32_t dns_owner;
	struct sip_tls *tls;
};

/*
 * jid          the account, bare or with the resource to bind. A bare JID
 *              binds XMPP_RESOURCE_DEFAULT.
 * host, port   the server. "" resolves the SRV records of the domain. Port
 *              0 takes XMPP_PORT, or XMPP_PORT_TLS with direct_tls.
 * direct_tls   XEP-0368 instead of STARTTLS.
 * tls_verify   check the peer certificate against the domain.
 * allow_plain  take PLAIN when the server offers no SCRAM.
 * ping_s       the liveness period, 0 takes XMPP_PING_S.
 * random       the entropy source of the nonces and the ids, NULL takes
 *              sip_os_random().
 */
struct xmpp_stream_cfg {
	char jid[JID_MAX];
	char password[SASL_PASS_MAX];
	char host[SIP_DNS_NAME_MAX];
	uint16_t port;
	bool direct_tls;
	bool tls_verify;
	bool allow_plain;
	unsigned int ping_s;
	void (*random)(void *buf, size_t len);
};

struct xmpp_iq_pending {
	char id[XMPP_ID_MAX];
	xmpp_iq_cb cb;
	void *priv;
	uint64_t deadline_ms;
	bool in_use;
};

struct xmpp_sm {
	bool offered;
	bool enabled;
	uint32_t in;			/* stanzas received, the h we report */
	uint32_t out;			/* stanzas sent */
	uint32_t acked;			/* the last h the server reported */
};

struct xmpp_stream_stat {
	uint64_t connects;
	uint64_t failures;
	uint64_t rx_bytes;
	uint64_t tx_bytes;
	uint64_t rx_stanzas;
	uint64_t tx_stanzas;
	uint64_t tx_refused;		/* the staging buffer was full */
	uint64_t iq_unmatched;		/* results that matched no request */
	uint64_t iq_timeouts;
	uint64_t pings;
};

struct xmpp_stream {
	struct xmpp_stream_cfg cfg;
	struct xmpp_stream_cb cb;
	struct xmpp_stream_env env;
	void *priv;

	struct uloop_fd ufd;
	struct uloop_timeout tick;
	struct sip_tls_conn tls;
	struct xml_stream parser;
	struct sasl_client sasl;

	struct jid jid;			/* the configured identity */
	struct jid bound;		/* what the server bound */
	char jid_text[JID_MAX];		/* the bare JID, for the trace */
	char stream_id[XMPP_STREAM_ID_MAX];
	char reason[XMPP_REASON_MAX];

	struct sip_dns_target target[SIP_DNS_TARGET_MAX];
	unsigned int targets;
	unsigned int target_next;
	uint32_t dns_query;

	enum xmpp_stream_state state;
	bool parser_ready;
	bool tls_up;
	bool authenticated;
	bool session_needed;
	enum xmpp_pending pending;
	bool carbons;			/* the server enabled XEP-0280 */
	bool stopping;
	struct xmpp_sm sm;

	uint64_t deadline_ms;
	uint64_t ping_due_ms;
	uint64_t backoff_until;
	bool ping_waiting;
	unsigned int fails;
	uint32_t id_seq;
	unsigned int gen;		/* grows on every connection close */

	struct xmpp_iq_pending iq[XMPP_IQ_MAX];

	size_t tx_len;
	size_t tx_off;
	char tx[XMPP_TX_MAX];

	struct xmpp_stream_stat stat;
};

/**
 * xmpp_stream_init() - prepare a stream
 * @xs: the stream, cleared here
 * @cb: the callbacks, copied
 * @priv: the owner's pointer
 * @env: the host, the resolver and the TLS contexts, copied
 */
void xmpp_stream_init(struct xmpp_stream *xs, const struct xmpp_stream_cb *cb,
		      void *priv, const struct xmpp_stream_env *env);

/**
 * xmpp_stream_start() - connect, then stay connected
 * @xs: the stream
 * @cfg: the account, copied
 *
 * Return: 0, -EINVAL for a JID or a password the stream refuses, -ENOMEM
 * when the parser could not be made, -EBUSY when the stream runs already.
 */
int xmpp_stream_start(struct xmpp_stream *xs,
		      const struct xmpp_stream_cfg *cfg);

/**
 * xmpp_stream_stop() - close the stream in order and reconnect no more
 * @xs: the stream
 *
 * Every pending iq callback runs with a NULL reply.
 */
void xmpp_stream_stop(struct xmpp_stream *xs);

/**
 * xmpp_stream_done() - stop the stream and release the parser
 * @xs: the stream
 */
void xmpp_stream_done(struct xmpp_stream *xs);

/**
 * xmpp_stream_dns_done() - hand a resolver answer to the stream
 * @xs: the stream
 * @query: the query handle the resolver reports
 * @res: the answer
 */
void xmpp_stream_dns_done(struct xmpp_stream *xs, uint32_t query,
			  const struct sip_dns_result *res);

/**
 * xmpp_stream_state() - the state
 * @xs: the stream
 *
 * Return: the state.
 */
enum xmpp_stream_state xmpp_stream_state(const struct xmpp_stream *xs);

/**
 * xmpp_stream_ready() - whether stanzas flow
 * @xs: the stream
 *
 * Return: true in XMPP_ST_READY.
 */
bool xmpp_stream_ready(const struct xmpp_stream *xs);

/**
 * xmpp_stream_jid() - the full JID the server bound
 * @xs: the stream
 *
 * Return: the JID, with an empty resource before the bind.
 */
const struct jid *xmpp_stream_jid(const struct xmpp_stream *xs);

/**
 * xmpp_stream_reason() - why the last connection ended
 * @xs: the stream
 *
 * Return: the text, "" when none ended.
 */
const char *xmpp_stream_reason(const struct xmpp_stream *xs);

/**
 * xmpp_stream_id_next() - a fresh stanza id
 * @xs: the stream
 * @out: receives the id
 * @len: capacity of @out, XMPP_ID_MAX suffices
 */
void xmpp_stream_id_next(struct xmpp_stream *xs, char *out, size_t len);

/**
 * xmpp_stream_send() - send one stanza
 * @xs: the stream
 * @text: the serialised stanza
 * @len: its length
 *
 * Return: 0, -ENOTCONN outside XMPP_ST_READY, -ENOSPC when it does not fit.
 */
int xmpp_stream_send(struct xmpp_stream *xs, const char *text, size_t len);

/**
 * xmpp_stream_iq_send() - send an iq of type get or set and await its answer
 * @xs: the stream
 * @text: the serialised iq
 * @len: its length
 * @id: the id the iq carries
 * @cb: runs once with the answer, or with NULL on a timeout or a close
 * @priv: handed to @cb
 *
 * Return: 0, -ENOTCONN, -ENOSPC, or -EBUSY when XMPP_IQ_MAX requests wait.
 */
int xmpp_stream_iq_send(struct xmpp_stream *xs, const char *text, size_t len,
			const char *id, xmpp_iq_cb cb, void *priv);

/**
 * xmpp_stream_iq_result() - answer an iq with an empty result
 * @xs: the stream
 * @iq: the iq of type get or set
 *
 * Return: 0, or the xmpp_stream_send() error.
 */
int xmpp_stream_iq_result(struct xmpp_stream *xs, const struct xml_doc *iq);

/**
 * xmpp_stream_iq_error() - answer an iq with a stanza error
 * @xs: the stream
 * @iq: the iq of type get or set
 * @type: the error type, "cancel", "modify", "wait" or "auth"
 * @condition: the condition element name of RFC 6120 section 8.3.3
 *
 * Return: 0, or the xmpp_stream_send() error.
 */
int xmpp_stream_iq_error(struct xmpp_stream *xs, const struct xml_doc *iq,
			 const char *type, const char *condition);

/**
 * xmpp_stream_stat() - the counters
 * @xs: the stream
 *
 * Return: the counters.
 */
const struct xmpp_stream_stat *xmpp_stream_stat(const struct xmpp_stream *xs);

#endif
