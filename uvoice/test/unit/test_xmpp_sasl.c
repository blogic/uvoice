// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the SASL client of the XMPP unit (src/proto/xmpp/sasl.c).
 *
 * The known answers come from the RFCs, not from the code under test: the
 * SCRAM-SHA-1 exchange of RFC 5802 section 5 and the SCRAM-SHA-256 exchange
 * of RFC 7677 section 3. Both run the account "user" with the password
 * "pencil". Every account in this file is fictitious.
 */
#include <libubox/utils.h>

#include "proto/xmpp/sasl.h"
#include "test_util.h"

#define T_USER			"user"
#define T_PASS			"pencil"
#define T_SALT_5802		"QSXCR+Q6sek8bf92"

#define RFC5802_NONCE		"fyko+d2lbbFgONRv9qkxdawL"
#define RFC5802_SERVER_NONCE	RFC5802_NONCE "3rfcNHYJY1ZVvWVs7j"
#define RFC5802_FIRST		"n,,n=" T_USER ",r=" RFC5802_NONCE
#define RFC5802_FIRST_BARE	"n=" T_USER ",r=" RFC5802_NONCE
#define RFC5802_SERVER_FIRST	"r=" RFC5802_SERVER_NONCE \
				",s=" T_SALT_5802 ",i=4096"
#define RFC5802_FINAL		"c=biws,r=" RFC5802_SERVER_NONCE \
				",p=v0X8v3Bz2T0CJGbJQyF0X+HI4Ts="
#define RFC5802_SERVER_FINAL	"v=rmF9pqV8S7suAoZWja4dJRkFsKQ="

#define RFC7677_NONCE		"rOprNGfwEbeRWgbNEkqO"
#define RFC7677_SERVER_NONCE	RFC7677_NONCE "%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0"
#define RFC7677_FIRST		"n,,n=" T_USER ",r=" RFC7677_NONCE
#define RFC7677_FIRST_BARE	"n=" T_USER ",r=" RFC7677_NONCE
#define RFC7677_SERVER_FIRST	"r=" RFC7677_SERVER_NONCE \
				",s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096"
#define RFC7677_PROOF		"dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ="
#define RFC7677_FINAL		"c=biws,r=" RFC7677_SERVER_NONCE \
				",p=" RFC7677_PROOF
#define RFC7677_SERVER_FINAL	"v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4="

#define SHA1_LEN		20U
#define SHA256_LEN		32U

/**
 * nonce_bytes() - decode the RFC 5802 client nonce into its 18 bytes
 * @out: receives the bytes
 *
 * Return: true when the text decoded to SASL_NONCE_BYTES bytes.
 */
static bool nonce_bytes(uint8_t out[SASL_NONCE_BYTES])
{
	uint8_t raw[B64_DECODE_LEN(sizeof(RFC5802_NONCE))];
	int decoded = b64_decode(RFC5802_NONCE, raw, sizeof(raw));

	if (decoded != (int)SASL_NONCE_BYTES)
		return false;
	memcpy(out, raw, SASL_NONCE_BYTES);

	return true;
}

/**
 * client_start() - run one SCRAM client up to its client-first message
 * @client: the client
 * @mech: the mechanism
 * @nonce_text: the client nonce to use, written over the encoded one
 * @first: receives the client-first message
 * @first_len: capacity of @first
 *
 * Return: what sasl_client_first() returned, or the refusal of
 * sasl_client_init().
 */
static int client_start(struct sasl_client *client, enum sasl_mech mech,
			const char *nonce_text, char *first, size_t first_len)
{
	uint8_t nonce[SASL_NONCE_BYTES] = { 0 };
	int ret;

	ret = sasl_client_init(client, mech, T_USER, T_PASS, nonce);
	if (ret)
		return ret;
	snprintf(client->nonce, sizeof(client->nonce), "%s", nonce_text);

	return sasl_client_first(client, first, first_len);
}

/**
 * client_step() - run one SCRAM client up to its client-final message
 * @client: the client
 * @mech: the mechanism
 * @nonce_text: the client nonce to use
 * @server_first: the server-first message to answer
 * @final: receives the client-final message
 * @final_len: capacity of @final
 *
 * Return: what sasl_client_step() returned, or an earlier refusal.
 */
static int client_step(struct sasl_client *client, enum sasl_mech mech,
		       const char *nonce_text, const char *server_first,
		       char *final, size_t final_len)
{
	char first[SASL_MSG_MAX];
	int ret;

	ret = client_start(client, mech, nonce_text, first, sizeof(first));
	if (ret < 0)
		return ret;

	return sasl_client_step(client, server_first, strlen(server_first),
				final, final_len);
}

/**
 * all_zero() - check that a buffer holds nothing but zero bytes
 * @data: the buffer
 * @len: its length
 *
 * Return: true when every byte is zero.
 */
static bool all_zero(const void *data, size_t len)
{
	const uint8_t *bytes = data;
	size_t i;

	for (i = 0; i < len; i++)
		if (bytes[i])
			return false;

	return true;
}

/**
 * fill() - a string of one repeated character
 * @letter: the character
 * @count: how many
 *
 * Return: the string, valid until the next call.
 */
static const char *fill(char letter, size_t count)
{
	static char mem[SASL_USER_MAX * 2];

	if (count >= sizeof(mem))
		count = sizeof(mem) - 1;
	memset(mem, letter, count);
	mem[count] = 0;

	return mem;
}

/**
 * mech_tests() - the name table and the pick order
 */
static void mech_tests(void)
{
	const char *const all[] = { "PLAIN", "SCRAM-SHA-1", "SCRAM-SHA-256" };
	const char *const sha1_plain[] = { "PLAIN", "SCRAM-SHA-1" };
	const char *const plain[] = { "PLAIN" };
	const char *const foreign[] = { "DIGEST-MD5", "EXTERNAL" };

	CASE("mechanism names round trip");
	CHECK(!strcmp(sasl_mech_name(SASL_MECH_SCRAM_SHA_256),
		      "SCRAM-SHA-256"));
	CHECK(!strcmp(sasl_mech_name(SASL_MECH_SCRAM_SHA_1), "SCRAM-SHA-1"));
	CHECK(!strcmp(sasl_mech_name(SASL_MECH_PLAIN), "PLAIN"));
	CHECK(!strcmp(sasl_mech_name(SASL_MECH_NONE), ""));
	CHECK(!strcmp(sasl_mech_name((enum sasl_mech)42), ""));
	CHECK(sasl_mech_of("SCRAM-SHA-256") == SASL_MECH_SCRAM_SHA_256);
	CHECK(sasl_mech_of("SCRAM-SHA-1") == SASL_MECH_SCRAM_SHA_1);
	CHECK(sasl_mech_of("PLAIN") == SASL_MECH_PLAIN);

	CASE("a name this client lacks, or a different case, is none");
	CHECK(sasl_mech_of("plain") == SASL_MECH_NONE);
	CHECK(sasl_mech_of("SCRAM-SHA-1-PLUS") == SASL_MECH_NONE);
	CHECK(sasl_mech_of("SCRAM-SHA-512") == SASL_MECH_NONE);
	CHECK(sasl_mech_of("") == SASL_MECH_NONE);
	CHECK(sasl_mech_of(NULL) == SASL_MECH_NONE);

	CASE("the pick prefers SCRAM-SHA-256 over SCRAM-SHA-1 over PLAIN");
	CHECK(sasl_mech_pick(all, 3, true) == SASL_MECH_SCRAM_SHA_256);
	CHECK(sasl_mech_pick(all, 3, false) == SASL_MECH_SCRAM_SHA_256);
	CHECK(sasl_mech_pick(sha1_plain, 2, true) == SASL_MECH_SCRAM_SHA_1);
	CHECK(sasl_mech_pick(sha1_plain, 2, false) == SASL_MECH_SCRAM_SHA_1);
	CHECK(sasl_mech_pick(plain, 1, true) == SASL_MECH_PLAIN);

	CASE("the pick refuses PLAIN when it is not allowed");
	CHECK(sasl_mech_pick(plain, 1, false) == SASL_MECH_NONE);
	CHECK(sasl_mech_pick(foreign, 2, true) == SASL_MECH_NONE);
	CHECK(sasl_mech_pick(all, 0, true) == SASL_MECH_NONE);
}

/**
 * init_tests() - the refusals and the state after init
 */
static void init_tests(void)
{
	uint8_t nonce[SASL_NONCE_BYTES] = { 0 };
	struct sasl_client client;

	CASE("init refuses an empty or oversized user and an oversized pass");
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_1, "", T_PASS,
			       nonce) == -EINVAL);
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_1,
			       fill('u', SASL_USER_MAX), T_PASS,
			       nonce) == -EINVAL);
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_1, T_USER,
			       fill('p', SASL_PASS_MAX), nonce) == -EINVAL);
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_1, NULL, T_PASS,
			       nonce) == -EINVAL);
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_1, T_USER, NULL,
			       nonce) == -EINVAL);

	CASE("a user and a pass at the limit are taken");
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_1,
			       fill('u', SASL_USER_MAX - 1), T_PASS,
			       nonce) == 0);
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_1, T_USER,
			       fill('p', SASL_PASS_MAX - 1), nonce) == 0);
	CHECK(sasl_client_init(&client, SASL_MECH_PLAIN, T_USER, "",
			       NULL) == 0);

	CASE("init refuses no mechanism and SCRAM without a nonce");
	CHECK(sasl_client_init(&client, SASL_MECH_NONE, T_USER, T_PASS,
			       nonce) == -EINVAL);
	CHECK(sasl_client_init(&client, (enum sasl_mech)42, T_USER, T_PASS,
			       nonce) == -EINVAL);
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_256, T_USER,
			       T_PASS, NULL) == -EINVAL);

	CASE("init records the mechanism, the state and the hash length");
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_1, T_USER, T_PASS,
			       nonce) == 0);
	CHECK(client.mech == SASL_MECH_SCRAM_SHA_1);
	CHECK(client.state == SASL_ST_INIT);
	CHECK_UINT(client.hash_len, SHA1_LEN);
	CHECK(!strcmp(client.user, T_USER));
	CHECK(!strcmp(client.pass, T_PASS));
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_256, T_USER,
			       T_PASS, nonce) == 0);
	CHECK_UINT(client.hash_len, SHA256_LEN);
	CHECK(sasl_client_init(&client, SASL_MECH_PLAIN, T_USER, T_PASS,
			       NULL) == 0);
	CHECK_UINT(client.hash_len, 0);

	CASE("the 18 nonce bytes encode to the RFC 5802 client nonce");
	CHECK(nonce_bytes(nonce));
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_1, T_USER, T_PASS,
			       nonce) == 0);
	CHECK(!strcmp(client.nonce, RFC5802_NONCE));
	CHECK_UINT(strlen(client.nonce), 24);
}

/**
 * sha1_tests() - the RFC 5802 section 5 exchange
 */
static void sha1_tests(void)
{
	uint8_t nonce[SASL_NONCE_BYTES];
	struct sasl_client client;
	char out[SASL_MSG_MAX];
	int ret;

	SUITE_CHECK(nonce_bytes(nonce));
	SUITE_CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_1, T_USER,
				     T_PASS, nonce) == 0);

	CASE("RFC 5802 section 5: the client-first message");
	ret = sasl_client_first(&client, out, sizeof(out));
	CHECK_UINT(ret, strlen(RFC5802_FIRST));
	CHECK(!strcmp(out, RFC5802_FIRST));
	CHECK(!strcmp(client.client_first_bare, RFC5802_FIRST_BARE));
	CHECK(client.state == SASL_ST_FIRST_SENT);

	CASE("RFC 5802 section 5: the client-final message");
	ret = sasl_client_step(&client, RFC5802_SERVER_FIRST,
			       strlen(RFC5802_SERVER_FIRST), out, sizeof(out));
	CHECK_UINT(ret, strlen(RFC5802_FINAL));
	CHECK(!strcmp(out, RFC5802_FINAL));
	CHECK(client.state == SASL_ST_FINAL_SENT);
	CHECK(!strcmp(client.server_first, RFC5802_SERVER_FIRST));

	CASE("RFC 5802 section 5: the server-final message verifies");
	CHECK(sasl_client_verify(&client, RFC5802_SERVER_FINAL,
				 strlen(RFC5802_SERVER_FINAL)) == 0);
	CHECK(client.state == SASL_ST_DONE);

	CASE("a second verify after done is out of order");
	CHECK(sasl_client_verify(&client, RFC5802_SERVER_FINAL,
				 strlen(RFC5802_SERVER_FINAL)) == -EINVAL);
}

/**
 * sha256_tests() - the RFC 7677 section 3 exchange
 */
static void sha256_tests(void)
{
	struct sasl_client client;
	char out[SASL_MSG_MAX];
	int ret;

	CASE("RFC 7677 section 3: the client-first message");
	ret = client_start(&client, SASL_MECH_SCRAM_SHA_256, RFC7677_NONCE,
			   out, sizeof(out));
	CHECK_UINT(ret, strlen(RFC7677_FIRST));
	CHECK(!strcmp(out, RFC7677_FIRST));
	CHECK(!strcmp(client.client_first_bare, RFC7677_FIRST_BARE));
	CHECK_UINT(client.hash_len, SHA256_LEN);

	CASE("RFC 7677 section 3: the client-final message");
	ret = sasl_client_step(&client, RFC7677_SERVER_FIRST,
			       strlen(RFC7677_SERVER_FIRST), out, sizeof(out));
	CHECK_UINT(ret, strlen(RFC7677_FINAL));
	CHECK(!strcmp(out, RFC7677_FINAL));
	CHECK(client.state == SASL_ST_FINAL_SENT);

	CASE("RFC 7677 section 3: the server-final message verifies");
	CHECK(sasl_client_verify(&client, RFC7677_SERVER_FINAL,
				 strlen(RFC7677_SERVER_FINAL)) == 0);
	CHECK(client.state == SASL_ST_DONE);

	CASE("the server-final may carry an extension after the verifier");
	ret = client_step(&client, SASL_MECH_SCRAM_SHA_256, RFC7677_NONCE,
			  RFC7677_SERVER_FIRST, out, sizeof(out));
	CHECK(ret > 0);
	CHECK(sasl_client_verify(&client, RFC7677_SERVER_FINAL ",x=1",
				 strlen(RFC7677_SERVER_FINAL ",x=1")) == 0);
}

/**
 * final_tests() - the refusals of the server-final message
 */
static void final_tests(void)
{
	const char *wrong = "v=rmF9pqV8S7suBoZWja4dJRkFsKQ=";
	const char *error = "e=invalid-proof";
	struct sasl_client client;
	char out[SASL_MSG_MAX];

	SUITE_CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
				RFC5802_SERVER_FIRST, out, sizeof(out)) > 0);
	CASE("a wrong server signature returns -EACCES");
	CHECK(sasl_client_verify(&client, wrong, strlen(wrong)) == -EACCES);
	CHECK(client.state == SASL_ST_FAILED);

	SUITE_CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
				RFC5802_SERVER_FIRST, out, sizeof(out)) > 0);
	CASE("a signature of the wrong length returns -EACCES");
	CHECK(sasl_client_verify(&client, RFC7677_SERVER_FINAL,
				 strlen(RFC7677_SERVER_FINAL)) == -EACCES);

	SUITE_CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
				RFC5802_SERVER_FIRST, out, sizeof(out)) > 0);
	CASE("e=invalid-proof returns -EACCES");
	CHECK(sasl_client_verify(&client, error, strlen(error)) == -EACCES);
	CHECK(client.state == SASL_ST_FAILED);

	SUITE_CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
				RFC5802_SERVER_FIRST, out, sizeof(out)) > 0);
	CASE("a server-final that is neither v= nor e= returns -EPROTO");
	CHECK(sasl_client_verify(&client, "x=abc", 5) == -EPROTO);
	CHECK(client.state == SASL_ST_FAILED);

	SUITE_CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
				RFC5802_SERVER_FIRST, out, sizeof(out)) > 0);
	CASE("an empty server-final returns -EPROTO");
	CHECK(sasl_client_verify(&client, "", 0) == -EPROTO);

	SUITE_CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
				RFC5802_SERVER_FIRST, out, sizeof(out)) > 0);
	CASE("a verifier that is not base64 returns -EPROTO");
	CHECK(sasl_client_verify(&client, "v=!!!!", 6) == -EPROTO);
}

/**
 * challenge_tests() - the refusals of the server-first message
 */
static void challenge_tests(void)
{
	const char *foreign = "r=abcdefghijklmnopqrstuvwxyz3rfcNHYJY1ZVvWVs7j"
			      ",s=" T_SALT_5802 ",i=4096";
	const char *mandatory = "m=ext," RFC5802_SERVER_FIRST;
	const char *too_many = "r=" RFC5802_SERVER_NONCE ",s=" T_SALT_5802
			       ",i=1000001";
	const char *none = "r=" RFC5802_SERVER_NONCE ",s=" T_SALT_5802 ",i=0";
	const char *text = "r=" RFC5802_SERVER_NONCE ",s=" T_SALT_5802
			   ",i=4096x";
	const char *swapped = "s=" T_SALT_5802 ",r=" RFC5802_SERVER_NONCE
			      ",i=4096";
	const char *no_iter = "r=" RFC5802_SERVER_NONCE ",s=" T_SALT_5802;
	const char *bad_salt = "r=" RFC5802_SERVER_NONCE ",s=!!!!,i=4096";
	const char *empty_salt = "r=" RFC5802_SERVER_NONCE ",s=,i=4096";
	const char *extended = RFC5802_SERVER_FIRST ",x=1";
	struct sasl_client client;
	char out[SASL_MSG_MAX];

	CASE("a server nonce that does not extend the client nonce is -EPROTO");
	CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
			  foreign, out, sizeof(out)) == -EPROTO);
	CHECK(client.state == SASL_ST_FAILED);

	CASE("a server nonce shorter than the client nonce is -EPROTO");
	CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
			  "r=fyko,s=" T_SALT_5802 ",i=4096", out,
			  sizeof(out)) == -EPROTO);

	CASE("an m= attribute is -EPROTO");
	CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
			  mandatory, out, sizeof(out)) == -EPROTO);
	CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
			  RFC5802_SERVER_FIRST ",m=ext", out,
			  sizeof(out)) == -EPROTO);

	CASE("iterations above the bound, zero or not a number are -EPROTO");
	CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
			  too_many, out, sizeof(out)) == -EPROTO);
	CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE, none,
			  out, sizeof(out)) == -EPROTO);
	CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE, text,
			  out, sizeof(out)) == -EPROTO);

	CASE("attributes out of order or missing are -EPROTO");
	CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
			  swapped, out, sizeof(out)) == -EPROTO);
	CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
			  no_iter, out, sizeof(out)) == -EPROTO);
	CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE, "",
			  out, sizeof(out)) == -EPROTO);

	CASE("a salt that is not base64 or is empty is -EPROTO");
	CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
			  bad_salt, out, sizeof(out)) == -EPROTO);
	CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
			  empty_salt, out, sizeof(out)) == -EPROTO);

	CASE("an optional extension after the iteration count is skipped");
	CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
			  extended, out, sizeof(out)) > 0);
	CHECK(client.state == SASL_ST_FINAL_SENT);
	CHECK(!strncmp(out, "c=biws,r=" RFC5802_SERVER_NONCE ",p=",
		       strlen("c=biws,r=" RFC5802_SERVER_NONCE ",p=")));

	CASE("a server-first with a NUL byte is -EPROTO");
	CHECK(client_start(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE, out,
			   sizeof(out)) > 0);
	CHECK(sasl_client_step(&client, "r=" RFC5802_SERVER_NONCE "\0,s=",
			       strlen(RFC5802_SERVER_NONCE) + 6, out,
			       sizeof(out)) == -EPROTO);
}

/**
 * order_tests() - calls out of sequence and short buffers
 */
static void order_tests(void)
{
	uint8_t nonce[SASL_NONCE_BYTES] = { 0 };
	struct sasl_client client;
	char out[SASL_MSG_MAX];

	CASE("a step or a verify before the first message is -EINVAL");
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_1, T_USER, T_PASS,
			       nonce) == 0);
	CHECK(sasl_client_step(&client, RFC5802_SERVER_FIRST,
			       strlen(RFC5802_SERVER_FIRST), out,
			       sizeof(out)) == -EINVAL);
	CHECK(sasl_client_verify(&client, RFC5802_SERVER_FINAL,
				 strlen(RFC5802_SERVER_FINAL)) == -EINVAL);
	CHECK(client.state == SASL_ST_INIT);

	CASE("a second first message is -EINVAL");
	CHECK(sasl_client_first(&client, out, sizeof(out)) > 0);
	CHECK(sasl_client_first(&client, out, sizeof(out)) == -EINVAL);

	CASE("a verify before the step is -EINVAL");
	CHECK(sasl_client_verify(&client, RFC5802_SERVER_FINAL,
				 strlen(RFC5802_SERVER_FINAL)) == -EINVAL);

	CASE("a short buffer for the first message is -ENOSPC");
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_1, T_USER, T_PASS,
			       nonce) == 0);
	CHECK(sasl_client_first(&client, out, 8) == -ENOSPC);
	CHECK(client.state == SASL_ST_INIT);
	CHECK(sasl_client_first(&client, out, strlen(RFC5802_FIRST)) ==
	      -ENOSPC);
	CHECK(sasl_client_first(&client, out, strlen(RFC5802_FIRST) + 1) ==
	      (int)strlen(RFC5802_FIRST));

	CASE("a short buffer for the final message is -ENOSPC");
	CHECK(client_start(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE, out,
			   sizeof(out)) > 0);
	CHECK(sasl_client_step(&client, RFC5802_SERVER_FIRST,
			       strlen(RFC5802_SERVER_FIRST), out,
			       strlen(RFC5802_FINAL)) == -ENOSPC);
	CHECK(client.state == SASL_ST_FIRST_SENT);
	CHECK(sasl_client_step(&client, RFC5802_SERVER_FIRST,
			       strlen(RFC5802_SERVER_FIRST), out,
			       strlen(RFC5802_FINAL) + 1) ==
	      (int)strlen(RFC5802_FINAL));
	CHECK(!strcmp(out, RFC5802_FINAL));
}

/**
 * plain_tests() - the PLAIN mechanism
 */
static void plain_tests(void)
{
	struct sasl_client client;
	char out[SASL_MSG_MAX];
	int ret;

	CASE("PLAIN produces \\0user\\0pencil of length 12");
	CHECK(sasl_client_init(&client, SASL_MECH_PLAIN, T_USER, T_PASS,
			       NULL) == 0);
	ret = sasl_client_first(&client, out, sizeof(out));
	CHECK_UINT(ret, 12);
	CHECK(!memcmp(out, "\0user\0pencil", 12));
	CHECK(client.state == SASL_ST_FIRST_SENT);

	CASE("PLAIN has no step");
	CHECK(sasl_client_step(&client, "", 0, out, sizeof(out)) == -EINVAL);

	CASE("PLAIN takes an empty final and succeeds");
	CHECK(sasl_client_verify(&client, "", 0) == 0);
	CHECK(client.state == SASL_ST_DONE);
	CHECK(sasl_client_init(&client, SASL_MECH_PLAIN, T_USER, T_PASS,
			       NULL) == 0);
	CHECK(sasl_client_first(&client, out, sizeof(out)) == 12);
	CHECK(sasl_client_verify(&client, NULL, 0) == 0);

	CASE("PLAIN refuses a final that carries data");
	CHECK(sasl_client_init(&client, SASL_MECH_PLAIN, T_USER, T_PASS,
			       NULL) == 0);
	CHECK(sasl_client_first(&client, out, sizeof(out)) == 12);
	CHECK(sasl_client_verify(&client, "v=x", 3) == -EPROTO);
	CHECK(client.state == SASL_ST_FAILED);

	CASE("PLAIN refuses a short buffer and a verify before the first");
	CHECK(sasl_client_init(&client, SASL_MECH_PLAIN, T_USER, T_PASS,
			       NULL) == 0);
	CHECK(sasl_client_verify(&client, "", 0) == -EINVAL);
	CHECK(sasl_client_first(&client, out, 12) == -ENOSPC);
	CHECK(sasl_client_first(&client, out, 13) == 12);
}

/**
 * escape_tests() - the n= attribute escaping of RFC 5802 section 5.1
 */
static void escape_tests(void)
{
	uint8_t nonce[SASL_NONCE_BYTES];
	struct sasl_client client;
	char out[SASL_MSG_MAX];

	SUITE_CHECK(nonce_bytes(nonce));

	CASE("a user name with ',' and '=' is escaped as =2C and =3D");
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_1, "a,b=c",
			       T_PASS, nonce) == 0);
	CHECK(sasl_client_first(&client, out, sizeof(out)) > 0);
	CHECK(!strcmp(out, "n,,n=a=2Cb=3Dc,r=" RFC5802_NONCE));
	CHECK(!strcmp(client.client_first_bare,
		      "n=a=2Cb=3Dc,r=" RFC5802_NONCE));
	CHECK(!strcmp(client.user, "a,b=c"));

	CASE("PLAIN sends the user name as it is");
	CHECK(sasl_client_init(&client, SASL_MECH_PLAIN, "a,b=c", T_PASS,
			       NULL) == 0);
	CHECK(sasl_client_first(&client, out, sizeof(out)) == 13);
	CHECK(!memcmp(out, "\0a,b=c\0pencil", 13));

	CASE("a user name at the limit made of ',' still fits");
	CHECK(sasl_client_init(&client, SASL_MECH_SCRAM_SHA_1,
			       fill(',', SASL_USER_MAX - 1), T_PASS,
			       nonce) == 0);
	CHECK(sasl_client_first(&client, out, sizeof(out)) ==
	      (int)(strlen("n,,n=") + (SASL_USER_MAX - 1) * 3 +
		    strlen(",r=" RFC5802_NONCE)));
	CHECK(!strncmp(out, "n,,n==2C=2C", 11));
}

/**
 * wipe_tests() - the zeroisation after an exchange
 */
static void wipe_tests(void)
{
	struct sasl_client client;
	char out[SASL_MSG_MAX];

	SUITE_CHECK(client_step(&client, SASL_MECH_SCRAM_SHA_1, RFC5802_NONCE,
				RFC5802_SERVER_FIRST, out, sizeof(out)) > 0);
	SUITE_CHECK(sasl_client_verify(&client, RFC5802_SERVER_FINAL,
				       strlen(RFC5802_SERVER_FINAL)) == 0);
	SUITE_CHECK(!all_zero(client.pass, sizeof(client.pass)));
	SUITE_CHECK(!all_zero(client.server_signature,
			      sizeof(client.server_signature)));

	CASE("sasl_client_wipe leaves the pass and the keys all zero");
	sasl_client_wipe(&client);
	CHECK(all_zero(client.pass, sizeof(client.pass)));
	CHECK(all_zero(client.nonce, sizeof(client.nonce)));
	CHECK(all_zero(client.server_signature,
		       sizeof(client.server_signature)));
	CHECK(all_zero(client.client_first_bare,
		       sizeof(client.client_first_bare)));
	CHECK(all_zero(client.server_first, sizeof(client.server_first)));
	CHECK(client.state == SASL_ST_DONE);
}

/**
 * main() - run every group
 *
 * Return: 0 when every case passed, 1 otherwise.
 */
int main(void)
{
	mech_tests();
	init_tests();
	sha1_tests();
	sha256_tests();
	final_tests();
	challenge_tests();
	order_tests();
	plain_tests();
	escape_tests();
	wipe_tests();

	return tu_summary("test_xmpp_sasl");
}
