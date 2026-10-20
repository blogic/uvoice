// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * SIP digest authentication. See sip/auth.h for the contracts.
 */
#include <stdio.h>
#include <string.h>

#include <openssl/evp.h>

#include "proto/sip/auth.h"

struct auth_hash {
	EVP_MD_CTX *ctx;
	bool ok;
};

/* the three challenge strings, unescaped into fixed buffers */
struct auth_fields {
	char realm[SIP_AUTH_REALM_MAX];
	char nonce[SIP_AUTH_NONCE_MAX];
	char opaque[SIP_AUTH_OPAQUE_MAX];
	bool opaque_present;
};

/* The hex digest widths this module answers: SHA-256 is 32 bytes and MD5 is
 * 16, so their hex forms are 64 and 32 characters. */
#define AUTH_HEXLEN_SHA256 64
#define AUTH_HEXLEN_MD5    32

const char *sip_auth_algo_name(enum sip_auth_algo algo)
{
	switch (algo) {
	case SIP_AUTH_ALGO_MD5:
		return "MD5";
	case SIP_AUTH_ALGO_MD5_SESS:
		return "MD5-sess";
	case SIP_AUTH_ALGO_SHA256:
		return "SHA-256";
	case SIP_AUTH_ALGO_SHA256_SESS:
		return "SHA-256-sess";
	}

	return "?";
}

bool sip_auth_algo_parse(struct sip_str s, enum sip_auth_algo *out)
{
	s = sip_str_trim(sip_quoted_strip(s));

	if (sip_str_eq_ci(s, "MD5"))
		*out = SIP_AUTH_ALGO_MD5;
	else if (sip_str_eq_ci(s, "MD5-sess"))
		*out = SIP_AUTH_ALGO_MD5_SESS;
	else if (sip_str_eq_ci(s, "SHA-256"))
		*out = SIP_AUTH_ALGO_SHA256;
	else if (sip_str_eq_ci(s, "SHA-256-sess"))
		*out = SIP_AUTH_ALGO_SHA256_SESS;
	else
		return false;

	return true;
}

bool sip_auth_algo_sess(enum sip_auth_algo algo)
{
	return algo == SIP_AUTH_ALGO_MD5_SESS || algo == SIP_AUTH_ALGO_SHA256_SESS;
}

size_t sip_auth_algo_hexlen(enum sip_auth_algo algo)
{
	if (algo == SIP_AUTH_ALGO_SHA256 || algo == SIP_AUTH_ALGO_SHA256_SESS)
		return AUTH_HEXLEN_SHA256;

	return AUTH_HEXLEN_MD5;
}

const char *sip_auth_kind_name(enum sip_auth_kind kind)
{
	switch (kind) {
	case SIP_AUTH_WWW:
		return "www";
	case SIP_AUTH_PROXY:
		return "proxy";
	case SIP_AUTH_KIND_MAX:
		break;
	}

	return "?";
}

enum sip_hdr_id sip_auth_chal_hdr(enum sip_auth_kind kind)
{
	return kind == SIP_AUTH_PROXY ? SIP_HDR_PROXY_AUTHENTICATE : SIP_HDR_WWW_AUTHENTICATE;
}

enum sip_hdr_id sip_auth_cred_hdr(enum sip_auth_kind kind)
{
	return kind == SIP_AUTH_PROXY ? SIP_HDR_PROXY_AUTHORIZATION : SIP_HDR_AUTHORIZATION;
}

const char *sip_auth_status_name(enum sip_auth_status st)
{
	switch (st) {
	case SIP_AUTH_OK:
		return "ok";
	case SIP_AUTH_ERR_NONE:
		return "no-challenge";
	case SIP_AUTH_ERR_SCHEME:
		return "scheme";
	case SIP_AUTH_ERR_ALGO:
		return "algorithm";
	case SIP_AUTH_ERR_QOP:
		return "qop";
	case SIP_AUTH_ERR_FIELD:
		return "field";
	case SIP_AUTH_ERR_REPEAT:
		return "repeat";
	case SIP_AUTH_ERR_STALE_LOOP:
		return "stale-loop";
	case SIP_AUTH_ERR_STATE:
		return "state";
	case SIP_AUTH_ERR_BUF:
		return "buffer";
	}

	return "?";
}

/* ------------------------------------------------------------------ hashing */

static void hash_init(struct auth_hash *h, enum sip_auth_algo algo)
{
	const EVP_MD *md = sip_auth_algo_hexlen(algo) == AUTH_HEXLEN_SHA256 ?
			   EVP_sha256() : EVP_md5();

	if (h->ok)
		h->ok = EVP_DigestInit_ex(h->ctx, md, NULL) == 1;
}

static void hash_add_len(struct auth_hash *h, const char *s, size_t len)
{
	if (h->ok)
		h->ok = EVP_DigestUpdate(h->ctx, s, len) == 1;
}

static void hash_add(struct auth_hash *h, const char *s)
{
	if (!s)
		return;

	hash_add_len(h, s, strlen(s));
}

static void hash_sep(struct auth_hash *h)
{
	hash_add_len(h, ":", 1);
}

/* out must hold sip_auth_algo_hexlen(algo) + 1 bytes */
static void hash_hex(struct auth_hash *h, char *out)
{
	static const char digits[] = "0123456789abcdef";
	uint8_t raw[EVP_MAX_MD_SIZE];
	unsigned int n = 0, i;

	if (h->ok)
		h->ok = EVP_DigestFinal_ex(h->ctx, raw, &n) == 1;
	if (!h->ok) {
		out[0] = 0;
		return;
	}

	for (i = 0; i < n; i++) {
		out[i * 2] = digits[raw[i] >> 4];
		out[i * 2 + 1] = digits[raw[i] & 0x0f];
	}
	out[n * 2] = 0;
}

bool sip_auth_cnonce_make(char *out, size_t olen, uint32_t r0, uint32_t r1)
{
	if (!out || olen < SIP_AUTH_CNONCE_MAX)
		return false;

	snprintf(out, olen, "%08x%08x", r0, r1);

	return true;
}

/* ---------------------------------------------------------------- response */

enum sip_auth_status sip_auth_response_calc(enum sip_auth_algo algo, const char *username,
					    const char *realm, const char *password,
					    const char *method, const char *uri,
					    const char *nonce, const char *cnonce, uint32_t nc,
					    const char *qop, char *out, size_t olen,
					    char *ha1_sess, size_t ha1_len)
{
	char ha1[SIP_AUTH_HEX_MAX], ha2[SIP_AUTH_HEX_MAX], nc_hex[9];
	bool sess = sip_auth_algo_sess(algo);
	bool use_qop = qop && *qop;
	struct auth_hash h;

	if (!username || !realm || !password || !method || !uri || !nonce || !out)
		return SIP_AUTH_ERR_FIELD;
	if (olen <= sip_auth_algo_hexlen(algo))
		return SIP_AUTH_ERR_BUF;
	if (ha1_sess && ha1_len <= sip_auth_algo_hexlen(algo))
		return SIP_AUTH_ERR_BUF;

	/* RFC 2617 3.2.2.1: qop=auth needs both, and the digest covers both. */
	if (use_qop && (!cnonce || !*cnonce || !nc))
		return SIP_AUTH_ERR_FIELD;
	if (use_qop && strcmp(qop, "auth"))
		return SIP_AUTH_ERR_QOP;

	/* RFC 2617 3.2.2.2: the -sess variants fold the nonce and the cnonce
	 * into HA1, which pins the session key to one nonce exchange. */
	if (sess && (!cnonce || !*cnonce))
		return SIP_AUTH_ERR_FIELD;

	h.ctx = EVP_MD_CTX_new();
	if (!h.ctx)
		return SIP_AUTH_ERR_ALGO;
	h.ok = true;

	hash_init(&h, algo);
	hash_add(&h, username);
	hash_sep(&h);
	hash_add(&h, realm);
	hash_sep(&h);
	hash_add(&h, password);
	hash_hex(&h, ha1);

	if (sess) {
		hash_init(&h, algo);
		hash_add(&h, ha1);
		hash_sep(&h);
		hash_add(&h, nonce);
		hash_sep(&h);
		hash_add(&h, cnonce);
		hash_hex(&h, ha1);
	}

	if (ha1_sess)
		memcpy(ha1_sess, ha1, strlen(ha1) + 1);

	/* RFC 2617 3.2.2.3, qop=auth and the qop-absent form share this HA2. */
	hash_init(&h, algo);
	hash_add(&h, method);
	hash_sep(&h);
	hash_add(&h, uri);
	hash_hex(&h, ha2);

	hash_init(&h, algo);
	hash_add(&h, ha1);
	hash_sep(&h);
	hash_add(&h, nonce);
	hash_sep(&h);

	if (use_qop) {
		snprintf(nc_hex, sizeof(nc_hex), "%08x", nc);
		hash_add(&h, nc_hex);
		hash_sep(&h);
		hash_add(&h, cnonce);
		hash_sep(&h);
		hash_add(&h, qop);
		hash_sep(&h);
	}

	hash_add(&h, ha2);
	hash_hex(&h, out);

	EVP_MD_CTX_free(h.ctx);

	return h.ok ? SIP_AUTH_OK : SIP_AUTH_ERR_ALGO;
}

/* ------------------------------------------------------------------- state */

void sip_auth_state_reset(struct sip_auth_state *st)
{
	if (st)
		memset(st, 0, sizeof(*st));
}

bool sip_auth_state_valid(const struct sip_auth_state *st)
{
	return st && st->valid;
}

void sip_auth_state_accept(struct sip_auth_state *st)
{
	if (st)
		st->stale_run = 0;
}

/*
 * A cached field goes back out inside a quoted string. sip_buf_add_quoted()
 * escapes a quote and a backslash, and nothing else, so a CR or an LF in a
 * challenge would break out of the header and add headers of the attacker's
 * choosing to the next REGISTER. Every control byte is refused here, which
 * also covers the NUL the message parser deliberately tolerates
 * (shared/msg.h "Tolerance policy"). Nothing in RFC 3261 25.1 puts a control
 * byte in a quoted string.
 */
static bool field_clean(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;

	for (; *p; p++) {
		if (*p < 0x20 || *p == 0x7f)
			return false;
	}

	return true;
}

/*
 * A quoted-string value is stored unescaped, because that is what the digest
 * covers, and refused when it does not fit. Truncation would build a well
 * formed credential with a wrong response, which reads to an operator like a
 * wrong password. A value that carries an embedded NUL is refused too: the C
 * string would stop there and the digest would cover half a nonce.
 */
static bool field_take(char *out, size_t olen, struct sip_str v)
{
	int n = sip_unquote(v, out, olen);

	if (n < 0 || strlen(out) != (size_t)n)
		return false;

	return field_clean(out);
}

static enum sip_auth_status chal_check(const struct sip_challenge *c, enum sip_auth_algo *algo,
				       bool *qop_auth, struct auth_fields *f)
{
	if (!(c->flags & SIP_CHAL_F_DIGEST))
		return SIP_AUTH_ERR_SCHEME;

	*algo = SIP_AUTH_ALGO_MD5;
	if (c->algorithm.len && !sip_auth_algo_parse(c->algorithm, algo))
		return SIP_AUTH_ERR_ALGO;

	/*
	 * No qop at all is the RFC 2069 form, which carriers send. auth wins
	 * when both are offered; auth-int alone is refused, because answering
	 * it means digesting every request body.
	 */
	*qop_auth = false;
	if (c->flags & SIP_CHAL_F_QOP) {
		if (!(c->flags & SIP_CHAL_F_QOP_AUTH))
			return SIP_AUTH_ERR_QOP;

		*qop_auth = true;
	}

	/*
	 * A -sess algorithm folds the cnonce into HA1, so the credentials must
	 * carry that cnonce or the server cannot recompute anything. The
	 * credential writer emits cnonce with qop only (RFC 3261 22.4,
	 * RFC 7616 3.4), so the qop-free -sess form would put a response on the
	 * wire that no server can check. It is refused instead.
	 */
	if (!*qop_auth && sip_auth_algo_sess(*algo))
		return SIP_AUTH_ERR_ALGO;

	if (!c->realm.p || !c->nonce.len)
		return SIP_AUTH_ERR_FIELD;

	memset(f, 0, sizeof(*f));
	if (!field_take(f->realm, sizeof(f->realm), c->realm))
		return SIP_AUTH_ERR_FIELD;
	if (!field_take(f->nonce, sizeof(f->nonce), c->nonce))
		return SIP_AUTH_ERR_FIELD;
	if (!f->nonce[0])
		return SIP_AUTH_ERR_FIELD;

	/* RFC 2617 3.2.1: an opaque value must be echoed unchanged, so one that
	 * does not fit fails the challenge instead of being dropped. */
	if (c->opaque.p) {
		if (!field_take(f->opaque, sizeof(f->opaque), c->opaque))
			return SIP_AUTH_ERR_FIELD;

		f->opaque_present = true;
	}

	return SIP_AUTH_OK;
}

enum sip_auth_status sip_auth_challenge_take(struct sip_auth_state *st,
					     const struct sip_challenge *c,
					     uint32_t r0, uint32_t r1)
{
	enum sip_auth_status status;
	enum sip_auth_algo algo;
	struct auth_fields f;
	bool qop_auth, same;

	if (!st || !c)
		return SIP_AUTH_ERR_FIELD;

	status = chal_check(c, &algo, &qop_auth, &f);
	if (status != SIP_AUTH_OK)
		return status;

	/*
	 * The same nonce back again means the credentials we built from it were
	 * refused, so building them a second time can only repeat the refusal.
	 * Stopping here is what keeps a wrong password from turning into a
	 * REGISTER loop and a carrier side lockout.
	 */
	same = st->valid && !strcmp(st->nonce, f.nonce) && !strcmp(st->realm, f.realm);
	if (same && st->attempts)
		return SIP_AUTH_ERR_REPEAT;

	if (!same && (c->flags & SIP_CHAL_F_STALE)) {
		st->stale_run++;
		if (st->stale_run > SIP_AUTH_STALE_MAX)
			return SIP_AUTH_ERR_STALE_LOOP;
	} else if (!same) {
		st->stale_run = 0;
	}

	st->stale = (c->flags & SIP_CHAL_F_STALE) != 0;
	st->algo = algo;
	st->algo_present = c->algorithm.len > 0;
	st->qop_auth = qop_auth;
	st->opaque_present = f.opaque_present;
	memcpy(st->realm, f.realm, sizeof(st->realm));
	memcpy(st->nonce, f.nonce, sizeof(st->nonce));
	memcpy(st->opaque, f.opaque, sizeof(st->opaque));
	st->response[0] = 0;
	st->ha1[0] = 0;
	st->nc = 0;
	st->attempts = 0;
	st->valid = true;
	sip_auth_cnonce_make(st->cnonce, sizeof(st->cnonce), r0, r1);

	return SIP_AUTH_OK;
}

enum sip_auth_status sip_auth_msg_challenge(struct sip_auth_state *st,
					    const struct sip_msg *resp,
					    enum sip_auth_kind kind, uint32_t r0, uint32_t r1)
{
	enum sip_auth_status first = SIP_AUTH_ERR_NONE, status;
	struct sip_challenge c;
	struct sip_chal_iter it;
	enum sip_auth_algo algo;
	struct auth_fields f;
	bool qop_auth;

	if (!st || !resp || kind >= SIP_AUTH_KIND_MAX)
		return SIP_AUTH_ERR_FIELD;
	if (!hdr_chal_first(resp, sip_auth_chal_hdr(kind), &it, &c))
		return SIP_AUTH_ERR_NONE;

	do {
		status = chal_check(&c, &algo, &qop_auth, &f);
		if (status == SIP_AUTH_OK)
			return sip_auth_challenge_take(st, &c, r0, r1);

		if (first == SIP_AUTH_ERR_NONE)
			first = status;
	} while (hdr_chal_next(&it, &c));

	return first;
}

bool sip_auth_challenges_take(struct sip_auth_state *auth,
			      const struct sip_msg *resp, unsigned int *retry,
			      uint32_t (*draw)(struct sip_txn_mgr *mgr),
			      struct sip_txn_mgr *mgr,
			      enum sip_auth_status *status)
{
	unsigned int k, fresh = 0;

	for (k = 0; k < SIP_AUTH_KIND_MAX; k++) {
		status[k] = sip_auth_msg_challenge(&auth[k], resp,
						   (enum sip_auth_kind)k,
						   draw(mgr), draw(mgr));
		if (status[k] != SIP_AUTH_OK)
			continue;

		if (auth[k].stale)
			retry[k] = 0;

		if (retry[k] < SIP_AUTH_RETRY_MAX)
			fresh++;
	}

	if (!fresh)
		return false;

	for (k = 0; k < SIP_AUTH_KIND_MAX; k++) {
		if (status[k] == SIP_AUTH_OK)
			retry[k]++;
	}

	return true;
}

enum sip_auth_status sip_auth_cred_make(struct sip_auth_state *st, const char *username,
					const char *password, const char *method,
					const char *uri, struct sip_cred *out)
{
	enum sip_auth_status status;
	uint32_t nc;

	if (!st || !out || !username || !password || !method || !uri)
		return SIP_AUTH_ERR_FIELD;
	if (!st->valid)
		return SIP_AUTH_ERR_STATE;

	/* Same rule as chal_check(): a -sess algorithm without qop cannot put
	 * its cnonce on the wire, so it must never reach a request. */
	if (!st->qop_auth && sip_auth_algo_sess(st->algo))
		return SIP_AUTH_ERR_ALGO;

	nc = st->qop_auth ? st->nc + 1 : 0;
	status = sip_auth_response_calc(st->algo, username, st->realm, password, method, uri,
					st->nonce, st->cnonce, nc,
					st->qop_auth ? "auth" : NULL, st->response,
					sizeof(st->response), st->ha1, sizeof(st->ha1));
	if (status != SIP_AUTH_OK)
		return status;

	memset(out, 0, sizeof(*out));
	out->username = username;
	out->realm = st->realm;
	out->nonce = st->nonce;
	out->uri = uri;
	out->response = st->response;
	out->opaque = st->opaque_present ? st->opaque : NULL;

	/* Echo the algorithm only when the challenge named one: a registrar
	 * that speaks the RFC 2069 form has no algorithm parameter to match. */
	if (st->algo_present)
		out->algorithm = sip_auth_algo_name(st->algo);

	if (st->qop_auth) {
		out->qop = "auth";
		out->cnonce = st->cnonce;
		out->nc = nc;
	}

	st->nc = nc;
	st->attempts++;

	return SIP_AUTH_OK;
}

enum sip_auth_status sip_auth_cred_write(struct sip_buf *b,
					 struct sip_auth_state *auth,
					 enum sip_auth_kind kind,
					 const char *username,
					 const char *password,
					 const char *method, const char *uri)
{
	struct sip_auth_state *st = &auth[kind];
	enum sip_auth_status status;
	struct sip_cred cred;

	if (!sip_auth_state_valid(st))
		return SIP_AUTH_OK;

	status = sip_auth_cred_make(st, username, password, method, uri, &cred);
	if (status == SIP_AUTH_OK)
		hdr_cred_write(b, sip_auth_cred_hdr(kind), &cred);

	return status;
}
