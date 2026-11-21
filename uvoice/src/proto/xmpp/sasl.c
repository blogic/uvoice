// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 */
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <libubox/utils.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include "proto/xmpp/sasl.h"

#define SASL_GS2_HEADER		"n,,"
#define SASL_GS2_HEADER_B64	"biws"
#define SASL_CLIENT_KEY_TEXT	"Client Key"
#define SASL_SERVER_KEY_TEXT	"Server Key"
#define SASL_FINAL_BARE_PREFIX	"c=" SASL_GS2_HEADER_B64 ",r="
#define SASL_PROOF_PREFIX	",p="
#define SASL_SALT_MAX		128U
#define SASL_ITER_MAX		1000000U
#define SASL_USER_ESC_MAX	((SASL_USER_MAX - 1) * 3 + 1)
#define SASL_AUTH_PIECES	5U

struct sasl_piece {
	const void *data;
	size_t len;
};

struct scram_challenge {
	const char *nonce;
	size_t nonce_len;
	uint8_t salt[SASL_SALT_MAX];
	size_t salt_len;
	unsigned int iterations;
};

struct scram_keys {
	uint8_t salted[SASL_HASH_MAX];
	uint8_t client_key[SASL_HASH_MAX];
	uint8_t stored_key[SASL_HASH_MAX];
	uint8_t client_sig[SASL_HASH_MAX];
	uint8_t server_key[SASL_HASH_MAX];
	uint8_t proof[SASL_HASH_MAX];
};

static const char *const sasl_mech_names[] = {
	[SASL_MECH_NONE] = "",
	[SASL_MECH_PLAIN] = "PLAIN",
	[SASL_MECH_SCRAM_SHA_1] = "SCRAM-SHA-1",
	[SASL_MECH_SCRAM_SHA_256] = "SCRAM-SHA-256",
};

/**
 * mech_md() - the OpenSSL digest of a SCRAM mechanism
 * @mech: the mechanism
 *
 * Return: the digest, NULL for PLAIN and for none.
 */
static const EVP_MD *mech_md(enum sasl_mech mech)
{
	switch (mech) {
	case SASL_MECH_SCRAM_SHA_1:
		return EVP_sha1();
	case SASL_MECH_SCRAM_SHA_256:
		return EVP_sha256();
	default:
		return NULL;
	}
}

/**
 * state_fail() - mark the authentication as failed
 * @client: the client
 * @err: the negative errno to hand back
 *
 * Return: @err.
 */
static int state_fail(struct sasl_client *client, int err)
{
	client->state = SASL_ST_FAILED;

	return err;
}

/**
 * char_escape() - the n= attribute spelling of one user name character
 * @letter: the character
 *
 * RFC 5802 section 5.1 replaces "," with "=2C" and "=" with "=3D".
 *
 * Return: the three character replacement, NULL when @letter stays as it is.
 */
static const char *char_escape(char letter)
{
	if (letter == ',')
		return "=2C";
	if (letter == '=')
		return "=3D";

	return NULL;
}

/**
 * user_escape() - write a user name as the n= attribute value spells it
 * @user: the user name
 * @out: receives the escaped name, NUL terminated
 * @len: capacity of @out
 *
 * Return: the escaped length, or -ENOSPC.
 */
static int user_escape(const char *user, char *out, size_t len)
{
	size_t pos = 0;

	for (; *user; user++) {
		const char *piece = char_escape(*user);
		size_t piece_len = piece ? strlen(piece) : 1;

		if (pos + piece_len >= len)
			return -ENOSPC;
		if (piece)
			memcpy(out + pos, piece, piece_len);
		else
			out[pos] = *user;
		pos += piece_len;
	}
	out[pos] = 0;

	return (int)pos;
}

/**
 * attr_take() - split the next attribute off a comma separated list
 * @cursor: the list, moved past the attribute and its separator
 * @value: receives the start of the value
 * @value_len: receives the length of the value
 *
 * Return: the attribute letter, or 0 at the end of the list or on a token
 * that is not "letter=value".
 */
static char attr_take(const char **cursor, const char **value,
		      size_t *value_len)
{
	const char *start = *cursor;
	const char *end;

	if (!isalpha((unsigned char)start[0]) || start[1] != '=')
		return 0;
	end = strchr(start, ',');
	if (!end)
		end = start + strlen(start);
	*value = start + 2;
	*value_len = (size_t)(end - *value);
	*cursor = *end ? end + 1 : end;

	return *value_len ? start[0] : 0;
}

/**
 * iterations_parse() - read the i= attribute
 * @value: the decimal digits
 * @value_len: how many
 * @iterations: receives the count
 *
 * Return: 0, or -EPROTO for a count that is not a number, is zero or is
 * above SASL_ITER_MAX.
 */
static int iterations_parse(const char *value, size_t value_len,
			    unsigned int *iterations)
{
	unsigned int count = 0;
	size_t i;

	for (i = 0; i < value_len; i++) {
		if (value[i] < '0' || value[i] > '9')
			return -EPROTO;
		count = count * 10 + (unsigned int)(value[i] - '0');
		if (count > SASL_ITER_MAX)
			return -EPROTO;
	}
	if (!count)
		return -EPROTO;
	*iterations = count;

	return 0;
}

/**
 * salt_decode() - read the s= attribute
 * @value: the base64 text
 * @value_len: its length
 * @challenge: receives the salt bytes
 *
 * Return: 0, or -EPROTO for text that is not base64 or a salt over
 * SASL_SALT_MAX bytes.
 */
static int salt_decode(const char *value, size_t value_len,
		       struct scram_challenge *challenge)
{
	char text[B64_ENCODE_LEN(SASL_SALT_MAX)];
	int decoded;

	if (value_len >= sizeof(text))
		return -EPROTO;
	memcpy(text, value, value_len);
	text[value_len] = 0;
	decoded = b64_decode(text, challenge->salt, sizeof(challenge->salt));
	if (decoded <= 0)
		return -EPROTO;
	challenge->salt_len = (size_t)decoded;

	return 0;
}

/**
 * challenge_parse() - read the server-first message
 * @server_first: the message, NUL terminated
 * @challenge: receives the nonce, the salt and the iteration count
 *
 * The nonce points into @server_first. An m= attribute names a mandatory
 * extension this client lacks, so it fails the parse wherever it stands.
 * Attributes after the iteration count are optional extensions and are
 * skipped.
 *
 * Return: 0 or -EPROTO.
 */
static int challenge_parse(const char *server_first,
			   struct scram_challenge *challenge)
{
	const char *cursor = server_first;
	const char *value;
	size_t value_len;
	int ret;

	if (attr_take(&cursor, &value, &value_len) != 'r')
		return -EPROTO;
	challenge->nonce = value;
	challenge->nonce_len = value_len;

	if (attr_take(&cursor, &value, &value_len) != 's')
		return -EPROTO;
	ret = salt_decode(value, value_len, challenge);
	if (ret)
		return ret;

	if (attr_take(&cursor, &value, &value_len) != 'i')
		return -EPROTO;
	ret = iterations_parse(value, value_len, &challenge->iterations);
	if (ret)
		return ret;

	for (;;) {
		char attr = attr_take(&cursor, &value, &value_len);

		if (!attr)
			return 0;
		if (attr == 'm')
			return -EPROTO;
	}
}

/**
 * nonce_extends() - check that the server nonce starts with ours
 * @client: the client
 * @challenge: the parsed server-first
 *
 * Return: true when it does.
 */
static bool nonce_extends(const struct sasl_client *client,
			  const struct scram_challenge *challenge)
{
	size_t own_len = strlen(client->nonce);

	return challenge->nonce_len >= own_len &&
	       !memcmp(challenge->nonce, client->nonce, own_len);
}

/**
 * hmac_calc() - HMAC over a list of pieces
 * @md: the digest
 * @key: the key
 * @key_len: its length
 * @pieces: the data, hashed in order
 * @count: how many pieces
 * @out: receives the MAC
 * @out_len: the MAC length, which is the digest size
 *
 * Return: 0, or -EIO when OpenSSL refuses.
 */
static int hmac_calc(const EVP_MD *md, const uint8_t *key, size_t key_len,
		     const struct sasl_piece *pieces, unsigned int count,
		     uint8_t *out, size_t out_len)
{
	const char *digest_name = EVP_MD_get0_name(md);
	OSSL_PARAM params[2];
	EVP_MAC_CTX *ctx;
	EVP_MAC *mac;
	size_t written = 0;
	unsigned int i;
	bool ok;

	mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
	if (!mac)
		return -EIO;
	ctx = EVP_MAC_CTX_new(mac);
	EVP_MAC_free(mac);
	if (!ctx)
		return -EIO;

	params[0] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,
						     (char *)digest_name, 0);
	params[1] = OSSL_PARAM_construct_end();
	ok = EVP_MAC_init(ctx, key, key_len, params) == 1;
	for (i = 0; ok && i < count; i++)
		ok = EVP_MAC_update(ctx, pieces[i].data, pieces[i].len) == 1;
	if (ok)
		ok = EVP_MAC_final(ctx, out, &written, out_len) == 1;
	EVP_MAC_CTX_free(ctx);

	return ok && written == out_len ? 0 : -EIO;
}

/**
 * keys_derive() - compute the SCRAM keys, the proof and the server signature
 * @client: the client, receives the server signature
 * @challenge: the parsed server-first
 * @auth_message: the SASL_AUTH_PIECES pieces of the AuthMessage
 * @keys: receives SaltedPassword, ClientKey, StoredKey, ClientSignature,
 *        ServerKey and ClientProof
 *
 * Return: 0, or -EIO when OpenSSL refuses.
 */
static int keys_derive(struct sasl_client *client,
		       const struct scram_challenge *challenge,
		       const struct sasl_piece *auth_message,
		       struct scram_keys *keys)
{
	const struct sasl_piece client_key_text = {
		SASL_CLIENT_KEY_TEXT, sizeof(SASL_CLIENT_KEY_TEXT) - 1
	};
	const struct sasl_piece server_key_text = {
		SASL_SERVER_KEY_TEXT, sizeof(SASL_SERVER_KEY_TEXT) - 1
	};
	const EVP_MD *md = mech_md(client->mech);
	size_t hash_len = client->hash_len;
	unsigned int stored_len = 0;
	size_t i;
	int ret;

	if (PKCS5_PBKDF2_HMAC(client->pass, (int)strlen(client->pass),
			      challenge->salt, (int)challenge->salt_len,
			      (int)challenge->iterations, md, (int)hash_len,
			      keys->salted) != 1)
		return -EIO;
	ret = hmac_calc(md, keys->salted, hash_len, &client_key_text, 1,
			keys->client_key, hash_len);
	if (ret)
		return ret;
	if (EVP_Digest(keys->client_key, hash_len, keys->stored_key,
		       &stored_len, md, NULL) != 1 || stored_len != hash_len)
		return -EIO;
	ret = hmac_calc(md, keys->stored_key, hash_len, auth_message,
			SASL_AUTH_PIECES, keys->client_sig, hash_len);
	if (ret)
		return ret;
	for (i = 0; i < hash_len; i++)
		keys->proof[i] = keys->client_key[i] ^ keys->client_sig[i];
	ret = hmac_calc(md, keys->salted, hash_len, &server_key_text, 1,
			keys->server_key, hash_len);
	if (ret)
		return ret;

	return hmac_calc(md, keys->server_key, hash_len, auth_message,
			 SASL_AUTH_PIECES, client->server_signature, hash_len);
}

/**
 * server_first_keep() - copy the server-first message into the client
 * @client: the client
 * @challenge: the message
 * @clen: its length
 *
 * Return: 0, or -EPROTO for an empty message, one over SASL_MSG_MAX - 1
 * bytes or one holding a NUL.
 */
static int server_first_keep(struct sasl_client *client,
			     const char *challenge, size_t clen)
{
	if (!challenge || !clen || clen >= sizeof(client->server_first))
		return -EPROTO;
	if (memchr(challenge, 0, clen))
		return -EPROTO;
	memcpy(client->server_first, challenge, clen);
	client->server_first[clen] = 0;

	return 0;
}

/**
 * auth_message_fill() - lay out the AuthMessage of RFC 5802 section 3
 * @pieces: SASL_AUTH_PIECES entries
 * @client: the client, holding client-first-bare and server-first
 * @final_bare: client-final-message-without-proof
 * @final_bare_len: its length
 */
static void auth_message_fill(struct sasl_piece *pieces,
			      const struct sasl_client *client,
			      const char *final_bare, size_t final_bare_len)
{
	pieces[0].data = client->client_first_bare;
	pieces[0].len = strlen(client->client_first_bare);
	pieces[1].data = ",";
	pieces[1].len = 1;
	pieces[2].data = client->server_first;
	pieces[2].len = strlen(client->server_first);
	pieces[3].data = ",";
	pieces[3].len = 1;
	pieces[4].data = final_bare;
	pieces[4].len = final_bare_len;
}

/**
 * proof_write() - append ",p=" and the base64 proof
 * @proof: the ClientProof bytes
 * @proof_len: how many
 * @out: the end of client-final-message-without-proof
 * @len: capacity left in @out
 *
 * Return: 0 or -ENOSPC.
 */
static int proof_write(const uint8_t *proof, size_t proof_len, char *out,
		       size_t len)
{
	size_t prefix_len = strlen(SASL_PROOF_PREFIX);

	if (len <= prefix_len)
		return -ENOSPC;
	memcpy(out, SASL_PROOF_PREFIX, prefix_len);
	out += prefix_len;
	len -= prefix_len;
	if (b64_encode(proof, proof_len, out, len) < 0)
		return -ENOSPC;

	return 0;
}

/**
 * first_plain() - the PLAIN initial response of RFC 4616
 * @client: the client
 * @out: receives "\0user\0pass"
 * @len: capacity of @out
 *
 * Return: the message length, which counts its two NUL bytes, or -ENOSPC.
 */
static int first_plain(struct sasl_client *client, char *out, size_t len)
{
	size_t user_len = strlen(client->user);
	size_t pass_len = strlen(client->pass);
	size_t total = 1 + user_len + 1 + pass_len;

	if (len <= total)
		return -ENOSPC;
	out[0] = 0;
	memcpy(out + 1, client->user, user_len);
	out[1 + user_len] = 0;
	memcpy(out + 2 + user_len, client->pass, pass_len);
	out[total] = 0;
	client->state = SASL_ST_FIRST_SENT;

	return (int)total;
}

/**
 * first_scram() - the SCRAM client-first message
 * @client: the client, keeps client-first-bare
 * @out: receives "n,,n=user,r=nonce"
 * @len: capacity of @out
 *
 * Return: the message length, or -ENOSPC.
 */
static int first_scram(struct sasl_client *client, char *out, size_t len)
{
	char escaped[SASL_USER_ESC_MAX];
	int written;

	written = user_escape(client->user, escaped, sizeof(escaped));
	if (written < 0)
		return written;
	written = snprintf(client->client_first_bare,
			   sizeof(client->client_first_bare), "n=%s,r=%s",
			   escaped, client->nonce);
	if (written < 0 || (size_t)written >= sizeof(client->client_first_bare))
		return -ENOSPC;
	written = snprintf(out, len, SASL_GS2_HEADER "%s",
			   client->client_first_bare);
	if (written < 0 || (size_t)written >= len)
		return -ENOSPC;
	client->state = SASL_ST_FIRST_SENT;

	return written;
}

/**
 * verify_plain() - take the empty server-final of PLAIN
 * @client: the client
 * @flen: the length of the final message
 *
 * Return: 0, -EINVAL out of order, -EPROTO when the server sent data.
 */
static int verify_plain(struct sasl_client *client, size_t flen)
{
	if (client->state != SASL_ST_FIRST_SENT)
		return -EINVAL;
	if (flen)
		return state_fail(client, -EPROTO);
	client->state = SASL_ST_DONE;

	return 0;
}

/**
 * verify_signature() - compare the v= attribute with the kept signature
 * @client: the client
 * @value: the base64 text after "v="
 * @value_len: how much of the message is left
 *
 * Return: 0, -EACCES when the signature differs, -EPROTO for text that is
 * not base64.
 */
static int verify_signature(struct sasl_client *client, const char *value,
			    size_t value_len)
{
	char text[B64_ENCODE_LEN(SASL_HASH_MAX)];
	uint8_t signature[B64_DECODE_LEN(sizeof(text))];
	const char *end = memchr(value, ',', value_len);
	int decoded;

	if (end)
		value_len = (size_t)(end - value);
	if (value_len >= sizeof(text))
		return state_fail(client, -EPROTO);
	memcpy(text, value, value_len);
	text[value_len] = 0;
	decoded = b64_decode(text, signature, sizeof(signature));
	if (decoded < 0)
		return state_fail(client, -EPROTO);
	if ((size_t)decoded != client->hash_len ||
	    CRYPTO_memcmp(signature, client->server_signature,
			  client->hash_len))
		return state_fail(client, -EACCES);
	client->state = SASL_ST_DONE;

	return 0;
}

/**
 * sasl_mech_name() - the IANA name of a mechanism
 * @mech: the mechanism
 *
 * Return: "SCRAM-SHA-256", "SCRAM-SHA-1", "PLAIN" or "" for none.
 */
const char *sasl_mech_name(enum sasl_mech mech)
{
	if ((unsigned int)mech >= ARRAY_SIZE(sasl_mech_names))
		return "";

	return sasl_mech_names[mech];
}

/**
 * sasl_mech_of() - the mechanism of an IANA name
 * @name: the name, as the server's <mechanism/> carries it
 *
 * Return: the mechanism, SASL_MECH_NONE for one this client lacks.
 */
enum sasl_mech sasl_mech_of(const char *name)
{
	unsigned int i;

	if (!name)
		return SASL_MECH_NONE;
	for (i = SASL_MECH_PLAIN; i < ARRAY_SIZE(sasl_mech_names); i++)
		if (!strcmp(name, sasl_mech_names[i]))
			return (enum sasl_mech)i;

	return SASL_MECH_NONE;
}

/**
 * sasl_mech_pick() - the strongest mechanism of an offered set
 * @offered: the names the server listed
 * @count: how many
 * @allow_plain: whether PLAIN may be picked, true over TLS only
 *
 * Return: SCRAM-SHA-256 before SCRAM-SHA-1 before PLAIN, or SASL_MECH_NONE.
 */
enum sasl_mech sasl_mech_pick(const char *const *offered, unsigned int count,
			      bool allow_plain)
{
	enum sasl_mech best = SASL_MECH_NONE;
	unsigned int i;

	for (i = 0; i < count; i++) {
		enum sasl_mech mech = sasl_mech_of(offered[i]);

		if (mech == SASL_MECH_PLAIN && !allow_plain)
			continue;
		if (mech > best)
			best = mech;
	}

	return best;
}

/**
 * sasl_client_init() - start one authentication
 * @client: the client, zeroed by the caller
 * @mech: the mechanism to run
 * @user: the authentication identity, the localpart of the JID
 * @pass: the password
 * @nonce: SASL_NONCE_BYTES random bytes, ignored by PLAIN
 *
 * Return: 0, or -EINVAL for a mechanism, user or password this client refuses.
 */
int sasl_client_init(struct sasl_client *client, enum sasl_mech mech,
		     const char *user, const char *pass,
		     const uint8_t nonce[SASL_NONCE_BYTES])
{
	const EVP_MD *md = mech_md(mech);
	size_t user_len, pass_len;

	if (mech != SASL_MECH_PLAIN && !md)
		return -EINVAL;
	if (!user || !pass || (md && !nonce))
		return -EINVAL;
	user_len = strlen(user);
	pass_len = strlen(pass);
	if (!user_len || user_len >= SASL_USER_MAX || pass_len >= SASL_PASS_MAX)
		return -EINVAL;

	memset(client, 0, sizeof(*client));
	client->mech = mech;
	client->state = SASL_ST_INIT;
	memcpy(client->user, user, user_len + 1);
	memcpy(client->pass, pass, pass_len + 1);
	if (!md)
		return 0;
	client->hash_len = (size_t)EVP_MD_get_size(md);
	if (b64_encode(nonce, SASL_NONCE_BYTES, client->nonce,
		       sizeof(client->nonce)) < 0)
		return -EINVAL;

	return 0;
}

/**
 * sasl_client_first() - the initial response
 * @client: the client
 * @out: receives the message in the clear
 * @len: capacity of @out
 *
 * Return: the message length, or a negative errno.
 */
int sasl_client_first(struct sasl_client *client, char *out, size_t len)
{
	if (client->state != SASL_ST_INIT)
		return -EINVAL;
	if (client->mech == SASL_MECH_PLAIN)
		return first_plain(client, out, len);
	if (mech_md(client->mech))
		return first_scram(client, out, len);

	return -EINVAL;
}

/**
 * sasl_client_step() - answer the server-first message of SCRAM
 * @client: the client
 * @challenge: the server-first message in the clear
 * @clen: its length
 * @out: receives the client-final message in the clear
 * @len: capacity of @out
 *
 * Return: the message length, -EPROTO for a server-first this client cannot
 * read or whose nonce does not start with ours, or another negative errno.
 */
int sasl_client_step(struct sasl_client *client, const char *challenge,
		     size_t clen, char *out, size_t len)
{
	struct sasl_piece auth_message[SASL_AUTH_PIECES];
	struct scram_challenge parsed;
	struct scram_keys keys;
	size_t bare_len, total;
	int ret;

	if (!mech_md(client->mech) || client->state != SASL_ST_FIRST_SENT)
		return -EINVAL;
	ret = server_first_keep(client, challenge, clen);
	if (ret)
		return state_fail(client, ret);
	ret = challenge_parse(client->server_first, &parsed);
	if (ret)
		return state_fail(client, ret);
	if (!nonce_extends(client, &parsed))
		return state_fail(client, -EPROTO);

	bare_len = strlen(SASL_FINAL_BARE_PREFIX) + parsed.nonce_len;
	total = bare_len + strlen(SASL_PROOF_PREFIX) +
		B64_ENCODE_LEN(client->hash_len) - 1;
	if (len <= total)
		return -ENOSPC;
	snprintf(out, len, SASL_FINAL_BARE_PREFIX "%.*s",
		 (int)parsed.nonce_len, parsed.nonce);
	auth_message_fill(auth_message, client, out, bare_len);

	ret = keys_derive(client, &parsed, auth_message, &keys);
	if (!ret)
		ret = proof_write(keys.proof, client->hash_len, out + bare_len,
				  len - bare_len);
	explicit_bzero(&keys, sizeof(keys));
	if (ret)
		return state_fail(client, ret);
	client->state = SASL_ST_FINAL_SENT;

	return (int)total;
}

/**
 * sasl_client_verify() - check the server-final message of SCRAM
 * @client: the client
 * @final: the server-final message in the clear, from <success/>
 * @flen: its length
 *
 * PLAIN takes an empty final and succeeds.
 *
 * Return: 0 when the server proved it knows the password, -EACCES when the
 * signature differs or the server reported an error, -EPROTO for a message
 * this client cannot read.
 */
int sasl_client_verify(struct sasl_client *client, const char *final,
		       size_t flen)
{
	if (client->mech == SASL_MECH_PLAIN)
		return verify_plain(client, flen);
	if (!mech_md(client->mech) || client->state != SASL_ST_FINAL_SENT)
		return -EINVAL;
	if (!final || flen < 2 || memchr(final, 0, flen))
		return state_fail(client, -EPROTO);
	if (final[0] == 'e' && final[1] == '=')
		return state_fail(client, -EACCES);
	if (final[0] != 'v' || final[1] != '=')
		return state_fail(client, -EPROTO);

	return verify_signature(client, final + 2, flen - 2);
}

/**
 * sasl_client_wipe() - zeroise the password and the keys
 * @client: the client
 */
void sasl_client_wipe(struct sasl_client *client)
{
	explicit_bzero(client->pass, sizeof(client->pass));
	explicit_bzero(client->nonce, sizeof(client->nonce));
	explicit_bzero(client->server_signature,
		       sizeof(client->server_signature));
	explicit_bzero(client->client_first_bare,
		       sizeof(client->client_first_bare));
	explicit_bzero(client->server_first, sizeof(client->server_first));
}
