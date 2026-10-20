// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * HTTP digest authentication for SIP (RFC 3261 22, RFC 2617, RFC 7616).
 *
 * Dependencies. libc, the message and header layer, and OpenSSL libcrypto for
 * the message digests (openssl/evp.h). No uloop, no socket, no timer. The
 * response builder allocates one digest context and frees it before it
 * returns; nothing else here allocates. Every function is pure: the caller
 * owns the state struct, one per account and per challenge kind, and nothing
 * in this file keeps a pointer past the call.
 *
 * Two forms, both required. A carrier decides which one uvoice speaks:
 *
 *   qop=auth      RFC 2617 3.2.2.1. response = H(HA1:nonce:nc:cnonce:qop:HA2)
 *                 and the credentials carry cnonce, nc and qop.
 *   qop absent    RFC 2069 2.1.2. response = H(HA1:nonce:HA2) and the
 *                 credentials carry no cnonce, no nc and no qop. Carriers
 *                 send this form as realm, nonce and opaque, with no qop
 *                 and no algorithm.
 *
 * qop=auth-int is refused (SIP_AUTH_ERR_QOP), because it digests the body and
 * uvoice would have to buffer and rehash every request body to answer it. A
 * challenge that offers both takes auth.
 *
 * Opaque. RFC 2617 makes the echo mandatory: "the client MUST return the
 * opaque value unchanged". The state keeps it and sip_auth_cred_make() emits
 * it, quoted, whenever the challenge carried one.
 *
 * 401 and 407 are peers. A registrar challenge (401, WWW-Authenticate,
 * Authorization) and a proxy challenge (407, Proxy-Authenticate,
 * Proxy-Authorization) have separate nonces, separate nonce counts and
 * separate stale state. The caller keeps one struct sip_auth_state per kind
 * and answers both when both arrive.
 *
 * Stale. stale=true with a fresh nonce means the credentials were right and
 * the nonce expired: the caller retries and counts no credential failure. The
 * same nonce again, stale or not, means no progress, and
 * SIP_AUTH_ERR_REPEAT/SIP_AUTH_ERR_STALE_LOOP stop the exchange instead of
 * letting a broken registrar spin the account into a lockout.
 */
#ifndef UVOICE_SIP_AUTH_H
#define UVOICE_SIP_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "proto/shared/hdr.h"

/*
 * Field bounds. RFC 3261 bounds none of these, so every one of them refuses
 * what does not fit instead of truncating: a truncated nonce or realm produces
 * a well formed credential with a wrong response, which looks to the operator
 * like a wrong password.
 */
#define SIP_AUTH_REALM_MAX	128
#define SIP_AUTH_NONCE_MAX	256
#define SIP_AUTH_OPAQUE_MAX	128
#define SIP_AUTH_CNONCE_MAX	17	/* 16 hex digits and the NUL */
#define SIP_AUTH_HEX_MAX	65	/* SHA-256, 64 hex digits and the NUL */

/* consecutive stale=true challenges before the exchange is given up */
#define SIP_AUTH_STALE_MAX	3

enum sip_auth_algo {
	SIP_AUTH_ALGO_MD5 = 0,		/* the default when algorithm is absent */
	SIP_AUTH_ALGO_MD5_SESS,
	SIP_AUTH_ALGO_SHA256,
	SIP_AUTH_ALGO_SHA256_SESS,
};

const char *sip_auth_algo_name(enum sip_auth_algo algo);
bool sip_auth_algo_parse(struct sip_str s, enum sip_auth_algo *out);
size_t sip_auth_algo_hexlen(enum sip_auth_algo algo);

/*
 * The two session variants. They fold the cnonce into HA1, so they are usable
 * only with qop: hdr_cred_write() emits a cnonce with qop and not without
 * (RFC 3261 22.4), and a response computed over a cnonce the server never
 * receives cannot be checked. A -sess challenge that carries no qop is
 * refused with SIP_AUTH_ERR_ALGO.
 */
bool sip_auth_algo_sess(enum sip_auth_algo algo);

/*
 * Which challenge and credential headers a state belongs to. The caller keeps
 * an array of SIP_AUTH_KIND_MAX states per account.
 */
enum sip_auth_kind {
	SIP_AUTH_WWW = 0,		/* 401, WWW-Authenticate, Authorization */
	SIP_AUTH_PROXY,			/* 407, Proxy-Authenticate, Proxy-Authorization */
	SIP_AUTH_KIND_MAX,
};

enum sip_hdr_id sip_auth_chal_hdr(enum sip_auth_kind kind);
enum sip_hdr_id sip_auth_cred_hdr(enum sip_auth_kind kind);
const char *sip_auth_kind_name(enum sip_auth_kind kind);

enum sip_auth_status {
	SIP_AUTH_OK = 0,
	SIP_AUTH_ERR_NONE,		/* no challenge of that kind in the message */
	SIP_AUTH_ERR_SCHEME,		/* not Digest: Basic and NTLM are refused */
	SIP_AUTH_ERR_ALGO,		/* an algorithm this build cannot compute */
	SIP_AUTH_ERR_QOP,		/* auth-int alone, or an unknown qop list */
	SIP_AUTH_ERR_FIELD,		/* realm or nonce missing, empty or too long */
	SIP_AUTH_ERR_REPEAT,		/* the nonce we already answered: bad credentials */
	SIP_AUTH_ERR_STALE_LOOP,	/* SIP_AUTH_STALE_MAX stale refreshes in a row */
	SIP_AUTH_ERR_STATE,		/* no usable challenge cached */
	SIP_AUTH_ERR_BUF,		/* a caller buffer was too small */
};

const char *sip_auth_status_name(enum sip_auth_status st);

/*
 * The cached challenge of one account and one kind. The strings are stored
 * unescaped (RFC 3261 25.1 quoted-pair resolved), because that is the form the
 * digest is computed over; hdr_cred_write() escapes them again on the wire.
 *
 * nc counts the credentials built against the current nonce and is what goes
 * on the wire, so it starts at 1 for the first request (RFC 2617 3.2.2).
 */
struct sip_auth_state {
	char realm[SIP_AUTH_REALM_MAX];
	char nonce[SIP_AUTH_NONCE_MAX];
	char opaque[SIP_AUTH_OPAQUE_MAX];
	char cnonce[SIP_AUTH_CNONCE_MAX];
	char response[SIP_AUTH_HEX_MAX];
	char ha1[SIP_AUTH_HEX_MAX];	/* MD5-sess and SHA-256-sess only */

	enum sip_auth_algo algo;
	uint32_t nc;
	unsigned int stale_run;		/* consecutive stale refreshes */
	unsigned int attempts;		/* credentials built for this nonce */

	bool valid;
	bool qop_auth;			/* qop=auth selected; false is the 2069 form */
	bool algo_present;		/* the challenge named an algorithm */
	bool opaque_present;
	bool stale;			/* the challenge that filled this was stale */
};

void sip_auth_state_reset(struct sip_auth_state *st);
bool sip_auth_state_valid(const struct sip_auth_state *st);

/*
 * A successful request against the cached nonce. It clears the stale run, so a
 * carrier that rolls its nonce on every request never trips the loop guard.
 */
void sip_auth_state_accept(struct sip_auth_state *st);

/*
 * Take one parsed challenge into the state. r0 and r1 are two words of
 * randomness for the cnonce (RFC 7616 3.4.2 wants at least 64 bits); the
 * caller draws them from sip_txn_rand().
 *
 * SIP_AUTH_OK means the state is ready for sip_auth_cred_make(). st->stale
 * then tells the caller whether this was a stale refresh, which must not count
 * as a credential failure.
 */
enum sip_auth_status sip_auth_challenge_take(struct sip_auth_state *st,
					     const struct sip_challenge *c,
					     uint32_t r0, uint32_t r1);

/*
 * Find the first challenge of that kind the build can answer and take it.
 * RFC 7616 3.7 leaves the order to the server, so the first supported
 * challenge wins rather than the strongest one. When every challenge is
 * unusable the status of the first one is returned, which is the one an
 * operator needs to see.
 */
enum sip_auth_status sip_auth_msg_challenge(struct sip_auth_state *st,
					    const struct sip_msg *resp,
					    enum sip_auth_kind kind, uint32_t r0, uint32_t r1);

/* credential retries per challenge kind in one exchange */
#define SIP_AUTH_RETRY_MAX	1

struct sip_txn_mgr;

/*
 * Take the challenges of one 401 or 407 into auth, one state per kind.
 * draw(mgr) gives the two random words of each kind; the callers pass
 * sip_txn_rand() and their manager. status receives the result of each
 * kind; SIP_AUTH_ERR_NONE means no challenge of that kind.
 *
 * retry counts the credential retries per kind in this exchange. A stale
 * challenge sets the count of its kind to 0, because the credentials were
 * right. True means send the request again: a kind that took a challenge has
 * a count below SIP_AUTH_RETRY_MAX. Each kind that took one then counts one
 * more.
 */
bool sip_auth_challenges_take(struct sip_auth_state *auth,
			      const struct sip_msg *resp, unsigned int *retry,
			      uint32_t (*draw)(struct sip_txn_mgr *mgr),
			      struct sip_txn_mgr *mgr,
			      enum sip_auth_status *status);

/*
 * Build the credentials for one request. method and uri are the ones that go
 * on the wire (uri is the request URI). username, password and uri must
 * outlive the returned struct sip_cred, which points at them and at the state.
 *
 * Every call with qop=auth advances the nonce count, so a retransmission of an
 * unchanged request must not call it again: the transaction layer already owns
 * retransmission and repeats the wire bytes.
 */
enum sip_auth_status sip_auth_cred_make(struct sip_auth_state *st, const char *username,
					const char *password, const char *method,
					const char *uri, struct sip_cred *out);

/*
 * Write the credential header of one kind with sip_auth_cred_make() and
 * hdr_cred_write(). auth holds one state per kind. A state without a
 * challenge writes no header and returns SIP_AUTH_OK. Any other status is a
 * refusal of sip_auth_cred_make(), which writes nothing.
 */
enum sip_auth_status sip_auth_cred_write(struct sip_buf *b,
					 struct sip_auth_state *auth,
					 enum sip_auth_kind kind,
					 const char *username,
					 const char *password,
					 const char *method, const char *uri);

/*
 * The digest itself, exported because it is the part with published test
 * vectors (RFC 2617 3.5, RFC 7616 3.9.1). qop NULL or empty selects the
 * RFC 2069 form and then cnonce and nc are ignored. ha1_sess, when not NULL,
 * receives HA1: the plain one, or the session one for a -sess algorithm.
 */
enum sip_auth_status sip_auth_response_calc(enum sip_auth_algo algo, const char *username,
					    const char *realm, const char *password,
					    const char *method, const char *uri,
					    const char *nonce, const char *cnonce, uint32_t nc,
					    const char *qop, char *out, size_t olen,
					    char *ha1_sess, size_t ha1_len);

/* 16 hex digits from two words of randomness; olen must be SIP_AUTH_CNONCE_MAX */
bool sip_auth_cnonce_make(char *out, size_t olen, uint32_t r0, uint32_t r1);

#endif
