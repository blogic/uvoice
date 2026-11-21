// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The XMPP account layer. See xmpp/account.h for the contracts.
 *
 * One resolver serves every account. Its done callback routes by the owner
 * handle: the low byte names what asked, the stream or one relay, the rest
 * names the line.
 *
 * The iq answers of the legs. The stream reports an answer to the function
 * that sent the iq with its own callback type, so a small table relays each
 * pending answer to the callback the leg layer gave.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <libubox/blobmsg.h>

#include "str.h"
#include "proto/proto_trace.h"
#include "proto/shared/resolve.h"
#include "proto/shared/tls.h"
#include "proto/xmpp/account.h"
#include "proto/xmpp/call.h"
#include "proto/xmpp/disco.h"
#include "proto/xmpp/jingle.h"
#include "proto/xmpp/jmi.h"
#include "proto/xmpp/stream.h"

#define XMPP_NS_ROSTER		"jabber:iq:roster"
#define XMPP_NS_EXTDISCO	"urn:xmpp:extdisco:2"
#define XMPP_SERVICE_STUN	"stun"
#define XMPP_SERVICE_TURN	"turn"
#define XMPP_STUN_PORT		3478
#define XMPP_RELAYS_MAX		16U
#define XMPP_OWNER_STREAM	0U
#define XMPP_OWNER_SHIFT	8U
#define XMPP_STANZA_MAX		4096U
#define XMPP_FAULT_TEXT							\
	"the call could not connect and no TURN relay is configured"

struct xmpp_contact {
	char jid[JID_MAX];
	char name[XCALL_NAME_MAX];
};

struct xmpp_service {
	bool turn;
	bool resolved;
	char host[HAL_TRANSPORT_HOST_MAX];
	char addr[HAL_TRANSPORT_HOST_MAX];
	char user[HAL_TRANSPORT_CRED_MAX];
	char pass[HAL_TRANSPORT_CRED_MAX];
	uint16_t port;
	uint32_t query;
};

struct xmpp_account {
	bool active;
	bool ready_posted;
	unsigned int line;
	struct cfg_channel ch;
	char iface[CFG_STR_MAX];
	struct sip_tls tls;
	struct xmpp_stream stream;
	struct xmpp_contact roster[XMPP_ROSTER_MAX];
	unsigned int contacts;
	struct xmpp_service service[XMPP_SERVICES_MAX];
	unsigned int services;
	char label[JID_MAX];
};

struct xmpp_relay {
	bool in_use;
	xcall_reply_cb cb;
	void *priv;
};

static struct {
	const struct proto_host *host;
	const struct cfg *cfg;
	struct sip_dns dns;
	struct proto_timer_env timer_env;
	struct xmpp_account acct[CFG_CHANNEL_MAX];
	struct xmpp_relay relay[XMPP_RELAYS_MAX];
	bool inited;
	bool started;
} xa;

#define XMPP_ACCT_TRACE(acct, fmt, ...)					\
	PROTO_TRACE(xa.host, TRACE_XMPP, "xmpp line %u: " fmt, (acct)->line, \
		    ##__VA_ARGS__)

static void account_teardown(struct xmpp_account *acct);

/* --------------------------------------------------------------- helpers */

/**
 * owner_make() - the resolver owner of a question
 * @line: the line
 * @kind: XMPP_OWNER_STREAM, or the relay index plus one
 *
 * Return: the owner handle.
 */
static uint32_t owner_make(unsigned int line, unsigned int kind)
{
	return ((uint32_t)line << XMPP_OWNER_SHIFT) | kind;
}

/**
 * acct_get() - the account of a line
 * @line: the line
 *
 * Return: the account, or NULL when the line runs none.
 */
static struct xmpp_account *acct_get(unsigned int line)
{
	if (line >= CFG_CHANNEL_MAX || !xa.acct[line].active)
		return NULL;

	return &xa.acct[line];
}

/**
 * channel_xmpp() - whether a channel names an XMPP account
 * @cfg: the configuration
 * @ch: the channel
 *
 * Return: true for an enabled xmpp channel with a JID and a password.
 */
static bool channel_xmpp(const struct cfg *cfg, const struct cfg_channel *ch)
{
	return cfg->enabled && ch->present && ch->enabled &&
	       ch->account_type == CFG_ACCOUNT_XMPP && ch->xmpp_jid[0] &&
	       ch->xmpp_password[0];
}

/**
 * host_port_split() - "host[:port]" into its parts
 * @text: the text
 * @host: receives the host
 * @len: capacity of @host
 * @port: receives the port, left alone when the text names none
 */
static void host_port_split(const char *text, char *host, size_t len,
			    uint16_t *port)
{
	const char *colon = strrchr(text, ':');
	unsigned long value;

	if (!colon || strchr(text, ':') != colon) {
		str_copy(host, len, text);
		return;
	}

	snprintf(host, len, "%.*s", (int)(colon - text), text);
	value = strtoul(colon + 1, NULL, 10);
	if (value && value <= 65535)
		*port = (uint16_t)value;
}

/* ---------------------------------------------------------------- relays */

/**
 * service_add() - remember one STUN or TURN server and resolve it
 * @acct: the account
 * @turn: whether it relays
 * @host: its host, a name or an address
 * @port: its port
 * @user: the TURN user, "" for none
 * @pass: the TURN password, "" for none
 */
static void service_add(struct xmpp_account *acct, bool turn, const char *host,
			uint16_t port, const char *user, const char *pass)
{
	struct xmpp_service *svc;
	unsigned int i;

	if (!host || !host[0])
		return;

	for (i = 0; i < acct->services; i++) {
		if (acct->service[i].turn == turn &&
		    !strcmp(acct->service[i].host, host) &&
		    acct->service[i].port == port)
			return;
	}

	if (acct->services >= XMPP_SERVICES_MAX)
		return;

	svc = &acct->service[acct->services++];
	memset(svc, 0, sizeof(*svc));
	svc->turn = turn;
	svc->port = port ? port : XMPP_STUN_PORT;
	str_copy(svc->host, sizeof(svc->host), host);
	str_copy(svc->user, sizeof(svc->user), user ? user : "");
	str_copy(svc->pass, sizeof(svc->pass), pass ? pass : "");

	svc->query = sip_dns_resolve(&xa.dns, svc->host, SIP_DNS_ADDR,
				     svc->port,
				     owner_make(acct->line,
						(unsigned int)(svc -
							       acct->service) +
						1));
	XMPP_ACCT_TRACE(acct, "%s server %s:%u", turn ? "turn" : "stun",
			svc->host, svc->port);
}

/**
 * services_configured() - the relays the channel names
 * @acct: the account
 */
static void services_configured(struct xmpp_account *acct)
{
	char host[HAL_TRANSPORT_HOST_MAX];
	uint16_t port;

	if (acct->ch.xmpp_stun[0]) {
		port = XMPP_STUN_PORT;
		host_port_split(acct->ch.xmpp_stun, host, sizeof(host), &port);
		service_add(acct, false, host, port, "", "");
	}

	if (acct->ch.xmpp_turn[0]) {
		port = XMPP_STUN_PORT;
		host_port_split(acct->ch.xmpp_turn, host, sizeof(host), &port);
		service_add(acct, true, host, port, acct->ch.xmpp_turn_user,
			    acct->ch.xmpp_turn_password);
	}
}

/**
 * service_resolved() - the resolver answered a relay
 * @acct: the account
 * @index: the relay
 * @res: the answer
 */
static void service_resolved(struct xmpp_account *acct, unsigned int index,
			     const struct sip_dns_result *res)
{
	struct xmpp_service *svc;

	if (index >= acct->services)
		return;

	svc = &acct->service[index];
	svc->query = SIP_HANDLE_NONE;
	if (res->status != SIP_DNS_OK || !res->count) {
		XMPP_ACCT_TRACE(acct, "relay %s did not resolve: %s", svc->host,
				sip_dns_status_name(res->status));
		return;
	}

	str_copy(svc->addr, sizeof(svc->addr), res->target[0].host);
	svc->resolved = true;
	XMPP_ACCT_TRACE(acct, "relay %s is %s", svc->host, svc->addr);
}

/**
 * services_reply() - the answer of the XEP-0215 query
 * @xs: the stream
 * @reply: the iq, NULL for none
 * @priv: the account
 */
static void services_reply(struct xmpp_stream *xs, const struct xml_doc *reply,
			   void *priv)
{
	struct xmpp_account *acct = priv;
	const char *type, *transport, *host, *port_text;
	unsigned int list, node;
	unsigned long port;

	if (!reply)
		return;

	type = xml_attr_get(reply, 0, "type");
	if (!type || strcmp(type, "result"))
		return;

	list = xml_child(reply, XML_NONE, "services", XMPP_NS_EXTDISCO);
	node = xml_child(reply, list, "service", XMPP_NS_EXTDISCO);
	while (node != XML_NONE) {
		type = xml_attr_get(reply, node, "type");
		transport = xml_attr_get(reply, node, "transport");
		host = xml_attr_get(reply, node, "host");
		port_text = xml_attr_get(reply, node, "port");
		port = port_text ? strtoul(port_text, NULL, 10) : 0;
		if (type && host && (!transport || !strcmp(transport, "udp")) &&
		    port <= 65535) {
			if (!strcmp(type, XMPP_SERVICE_STUN))
				service_add(acct, false, host, (uint16_t)port,
					    "", "");
			else if (!strcmp(type, XMPP_SERVICE_TURN))
				service_add(acct, true, host, (uint16_t)port,
					    xml_attr_get(reply, node,
							 "username"),
					    xml_attr_get(reply, node,
							 "password"));
		}
		node = xml_next(reply, node, "service", XMPP_NS_EXTDISCO);
	}
}

/**
 * services_ask() - the XEP-0215 query to the server
 * @acct: the account
 */
static void services_ask(struct xmpp_account *acct)
{
	char mem[XMPP_STANZA_MAX];
	char id[XMPP_ID_MAX];
	struct xml_w writer;
	size_t len;

	xmpp_stream_id_next(&acct->stream, id, sizeof(id));
	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "iq");
	xml_w_attr(&writer, "type", "get");
	xml_w_attr(&writer, "to", acct->stream.jid.domain);
	xml_w_attr(&writer, "id", id);
	xml_w_start(&writer, "services");
	xml_w_attr(&writer, "xmlns", XMPP_NS_EXTDISCO);
	xml_w_end(&writer);
	xml_w_end(&writer);
	len = xml_w_finish(&writer);
	if (len)
		xmpp_stream_iq_send(&acct->stream, mem, len, id, services_reply,
				    acct);
}

/* ---------------------------------------------------------------- roster */

/**
 * roster_reply() - the answer of the roster query
 * @xs: the stream
 * @reply: the iq, NULL for none
 * @priv: the account
 */
static void roster_reply(struct xmpp_stream *xs, const struct xml_doc *reply,
			 void *priv)
{
	struct xmpp_account *acct = priv;
	struct xmpp_contact *contact;
	const char *type, *jid, *name;
	unsigned int query, node;

	if (!reply)
		return;

	type = xml_attr_get(reply, 0, "type");
	if (!type || strcmp(type, "result"))
		return;

	acct->contacts = 0;
	query = xml_child(reply, XML_NONE, "query", XMPP_NS_ROSTER);
	node = xml_child(reply, query, "item", XMPP_NS_ROSTER);
	while (node != XML_NONE && acct->contacts < XMPP_ROSTER_MAX) {
		jid = xml_attr_get(reply, node, "jid");
		name = xml_attr_get(reply, node, "name");
		if (jid && jid[0]) {
			contact = &acct->roster[acct->contacts++];
			jid_text_bare(jid, contact->jid, sizeof(contact->jid));
			str_copy(contact->name, sizeof(contact->name),
				 name ? name : "");
		}
		node = xml_next(reply, node, "item", XMPP_NS_ROSTER);
	}

	XMPP_ACCT_TRACE(acct, "roster with %u contacts", acct->contacts);
}

/**
 * roster_ask() - the roster query
 * @acct: the account
 */
static void roster_ask(struct xmpp_account *acct)
{
	char mem[XMPP_STANZA_MAX];
	char id[XMPP_ID_MAX];
	struct xml_w writer;
	size_t len;

	xmpp_stream_id_next(&acct->stream, id, sizeof(id));
	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "iq");
	xml_w_attr(&writer, "type", "get");
	xml_w_attr(&writer, "id", id);
	xml_w_start(&writer, "query");
	xml_w_attr(&writer, "xmlns", XMPP_NS_ROSTER);
	xml_w_end(&writer);
	xml_w_end(&writer);
	len = xml_w_finish(&writer);
	if (len)
		xmpp_stream_iq_send(&acct->stream, mem, len, id, roster_reply,
				    acct);
}

/**
 * presence_send() - the initial presence with the entity capabilities
 * @acct: the account
 */
static void presence_send(struct xmpp_account *acct)
{
	char mem[XMPP_STANZA_MAX];
	struct xml_w writer;
	size_t len;

	xml_w_init(&writer, mem, sizeof(mem));
	xml_w_start(&writer, "presence");
	disco_caps_write(&writer);
	xml_w_end(&writer);
	len = xml_w_finish(&writer);
	if (len)
		xmpp_stream_send(&acct->stream, mem, len);
}

/* ---------------------------------------------------------- the stream */

/**
 * reg_post() - tell the core the registration state of the line
 * @acct: the account
 * @up: whether the stream is ready
 */
static void reg_post(struct xmpp_account *acct, bool up)
{
	if (acct->ready_posted == up)
		return;

	acct->ready_posted = up;
	if (xa.host && xa.host->reg_state_post)
		xa.host->reg_state_post(xa.host->ctx, acct->line, up,
					acct->stream.jid.domain, false);
}

/**
 * stream_state() - a transition of the stream
 * @xs: the stream
 * @state: the new state
 * @reason: the text of a connection that ended
 */
static void stream_state(struct xmpp_stream *xs, enum xmpp_stream_state state,
			 const char *reason)
{
	struct xmpp_account *acct = xs->priv;

	if (state == XMPP_ST_READY) {
		presence_send(acct);
		roster_ask(acct);
		services_ask(acct);
		reg_post(acct, true);
		return;
	}

	if (acct->ready_posted) {
		xcalls_stream_down(acct->line);
		reg_post(acct, false);
	}
}

/**
 * stream_stanza() - a stanza the stream did not answer itself
 * @xs: the stream
 * @doc: the stanza
 *
 * Return: true when the legs took it.
 */
static bool stream_stanza(struct xmpp_stream *xs, const struct xml_doc *doc)
{
	struct xmpp_account *acct = xs->priv;

	if (xml_node_is(doc, 0, "iq", XMPP_NS_CLIENT))
		return xcall_jingle_rx(acct->line, doc);
	if (xml_node_is(doc, 0, "message", XMPP_NS_CLIENT))
		return xcall_jmi_rx(acct->line, doc);

	return false;
}

static const struct xmpp_stream_cb stream_cb = {
	.state = stream_state,
	.stanza = stream_stanza,
};

/**
 * dns_done() - the shared resolver answered
 * @resolver: the resolver
 * @query: the query handle
 * @owner: the owner handle
 * @res: the answer
 * @priv: unused
 */
static void dns_done(struct sip_dns *resolver, uint32_t query, uint32_t owner,
		     const struct sip_dns_result *res, void *priv)
{
	unsigned int line = owner >> XMPP_OWNER_SHIFT;
	unsigned int kind = owner & ((1u << XMPP_OWNER_SHIFT) - 1);
	struct xmpp_account *acct = acct_get(line);

	if (!acct)
		return;

	if (kind == XMPP_OWNER_STREAM)
		xmpp_stream_dns_done(&acct->stream, query, res);
	else
		service_resolved(acct, kind - 1, res);
}

/* -------------------------------------------------- the leg environment */

/**
 * relay_take() - a free slot of the iq relay table
 * @cb: the callback of the leg
 * @priv: its pointer
 *
 * Return: the slot, or NULL.
 */
static struct xmpp_relay *relay_take(xcall_reply_cb cb, void *priv)
{
	unsigned int i;

	for (i = 0; i < XMPP_RELAYS_MAX; i++) {
		if (xa.relay[i].in_use)
			continue;
		xa.relay[i].in_use = true;
		xa.relay[i].cb = cb;
		xa.relay[i].priv = priv;
		return &xa.relay[i];
	}

	return NULL;
}

/**
 * relay_reply() - hand an iq answer to the leg that sent the iq
 * @xs: the stream
 * @reply: the answer, NULL for none
 * @priv: the relay slot
 */
static void relay_reply(struct xmpp_stream *xs, const struct xml_doc *reply,
			void *priv)
{
	struct xmpp_relay *relay = priv;
	xcall_reply_cb cb = relay->cb;
	void *cb_priv = relay->priv;
	const char *type = reply ? xml_attr_get(reply, 0, "type") : NULL;

	memset(relay, 0, sizeof(*relay));
	if (cb)
		cb(cb_priv, type && !strcmp(type, "result"), reply);
}

/**
 * env_ready() - whether the stream of a line carries stanzas
 * @priv: unused
 * @line: the line
 *
 * Return: true in the ready state.
 */
static bool env_ready(void *priv, unsigned int line)
{
	struct xmpp_account *acct = acct_get(line);

	return acct && xmpp_stream_ready(&acct->stream);
}

/**
 * env_send() - one stanza on the stream of a line
 * @priv: unused
 * @line: the line
 * @text: the stanza
 * @len: its length
 *
 * Return: 0, or a negative errno.
 */
static int env_send(void *priv, unsigned int line, const char *text,
		    size_t len)
{
	struct xmpp_account *acct = acct_get(line);

	if (!acct)
		return -ENOTCONN;

	return xmpp_stream_send(&acct->stream, text, len);
}

/**
 * env_iq_send() - an iq of a leg, with its answer relayed back
 * @priv: unused
 * @line: the line
 * @text: the iq
 * @len: its length
 * @id: its id
 * @cb: the callback of the leg
 * @cb_priv: its pointer
 *
 * Return: 0, -EBUSY when the relay table is full, or the stream's errno.
 */
static int env_iq_send(void *priv, unsigned int line, const char *text,
		       size_t len, const char *id, xcall_reply_cb cb,
		       void *cb_priv)
{
	struct xmpp_account *acct = acct_get(line);
	struct xmpp_relay *relay;
	int ret;

	if (!acct)
		return -ENOTCONN;

	relay = relay_take(cb, cb_priv);
	if (!relay)
		return -EBUSY;

	ret = xmpp_stream_iq_send(&acct->stream, text, len, id, relay_reply,
				  relay);
	if (ret)
		memset(relay, 0, sizeof(*relay));

	return ret;
}

/**
 * env_reply_result() - the empty result of an iq a leg took
 * @priv: unused
 * @line: the line
 * @iq: the iq
 *
 * Return: 0, or a negative errno.
 */
static int env_reply_result(void *priv, unsigned int line,
			    const struct xml_doc *iq)
{
	struct xmpp_account *acct = acct_get(line);

	if (!acct)
		return -ENOTCONN;

	return xmpp_stream_iq_result(&acct->stream, iq);
}

/**
 * env_reply_error() - the stanza error of an iq a leg refused
 * @priv: unused
 * @line: the line
 * @iq: the iq
 * @type: the error type
 * @condition: the condition element
 *
 * Return: 0, or a negative errno.
 */
static int env_reply_error(void *priv, unsigned int line,
			   const struct xml_doc *iq, const char *type,
			   const char *condition)
{
	struct xmpp_account *acct = acct_get(line);

	if (!acct)
		return -ENOTCONN;

	return xmpp_stream_iq_error(&acct->stream, iq, type, condition);
}

/**
 * env_id_next() - a fresh stanza id of the stream of a line
 * @priv: unused
 * @line: the line
 * @out: receives the id
 * @len: capacity of @out
 */
static void env_id_next(void *priv, unsigned int line, char *out, size_t len)
{
	struct xmpp_account *acct = acct_get(line);

	if (!acct) {
		snprintf(out, len, "x%u", line);
		return;
	}

	xmpp_stream_id_next(&acct->stream, out, len);
}

/**
 * env_self_jid() - the full JID the stream of a line bound
 * @priv: unused
 * @line: the line
 * @out: receives it
 *
 * Return: true once the resource is bound.
 */
static bool env_self_jid(void *priv, unsigned int line, struct jid *out)
{
	struct xmpp_account *acct = acct_get(line);
	const struct jid *bound;

	if (!acct)
		return false;

	bound = xmpp_stream_jid(&acct->stream);
	if (!bound->resource[0])
		return false;

	*out = *bound;

	return true;
}

/**
 * env_local_addr() - the address of the voice interface the media binds to
 * @priv: unused
 * @line: unused, every line shares the interface
 * @out: receives the address text
 * @len: capacity of @out
 *
 * Return: true when the tracker knows an address.
 */
static bool env_local_addr(void *priv, unsigned int line, char *out,
			   size_t len)
{
	const struct net_iface_state *state;

	out[0] = 0;
	if (!xa.host || !xa.host->net_iface_state_get)
		return false;

	state = xa.host->net_iface_state_get(xa.host->ctx);
	if (!state)
		return false;

	if (state->addr4_known) {
		str_copy(out, len, state->addr4);
		return true;
	}
	if (state->addr6_known && state->ipv6_served) {
		str_copy(out, len, state->addr6);
		return true;
	}

	return false;
}

/**
 * env_bind_dev() - the netdev the media of a line scopes to
 * @priv: unused
 * @line: the line
 *
 * Return: the netdev, "" for none.
 */
static const char *env_bind_dev(void *priv, unsigned int line)
{
	struct xmpp_account *acct = acct_get(line);
	const char *dev;

	if (!acct || !xa.host || !xa.host->net_iface_bind_device)
		return "";

	dev = xa.host->net_iface_bind_device(xa.host->ctx, acct->iface);

	return dev ? dev : "";
}

/**
 * env_contact_name() - the roster name of a bare JID
 * @priv: unused
 * @line: the line
 * @bare: the bare JID
 * @out: receives the name
 * @len: capacity of @out
 *
 * Return: true when the roster names the contact.
 */
static bool env_contact_name(void *priv, unsigned int line, const char *bare,
			     char *out, size_t len)
{
	struct xmpp_account *acct = acct_get(line);
	unsigned int i;

	if (!acct)
		return false;

	for (i = 0; i < acct->contacts; i++) {
		if (strcmp(acct->roster[i].jid, bare) ||
		    !acct->roster[i].name[0])
			continue;
		str_copy(out, len, acct->roster[i].name);
		return true;
	}

	return false;
}

/**
 * env_speed_dial() - the JID a dialled digit string stands for
 * @priv: unused
 * @line: the line
 * @digits: the digits
 * @out: receives the JID
 * @len: capacity of @out
 *
 * Return: true when a speed dial entry "digits=jid" matched.
 */
static bool env_speed_dial(void *priv, unsigned int line, const char *digits,
			   char *out, size_t len)
{
	struct xmpp_account *acct = acct_get(line);
	const char *entry, *eq;
	size_t dlen = strlen(digits);
	unsigned int i;

	if (!acct)
		return false;

	for (i = 0; i < CFG_XMPP_SPEED_DIAL_MAX; i++) {
		entry = acct->ch.xmpp_speed_dial[i];
		eq = strchr(entry, '=');
		if (!eq || (size_t)(eq - entry) != dlen ||
		    strncmp(entry, digits, dlen) || !eq[1])
			continue;
		str_copy(out, len, eq + 1);
		return true;
	}

	return false;
}

/**
 * env_ice_servers() - the resolved relays of a line, into a transport
 * @priv: unused
 * @line: the line
 * @out: the transport block, its STUN host and TURN rows filled
 */
static void env_ice_servers(void *priv, unsigned int line,
			    struct hal_transport *out)
{
	struct xmpp_account *acct = acct_get(line);
	const struct xmpp_service *svc;
	struct hal_turn *turn;
	unsigned int i;

	if (!acct)
		return;

	for (i = 0; i < acct->services; i++) {
		svc = &acct->service[i];
		if (!svc->resolved)
			continue;
		if (!svc->turn) {
			if (out->stun_host[0])
				continue;
			str_copy(out->stun_host, sizeof(out->stun_host),
				 svc->addr);
			out->stun_port = svc->port;
			continue;
		}
		if (out->turns >= HAL_TURN_MAX)
			continue;
		turn = &out->turn[out->turns++];
		str_copy(turn->host, sizeof(turn->host), svc->addr);
		str_copy(turn->user, sizeof(turn->user), svc->user);
		str_copy(turn->pass, sizeof(turn->pass), svc->pass);
		turn->port = svc->port;
	}
}

/**
 * env_relay_missing() - a call failed its checks with no relay to fall to
 * @priv: unused
 * @line: the line
 */
static void env_relay_missing(void *priv, unsigned int line)
{
	if (!xa.host)
		return;

	if (xa.host->line_fault)
		xa.host->line_fault(xa.host->ctx, line, true, XMPP_FAULT_TEXT);
	if (xa.host->log)
		xa.host->log(xa.host->ctx, LOG_WARNING, "xmpp line %u: %s",
			     line, XMPP_FAULT_TEXT);
}

/* ---------------------------------------------------------------- build */

/**
 * stream_cfg_fill() - the stream configuration of a channel
 * @ch: the channel
 * @out: receives it
 */
static void stream_cfg_fill(const struct cfg_channel *ch,
			    struct xmpp_stream_cfg *out)
{
	memset(out, 0, sizeof(*out));
	if (ch->xmpp_resource[0] && !strchr(ch->xmpp_jid, '/'))
		snprintf(out->jid, sizeof(out->jid), "%s/%s", ch->xmpp_jid,
			 ch->xmpp_resource);
	else
		str_copy(out->jid, sizeof(out->jid), ch->xmpp_jid);
	str_copy(out->password, sizeof(out->password), ch->xmpp_password);
	str_copy(out->host, sizeof(out->host), ch->xmpp_host);
	out->port = (uint16_t)ch->xmpp_port;
	out->direct_tls = ch->xmpp_direct_tls;
	out->tls_verify = ch->xmpp_tls_verify;
	out->allow_plain = ch->xmpp_allow_plain;
}

/**
 * account_build() - start the account of a line
 * @acct: the account
 * @cfg: the configuration
 * @line: the line
 */
static void account_build(struct xmpp_account *acct, const struct cfg *cfg,
			  unsigned int line)
{
	const struct cfg_channel *ch = &cfg->channel[line];
	struct sip_tls_cfg tls = {
		.ca_path = ch->xmpp_tls_ca[0] ? ch->xmpp_tls_ca : NULL,
		.verify = ch->xmpp_tls_verify,
	};
	struct xmpp_stream_env env = {
		.host = xa.host,
		.dns = &xa.dns,
		.dns_owner = owner_make(line, XMPP_OWNER_STREAM),
	};
	struct xmpp_stream_cfg scfg;
	int ret;

	memset(acct, 0, sizeof(*acct));
	acct->line = line;
	acct->ch = *ch;
	str_copy(acct->iface, sizeof(acct->iface),
		 ch->local_interface[0] ? ch->local_interface :
					  cfg->local_interface);
	jid_text_bare(ch->xmpp_jid, acct->label, sizeof(acct->label));

	sip_tls_init(&acct->tls);
	if (!sip_tls_ctx_load(&acct->tls, &tls)) {
		if (xa.host && xa.host->log)
			xa.host->log(xa.host->ctx, LOG_ERR,
				     "xmpp line %u: tls: %s", line,
				     sip_tls_error(&acct->tls));
		return;
	}
	env.tls = &acct->tls;

	xmpp_stream_init(&acct->stream, &stream_cb, acct, &env);
	stream_cfg_fill(ch, &scfg);
	acct->active = true;

	ret = xmpp_stream_start(&acct->stream, &scfg);
	if (ret) {
		if (xa.host && xa.host->log)
			xa.host->log(xa.host->ctx, LOG_ERR,
				     "xmpp line %u: the jid or the password is "
				     "unusable (%d)", line, ret);
		account_teardown(acct);
		return;
	}

	services_configured(acct);
}

/**
 * account_teardown() - end the account of a line
 * @acct: the account
 */
static void account_teardown(struct xmpp_account *acct)
{
	unsigned int i;

	if (!acct->active)
		return;

	xcalls_line_teardown(acct->line);
	for (i = 0; i < acct->services; i++) {
		if (acct->service[i].query != SIP_HANDLE_NONE)
			sip_dns_cancel(&xa.dns, acct->service[i].query);
	}
	xmpp_stream_done(&acct->stream);
	reg_post(acct, false);
	sip_tls_done(&acct->tls);
	acct->active = false;
	memset(acct, 0, sizeof(*acct));
}

/**
 * stream_options_moved() - whether a channel change rebuilds the stream
 * @old_ch: the channel as built
 * @new_ch: the channel now
 *
 * Return: true when an option the stream or its TLS context read moved.
 */
static bool stream_options_moved(const struct cfg_channel *old_ch,
				 const struct cfg_channel *new_ch)
{
	return strcmp(old_ch->xmpp_jid, new_ch->xmpp_jid) ||
	       strcmp(old_ch->xmpp_password, new_ch->xmpp_password) ||
	       strcmp(old_ch->xmpp_resource, new_ch->xmpp_resource) ||
	       strcmp(old_ch->xmpp_host, new_ch->xmpp_host) ||
	       strcmp(old_ch->xmpp_tls_ca, new_ch->xmpp_tls_ca) ||
	       strcmp(old_ch->local_interface, new_ch->local_interface) ||
	       old_ch->xmpp_port != new_ch->xmpp_port ||
	       old_ch->xmpp_direct_tls != new_ch->xmpp_direct_tls ||
	       old_ch->xmpp_tls_verify != new_ch->xmpp_tls_verify ||
	       old_ch->xmpp_allow_plain != new_ch->xmpp_allow_plain;
}

/* -------------------------------------------------------------- lifecycle */

void xmpp_accounts_init(const struct proto_host *host, const struct call_cb *cb)
{
	struct sip_dns_cb dcb = { .done = dns_done };
	struct sip_dns_cfg dcfg;
	struct xcall_env env = {
		.timer = &xa.timer_env,
		.media = host ? &host->media : NULL,
		.cb = cb,
		.host = host,
		.ready = env_ready,
		.send = env_send,
		.iq_send = env_iq_send,
		.reply_result = env_reply_result,
		.reply_error = env_reply_error,
		.id_next = env_id_next,
		.self_jid = env_self_jid,
		.local_addr = env_local_addr,
		.bind_dev = env_bind_dev,
		.contact_name = env_contact_name,
		.speed_dial = env_speed_dial,
		.ice_servers = env_ice_servers,
		.relay_missing = env_relay_missing,
	};

	memset(&xa, 0, sizeof(xa));
	xa.host = host;
	proto_timer_env_uloop_init(&xa.timer_env);

	sip_dns_cfg_default(&dcfg);
	dcfg.srv_fallback = true;
	dcfg.trace_host = host;
	dcfg.trace_dom = TRACE_XMPP;
	sip_dns_init(&xa.dns, &dcfg, &dcb, NULL);

	xcalls_init(&env);
	xa.inited = true;
}

void xmpp_accounts_start(const struct cfg *cfg)
{
	unsigned int i;

	if (!xa.inited)
		return;

	xa.cfg = cfg;
	xcalls_reload(cfg);
	sip_dns_ipv6_set(&xa.dns, cfg->ipv6_enabled);

	for (i = 0; i < CFG_CHANNEL_MAX && i < cfg->channels; i++) {
		if (channel_xmpp(cfg, &cfg->channel[i]))
			account_build(&xa.acct[i], cfg, i);
	}

	xa.started = true;
}

void xmpp_accounts_reload(const struct cfg *old_cfg, const struct cfg *new_cfg)
{
	struct xmpp_account *acct;
	bool want, rebuild;
	unsigned int i;

	if (!xa.inited || !xa.started)
		return;

	xa.cfg = new_cfg;
	xcalls_reload(new_cfg);
	sip_dns_ipv6_set(&xa.dns, new_cfg->ipv6_enabled);

	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		acct = &xa.acct[i];
		want = i < new_cfg->channels &&
		       channel_xmpp(new_cfg, &new_cfg->channel[i]);
		rebuild = acct->active && want &&
			  (stream_options_moved(&acct->ch,
						&new_cfg->channel[i]) ||
			   strcmp(old_cfg->local_interface,
				  new_cfg->local_interface));

		if (acct->active && (!want || rebuild))
			account_teardown(acct);
		if (want && !acct->active) {
			account_build(acct, new_cfg, i);
			continue;
		}
		if (!acct->active)
			continue;

		acct->ch = new_cfg->channel[i];
		acct->services = 0;
		services_configured(acct);
		if (xmpp_stream_ready(&acct->stream))
			services_ask(acct);
	}
}

void xmpp_accounts_hal_set(uint32_t features, uint32_t codecs)
{
	xcalls_hal_features_set(features);
	xcalls_hal_codecs_set(codecs);
}

void xmpp_accounts_shutdown_begin(void)
{
	unsigned int i;

	for (i = 0; i < CFG_CHANNEL_MAX; i++)
		account_teardown(&xa.acct[i]);
}

void xmpp_accounts_done(void)
{
	if (!xa.inited)
		return;

	xmpp_accounts_shutdown_begin();
	xcalls_done();
	sip_dns_done(&xa.dns);
	xa.inited = false;
	xa.started = false;
}

bool xmpp_account_active(unsigned int line)
{
	return acct_get(line) != NULL;
}

bool xmpp_account_ready(unsigned int line)
{
	struct xmpp_account *acct = acct_get(line);

	return acct && xmpp_stream_ready(&acct->stream);
}

const char *xmpp_account_label(unsigned int line)
{
	struct xmpp_account *acct = acct_get(line);

	return acct ? acct->label : "";
}

/* ----------------------------------------------------------------- dumps */

/**
 * account_dump() - one account for the status verb
 * @acct: the account
 * @b: the blob buffer
 */
static void account_dump(const struct xmpp_account *acct, struct blob_buf *b)
{
	const struct xmpp_stream_stat *stat = xmpp_stream_stat(&acct->stream);
	const struct xmpp_service *svc;
	char jid[JID_MAX];
	void *tbl, *arr, *row;
	unsigned int i;

	tbl = blobmsg_open_table(b, NULL);
	blobmsg_add_u32(b, "line", acct->line);
	blobmsg_add_string(b, "jid", acct->label);
	jid_full(xmpp_stream_jid(&acct->stream), jid, sizeof(jid));
	blobmsg_add_string(b, "bound", jid);
	blobmsg_add_string(b, "state",
			   xmpp_stream_state_name(acct->stream.state));
	blobmsg_add_string(b, "reason", xmpp_stream_reason(&acct->stream));
	blobmsg_add_u8(b, "tls", acct->stream.tls_up);
	blobmsg_add_u8(b, "sm", acct->stream.sm.enabled);
	blobmsg_add_u8(b, "carbons", acct->stream.carbons);
	blobmsg_add_u32(b, "contacts", acct->contacts);
	blobmsg_add_u64(b, "connects", stat->connects);
	blobmsg_add_u64(b, "failures", stat->failures);
	blobmsg_add_u64(b, "rx_stanzas", stat->rx_stanzas);
	blobmsg_add_u64(b, "tx_stanzas", stat->tx_stanzas);

	arr = blobmsg_open_array(b, "relays");
	for (i = 0; i < acct->services; i++) {
		svc = &acct->service[i];
		row = blobmsg_open_table(b, NULL);
		blobmsg_add_string(b, "type", svc->turn ? XMPP_SERVICE_TURN :
							  XMPP_SERVICE_STUN);
		blobmsg_add_string(b, "host", svc->host);
		blobmsg_add_u32(b, "port", svc->port);
		blobmsg_add_string(b, "address", svc->addr);
		blobmsg_close_table(b, row);
	}
	blobmsg_close_array(b, arr);

	xcalls_status_dump(b, acct->line);
	blobmsg_close_table(b, tbl);
}

void xmpp_accounts_status_dump(struct blob_buf *b, int want_line)
{
	unsigned int i;
	void *arr;

	arr = blobmsg_open_array(b, "xmpp");
	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		if (!xa.acct[i].active)
			continue;
		if (want_line >= 0 && (unsigned int)want_line != i)
			continue;
		account_dump(&xa.acct[i], b);
	}
	blobmsg_close_array(b, arr);
}

void xmpp_roster_dump(struct blob_buf *b)
{
	const struct xmpp_account *acct;
	unsigned int i, j;
	void *arr, *tbl;

	arr = blobmsg_open_array(b, "roster");
	for (i = 0; i < CFG_CHANNEL_MAX; i++) {
		acct = &xa.acct[i];
		if (!acct->active)
			continue;
		for (j = 0; j < acct->contacts; j++) {
			tbl = blobmsg_open_table(b, NULL);
			blobmsg_add_u32(b, "line", i);
			blobmsg_add_string(b, "jid", acct->roster[j].jid);
			blobmsg_add_string(b, "name", acct->roster[j].name);
			blobmsg_close_table(b, tbl);
		}
	}
	blobmsg_close_array(b, arr);
}
