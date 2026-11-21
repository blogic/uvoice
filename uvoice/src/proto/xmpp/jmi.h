// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Jingle Message Initiation (src/proto/xmpp/jmi.c), XEP-0353 version 0.8.
 * The messages that ring every device of an account before a Jingle session
 * exists: propose, retract, ringing, proceed, accept, reject and finish.
 * This layer reads them into values and writes them. The call layer owns
 * what they mean.
 *
 * A message of this family that another device of the same account sent
 * reaches this one as a carbon (XEP-0280), wrapped in <sent/> or
 * <received/> around <forwarded/>. The parser unwraps it and reports which
 * wrapper it was, because an <accept/> or a <proceed/> another device sent
 * is how this phone learns that a call was taken elsewhere.
 */
#ifndef UVOICE_XMPP_JMI_H
#define UVOICE_XMPP_JMI_H

#include <stdbool.h>

#include "proto/xmpp/jid.h"
#include "proto/xmpp/jingle.h"
#include "proto/xmpp/xml.h"

#define JMI_NS			"urn:xmpp:jingle-message:0"
#define XMPP_NS_HINTS		"urn:xmpp:hints"
#define XMPP_NS_FORWARD		"urn:xmpp:forward:0"
#define JMI_STANZA_MAX		1024U

enum jmi_kind {
	JMI_NONE = 0,
	JMI_PROPOSE,
	JMI_RETRACT,
	JMI_RINGING,
	JMI_PROCEED,
	JMI_ACCEPT,
	JMI_REJECT,
	JMI_FINISH,
	JMI_KIND_MAX,
};

/*
 * One parsed message. from and to are those of the message that carried
 * the element, the inner one for a carbon. audio says a propose named an
 * RTP description of media audio. carbon says the message arrived inside a
 * carbons copy, carbon_sent that the copy was of a message this account
 * sent.
 */
struct jmi_msg {
	enum jmi_kind kind;
	char id[JINGLE_SID_MAX];
	char from[JID_MAX];
	char to[JID_MAX];
	bool audio;
	bool carbon;
	bool carbon_sent;
};

/**
 * jmi_kind_name() - the element name of a kind
 * @kind: the kind
 *
 * Return: the name, "" for none.
 */
const char *jmi_kind_name(enum jmi_kind kind);

/**
 * jmi_parse() - read a message of the family
 * @message: the message stanza
 * @out: receives the values, cleared first
 *
 * Return: true when the message, or the message a carbon wrapped, carries
 * one element of the family with an id.
 */
bool jmi_parse(const struct xml_doc *message, struct jmi_msg *out);

/**
 * jmi_write() - one message of the family
 * @writer: the writer
 * @kind: the element
 * @to: the recipient, a bare or a full JID
 * @msg_id: the message id
 * @id: the session id the element carries
 *
 * A propose names an audio description. Every message asks for storage with
 * the hint of XEP-0334, so a device that is offline sees the call when it
 * returns.
 */
void jmi_write(struct xml_w *writer, enum jmi_kind kind, const char *to,
	       const char *msg_id, const char *id);

#endif
