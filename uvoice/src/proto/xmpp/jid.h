// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The JID helpers of the XMPP unit (src/proto/xmpp/jid.c), RFC 7622. A JID
 * is localpart@domainpart/resourcepart with the first and the last part
 * optional. The domainpart is lower-cased here. The localpart and the
 * resourcepart keep their spelling: the servers this phone talks to
 * normalise the localpart before they route, so an exact compare on what
 * they deliver is the compare they make.
 */
#ifndef UVOICE_XMPP_JID_H
#define UVOICE_XMPP_JID_H

#include <stdbool.h>
#include <stddef.h>

#define JID_PART_MAX		128U
/* local@domain/resource with its terminator. */
#define JID_MAX			(3U * JID_PART_MAX)

struct jid {
	char local[JID_PART_MAX];	/* "" for a bare domain */
	char domain[JID_PART_MAX];
	char resource[JID_PART_MAX];	/* "" for a bare JID */
};

/**
 * jid_parse() - split a JID into its three parts
 * @text: the JID
 * @out: receives the parts, the domain lower-cased
 *
 * Refuses an empty domain, an empty localpart in front of '@', an empty
 * resourcepart behind '/', a part over JID_PART_MAX - 1 bytes, a localpart
 * with one of the characters RFC 7622 section 3.3.1 excludes and a
 * localpart with a space.
 *
 * Return: true when the JID is well formed.
 */
bool jid_parse(const char *text, struct jid *out);

/**
 * jid_bare() - write local@domain, or the domain alone
 * @jid: the parts
 * @out: receives the text
 * @len: capacity of @out
 */
void jid_bare(const struct jid *jid, char *out, size_t len);

/**
 * jid_full() - write local@domain/resource, the bare form without a resource
 * @jid: the parts
 * @out: receives the text
 * @len: capacity of @out
 */
void jid_full(const struct jid *jid, char *out, size_t len);

/**
 * jid_text_bare() - the bare form of a JID text
 * @text: the JID, bare or full
 * @out: receives local@domain, or the text as it is when it does not parse
 * @len: capacity of @out
 */
void jid_text_bare(const char *text, char *out, size_t len);

/**
 * jid_bare_eq() - whether two JIDs name one account
 * @first: a JID
 * @second: another JID
 *
 * Return: true when the localparts and the domainparts match.
 */
bool jid_bare_eq(const struct jid *first, const struct jid *second);

/**
 * jid_text_bare_eq() - whether two JID texts name one account
 * @first: a JID text, bare or full
 * @second: another JID text, bare or full
 *
 * Return: true when both parse and jid_bare_eq() holds.
 */
bool jid_text_bare_eq(const char *first, const char *second);

#endif
