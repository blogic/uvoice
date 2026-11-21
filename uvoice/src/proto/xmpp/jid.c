// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "proto/xmpp/jid.h"

#define JID_LOCAL_EXCLUDED	" \"&'/:<>@"

/**
 * part_take() - copy one part of a JID into its field
 * @begin: the first byte of the part
 * @end: one past its last byte
 * @out: receives the part
 * @lower: whether the part is lower-cased
 *
 * Return: true when the part fits.
 */
static bool part_take(const char *begin, const char *end, char *out,
		      bool lower)
{
	size_t len = (size_t)(end - begin);
	size_t i;

	if (len >= JID_PART_MAX)
		return false;

	for (i = 0; i < len; i++)
		out[i] = lower ? (char)tolower((unsigned char)begin[i]) :
				 begin[i];
	out[len] = 0;

	return true;
}

/**
 * local_ok() - whether a localpart avoids the excluded characters
 * @local: the localpart
 *
 * Return: true when every byte is allowed.
 */
static bool local_ok(const char *local)
{
	return strpbrk(local, JID_LOCAL_EXCLUDED) == NULL;
}

bool jid_parse(const char *text, struct jid *out)
{
	const char *at, *slash, *domain_end;

	memset(out, 0, sizeof(*out));
	if (!text || !*text)
		return false;

	slash = strchr(text, '/');
	at = memchr(text, '@', slash ? (size_t)(slash - text) : strlen(text));
	domain_end = slash ? slash : text + strlen(text);

	if (at) {
		if (at == text || !part_take(text, at, out->local, false) ||
		    !local_ok(out->local))
			return false;
		text = at + 1;
	}

	if (domain_end == text ||
	    !part_take(text, domain_end, out->domain, true))
		return false;

	if (!slash)
		return true;

	return slash[1] && part_take(slash + 1, slash + 1 + strlen(slash + 1),
				     out->resource, false);
}

void jid_bare(const struct jid *jid, char *out, size_t len)
{
	if (jid->local[0])
		snprintf(out, len, "%s@%s", jid->local, jid->domain);
	else
		snprintf(out, len, "%s", jid->domain);
}

void jid_full(const struct jid *jid, char *out, size_t len)
{
	char bare[JID_MAX];

	jid_bare(jid, bare, sizeof(bare));
	if (jid->resource[0])
		snprintf(out, len, "%s/%s", bare, jid->resource);
	else
		snprintf(out, len, "%s", bare);
}

void jid_text_bare(const char *text, char *out, size_t len)
{
	struct jid jid;

	if (jid_parse(text, &jid)) {
		jid_bare(&jid, out, len);
		return;
	}

	snprintf(out, len, "%s", text ? text : "");
}

bool jid_bare_eq(const struct jid *first, const struct jid *second)
{
	return !strcmp(first->local, second->local) &&
	       !strcmp(first->domain, second->domain);
}

bool jid_text_bare_eq(const char *first, const char *second)
{
	struct jid one, two;

	if (!jid_parse(first, &one) || !jid_parse(second, &two))
		return false;

	return jid_bare_eq(&one, &two);
}
