// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "str.h"
#include "proto/xmpp/jingle.h"

#define JINGLE_FP_HASH		"sha-256"
#define JINGLE_TOKEN_MAX	64U
#define JINGLE_SENDERS_BOTH	"both"

static const char *const action_names[JINGLE_ACTION_MAX] = {
	[JINGLE_SESSION_INITIATE] = "session-initiate",
	[JINGLE_SESSION_ACCEPT] = "session-accept",
	[JINGLE_SESSION_TERMINATE] = "session-terminate",
	[JINGLE_SESSION_INFO] = "session-info",
	[JINGLE_TRANSPORT_INFO] = "transport-info",
	[JINGLE_TRANSPORT_ACCEPT] = "transport-accept",
	[JINGLE_TRANSPORT_REJECT] = "transport-reject",
	[JINGLE_TRANSPORT_REPLACE] = "transport-replace",
	[JINGLE_CONTENT_ACCEPT] = "content-accept",
	[JINGLE_CONTENT_ADD] = "content-add",
	[JINGLE_CONTENT_MODIFY] = "content-modify",
	[JINGLE_CONTENT_REJECT] = "content-reject",
	[JINGLE_CONTENT_REMOVE] = "content-remove",
	[JINGLE_DESCRIPTION_INFO] = "description-info",
	[JINGLE_SECURITY_INFO] = "security-info",
};

static const char *const reason_names[JINGLE_REASON_MAX] = {
	[JINGLE_REASON_SUCCESS] = "success",
	[JINGLE_REASON_BUSY] = "busy",
	[JINGLE_REASON_DECLINE] = "decline",
	[JINGLE_REASON_CANCEL] = "cancel",
	[JINGLE_REASON_CONNECTIVITY_ERROR] = "connectivity-error",
	[JINGLE_REASON_FAILED_APPLICATION] = "failed-application",
	[JINGLE_REASON_FAILED_TRANSPORT] = "failed-transport",
	[JINGLE_REASON_GENERAL_ERROR] = "general-error",
	[JINGLE_REASON_GONE] = "gone",
	[JINGLE_REASON_INCOMPATIBLE_PARAMETERS] = "incompatible-parameters",
	[JINGLE_REASON_MEDIA_ERROR] = "media-error",
	[JINGLE_REASON_SECURITY_ERROR] = "security-error",
	[JINGLE_REASON_TIMEOUT] = "timeout",
	[JINGLE_REASON_UNSUPPORTED_APPLICATIONS] = "unsupported-applications",
	[JINGLE_REASON_UNSUPPORTED_TRANSPORTS] = "unsupported-transports",
	[JINGLE_REASON_EXPIRED] = "expired",
	[JINGLE_REASON_ALTERNATIVE_SESSION] = "alternative-session",
};

static const char *const info_names[JINGLE_INFO_MAX] = {
	[JINGLE_INFO_ACTIVE] = "active",
	[JINGLE_INFO_HOLD] = "hold",
	[JINGLE_INFO_UNHOLD] = "unhold",
	[JINGLE_INFO_MUTE] = "mute",
	[JINGLE_INFO_UNMUTE] = "unmute",
	[JINGLE_INFO_RINGING] = "ringing",
};

static const char *const setup_names[JINGLE_SETUP_MAX] = {
	[JINGLE_SETUP_ACTPASS] = "actpass",
	[JINGLE_SETUP_ACTIVE] = "active",
	[JINGLE_SETUP_PASSIVE] = "passive",
};

/**
 * table_name() - the word of a table entry
 * @table: the table
 * @count: its length
 * @index: the entry
 *
 * Return: the word, "" for none.
 */
static const char *table_name(const char *const *table, unsigned int count,
			      unsigned int index)
{
	if (index >= count || !table[index])
		return "";

	return table[index];
}

/**
 * table_find() - the entry of a word
 * @table: the table
 * @count: its length
 * @name: the word, NULL for none
 *
 * Return: the entry, or 0 for an unknown word.
 */
static unsigned int table_find(const char *const *table, unsigned int count,
			       const char *name)
{
	unsigned int i;

	if (!name)
		return 0;

	for (i = 1; i < count; i++) {
		if (table[i] && !strcmp(table[i], name))
			return i;
	}

	return 0;
}

const char *jingle_action_name(enum jingle_action action)
{
	return table_name(action_names, JINGLE_ACTION_MAX, action);
}

enum jingle_action jingle_action_of(const char *name)
{
	return (enum jingle_action)table_find(action_names, JINGLE_ACTION_MAX,
					      name);
}

const char *jingle_reason_name(enum jingle_reason reason)
{
	return table_name(reason_names, JINGLE_REASON_MAX, reason);
}

const char *jingle_info_name(enum jingle_info info)
{
	return table_name(info_names, JINGLE_INFO_MAX, info);
}

const char *jingle_setup_name(enum jingle_setup setup)
{
	return table_name(setup_names, JINGLE_SETUP_MAX, setup);
}

enum jingle_setup jingle_setup_of(const char *name)
{
	return (enum jingle_setup)table_find(setup_names, JINGLE_SETUP_MAX,
					     name);
}

/* ---------------------------------------------------------------- parsing */

/**
 * attr_copy() - copy an attribute into a field
 * @doc: the tree
 * @node: the element
 * @name: the attribute
 * @out: receives the value, "" when absent
 * @len: capacity of @out
 */
static void attr_copy(const struct xml_doc *doc, unsigned int node,
		      const char *name, char *out, size_t len)
{
	const char *value = xml_attr_get(doc, node, name);

	str_copy(out, len, value ? value : "");
}

/**
 * attr_uint() - a numeric attribute
 * @doc: the tree
 * @node: the element
 * @name: the attribute
 * @out: receives the number
 *
 * Return: true when the attribute was a decimal number.
 */
static bool attr_uint(const struct xml_doc *doc, unsigned int node,
		      const char *name, unsigned long *out)
{
	const char *value = xml_attr_get(doc, node, name);
	char *end;

	if (!value || !isdigit((unsigned char)value[0]))
		return false;

	*out = strtoul(value, &end, 10);

	return *end == 0;
}

/**
 * hex_nibble() - the value of one hex digit
 * @digit: the character
 *
 * Return: 0 to 15, or -1.
 */
static int hex_nibble(char digit)
{
	if (digit >= '0' && digit <= '9')
		return digit - '0';
	if (digit >= 'a' && digit <= 'f')
		return digit - 'a' + 10;
	if (digit >= 'A' && digit <= 'F')
		return digit - 'A' + 10;

	return -1;
}

bool jingle_fingerprint_parse(const char *text, uint8_t out[JINGLE_FP_LEN])
{
	unsigned int i;
	int high, low;

	while (isspace((unsigned char)*text))
		text++;

	for (i = 0; i < JINGLE_FP_LEN; i++) {
		high = hex_nibble(text[0]);
		low = text[0] ? hex_nibble(text[1]) : -1;
		if (high < 0 || low < 0)
			return false;
		out[i] = (uint8_t)((high << 4) | low);
		text += 2;
		if (i + 1 < JINGLE_FP_LEN && *text == ':')
			text++;
	}

	while (isspace((unsigned char)*text))
		text++;

	return *text == 0;
}

void jingle_fingerprint_text(const uint8_t fp[JINGLE_FP_LEN], char *out,
			     size_t len)
{
	size_t used = 0;
	unsigned int i;
	int wrote;

	out[0] = 0;
	for (i = 0; i < JINGLE_FP_LEN && used < len; i++) {
		wrote = snprintf(out + used, len - used, "%s%02X", i ? ":" : "",
				 fp[i]);
		if (wrote < 0)
			return;
		used += (size_t)wrote;
	}
}

bool jingle_candidate_sdp(const struct xml_doc *doc, unsigned int node,
			  char *out, size_t len)
{
	const char *foundation = xml_attr_get(doc, node, "foundation");
	const char *protocol = xml_attr_get(doc, node, "protocol");
	const char *ip = xml_attr_get(doc, node, "ip");
	const char *type = xml_attr_get(doc, node, "type");
	const char *rel_addr = xml_attr_get(doc, node, "rel-addr");
	unsigned long component, priority, port, rel_port, generation;
	char tail[JINGLE_TOKEN_MAX * 2];
	int wrote;

	if (!foundation || !protocol || !ip || !type ||
	    !attr_uint(doc, node, "component", &component) ||
	    !attr_uint(doc, node, "priority", &priority) ||
	    !attr_uint(doc, node, "port", &port) || port > 65535 ||
	    strpbrk(ip, " \t\r\n") || strpbrk(foundation, " \t\r\n"))
		return false;

	tail[0] = 0;
	if (rel_addr && attr_uint(doc, node, "rel-port", &rel_port) &&
	    !strpbrk(rel_addr, " \t\r\n"))
		snprintf(tail, sizeof(tail), " raddr %s rport %lu", rel_addr,
			 rel_port);

	wrote = snprintf(out, len, "a=candidate:%s %lu %s %lu %s %lu typ %s%s",
			 foundation, component, protocol, priority, ip, port,
			 type, tail);
	if (wrote < 0 || (size_t)wrote >= len)
		return false;

	if (attr_uint(doc, node, "generation", &generation)) {
		wrote = snprintf(out + strlen(out), len - strlen(out),
				 " generation %lu", generation);
		if (wrote < 0 || (size_t)wrote >= len - strlen(out))
			return false;
	}

	return true;
}

/**
 * payload_parse() - one <payload-type/>
 * @doc: the tree
 * @node: the element
 * @out: receives the values
 *
 * Return: true when the element carried an id.
 */
static bool payload_parse(const struct xml_doc *doc, unsigned int node,
			  struct jingle_payload *out)
{
	unsigned long id, clockrate, channels;

	memset(out, 0, sizeof(*out));
	if (!attr_uint(doc, node, "id", &id) || id > 127)
		return false;

	out->id = (uint8_t)id;
	attr_copy(doc, node, "name", out->name, sizeof(out->name));
	if (attr_uint(doc, node, "clockrate", &clockrate))
		out->clockrate = (uint32_t)clockrate;
	if (attr_uint(doc, node, "channels", &channels) && channels <= 255)
		out->channels = (uint8_t)channels;

	return true;
}

/**
 * description_parse() - the RTP description of a content
 * @doc: the tree
 * @node: the description element
 * @out: the content
 */
static void description_parse(const struct xml_doc *doc, unsigned int node,
			      struct jingle_content *out)
{
	const char *media = xml_attr_get(doc, node, "media");
	unsigned int child;

	out->description = true;
	out->audio = media && !strcmp(media, "audio");
	out->rtcp_mux = xml_child(doc, node, "rtcp-mux", JINGLE_NS_RTP) !=
			XML_NONE;

	child = xml_child(doc, node, "payload-type", JINGLE_NS_RTP);
	while (child != XML_NONE) {
		if (out->payloads >= JINGLE_PAYLOADS_MAX)
			out->payloads_dropped = true;
		else if (payload_parse(doc, child,
				       &out->payload[out->payloads]))
			out->payloads++;
		child = xml_next(doc, child, "payload-type", JINGLE_NS_RTP);
	}
}

/**
 * fingerprint_parse() - the <fingerprint/> of a transport
 * @doc: the tree
 * @node: the fingerprint element
 * @out: the transport
 */
static void fingerprint_parse(const struct xml_doc *doc, unsigned int node,
			      struct jingle_transport *out)
{
	const char *hash = xml_attr_get(doc, node, "hash");

	if (!hash || strcasecmp(hash, JINGLE_FP_HASH))
		return;

	out->fp_present = jingle_fingerprint_parse(xml_text_get(doc, node),
						   out->fp);
	out->setup = jingle_setup_of(xml_attr_get(doc, node, "setup"));
}

/**
 * transport_parse() - the ICE-UDP transport of a content
 * @doc: the tree
 * @node: the transport element
 * @out: the content
 */
static void transport_parse(const struct xml_doc *doc, unsigned int node,
			    struct jingle_content *out)
{
	struct jingle_transport *ice = &out->ice;
	struct jingle_candidate *slot;
	unsigned int child;

	out->transport = true;
	attr_copy(doc, node, "ufrag", ice->ufrag, sizeof(ice->ufrag));
	attr_copy(doc, node, "pwd", ice->pwd, sizeof(ice->pwd));

	child = xml_child(doc, node, "fingerprint", JINGLE_NS_DTLS);
	if (child != XML_NONE)
		fingerprint_parse(doc, child, ice);

	child = xml_child(doc, node, "candidate", JINGLE_NS_ICE);
	while (child != XML_NONE) {
		slot = &ice->candidate[ice->candidates];
		if (ice->candidates >= JINGLE_CANDIDATES_MAX)
			ice->candidates_dropped = true;
		else if (jingle_candidate_sdp(doc, child, slot->sdp,
					      sizeof(slot->sdp)))
			ice->candidates++;
		child = xml_next(doc, child, "candidate", JINGLE_NS_ICE);
	}
}

/**
 * content_parse() - one <content/>
 * @doc: the tree
 * @node: the content element
 * @out: receives the values
 *
 * Return: true when the content held a description or a transport.
 */
static bool content_parse(const struct xml_doc *doc, unsigned int node,
			  struct jingle_content *out)
{
	unsigned int child;

	memset(out, 0, sizeof(*out));
	attr_copy(doc, node, "name", out->name, sizeof(out->name));
	attr_copy(doc, node, "creator", out->creator, sizeof(out->creator));
	attr_copy(doc, node, "senders", out->senders, sizeof(out->senders));

	child = xml_child(doc, node, "description", JINGLE_NS_RTP);
	if (child != XML_NONE)
		description_parse(doc, child, out);

	child = xml_child(doc, node, "transport", JINGLE_NS_ICE);
	if (child != XML_NONE)
		transport_parse(doc, child, out);

	return out->description || out->transport;
}

/**
 * reason_parse() - the <reason/> of a session-terminate
 * @doc: the tree
 * @node: the reason element
 * @out: the message
 */
static void reason_parse(const struct xml_doc *doc, unsigned int node,
			 struct jingle_msg *out)
{
	unsigned int child = xml_child(doc, node, NULL, JINGLE_NS);
	unsigned int text;

	while (child != XML_NONE && out->reason == JINGLE_REASON_NONE) {
		out->reason = (enum jingle_reason)
			table_find(reason_names, JINGLE_REASON_MAX,
				   doc->node[child].name);
		child = xml_next(doc, child, NULL, JINGLE_NS);
	}

	text = xml_child(doc, node, "text", JINGLE_NS);
	if (text != XML_NONE)
		str_copy(out->reason_text, sizeof(out->reason_text),
			 xml_text_get(doc, text));
}

bool jingle_parse(const struct xml_doc *iq, struct jingle_msg *out)
{
	const char *type = xml_attr_get(iq, 0, "type");
	unsigned int jingle, child;
	bool content_taken = false;

	memset(out, 0, sizeof(*out));
	if (!xml_node_is(iq, 0, "iq", XMPP_NS_CLIENT) || !type ||
	    strcmp(type, "set"))
		return false;

	jingle = xml_child(iq, XML_NONE, "jingle", JINGLE_NS);
	if (jingle == XML_NONE)
		return false;

	out->action = jingle_action_of(xml_attr_get(iq, jingle, "action"));
	attr_copy(iq, jingle, "sid", out->sid, sizeof(out->sid));
	attr_copy(iq, jingle, "initiator", out->initiator,
		  sizeof(out->initiator));
	attr_copy(iq, jingle, "responder", out->responder,
		  sizeof(out->responder));
	if (out->action == JINGLE_ACTION_NONE || !out->sid[0])
		return false;

	child = xml_child(iq, jingle, "content", JINGLE_NS);
	while (child != XML_NONE) {
		out->contents++;
		if (!content_taken)
			content_taken = content_parse(iq, child, &out->content);
		child = xml_next(iq, child, "content", JINGLE_NS);
	}

	child = xml_child(iq, jingle, "reason", JINGLE_NS);
	if (child != XML_NONE)
		reason_parse(iq, child, out);

	child = xml_child(iq, jingle, NULL, JINGLE_NS_RTP_INFO);
	if (child != XML_NONE)
		out->info = (enum jingle_info)
			table_find(info_names, JINGLE_INFO_MAX,
				   iq->node[child].name);

	return true;
}

/* ---------------------------------------------------------------- writing */

void jingle_open(struct xml_w *writer, const char *to, const char *id,
		 enum jingle_action action, const char *sid,
		 const char *initiator, const char *responder)
{
	xml_w_start(writer, "iq");
	xml_w_attr(writer, "type", "set");
	xml_w_attr(writer, "to", to);
	xml_w_attr(writer, "id", id);
	xml_w_start(writer, "jingle");
	xml_w_attr(writer, "xmlns", JINGLE_NS);
	xml_w_attr(writer, "action", jingle_action_name(action));
	xml_w_attr(writer, "sid", sid);
	if (initiator && initiator[0])
		xml_w_attr(writer, "initiator", initiator);
	if (responder && responder[0])
		xml_w_attr(writer, "responder", responder);
}

void jingle_close(struct xml_w *writer)
{
	xml_w_end(writer);
	xml_w_end(writer);
}

void jingle_content_open(struct xml_w *writer, const struct jingle_local *local)
{
	xml_w_start(writer, "content");
	xml_w_attr(writer, "creator", local->creator);
	xml_w_attr(writer, "name", local->content_name);
	xml_w_attr(writer, "senders", JINGLE_SENDERS_BOTH);
}

void jingle_content_close(struct xml_w *writer)
{
	xml_w_end(writer);
}

void jingle_description_write(struct xml_w *writer,
			      const struct jingle_local *local)
{
	const struct jingle_payload *payload;
	unsigned int i;

	xml_w_start(writer, "description");
	xml_w_attr(writer, "xmlns", JINGLE_NS_RTP);
	xml_w_attr(writer, "media", "audio");

	for (i = 0; i < local->payloads && i < JINGLE_PAYLOADS_MAX; i++) {
		payload = &local->payload[i];
		xml_w_start(writer, "payload-type");
		xml_w_attr_uint(writer, "id", payload->id);
		if (payload->name[0])
			xml_w_attr(writer, "name", payload->name);
		if (payload->clockrate)
			xml_w_attr_uint(writer, "clockrate",
					payload->clockrate);
		if (payload->channels > 1)
			xml_w_attr_uint(writer, "channels", payload->channels);
		xml_w_end(writer);
	}

	if (local->rtcp_mux) {
		xml_w_start(writer, "rtcp-mux");
		xml_w_end(writer);
	}

	xml_w_end(writer);
}

void jingle_transport_open(struct xml_w *writer,
			   const struct jingle_local *local, bool fingerprint)
{
	char text[3 * JINGLE_FP_LEN];

	xml_w_start(writer, "transport");
	xml_w_attr(writer, "xmlns", JINGLE_NS_ICE);
	xml_w_attr(writer, "ufrag", local->ufrag);
	xml_w_attr(writer, "pwd", local->pwd);

	if (!fingerprint || !local->fp_present)
		return;

	jingle_fingerprint_text(local->fp, text, sizeof(text));
	xml_w_start(writer, "fingerprint");
	xml_w_attr(writer, "xmlns", JINGLE_NS_DTLS);
	xml_w_attr(writer, "hash", JINGLE_FP_HASH);
	xml_w_attr(writer, "setup", jingle_setup_name(local->setup));
	xml_w_text(writer, text);
	xml_w_end(writer);
}

void jingle_transport_close(struct xml_w *writer)
{
	xml_w_end(writer);
}

/**
 * token_next() - the next space separated token of a line
 * @cursor: the read position, moved past the token
 * @out: receives the token
 * @len: capacity of @out
 *
 * Return: true when a token was read.
 */
static bool token_next(const char **cursor, char *out, size_t len)
{
	const char *start = *cursor;
	const char *end;
	size_t tlen;

	while (*start == ' ')
		start++;
	if (!*start)
		return false;

	end = start;
	while (*end && *end != ' ')
		end++;

	tlen = (size_t)(end - start);
	if (tlen >= len)
		return false;

	memcpy(out, start, tlen);
	out[tlen] = 0;
	*cursor = end;

	return true;
}

/**
 * lower_copy() - a lower cased copy of a word
 * @src: the word
 * @out: receives the copy
 * @len: capacity of @out
 */
static void lower_copy(const char *src, char *out, size_t len)
{
	size_t i;

	for (i = 0; i + 1 < len && src[i]; i++)
		out[i] = (char)tolower((unsigned char)src[i]);
	out[i] = 0;
}

bool jingle_candidate_write(struct xml_w *writer, const char *sdp)
{
	char foundation[JINGLE_TOKEN_MAX], component[JINGLE_TOKEN_MAX];
	char protocol[JINGLE_TOKEN_MAX], priority[JINGLE_TOKEN_MAX];
	char ip[JINGLE_TOKEN_MAX], port[JINGLE_TOKEN_MAX];
	char typ[JINGLE_TOKEN_MAX], type[JINGLE_TOKEN_MAX];
	char key[JINGLE_TOKEN_MAX], value[JINGLE_TOKEN_MAX];
	char lower[JINGLE_TOKEN_MAX];
	char id[2 * JINGLE_TOKEN_MAX];
	const char *cursor = sdp;
	const char *generation = "0";
	char rel_addr[JINGLE_TOKEN_MAX] = "";
	char rel_port[JINGLE_TOKEN_MAX] = "";
	char gen[JINGLE_TOKEN_MAX] = "";

	if (!strncmp(cursor, "a=", 2))
		cursor += 2;
	if (strncmp(cursor, "candidate:", 10))
		return false;
	cursor += 10;

	if (!token_next(&cursor, foundation, sizeof(foundation)) ||
	    !token_next(&cursor, component, sizeof(component)) ||
	    !token_next(&cursor, protocol, sizeof(protocol)) ||
	    !token_next(&cursor, priority, sizeof(priority)) ||
	    !token_next(&cursor, ip, sizeof(ip)) ||
	    !token_next(&cursor, port, sizeof(port)) ||
	    !token_next(&cursor, typ, sizeof(typ)) || strcmp(typ, "typ") ||
	    !token_next(&cursor, type, sizeof(type)))
		return false;

	while (token_next(&cursor, key, sizeof(key)) &&
	       token_next(&cursor, value, sizeof(value))) {
		if (!strcmp(key, "raddr"))
			str_copy(rel_addr, sizeof(rel_addr), value);
		else if (!strcmp(key, "rport"))
			str_copy(rel_port, sizeof(rel_port), value);
		else if (!strcmp(key, "generation"))
			str_copy(gen, sizeof(gen), value);
	}
	if (gen[0])
		generation = gen;

	lower_copy(protocol, lower, sizeof(lower));
	snprintf(id, sizeof(id), "%s%s", foundation, component);

	xml_w_start(writer, "candidate");
	xml_w_attr(writer, "component", component);
	xml_w_attr(writer, "foundation", foundation);
	xml_w_attr(writer, "generation", generation);
	xml_w_attr(writer, "id", id);
	xml_w_attr(writer, "ip", ip);
	xml_w_attr(writer, "network", "0");
	xml_w_attr(writer, "port", port);
	xml_w_attr(writer, "priority", priority);
	xml_w_attr(writer, "protocol", lower);
	xml_w_attr(writer, "type", type);
	if (rel_addr[0] && rel_port[0]) {
		xml_w_attr(writer, "rel-addr", rel_addr);
		xml_w_attr(writer, "rel-port", rel_port);
	}
	xml_w_end(writer);

	return true;
}

void jingle_reason_write(struct xml_w *writer, enum jingle_reason reason,
			 const char *text)
{
	xml_w_start(writer, "reason");
	xml_w_start(writer, jingle_reason_name(reason));
	xml_w_end(writer);
	if (text && text[0]) {
		xml_w_start(writer, "text");
		xml_w_text(writer, text);
		xml_w_end(writer);
	}
	xml_w_end(writer);
}

void jingle_info_write(struct xml_w *writer, enum jingle_info info)
{
	xml_w_start(writer, jingle_info_name(info));
	xml_w_attr(writer, "xmlns", JINGLE_NS_RTP_INFO);
	xml_w_end(writer);
}
