// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Service discovery of the XMPP unit (src/proto/xmpp/disco.c): the
 * disco#info answer of XEP-0030 with the fixed feature set of this phone,
 * and the entity capabilities hash of XEP-0115 that the presence carries,
 * so a peer learns the set without a query.
 *
 * The feature set is what the calling clients look for before they offer a
 * call: Jingle, its RTP application, audio, the ICE-UDP transport, DTLS-SRTP
 * and Jingle Message Initiation, beside disco#info, caps and ping.
 */
#ifndef UVOICE_XMPP_DISCO_H
#define UVOICE_XMPP_DISCO_H

#include <stddef.h>

#include "proto/xmpp/xml.h"

#define DISCO_NS_INFO		"http://jabber.org/protocol/disco#info"
#define DISCO_NS_ITEMS		"http://jabber.org/protocol/disco#items"
#define DISCO_NS_CAPS		"http://jabber.org/protocol/caps"
#define DISCO_NODE		"https://openwrt.org/uvoice"
#define DISCO_CATEGORY		"client"
#define DISCO_TYPE		"phone"
#define DISCO_NAME		"uvoice"
/* The base64 of a SHA-1 with its terminator. */
#define DISCO_VER_MAX		32U

struct disco_identity {
	const char *category;
	const char *type;
	const char *lang;		/* "" for none */
	const char *name;		/* "" for none */
};

/**
 * disco_features() - the feature set of this phone, sorted
 * @count: receives the count
 *
 * Return: the feature strings.
 */
const char *const *disco_features(unsigned int *count);

/**
 * disco_ver_compute() - the XEP-0115 section 5.1 verification string
 * @identities: the identities, in any order
 * @nidentities: how many
 * @features: the feature strings, in any order
 * @nfeatures: how many
 * @out: receives the base64 SHA-1
 * @len: capacity of @out, DISCO_VER_MAX suffices
 *
 * Return: 0, or -E2BIG when the sorted input or the output does not fit.
 */
int disco_ver_compute(const struct disco_identity *identities,
		      unsigned int nidentities, const char *const *features,
		      unsigned int nfeatures, char *out, size_t len);

/**
 * disco_ver() - the verification string of this phone
 * @out: receives the base64 SHA-1
 * @len: capacity of @out
 *
 * Return: 0, or -E2BIG.
 */
int disco_ver(char *out, size_t len);

/**
 * disco_info_write() - the disco#info query of an answer
 * @writer: the stanza writer, inside the iq element
 * @node: the node the query named, NULL for none
 */
void disco_info_write(struct xml_w *writer, const char *node);

/**
 * disco_caps_write() - the <c/> element of a presence
 * @writer: the stanza writer, inside the presence element
 */
void disco_caps_write(struct xml_w *writer);

#endif
