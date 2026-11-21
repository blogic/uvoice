// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <string.h>

#include "str.h"
#include "proto/xmpp/jmi.h"

#define XMPP_NS_CARBONS_JMI	"urn:xmpp:carbons:2"

static const char *const kind_names[JMI_KIND_MAX] = {
	[JMI_PROPOSE] = "propose",
	[JMI_RETRACT] = "retract",
	[JMI_RINGING] = "ringing",
	[JMI_PROCEED] = "proceed",
	[JMI_ACCEPT] = "accept",
	[JMI_REJECT] = "reject",
	[JMI_FINISH] = "finish",
};

const char *jmi_kind_name(enum jmi_kind kind)
{
	if (kind >= JMI_KIND_MAX || !kind_names[kind])
		return "";

	return kind_names[kind];
}

/**
 * kind_of() - the kind of an element name
 * @name: the name
 *
 * Return: the kind, JMI_NONE for a name outside the family.
 */
static enum jmi_kind kind_of(const char *name)
{
	unsigned int i;

	for (i = 1; i < JMI_KIND_MAX; i++) {
		if (!strcmp(kind_names[i], name))
			return (enum jmi_kind)i;
	}

	return JMI_NONE;
}

/**
 * carbon_unwrap() - the message a carbons copy forwards
 * @doc: the tree
 * @out: receives the wrapper flags
 *
 * Return: the inner message node, or XML_NONE when the stanza is no carbon.
 */
static unsigned int carbon_unwrap(const struct xml_doc *doc,
				  struct jmi_msg *out)
{
	unsigned int wrapper, forwarded;

	wrapper = xml_child(doc, XML_NONE, "sent", XMPP_NS_CARBONS_JMI);
	if (wrapper != XML_NONE)
		out->carbon_sent = true;
	else
		wrapper = xml_child(doc, XML_NONE, "received",
				    XMPP_NS_CARBONS_JMI);
	if (wrapper == XML_NONE)
		return XML_NONE;

	out->carbon = true;
	forwarded = xml_child(doc, wrapper, "forwarded", XMPP_NS_FORWARD);
	if (forwarded == XML_NONE)
		return XML_NONE;

	return xml_child(doc, forwarded, "message", XMPP_NS_CLIENT);
}

bool jmi_parse(const struct xml_doc *message, struct jmi_msg *out)
{
	unsigned int node, child, description;
	const char *from, *to, *id, *media;

	memset(out, 0, sizeof(*out));
	if (!xml_node_is(message, 0, "message", XMPP_NS_CLIENT))
		return false;

	node = carbon_unwrap(message, out);
	if (node == XML_NONE) {
		if (out->carbon)
			return false;
		node = 0;
	}

	child = xml_child(message, node, NULL, JMI_NS);
	while (child != XML_NONE && out->kind == JMI_NONE) {
		out->kind = kind_of(message->node[child].name);
		if (out->kind == JMI_NONE)
			child = xml_next(message, child, NULL, JMI_NS);
	}
	if (out->kind == JMI_NONE)
		return false;

	id = xml_attr_get(message, child, "id");
	if (!id || !id[0])
		return false;
	str_copy(out->id, sizeof(out->id), id);

	from = xml_attr_get(message, node, "from");
	to = xml_attr_get(message, node, "to");
	str_copy(out->from, sizeof(out->from), from ? from : "");
	str_copy(out->to, sizeof(out->to), to ? to : "");

	description = xml_child(message, child, "description", JINGLE_NS_RTP);
	media = description == XML_NONE ? NULL :
		xml_attr_get(message, description, "media");
	out->audio = media && !strcmp(media, "audio");

	return true;
}

void jmi_write(struct xml_w *writer, enum jmi_kind kind, const char *to,
	       const char *msg_id, const char *id)
{
	xml_w_start(writer, "message");
	xml_w_attr(writer, "to", to);
	xml_w_attr(writer, "type", "chat");
	xml_w_attr(writer, "id", msg_id);
	xml_w_start(writer, jmi_kind_name(kind));
	xml_w_attr(writer, "xmlns", JMI_NS);
	xml_w_attr(writer, "id", id);
	if (kind == JMI_PROPOSE) {
		xml_w_start(writer, "description");
		xml_w_attr(writer, "xmlns", JINGLE_NS_RTP);
		xml_w_attr(writer, "media", "audio");
		xml_w_end(writer);
	}
	xml_w_end(writer);
	xml_w_start(writer, "store");
	xml_w_attr(writer, "xmlns", XMPP_NS_HINTS);
	xml_w_end(writer);
	xml_w_end(writer);
}
