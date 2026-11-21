// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <libubox/utils.h>
#include <openssl/evp.h>

#include "proto/xmpp/disco.h"

#define DISCO_FEATURES_MAX	32U
#define DISCO_IDENTITIES_MAX	4U
#define DISCO_INPUT_MAX		2048U
#define DISCO_SHA1_LEN		20U

static const char *const disco_feature_table[] = {
	DISCO_NS_CAPS,
	DISCO_NS_INFO,
	"urn:xmpp:jingle-message:0",
	"urn:xmpp:jingle:1",
	"urn:xmpp:jingle:apps:dtls:0",
	"urn:xmpp:jingle:apps:rtp:1",
	"urn:xmpp:jingle:apps:rtp:audio",
	"urn:xmpp:jingle:transports:ice-udp:1",
	"urn:xmpp:ping",
};

static const struct disco_identity disco_identity_table[] = {
	{ DISCO_CATEGORY, DISCO_TYPE, "", DISCO_NAME },
};

const char *const *disco_features(unsigned int *count)
{
	*count = sizeof(disco_feature_table) / sizeof(disco_feature_table[0]);

	return disco_feature_table;
}

/**
 * str_cmp() - the qsort order of two strings
 * @first: a pointer to the first string
 * @second: a pointer to the second string
 *
 * Return: the strcmp() verdict.
 */
static int str_cmp(const void *first, const void *second)
{
	return strcmp(*(const char *const *)first,
		      *(const char *const *)second);
}

/**
 * identity_cmp() - the XEP-0115 order of two identities
 * @first: a pointer to the first identity
 * @second: a pointer to the second identity
 *
 * Category first, then type, then language, then name.
 *
 * Return: the order.
 */
static int identity_cmp(const void *first, const void *second)
{
	const struct disco_identity *one = first, *two = second;
	int verdict = strcmp(one->category, two->category);

	if (!verdict)
		verdict = strcmp(one->type, two->type);
	if (!verdict)
		verdict = strcmp(one->lang, two->lang);
	if (!verdict)
		verdict = strcmp(one->name, two->name);

	return verdict;
}

/**
 * input_append() - append one piece of the verification input
 * @input: the input buffer
 * @used: its fill, moved
 * @text: the piece
 *
 * Return: true when it fits.
 */
static bool input_append(char *input, size_t *used, const char *text)
{
	size_t len = strlen(text);

	if (*used + len >= DISCO_INPUT_MAX)
		return false;
	memcpy(input + *used, text, len);
	*used += len;
	input[*used] = 0;

	return true;
}

int disco_ver_compute(const struct disco_identity *identities,
		      unsigned int nidentities, const char *const *features,
		      unsigned int nfeatures, char *out, size_t len)
{
	struct disco_identity ids[DISCO_IDENTITIES_MAX];
	const char *vars[DISCO_FEATURES_MAX];
	char input[DISCO_INPUT_MAX];
	unsigned char digest[DISCO_SHA1_LEN];
	unsigned int digest_len = 0;
	size_t used = 0;
	unsigned int i;

	if (nidentities > DISCO_IDENTITIES_MAX ||
	    nfeatures > DISCO_FEATURES_MAX ||
	    len < B64_ENCODE_LEN(DISCO_SHA1_LEN))
		return -E2BIG;

	memcpy(ids, identities, nidentities * sizeof(ids[0]));
	memcpy(vars, features, nfeatures * sizeof(vars[0]));
	qsort(ids, nidentities, sizeof(ids[0]), identity_cmp);
	qsort(vars, nfeatures, sizeof(vars[0]), str_cmp);

	input[0] = 0;
	for (i = 0; i < nidentities; i++) {
		if (!input_append(input, &used, ids[i].category) ||
		    !input_append(input, &used, "/") ||
		    !input_append(input, &used, ids[i].type) ||
		    !input_append(input, &used, "/") ||
		    !input_append(input, &used, ids[i].lang) ||
		    !input_append(input, &used, "/") ||
		    !input_append(input, &used, ids[i].name) ||
		    !input_append(input, &used, "<"))
			return -E2BIG;
	}
	for (i = 0; i < nfeatures; i++) {
		if (!input_append(input, &used, vars[i]) ||
		    !input_append(input, &used, "<"))
			return -E2BIG;
	}

	if (!EVP_Digest(input, used, digest, &digest_len, EVP_sha1(), NULL) ||
	    digest_len != DISCO_SHA1_LEN)
		return -E2BIG;
	if (b64_encode(digest, DISCO_SHA1_LEN, out, len) < 0)
		return -E2BIG;

	return 0;
}

int disco_ver(char *out, size_t len)
{
	unsigned int nfeatures;
	const char *const *features = disco_features(&nfeatures);

	return disco_ver_compute(disco_identity_table, 1, features, nfeatures,
				 out, len);
}

void disco_info_write(struct xml_w *writer, const char *node)
{
	unsigned int nfeatures, i;
	const char *const *features = disco_features(&nfeatures);

	xml_w_start(writer, "query");
	xml_w_attr(writer, "xmlns", DISCO_NS_INFO);
	if (node)
		xml_w_attr(writer, "node", node);
	xml_w_start(writer, "identity");
	xml_w_attr(writer, "category", DISCO_CATEGORY);
	xml_w_attr(writer, "type", DISCO_TYPE);
	xml_w_attr(writer, "name", DISCO_NAME);
	xml_w_end(writer);
	for (i = 0; i < nfeatures; i++) {
		xml_w_start(writer, "feature");
		xml_w_attr(writer, "var", features[i]);
		xml_w_end(writer);
	}
	xml_w_end(writer);
}

void disco_caps_write(struct xml_w *writer)
{
	char ver[DISCO_VER_MAX];

	if (disco_ver(ver, sizeof(ver)))
		return;

	xml_w_start(writer, "c");
	xml_w_attr(writer, "xmlns", DISCO_NS_CAPS);
	xml_w_attr(writer, "hash", "sha-1");
	xml_w_attr(writer, "node", DISCO_NODE);
	xml_w_attr(writer, "ver", ver);
	xml_w_end(writer);
}
