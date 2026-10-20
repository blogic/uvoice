// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Per line SIP account. See account.h for the contracts.
 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubox/ulog.h>
#include <libubox/utils.h>
#include <libubus.h>

#include "hal/hal.h"
#include "posix.h"
#include "proto/proto.h"
#include "proto/proto_trace.h"
#include "proto/shared/hdr.h"
#include "proto/sip/account.h"
#include "proto/sip/call.h"
#include "proto/sip/dialog.h"
#include "proto/sip/keepalive.h"
#include "proto/sip/line.h"
#include "proto/sip/reg.h"
#include "proto/shared/resolve.h"
#include "proto/sip/stream.h"
#include "proto/sip/sub.h"
#include "proto/sip/tls.h"
#include "proto/sip/transport.h"
#include "proto/sip/ua.h"
#include "str.h"

/* One line in the sip trace domain, through the host table. */
#define ACCT_TRACE(fmt, ...)	PROTO_TRACE(vr.host, TRACE_SIP, fmt, \
					    ##__VA_ARGS__)

#define ACCT_USER_AGENT		"SmartOS uvoice/" UVOICE_VERSION

/* Retry a resolution that could not start or failed, and the window in which a
 * successful answer is re-resolved to catch a moved record. The retry is the
 * first rung of a ladder that doubles to DNS_RETRY_MAX_MS, so a name that stays
 * dead costs one query a minute; the warning repeats at each tenth attempt once
 * the cap holds the delay flat. */
#define DNS_RETRY_MS		5000
#define DNS_RETRY_MAX_MS	60000
#define DNS_RETRY_LOG_EVERY	10
#define DNS_REFRESH_MIN_S	30
#define DNS_REFRESH_MAX_S	3600

/* One failure-driven re-resolve per line inside this window: the same floor
 * the TTL clamp allows, so a dead target cannot raise the query cadence. */
#define DNS_FAIL_RESOLVE_MIN_S	30

/* Bounded window a reload waits for a line's Expires 0 before it discards the
 * registration and rebuilds, the same 3 s the SIGTERM drain uses. */
#define GRACE_MS		3000

/*
 * The directory SIP_TLS_KEYLOG_DEFAULT names. Nothing else creates it, and the
 * key log open of tls.c does not create a path: without this directory the
 * default key log path writes nothing, so the account layer makes the
 * directory before it hands the path to the load.
 */
#define ACCT_KEYLOG_DIR		"/tmp/uvoice"

/*
 * The transaction environment of one line. Both members start with the
 * struct sip_txn_env the transaction layer holds, so the env pointer of an
 * upcall names this union whatever transport built it, and cfg.transport says
 * which member is the live one.
 */
union acct_env {
	struct sip_udp_env ue;
	struct sip_stream_env se;
};

struct account {
	bool active;
	unsigned int line;
	struct acct_cfg cfg;

	/*
	 * AF_INET or AF_INET6: the family of the line's socket or listener, its
	 * local host, its Via, its Contact and its media. It follows the family
	 * of the registrar target the line uses (account_family_sync).
	 */
	int family;

	/* UDP: the datagram socket. A stream line leaves it SIP_HANDLE_NONE
	 * and holds a listener instead (sip/stream.h "Listeners"). */
	uint32_t sock;
	uint32_t listener;

	union acct_env env;
	struct sip_txn_mgr mgr;
	struct sip_reg reg;
	bool reg_inited;

	struct sip_ka ka;
	bool ka_inited;

	/* the conntrack udp bound the keepalive interval was derived under; a
	 * mode 2 udp line re-reads it on every round (acct_pinhole_bound_sync) */
	unsigned int ct_bound_s;

	/* the message waiting subscription; the NOTIFY intake needs no client */
	struct sip_sub sub;
	bool sub_inited;

	/* the call side of the line: one UA core and one dialog set */
	struct ua ua;
	struct sip_dialog_set dialogs;
	bool ua_inited;

	char local_host[SIP_SOCK_HOST_MAX];

	uint32_t dns_query;
	struct uloop_timeout resolve_timer;

	/* The failure ladder of the resolution: the attempts since the last
	 * answer, and the delay the last of them asked for. Both stay 0 while
	 * the timer above carries the TTL refresh or the fixed wait for a local
	 * address, neither of which is a rung. */
	unsigned int dns_fails;
	unsigned int dns_retry_ms;

	/* the last failure-driven re-resolve, on the env clock; bounds them to
	 * one per DNS_FAIL_RESOLVE_MIN_S */
	uint64_t fail_resolve_ms;

	/* the dns_linger retention (account.h) and its expiry deadline */
	struct acct_linger linger;
	struct uloop_timeout linger_timer;

	/* A reload that changes or removes a line unregisters the old binding and
	 * completes the teardown from the grace timer, on the unregistered
	 * callback or the timeout, whichever comes first. */
	struct uloop_timeout grace_timer;
	struct acct_cfg pending_cfg;
	bool pending;
	bool pending_remove;

	/*
	 * Reconnect re-registers (sip/stream.h "Reconnect re-registers").
	 * reg_conn is the connection the registration destination last had up
	 * and SIP_HANDLE_NONE once it closed; reg_conn_seen says one ever came
	 * up in the lifetime of this registration, so an up after a close is
	 * told apart from the first up of all.
	 */
	uint32_t reg_conn;
	bool reg_conn_seen;

	/* whether SRV records built the targets the line holds now; the status
	 * surface reports it beside the question the line asked */
	bool dns_answer_srv;

	uint32_t last_code;
	uint64_t refresh_at_ms;
	bool unreg_pending;		/* SIGTERM drain accounting only */
};

static struct {
	struct sip_transport tp;

	/*
	 * One stream pool and one pair of TLS contexts for every line. The
	 * connection key carries the local identity (sip/stream.h), so a
	 * shared pool never hands one line's connection to another.
	 */
	struct sip_stream st;
	struct sip_tls tls;

	/* the two tls_* values the contexts are built from, and whether the
	 * running configuration asks for TLS at all, kept so
	 * accounts_tls_reload() decides and rebuilds without a struct cfg */
	bool tls_verify;
	bool tls_keylog;
	bool tls_wanted;

	struct sip_dns dns;
	struct account acct[CFG_CHANNEL_MAX];

	/* the core services of the daemon (accounts_host_bind) */
	const struct proto_host *host;

	/* the ipv6_enabled operator gate, as the last start or reload saw it */
	bool ipv6;

	/* the backend feature mask (accounts_hal_features_set) */
	uint32_t hal_features;

	bool inited;

	/* accounts_start() ran. The daemon connects to ubus before it starts
	 * the accounts, so a command or a tracker report can arrive first. */
	bool started;
	bool shutting_down;
} vr;

/* ------------------------------------------------------------- pure helpers */

/* the uci vocabulary of enum acct_transport, for the status keys and the logs */
static const char *acct_transport_name(enum acct_transport tp)
{
	switch (tp) {
	case ACCT_TP_UDP:
		return "udp";
	case ACCT_TP_TCP:
		return "tcp";
	case ACCT_TP_TLS:
		return "tls";
	}

	return "udp";
}

/* the Via and Contact token of one transport; account.h carries the contract */
const char *acct_transport_token(enum acct_transport tp)
{
	switch (tp) {
	case ACCT_TP_TCP:
		return "TCP";
	case ACCT_TP_TLS:
		return "TLS";
	case ACCT_TP_UDP:
		break;
	}

	return NULL;
}

static bool acct_transport_map(const char *token, enum acct_transport *out)
{
	if (!token || !token[0] || !strcmp(token, "udp"))
		*out = ACCT_TP_UDP;
	else if (!strcmp(token, "tcp"))
		*out = ACCT_TP_TCP;
	else if (!strcmp(token, "tls"))
		*out = ACCT_TP_TLS;
	else
		return false;

	return true;
}

/* a stream line: TCP or TLS, the two transports sip/stream.h serves */
static bool acct_stream(const struct acct_cfg *c)
{
	return c->transport != ACCT_TP_UDP;
}

/* the server_name of one line, or NULL; account.h carries the contract */
const char *acct_sni_of(const struct acct_cfg *c)
{
	struct sip_txn_dst literal;

	if (c->transport != ACCT_TP_TLS || !c->resolve_host[0])
		return NULL;

	/* an address target sends none: sip_dst_set() accepts exactly the
	 * literals RFC 6066 3 forbids in the extension */
	if (sip_dst_set(&literal, c->resolve_host, 0))
		return NULL;

	return c->resolve_host;
}

/* the stream kind of one transport; account.h carries the contract */
int acct_stream_kind(enum acct_transport tp)
{
	return tp == ACCT_TP_TLS ? SIP_STREAM_TLS : SIP_STREAM_TCP;
}

/* the RFC 3263 4.1 service label of one transport; account.h carries it */
const char *acct_dns_service(enum acct_transport tp)
{
	switch (tp) {
	case ACCT_TP_TCP:
		return SIP_DNS_SERVICE_TCP;
	case ACCT_TP_TLS:
		return SIP_DNS_SERVICE_TLS;
	case ACCT_TP_UDP:
		break;
	}

	return SIP_DNS_SERVICE_DEFAULT;
}

/*
 * The next-hop question of one line; account.h carries the contract.
 *
 * A named port asks SIP_DNS_ADDR_SRV and not SIP_DNS_ADDR. Its address
 * questions are the "A or AAAA record lookup" of RFC 3263 4.2, so a proxy
 * with an address gets no SRV question. A name with no address then gets
 * one, because a zone that names its service only in SRV records is
 * otherwise unreachable. Neither kind pins the line to IPv4 as SIP_DNS_A
 * does: both ask AAAA beside A when cfg.ipv6 is set.
 */
int acct_dns_kind(enum cfg_dns_srv_mode mode, bool port_named)
{
	switch (mode) {
	case CFG_DNS_SRV_ALWAYS:
		return SIP_DNS_SRV;
	case CFG_DNS_SRV_NEVER:
		return SIP_DNS_ADDR;
	case CFG_DNS_SRV_AUTO:
		break;
	}

	return port_named ? SIP_DNS_ADDR_SRV : SIP_DNS_SRV;
}

/*
 * Transport is base only (account.h). The channel value is read only to report
 * that it was ignored: the firewall reconcile writes one proto for the whole
 * rule set, so two lines on two transports would take one line's inbound SIP
 * down without a word.
 *
 * An empty base token is udp, and that guard is required, not defensive: the
 * Via writers default on a NULL token alone, so an empty string must never
 * reach them. An unknown token is udp with a warning; the schema enum
 * validates the option, so this arm defends the account against a loader that
 * stops that validation.
 */
static enum acct_transport acct_transport_derive(const struct cfg *base,
						 const struct cfg_channel *ch,
						 unsigned int line)
{
	enum acct_transport tp;
	enum acct_transport chan;

	if (!acct_transport_map(base->transport, &tp)) {
		ULOG_WARN("line %u transport '%s' is not one of udp, tcp or tls, "
			  "using udp\n", line, base->transport);
		tp = ACCT_TP_UDP;
	}

	if (!ch->transport[0] || (acct_transport_map(ch->transport, &chan) && chan == tp))
		return tp;

	ULOG_WARN("line %u channel transport '%s' ignored, the base section is "
		  "authoritative and names '%s'\n", line, ch->transport,
		  acct_transport_name(tp));

	return tp;
}

unsigned int acct_ka_interval_effective(const struct acct_cfg *c,
					unsigned int bound_s, bool *forced)
{
	bool on = c->ka_enabled && c->ka_interval;
	unsigned int half = bound_s / 2;
	unsigned int configured = on ? c->ka_interval : half;
	unsigned int secs = configured < half ? configured : half;

	if (secs < SIP_KA_INTERVAL_MIN)
		secs = SIP_KA_INTERVAL_MIN;

	*forced = !c->ka_enabled || !c->ka_interval || secs != c->ka_interval;

	return secs;
}

/*
 * What restrict_source 2 asks of the signalling refresher, said once per
 * derive. A udp line runs the CRLF keepalive inside half the conntrack udp
 * bound whatever the configuration says (acct_ka_interval_effective), and the
 * derive reports when that overrode the configured pair, or when the bound is
 * too short for the SIP_KA_INTERVAL_MIN floor to meet. A stream line needs no
 * refresher: the connection itself is the pinhole, conntrack holds it as an
 * established tcp entry, and SO_KEEPALIVE refreshes that entry from the kernel
 * (sip/stream.h, SIP_STREAM_KA_IDLE_S).
 */
static void acct_refresher_report(const struct acct_cfg *c)
{
	unsigned int bound, secs;
	bool forced;

	if (!c->pinhole)
		return;

	if (c->transport != ACCT_TP_UDP) {
		ULOG_INFO("line %u restrict_source 2 over %s: the connection holds the "
			  "SIP pinhole and its keepalive refreshes it every %u s\n",
			  c->line, acct_transport_name(c->transport),
			  (unsigned int)SIP_STREAM_KA_IDLE_S);
		return;
	}

	bound = vr.host->net_ct_udp_bound_s(vr.host->ctx);
	secs = acct_ka_interval_effective(c, bound, &forced);
	if (bound < 2)
		ULOG_WARN("line %u restrict_source 2 under a %u s conntrack "
			  "udp timeout: the %u s floor of the SIP keepalive "
			  "cannot refresh the pinhole in time\n", c->line,
			  bound, (unsigned int)SIP_KA_INTERVAL_MIN);
	if (forced)
		ULOG_INFO("line %u sip keepalive forced to %u s: the SIP "
			  "pinhole needs a refresher inside the %u s "
			  "conntrack udp timeout\n", c->line, secs, bound);
}

/*
 * SDES carries the master keys in the SDP itself (RFC 4568 7.2), so a
 * signalling transport that does not encrypt hands them to every hop. The
 * config stays expressible, and the operator hears about the exposure once
 * per derive.
 */
static void acct_srtp_transport_check(const struct cfg *base, const struct acct_cfg *c)
{
	/* The derived transport, which is base only already: the check and the
	 * account it warns about read one value, so an ignored channel
	 * transport cannot silence it. */
	if (!base->srtp_enabled || c->transport == ACCT_TP_TLS)
		return;

	ULOG_WARN("line %u srtp_enabled over %s: the SDES master keys travel in clear "
		  "in the SDP (RFC 4568 7.2)\n", c->line, acct_transport_name(c->transport));
}

/*
 * An operator who reads a trace and finds an address question in front of
 * the SRV question, or no SRV question, has to be able to see why, so the
 * derive says it once, the way the keepalive and the SRTP checks do. An
 * auto line that asks SRV is the default and stays quiet, and so does a
 * channel with no next hop: accounts_start() derives it before it finds it
 * not registrable, and that line asks nothing.
 */
static void acct_dns_kind_report(enum cfg_dns_srv_mode mode,
				 const struct acct_cfg *c, bool port_named)
{
	if (!c->resolve_host[0])
		return;

	if (mode == CFG_DNS_SRV_NEVER) {
		ULOG_INFO("line %u asks the address of %s at port %u and never "
			  "SRV: dns_srv_mode is never\n", c->line,
			  c->resolve_host, c->resolve_port);
		return;
	}

	if (mode != CFG_DNS_SRV_AUTO || !port_named)
		return;

	ULOG_INFO("line %u names port %u, so the lookup asks the address of %s "
		  "first (RFC 3263 4.2), and SRV only if it has none\n",
		  c->line, c->resolve_port, c->resolve_host);
}

void acct_cfg_derive(const struct cfg *base, const struct cfg_channel *ch,
		     struct acct_cfg *out)
{
	const char *proxy = ch->sip_proxy;
	const char *domain = ch->sip_domain[0] ? ch->sip_domain : proxy;
	const char *outbound;
	const char *registrar;
	const char *user = ch->sip_username;
	const char *auth;
	const char *iface;
	char dom_host[ACCT_HOST_MAX];
	char reg_host[ACCT_HOST_MAX];
	uint16_t sip_port = ch->sip_port ? (uint16_t)ch->sip_port : 5060;
	uint16_t dom_port, reg_port, ob_embed;
	uint16_t remote_def, uri_omit;
	const char *dom_open, *dom_close;
	const char *reg_open, *reg_close;
	bool tls_def;
	bool port_named;

	memset(out, 0, sizeof(*out));
	out->line = ch->id;
	out->transport = acct_transport_derive(base, ch, out->line);

	/* The SIP port fallback chain (cfg.h). */
	if (!proxy[0])
		proxy = domain;
	outbound = ch->sip_outbound_proxy[0] ? ch->sip_outbound_proxy : proxy;
	registrar = ch->sip_registrar[0] ? ch->sip_registrar : domain;
	auth = ch->sip_auth_username[0] ? ch->sip_auth_username : user;

	iface = ch->local_interface[0] ? ch->local_interface : base->local_interface;

	str_copy(out->iface, sizeof(out->iface), iface);
	str_copy(out->contact_user, sizeof(out->contact_user), user);
	str_copy(out->auth_user, sizeof(out->auth_user), auth);
	str_copy(out->auth_pass, sizeof(out->auth_pass), ch->sip_secret);
	str_copy(out->display, sizeof(out->display), ch->sip_callerid_name);

	/*
	 * A proxy, domain or registrar string may carry an embedded :port. The
	 * resolver takes a bare host for SRV and A, so the host and the port
	 * are split here and never passed whole. The embedded port wins, then
	 * the matching explicit *_port option, then sip_port (the schema
	 * fallback).
	 */
	acct_host_port_split(domain, dom_host, sizeof(dom_host), &dom_port);
	acct_host_port_split(registrar, reg_host, sizeof(reg_host), &reg_port);
	acct_host_port_split(outbound, out->resolve_host, sizeof(out->resolve_host), &ob_embed);

	/*
	 * RFC 3263 4.2: the default remote port of sips is 5061, and it applies
	 * only when nothing named a port anywhere. That is the whole predicate:
	 * no embedded :port in the domain, the registrar or the outbound
	 * string (sip_outbound_proxy, else sip_proxy, else the domain), no
	 * explicit sip_outbound_proxy_port or sip_registrar_port, and no
	 * sip_port. sip_port_set is the one place the last of those is
	 * visible (cfg.h): the schema default 5060 always loads, so the value
	 * alone cannot tell a configured 5060 from an absent option. It is
	 * all or nothing on purpose: a line that names one port has an operator
	 * behind it who thinks in 5060, so a quiet move of its other port to
	 * 5061 would resolve half the line somewhere nobody configured. udp and
	 * tcp keep 5060, and an SRV answer names its own port over either
	 * default.
	 */
	tls_def = out->transport == ACCT_TP_TLS && !ch->sip_port_set && !dom_port &&
		  !reg_port && !ob_embed && !ch->sip_outbound_proxy_port &&
		  !ch->sip_registrar_port;
	remote_def = tls_def ? 5061 : sip_port;
	uri_omit = tls_def ? 5061 : 5060;

	if (!reg_port)
		reg_port = ch->sip_registrar_port ? (uint16_t)ch->sip_registrar_port : remote_def;
	out->resolve_port = ob_embed ? ob_embed :
			    ch->sip_outbound_proxy_port ? (uint16_t)ch->sip_outbound_proxy_port
							: remote_def;

	/*
	 * RFC 3263 4.2 asks SRV first only for a next hop that names no port. A
	 * next hop that names one asks its address first, and SRV only when the
	 * name has no address, which is product policy beyond RFC 3263 4.2. The
	 * predicate reads the three sources of resolve_port above and nothing
	 * else. dom_port, reg_port and sip_registrar_port reach the address of
	 * record and the registrar request URI, which is a different hop. It is
	 * narrower than tls_def on purpose: that one moves a default port and
	 * asks about every port the line carries, this one chooses a question
	 * and asks about the port of the string the line resolves. A domain
	 * port therefore counts here when, and only when, the fallback chain
	 * made the domain string the outbound string.
	 */
	port_named = ob_embed || ch->sip_outbound_proxy_port || ch->sip_port_set;
	out->dns_kind = (uint8_t)acct_dns_kind(base->dns_srv_mode, port_named);
	acct_dns_kind_report(base->dns_srv_mode, out, port_named);

	/*
	 * A URI wraps an IPv6 host in square brackets (RFC 3261 19.1.1), and
	 * both strings below are URIs: the address of record reaches the From,
	 * the To and, through acct_domain_of(), every request URI a call
	 * builds. resolve_host stays bare: the resolver takes a bare literal.
	 */
	dom_open = sip_host_bracket_needed(dom_host) ? "[" : "";
	dom_close = dom_open[0] ? "]" : "";
	reg_open = sip_host_bracket_needed(reg_host) ? "[" : "";
	reg_close = reg_open[0] ? "]" : "";

	if (user[0] && dom_host[0])
		snprintf(out->aor, sizeof(out->aor), "sip:%s@%s%s%s", user,
			 dom_open, dom_host, dom_close);
	else if (dom_host[0])
		snprintf(out->aor, sizeof(out->aor), "sip:%s%s%s", dom_open,
			 dom_host, dom_close);

	/* Include the registrar port in the request URI only when it is not the
	 * default of the transport, so the common case stays "sip:<domain>" for
	 * a tls line the same way it does for a udp one. */
	if (reg_host[0] && reg_port && reg_port != uri_omit)
		snprintf(out->registrar_uri, sizeof(out->registrar_uri),
			 "sip:%s%s%s:%u", reg_open, reg_host, reg_close,
			 reg_port);
	else if (reg_host[0])
		snprintf(out->registrar_uri, sizeof(out->registrar_uri),
			 "sip:%s%s%s", reg_open, reg_host, reg_close);

	out->local_port = ch->sip_local_port ? (uint16_t)ch->sip_local_port : 5060;
	out->sip_tos = (uint8_t)(ch->sip_tos & 0xff);
	out->expires = ch->sip_registration_expiry;
	out->retry_s = ch->sip_register_retry;
	out->dns_cache_update_mode = base->dns_cache_update_mode;
	out->dns_move_unregister = base->dns_move_unregister;
	out->mwi_subscribe = base->mwi_subscribe;

	/* The NAT keepalive is base scope; options_interval is a channel alias
	 * and the channel wins when it is nonzero. */
	out->ka_enabled = base->keepalive_enabled;
	out->ka_interval = base->keepalive_interval;
	out->options_interval = ch->options_interval ? ch->options_interval
						     : base->options_interval;
	out->pinhole = base->restrict_source == 2;
	out->linger_s = base->dns_linger;

	out->t1 = ch->timers.t1;
	out->t2 = ch->timers.t2;
	out->t4 = ch->timers.t4;
	out->ta = ch->timers.a;
	out->tb = ch->timers.b;
	out->td = ch->timers.d;
	out->te = ch->timers.e;
	out->tf = ch->timers.f;
	out->tg = ch->timers.g;
	out->th = ch->timers.h;
	out->ti = ch->timers.i;
	out->tj = ch->timers.j;
	out->tk = ch->timers.k;

	/* Every transport registers: the reliable ones run on the stream
	 * pool of sip/stream.h, so nothing here gates on udp. */
	out->enabled = ch->enabled && out->resolve_host[0] && out->aor[0] &&
		       out->registrar_uri[0] && out->contact_user[0];

	acct_refresher_report(out);
	acct_srtp_transport_check(base, out);
}

enum acct_apply acct_apply_class(const struct acct_cfg *a, const struct acct_cfg *b)
{
	if (a->enabled != b->enabled)
		return ACCT_APPLY_RESTART;

	if (strcmp(a->iface, b->iface) || a->local_port != b->local_port ||
	    a->sip_tos != b->sip_tos || a->transport != b->transport)
		return ACCT_APPLY_RESTART;

	if (strcmp(a->aor, b->aor) || strcmp(a->registrar_uri, b->registrar_uri) ||
	    strcmp(a->resolve_host, b->resolve_host) || a->resolve_port != b->resolve_port ||
	    strcmp(a->contact_user, b->contact_user) || strcmp(a->auth_user, b->auth_user) ||
	    strcmp(a->auth_pass, b->auth_pass) || strcmp(a->display, b->display) ||
	    a->expires != b->expires || a->retry_s != b->retry_s ||
	    a->dns_cache_update_mode != b->dns_cache_update_mode ||
	    a->dns_move_unregister != b->dns_move_unregister ||
	    a->dns_kind != b->dns_kind ||
	    a->mwi_subscribe != b->mwi_subscribe)
		return ACCT_APPLY_REREG;

	if (a->t1 != b->t1 || a->t2 != b->t2 || a->t4 != b->t4 || a->ta != b->ta ||
	    a->tb != b->tb || a->td != b->td || a->te != b->te || a->tf != b->tf ||
	    a->tg != b->tg || a->th != b->th || a->ti != b->ti || a->tj != b->tj ||
	    a->tk != b->tk || a->ka_enabled != b->ka_enabled ||
	    a->ka_interval != b->ka_interval || a->linger_s != b->linger_s ||
	    a->options_interval != b->options_interval || a->pinhole != b->pinhole)
		return ACCT_APPLY_LIVE;

	return ACCT_APPLY_NONE;
}

const char *acct_apply_name(enum acct_apply cls)
{
	switch (cls) {
	case ACCT_APPLY_NONE:
		return "none";
	case ACCT_APPLY_LIVE:
		return "live";
	case ACCT_APPLY_REREG:
		return "rereg";
	case ACCT_APPLY_RESTART:
		return "restart";
	}

	return "?";
}

const char *acct_status_name(int reg_state)
{
	switch ((enum sip_reg_state)reg_state) {
	case SIP_REG_UNREGISTERED:
		return "unregistered";
	case SIP_REG_RESOLVING:
	case SIP_REG_TRYING:
		return "trying";
	case SIP_REG_REGISTERED:
	case SIP_REG_REFRESHING:
		return "registered";
	case SIP_REG_BACKOFF:
		return "failed";
	}

	return "unknown";
}

bool acct_reg_live(int reg_state)
{
	switch ((enum sip_reg_state)reg_state) {
	case SIP_REG_REGISTERED:
	case SIP_REG_REFRESHING:
		return true;
	case SIP_REG_UNREGISTERED:
	case SIP_REG_RESOLVING:
	case SIP_REG_TRYING:
	case SIP_REG_BACKOFF:
		break;
	}

	return false;
}

bool acct_reregister_needed(bool dns_cache_update_mode, bool answer_changed)
{
	return dns_cache_update_mode && answer_changed;
}

bool acct_targets_exhausted(const struct sip_reg *reg, uint64_t now_ms)
{
	unsigned int i, n = sip_reg_target_count(reg);

	if (n < 2)
		return true;

	for (i = 0; i < n; i++) {
		if (sip_reg_target_get(reg, i)->penalty_until <= now_ms)
			return false;
	}

	return true;
}

/*
 * A new connection under the registration is a reconnect (account.h). A close
 * clears the handle and leaves up_seen standing, so the up that follows it
 * differs from SIP_HANDLE_NONE and refreshes; the first up of a registration
 * has nothing to differ from and refreshes nothing, because the REGISTER that
 * opened the connection carries the right five tuple already.
 */
bool acct_reconnect_needed(bool up_seen, uint32_t known, uint32_t conn,
			   bool inbound)
{
	return !inbound && up_seen && conn != known;
}

bool acct_reconnect_refresh_ok(int reg_state)
{
	switch ((enum sip_reg_state)reg_state) {
	case SIP_REG_TRYING:
	case SIP_REG_REFRESHING:
		return false;
	case SIP_REG_UNREGISTERED:
	case SIP_REG_RESOLVING:
	case SIP_REG_REGISTERED:
	case SIP_REG_BACKOFF:
		break;
	}

	return true;
}

unsigned int acct_conn_line(const uint16_t *ports, const char *const *ifaces,
			    const int *families, unsigned int count,
			    uint16_t local_port, const char *iface, int family)
{
	unsigned int i;

	if (!local_port)
		return ACCT_LINE_NONE;

	for (i = 0; i < count; i++) {
		if (!ports[i] || ports[i] != local_port || !ifaces[i])
			continue;
		if (families[i] != family)
			continue;
		if (strcmp(ifaces[i], iface ? iface : ""))
			continue;

		return i;
	}

	return ACCT_LINE_NONE;
}

enum acct_reload_op acct_reload_action(bool was_active, bool want, enum acct_apply cls)
{
	if (!was_active)
		return want ? ACCT_ROP_BUILD : ACCT_ROP_NONE;
	if (!want)
		return ACCT_ROP_REMOVE;

	switch (cls) {
	case ACCT_APPLY_NONE:
		return ACCT_ROP_NONE;
	case ACCT_APPLY_LIVE:
		return ACCT_ROP_TIMERS;
	case ACCT_APPLY_REREG:
	case ACCT_APPLY_RESTART:
		return ACCT_ROP_REPLACE;
	}

	return ACCT_ROP_NONE;
}

bool acct_host_port_split(const char *in, char *host, size_t hlen, uint16_t *port)
{
	const char *colon, *second;
	char *end;
	unsigned long p;

	*port = 0;
	if (!hlen)
		return false;
	host[0] = 0;
	if (!in || !*in)
		return false;

	/* [v6]:port, or a bracketed literal with no port. */
	if (in[0] == '[') {
		const char *close = strchr(in, ']');
		size_t n;

		if (!close) {
			snprintf(host, hlen, "%s", in);
			return true;
		}

		n = (size_t)(close - (in + 1));
		if (n >= hlen)
			n = hlen - 1;
		memcpy(host, in + 1, n);
		host[n] = 0;

		if (close[1] == ':') {
			p = strtoul(close + 2, &end, 10);
			if (!*end && p >= 1 && p <= 65535)
				*port = (uint16_t)p;
		}

		return true;
	}

	/* A single colon splits host:port; two or more colons is a bare IPv6
	 * literal and stays whole with no port. */
	colon = strchr(in, ':');
	second = colon ? strchr(colon + 1, ':') : NULL;
	if (colon && !second) {
		p = strtoul(colon + 1, &end, 10);
		if (!*end && p >= 1 && p <= 65535) {
			size_t n = (size_t)(colon - in);

			if (n >= hlen)
				n = hlen - 1;
			memcpy(host, in, n);
			host[n] = 0;
			*port = (uint16_t)p;
			return true;
		}
	}

	snprintf(host, hlen, "%s", in);

	return true;
}

/* ---------------------------------------------------------------- runtime */

/*
 * The environment of one line, whichever member of the union it is. Both start
 * with the struct sip_txn_env, so the address is the same either way and the
 * branch says which transport owns it rather than where it lives.
 */
static struct sip_txn_env *acct_env_of(struct account *a)
{
	if (acct_stream(&a->cfg))
		return &a->env.se.env;

	return &a->env.ue.env;
}

/* Both env inits point priv at their own member, which is the union itself. */
static struct account *acct_of_env(struct sip_txn_env *env)
{
	union acct_env *ae = env->base.priv;

	return container_of(ae, struct account, env);
}

/*
 * One transaction manager serves the registration client and the call legs of
 * a line, so the opaque owner carries a kind (account.h, the call interface of
 * account.c). The
 * ACK a client INVITE transaction owes for a non-2xx final is built inside
 * call_txn_resp(), which is the only place that holds the request, the dialog
 * and the branch it has to reuse; the registration client sends no INVITE and
 * therefore owes no ACK.
 */
static void acct_env_resp(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
			  const struct sip_msg *msg)
{
	struct account *a = acct_of_env(env);

	switch (acct_owner_kind_of(owner)) {
	case ACCT_OWNER_CALL:
		call_txn_resp(acct_owner_val(owner), txn, msg);
		return;
	case ACCT_OWNER_REG:
		if (a->reg_inited)
			sip_reg_txn_resp(&a->reg, txn, msg);
		return;
	case ACCT_OWNER_KA:
		if (a->ka_inited)
			sip_ka_txn_resp(&a->ka, txn, msg);
		return;
	case ACCT_OWNER_SUB:
		if (a->sub_inited)
			sip_sub_txn_resp(&a->sub, txn, msg);
		return;
	case ACCT_OWNER_NONE:
	case ACCT_OWNER_UA:
		return;
	}
}

static void acct_env_done(struct sip_txn_env *env, uint32_t owner, uint32_t txn,
			  enum sip_txn_reason reason)
{
	struct account *a = acct_of_env(env);

	switch (acct_owner_kind_of(owner)) {
	case ACCT_OWNER_CALL:
		call_txn_done(acct_owner_val(owner), txn, reason);
		return;
	case ACCT_OWNER_REG:
		if (a->reg_inited)
			sip_reg_txn_done(&a->reg, txn, reason);
		return;
	case ACCT_OWNER_KA:
		if (a->ka_inited)
			sip_ka_txn_done(&a->ka, txn, reason);
		return;
	case ACCT_OWNER_SUB:
		if (a->sub_inited)
			sip_sub_txn_done(&a->sub, txn, reason);
		return;
	case ACCT_OWNER_NONE:
	case ACCT_OWNER_UA:
		return;
	}
}

static void acct_ua_ack_missing(struct ua *ua, uint32_t owner, void *priv)
{
	if (acct_owner_kind_of(owner) == ACCT_OWNER_CALL)
		call_ua_ack_missing(acct_owner_val(owner));
}

static void acct_ua_glare_retry(struct ua *ua, uint32_t owner, void *priv)
{
	if (acct_owner_kind_of(owner) == ACCT_OWNER_CALL)
		call_ua_glare_retry(acct_owner_val(owner));
}

/* RFC 3261 21.5.4: the 503 delay is reported, and the call layer already
 * cleared the leg with the congestion class. Nothing schedules on it. */
static void acct_ua_retry_after(struct ua *ua, uint32_t owner, uint32_t secs, bool present,
				void *priv)
{
}

static const struct ua_cb account_ua_cb = {
	.ack_missing = acct_ua_ack_missing,
	.retry_after = acct_ua_retry_after,
	.glare_retry = acct_ua_glare_retry,
};

static uint64_t acct_now(struct account *a)
{
	struct sip_txn_env *env = acct_env_of(a);

	return env->base.now_ms(&env->base);
}

/*
 * The identifier entropy of the transaction layer. Its own fallback generator
 * seeds from the millisecond clock (sip_txn_rand), so two accounts built in
 * the same millisecond draw identical branch, Call-ID and From tag streams,
 * and tp_msg_cb offers every datagram to every manager: one account's
 * responses would then match, and be consumed by, the other account's
 * transactions. OS randomness removes the collision.
 *
 * The last resort mixes three quantities that cannot all repeat between two
 * accounts of one daemon: the clock, a process wide counter and the address of
 * the account itself. A predictable branch is a lesser fault than a shared one.
 */
static uint32_t acct_env_rand32(struct proto_timer_env *base)
{
	static uint32_t seq;
	struct account *a = acct_of_env(sip_txn_env_of(base));
	uint64_t now;
	uint32_t v = 0;

	if (sip_os_random(&v, sizeof(v)))
		return v;

	now = acct_now(a);
	seq++;

	return (uint32_t)(now ^ (now >> 32)) ^ (seq * 2654435761u) ^
	       (uint32_t)((uintptr_t)a >> 4);
}

static void account_grace_cb(struct uloop_timeout *t);
/*
 * What one event moved, gathered by the entry point that handles it so that no
 * line is rebuilt twice for it. One tracker event can carry a netdev move, an
 * address of each family and a preference move at once, and each of them
 * reaches a different set of lines.
 */
struct acct_moved {
	bool addr4;		/* the IPv4 address of the voice netdev moved */
	bool addr6;		/* the IPv6 address moved */
	bool prefer;		/* the preferred family moved */
	bool force;		/* the IPv4 address came back after a down */
};

static int acct_family_prefer(void);
static void acct_served_push(void);
static const struct net_iface_sub acct_iface_sub;
static const char *acct_bind_device_of(const struct account *a);
static const struct sip_txn_dst *acct_target_dst(const struct account *a);

/*
 * What a family move leaves the registration doing.
 *
 * ACCT_FAMILY_REFRESH sends a REGISTER at once. A resolved answer takes it: the
 * registrar holds a Contact of the family the line left, nothing answers on it,
 * and with dns_cache_update_mode 0 no other path would register the new one.
 *
 * ACCT_FAMILY_RETRY sends nothing. A failed attempt takes it: reg.c armed the
 * retry timer with the ordinary backoff before it reported the failure, and the
 * attempt that timer starts reads the new local host, Contact and socket. A
 * refresh there would clear the backoff instead (sip_reg_refresh_now), so a
 * line whose targets of both families all refuse would retry at the first delay
 * for ever.
 */
enum acct_family_send {
	ACCT_FAMILY_REFRESH = 0,
	ACCT_FAMILY_RETRY,
};

/*
 * What a move did. The resolved-answer path has to tell a move from a line
 * that signals in the target's family already: the move sends its own REGISTER
 * and the re-registration the dns_cache_update_mode hook asks for would put a
 * second one on the wire, with the line reported down in between. There are
 * three answers, because a refused move is neither of the other two; a bool
 * with an out parameter would make every call site carry both.
 */
enum acct_family_result {
	/* the line is in that family already, or nothing may move it */
	ACCT_FAMILY_STAY = 0,
	ACCT_FAMILY_MOVED,
	ACCT_FAMILY_REFUSED,	/* refused; the line stands as it stood */
};

static enum acct_family_result
account_family_sync(struct account *a, const struct sip_txn_dst *dst,
		    enum acct_family_send send);
static void fw_reconcile(void);
static void account_sub_init(struct account *a);
static void account_sub_start(struct account *a);
static void acct_reg_conn_seed(struct account *a);
static void account_resolve(struct account *a);

/* --------------------------------------------------- registration callbacks */

/* A refresh reports the state the line already held, and moved false keeps
 * the call trace from claiming a move that did not happen. */
void acct_reg_state_post(unsigned int line, bool up, const char *proxy,
			 bool moved)
{
	vr.host->reg_state_post(vr.host->ctx, line, up, proxy, moved);
}

static void reg_registered_cb(struct sip_reg *reg, uint32_t expires_s, void *priv)
{
	struct account *a = priv;
	/* Read before the two stores below. A refresh finds the line registered
	 * already and is routine, about every half expiry; the first answer
	 * after a stop or a failure is the state change. */
	bool first = a->refresh_at_ms == 0 || a->last_code != 200;

	a->last_code = 200;
	a->refresh_at_ms = acct_now(a) + (uint64_t)sip_reg_refresh_secs(expires_s) * 1000;
	if (first)
		ULOG_INFO("line %u registered, expires %u s\n", a->line, expires_s);
	else
		ACCT_TRACE("line %u registration refreshed, expires %u s",
			   a->line, expires_s);

	/* the configured next hop, never the resolved address: an address
	 * would change the field's shape for the cloud consumers and churn on
	 * every failover (event.h) */
	acct_reg_state_post(a->line, true, a->cfg.resolve_host, first);

	/*
	 * The subscription rides an established registration: the notifier is
	 * handed the Contact the registrar already holds, so the NOTIFY it
	 * sends reaches the same address (sip/sub.h). A later refresh finds it
	 * running and changes nothing.
	 */
	account_sub_start(a);
}

static void reg_unregistered_cb(struct sip_reg *reg, void *priv)
{
	struct account *a = priv;

	a->refresh_at_ms = 0;
	a->unreg_pending = false;
	ULOG_INFO("line %u unregistered\n", a->line);
	acct_reg_state_post(a->line, false, NULL, true);

	/*
	 * A reload's Expires 0 just completed: run the teardown or rebuild now,
	 * on a clean stack, instead of waiting out the grace timeout. The call
	 * drain has its own claim on the same objects, though: a BYE or a
	 * CANCEL still on the wire owes an ACK for the answer it is waiting
	 * for, and tearing the manager down now would detach the transaction
	 * that has to build it. The grace deadline still bounds the wait.
	 */
	if (a->pending && !calls_line_busy(a->line))
		uloop_timeout_set(&a->grace_timer, 0);

	if (vr.shutting_down && !accounts_shutdown_pending())
		vr.host->shutdown_done(vr.host->ctx);
}

static void reg_failed_cb(struct sip_reg *reg, uint32_t code, uint32_t retry_s, void *priv)
{
	struct account *a = priv;
	struct sip_txn_dst probe;
	uint64_t now = acct_now(a);

	a->last_code = code;
	a->refresh_at_ms = 0;
	ULOG_WARN("line %u registration failed, code %u, next attempt in %u s\n", a->line,
		  code, retry_s);
	acct_reg_state_post(a->line, false, NULL, true);

	/*
	 * The failure already moved the sticky selection to the target the next
	 * attempt uses (reg.c reg_fail), so the line follows that target's
	 * family now and the retry timer, armed above with the ordinary
	 * backoff, sends the attempt that reads it.
	 */
	account_family_sync(a, acct_target_dst(a), ACCT_FAMILY_RETRY);

	/*
	 * A failure with no fresh target left asks DNS for a member that is
	 * live now, ahead of the TTL: the cache entry expires and the answer
	 * flows through account_dns_result() like any other, so a moved pool
	 * recovers inside the retry delay instead of waiting out the TTL. A
	 * lookup already in flight serves the same purpose and is left alone:
	 * a second account_resolve() would overwrite a->dns_query and
	 * dns_done_cb() would discard the first answer. A literal proxy never
	 * asks: its answer cannot move, so the redelivery could only repeat
	 * the address that just failed, once per window, for nothing.
	 */
	if (a->pending || vr.shutting_down)
		return;
	if (sip_dst_set(&probe, a->cfg.resolve_host, 5060))
		return;
	if (!acct_targets_exhausted(&a->reg, now))
		return;
	if (now - a->fail_resolve_ms < DNS_FAIL_RESOLVE_MIN_S * 1000)
		return;

	a->fail_resolve_ms = now;
	sip_dns_cache_expire(&vr.dns, a->cfg.resolve_host,
			     (enum sip_dns_kind)a->cfg.dns_kind,
			     a->cfg.resolve_port,
			     acct_dns_service(a->cfg.transport));
	if (a->dns_query == SIP_HANDLE_NONE) {
		uloop_timeout_cancel(&a->resolve_timer);
		account_resolve(a);
	}
}

/*
 * A move failed and the client went back to the server that holds the binding
 * (sip/reg.h "The server move"). The line may have followed the new server into
 * the other family before that REGISTER left, because the answer runs the
 * family sync ahead of the retarget gate; nothing else tells it to come back,
 * since the rollback reports no failure by design. The sync reads the selection
 * the rollback restored: a same-family rollback answers ACCT_FAMILY_STAY and
 * does nothing, and a cross-family one takes the transport, the local host and
 * the Contact back and registers at the binding's server at once.
 */
static void reg_rolled_back_cb(struct sip_reg *reg, void *priv)
{
	struct account *a = priv;

	account_family_sync(a, acct_target_dst(a), ACCT_FAMILY_REFRESH);
}

static const struct sip_reg_cb account_reg_cb = {
	.registered = reg_registered_cb,
	.unregistered = reg_unregistered_cb,
	.failed = reg_failed_cb,
	.rolled_back = reg_rolled_back_cb,
};

/* --------------------------------------------------------- resolve and reg */

/* the one family word the status keys and the logs of this file use */
static const char *acct_family_name(int family)
{
	return family == AF_INET6 ? "ipv6" : "ipv4";
}

/*
 * The address netifd reports for the voice netdev in one family, empty when it
 * reports none. It is the address a Via and a Contact of that family carry.
 */
static const char *acct_iface_addr_of(int family)
{
	const struct net_iface_state *st =
		vr.host->net_iface_state_get(vr.host->ctx);

	return family == AF_INET6 ? st->addr6 : st->addr4;
}

/* the family of a resolved next hop, 0 for no destination at all */
static int acct_dst_family(const struct sip_txn_dst *dst)
{
	return dst ? (int)dst->sa.ss_family : 0;
}

/*
 * Whether a line may signal in this family. The two gates hold a line out of
 * IPv6, and out of IPv6 only: the operator switch, and a backend that carries
 * no IPv6 media, because the media of a call follows the family of its
 * signalling (RFC 6157 section 5) and every call would answer with congestion.
 * The resolver knows the first gate alone (sip_dns_ipv6_set). The target list
 * applies neither: the answer carries no mark that tells a configured literal
 * from a resolved record (acct_targets_load), so a target of a refused family
 * stays selectable and the move it earns is refused here, once per penalty
 * cycle.
 */
static bool acct_family_served(int family)
{
	if (family != AF_INET6)
		return true;

	return vr.ipv6 && (vr.hal_features & HAL_FEAT_IPV6_MEDIA);
}

/*
 * The local host of one line in one family: the Via sent-by, the Contact host
 * and the address its media answers on. family is the family the line signals
 * in, or the candidate family of a move, so every answer below is of that
 * family and never of the one the line is leaving.
 */
static bool account_local_host(struct account *a, int family,
			       const struct sip_txn_dst *dst)
{
	const char *iface_addr = acct_iface_addr_of(family);
	struct sip_sock *sk = sip_sock_get(&vr.tp, a->sock);
	char host[SIP_SOCK_HOST_MAX];

	/* The netifd reported address is authoritative for the voice interface. */
	if (iface_addr[0]) {
		str_copy(a->local_host, sizeof(a->local_host), iface_addr);
		return true;
	}

	/*
	 * A stream line has no socket to ask. Its inbound path is a listener on
	 * the wildcard address and its outbound one is a connection that does
	 * not exist yet, so the only answer left is the one the route gives:
	 * the probe below is sip_sock_source_host() without a bound socket. It
	 * matters on a box where netifd never answers, which is where a udp
	 * line falls back to exactly the same question.
	 *
	 * A line that moves family asks the same probe: the datagram socket it
	 * still holds is bound in the family it leaves, and both the address it
	 * reports and the route it can ask about belong to that family.
	 *
	 * The probe answers in the family asked for here, so a destination of
	 * the other family leaves the line without a local host, which is the
	 * answer an unreachable target has always had.
	 */
	if (acct_stream(&a->cfg) || a->family != family) {
		const char *dev = acct_stream(&a->cfg) ? a->env.se.iface :
							acct_bind_device_of(a);

		if (!dst ||
		    !vr.host->net_iface_source_host_probe(
			    vr.host->ctx, dev, family,
			    (const struct sockaddr *)&dst->sa, dst->sa_len,
			    host, sizeof(host)))
			return false;

		str_copy(a->local_host, sizeof(a->local_host), host);

		return true;
	}

	if (!sk)
		return false;

	/* A socket bound to a specific address answers with it. A wildcard bind
	 * reports 0.0.0.0, which is no use in a Via, so fall through to the
	 * source address the kernel picks toward the first target. */
	if (sip_sock_local_host(sk, host, sizeof(host)) && strcmp(host, "0.0.0.0") &&
	    strcmp(host, "::")) {
		str_copy(a->local_host, sizeof(a->local_host), host);
		return true;
	}

	if (dst && sip_sock_source_host(sk, dst, host, sizeof(host))) {
		str_copy(a->local_host, sizeof(a->local_host), host);
		return true;
	}

	return false;
}

static void account_reg_start(struct account *a)
{
	struct sip_reg_cfg rc;

	memset(&rc, 0, sizeof(rc));
	rc.aor = a->cfg.aor;
	rc.registrar = a->cfg.registrar_uri;
	rc.contact_user = a->cfg.contact_user;
	rc.auth_user = a->cfg.auth_user;
	rc.auth_pass = a->cfg.auth_pass;
	rc.display = a->cfg.display[0] ? a->cfg.display : NULL;
	rc.user_agent = ACCT_USER_AGENT;
	rc.local_host = a->local_host;
	rc.local_port = a->cfg.local_port;
	rc.transport = acct_transport_token(a->cfg.transport);
	rc.expires = a->cfg.expires;
	rc.retry_s = a->cfg.retry_s;
	rc.move_unregister = a->cfg.dns_move_unregister;

	/* a fresh registration has seen no connection of its own yet, so the
	 * first one that comes up is no reconnect (acct_reconnect_needed) */
	a->reg_conn = SIP_HANDLE_NONE;
	a->reg_conn_seen = false;

	sip_reg_init(&a->reg, &a->mgr, acct_env_of(a),
		     acct_owner_make(ACCT_OWNER_REG, a->line + 1),
		     &rc, &account_reg_cb, a);
	a->reg_inited = true;
	account_sub_init(a);
	sip_reg_start(&a->reg);
	ULOG_INFO("line %u registering %s via %s:%u\n", a->line, a->cfg.aor,
		  a->cfg.resolve_host, a->cfg.resolve_port);
}

/*
 * The destination of the moment: the sticky registration target (sip/reg.h).
 * NULL before the first resolution, which is what makes a tick skip instead of
 * guessing an address. The keepalive and the subscription both ask for it.
 */
static const struct sip_txn_dst *acct_target_dst(const struct account *a)
{
	const struct sip_reg_target *tg;

	if (!a->reg_inited || !sip_reg_target_count(&a->reg))
		return NULL;

	tg = sip_reg_target_get(&a->reg, sip_reg_target_current(&a->reg));

	return tg ? &tg->dst : NULL;
}

/*
 * The service route a request toward dst may carry (RFC 3608, sip/reg.h), or
 * NULL for none. The registrar granted the route with the binding it holds, so
 * only a request toward that same server takes it. The two differ during a
 * server move. They differ again under dns_cache_update_mode 0, after a
 * reload returns the selection to priority 0. A Route naming SBC A must not
 * reach a request to SBC B.
 */
static const struct sip_route_hdrs *
acct_service_route_of(const struct account *a, const struct sip_txn_dst *dst)
{
	const struct sip_txn_dst *binding;

	if (!dst)
		return NULL;

	binding = sip_reg_binding_dst(&a->reg);
	if (!binding || !sip_dst_eq(dst, binding))
		return NULL;

	return sip_reg_service_route(&a->reg);
}

/* a udp line under the conntrack pinhole form: the one that enforces the
 * refresher, because nothing else holds its SIP tuple open */
static bool acct_pinhole_udp(const struct account *a)
{
	return a->cfg.pinhole && !acct_stream(&a->cfg);
}

static void acct_ka_cfg_make(const struct account *a, struct sip_ka_cfg *kc)
{
	bool forced;

	memset(kc, 0, sizeof(*kc));
	kc->aor = a->cfg.aor;
	kc->registrar = a->cfg.registrar_uri;
	kc->local_host = a->local_host;
	kc->local_port = a->cfg.local_port;
	kc->user_agent = ACCT_USER_AGENT;
	kc->transport = acct_transport_token(a->cfg.transport);
	kc->display = a->cfg.display[0] ? a->cfg.display : NULL;
	kc->crlf_enabled = a->cfg.ka_enabled;
	kc->crlf_interval_s = a->cfg.ka_interval;
	kc->options_interval_s = a->cfg.options_interval;

	if (!acct_pinhole_udp(a))
		return;

	kc->crlf_enabled = true;
	kc->crlf_interval_s = acct_ka_interval_effective(&a->cfg, a->ct_bound_s,
							 &forced);
}

static void account_ka_update(struct account *a)
{
	struct sip_ka_cfg kc;

	if (!a->ka_inited)
		return;

	acct_ka_cfg_make(a, &kc);
	sip_ka_cfg_update(&a->ka, &kc);
}

/*
 * The conntrack udp bound of the moment, read again on every round of a mode 2
 * udp line: /etc/config/conntrack can move it under a running daemon, and a
 * keepalive derived under the old value could then run outside the new one.
 * A move re-derives the interval and re-arms the timer inside it; the update
 * is safe from inside the round (sip/keepalive.h, sip_ka_cfg_update).
 */
static void acct_pinhole_bound_sync(struct account *a)
{
	unsigned int bound, secs;
	bool forced;

	if (!acct_pinhole_udp(a))
		return;

	bound = vr.host->net_ct_udp_bound_s(vr.host->ctx);
	if (bound == a->ct_bound_s)
		return;

	secs = acct_ka_interval_effective(&a->cfg, bound, &forced);
	ULOG_INFO("line %u conntrack udp timeout %u s -> %u s, sip keepalive "
		  "every %u s\n", a->line, a->ct_bound_s, bound, secs);
	a->ct_bound_s = bound;
	account_ka_update(a);
}

/* whether a destination is one of the line's resolved targets, any family */
static bool acct_target_covers(const struct account *a,
			       const struct sip_txn_dst *dst)
{
	unsigned int i, n = sip_reg_target_count(&a->reg);

	for (i = 0; i < n; i++) {
		if (sip_dst_eq(&sip_reg_target_get(&a->reg, i)->dst, dst))
			return true;
	}

	return false;
}

/* ------------------------------------------------------ the linger retention */

void acct_linger_note(struct acct_linger *lg, const struct sip_txn_dst *dst,
		      uint64_t now_ms)
{
	struct acct_linger_entry *e;
	unsigned int i, oldest = 0;

	if (!dst || !dst->sa_len)
		return;

	for (i = 0; i < lg->count; i++) {
		e = &lg->entry[i];
		if (sip_dst_eq(&e->dst, dst)) {
			e->last_seen_ms = now_ms;
			return;
		}
		if (e->last_seen_ms < lg->entry[oldest].last_seen_ms)
			oldest = i;
	}

	e = lg->count < ACCT_LINGER_MAX ? &lg->entry[lg->count++]
					: &lg->entry[oldest];
	e->dst = *dst;
	e->last_seen_ms = now_ms;
}

void acct_linger_expire(struct acct_linger *lg, uint64_t now_ms,
			uint32_t linger_s)
{
	unsigned int i, keep = 0;

	for (i = 0; i < lg->count; i++) {
		if (!linger_s ||
		    now_ms - lg->entry[i].last_seen_ms >= (uint64_t)linger_s * 1000)
			continue;

		lg->entry[keep++] = lg->entry[i];
	}

	lg->count = keep;
}

bool acct_linger_covers(const struct acct_linger *lg,
			const struct sip_txn_dst *dst)
{
	unsigned int i;

	for (i = 0; i < lg->count; i++) {
		if (sip_dst_eq(&lg->entry[i].dst, dst))
			return true;
	}

	return false;
}

uint64_t acct_linger_deadline(const struct acct_linger *lg, uint32_t linger_s)
{
	uint64_t at = 0;
	unsigned int i;

	if (!linger_s)
		return 0;

	for (i = 0; i < lg->count; i++) {
		uint64_t due = lg->entry[i].last_seen_ms +
			       (uint64_t)linger_s * 1000;

		if (!at || due < at)
			at = due;
	}

	return at;
}

static bool proxy_list_has(char (*list)[PROTO_FW_SRC_MAX], unsigned int count,
			   const char *addr)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (!strcmp(list[i], addr))
			return true;
	}

	return false;
}

unsigned int acct_linger_collect(const struct acct_linger *lg,
				 char (*list)[PROTO_FW_SRC_MAX], unsigned int count)
{
	char host[PROTO_FW_SRC_MAX];
	unsigned int i;

	for (i = 0; i < lg->count; i++) {
		if (!sip_dst_host(&lg->entry[i].dst, host, sizeof(host)))
			continue;
		if (proxy_list_has(list, count, host))
			continue;
		if (count >= PROTO_FW_PROXY_MAX) {
			ULOG_WARN("firewall: lingering proxy source %s dropped, "
				  "only %u rules exist\n", host,
				  (unsigned int)PROTO_FW_PROXY_MAX);
			continue;
		}

		count = vr.host->fw_proxy_add(vr.host->ctx, list, count, host);
	}

	return count;
}

/*
 * The expiry deadline of the retention, independent of answers and rounds: a
 * failed resolver and a disabled keepalive still remove a mode 1 rule when
 * its window ends, because neither of the two ordinary reconcile triggers
 * runs then. Expiring from the round alone would also never run in mode 1,
 * which has no round.
 */
static void acct_linger_rearm(struct account *a)
{
	uint64_t at = acct_linger_deadline(&a->linger, a->cfg.linger_s);
	uint64_t now = acct_now(a);

	if (!at) {
		uloop_timeout_cancel(&a->linger_timer);
		return;
	}

	uloop_timeout_set(&a->linger_timer, at > now ? (int)(at - now) : 0);
}

static void linger_timer_cb(struct uloop_timeout *t)
{
	struct account *a = container_of(t, struct account, linger_timer);

	acct_linger_expire(&a->linger, acct_now(a), a->cfg.linger_s);
	acct_linger_rearm(a);
	fw_reconcile();
}

/*
 * The destinations of one CRLF round (sip/keepalive.h). Index 0 is the sticky
 * target, and the only one a stream line pings: its connection is the pinhole.
 * A udp line follows with the other resolved targets of its own family, so a
 * secondary SRV target can deliver an INVITE through a tuple the round holds;
 * then with the lingering addresses of its family that
 * no target covers, so every member of a pool that rotates one record per
 * query keeps a tuple for the dns_linger window; and then with the
 * destinations of its live dialogs that neither set covers, so a call that
 * outlives a DNS move keeps its proxy tuple alive until it ends. A line
 * signals in one family at a time (acct_family_move), so a target of the
 * other family gets no ping. Index 0 also compacts the retention, so the
 * indexes stay stable for the rest of the walk.
 */
static const struct sip_txn_dst *acct_ka_dst_at(struct sip_ka *ka, void *priv,
						unsigned int idx)
{
	struct account *a = priv;
	const struct sip_txn_dst *dst;
	unsigned int i, n, cur, seen = 1;

	if (!idx) {
		acct_pinhole_bound_sync(a);
		acct_linger_expire(&a->linger, acct_now(a), a->cfg.linger_s);
		return acct_target_dst(a);
	}

	if (acct_stream(&a->cfg) || !acct_target_dst(a))
		return NULL;

	n = sip_reg_target_count(&a->reg);
	cur = sip_reg_target_current(&a->reg);
	for (i = 0; i < n; i++) {
		dst = &sip_reg_target_get(&a->reg, i)->dst;
		if (i == cur || acct_dst_family(dst) != a->family)
			continue;
		if (seen++ == idx)
			return dst;
	}

	for (i = 0; i < a->linger.count; i++) {
		dst = &a->linger.entry[i].dst;
		if (acct_dst_family(dst) != a->family ||
		    acct_target_covers(a, dst))
			continue;
		if (seen++ == idx)
			return dst;
	}

	for (i = 0; (dst = calls_line_dialog_dst(a->line, i)); i++) {
		if (acct_target_covers(a, dst) ||
		    acct_linger_covers(&a->linger, dst))
			continue;
		if (seen++ == idx)
			return dst;
	}

	return NULL;
}

/*
 * The live apply of the keepalive options. A line that sat outside mode 2
 * never re-read the conntrack bound, so the bound is taken fresh before the
 * interval is derived. A udp line this reload moved into mode 2 sends the
 * round now: its tuples may have aged out under a keepalive the old mode
 * left off, and the reconcile of the same reload removes the rules.
 */
static void account_ka_reload(struct account *a, bool entered)
{
	if (acct_pinhole_udp(a))
		a->ct_bound_s = vr.host->net_ct_udp_bound_s(vr.host->ctx);

	account_ka_update(a);

	if (entered)
		sip_ka_ping_now(&a->ka);
}

/*
 * The round now on every udp line, for a change that moved the tuples under
 * them all at once. A line draining its Expires 0 sends nothing more; a stream
 * line's connection reopens on its next send and needs no ping.
 */
static void acct_udp_lines_ping(void)
{
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];

		if (a->active && a->ka_inited && !a->pending &&
		    !acct_stream(&a->cfg))
			sip_ka_ping_now(&a->ka);
	}
}

static void acct_sub_cfg_make(const struct account *a, struct sip_sub_cfg *sc)
{
	memset(sc, 0, sizeof(*sc));
	sc->aor = a->cfg.aor;
	sc->registrar = a->cfg.registrar_uri;

	/*
	 * The registered Contact, and a pointer into the registration rather
	 * than a copy: reg.c rebuilds that string in place whenever a registrar
	 * reports another address (sip/reg.h "Contact and NAT"), so a refresh
	 * of the subscription always carries the address the registrar holds
	 * now. The registration outlives the subscription on every teardown
	 * path.
	 */
	sc->contact = sip_reg_contact(&a->reg);
	sc->auth_user = a->cfg.auth_user;
	sc->auth_pass = a->cfg.auth_pass;
	sc->display = a->cfg.display[0] ? a->cfg.display : NULL;
	sc->user_agent = ACCT_USER_AGENT;
	sc->local_host = a->local_host;
	sc->local_port = a->cfg.local_port;
	sc->transport = acct_transport_token(a->cfg.transport);
	sc->expires = a->cfg.expires;
	sc->service_route = acct_service_route_of(a, acct_target_dst(a));
}

/*
 * The destination of the moment, and the one point where the service route of
 * the subscription's configuration is decided.
 *
 * The decision cannot be made once and kept. sip_sub_init() copies the
 * configuration by value, and the account builds it before the first 2xx.
 * There is no binding and no route at that moment.
 *
 * Nor can an event answer it. resp_ok() of reg.c stores a new binding_dst and
 * a new route, then returns with no callback at all when that answer moved
 * the Contact. An account that watched the callbacks would hand out the route
 * of one server for a request toward another.
 *
 * sip/sub.h calls this back when a request is built, directly before the
 * build. The rule of acct_service_route_of() is therefore answered against
 * the destination the request leaves for, and cannot go stale.
 * sip_sub_cfg_update() replaces the configuration by value and touches no
 * timer, identity or transaction, so it is safe from inside this callback.
 */
static const struct sip_txn_dst *acct_sub_dst(struct sip_sub *sub, void *priv)
{
	struct account *a = priv;
	struct sip_sub_cfg sc;

	acct_sub_cfg_make(a, &sc);
	sip_sub_cfg_update(sub, &sc);

	return acct_target_dst(a);
}

static void account_sub_done(struct account *a)
{
	if (!a->sub_inited)
		return;

	/*
	 * Expires 0 leaves as one datagram and is not waited for: a
	 * subscription no notifier can deliver to costs the switch a
	 * retransmission or two and expires on its own, while the account is
	 * already discarding the objects underneath it.
	 */
	sip_sub_stop(&a->sub, true);
	sip_sub_done(&a->sub);
	a->sub_inited = false;
}

static void account_sub_init(struct account *a)
{
	struct sip_sub_cfg sc;

	account_sub_done(a);
	acct_sub_cfg_make(a, &sc);
	sip_sub_init(&a->sub, &a->mgr, acct_env_of(a),
		     acct_owner_make(ACCT_OWNER_SUB, a->line + 1), &sc, acct_sub_dst, a);
	a->sub_inited = true;
}

static void account_sub_start(struct account *a)
{
	/* The intake needs no client at all, so a line with mwi_subscribe 0
	 * still takes every unsolicited NOTIFY the switch sends it. */
	if (!a->cfg.mwi_subscribe || !a->sub_inited)
		return;

	sip_sub_start(&a->sub);
}

/*
 * A resolved answer, applied to the sticky target list. The TTL re-resolution
 * asks for the same name and gets the same servers back in a fresh RFC 2782
 * draw (shared/resolve.h), so a clear plus re-add would move the sticky
 * selection to whatever the draw put at index 0, and empty every penalty box,
 * once per TTL cycle. The reload pair replaces the list instead: the selection
 * and the penalty state follow the identity of a target, which is its address
 * and port (sip/reg.h).
 *
 * Each target carries the priority of the SRV record it came from, so the
 * client follows the zone operator's routing policy.
 *
 * Every target of the answer enters the list, a family the line may not signal
 * in included. The resolver hands an IPv6 literal over whatever the operator
 * gate says (shared/resolve.h "Numeric targets"), and the result carries no
 * mark that tells a literal from a resolved record, so a family filter here
 * would drop the literal proxy the line is configured with.
 *
 * It is non-static so test_account.c can script two answers over one live
 * registration; the prototype lives in account.h.
 */
bool acct_targets_load(struct sip_reg *reg, const struct sip_dns_result *res)
{
	unsigned int i;

	sip_reg_targets_reload(reg);
	for (i = 0; i < res->count; i++) {
		const struct sip_dns_target *t = &res->target[i];

		sip_reg_target_add_prio(reg, t->host, t->port, t->prio);
	}

	return sip_reg_targets_reload_end(reg);
}

/*
 * Arm the next resolution after a failed one. st is the reason of the failure,
 * the status of the answer or the refusal of the resolver to take the question
 * at all, and it reaches the log with the delay: once per step of the ladder,
 * and again at each tenth attempt once the cap holds the delay flat. A provider
 * name that stays dead costs one line a minute instead of one every five
 * seconds.
 *
 * No "already pending" guard, where the tracker's recovery (net/iface.c
 * recover_arm()) has one: one attempt of
 * one line ends in one arm here. The two callers are the answer of that line's
 * own query (account_dns_result) and the refusal of the resolver to start it
 * (account_resolve), where the recovery timer serves every line and takes a
 * reason from several of them for one attempt.
 */
static void dns_retry_arm(struct account *a, enum sip_dns_status st)
{
	unsigned int delay;

	a->dns_fails++;
	delay = retry_delay_ms(a->dns_fails, DNS_RETRY_MS, DNS_RETRY_MAX_MS);

	if (delay != a->dns_retry_ms || !(a->dns_fails % DNS_RETRY_LOG_EVERY))
		ULOG_WARN("line %u resolve of %s failed %u time(s): %s; next "
			  "attempt in %u ms\n", a->line, a->cfg.resolve_host,
			  a->dns_fails, sip_dns_status_name(st), delay);

	a->dns_retry_ms = delay;
	uloop_timeout_set(&a->resolve_timer, (int)delay);
}

/* The ladder starts from the first rung whenever the next failure arms it. */
static void dns_retry_reset(struct account *a)
{
	a->dns_fails = 0;
	a->dns_retry_ms = 0;
}

static void account_resolve(struct account *a)
{
	if (!a->active)
		return;

	/* One SRV service label per transport (RFC 3263 4.1). It joins the
	 * cache identity in the resolver, so two lines on two transports never
	 * serve each other's targets (shared/resolve.h "Cache"). The resolver
	 * blanks the label for every kind that cannot ask SRV, so an address
	 * lookup keeps the same key whatever this hands it. */
	a->dns_query = sip_dns_resolve_svc(&vr.dns, a->cfg.resolve_host,
					   (enum sip_dns_kind)a->cfg.dns_kind,
					   a->cfg.resolve_port, a->line + 1,
					   acct_dns_service(a->cfg.transport));
	if (a->dns_query != SIP_HANDLE_NONE)
		return;

	/* the resolver refused the question itself, for a name it cannot ask or
	 * a query pool with no free slot (shared/resolve.h) */
	dns_retry_arm(a, SIP_DNS_ERR_INTERNAL);
}

static void account_dns_result(struct account *a, const struct sip_dns_result *res)
{
	bool first = !a->reg_inited;
	enum acct_family_result fam = ACCT_FAMILY_STAY;
	bool changed;
	uint32_t ttl;

	if (res->status != SIP_DNS_OK || res->count == 0) {
		dns_retry_arm(a, res->status);
		return;
	}

	/* An answer with targets ends the ladder. A cache hit arrives here the
	 * same way, so a line served from the cache starts again at the first
	 * rung too. */
	dns_retry_reset(a);

	a->dns_answer_srv = res->srv;

	ttl = res->ttl;
	if (ttl < DNS_REFRESH_MIN_S)
		ttl = DNS_REFRESH_MIN_S;
	if (ttl > DNS_REFRESH_MAX_S)
		ttl = DNS_REFRESH_MAX_S;
	uloop_timeout_set(&a->resolve_timer, (int)(ttl * 1000));

	/* The registration client is built before its targets are loaded: its
	 * init clears the target list this answer is about to fill. */
	if (first) {
		/* The first target decides the family, so the local host, the
		 * Via and the Contact below are built in it from the start. */
		account_family_sync(a, &res->target[0].dst,
				    ACCT_FAMILY_REFRESH);

		if (!account_local_host(a, a->family, &res->target[0].dst)) {
			/* The resolution answered, so this waits on the address
			 * of the box and not on DNS: it keeps the fixed delay,
			 * because a first boot whose WAN address is late must
			 * not wait out a minute of the ladder. */
			ULOG_WARN("line %u no local address yet, retrying\n", a->line);
			uloop_timeout_set(&a->resolve_timer, DNS_RETRY_MS);
			return;
		}

		account_reg_start(a);
	}

	/*
	 * The change signal is the line's own target list, not the resolver's
	 * cache flag: the cache flag reports one transition per cache entry,
	 * so a second line served from the shared entry reads it false and
	 * would never react to the same move.
	 */
	changed = acct_targets_load(&a->reg, res);

	/*
	 * The retention notes the loaded list, not the raw answer: the reload
	 * deduplicated it and gave every target its port. It runs before the
	 * ping below, so the round of a changed answer still pings the
	 * departed address one last time and its pinhole starts the window
	 * with a fresh conntrack entry.
	 */
	if (a->cfg.linger_s) {
		uint64_t lnow = acct_now(a);
		unsigned int t, nt = sip_reg_target_count(&a->reg);

		for (t = 0; t < nt; t++)
			acct_linger_note(&a->linger,
					 &sip_reg_target_get(&a->reg, t)->dst,
					 lnow);
	}
	acct_linger_expire(&a->linger, acct_now(a), a->cfg.linger_s);
	acct_linger_rearm(a);

	/* The answer can hand the sticky selection to a target of the other
	 * family, and the line goes with it and registers at once. */
	if (!first)
		fam = account_family_sync(a, acct_target_dst(a),
					  ACCT_FAMILY_REFRESH);

	/* the sticky target exists from here, so this is the first point a
	 * connection under the registration can be recognised at all */
	acct_reg_conn_seed(a);

	/*
	 * A reload that is draining an Expires 0 must not have a REGISTER sent
	 * from here: the line the grace path rebuilds resolves afresh anyway.
	 * The retarget itself also refuses an unregister in progress, but the
	 * gate stays for the ping below and for symmetry with the move gate.
	 *
	 * A line the answer moved is registering already: the move sent the
	 * REGISTER itself and kept the targets, the selection and the Call-ID,
	 * so a retarget on top would put a second REGISTER on the wire. A
	 * selection the sync left in the other family stands down too, the
	 * refused move and the stand-still alike: a retarget would chase that
	 * target over a socket of the line's family into a transport failure
	 * loop, where doing nothing keeps the standing registration.
	 *
	 * The retarget refreshes in place: one Call-ID, a growing CSeq, no
	 * down event, and an exchange in flight completes first (sip/reg.h).
	 */
	if (!first && !a->pending && fam == ACCT_FAMILY_STAY &&
	    acct_target_dst(a) &&
	    acct_dst_family(acct_target_dst(a)) == a->family &&
	    acct_reregister_needed(a->cfg.dns_cache_update_mode, changed)) {
		ULOG_INFO("line %u DNS answer changed, re-registering\n", a->line);
		sip_reg_retarget(&a->reg);
	}

	/*
	 * A conntrack tuple exists only after the box sent on it, so the round
	 * runs now on the two answers that can name a target nothing pinged
	 * yet, the first one and a changed one, instead of one interval later.
	 * It runs behind the REGISTER above so the registrar reads that first.
	 * A line draining its Expires 0 sends nothing more.
	 */
	if ((first || changed) && !a->pending)
		sip_ka_ping_now(&a->ka);

	/*
	 * The proxy addresses of a restricted rule set come from these targets,
	 * so a resolution that moved them has to reach the firewall. Nothing
	 * else runs on this path: the build reconciled before any answer
	 * existed. A no-op reconcile is absorbed by the chain comparison.
	 */
	fw_reconcile();
}

/*
 * One resolver answer handed to a live line, the way dns_done_cb() delivers
 * one. The suite cannot reach the resolver of a running daemon, whose servers
 * come from resolv.conf and whose handle is private to this file, and a literal
 * answer never carries changed, so this is the only way to drive the
 * re-register pair above into a chosen frame. Non-static for test_account.c on
 * the acct_targets_load() precedent; the prototype lives in account.h
 * beside it.
 */
void acct_dns_result_inject(unsigned int line, const struct sip_dns_result *res)
{
	struct account *a;

	if (line >= CFG_CHANNEL_MAX || !res)
		return;

	a = &vr.acct[line];
	if (!a->active)
		return;

	account_dns_result(a, res);
}

/*
 * The resolver of the daemon, read only. The account layer drives two live
 * inputs of it, the AAAA questions and the family the delivery puts first, and
 * neither reaches the status: the status reports the address the preference is
 * derived from, not the preference. Non-static for test_account.c on the
 * acct_dns_result_inject() precedent; the prototype lives in account.h.
 */
const struct sip_dns *acct_dns(void)
{
	return &vr.dns;
}

/*
 * The datagram transport of the daemon, read only. A family move opens a socket
 * in the new family and releases the one the line held, and a send to a
 * destination of another family than the socket is refused and counted; the
 * pool is where both show. Non-static for test_account.c on the acct_dns()
 * precedent; the prototype lives in account.h.
 */
const struct sip_transport *acct_transport(void)
{
	return &vr.tp;
}

/*
 * The same pool, writable, for the one case a reader cannot serve: a socket
 * whose rebind fails needs a configured port that another socket holds, and
 * that port lives in the pool (account.h).
 */
struct sip_transport *acct_transport_test(void)
{
	return &vr.tp;
}

static void resolve_timer_cb(struct uloop_timeout *t)
{
	struct account *a = container_of(t, struct account, resolve_timer);

	account_resolve(a);
}

unsigned int acct_test_dns_retry_ms(unsigned int line)
{
	struct account *a;
	int64_t left;

	if (line >= CFG_CHANNEL_MAX)
		return 0;

	a = &vr.acct[line];
	if (!a->resolve_timer.pending)
		return 0;

	/*
	 * The deadline the timer carries, not the delay the ladder recorded: an
	 * arm that writes the field and sets another delay is the defect this
	 * answers. A timer already due reads 0, which no armed delay does.
	 */
	left = uloop_timeout_remaining64(&a->resolve_timer);

	return left > 0 ? (unsigned int)left : 0;
}

bool acct_test_dns_retry_fire(unsigned int line)
{
	struct account *a;

	if (line >= CFG_CHANNEL_MAX)
		return false;

	a = &vr.acct[line];
	if (!a->resolve_timer.pending)
		return false;

	/* what the uloop dispatch does: clear pending, then run the cb */
	uloop_timeout_cancel(&a->resolve_timer);
	a->resolve_timer.cb(&a->resolve_timer);

	return true;
}

static void dns_done_cb(struct sip_dns *r, uint32_t query, uint32_t owner,
			const struct sip_dns_result *res, void *priv)
{
	unsigned int line = owner ? owner - 1 : 0;
	struct account *a;

	if (line >= CFG_CHANNEL_MAX)
		return;

	a = &vr.acct[line];
	if (!a->active || a->dns_query != query)
		return;

	a->dns_query = SIP_HANDLE_NONE;
	account_dns_result(a, res);
}

/* ----------------------------------------------------- transport dispatch */

/*
 * Whether the account can still take a new call. A line that is unregistering
 * for a reload or a shutdown keeps answering the calls it has, but a new one
 * would be built on a transaction manager, a UA core and a dialog set the
 * teardown is about to destroy.
 */
static bool acct_call_ready(const struct account *a)
{
	return a->active && a->ua_inited && !a->pending && !vr.shutting_down;
}

/* the live binding that the seize, the dial and a new inbound call need */
static bool acct_registered(const struct account *a)
{
	return a->reg_inited && acct_reg_live(sip_reg_state(&a->reg));
}

/*
 * A refusal the UA core builds for us. A response that cannot be built leaves
 * a server transaction with no timer to free it, so the transaction is
 * discarded on failure, exactly as call.c does for the responses it owns
 * (sip/txn.h sip_txn_discard).
 */
static void acct_resp_send(struct account *a, struct sip_txn *txn, const struct sip_msg *req,
			   uint32_t code)
{
	uint32_t handle = sip_txn_handle(txn);
	struct ua_resp r;

	memset(&r, 0, sizeof(r));
	r.code = code;
	if (ua_response_send(&a->ua, txn, req, &r))
		return;

	txn = sip_txn_get(&a->mgr, handle);
	if (txn)
		sip_txn_discard(txn);
}

/* a new INVITE turned away before the call layer, counted with the refusals
 * of the call layer so the line's refused table shows every call it lost */
static void acct_invite_refuse(struct account *a, struct sip_txn *txn,
			       const struct sip_msg *req, uint32_t code)
{
	calls_refused_note(a->line, acct_env_of(a), code);
	acct_resp_send(a, txn, req, code);
}

/*
 * The message summary of one NOTIFY, stored for the line whatever either
 * presentation gate says (proto/proto.h mwi_set). A body that does not parse is
 * answered 200 with nothing stored: the switch is not helped by a refusal it
 * does not act on, and a 4xx earns a retransmission of the same body
 * (sip/sub.h).
 */
static void account_mwi_recv(struct account *a, const struct sip_msg *msg)
{
	struct sip_sub *sub = a->sub_inited ? &a->sub : NULL;
	struct sip_mwi mwi;

	if (sip_sub_notify_recv(sub, msg, &mwi) != SIP_SUB_RX_STORE) {
		ACCT_TRACE("line %u message summary refused, nothing stored",
			   a->line);
		return;
	}

	ACCT_TRACE("line %u message waiting %d, new %u old %u%s", a->line,
		   mwi.waiting, mwi.new_msgs, mwi.old_msgs,
		   mwi.counts_valid ? "" : " (no counts)");
	/* the core stores and presents the record, and the machine of the
	 * line takes the audible choice from the store */
	vr.host->mwi_set(vr.host->ctx, a->line, mwi.waiting, mwi.new_msgs,
			 mwi.old_msgs);
	sip_line_mwi_push(a->line);
}

/*
 * A NOTIFY, routed by its event package and not by a dialog. The message
 * summary arrives unsolicited on every shipped switch (sip/sub.h), so it never
 * matches one, and call.c would answer it 481 from its no-dialog tail. A
 * package nobody here serves is answered 489 (RFC 6665 8.2.1) rather than the
 * same 481, because the two say different things to a switch: 481 invites a
 * fresh subscription, 489 says the package is not served at all.
 *
 * True means the request is finished with.
 */
static bool account_notify_recv(struct account *a, struct sip_txn *txn,
				const struct sip_msg *msg)
{
	switch (sip_sub_event_class(msg)) {
	case SIP_SUB_EV_MWI:
		account_mwi_recv(a, msg);
		acct_resp_send(a, txn, msg, 200);
		return true;
	case SIP_SUB_EV_OTHER:
		acct_resp_send(a, txn, msg, 489);
		return true;
	/*
	 * The implicit subscription of a transfer belongs to a call leg, so
	 * this intake answers none of it. One that matches no dialog is the
	 * tail of a transfer whose leg already ended, and the call layer
	 * answers that 481 rather than the 489 a package nobody serves would
	 * take (call.h, enum call_xfer_kind).
	 */
	case SIP_SUB_EV_REFER:
	case SIP_SUB_EV_NONE:
		break;
	}

	return false;
}

bool acct_invite_replaces(const struct sip_msg *msg)
{
	return msg && msg->is_request && msg->method == SIP_METHOD_INVITE &&
	       msg_hdr_first(msg, SIP_HDR_REPLACES) != NULL;
}

/*
 * A request no transaction matched, in the order sip/ua.h documents:
 * ua_msg_recv() first, so a retransmitted INVITE or ACK never reaches the call
 * layer twice, then the server transaction, the automatic 100, the inbound
 * policy, and only then the admission check and the call layer.
 */
static bool account_request_recv(struct account *a, struct sip_msg *msg,
				 const struct sip_txn_dst *src)
{
	unsigned int slot = 0;
	struct sip_txn *txn;
	struct sip_dialog *d;
	enum ua_policy pol;
	uint32_t owner, target;
	bool new_invite;

	switch (ua_msg_recv(&a->ua, msg)) {
	/* the compiler checks the set; the cases below are the whole of it */
	case UA_RX_ABSORBED:
		return true;
	case UA_RX_ACK:
		call_rx_ack(a->line, msg);
		return true;
	case UA_RX_PASS:
		break;
	}

	/* An ACK that belongs to no accepted 2xx is never answered and never
	 * gets a transaction of its own (RFC 3261 section 17). */
	if (msg->method == SIP_METHOD_ACK)
		return true;

	if (msg->method == SIP_METHOD_CANCEL) {
		/* The target has to be read while the INVITE is still live:
		 * ua_cancel_handle() answers it with 487 (RFC 3261 9.2). */
		target = sip_txn_cancel_target(&a->mgr, msg);
		txn = sip_txn_server_start(&a->mgr, msg, src,
					   acct_owner_make(ACCT_OWNER_UA, a->line + 1));
		if (!txn)
			return true;

		if (ua_cancel_handle(&a->ua, msg, txn, call_txn_local_tag(a->line, target)))
			call_rx_cancel(a->line, target);

		return true;
	}

	d = sip_dialog_find(&a->dialogs, msg);
	new_invite = msg->method == SIP_METHOD_INVITE && !d;

	/*
	 * The owner is fixed when the transaction starts, so a new INVITE
	 * names the slot it is about to fill. Everything else belongs to the
	 * UA core, which needs no upcall of its own.
	 */
	owner = acct_owner_make(ACCT_OWNER_UA, a->line + 1);
	if (new_invite && call_slot_free(a->line, &slot))
		owner = acct_owner_make(ACCT_OWNER_CALL, call_slot_handle(a->line, slot));

	txn = sip_txn_server_start(&a->mgr, msg, src, owner);
	if (!txn)
		return true;

	if (msg->method == SIP_METHOD_INVITE)
		ua_trying_arm(&a->ua, txn);

	pol = ua_request_policy(&a->ua, msg, txn);
	if (pol != UA_POLICY_PASS)
		return true;

	/*
	 * The event packages this daemon serves are answered here, between the
	 * inbound policy and the call layer, because they belong to no call.
	 *
	 * A refer NOTIFY that matches a dialog is the exception and skips the
	 * intake outright: it carries the outcome of a transfer the leg behind
	 * that dialog waits for, and a 489 from here would end that transfer
	 * before the leg ever saw it. The MWI routing is not affected.
	 *
	 * The intake declines the class as well, because a refer NOTIFY that
	 * matches no dialog has to reach the call layer too, for the 481 its
	 * no-dialog tail answers. The two are one rule read from both ends: the
	 * dialog match is what says the request belongs to a leg, whatever the
	 * intake below grows into.
	 */
	if (!(d && sip_sub_event_class(msg) == SIP_SUB_EV_REFER) &&
	    account_notify_recv(a, txn, msg))
		return true;

	/*
	 * Both slots occupied answers 486 before any dialog. A line that is
	 * unregistering for a reload or a shutdown answers 480 instead: it is
	 * not busy, it is going away, and its objects are about to be
	 * destroyed under any leg this INVITE would create.
	 */
	if (new_invite && !acct_call_ready(a)) {
		acct_invite_refuse(a, txn, msg, 480);
		return true;
	}

	/*
	 * A line with no live registration takes no new call.
	 * The answer is the 480 of RFC 3261 21.4.18, a callee that is not
	 * logged in. It sits in front of the 486 below, because the missing
	 * binding is a fact of the account and not of the slots. A Replaces
	 * INVITE passes: it names a call of this line, and call.c admits it
	 * only for a confirmed dialog, as it admits the referral child.
	 */
	if (new_invite && !acct_invite_replaces(msg) && !acct_registered(a)) {
		acct_invite_refuse(a, txn, msg, 480);
		return true;
	}

	/*
	 * A Replaces INVITE is passed through whatever the line holds
	 * (account.h, acct_invite_replaces): the refusal below would hide the
	 * 400, the 481 and the 486 that call.c answers from the match.
	 *
	 * It sits behind the two 480s above. They answer for the account, which
	 * is going away or holds no live registration, rather than for the
	 * caller's request, and that is the order call.c keeps in front of its
	 * own admission.
	 */
	if (new_invite && !call_slot_free(a->line, &slot) &&
	    !acct_invite_replaces(msg)) {
		acct_invite_refuse(a, txn, msg, 486);
		return true;
	}

	call_rx_request(a->line, txn, msg, src);

	return true;
}

/*
 * Which account serves a request no transaction matched. A shared local port
 * gives every line the same socket, so the socket alone cannot name the line:
 * an in-dialog request goes to the dialog that owns it, a CANCEL to the line
 * holding the INVITE it targets, and a new call to the line whose user the
 * request URI names. The socket owner is the last resort, which is also the
 * only answer when the URI names nothing we know.
 */
bool acct_notify_to_user(const struct sip_msg *msg, struct sip_str *user)
{
	struct sip_addr to;

	if (!msg || !msg->is_request || msg->method != SIP_METHOD_NOTIFY)
		return false;
	if (!hdr_addr_get(msg, SIP_HDR_TO, &to) || !to.uri.user.len)
		return false;

	*user = to.uri.user;

	return true;
}

unsigned int acct_user_line(const char *const *users, unsigned int count,
			    struct sip_str user, unsigned int sock_owner)
{
	unsigned int first = ACCT_LINE_NONE;
	unsigned int matches = 0;
	unsigned int i;
	bool owner_match = false;

	for (i = 0; i < count; i++) {
		if (!users[i] || !users[i][0] || !sip_str_eq(user, users[i]))
			continue;

		matches++;
		if (first == ACCT_LINE_NONE)
			first = i;
		if (i == sock_owner)
			owner_match = true;
	}

	if (matches > 1 && owner_match)
		return sock_owner;

	return first;
}

/*
 * sock_owner is the account that owns the socket the datagram arrived on, or
 * NULL for a caller that has none to offer. It only ever decides an ambiguous
 * name (account.h, acct_user_line).
 */
static struct account *account_by_user(struct sip_str user,
				       const struct account *sock_owner)
{
	const char *users[CFG_CHANNEL_MAX];
	unsigned int owner = ACCT_LINE_NONE;
	unsigned int line;
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		const struct account *a = &vr.acct[i];

		users[i] = (a->active && a->ua_inited) ? a->cfg.contact_user : NULL;
		if (sock_owner && a == sock_owner)
			owner = i;
	}

	line = acct_user_line(users, CFG_CHANNEL_MAX, user, owner);

	return line == ACCT_LINE_NONE ? NULL : &vr.acct[line];
}

/*
 * The account the owner hint names, or NULL. The hint is the line the message
 * arrived at (the socket for udp, the connection key for a stream), and it only
 * ever decides an ambiguous name, so a line that is not serving yet is no hint
 * at all.
 */
static struct account *acct_hinted(unsigned int owner_hint)
{
	struct account *a;

	if (owner_hint >= CFG_CHANNEL_MAX)
		return NULL;

	a = &vr.acct[owner_hint];

	return (a->active && a->ua_inited) ? a : NULL;
}

static struct account *account_for_request(const struct sip_msg *msg,
					   unsigned int owner_hint)
{
	struct account *sock_owner = acct_hinted(owner_hint);
	struct account *named;
	struct sip_str user;
	struct sip_uri uri;
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];

		if (!a->active || !a->ua_inited)
			continue;

		if (sip_dialog_find(&a->dialogs, msg))
			return a;
		if (msg->method == SIP_METHOD_CANCEL &&
		    sip_txn_cancel_target(&a->mgr, msg) != SIP_HANDLE_NONE)
			return a;
	}

	/* The request URI keeps the rule it always had: the first line carrying
	 * the name serves it, whatever socket the datagram arrived on. */
	if (uri_parse(msg->ruri, &uri) && uri.user.len) {
		named = account_by_user(uri.user, NULL);
		if (named)
			return named;
	}

	/*
	 * A NOTIFY is addressed to the Contact the registration published, and
	 * a contact URI need not carry the subscriber's user part: several
	 * switches name the subscriber in To alone (account.h,
	 * acct_notify_to_user). Without this a two line device hands every
	 * message summary to whichever line owns the socket, so one mailbox
	 * lights both phones and the other line never hears about its own.
	 *
	 * The socket owner decides an ambiguous name here, because this path
	 * writes a line's store: two lines carrying one user would otherwise
	 * take turns owning every mailbox by table order alone.
	 */
	if (acct_notify_to_user(msg, &user)) {
		named = account_by_user(user, sock_owner);
		if (named)
			return named;
	}

	return sock_owner;
}

/*
 * One intake for both transports. The datagram socket and the stream connection
 * differ only in what names the line a message arrived at, so each callback
 * computes that owner hint in its own vocabulary and the routing below is
 * shared: a transport that answered a message twice, or routed it by another
 * rule, would be a second SIP stack.
 */
static void account_msg_dispatch(struct sip_msg *msg, const struct sip_txn_dst *src,
				 unsigned int owner_hint)
{
	unsigned int i;

	struct account *serve = NULL;
	bool request = false;

	/*
	 * Every manager is scanned first. Accounts may share a local port, so
	 * the arrival does not name the transaction, and a message that belongs
	 * to a client transaction of the second line must never be consumed by
	 * the first. Only what no transaction matched needs an account chosen
	 * for it.
	 */
	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];
		struct sip_txn *txn = NULL;
		enum sip_txn_rx rx;

		if (!a->active)
			continue;

		rx = sip_txn_msg_recv(&a->mgr, msg, &txn);
		if (rx == SIP_RX_ABSORBED || rx == SIP_RX_DELIVERED)
			return;

		if (rx == SIP_RX_REQUEST_NEW || rx == SIP_RX_ACK_2XX)
			request = true;
	}

	if (request) {
		serve = account_for_request(msg, owner_hint);
		if (serve)
			account_request_recv(serve, msg, src);
		return;
	}

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];

		if (!a->active || !a->ua_inited)
			continue;

		/* RFC 6026: a retransmitted 2xx is answered with the stored ACK
		 * before the call layer ever sees it. */
		if (sip_dialog_resp_recv(&a->dialogs, msg) == SIP_DLG_RX_ABSORBED)
			return;

		if (call_rx_stray(a->line, msg))
			return;
	}
}

/* the line whose socket a datagram arrived on, or ACCT_LINE_NONE. A shared
 * local port gives several lines one socket, and the first of them is the hint
 * the routing rules may still overrule. A line that is not serving yet is no
 * hint: it would shadow the line behind it that is. */
static unsigned int acct_sock_line(uint32_t sock)
{
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		const struct account *a = &vr.acct[i];

		if (a->active && a->ua_inited && a->sock == sock)
			return i;
	}

	return ACCT_LINE_NONE;
}

static void tp_msg_cb(struct sip_transport *tp, uint32_t sock, struct sip_msg *msg,
		      const struct sip_txn_dst *src, void *priv)
{
	account_msg_dispatch(msg, src, acct_sock_line(sock));
}

/* the same hint from a stream connection: the line whose local identity the
 * connection key carries (account.h acct_conn_line) */
static unsigned int acct_conn_owner(const struct sip_conn *c)
{
	uint16_t ports[CFG_CHANNEL_MAX];
	const char *ifaces[CFG_CHANNEL_MAX];
	int families[CFG_CHANNEL_MAX];
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		const struct account *a = &vr.acct[i];
		/* a line that is not serving yet is no hint, the rule
		 * acct_sock_line() follows for a shared socket */
		bool serving = a->active && a->ua_inited && acct_stream(&a->cfg);

		ports[i] = serving ? a->cfg.local_port : 0;
		ifaces[i] = serving ? a->env.se.iface : NULL;
		families[i] = serving ? a->family : 0;
	}

	return acct_conn_line(ports, ifaces, families, CFG_CHANNEL_MAX,
			      c->key.local_port, c->key.iface,
			      acct_dst_family(&c->key.peer));
}

static void st_msg_cb(struct sip_stream *st, uint32_t conn, struct sip_msg *msg,
		      const struct sip_txn_dst *src, void *priv)
{
	struct sip_conn *c = sip_stream_conn_get(st, conn);

	account_msg_dispatch(msg, src, c ? acct_conn_owner(c) : ACCT_LINE_NONE);
}

/*
 * The connection serving the line's registration destination, or NULL. It is
 * the reuse decision the send path takes, asked with the SNI this line would
 * send with (sip/stream.h sip_stream_conn_find), so the answer is the very
 * connection its next REGISTER would travel on.
 */
static struct sip_conn *acct_conn_of(const struct account *a)
{
	const struct sip_txn_dst *dst = acct_target_dst(a);
	struct sip_stream_key key;

	if (!acct_stream(&a->cfg) || !dst)
		return NULL;

	if (!sip_stream_env_key(&a->env.se, dst, &key))
		return NULL;

	return sip_stream_conn_find(&vr.st, &key, a->env.se.sni);
}

/*
 * A registration that starts on a live connection never sees the upcall that
 * opened it: the pool is shared, so another line on the same local identity can
 * already hold it up, and a start after a stop can straddle it. Without this
 * seed reg_conn_seen stays false, and the close and reopen that follows looks
 * like the first connection of the registration, which skips the refresh the
 * registrar needs (sip/stream.h "Reconnect re-registers").
 *
 * It seeds an unseen registration only: once an upcall recorded a handle, that
 * handle is the truth, and a seed could only overwrite a genuine reconnect with
 * the connection that replaced it.
 *
 * It runs where the sticky target first exists, which is after the answer is
 * loaded and not in account_reg_start(): the client is built before its targets
 * are, so a seed there could never resolve a connection at all.
 */
static void acct_reg_conn_seed(struct account *a)
{
	struct sip_conn *c;

	if (!a->reg_inited || a->reg_conn_seen)
		return;

	c = acct_conn_of(a);
	if (!c || c->state != SIP_CONN_UP)
		return;

	a->reg_conn = sip_stream_conn_handle(c);
	a->reg_conn_seen = true;
}

/*
 * A connection that comes up toward a peer other than the server of the live
 * binding. The keepalive round pings every resolved target, so an answer that
 * moves the selection opens a connection to the new server while the binding
 * still lives on the old one. That connection is not the registration's, and
 * recording it would make the binding's own connection read as a reconnect.
 * With no binding there is nothing to compare against, so every connection of
 * the line is its own.
 */
static bool acct_conn_off_binding(struct account *a, uint32_t conn)
{
	const struct sip_txn_dst *dst = sip_reg_binding_dst(&a->reg);
	struct sip_stream_key key;
	const struct sip_conn *c;

	if (!dst)
		return false;

	c = sip_stream_conn_get(&vr.st, conn);
	if (!c || !sip_stream_env_key(&a->env.se, dst, &key))
		return false;

	return !sip_stream_key_eq(&c->key, &key);
}

/*
 * Reconnect re-registers (sip/stream.h "Reconnect re-registers"). The
 * registrar's binding names the ephemeral five tuple of the connection that
 * carried the REGISTER, so a connection that died and came back leaves the
 * binding pointing at nothing: the refresh is what teaches the registrar the
 * new one. It is the dns_cache_update_mode address-change shape, for the same
 * reason and with the same two calls.
 *
 * A connection that the peer opened is recorded but forces nothing
 * (account.h acct_reconnect_needed). The record keeps the bookkeeping of a
 * connection that this side opens later: under it or after its close, that
 * one is still a reconnect.
 */
static void acct_reg_conn_up(struct account *a, uint32_t conn, bool inbound)
{
	bool reconnect;

	if (acct_conn_off_binding(a, conn))
		return;

	reconnect = acct_reconnect_needed(a->reg_conn_seen, a->reg_conn, conn,
					  inbound);
	a->reg_conn = conn;
	a->reg_conn_seen = true;

	if (!reconnect || a->pending || vr.shutting_down)
		return;

	/*
	 * The send that opened this connection may be the refresh itself. A
	 * connection reports up before its staged bytes drain, so a REGISTER
	 * queued on it is still in flight here, and a stop and start would
	 * abandon that transaction and put a second REGISTER on the wire. The
	 * in-flight one already carries the five tuple of this connection; if
	 * it travelled on the connection that died instead, its own timer F
	 * ends it and reg.c retries (sip/reg.h), so nothing is lost either way.
	 */
	if (!acct_reconnect_refresh_ok(sip_reg_state(&a->reg)))
		return;

	ULOG_INFO("line %u registration connection re-established, re-registering\n",
		  a->line);
	sip_reg_stop(&a->reg, false);
	sip_reg_start(&a->reg);
}

/*
 * A connection changed state. A close alone forces nothing, because the next
 * send opens one lazily (sip/stream.h "Queued send semantics"); it only clears
 * the handle, so the up that follows is told apart from a repeated report of
 * the same one.
 */
static void st_conn_cb(struct sip_stream *st, uint32_t conn, enum sip_conn_state state,
		       void *priv)
{
	unsigned int i;

	if (state != SIP_CONN_UP && state != SIP_CONN_FREE)
		return;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];
		struct sip_conn *up;

		if (!a->active || !acct_stream(&a->cfg) || !a->reg_inited)
			continue;

		/* the connection is already gone when a close is reported, so
		 * the handle is the only thing left to recognise it by */
		if (state == SIP_CONN_FREE) {
			if (a->reg_conn == conn)
				a->reg_conn = SIP_HANDLE_NONE;
			continue;
		}

		/* resolved per line, because the refresh of an earlier one sends
		 * and a send can take the connection down */
		up = sip_stream_conn_get(st, conn);
		if (!up || acct_conn_of(a) != up)
			continue;

		acct_reg_conn_up(a, conn, up->inbound);
	}
}

/*
 * RFC 5626 4.4.1: a double CRLF is a ping and is answered with a single CRLF.
 * A single CRLF is the pong of a peer and is never answered, or two conforming
 * stacks would keep answering each other forever. The TOS follows the account
 * that owns the socket, because a shared local port carries one byte per send
 * (sip/transport.h "Per-send TOS").
 *
 * It is the udp path alone. A stream answers the ping inside the connection it
 * arrived on and calls nothing back here (sip/stream.h "Ping is answered
 * inside the connection"), which is what makes an unencrypted keepalive
 * impossible on a TLS line.
 */
static void tp_ping_cb(struct sip_transport *tp, uint32_t sock, size_t len,
		       const struct sip_txn_dst *src, void *priv)
{
	char host[SIP_SOCK_HOST_MAX] = "";
	uint8_t tos = 0;
	unsigned int i;

	if (len < SIP_KA_PING_LEN)
		return;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		if (!vr.acct[i].active || vr.acct[i].sock != sock)
			continue;

		tos = vr.acct[i].cfg.sip_tos;
		break;
	}

	sip_sock_send(tp, sock, tos, SIP_KA_PONG, SIP_KA_PONG_LEN, src);

	if (!vr.host->trace_active(vr.host->ctx, TRACE_SIP))
		return;

	sip_dst_host(src, host, sizeof(host));
	vr.host->trace_write(vr.host->ctx, TRACE_SIP,
			     "keepalive ping from %s:%u answered with a pong",
			     host, sip_dst_port(src));
}

/* ----------------------------------------------------------- build/teardown */

/*
 * The device this account binds to right now, measured against the kernel's
 * current netdev set. It is re-read on every bind, because both the tracked
 * l3 device and the netdevs themselves move under a running daemon.
 */
static const char *acct_bind_device_of(const struct account *a)
{
	return vr.host->net_iface_bind_device(vr.host->ctx, a->cfg.iface);
}

/*
 * SO_BINDTODEVICE hard fails the bind when the interface is absent, which is
 * the host live test case where the configured "wan" does not exist. Only
 * scope the socket when the kernel knows the device; the same rule serves the
 * stream listener, which binds the same way (sip/stream.h "Listeners").
 */
static const char *account_bind_device_log(struct account *a)
{
	const char *dev = acct_bind_device_of(a);

	if (!dev && a->cfg.iface[0])
		ULOG_WARN("line %u interface '%s' absent, binding unscoped\n", a->line,
			  a->cfg.iface);
	else if (dev && strcmp(dev, a->cfg.iface))
		ULOG_INFO("line %u interface '%s' binds to device '%s'\n", a->line,
			  a->cfg.iface, dev);

	return dev;
}

/*
 * The inbound half of a stream line. The signalling port is sip_local_port for
 * every transport, so the listener binds exactly what the Via and the Contact
 * carry.
 *
 * A TLS listener with no server context stays down and the line comes up
 * anyway (sip/tls.h "Client only is a normal state"). uvoice registers over
 * TLS and takes no inbound connection until the certificate bundle lands, and
 * accounts_tls_reload() retries the listener when it does. A TCP listener that
 * cannot bind is a failed build, exactly as a failed udp bind is.
 */
static uint32_t acct_listen_take(struct account *a, int family)
{
	struct sip_stream_listen_cfg lc;

	memset(&lc, 0, sizeof(lc));
	lc.addr = "";
	lc.iface = account_bind_device_log(a);
	lc.port = a->cfg.local_port;
	lc.kind = (enum sip_stream_kind)acct_stream_kind(a->cfg.transport);
	/* the family joins the listener identity, so the line takes a listener
	 * of its own family and never the other one's (sip/stream.h) */
	lc.family = family;

	return sip_stream_listen(&vr.st, &lc);
}

/*
 * A listener that did not open is not always a failure: a TLS line with no
 * server context is client only, and it registers and takes no inbound
 * connection until the bundle lands (sip/tls.h).
 */
static bool acct_listen_optional(const struct account *a)
{
	return a->cfg.transport == ACCT_TP_TLS &&
	       !sip_tls_server_ready(&vr.tls);
}

static bool account_listen_open(struct account *a)
{
	a->listener = acct_listen_take(a, a->family);
	if (a->listener != SIP_HANDLE_NONE)
		return true;

	if (acct_listen_optional(a)) {
		ULOG_INFO("line %u tls listener stays down, no server certificate yet: "
			  "the line registers and takes no inbound connection\n", a->line);
		return true;
	}

	ULOG_ERR("line %u listen on %s %s port %u failed: %s\n", a->line,
		 acct_family_name(a->family),
		 acct_transport_name(a->cfg.transport), a->cfg.local_port,
		 strerror(errno));

	return false;
}

static void account_listen_close(struct account *a)
{
	if (a->listener == SIP_HANDLE_NONE)
		return;

	sip_stream_listen_close(&vr.st, a->listener);
	a->listener = SIP_HANDLE_NONE;
}

/*
 * The datagram socket of one line in one family. The family is part of the
 * socket key, so a line that asks for the other family always gets a socket of
 * its own, and two lines that ask for the same key share one (sip/transport.h).
 */
static uint32_t acct_sock_take(struct account *a, int family)
{
	struct sip_sock_cfg sc;

	memset(&sc, 0, sizeof(sc));
	sc.family = family;
	sc.addr = "";
	sc.port = a->cfg.local_port;
	sc.iface = account_bind_device_log(a);

	return sip_sock_open(&vr.tp, &sc);
}

static bool account_socket_open(struct account *a)
{
	/* A stream line opens no datagram socket: its inbound path is the
	 * listener and its outbound one is the connection pool. */
	if (acct_stream(&a->cfg))
		return account_listen_open(a);

	a->sock = acct_sock_take(a, a->family);
	if (a->sock != SIP_HANDLE_NONE)
		return true;

	ULOG_ERR("line %u bind on %s udp port %u failed: %s\n", a->line,
		 acct_family_name(a->family), a->cfg.local_port,
		 strerror(errno));

	return false;
}

static void timers_to_cfg(const struct acct_cfg *c, struct sip_timer_cfg *tc)
{
	memset(tc, 0, sizeof(*tc));
	tc->t1 = c->t1;
	tc->t2 = c->t2;
	tc->t4 = c->t4;
	tc->a = c->ta;
	tc->b = c->tb;
	tc->d = c->td;
	tc->e = c->te;
	tc->f = c->tf;
	tc->g = c->tg;
	tc->h = c->th;
	tc->i = c->ti;
	tc->j = c->tj;
	tc->k = c->tk;
	sip_timer_cfg_fixup(tc);
}

/*
 * The transaction environment of the line, over the transport it configured.
 * sip_stream_env_init() arms SIP_TXN_ENV_RELIABLE itself (sip/stream.h), so the
 * reliable timer set of RFC 3261 comes with the environment and needs no second
 * call here.
 */
static void account_env_init(struct account *a, const char *dev)
{
	const char *sni = acct_sni_of(&a->cfg);
	struct sip_txn_env *env;

	if (acct_stream(&a->cfg)) {
		sip_stream_env_init(&a->env.se, &vr.st,
				    (enum sip_stream_kind)acct_stream_kind(a->cfg.transport),
				    a->cfg.local_port, dev, a->cfg.sip_tos);

		if (sni)
			sip_stream_env_sni_set(&a->env.se, sni);
	} else {
		sip_udp_env_init(&a->env.ue, &vr.tp, a->sock, a->cfg.sip_tos);
	}

	env = acct_env_of(a);
	env->resp = acct_env_resp;
	env->done = acct_env_done;

	/* Before the manager, because sip_txn_rand() seeds its own fallback on
	 * the first call and two accounts share that seed (acct_env_rand32). */
	env->base.rand32 = acct_env_rand32;
}

static bool account_build(struct account *a, const struct acct_cfg *c)
{
	struct sip_timer_cfg tc;
	struct sip_ka_cfg kc;
	struct sip_txn_env *env;

	memset(a, 0, sizeof(*a));
	a->line = c->line;
	a->cfg = *c;
	a->sock = SIP_HANDLE_NONE;
	a->listener = SIP_HANDLE_NONE;
	a->reg_conn = SIP_HANDLE_NONE;
	a->dns_query = SIP_HANDLE_NONE;
	a->resolve_timer.cb = resolve_timer_cb;
	a->grace_timer.cb = account_grace_cb;
	a->linger_timer.cb = linger_timer_cb;

	/* The line starts in the preferred family and follows its registrar
	 * target from the first answer on (account_family_sync). */
	a->family = acct_family_prefer();

	if (!account_socket_open(a))
		return false;

	account_env_init(a, acct_bind_device_of(a));
	env = acct_env_of(a);

	timers_to_cfg(&a->cfg, &tc);
	sip_txn_mgr_init(&a->mgr, env, &tc);

	ua_init(&a->ua, &a->mgr, env, NULL, &account_ua_cb, a);
	sip_dialog_set_init(&a->dialogs, env);
	a->ua_inited = true;

	a->ct_bound_s = vr.host->net_ct_udp_bound_s(vr.host->ctx);
	acct_ka_cfg_make(a, &kc);
	sip_ka_init(&a->ka, &a->mgr, env, acct_owner_make(ACCT_OWNER_KA, a->line + 1),
		    &kc, acct_ka_dst_at, a);
	a->ka_inited = true;
	sip_ka_start(&a->ka);

	a->active = true;

	/*
	 * Reconcile on the activation edge, after the line is active and before
	 * its first resolution. Every caller reconciles again after the build,
	 * and the chain coalescing absorbs the redundant call. A build that
	 * cannot open its socket or listener returns before this point, and the
	 * line stays inactive until a reload.
	 */
	fw_reconcile();
	account_resolve(a);

	return true;
}

/* the local identity of a stream line: what every key it builds carries, and
 * what an accepted connection of its listener carries too (sip/stream.h). The
 * family is part of it: two lines on one port and one device in two families
 * are two identities, and neither owns the other's connections. */
static bool acct_identity_eq(const struct account *a, const struct account *o)
{
	return o->cfg.local_port == a->cfg.local_port &&
	       o->cfg.transport == a->cfg.transport && o->family == a->family &&
	       !strcmp(o->env.se.iface, a->env.se.iface);
}

static bool acct_key_is_local(const struct account *a, const struct sip_stream_key *key)
{
	return key->local_port == a->cfg.local_port &&
	       key->kind == (uint8_t)acct_stream_kind(a->cfg.transport) &&
	       acct_dst_family(&key->peer) == a->family &&
	       !strncmp(key->iface, a->env.se.iface, SIP_SOCK_IFACE_MAX);
}

/*
 * Every pool connection this line's local identity owns, closed when the line
 * goes away or is rebuilt. Not the registration connection alone: an SRV redraw
 * leaves connections to targets that are no longer current, and the listener's
 * accepted connections carry the same local identity, so a single target lookup
 * would walk past both. Outbound connections are never idle closed
 * (sip/stream.h "Connection lifetime"), so anything missed here holds one of
 * the eight slots until the LRU needs it.
 *
 * It runs after every send capable client of the line stopped. The
 * subscription's Expires 0 and the registration's farewell both go out through
 * the pool, and a send with no live connection opens one (sip/stream.h "Queued
 * send semantics"): a sweep before them would close a slot and immediately
 * leave a new one behind. On a connection that is already up with an empty
 * staging buffer those farewell writes reach the kernel inside the send, so the
 * sweep that follows takes nothing away from them; a farewell that had to open
 * a connection is queued, and a close drops those bytes, which is the same
 * trade sip_stream_conn_close() documents and the same one the udp socket close
 * makes.
 *
 * A line whose local identity another active stream line shares keeps every
 * connection: they are that line's inbound path and its registration's
 * outbound one, and the key cannot tell the two lines apart.
 */
static unsigned int acct_conn_sweep(struct account *a)
{
	unsigned int closed = 0;
	unsigned int i;

	a->reg_conn = SIP_HANDLE_NONE;
	a->reg_conn_seen = false;

	if (!acct_stream(&a->cfg))
		return 0;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		const struct account *o = &vr.acct[i];

		if (o != a && o->active && acct_stream(&o->cfg) && acct_identity_eq(a, o))
			return 0;
	}

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		struct sip_conn *c = &vr.st.conn[i];

		if (!c->in_use || !acct_key_is_local(a, &c->key))
			continue;

		sip_stream_conn_close(&vr.st, sip_stream_conn_handle(c));
		closed++;
	}

	return closed;
}

/*
 * Put the address the line had back in the buffer its Via reads. The Contact is
 * not rebuilt from it: the move that failed either never reached the Contact or
 * was refused by it, and a rebuild would replace an address a registrar
 * reported through received and rport with the configured one (sip/reg.h).
 */
static void acct_local_host_restore(struct account *a, const char *host)
{
	str_copy(a->local_host, sizeof(a->local_host), host);
}

/*
 * The local host of the line and the Contact of its registration, moved
 * together to the address of one family. Each request reads its Via host from
 * the buffer and the Contact is rebuilt from it, so a Contact that is refused
 * takes the address back with it: the two must not name two addresses
 * (sip/reg.h). False leaves both as they stood.
 *
 * A local host that did not move leaves the Contact alone. Behind NAT that
 * Contact is the public mapping the registrar reported, and it is the only
 * binding that reaches the line: a Contact rebuilt from the local address
 * would record the public one for removal, and the REGISTER of a flap on an
 * unchanged address would take it away. A family move never reads as
 * unchanged, because its two addresses are of two families.
 */
static bool acct_local_host_move(struct account *a, int family,
				 const struct sip_txn_dst *dst)
{
	char old_host[SIP_SOCK_HOST_MAX];

	str_copy(old_host, sizeof(old_host), a->local_host);

	/* the derive writes the buffer only when it has an answer */
	if (!account_local_host(a, family, dst)) {
		ULOG_WARN("line %u has no %s address to move to\n", a->line,
			  acct_family_name(family));

		return false;
	}

	if (!strcmp(a->local_host, old_host))
		return true;

	if (!a->reg_inited || sip_reg_local_host_set(&a->reg))
		return true;

	acct_local_host_restore(a, old_host);

	return false;
}

/*
 * The transport of the new family, opened while the line still holds the one it
 * has. The family is part of the socket key and of the listener identity, so
 * the two never collide, and two families share one port under IPV6_V6ONLY
 * (sip/transport.h, sip/stream.h). False means the line keeps everything.
 */
static bool acct_transport_take(struct account *a, int family, uint32_t *out)
{
	if (!acct_stream(&a->cfg)) {
		*out = acct_sock_take(a, family);
		if (*out != SIP_HANDLE_NONE)
			return true;

		ULOG_ERR("line %u bind on %s udp port %u failed: %s\n", a->line,
			 acct_family_name(family), a->cfg.local_port,
			 strerror(errno));

		return false;
	}

	*out = acct_listen_take(a, family);
	if (*out != SIP_HANDLE_NONE || acct_listen_optional(a))
		return true;

	ULOG_ERR("line %u listen on %s %s port %u failed: %s\n", a->line,
		 acct_family_name(family),
		 acct_transport_name(a->cfg.transport), a->cfg.local_port,
		 strerror(errno));

	return false;
}

/* the transport a refused move gives back */
static void acct_transport_drop(struct account *a, uint32_t taken)
{
	if (taken == SIP_HANDLE_NONE)
		return;

	if (acct_stream(&a->cfg))
		sip_stream_listen_close(&vr.st, taken);
	else
		sip_sock_close(&vr.tp, taken);
}

/*
 * The line takes the transport it opened and gives up the one it held. A stream
 * line sweeps its connections first, while its identity still names the family
 * it leaves (acct_key_is_local), and the sweep gives up the connection the
 * registration knew, so the next one that comes up is the first of a fresh
 * registration (account_reg_start).
 */
static void acct_transport_swap(struct account *a, uint32_t taken)
{
	if (acct_stream(&a->cfg)) {
		acct_conn_sweep(a);
		account_listen_close(a);
		a->listener = taken;
		return;
	}

	sip_udp_env_sock_set(&a->env.ue, taken);
	sip_sock_close(&vr.tp, a->sock);
	a->sock = taken;
}

/*
 * The move itself. The order is what leaves a refused move harmless: the
 * transport of the new family opens first, beside the one the line holds, and
 * the Contact is the last step that can refuse. Nothing the line shows the far
 * end changes before that. account_family_sync() owns the gate in front of it.
 */
static enum acct_family_result acct_family_move(struct account *a, int family,
						const struct sip_txn_dst *dst,
						enum acct_family_send send)
{
	uint32_t taken = SIP_HANDLE_NONE;
	int old = a->family;

	if (!acct_transport_take(a, family, &taken))
		return ACCT_FAMILY_REFUSED;

	if (!acct_local_host_move(a, family, dst)) {
		acct_transport_drop(a, taken);

		return ACCT_FAMILY_REFUSED;
	}

	/* Every live leg copied the old address into its Contact, its Via and
	 * its HAL media stream, and none of it moves without a re-INVITE. */
	calls_line_teardown(a->line);

	/* The subscription borrows the Contact the registrar holds, so it is
	 * rebuilt on the new one here and starts again on the next 2xx
	 * (reg_registered_cb). */
	if (a->sub_inited)
		account_sub_init(a);

	acct_transport_swap(a, taken);
	a->family = family;

	ULOG_INFO("line %u family %s -> %s\n", a->line, acct_family_name(old),
		  acct_family_name(family));

	/*
	 * The new binding registers at once when the caller asks for it,
	 * whatever dns_cache_update_mode says: the registrar holds a Contact of
	 * the family the line left, and nothing answers on it. Each REGISTER
	 * until the 2xx carries that old Contact with expires=0, so the
	 * registrar drops it (RFC 3261 10.2.2). A move that follows a failed
	 * attempt sends nothing and leaves that REGISTER to the retry timer.
	 */
	if (a->reg_inited && send == ACCT_FAMILY_REFRESH)
		sip_reg_refresh_now(&a->reg);

	return ACCT_FAMILY_MOVED;
}

/*
 * The line follows the family of the registrar target it uses now: its socket
 * or listener, its local host, its Via, its Contact and its media all move
 * together, and one line signals in one family at a time.
 *
 * A refused move leaves the line exactly as it stood: the next REGISTER then
 * leaves for a target of the other family, the transport refuses it and counts
 * it, and the failure penalizes that target, advances to the next one and asks
 * for the move again under the ordinary registration backoff.
 */
static enum acct_family_result
account_family_sync(struct account *a, const struct sip_txn_dst *dst,
		    enum acct_family_send send)
{
	int family = acct_dst_family(dst);

	/*
	 * A proxy that is an IPv6 literal is delivered whatever the operator
	 * gate says (shared/resolve.h "Numeric targets"), and with
	 * ipv6_enabled 0 it stays the target the IPv4 socket refuses and
	 * counts.
	 *
	 * The way back always runs. A gate that closes under a line that
	 * already signals in IPv6, or a backend that loses the feature, leaves
	 * that line on a family the box no longer serves, and only a move to
	 * IPv4 takes it off.
	 */
	if (!acct_family_served(family))
		return ACCT_FAMILY_STAY;

	/*
	 * A line that is draining an Expires 0 is going away: the objects the
	 * move would rewrite are the ones the grace path discards, and the
	 * rebuild behind it reads the preference and resolves afresh anyway.
	 */
	if (a->pending)
		return ACCT_FAMILY_STAY;

	if (!family || family == a->family)
		return ACCT_FAMILY_STAY;

	return acct_family_move(a, family, dst, send);
}

static void account_teardown(struct account *a)
{
	if (!a->active)
		return;

	/* The legs go before the objects they run on (account.h, the view
	 * lifetime rule). It is idempotent: the reload path already did it. */
	calls_line_teardown(a->line);

	/*
	 * The grace timer goes with the line, and the pending action with the
	 * timer. Every other uloop node this account owns is detached by the
	 * done() chain below, the SIP timers included, which are uloop nodes
	 * too through the slot the uloop timer environment keeps in
	 * sip_timer.priv (proto/shared/timer.c). These two
	 * are what the reload path would otherwise leave behind, and
	 * account_build() memsets the whole struct: a timer still linked in
	 * uloop's list is zeroed where it lies, and the list is then broken two
	 * ways. Its neighbours still point at the node, whose own links are now
	 * NULL, so the next insertion walks into them. And its time is {0,0},
	 * which is always due, while its pending flag is false, which makes
	 * uloop_timeout_cancel() return without unlinking it, so a walk that
	 * reaches it takes it for ever. The reload gets there through
	 * account_graceful_replace(): a line whose Expires 0 has completed
	 * carries an armed grace timer and an expiry of 0, so a second reload
	 * takes the immediate branch straight into the memset.
	 * accounts_shutdown_begin() cancels both before the shutdown drain.
	 *
	 * The pending clear answers a path that calls back into
	 * reg_unregistered_cb() from inside this teardown: that callback
	 * re-arms the grace timer whenever a pending action is still set.
	 * Anywhere inside this function, ahead of that memset, would do.
	 */
	uloop_timeout_cancel(&a->resolve_timer);
	uloop_timeout_cancel(&a->grace_timer);
	uloop_timeout_cancel(&a->linger_timer);
	memset(&a->linger, 0, sizeof(a->linger));
	a->pending = false;
	sip_dns_owner_release(&vr.dns, a->line + 1);
	a->dns_query = SIP_HANDLE_NONE;

	/* The subscription unsubscribes while the socket and the registration
	 * it borrows its Contact from are both still alive. */
	account_sub_done(a);

	/* The keepalive goes before the registration: its OPTIONS runs on the
	 * same manager and the same socket, and both die below. */
	if (a->ka_inited) {
		sip_ka_stop(&a->ka);
		sip_ka_done(&a->ka);
		a->ka_inited = false;
	}

	if (a->reg_inited) {
		sip_reg_stop(&a->reg, false);
		sip_reg_done(&a->reg);
		a->reg_inited = false;
	}

	/* Last of the transport steps and after every send capable client
	 * stopped, so nothing reopens a connection behind it (acct_conn_sweep).
	 * A line that never reached a registration still sweeps: its listener
	 * may have accepted connections, and they carry its local identity. */
	acct_conn_sweep(a);

	sip_txn_mgr_done(&a->mgr);

	if (a->ua_inited) {
		ua_done(&a->ua);
		sip_dialog_set_done(&a->dialogs);
		a->ua_inited = false;
	}

	if (a->sock != SIP_HANDLE_NONE) {
		sip_sock_close(&vr.tp, a->sock);
		a->sock = SIP_HANDLE_NONE;
	}

	account_listen_close(a);

	a->active = false;
}

/*
 * The full rebuild behind an interface change, for a line that has nothing to
 * keep or that needs a target it does not have: no granted binding, no resolved
 * target, or a family preference that moved, which only a fresh resolution
 * answers. The registration is dropped locally and nothing is sent for it. The
 * resolve completion builds a new one with a new Call-ID, local host and
 * Contact. A Contact the registrar still holds stays there until it expires;
 * the line that holds a binding moves in place instead and removes it
 * (account_addr_move_inplace).
 */
static void account_reg_rebuild(struct account *a)
{
	/* the subscription borrows the registration's Contact, so it goes
	 * first and account_reg_start() builds a fresh one behind it */
	account_sub_done(a);

	if (a->reg_inited) {
		sip_reg_done(&a->reg);
		a->reg_inited = false;
	}

	uloop_timeout_cancel(&a->resolve_timer);
	/* the address moved under the line, so the question is a new one: a
	 * first boot whose WAN address arrives before its DNS starts again at
	 * the first rung */
	dns_retry_reset(a);
	sip_dns_owner_release(&vr.dns, a->line + 1);
	a->dns_query = SIP_HANDLE_NONE;

	account_resolve(a);
}

/*
 * The address of the line's family moved and the registrar holds a binding of
 * the line. The registration stays: its Call-ID, its targets, their penalties
 * and the sticky selection, with no new resolution. The REGISTER that leaves
 * here carries the new address in its Via and its Contact, and the Contact the
 * registrar holds as a second Contact with expires=0 (RFC 3261 10.2.2), so an
 * inbound call is not forked to an address nothing answers on until the old
 * binding expires. It travels to the registrar, from the new address: the old
 * Contact only has to be named, not reached.
 *
 * An interface that came back on the address it had moves nothing: the Contact
 * stays the one the registrar holds, behind NAT the public one, no removal is
 * recorded, and the REGISTER refreshes the binding through the socket the
 * rebind opened (acct_local_host_move).
 *
 * A stream line takes no connection sweep here, unlike a family move
 * (acct_transport_swap). For an IPv4 change the rebind closed the connections
 * of the device before this runs (netif_addr4_take, acct_stream_unbind), so the
 * BYE of a leg that was torn down, the Expires 0 of the subscription and this
 * REGISTER open their connections from the new address and queue on them, and
 * a sweep would close a connection under the first two. An IPv6 address move
 * runs no such rebind and closes no connection, here or in the full rebuild.
 * A connection that comes up does so while the state reads refreshing, which
 * never reads as a reconnect (acct_reconnect_refresh_ok), so the registrar
 * gets one REGISTER.
 *
 * False means the line has no local host or the Contact does not fit, nothing
 * changed, and the caller rebuilds.
 */
static bool account_addr_move_inplace(struct account *a)
{
	if (!acct_local_host_move(a, a->family, acct_target_dst(a)))
		return false;

	/* the subscription borrows the Contact the registrar holds, so it is
	 * rebuilt on the new one and starts again on the 2xx */
	if (a->sub_inited)
		account_sub_init(a);

	sip_reg_refresh_now(&a->reg);

	return true;
}

static void account_ip_changed(struct account *a, bool force, bool own_family)
{
	if (!a->active || a->pending)
		return;
	/* the address of the line's own family: the other one says nothing
	 * about the Via and the Contact this line carries */
	if (!force && a->reg_inited &&
	    !strcmp(a->local_host, acct_iface_addr_of(a->family)))
		return;

	ULOG_INFO("line %u interface address changed, re-registering\n", a->line);

	/*
	 * Every live leg copied the old address into its Contact, its Via and
	 * its HAL media stream, and none of that can be moved without a
	 * re-INVITE, which this path does not send. The legs are cleared
	 * before the rebind so the FSM does not keep a call the far end can no
	 * longer reach.
	 */
	calls_line_teardown(a->line);

	/*
	 * A binding the registrar holds is what the move in place keeps and
	 * what its removal Contact names, so a line without one, in backoff or
	 * with its resolution pending, has neither and rebuilds. So does a line
	 * the preference left in the wrong family, whatever its address did:
	 * only a fresh resolution gives it a target of the other family.
	 */
	if (own_family && a->reg_inited && sip_reg_expires(&a->reg) &&
	    sip_reg_target_count(&a->reg) && account_addr_move_inplace(a))
		return;

	account_reg_rebuild(a);
}

/*
 * Every active line one event left on the wrong address, put right once.
 * Two reasons, one pass:
 *
 *   - the address of the line's own family moved, and that address is in its
 *     Via, its Contact and its media. A line with a binding moves in place
 *     and removes the old Contact (account_addr_move_inplace);
 *   - the line signals in a family that is no longer the preferred one, and
 *     only a fresh resolution delivers a target of the new one, which the DNS
 *     refresh timer would otherwise do up to an hour later. This reason wins
 *     over the first, because a move in place keeps the targets the line has.
 *
 * One netifd answer can carry both, for both families, so the caller gathers
 * what moved and this runs once at the end of it: two rebuilds of one line in
 * one answer would cancel and restart its resolution twice, and two moves in
 * place would send two REGISTERs.
 *
 * The IPv4 half keeps its own force flag, which is what rebuilds a line whose
 * address came back unchanged after an interface flap or a netdev move; the
 * other reasons always force, because the address a line reads may be the one
 * it already has.
 */
static void acct_family_lines_rebuild(const struct acct_moved *mv)
{
	int want = acct_family_prefer();
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];
		bool addr4 = mv->addr4 && a->family == AF_INET;
		bool addr6 = mv->addr6 && a->family == AF_INET6;
		bool wrong_family = mv->prefer && a->family != want;

		if (!a->active || (!addr4 && !addr6 && !wrong_family))
			continue;

		if (wrong_family)
			ULOG_INFO("line %u signals %s, the preference is %s\n",
				  a->line, acct_family_name(a->family),
				  acct_family_name(want));
		else
			ULOG_INFO("line %u signals %s and that address moved\n",
				  a->line, acct_family_name(a->family));

		account_ip_changed(a, wrong_family || addr6 || mv->force,
				   (addr4 || addr6) && !wrong_family);
	}
}

/*
 * The preferred family the resolver takes now, and whether it is not the one
 * the caller read before it moved the input. Both inputs that move under
 * running lines come through here: the address of the voice netdev and the
 * backend feature mask.
 */
static void acct_prefer_push(int was, struct acct_moved *mv)
{
	int want = acct_family_prefer();

	sip_dns_prefer_family_set(&vr.dns, want);

	if (want != was)
		mv->prefer = true;
}

/*
 * The grace timer fires on the unregistered callback (early) or on the
 * timeout: the old Expires 0 was sent and either answered or given up on, so
 * the old registration is discarded and the pending action runs.
 */
static void account_grace_cb(struct uloop_timeout *t)
{
	struct account *a = container_of(t, struct account, grace_timer);
	struct acct_cfg nc;
	bool remove;

	if (!a->pending)
		return;

	nc = a->pending_cfg;
	remove = a->pending_remove;
	a->pending = false;

	account_teardown(a);
	if (!remove)
		account_build(a, &nc);

	fw_reconcile();
}

/*
 * Unregister the current binding (Expires 0) before the discard, then rebuild
 * on nc, or tear the line down when nc is NULL. With no live binding there is
 * nothing to drain, so it acts at once. Otherwise the grace timer completes it.
 */
static void account_graceful_replace(struct account *a, const struct acct_cfg *nc)
{
	/* The calls go first: their dialogs and transactions run on the objects
	 * this replace is about to discard. */
	calls_line_teardown(a->line);

	if (!a->reg_inited || sip_reg_expires(&a->reg) == 0) {
		account_teardown(a);
		if (nc)
			account_build(a, nc);
		fw_reconcile();
		return;
	}

	if (nc) {
		a->pending_cfg = *nc;
		a->pending_remove = false;
	} else {
		a->pending_remove = true;
	}
	a->pending = true;

	uloop_timeout_cancel(&a->resolve_timer);

	/*
	 * The binding is on its way out, so an answer to a query still in
	 * flight has nothing left to reach: the line is rebuilt on a fresh
	 * resolution or torn down. The queries go here, the pair
	 * account_teardown() runs, instead of up to GRACE_MS later.
	 */
	sip_dns_owner_release(&vr.dns, a->line + 1);
	a->dns_query = SIP_HANDLE_NONE;

	sip_reg_stop(&a->reg, true);
	uloop_timeout_set(&a->grace_timer, GRACE_MS);
	ULOG_INFO("line %u unregistering before %s\n", a->line, nc ? "rebuild" : "removal");
}

static void account_reload_dispatch(struct account *a, const struct acct_cfg *nc,
				    enum acct_reload_op op)
{
	struct sip_timer_cfg tc;
	bool entered;

	switch (op) {
	case ACCT_ROP_NONE:
		return;
	case ACCT_ROP_BUILD:
		account_build(a, nc);
		return;
	case ACCT_ROP_TIMERS:
		entered = !a->cfg.pinhole && nc->pinhole && !acct_stream(nc);
		a->cfg = *nc;
		timers_to_cfg(&a->cfg, &tc);
		sip_txn_mgr_timers_set(&a->mgr, &tc);
		account_ka_reload(a, entered);
		/* a moved dns_linger applies to the standing entries at once;
		 * the reload's own reconcile picks the rule set up */
		acct_linger_expire(&a->linger, acct_now(a), a->cfg.linger_s);
		acct_linger_rearm(a);
		ULOG_INFO("line %u timers and keepalive updated live\n", a->line);
		return;
	case ACCT_ROP_REPLACE:
		account_graceful_replace(a, nc);
		return;
	case ACCT_ROP_REMOVE:
		account_graceful_replace(a, NULL);
		return;
	}
}

/* ------------------------------------------------- local transport recovery */

/*
 * A socket that a failed rebind left down and a stream listener that did not
 * come back both leave a line unable to reach its registrar until the next
 * interface event, which may be hours away. The one recovery timer of the
 * process belongs to the tracker (net/iface.h NET_IFACE_RECOVER_MS), and this
 * layer is its subscriber: the attempt reopens what is down here
 * (acct_iface_recover) and the count of what stays down arms the next one
 * (acct_iface_down_count).
 *
 * net_iface_recover_settle() is the one decision point. Besides the tracker's
 * own paths, two of this layer end there: a certificate bundle that moved the
 * TLS listeners (acct_tls_ctx_load) and the end of a reload. Not the family
 * move or a single line rebuild: neither opens a socket or a listener of its
 * own, and the reload behind them settles. A bind that fails while
 * account_build() runs is not covered either: that line never becomes active,
 * and no pass can reach it.
 */

/*
 * A stream line whose listener is absent. A TLS line without a server context
 * is not one of them: it is client only until the certificate bundle lands and
 * accounts_tls_reload() opens the listener then (sip/tls.h), so a recovery pass
 * that took it would never settle.
 */
static bool acct_listener_down(const struct account *a)
{
	return a->active && acct_stream(&a->cfg) &&
	       a->listener == SIP_HANDLE_NONE && !acct_listen_optional(a);
}

/* the sockets of the pool and the listeners of the lines that are down */
static unsigned int acct_down_count(void)
{
	unsigned int n = sip_transport_down_count(&vr.tp);
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		if (acct_listener_down(&vr.acct[i]))
			n++;
	}

	return n;
}

/* whether the line can send from its own socket, or hold its own listener */
static bool acct_line_transport_up(const struct account *a)
{
	struct sip_sock *sk;

	if (acct_stream(&a->cfg))
		return !acct_listener_down(a);

	sk = sip_sock_get(&vr.tp, a->sock);

	return sk && sip_sock_up(sk);
}

/*
 * Every socket that is down, rebound, and every absent listener, opened again.
 * True when at least one of them came back. Only that answer kicks a line: a
 * plain attempt must leave the backoff ladder of a line with a dead registrar
 * where it is.
 *
 * A working socket is never rebound: the rebind closes the file descriptor and
 * the flow loses its conntrack tuple with it (sip/transport.h).
 */
static bool acct_transport_recover(void)
{
	unsigned int down = sip_transport_down_count(&vr.tp);
	bool back = false;
	unsigned int i;

	if (down && sip_transport_rebind_down(&vr.tp) < down)
		back = true;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];

		if (!acct_listener_down(a))
			continue;

		if (account_listen_open(a) && a->listener != SIP_HANDLE_NONE)
			back = true;
	}

	return back;
}

/*
 * The attempt a failure backoff holds, started for every line whose own
 * transport is back. A line whose socket is still down is left alone: the
 * attempt would spend one step of its ladder and one penalty of its target on
 * a send that cannot leave (sip/reg.h sip_reg_retry_now). That call can run the
 * whole failure path inside itself, the failed callback included, so the loop
 * reads each account again through its own slot.
 */
static void acct_lines_retry_kick(void)
{
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];

		if (!a->active || !a->reg_inited || a->pending)
			continue;
		if (!acct_line_transport_up(a))
			continue;

		sip_reg_retry_now(&a->reg);
	}
}

/* --------------------------------------------------------------- firewall */

/*
 * The resolved registration targets of one line (sip/reg.h). They are the
 * addresses the proxy answers and registers from, so they are what the
 * restricted rule admits. A line that never resolved has none, which is why the
 * reconcile has to run again when a resolution completes. The targets are read
 * in target order and land in the sorted order fw_reconcile_proxy_add() keeps,
 * so the
 * answer order never reaches a rule number.
 *
 * started is the account's reg_inited, and it is not a formality:
 * sip_reg_done() stops the client and leaves the target list in place, so an
 * ungated collect hands a torn down line's proxy addresses to the next rule
 * set. account.h exports the function on the acct_targets_load() precedent, so
 * the suite collects what a scripted answer would put into a rule.
 */
unsigned int acct_proxy_collect(const struct sip_reg *reg, bool started,
				char (*list)[PROTO_FW_SRC_MAX], unsigned int count)
{
	char host[PROTO_FW_SRC_MAX];
	unsigned int i, targets;

	if (!started)
		return count;

	targets = sip_reg_target_count(reg);
	for (i = 0; i < targets; i++) {
		const struct sip_reg_target *tg = sip_reg_target_get(reg, i);

		if (!tg || !sip_dst_host(&tg->dst, host, sizeof(host)))
			continue;

		count = vr.host->fw_proxy_add(vr.host->ctx, list, count, host);
	}

	return count;
}

/*
 * What the running accounts ask of the firewall, handed to the core reconcile
 * (net/fw_reconcile.h), which owns the policy and the RTP rules. It runs on
 * every event that can move the set: a build, the start, a teardown, a
 * reload, a completed resolution and the expiry of a lingering address. An
 * interface change reaches it through the resolution that follows.
 *
 * The lingering addresses fill the slots the current targets left, never the
 * other way round: every line's current proxies land first, so a rotating
 * pool's history cannot push out another line's live rule
 * (acct_linger_collect refuses to evict).
 */
static void fw_reconcile(void)
{
	struct fw_sig_ask ask;
	unsigned int i;

	memset(&ask, 0, sizeof(ask));
	ask.proto_udp = true;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];

		if (!a->active)
			continue;

		ask.active_mask |= 1u << i;
		ask.port[i] = a->cfg.local_port;
		/* fw.c takes one bool for the proto: the transport is base
		 * only, so every active line names the same proto and the
		 * last write is the only write. */
		ask.proto_udp = a->cfg.transport == ACCT_TP_UDP;
		ask.nproxy = acct_proxy_collect(&a->reg, a->reg_inited, ask.proxy,
						ask.nproxy);
	}

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];

		if (a->active && a->reg_inited && a->cfg.linger_s)
			ask.nproxy = acct_linger_collect(&a->linger, ask.proxy,
							 ask.nproxy);
	}

	vr.host->fw_ask(vr.host->ctx, &ask);
}

/* --------------------------------------------------------------- lifecycle */

void accounts_init(void)
{
	struct sip_transport_cb tcb = { .msg = tp_msg_cb, .ping = tp_ping_cb };
	struct sip_stream_cb scb = { .msg = st_msg_cb, .conn = st_conn_cb };
	struct sip_dns_cb dcb = { .done = dns_done_cb, .rand32 = NULL };
	struct sip_dns_cfg dcfg;

	if (vr.inited)
		return;

	sip_transport_init(&vr.tp, &tcb, NULL);

	/* The contexts belong to the caller and the pool refuses every TLS key
	 * while they are empty (sip/stream.h), which is the state before the
	 * configuration names a transport at all. */
	sip_tls_init(&vr.tls);
	sip_stream_init(&vr.st, &scb, NULL, &vr.tls);

	sip_dns_cfg_default(&dcfg);
	dcfg.trace_host = vr.host;
	dcfg.trace_dom = TRACE_SIP;
	sip_dns_init(&vr.dns, &dcfg, &dcb, NULL);
	vr.host->net_iface_subscribe(vr.host->ctx, &acct_iface_sub);
	vr.inited = true;
}

void accounts_host_bind(const struct proto_host *host)
{
	vr.host = host;
}

/*
 * Every TLS connection of the pool. sip/stream.h exposes the pool itself and
 * offers a close by handle, by interface and by all; a close by kind is what a
 * bundle reload needs, because the TCP connections beside them are built on
 * nothing the reload touches.
 */
static unsigned int acct_tls_conns_close(void)
{
	unsigned int closed = 0;
	unsigned int i;

	for (i = 0; i < SIP_STREAM_CONN_MAX; i++) {
		struct sip_conn *c = &vr.st.conn[i];

		if (!c->in_use || c->key.kind != SIP_STREAM_TLS)
			continue;

		sip_stream_conn_close(&vr.st, sip_stream_conn_handle(c));
		closed++;
	}

	return closed;
}

/*
 * The inbound half of every TLS line after a context load.
 *
 * A server context that is gone takes the listeners with it. Without one, every
 * connection the listener accepts fails its handshake at once (sip/stream.h
 * refuses a TLS key with no context), so the socket would answer the SYN, drop
 * the peer and leave the status claiming an inbound service that does not
 * exist. A context that arrived brings up the listener of every TLS line that
 * came up client only, which is what turns a fetched bundle into an inbound
 * service without a reload.
 */
static void acct_tls_listeners_sync(void)
{
	bool ready = sip_tls_server_ready(&vr.tls);
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];

		if (!a->active || a->cfg.transport != ACCT_TP_TLS)
			continue;

		if (!ready) {
			if (a->listener != SIP_HANDLE_NONE)
				ULOG_INFO("line %u tls listener closed, the bundle carries "
					  "no server certificate\n", a->line);
			account_listen_close(a);
			continue;
		}

		if (a->listener == SIP_HANDLE_NONE)
			account_listen_open(a);
	}
}

/*
 * Build the TLS contexts from the fixed bundle paths (sip/tls.h) and the two
 * tls_* options that shape them. A load that replaces contexts closes every
 * TLS connection after the load, the listener sync and the recovery settle.
 * Their session secrets belong to the bundle that is going away
 * (sip_tls_ctx_load, "closed by the caller").
 *
 * A failed load is not fatal here. The client context is the only one that can
 * fail (a verifying client with an unreadable trust store), the sends of a TLS
 * line then fail and the registration retries, and the operator gets the reason
 * once instead of a daemon that refuses to start.
 */
static void acct_tls_ctx_load(bool verify, bool keylog)
{
	struct sip_tls_cfg tc;
	unsigned int closed;
	bool reload = sip_tls_client_ready(&vr.tls);

	memset(&tc, 0, sizeof(tc));
	tc.cert_path = SIP_TLS_CERT_DEFAULT;
	tc.key_path = SIP_TLS_KEY_DEFAULT;
	tc.ca_path = verify ? SIP_TLS_CA_DEFAULT : NULL;
	tc.verify = verify;
	tc.keylog_path = keylog ? SIP_TLS_KEYLOG_DEFAULT : NULL;

	/*
	 * Nothing else creates it. tls.c opens the key log and does not build
	 * a path, so without this directory the default key log path writes
	 * nothing at all. An existing directory is the normal case; any other
	 * failure is reported and the load runs anyway, because a key log is a
	 * debug facility and no reason to keep a line from registering.
	 */
	if (keylog && mkdir(ACCT_KEYLOG_DIR, 0700) && errno != EEXIST)
		ULOG_WARN("could not create %s for the tls key log: %s\n", ACCT_KEYLOG_DIR,
			  strerror(errno));

	if (!sip_tls_ctx_load(&vr.tls, &tc))
		ULOG_ERR("tls contexts not loaded: %s\n", sip_tls_error(&vr.tls));
	else
		ULOG_INFO("tls contexts loaded, verify %u, key log %u, inbound %s\n", verify,
			  keylog, sip_tls_server_ready(&vr.tls) ? "ready" : "client only");

	acct_tls_listeners_sync();

	/* a listener that came back or went down moves the recovery state, and
	 * a bundle that lands is the one thing that opens a TLS listener
	 * outside an interface event */
	vr.host->net_iface_recover_settle(vr.host->ctx);

	if (!reload)
		return;

	closed = acct_tls_conns_close();
	if (closed)
		ULOG_INFO("tls bundle replaced, %u connection(s) closed\n", closed);
}

/* The five tls_* base options are the certificate fetch and the two context
 * shapes; any of them moving asks for the contexts to be built again. */
static bool acct_tls_cfg_changed(const struct cfg *old_cfg, const struct cfg *new_cfg)
{
	return strcmp(old_cfg->tls_cert_path, new_cfg->tls_cert_path) != 0 ||
	       strcmp(old_cfg->tls_cert_username, new_cfg->tls_cert_username) != 0 ||
	       strcmp(old_cfg->tls_cert_password, new_cfg->tls_cert_password) != 0 ||
	       old_cfg->tls_server_verification_enabled !=
		       new_cfg->tls_server_verification_enabled ||
	       old_cfg->tls_keylog_enabled != new_cfg->tls_keylog_enabled;
}

/*
 * Whether this configuration asks for TLS at all. Transport is base only
 * (account.h), so the base token is the whole answer and no channel can add a
 * TLS line behind it.
 */
static bool cfg_tls_wanted(const struct cfg *cfg)
{
	enum acct_transport tp;

	return acct_transport_map(cfg->transport, &tp) && tp == ACCT_TP_TLS;
}

static void acct_tls_take(const struct cfg *cfg, bool moved)
{
	vr.tls_verify = cfg->tls_server_verification_enabled;
	vr.tls_keylog = cfg->tls_keylog_enabled;
	vr.tls_wanted = cfg_tls_wanted(cfg);

	if (vr.tls_wanted)
		vr.host->tls_cert_start(vr.host->ctx, cfg->tls_cert_path,
					cfg->tls_cert_username,
					cfg->tls_cert_password,
					cfg->tls_server_verification_enabled,
					accounts_tls_reload);
	else
		vr.host->tls_cert_stop(vr.host->ctx, PROTO_TLS_STOP_CONFIG);

	/* Load when a TLS line needs the contexts, and again when the options
	 * that shape them moved. A reload that touched neither keeps the
	 * contexts and the connections built on them. */
	if (!vr.tls_wanted)
		return;
	if (!moved && sip_tls_client_ready(&vr.tls))
		return;

	acct_tls_ctx_load(vr.tls_verify, vr.tls_keylog);
}

void accounts_tls_reload(void)
{
	/*
	 * A box that configures no TLS line has nothing to reload and nothing
	 * to serve with it, so a certificate that lands anyway (the helper runs
	 * off tls_cert_path, which a udp line may still carry) builds no
	 * contexts and reads no files.
	 */
	if (!vr.inited || !vr.started || !vr.tls_wanted)
		return;

	/* The listeners follow the load itself: a bundle with a server
	 * certificate brings them up, one without takes them down
	 * (acct_tls_listeners_sync). */
	acct_tls_ctx_load(vr.tls_verify, vr.tls_keylog);
}

void accounts_hal_features_set(uint32_t features)
{
	struct acct_moved mv = { 0 };
	int was = acct_family_prefer();

	if (vr.hal_features == features)
		return;

	vr.hal_features = features;
	acct_served_push();

	/* Before the first build there is nothing to push to and no line to
	 * move: account_build() reads the preference itself. */
	if (!vr.started)
		return;

	/* The shell pushes the mask again when the DSP reports the features it
	 * really carries, and the lines are up by then, so a mask that opens
	 * IPv6 media moves them the way an arriving address does. */
	acct_prefer_push(was, &mv);
	acct_family_lines_rebuild(&mv);
}

void accounts_start(const struct cfg *cfg)
{
	unsigned int i;

	if (!vr.inited)
		accounts_init();

	/* Before the first build: a TLS listener needs the server context, and
	 * the first REGISTER needs the client one. */
	acct_tls_take(cfg, true);

	/*
	 * The two live inputs of the resolver, before the first build: the
	 * operator gate turns the AAAA questions on, and the address the netdev
	 * holds says which family the delivery puts first. The tracker asked
	 * netifd for that address before the protocol started (net_iface_init),
	 * so the first sockets scope to the netdev it named.
	 */
	vr.ipv6 = cfg->ipv6_enabled;
	acct_served_push();
	sip_dns_ipv6_set(&vr.dns, vr.ipv6);
	sip_dns_prefer_family_set(&vr.dns, acct_family_prefer());

	if (cfg->enabled) {
		for (i = 0; i < CFG_CHANNEL_MAX; i++) {
			const struct cfg_channel *ch = &cfg->channel[i];
			struct acct_cfg c;

			if (!ch->present || i >= cfg->channels)
				continue;

			acct_cfg_derive(cfg, ch, &c);
			if (!c.enabled) {
				if (ch->enabled)
					ULOG_WARN("line %u enabled but not registrable "
						  "(missing proxy or user)\n", i);
				continue;
			}

			account_build(&vr.acct[i], &c);
		}
	}

	fw_reconcile();
	vr.started = true;
}

static void accounts_teardown_all(void)
{
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++)
		account_teardown(&vr.acct[i]);
}

/*
 * The base options whose move rebuilds every line. The schema RESTART_ALL class
 * is a proposal with no reader, so this comparison is the mechanism that makes
 * the class real, next to the config swap the next call reads.
 *
 * It is non-static so test_account.c pins the set without a live account; the
 * prototype lives in account.h beside acct_reload_action().
 */
bool acct_base_restart(const struct cfg *old_cfg, const struct cfg *new_cfg)
{
	return old_cfg->enabled != new_cfg->enabled ||
	       old_cfg->channels != new_cfg->channels ||
	       strcmp(old_cfg->local_interface, new_cfg->local_interface) != 0 ||
	       strcmp(old_cfg->transport, new_cfg->transport) != 0 ||
	       old_cfg->srtp_enabled != new_cfg->srtp_enabled ||
	       old_cfg->ipv6_enabled != new_cfg->ipv6_enabled ||
	       acct_tls_cfg_changed(old_cfg, new_cfg);
}

void accounts_reload(const struct cfg *old_cfg, const struct cfg *new_cfg)
{
	bool base_restart;
	unsigned int i;

	if (vr.shutting_down)
		return;

	/*
	 * A reload that beats accounts_start() has nothing to reload, and building
	 * here would leave accounts_start() to build the same lines a second time.
	 * The start reads the config main.c holds, which is the one this reload
	 * just loaded, so nothing is lost by refusing.
	 */
	if (!vr.started)
		return;

	base_restart = acct_base_restart(old_cfg, new_cfg);

	/* Before the lines: a line this reload rebuilds opens its listener on
	 * the contexts, and a bundle change closes the connections built on the
	 * old one first. */
	acct_tls_take(new_cfg, acct_tls_cfg_changed(old_cfg, new_cfg));

	/*
	 * rtp_local_port_min/max is RESTART_LINE in the schema, but the range is
	 * not part of struct acct_cfg, so acct_apply_class() never sees it. A
	 * live leg holds a port from the old range and the new rule would not
	 * cover it, so the line is cleared first, the same way the account
	 * replace path clears it before it discards the account objects. The
	 * port pool rebases itself once it drains (proto/proto.h
	 * net_port_alloc: a range is only adopted while no port is out), so the
	 * next call allocates from the new range the new rule opens. The core
	 * took the new ranges for the firewall before this reload, and a moved
	 * voice interface is the tracker's.
	 */
	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		if (!vr.acct[i].active)
			continue;
		if (vr.host->net_port_range_eq(vr.host->ctx,
					       &old_cfg->channel[i],
					       &new_cfg->channel[i]))
			continue;

		ULOG_INFO("line %u RTP range moved, clearing its calls\n", i);
		calls_line_teardown(i);
	}

	/*
	 * The resolver's two live inputs, before the loop below, so every query
	 * the rebuild starts asks the new question set. A query that is already
	 * in flight is the resolver's own: it finishes under the set of the
	 * moment (shared/resolve.h, sip_dns_ipv6_set), and the line its answer
	 * reaches is either rebuilt behind it or discarded with its targets.
	 */
	vr.ipv6 = new_cfg->ipv6_enabled;
	acct_served_push();
	sip_dns_ipv6_set(&vr.dns, vr.ipv6);
	sip_dns_prefer_family_set(&vr.dns, acct_family_prefer());

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];
		const struct cfg_channel *ch = &new_cfg->channel[i];
		struct acct_cfg nc;
		enum acct_reload_op op;
		enum acct_apply cls = ACCT_APPLY_NONE;
		bool want;

		/* An operator changed the configuration, so every line asks its
		 * question again from the first rung, the line a reload leaves
		 * in place included. */
		dns_retry_reset(a);

		if (ch->present && i < new_cfg->channels)
			acct_cfg_derive(new_cfg, ch, &nc);
		else
			memset(&nc, 0, sizeof(nc));

		want = ch->present && i < new_cfg->channels && nc.enabled;

		if (a->active && want)
			cls = acct_apply_class(&a->cfg, &nc);

		op = acct_reload_action(a->active, want, cls);

		/* A base change (interface, transport, srtp, enable, channels)
		 * rebuilds every active line even when its own fields did not
		 * move. */
		if (base_restart && a->active && want)
			op = ACCT_ROP_REPLACE;

		account_reload_dispatch(a, &nc, op);
	}

	fw_reconcile();

	/* the lines the loop rebuilt took their sockets and listeners again,
	 * so what is down now is what the recovery owns */
	vr.host->net_iface_recover_settle(vr.host->ctx);
}

void accounts_shutdown_begin(void)
{
	unsigned int i;

	/* the drain owns the sockets from here on: the tracker went quiet in
	 * front of it (net_iface_quiesce), so no attempt rebinds one under the
	 * unregister that is leaving */
	vr.shutting_down = true;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];

		if (!a->active)
			continue;

		/* Clear the calls before the binding: a BYE needs the socket
		 * and the transaction manager the drain is about to close. */
		calls_line_teardown(a->line);

		/* Nothing the keepalive owes anybody survives the drain, so it
		 * stops before it: no ping leaves during the window, and no
		 * upcall of its own arrives while the teardown runs. It stops
		 * even for a line that never registered, because its timers run
		 * from the build. account_teardown() owns the sip_ka_done. */
		if (a->ka_inited)
			sip_ka_stop(&a->ka);

		if (!a->reg_inited)
			continue;

		/* Cancel any in-flight reload teardown so its grace timer cannot
		 * rebuild a line during the shutdown drain. */
		uloop_timeout_cancel(&a->resolve_timer);
		uloop_timeout_cancel(&a->grace_timer);
		uloop_timeout_cancel(&a->linger_timer);
		a->pending = false;

		a->unreg_pending = true;
		sip_reg_stop(&a->reg, true);
	}
}

bool accounts_shutdown_pending(void)
{
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		if (vr.acct[i].unreg_pending)
			return true;
	}

	return false;
}

void accounts_done(void)
{
	vr.host->net_iface_subscribe(vr.host->ctx, NULL);
	accounts_teardown_all();

	vr.ipv6 = false;

	if (!vr.inited)
		return;

	sip_dns_done(&vr.dns);

	/* the pool before the contexts it holds, and both after the accounts
	 * that sent through them */
	sip_stream_close_all(&vr.st);
	sip_stream_done(&vr.st);
	sip_tls_done(&vr.tls);

	sip_transport_done(&vr.tp);
	vr.inited = false;
}

/* --------------------------------------------------------------- status */

/*
 * The state of the line's SUBSCRIBE machine, beside its registration keys.
 *
 * The key is present only for a line that runs one. An absent key says the
 * line never subscribes, which is what a status consumer reads it for: the
 * unsolicited NOTIFY intake is the primary message waiting path (sip/sub.h)
 * and is not a subscription. The value is the sip/sub.h name verbatim, so the
 * daemon and its consumers keep one vocabulary, and "off" is a machine that is
 * armed and not started yet, which is every line before its first REGISTER
 * succeeds.
 *
 * It is non-static so test_account.c pins the presence rule without a live
 * account; the prototype lives in account.h beside it.
 */
void acct_sub_status_dump(struct blob_buf *b, bool subscribe, const struct sip_sub *sub)
{
	if (!subscribe || !sub)
		return;

	blobmsg_add_string(b, "mwi_subscription",
			   sip_sub_status_name(sip_sub_status_get(sub)));
}

void acct_conn_status_dump(struct blob_buf *b, enum acct_transport transport,
			   const struct sip_conn *conn, bool listener, const char *tls_err)
{
	char peer[SIP_SOCK_HOST_MAX] = "";
	void *tbl;

	blobmsg_add_string(b, "transport", acct_transport_name(transport));

	if (transport == ACCT_TP_UDP)
		return;

	/* Every stream transport reports it, not tls alone: a tcp listener that
	 * did not come back after an interface move is down too, and this key
	 * is the only place it shows. */
	blobmsg_add_u8(b, "listener", listener);

	if (transport == ACCT_TP_TLS && tls_err && tls_err[0])
		blobmsg_add_string(b, "tls_error", tls_err);

	if (!conn)
		return;

	tbl = blobmsg_open_table(b, "connection");
	blobmsg_add_string(b, "state", sip_conn_state_name(conn->state));

	if (sip_dst_host(&conn->key.peer, peer, sizeof(peer))) {
		char text[SIP_SOCK_HOST_MAX + 8];

		snprintf(text, sizeof(text), "%s:%u", peer,
			 sip_dst_port(&conn->key.peer));
		blobmsg_add_string(b, "peer", text);
	}

	blobmsg_add_u32(b, "queued", (uint32_t)sip_stream_conn_queued(conn));
	blobmsg_close_table(b, tbl);
}

/*
 * The transport keys of one live line. The error the status carries is the
 * connection's when there is one, because a handshake failure is what a support
 * case asks about.
 *
 * The context error is only a fault when the client context failed. sip/tls.h
 * records a reason for the server half whenever there is no certificate pair
 * to build it from, and that is the normal client-only state (sip/tls.h
 * "Client only is a normal state"), not a failure: the load returned true and
 * the line registers. A published residue would put a red error on every line
 * that only waits for its bundle.
 */
static void account_conn_dump(struct account *a, struct blob_buf *b)
{
	const struct sip_conn *c = acct_conn_of(a);
	const char *err = "";

	if (a->cfg.transport == ACCT_TP_TLS) {
		if (c && sip_tls_conn_error(&c->tls)[0])
			err = sip_tls_conn_error(&c->tls);
		else if (!sip_tls_client_ready(&vr.tls))
			err = sip_tls_error(&vr.tls);
	}

	acct_conn_status_dump(b, a->cfg.transport, c, a->listener != SIP_HANDLE_NONE, err);
}

static void account_status_dump(struct account *a, struct blob_buf *b)
{
	enum sip_reg_state st;
	const struct sip_reg_target *tg;
	char host[SIP_SOCK_HOST_MAX] = "";

	blobmsg_add_u32(b, "line", a->line);
	blobmsg_add_string(b, "aor", a->cfg.aor);
	blobmsg_add_string(b, "resolve_host", a->cfg.resolve_host);

	/* The question the line asks. It stands in front of the registration
	 * half below for the reason family does: a line that is still
	 * resolving has one already. */
	blobmsg_add_string(b, "dns_lookup",
			   sip_dns_kind_name((enum sip_dns_kind)a->cfg.dns_kind));
	blobmsg_add_u32(b, "local_port", a->cfg.local_port);

	/* A line has a family from its build on, so this key stands in front of
	 * the registration half below, which a line without one omits. */
	blobmsg_add_string(b, "family", acct_family_name(a->family));
	account_conn_dump(a, b);

	if (!a->reg_inited) {
		blobmsg_add_string(b, "state", "resolving");
		blobmsg_add_string(b, "status", "trying");
		return;
	}

	st = sip_reg_state(&a->reg);
	blobmsg_add_string(b, "state", sip_reg_state_name(st));
	blobmsg_add_string(b, "status", acct_status_name(st));
	blobmsg_add_u32(b, "expires", sip_reg_expires(&a->reg));
	blobmsg_add_u32(b, "last_code", a->last_code);
	blobmsg_add_string(b, "contact", sip_reg_contact(&a->reg));
	blobmsg_add_u32(b, "targets", sip_reg_target_count(&a->reg));

	/* The answer the line got, which an SRV question and its address
	 * fallback tell apart. It belongs to the registration half: no answer
	 * exists before the registration is built. */
	blobmsg_add_string(b, "dns_answer", a->dns_answer_srv ? "srv" : "addr");

	/* Both lists follow the omit-empty rule of net_iface_status_dump(): a
	 * key is present only while it holds something to say. */
	if (sip_reg_target_count(&a->reg)) {
		void *arr = blobmsg_open_array(b, "resolved");
		unsigned int i, n = sip_reg_target_count(&a->reg);

		for (i = 0; i < n; i++) {
			const struct sip_reg_target *rt =
				sip_reg_target_get(&a->reg, i);
			void *entry;

			if (!rt || !sip_dst_host(&rt->dst, host, sizeof(host)))
				continue;
			entry = blobmsg_open_table(b, NULL);
			blobmsg_add_string(b, "host", host);
			blobmsg_add_u32(b, "port", sip_dst_port(&rt->dst));
			blobmsg_close_table(b, entry);
		}
		blobmsg_close_array(b, arr);
	}

	if (a->linger.count) {
		void *arr = blobmsg_open_array(b, "linger");
		uint64_t now = acct_now(a);
		unsigned int i;

		for (i = 0; i < a->linger.count; i++) {
			const struct acct_linger_entry *e = &a->linger.entry[i];
			void *entry;

			if (!sip_dst_host(&e->dst, host, sizeof(host)))
				continue;
			entry = blobmsg_open_table(b, NULL);
			blobmsg_add_string(b, "host", host);
			blobmsg_add_u32(b, "port", sip_dst_port(&e->dst));
			blobmsg_add_u32(b, "age",
					(uint32_t)((now - e->last_seen_ms) / 1000));
			blobmsg_close_table(b, entry);
		}
		blobmsg_close_array(b, arr);
	}

	if (a->refresh_at_ms) {
		uint64_t now = acct_now(a);
		uint32_t rem = a->refresh_at_ms > now ?
			       (uint32_t)((a->refresh_at_ms - now) / 1000) : 0;

		blobmsg_add_u32(b, "refresh_in", rem);
	}

	tg = sip_reg_target_get(&a->reg, sip_reg_target_current(&a->reg));
	if (tg && sip_dst_host(&tg->dst, host, sizeof(host))) {
		blobmsg_add_string(b, "registrar", host);
		blobmsg_add_u32(b, "registrar_port", sip_dst_port(&tg->dst));
	}

	acct_sub_status_dump(b, a->cfg.mwi_subscribe, a->sub_inited ? &a->sub : NULL);
}

static void account_ka_dump(struct account *a, struct blob_buf *b)
{
	const struct sip_ka_stat *st = &a->ka.stat;
	void *tbl;

	if (!a->ka_inited)
		return;

	tbl = blobmsg_open_table(b, "keepalive");
	blobmsg_add_u64(b, "crlf_sent", st->crlf_sent);
	blobmsg_add_u64(b, "crlf_skipped", st->crlf_skipped);
	blobmsg_add_u64(b, "crlf_failed", st->crlf_failed);
	blobmsg_add_u64(b, "options_sent", st->options_sent);
	blobmsg_add_u64(b, "options_alive", st->options_alive);
	blobmsg_add_u64(b, "options_failed", st->options_failed);
	blobmsg_add_u64(b, "options_skipped", st->options_skipped);
	/* the effective CRLF cadence and the conntrack bound it runs under,
	 * so a bench read needs no procfs cross-check */
	blobmsg_add_u32(b, "interval", a->ka.cfg.crlf_interval_s);
	blobmsg_add_u32(b, "ct_bound", a->ct_bound_s);
	blobmsg_close_table(b, tbl);
}

void accounts_status_dump(struct blob_buf *b, int want_line)
{
	bool all_lines = want_line < 0;
	void *arr, *entry;
	unsigned int i;

	arr = blobmsg_open_array(b, "lines");
	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];

		if (!all_lines && i != (unsigned int)want_line)
			continue;
		if (!a->active)
			continue;

		entry = blobmsg_open_table(b, NULL);
		account_status_dump(a, b);
		account_ka_dump(a, b);
		blobmsg_close_table(b, entry);
	}
	blobmsg_close_array(b, arr);
}

bool account_register_cmd(unsigned int line)
{
	struct account *a;

	if (line >= CFG_CHANNEL_MAX || !vr.started || vr.shutting_down)
		return false;

	a = &vr.acct[line];
	if (!a->active || a->pending)
		return false;

	if (a->reg_inited)
		sip_reg_start(&a->reg);
	else
		account_resolve(a);

	return true;
}

bool account_unregister_cmd(unsigned int line)
{
	struct account *a;

	if (line >= CFG_CHANNEL_MAX || !vr.started || vr.shutting_down)
		return false;

	a = &vr.acct[line];
	if (!a->active || !a->reg_inited || a->pending)
		return false;

	sip_reg_stop(&a->reg, true);

	return true;
}

/* ------------------------------------- the call interface of account.c */

/*
 * The address of record is "sip:user@domain", so the domain the call layer
 * puts in a request URI and in the From is the part behind the '@'. A bare
 * "sip:host" address of record has no user part and is its own domain.
 */
static const char *acct_domain_of(const struct account *a)
{
	const char *at = strchr(a->cfg.aor, '@');

	if (at)
		return at + 1;

	return strncmp(a->cfg.aor, "sip:", 4) ? a->cfg.aor : a->cfg.aor + 4;
}

bool account_call_view(unsigned int line, struct acct_call_view *out)
{
	const struct sip_reg_target *tg;
	struct account *a;

	if (line >= CFG_CHANNEL_MAX || !out)
		return false;

	a = &vr.acct[line];
	if (!a->active || !a->ua_inited)
		return false;

	memset(out, 0, sizeof(*out));
	out->mgr = &a->mgr;
	out->env = acct_env_of(a);
	out->ua = &a->ua;
	out->dialogs = &a->dialogs;
	out->local_host = a->local_host;
	out->domain = acct_domain_of(a);
	out->user = a->cfg.contact_user;
	out->display = a->cfg.display;
	out->auth_user = a->cfg.auth_user;
	out->auth_pass = a->cfg.auth_pass;
	out->user_agent = ACCT_USER_AGENT;
	out->transport = acct_transport_token(a->cfg.transport);
	/* One resolver for both forks. The media fork runs
	 * net_iface_bind_device() again here instead of reading what the
	 * sockets bound to, so both forks answer from one rule and a view
	 * taken after a device-only netifd answer already names the new
	 * netdev. */
	out->bind_dev = acct_bind_device_of(a);
	out->punch_interval_ms = vr.host->net_ct_punch_interval_ms(vr.host->ctx);
	out->local_port = a->cfg.local_port;
	out->active = a->active && !a->pending && !vr.shutting_down;
	out->registered = acct_registered(a);

	/* the sticky registrar selection of the moment (sip/reg.h) */
	tg = a->reg_inited ? sip_reg_target_get(&a->reg, sip_reg_target_current(&a->reg)) : NULL;
	if (tg) {
		out->dst = &tg->dst;
		out->dst_valid = true;
		out->service_route = acct_service_route_of(a, &tg->dst);
	}

	return true;
}

/* ------------------------------------------- the subscriber of the tracker */

/*
 * The address family the resolver puts first, for an IPv6 address of the
 * voice netdev given as addr6. IPv6 needs three things at once.
 *
 * The operator gate, which is what ipv6_enabled says. An address of that family
 * on the voice netdev, because a target the box cannot send to is no target at
 * all. And a backend that carries IPv6 media: the media of a call follows the
 * family of its signaling (RFC 6157 section 5), so a line that registered over
 * IPv6 on a backend without HAL_FEAT_IPV6_MEDIA would answer every call with
 * congestion.
 */
static int acct_family_prefer_of(const char *addr6)
{
	if (addr6 && addr6[0] && acct_family_served(AF_INET6))
		return AF_INET6;

	return AF_INET;
}

static int acct_family_prefer(void)
{
	return acct_family_prefer_of(
		vr.host->net_iface_state_get(vr.host->ctx)->addr6);
}

/*
 * The two gates of acct_family_served() as the tracker reads them: it clears
 * the down flag of an interface that came back on IPv6 alone only while a line
 * may signal in IPv6. Every writer of either gate pushes it here.
 */
static void acct_served_push(void)
{
	vr.host->net_iface_ipv6_served_set(vr.host->ctx,
					   acct_family_served(AF_INET6));
}

/*
 * The stream half of the move. A connection and a listener are both scoped to
 * the device the key names, so both go with it: the connections of the old
 * device close (the next send opens one lazily, sip/stream.h "Queued send
 * semantics"), the listener is released, and the environment takes the new
 * local identity. Without the environment update every key the line builds
 * afterwards would still name the dead netdev, and every send would open a
 * connection scoped to it.
 *
 * It is the first of two phases, and the split is not cosmetic. Two lines share
 * one refcounted listener on one port, so a release and re-take per line puts
 * the new socket's bind in front of the old socket, which the other line still
 * holds open. Two listening sockets on one port need distinct nonzero
 * SO_BINDTODEVICE scopes to coexist, and SO_REUSEPORT is deliberately off
 * (sip/stream.h), so that bind fails with EADDRINUSE and the line loses its
 * inbound path until the next reload. A release of every listener first, and a
 * take afterwards, leaves no old socket in the way.
 */
static void acct_stream_unbind(struct account *a, const char *dev)
{
	sip_stream_close_iface(&vr.st, a->env.se.iface);
	account_listen_close(a);
	sip_stream_env_local_set(&a->env.se, a->cfg.local_port, dev);
	a->reg_conn = SIP_HANDLE_NONE;
}

/*
 * Re-derive the device of every live socket, just before the rebind that is
 * about to reopen it. A rebind reuses the device the socket was opened with
 * (sip/transport.h), so a netifd move of the logical interface onto another
 * netdev would rebind every socket onto the dead one and keep it there until
 * the daemon restarted: a permanent signaling outage on a wan failover.
 */
static void acct_bind_devices_refresh(void)
{
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];
		const char *dev;

		if (!a->active)
			continue;

		dev = acct_bind_device_of(a);

		if (acct_stream(&a->cfg)) {
			acct_stream_unbind(a, dev);
			continue;
		}

		if (a->sock == SIP_HANDLE_NONE || sip_sock_iface_set(&vr.tp, a->sock, dev))
			continue;

		/* Only a dead handle refuses: both names come from bounded
		 * fields the transport accepts. The socket keeps its device. */
		ULOG_WARN("line %u could not take device '%s'\n", a->line,
			  dev ? dev : "(none)");
	}

	/* Phase 2, once every old listener is gone (acct_stream_unbind). */
	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		struct account *a = &vr.acct[i];

		if (!a->active || !acct_stream(&a->cfg))
			continue;

		if (!account_listen_open(a))
			ULOG_WARN("line %u listener did not come back on device '%s'\n",
				  a->line, a->env.se.iface[0] ? a->env.se.iface : "(none)");
	}
}

/*
 * The rebind an IPv4 move asks for: the address every Via and Contact of an
 * IPv4 line carries moved, it came back after an ifdown, or the netdev moved.
 *
 * A socket the rebind could not reopen is down and no send of it leaves
 * (sip/transport.h). The recovery pass at the end of the tracker event rebinds
 * it again and keeps trying, so the count is news for the operator and not an
 * error path here.
 *
 * The rebind put every datagram socket on the new address, so no conntrack
 * entry covers the tuples the lines used: the round runs now instead of one
 * interval later. A line that registers again spends one four byte datagram
 * for nothing.
 */
static void acct_addr4_rebind(const struct net_iface_state *st)
{
	unsigned int failed;

	acct_bind_devices_refresh();

	failed = sip_transport_rebind_all(&vr.tp);
	if (failed)
		ULOG_WARN("voice interface %s: %u socket(s) stayed down after the rebind\n",
			  st->iface, failed);

	acct_udp_lines_ping();
}

/*
 * One tracker event (net/iface.h). The IPv4 half rebinds the sockets, and only
 * a line that signals in IPv4 follows that address; a line that signals in
 * IPv6 gets its socket reopened by the rebind and its stream connection on the
 * next send, which is the whole of what the change costs it. A move of the
 * IPv6 address moves the family the resolver delivers first. Every line is
 * then put right once (acct_family_lines_rebuild).
 *
 * A move of the served gate alone is this layer's own push, and the two
 * writers handle it where they make it: accounts_hal_features_set() moves the
 * lines, and a reload rebuilds them anyway (acct_base_restart).
 */
static void acct_iface_changed(void *priv, const struct net_iface_state *st,
			       const struct net_iface_change *ch)
{
	struct acct_moved mv = { 0 };
	int was = acct_family_prefer_of(ch->addr6_old);

	if (!vr.started || vr.shutting_down)
		return;

	if (ch->addr4) {
		acct_addr4_rebind(st);
		mv.addr4 = true;
		mv.force = ch->force;
	}

	if (ch->addr6) {
		mv.addr6 = true;
		acct_prefer_push(was, &mv);
	}

	acct_family_lines_rebuild(&mv);
}

/* The recovery attempt of the tracker: what came back kicks the lines it
 * freed (net_iface_recover_settle). */
static void acct_iface_recover(void *priv)
{
	if (acct_transport_recover())
		acct_lines_retry_kick();
}

static unsigned int acct_iface_down_count(void *priv)
{
	return acct_down_count();
}

static const struct net_iface_sub acct_iface_sub = {
	.changed = acct_iface_changed,
	.recover = acct_iface_recover,
	.down_count = acct_iface_down_count,
};

