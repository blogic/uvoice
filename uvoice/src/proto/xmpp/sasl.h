// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 John Crispin <john@phrozen.org>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * The SASL client of the XMPP unit (src/proto/xmpp/sasl.c): SCRAM-SHA-1 and
 * SCRAM-SHA-256 (RFC 5802, RFC 7677) without channel binding, and PLAIN
 * (RFC 4616) for a server that offers nothing better over TLS.
 *
 * The messages cross here in the clear. The stream base64-encodes them into
 * the <auth/>, <response/> and <challenge/> elements (RFC 6120 section 6.4)
 * and decodes the server's. The hashes come from OpenSSL. The nonce comes
 * from the caller, so this layer holds no entropy source.
 *
 * SASLprep (RFC 4013) is not applied: the user name and the password travel
 * as the configuration spelled them, which is right for the ASCII accounts
 * this phone carries.
 */
#ifndef UVOICE_XMPP_SASL_H
#define UVOICE_XMPP_SASL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SASL_USER_MAX		128U
#define SASL_PASS_MAX		128U
/* 18 random bytes make a 24 character base64 nonce. */
#define SASL_NONCE_BYTES	18U
#define SASL_NONCE_MAX		32U
/* The longest message either side sends, with its terminator. */
#define SASL_MSG_MAX		1024U
#define SASL_HASH_MAX		32U

enum sasl_mech {
	SASL_MECH_NONE = 0,
	SASL_MECH_PLAIN,
	SASL_MECH_SCRAM_SHA_1,
	SASL_MECH_SCRAM_SHA_256,
};

enum sasl_state {
	SASL_ST_INIT = 0,
	SASL_ST_FIRST_SENT,
	SASL_ST_FINAL_SENT,
	SASL_ST_DONE,
	SASL_ST_FAILED,
};

struct sasl_client {
	enum sasl_mech mech;
	enum sasl_state state;
	char user[SASL_USER_MAX];
	char pass[SASL_PASS_MAX];
	char nonce[SASL_NONCE_MAX];
	char client_first_bare[SASL_MSG_MAX];
	char server_first[SASL_MSG_MAX];
	uint8_t server_signature[SASL_HASH_MAX];
	size_t hash_len;
};

/**
 * sasl_mech_name() - the IANA name of a mechanism
 * @mech: the mechanism
 *
 * Return: "SCRAM-SHA-256", "SCRAM-SHA-1", "PLAIN" or "" for none.
 */
const char *sasl_mech_name(enum sasl_mech mech);

/**
 * sasl_mech_of() - the mechanism of an IANA name
 * @name: the name, as the server's <mechanism/> carries it
 *
 * Return: the mechanism, SASL_MECH_NONE for one this client lacks.
 */
enum sasl_mech sasl_mech_of(const char *name);

/**
 * sasl_mech_pick() - the strongest mechanism of an offered set
 * @offered: the names the server listed
 * @count: how many
 * @allow_plain: whether PLAIN may be picked, true over TLS only
 *
 * Return: SCRAM-SHA-256 before SCRAM-SHA-1 before PLAIN, or SASL_MECH_NONE.
 */
enum sasl_mech sasl_mech_pick(const char *const *offered, unsigned int count,
			      bool allow_plain);

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
		     const uint8_t nonce[SASL_NONCE_BYTES]);

/**
 * sasl_client_first() - the initial response
 * @client: the client
 * @out: receives the message in the clear
 * @len: capacity of @out
 *
 * Return: the message length, or a negative errno.
 */
int sasl_client_first(struct sasl_client *client, char *out, size_t len);

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
		     size_t clen, char *out, size_t len);

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
		       size_t flen);

/**
 * sasl_client_wipe() - zeroise the password and the keys
 * @client: the client
 */
void sasl_client_wipe(struct sasl_client *client);

#endif
