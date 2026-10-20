// SPDX-License-Identifier: GPL-2.0-only
/*
 * uvoice - native voice stack for OpenWrt
 * Copyright (C) 2026 Adtran Inc.
 * Author: Chad Monroe <chad.monroe@adtran.com>
 *
 * Licensed under GPL-2.0-only with the linking exception stated in LICENSE.
 *
 * Unit tests for the digest layer (src/proto/sip/auth.c).
 *
 * The known answers come from three sources, and none of them is the code under
 * test. The first is the published example of RFC 2617 3.5. The second is the
 * published example of RFC 7616 3.9.1, with both responses, MD5 and SHA-256.
 * The third covers the shapes that no RFC tabulates: an independent
 * implementation computed those values.
 *
 * Every account in this file is fictitious. No provider credential appears in
 * this file or in any other file of the tree.
 */
#include <stdlib.h>

#include "proto/sip/auth.h"
#include "test_msg.h"
#include "test_util.h"

#define MSG_TEXT_MAX	2048

/* the fictitious account every uvoice case below authenticates with */
#define T_USER		"uvoice-test"
#define T_PASS		"not-a-real-secret"
#define T_REALM		"sip.example.com"
#define T_URI		"sip:sip.example.com"
#define T_NONCE		"1f3870be274f6c49b3e31a0c6728957f"
#define T_OPAQUE	"9a8b7c6d5e4f"

/* sip_auth_cnonce_make(0x2a, 0x2b) */
#define T_CNONCE	"0000002a0000002b"

static const char *resp_text(uint32_t code, const char *hdrs)
{
	static char buf[MSG_TEXT_MAX];

	snprintf(buf, sizeof(buf),
		 "SIP/2.0 %u %s\r\n"
		 "Via: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bK-a1\r\n"
		 "From: <sip:%s@%s>;tag=localtag\r\n"
		 "To: <sip:%s@%s>;tag=remotetag\r\n"
		 "Call-ID: auth-1@10.0.0.1\r\n"
		 "CSeq: 1 REGISTER\r\n"
		 "%s"
		 "Content-Length: 0\r\n\r\n",
		 code, sip_status_reason(code), T_USER, T_REALM, T_USER, T_REALM, hdrs);

	return buf;
}

/* one challenge value, parsed the way a response header carries it */
static bool chal_of(const char *value, struct sip_challenge *c)
{
	struct sip_chal_iter it;

	return hdr_chal_parse(sip_str_cstr(value), &it, c);
}

static const char *cred_line(const struct sip_cred *cred, enum sip_hdr_id id)
{
	static char mem[1024];
	struct sip_buf b;

	sip_buf_init(&b, mem, sizeof(mem));
	if (!hdr_cred_write(&b, id, cred))
		return "";

	return sip_buf_str(&b);
}

static const char *fill(char c, size_t n)
{
	static char mem[SIP_AUTH_NONCE_MAX * 4];

	if (n >= sizeof(mem))
		n = sizeof(mem) - 1;

	memset(mem, c, n);
	mem[n] = 0;

	return mem;
}

/* ----------------------------------------------------------- plain helpers */

static void helper_tests(void)
{
	enum sip_auth_algo algo;
	char cn[SIP_AUTH_CNONCE_MAX];

	CASE("algorithm names round trip");
	CHECK(sip_auth_algo_parse(sip_str_cstr("MD5"), &algo) && algo == SIP_AUTH_ALGO_MD5);
	CHECK(sip_auth_algo_parse(sip_str_cstr("md5-sess"), &algo) &&
	      algo == SIP_AUTH_ALGO_MD5_SESS);
	CHECK(sip_auth_algo_parse(sip_str_cstr("SHA-256"), &algo) &&
	      algo == SIP_AUTH_ALGO_SHA256);
	CHECK(sip_auth_algo_parse(sip_str_cstr("SHA-256-SESS"), &algo) &&
	      algo == SIP_AUTH_ALGO_SHA256_SESS);
	CHECK(!strcmp(sip_auth_algo_name(SIP_AUTH_ALGO_SHA256), "SHA-256"));
	CHECK(!strcmp(sip_auth_algo_name(SIP_AUTH_ALGO_MD5_SESS), "MD5-sess"));

	CASE("the parser refuses an algorithm name this build cannot compute");
	CHECK(!sip_auth_algo_parse(sip_str_cstr("SHA-512-256"), &algo));
	CHECK(!sip_auth_algo_parse(sip_str_cstr("SHA-1"), &algo));
	CHECK(!sip_auth_algo_parse(sip_str_cstr(""), &algo));

	CASE("a quoted algorithm parses (RFC 3261 22.4 leaves it a token)");
	CHECK(sip_auth_algo_parse(sip_str_cstr("\"SHA-256\""), &algo) &&
	      algo == SIP_AUTH_ALGO_SHA256);

	CASE("the hex length follows the algorithm");
	CHECK_UINT(sip_auth_algo_hexlen(SIP_AUTH_ALGO_MD5), 32);
	CHECK_UINT(sip_auth_algo_hexlen(SIP_AUTH_ALGO_MD5_SESS), 32);
	CHECK_UINT(sip_auth_algo_hexlen(SIP_AUTH_ALGO_SHA256), 64);
	CHECK_UINT(sip_auth_algo_hexlen(SIP_AUTH_ALGO_SHA256_SESS), 64);

	CASE("401 and 407 name different header pairs");
	CHECK(sip_auth_chal_hdr(SIP_AUTH_WWW) == SIP_HDR_WWW_AUTHENTICATE);
	CHECK(sip_auth_cred_hdr(SIP_AUTH_WWW) == SIP_HDR_AUTHORIZATION);
	CHECK(sip_auth_chal_hdr(SIP_AUTH_PROXY) == SIP_HDR_PROXY_AUTHENTICATE);
	CHECK(sip_auth_cred_hdr(SIP_AUTH_PROXY) == SIP_HDR_PROXY_AUTHORIZATION);

	CASE("the cnonce is 16 hex digits and refuses a short buffer");
	CHECK(sip_auth_cnonce_make(cn, sizeof(cn), 0x2a, 0x2b));
	CHECK(!strcmp(cn, T_CNONCE));
	CHECK(!sip_auth_cnonce_make(cn, SIP_AUTH_CNONCE_MAX - 1, 1, 2));
}

/* ------------------------------------------------------------ RFC vectors */

static const char *calc(enum sip_auth_algo algo, const char *user, const char *realm,
			const char *pass, const char *method, const char *uri,
			const char *nonce, const char *cnonce, uint32_t nc, const char *qop)
{
	static char out[SIP_AUTH_HEX_MAX];

	if (sip_auth_response_calc(algo, user, realm, pass, method, uri, nonce, cnonce, nc, qop,
				   out, sizeof(out), NULL, 0) != SIP_AUTH_OK)
		return "";

	return out;
}

static void vector_tests(void)
{
	const char *n2617 = "dcd98b7102dd2f0e8b11d0f600bfb0c093";
	const char *n7616 = "7ypf/xlj9XXwfDPEoM4URrv/xwf94BcCAzFZH4GiTo0v";
	const char *c7616 = "f2/wE4q74E6zIJEtWaHKaf5wv/H5QzzpXusqGemxURZJ";
	char out[SIP_AUTH_HEX_MAX], ha1[SIP_AUTH_HEX_MAX];

	CASE("RFC 2617 3.5 example, qop=auth");
	CHECK(!strcmp(calc(SIP_AUTH_ALGO_MD5, "Mufasa", "testrealm@host.com", "Circle Of Life",
			   "GET", "/dir/index.html", n2617, "0a4f113b", 1, "auth"),
		      "6629fae49393a05397450978507c4ef1"));

	CASE("RFC 2617 3.5 parameters in the RFC 2069 form, qop absent");
	CHECK(!strcmp(calc(SIP_AUTH_ALGO_MD5, "Mufasa", "testrealm@host.com", "Circle Of Life",
			   "GET", "/dir/index.html", n2617, NULL, 0, NULL),
		      "670fd8c2df070c60b045671b8b24ff02"));

	CASE("an empty qop string is the RFC 2069 form as well");
	CHECK(!strcmp(calc(SIP_AUTH_ALGO_MD5, "Mufasa", "testrealm@host.com", "Circle Of Life",
			   "GET", "/dir/index.html", n2617, NULL, 0, ""),
		      "670fd8c2df070c60b045671b8b24ff02"));

	CASE("the nonce count changes the response");
	CHECK(!strcmp(calc(SIP_AUTH_ALGO_MD5, "Mufasa", "testrealm@host.com", "Circle Of Life",
			   "GET", "/dir/index.html", n2617, "0a4f113b", 2, "auth"),
		      "15b6bb427e3fecd23a43cb702ce447d5"));

	CASE("RFC 7616 3.9.1 example, MD5");
	CHECK(!strcmp(calc(SIP_AUTH_ALGO_MD5, "Mufasa", "http-auth@example.org",
			   "Circle of Life", "GET", "/dir/index.html", n7616, c7616, 1, "auth"),
		      "8ca523f5e9506fed4657c9700eebdbec"));

	CASE("RFC 7616 3.9.1 example, SHA-256");
	CHECK(!strcmp(calc(SIP_AUTH_ALGO_SHA256, "Mufasa", "http-auth@example.org",
			   "Circle of Life", "GET", "/dir/index.html", n7616, c7616, 1, "auth"),
		      "753927fa0e85d155564e2e272a28d1802ca10daf4496794697cf8db5856cb6c1"));

	CASE("MD5-sess folds the nonce and the cnonce into HA1");
	CHECK(!strcmp(calc(SIP_AUTH_ALGO_MD5_SESS, "Mufasa", "http-auth@example.org",
			   "Circle of Life", "GET", "/dir/index.html", n7616, c7616, 1, "auth"),
		      "e783283f46242139c486a698fec7211d"));

	CASE("SHA-256-sess folds the nonce and the cnonce into HA1");
	CHECK(!strcmp(calc(SIP_AUTH_ALGO_SHA256_SESS, "Mufasa", "http-auth@example.org",
			   "Circle of Life", "GET", "/dir/index.html", n7616, c7616, 1, "auth"),
		      "2fd51b3a77ad75bad6afad6003e818d767133c46d9e2749e7f5232ae1ea3efd7"));

	CASE("the session HA1 comes back to the caller");
	CHECK(sip_auth_response_calc(SIP_AUTH_ALGO_MD5, "Mufasa", "testrealm@host.com",
				     "Circle Of Life", "GET", "/dir/index.html", n2617,
				     "0a4f113b", 1, "auth", out, sizeof(out), ha1,
				     sizeof(ha1)) == SIP_AUTH_OK);
	CHECK(!strcmp(ha1, "939e7578ed9e3c518a452acee763bce9"));

	CASE("a SHA-256 response does not fit an MD5 sized buffer");
	CHECK(sip_auth_response_calc(SIP_AUTH_ALGO_SHA256, "u", "r", "p", "REGISTER", T_URI,
				     T_NONCE, NULL, 0, NULL, out, 33, NULL,
				     0) == SIP_AUTH_ERR_BUF);

	CASE("qop=auth without a cnonce or without a count is refused");
	CHECK(sip_auth_response_calc(SIP_AUTH_ALGO_MD5, "u", "r", "p", "REGISTER", T_URI,
				     T_NONCE, NULL, 1, "auth", out, sizeof(out), NULL,
				     0) == SIP_AUTH_ERR_FIELD);
	CHECK(sip_auth_response_calc(SIP_AUTH_ALGO_MD5, "u", "r", "p", "REGISTER", T_URI,
				     T_NONCE, "abc", 0, "auth", out, sizeof(out), NULL,
				     0) == SIP_AUTH_ERR_FIELD);

	CASE("qop=auth-int is refused by the calculator too");
	CHECK(sip_auth_response_calc(SIP_AUTH_ALGO_MD5, "u", "r", "p", "REGISTER", T_URI,
				     T_NONCE, "abc", 1, "auth-int", out, sizeof(out), NULL,
				     0) == SIP_AUTH_ERR_QOP);

	CASE("a -sess algorithm without a cnonce is refused");
	CHECK(sip_auth_response_calc(SIP_AUTH_ALGO_MD5_SESS, "u", "r", "p", "REGISTER", T_URI,
				     T_NONCE, NULL, 0, NULL, out, sizeof(out), NULL,
				     0) == SIP_AUTH_ERR_FIELD);

	CASE("a missing argument is refused, not dereferenced");
	CHECK(sip_auth_response_calc(SIP_AUTH_ALGO_MD5, NULL, "r", "p", "REGISTER", T_URI,
				     T_NONCE, NULL, 0, NULL, out, sizeof(out), NULL,
				     0) == SIP_AUTH_ERR_FIELD);
	CHECK(sip_auth_response_calc(SIP_AUTH_ALGO_MD5, "u", "r", "p", "REGISTER", T_URI, NULL,
				     NULL, 0, NULL, out, sizeof(out), NULL,
				     0) == SIP_AUTH_ERR_FIELD);
}

/* ------------------------------------------------------- challenge intake */

static void challenge_tests(void)
{
	struct sip_auth_state st;
	struct sip_challenge c;
	struct sip_cred cred;
	char value[SIP_AUTH_NONCE_MAX * 8];

	/*
	 * The base shape of a provider challenge: Digest, realm, nonce,
	 * opaque, no qop and no algorithm. Everything
	 * else in this file is a variation on it.
	 */
	CASE("the provider challenge shape: no qop, no algorithm, opaque present");
	sip_auth_state_reset(&st);
	snprintf(value, sizeof(value), "Digest realm=\"%s\",nonce=\"%s\",opaque=\"%s\"",
		 T_REALM, T_NONCE, T_OPAQUE);
	CHECK(chal_of(value, &c));
	CHECK(sip_auth_challenge_take(&st, &c, 0x2a, 0x2b) == SIP_AUTH_OK);
	CHECK(st.valid && !st.qop_auth && !st.algo_present && st.opaque_present);
	CHECK(st.algo == SIP_AUTH_ALGO_MD5);
	CHECK(!strcmp(st.realm, T_REALM));
	CHECK(!strcmp(st.nonce, T_NONCE));
	CHECK(!strcmp(st.opaque, T_OPAQUE));
	CHECK_UINT(st.nc, 0);

	CASE("its credentials carry the known answer, the opaque and no qop");
	CHECK(sip_auth_cred_make(&st, T_USER, T_PASS, "REGISTER", T_URI, &cred) == SIP_AUTH_OK);
	CHECK(!strcmp(cred.response, "a3a8dd76559ceaf5a361177f6a87a08f"));
	CHECK(!cred.qop && !cred.cnonce && !cred.algorithm);
	CHECK_UINT(cred.nc, 0);
	CHECK(strstr(cred_line(&cred, SIP_HDR_AUTHORIZATION),
		     "opaque=\"" T_OPAQUE "\"") != NULL);
	CHECK(strstr(cred_line(&cred, SIP_HDR_AUTHORIZATION), "qop=") == NULL);
	CHECK(strstr(cred_line(&cred, SIP_HDR_AUTHORIZATION), "nc=") == NULL);
	CHECK(strstr(cred_line(&cred, SIP_HDR_AUTHORIZATION), "algorithm=") == NULL);
	CHECK(strstr(cred_line(&cred, SIP_HDR_AUTHORIZATION),
		     "response=\"a3a8dd76559ceaf5a361177f6a87a08f\"") != NULL);

	CASE("the same nonce with a second method gives a different response");
	CHECK(sip_auth_cred_make(&st, T_USER, T_PASS, "INVITE", "sip:5552000@sip.example.com",
				 &cred) == SIP_AUTH_OK);
	CHECK(!strcmp(cred.response, "ecba0b01ce0285ce1dd37c98d74997f5"));

	CASE("SHA-256 in the same qop absent form");
	sip_auth_state_reset(&st);
	snprintf(value, sizeof(value),
		 "Digest realm=\"%s\",nonce=\"%s\",algorithm=SHA-256", T_REALM, T_NONCE);
	CHECK(chal_of(value, &c));
	CHECK(sip_auth_challenge_take(&st, &c, 0x2a, 0x2b) == SIP_AUTH_OK);
	CHECK(st.algo == SIP_AUTH_ALGO_SHA256 && st.algo_present && !st.opaque_present);
	CHECK(sip_auth_cred_make(&st, T_USER, T_PASS, "REGISTER", T_URI, &cred) == SIP_AUTH_OK);
	CHECK(!strcmp(cred.response,
		      "15449d05fb0c3d8def579e2a72deeb8a138b447a961d1e40b6708e4614af73ab"));
	CHECK(cred.algorithm && !strcmp(cred.algorithm, "SHA-256"));
	CHECK(strstr(cred_line(&cred, SIP_HDR_AUTHORIZATION), "algorithm=SHA-256") != NULL);
	CHECK(strstr(cred_line(&cred, SIP_HDR_AUTHORIZATION), "opaque=") == NULL);

	CASE("qop=auth advances the nonce count on every request");
	sip_auth_state_reset(&st);
	snprintf(value, sizeof(value), "Digest realm=\"%s\", nonce=\"%s\", qop=\"auth\"",
		 T_REALM, T_NONCE);
	CHECK(chal_of(value, &c));
	CHECK(sip_auth_challenge_take(&st, &c, 0x2a, 0x2b) == SIP_AUTH_OK);
	CHECK(st.qop_auth);
	CHECK(sip_auth_cred_make(&st, T_USER, T_PASS, "REGISTER", T_URI, &cred) == SIP_AUTH_OK);
	CHECK_UINT(cred.nc, 1);
	CHECK(!strcmp(cred.cnonce, T_CNONCE));
	CHECK(!strcmp(cred.response, "2d0ff43860e52b7b7f21292f2c87c1bf"));
	CHECK(strstr(cred_line(&cred, SIP_HDR_AUTHORIZATION), "nc=00000001") != NULL);
	CHECK(strstr(cred_line(&cred, SIP_HDR_AUTHORIZATION), "qop=auth") != NULL);
	CHECK(strstr(cred_line(&cred, SIP_HDR_AUTHORIZATION),
		     "cnonce=\"" T_CNONCE "\"") != NULL);

	CASE("the second and third request under one nonce count up");
	CHECK(sip_auth_cred_make(&st, T_USER, T_PASS, "REGISTER", T_URI, &cred) == SIP_AUTH_OK);
	CHECK_UINT(cred.nc, 2);
	CHECK(!strcmp(cred.response, "9f119034b8313607bbd37371250b2d48"));
	CHECK(sip_auth_cred_make(&st, T_USER, T_PASS, "REGISTER", T_URI, &cred) == SIP_AUTH_OK);
	CHECK_UINT(cred.nc, 3);
	CHECK(!strcmp(cred.response, "5382aae4397f94492bfbe1dc27060ac8"));
	CHECK(strstr(cred_line(&cred, SIP_HDR_AUTHORIZATION), "nc=00000003") != NULL);

	CASE("a qop list holding auth and auth-int selects auth");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"n\", qop=\"auth-int,auth\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK(st.qop_auth);

	CASE("an unquoted qop list keeps every token and still selects auth");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"n\", qop=auth-int,auth", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK(st.qop_auth);

	CASE("qop=auth-int alone is refused cleanly");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"n\", qop=\"auth-int\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_QOP);
	CHECK(!st.valid);

	CASE("an unknown qop token is refused, not treated as the 2069 form");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"n\", qop=\"auth-conf\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_QOP);

	CASE("an empty qop value falls back to the 2069 form");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"n\", qop=\"\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK(!st.qop_auth);

	CASE("a challenge whose algorithm the build cannot compute is refused");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"n\", algorithm=SHA-512-256", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_ALGO);

	CASE("a non Digest scheme is refused");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Basic realm=\"r\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_SCHEME);

	CASE("a challenge without realm or without nonce is refused");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest nonce=\"n\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_FIELD);
	CHECK(chal_of("Digest realm=\"r\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_FIELD);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_FIELD);

	CASE("an empty realm is legal, an absent one is not");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest realm=\"\", nonce=\"n\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK(st.realm[0] == 0);

	CASE("a nonce too long to store is refused, never truncated");
	sip_auth_state_reset(&st);
	snprintf(value, sizeof(value), "Digest realm=\"r\", nonce=\"%s\"",
		 fill('n', SIP_AUTH_NONCE_MAX));
	CHECK(chal_of(value, &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_FIELD);
	CHECK(!st.valid);

	CASE("an opaque too long to echo fails the challenge (RFC 2617 3.2.1)");
	sip_auth_state_reset(&st);
	snprintf(value, sizeof(value), "Digest realm=\"r\", nonce=\"n\", opaque=\"%s\"",
		 fill('o', SIP_AUTH_OPAQUE_MAX));
	CHECK(chal_of(value, &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_FIELD);

	CASE("a realm at the limit still fits");
	sip_auth_state_reset(&st);
	snprintf(value, sizeof(value), "Digest realm=\"%s\", nonce=\"n\"",
		 fill('r', SIP_AUTH_REALM_MAX - 1));
	CHECK(chal_of(value, &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK_UINT(strlen(st.realm), SIP_AUTH_REALM_MAX - 1);

	CASE("an escaped quote in the realm digests unescaped and goes back escaped");
	sip_auth_state_reset(&st);
	snprintf(value, sizeof(value), "Digest realm=\"a\\\"b\", nonce=\"%s\"", T_NONCE);
	CHECK(chal_of(value, &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK(!strcmp(st.realm, "a\"b"));
	CHECK(sip_auth_cred_make(&st, T_USER, T_PASS, "REGISTER", T_URI, &cred) == SIP_AUTH_OK);
	CHECK(!strcmp(cred.response, "fed8df75641a0fadac92defcfbf51144"));
	CHECK(strstr(cred_line(&cred, SIP_HDR_AUTHORIZATION), "realm=\"a\\\"b\"") != NULL);

	/*
	 * A -sess algorithm folds the cnonce into HA1, and hdr_cred_write()
	 * emits a cnonce only with qop, so the qop-free -sess form would put a
	 * response on the wire that no server can recompute.
	 */
	CASE("a -sess algorithm without qop is refused, not sent uncheckable");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"n\", algorithm=MD5-sess", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_ALGO);
	CHECK(!sip_auth_state_valid(&st));
	CHECK(chal_of("Digest realm=\"r\", nonce=\"n\", algorithm=SHA-256-sess", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_ALGO);
	CHECK(sip_auth_algo_sess(SIP_AUTH_ALGO_MD5_SESS));
	CHECK(!sip_auth_algo_sess(SIP_AUTH_ALGO_MD5));

	CASE("the same algorithm with qop is taken and carries its cnonce");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"n\", algorithm=MD5-sess, qop=\"auth\"",
		      &c));
	CHECK(sip_auth_challenge_take(&st, &c, 0x2a, 0x2b) == SIP_AUTH_OK);
	CHECK(sip_auth_cred_make(&st, T_USER, T_PASS, "REGISTER", T_URI, &cred) == SIP_AUTH_OK);
	CHECK(strstr(cred_line(&cred, SIP_HDR_AUTHORIZATION),
		     "cnonce=\"" T_CNONCE "\"") != NULL);
	CHECK(strstr(cred_line(&cred, SIP_HDR_AUTHORIZATION), "algorithm=MD5-sess") != NULL);

	CASE("a hand built -sess state without qop is refused by the writer too");
	st.qop_auth = false;
	CHECK(sip_auth_cred_make(&st, T_USER, T_PASS, "REGISTER", T_URI,
				 &cred) == SIP_AUTH_ERR_ALGO);

	/*
	 * sip_buf_add_quoted() escapes a quote and a backslash and nothing
	 * else, so a CR or an LF in a challenge would break out of the header
	 * and inject headers into the next REGISTER.
	 */
	CASE("a CR or an LF in a challenge field is refused");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"a\rEvil: x\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_FIELD);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"a\nEvil: x\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_FIELD);
	CHECK(!sip_auth_state_valid(&st));

	CASE("an escaped control byte is refused as well");
	CHECK(chal_of("Digest realm=\"r\", nonce=\"a\\\rb\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_FIELD);

	CASE("a control byte in the realm or in the opaque is refused");
	CHECK(chal_of("Digest realm=\"a\tb\", nonce=\"n\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_FIELD);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"n\", opaque=\"a\177" "b\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_FIELD);

	CASE("an embedded NUL is refused, not silently cut");
	sip_auth_state_reset(&st);
	{
		char raw[128];
		struct sip_chal_iter it;
		size_t len;

		len = (size_t)snprintf(raw, sizeof(raw),
				       "Digest realm=\"r\", nonce=\"abcXdef\"");
		raw[strchr(raw, 'X') - raw] = 0;
		CHECK(hdr_chal_parse(sip_str_init(raw, len), &it, &c));
		CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_FIELD);
		CHECK(!sip_auth_state_valid(&st));
	}

	CASE("a base64 nonce with + / and = is still accepted");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"7ypf/xlj9XX+wfDPEoM4URrv=\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK(!strcmp(st.nonce, "7ypf/xlj9XX+wfDPEoM4URrv="));

	CASE("credentials without a cached challenge are refused");
	sip_auth_state_reset(&st);
	CHECK(sip_auth_cred_make(&st, T_USER, T_PASS, "REGISTER", T_URI,
				 &cred) == SIP_AUTH_ERR_STATE);
}

/* ------------------------------------------------------- stale and repeat */

static void stale_tests(void)
{
	struct sip_auth_state st;
	struct sip_challenge c;
	struct sip_cred cred;
	char value[512];
	unsigned int i;

	CASE("the same nonce back again means the credentials were refused");
	sip_auth_state_reset(&st);
	snprintf(value, sizeof(value), "Digest realm=\"%s\", nonce=\"%s\"", T_REALM, T_NONCE);
	CHECK(chal_of(value, &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK(sip_auth_cred_make(&st, T_USER, T_PASS, "REGISTER", T_URI, &cred) == SIP_AUTH_OK);
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_REPEAT);

	CASE("the repeat rule holds even when the second challenge claims stale");
	snprintf(value, sizeof(value), "Digest realm=\"%s\", nonce=\"%s\", stale=true", T_REALM,
		 T_NONCE);
	CHECK(chal_of(value, &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_REPEAT);

	CASE("a repeated challenge before any credentials is not a repeat");
	sip_auth_state_reset(&st);
	snprintf(value, sizeof(value), "Digest realm=\"%s\", nonce=\"%s\"", T_REALM, T_NONCE);
	CHECK(chal_of(value, &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);

	CASE("the same nonce in a different realm is a new challenge");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest realm=\"one\", nonce=\"shared\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK(sip_auth_cred_make(&st, T_USER, T_PASS, "REGISTER", T_URI, &cred) == SIP_AUTH_OK);
	CHECK(chal_of("Digest realm=\"two\", nonce=\"shared\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK(!strcmp(st.realm, "two"));

	CASE("stale=true with a fresh nonce is accepted and marked");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"n1\", qop=\"auth\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK(!st.stale);
	CHECK(sip_auth_cred_make(&st, T_USER, T_PASS, "REGISTER", T_URI, &cred) == SIP_AUTH_OK);
	CHECK_UINT(cred.nc, 1);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"n2\", qop=\"auth\", stale=TRUE", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK(st.stale);
	CHECK_UINT(st.stale_run, 1);

	CASE("a fresh nonce resets the count, so the next request starts at nc=1 again");
	CHECK_UINT(st.nc, 0);
	CHECK(sip_auth_cred_make(&st, T_USER, T_PASS, "REGISTER", T_URI, &cred) == SIP_AUTH_OK);
	CHECK_UINT(cred.nc, 1);

	CASE("a run of stale refreshes is given up at SIP_AUTH_STALE_MAX");
	sip_auth_state_reset(&st);
	for (i = 0; i < SIP_AUTH_STALE_MAX; i++) {
		snprintf(value, sizeof(value),
			 "Digest realm=\"r\", nonce=\"loop-%u\", stale=true", i);
		CHECK(chal_of(value, &c));
		CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
		CHECK_UINT(st.stale_run, i + 1);
	}
	snprintf(value, sizeof(value), "Digest realm=\"r\", nonce=\"loop-last\", stale=true");
	CHECK(chal_of(value, &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_ERR_STALE_LOOP);

	CASE("an accepted request clears the stale run");
	sip_auth_state_accept(&st);
	CHECK_UINT(st.stale_run, 0);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"after-accept\", stale=true", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);

	CASE("a non stale challenge with a new nonce clears the run by itself");
	sip_auth_state_reset(&st);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"s1\", stale=true", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK_UINT(st.stale_run, 1);
	CHECK(chal_of("Digest realm=\"r\", nonce=\"s2\"", &c));
	CHECK(sip_auth_challenge_take(&st, &c, 1, 2) == SIP_AUTH_OK);
	CHECK_UINT(st.stale_run, 0);
	CHECK(!st.stale);
}

/* ------------------------------------------------------- message level */

static void msg_tests(void)
{
	struct sip_auth_state st[SIP_AUTH_KIND_MAX];
	struct sip_cred cred;
	struct sip_msg *m;
	char hdrs[1024];
	char mem[2048];
	struct sip_buf b;

	CASE("a 401 hands the WWW-Authenticate challenge over");
	sip_auth_state_reset(&st[SIP_AUTH_WWW]);
	snprintf(hdrs, sizeof(hdrs),
		 "WWW-Authenticate: Digest realm=\"%s\", nonce=\"%s\", opaque=\"%s\"\r\n",
		 T_REALM, T_NONCE, T_OPAQUE);
	m = msg_take(resp_text(401, hdrs));
	CHECK(m != NULL);
	CHECK(sip_auth_msg_challenge(&st[SIP_AUTH_WWW], m, SIP_AUTH_WWW, 0x2a,
				     0x2b) == SIP_AUTH_OK);
	CHECK(!strcmp(st[SIP_AUTH_WWW].nonce, T_NONCE));

	CASE("the proxy kind reads nothing from a 401");
	sip_auth_state_reset(&st[SIP_AUTH_PROXY]);
	CHECK(sip_auth_msg_challenge(&st[SIP_AUTH_PROXY], m, SIP_AUTH_PROXY, 1,
				     2) == SIP_AUTH_ERR_NONE);
	CHECK(!sip_auth_state_valid(&st[SIP_AUTH_PROXY]));

	CASE("a 407 hands the Proxy-Authenticate challenge over");
	m = msg_take(resp_text(407, "Proxy-Authenticate: Digest realm=\"proxy.example.net\","
				    " nonce=\"proxy-nonce-1\", qop=\"auth\"\r\n"));
	CHECK(m != NULL);
	CHECK(sip_auth_msg_challenge(&st[SIP_AUTH_PROXY], m, SIP_AUTH_PROXY, 1,
				     2) == SIP_AUTH_OK);
	CHECK(st[SIP_AUTH_PROXY].qop_auth);
	CHECK(!strcmp(st[SIP_AUTH_PROXY].realm, "proxy.example.net"));

	CASE("the two kinds keep separate nonces and separate counts");
	CHECK(!strcmp(st[SIP_AUTH_WWW].nonce, T_NONCE));
	CHECK(sip_auth_cred_make(&st[SIP_AUTH_WWW], T_USER, T_PASS, "REGISTER", T_URI,
				 &cred) == SIP_AUTH_OK);
	CHECK_UINT(cred.nc, 0);
	CHECK(sip_auth_cred_make(&st[SIP_AUTH_PROXY], T_USER, T_PASS, "REGISTER", T_URI,
				 &cred) == SIP_AUTH_OK);
	CHECK_UINT(cred.nc, 1);
	CHECK_UINT(st[SIP_AUTH_WWW].nc, 0);
	CHECK_UINT(st[SIP_AUTH_PROXY].nc, 1);

	CASE("both credential headers fit in one request");
	sip_buf_init(&b, mem, sizeof(mem));
	CHECK(sip_auth_cred_make(&st[SIP_AUTH_WWW], T_USER, T_PASS, "REGISTER", T_URI,
				 &cred) == SIP_AUTH_OK);
	CHECK(hdr_cred_write(&b, sip_auth_cred_hdr(SIP_AUTH_WWW), &cred));
	CHECK(sip_auth_cred_make(&st[SIP_AUTH_PROXY], T_USER, T_PASS, "REGISTER", T_URI,
				 &cred) == SIP_AUTH_OK);
	CHECK(hdr_cred_write(&b, sip_auth_cred_hdr(SIP_AUTH_PROXY), &cred));
	CHECK(strstr(sip_buf_str(&b), "Authorization: Digest") != NULL);
	CHECK(strstr(sip_buf_str(&b), "Proxy-Authorization: Digest") != NULL);
	CHECK(strstr(sip_buf_str(&b), "nc=00000002") != NULL);

	CASE("a 401 with no challenge at all reports no challenge");
	m = msg_take(resp_text(401, ""));
	CHECK(m != NULL);
	CHECK(sip_auth_msg_challenge(&st[SIP_AUTH_WWW], m, SIP_AUTH_WWW, 1,
				     2) == SIP_AUTH_ERR_NONE);

	/*
	 * RFC 7616 3.7 leaves the order to the server, so the client takes the
	 * first challenge it can answer rather than the strongest one.
	 */
	CASE("the first answerable challenge of several wins");
	sip_auth_state_reset(&st[SIP_AUTH_WWW]);
	m = msg_take(resp_text(401,
			       "WWW-Authenticate: Basic realm=\"first\"\r\n"
			       "WWW-Authenticate: Digest realm=\"second\", nonce=\"n2\","
			       " algorithm=SHA-256\r\n"
			       "WWW-Authenticate: Digest realm=\"third\", nonce=\"n3\"\r\n"));
	CHECK(m != NULL);
	CHECK(sip_auth_msg_challenge(&st[SIP_AUTH_WWW], m, SIP_AUTH_WWW, 1, 2) == SIP_AUTH_OK);
	CHECK(!strcmp(st[SIP_AUTH_WWW].realm, "second"));
	CHECK(st[SIP_AUTH_WWW].algo == SIP_AUTH_ALGO_SHA256);

	CASE("two challenges on one header line are both considered");
	sip_auth_state_reset(&st[SIP_AUTH_WWW]);
	m = msg_take(resp_text(401,
			       "WWW-Authenticate: Digest realm=\"a\", nonce=\"na\","
			       " qop=\"auth-int\", Digest realm=\"b\", nonce=\"nb\"\r\n"));
	CHECK(m != NULL);
	CHECK(sip_auth_msg_challenge(&st[SIP_AUTH_WWW], m, SIP_AUTH_WWW, 1, 2) == SIP_AUTH_OK);
	CHECK(!strcmp(st[SIP_AUTH_WWW].realm, "b"));

	CASE("when nothing is answerable the first reason is reported");
	sip_auth_state_reset(&st[SIP_AUTH_WWW]);
	m = msg_take(resp_text(401,
			       "WWW-Authenticate: Digest realm=\"a\", nonce=\"na\","
			       " algorithm=SHA-512-256\r\n"
			       "WWW-Authenticate: Digest realm=\"b\", nonce=\"nb\","
			       " qop=\"auth-int\"\r\n"));
	CHECK(m != NULL);
	CHECK(sip_auth_msg_challenge(&st[SIP_AUTH_WWW], m, SIP_AUTH_WWW, 1,
				     2) == SIP_AUTH_ERR_ALGO);
	CHECK(!sip_auth_state_valid(&st[SIP_AUTH_WWW]));

	CASE("a request is not a challenge source");
	m = msg_take("REGISTER sip:sip.example.com SIP/2.0\r\n"
		     "Via: SIP/2.0/UDP 10.0.0.1:5060;branch=z9hG4bK-x\r\n"
		     "CSeq: 1 REGISTER\r\n\r\n");
	CHECK(m != NULL);
	CHECK(sip_auth_msg_challenge(&st[SIP_AUTH_WWW], m, SIP_AUTH_WWW, 1,
				     2) == SIP_AUTH_ERR_NONE);

	CASE("status names exist for every code");
	CHECK(!strcmp(sip_auth_status_name(SIP_AUTH_OK), "ok"));
	CHECK(!strcmp(sip_auth_status_name(SIP_AUTH_ERR_STALE_LOOP), "stale-loop"));
	CHECK(!strcmp(sip_auth_kind_name(SIP_AUTH_PROXY), "proxy"));
}

int main(void)
{
	helper_tests();
	vector_tests();
	challenge_tests();
	stale_tests();
	msg_tests();

	return tu_summary("test_sip_auth");
}
